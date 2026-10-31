/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include <bit>                // for std::bit_cast, std::countl_zero, std::countr_zero
#include <cfloat>             // for FLT_MIN, FLT_EVAL_METHOD
#include <clocale>            // for std::localeconv
#include <cstddef>            // for std::ptrdiff_t
#include <cstdint>            // for std::uint64_t
#include <cstdio>             // for std::size_t
#include <cstdlib>            // for std::strto*
#include <cstring>            // for std::memcpy
#include <limits>             // for std::numeric_limits
#include <stdexcept>          // for std::invalid_argument, std::out_of_range
#include <string>             // for std::string
#include <string_view>        // for std::string_view
#include <errno.h>            // for errno
#include <type_traits>        // for std::true_type, std::false_type
#include <utility>            // for std::swap
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>           // for _umul128, __umulh
#endif
#if defined(__has_include) && !defined(STRICT_STOX_NO_LANGINFO)
#if __has_include(<langinfo.h>)
#include <langinfo.h>         // for nl_langinfo, RADIXCHAR
#define STRICT_STOX_HAVE_LANGINFO 1
#endif
#endif


// Clean-input fast path.
//
// strict_stox wraps the C strto* functions, which are locale aware and accept
// leading whitespace, '+', base prefixes, inf/nan and hex floats, and the wrapper
// has to copy the text into a NUL-terminated buffer and save/restore errno.
// Almost every real input is plainer than that: an optional '-', digits (and for
// floating point an optional fraction and exponent) and nothing else. For those
// the result is computed directly, with no copy and no errno traffic. Anything
// else (and anything that could overflow, lose precision, or that strto* might
// read differently) is declined and handled by the original strto* path, so
// results, errno values, idx and exceptions are identical.
namespace strict_stox_detail {

struct DigitTable {
	unsigned char v[256];
	constexpr DigitTable() : v() {
		for (int i = 0; i < 256; ++i) { v[i] = 0xff; }
		for (int i = 0; i < 10; ++i) { v['0' + i] = static_cast<unsigned char>(i); }
		for (int i = 0; i < 26; ++i) {
			v['a' + i] = static_cast<unsigned char>(10 + i);
			v['A' + i] = static_cast<unsigned char>(10 + i);
		}
	}
};
inline constexpr DigitTable digits{};

// Integers: [-]digits in `base` (2..36), whole input, < 64 bytes, no overflow.
template <typename R>
inline bool fast_int(R& out, std::string_view s, int base) noexcept {
	using U = std::make_unsigned_t<R>;
	const std::size_t n = s.size();
	if (n == 0 || n >= 64 || base < 2 || base > 36) { return false; }
	const auto* p = reinterpret_cast<const unsigned char*>(s.data());
	const auto* e = p + n;
	bool neg = false;
	if constexpr (std::is_signed_v<R>) {
		if (*p == '-') {
			neg = true;
			if (++p == e) { return false; }
		}
	}
	U acc = 0;
	const auto ub = static_cast<U>(base);
	do {
		const unsigned d = digits.v[*p];
		if (d >= static_cast<unsigned>(base)) { return false; }
#if defined(__GNUC__) || defined(__clang__)
		if (__builtin_mul_overflow(acc, ub, &acc) || __builtin_add_overflow(acc, static_cast<U>(d), &acc)) { return false; }
#else
		if (acc > (std::numeric_limits<U>::max() - d) / ub) { return false; }
		acc = acc * ub + d;
#endif
	} while (++p != e);
	if constexpr (std::is_signed_v<R>) {
		const U lim = static_cast<U>(std::numeric_limits<R>::max());
		if (neg ? acc > lim + 1 : acc > lim) { return false; }
		out = neg ? static_cast<R>(U(0) - acc) : static_cast<R>(acc);
	} else {
		out = static_cast<R>(acc);
	}
	return true;
}

// ---- float/double ----------------------------------------------------------
//
// [-]digits[.digits][(e|E)[+-]digits], the whole input, any length. The decimal
// text is reduced to w * 10^q (w < 2^64, the first 19 significant digits) and
// converted to the nearest binary value: by Clinger's exact multiply or divide
// when w and 10^|q| are exactly representable, and otherwise by the approach of
// Lemire's "Number Parsing at a Gigabyte per Second" (Software: Practice and
// Experience, 2021): w is normalised, multiplied by the 64 leading bits of 5^q,
// and the leading bits of the 128-bit product are rounded. The table entry is
// truncated, so the product is a lower bound within 2^64 of the exact one (in
// the units of the low half); when the rounding decision could depend on that
// error, the conversion declines. See `el_convert`.
//
// A conversion is declined (the caller falls back to strtod) when the result
// would be subnormal, zero from a non-zero mantissa or infinite (strtod sets
// ERANGE there), when the rounding is ambiguous, for any other syntax
// (whitespace, '+', inf, nan, hex floats, trailing text), when the rounding mode
// is not to nearest, and when the decimal point of the current locale is not '.'
// (unless STRICT_STOX_C_LOCALE is defined).
#if defined(_MSC_VER) && !defined(__clang__)
#define STRICT_STOX_MSVC 1
#endif

#if defined(__SIZEOF_INT128__) && !defined(STRICT_STOX_PORTABLE_MUL64)
__extension__ typedef unsigned __int128 uint128_t;
inline void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept {
	const uint128_t p = static_cast<uint128_t>(a) * b;
	hi = static_cast<std::uint64_t>(p >> 64);
	lo = static_cast<std::uint64_t>(p);
}
#elif defined(STRICT_STOX_MSVC) && defined(_M_X64) && !defined(STRICT_STOX_PORTABLE_MUL64)
inline void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept {
	lo = _umul128(a, b, &hi);
}
#elif defined(STRICT_STOX_MSVC) && defined(_M_ARM64) && !defined(STRICT_STOX_PORTABLE_MUL64)
inline void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept {
	hi = __umulh(a, b);
	lo = a * b;
}
#else
inline void mul64(std::uint64_t a, std::uint64_t b, std::uint64_t& hi, std::uint64_t& lo) noexcept {
	const std::uint64_t a0 = a & 0xffffffffULL, a1 = a >> 32, b0 = b & 0xffffffffULL, b1 = b >> 32;
	const std::uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
	const std::uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
	lo = (mid << 32) | (p00 & 0xffffffffULL);
	hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}
#endif

// Leading zero bits of a non-zero word.
inline int clz64(std::uint64_t x) noexcept {
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_clzll(x);
#else
	return std::countl_zero(x);
#endif
}

template <typename R>
struct fp_traits;
template <>
struct fp_traits<double> {
	using bits = std::uint64_t;
	static constexpr int mant = 52;      // stored mantissa bits
	static constexpr int bias = 1023;
	static constexpr int max_be = 2047;  // biased exponent of inf and nan
};
template <>
struct fp_traits<float> {
	using bits = std::uint32_t;
	static constexpr int mant = 23;
	static constexpr int bias = 127;
	static constexpr int max_be = 255;
};

// 5^q for q in [pow5_qmin, pow5_qmax]: its 64 leading bits, truncated, as a value in
// [2^63, 2^64) (tools/gen_pow5.py). A smaller q cannot give a normal double from a
// 64-bit w; a larger one overflows.
inline constexpr int pow5_qmin = -326;
inline constexpr int pow5_qmax = 308;
inline constexpr std::uint64_t pow5_tab[pow5_qmax - pow5_qmin + 1] = {
// BEGIN pow5 table (generated by tools/gen_pow5.py; do not edit)
	0x84a57695fe98746dULL, 0xa5ced43b7e3e9188ULL, 0xcf42894a5dce35eaULL, 0x818995ce7aa0e1b2ULL,
	0xa1ebfb4219491a1fULL, 0xca66fa129f9b60a6ULL, 0xfd00b897478238d0ULL, 0x9e20735e8cb16382ULL,
	0xc5a890362fddbc62ULL, 0xf712b443bbd52b7bULL, 0x9a6bb0aa55653b2dULL, 0xc1069cd4eabe89f8ULL,
	0xf148440a256e2c76ULL, 0x96cd2a865764dbcaULL, 0xbc807527ed3e12bcULL, 0xeba09271e88d976bULL,
	0x93445b8731587ea3ULL, 0xb8157268fdae9e4cULL, 0xe61acf033d1a45dfULL, 0x8fd0c16206306babULL,
	0xb3c4f1ba87bc8696ULL, 0xe0b62e2929aba83cULL, 0x8c71dcd9ba0b4925ULL, 0xaf8e5410288e1b6fULL,
	0xdb71e91432b1a24aULL, 0x892731ac9faf056eULL, 0xab70fe17c79ac6caULL, 0xd64d3d9db981787dULL,
	0x85f0468293f0eb4eULL, 0xa76c582338ed2621ULL, 0xd1476e2c07286faaULL, 0x82cca4db847945caULL,
	0xa37fce126597973cULL, 0xcc5fc196fefd7d0cULL, 0xff77b1fcbebcdc4fULL, 0x9faacf3df73609b1ULL,
	0xc795830d75038c1dULL, 0xf97ae3d0d2446f25ULL, 0x9becce62836ac577ULL, 0xc2e801fb244576d5ULL,
	0xf3a20279ed56d48aULL, 0x9845418c345644d6ULL, 0xbe5691ef416bd60cULL, 0xedec366b11c6cb8fULL,
	0x94b3a202eb1c3f39ULL, 0xb9e08a83a5e34f07ULL, 0xe858ad248f5c22c9ULL, 0x91376c36d99995beULL,
	0xb58547448ffffb2dULL, 0xe2e69915b3fff9f9ULL, 0x8dd01fad907ffc3bULL, 0xb1442798f49ffb4aULL,
	0xdd95317f31c7fa1dULL, 0x8a7d3eef7f1cfc52ULL, 0xad1c8eab5ee43b66ULL, 0xd863b256369d4a40ULL,
	0x873e4f75e2224e68ULL, 0xa90de3535aaae202ULL, 0xd3515c2831559a83ULL, 0x8412d9991ed58091ULL,
	0xa5178fff668ae0b6ULL, 0xce5d73ff402d98e3ULL, 0x80fa687f881c7f8eULL, 0xa139029f6a239f72ULL,
	0xc987434744ac874eULL, 0xfbe9141915d7a922ULL, 0x9d71ac8fada6c9b5ULL, 0xc4ce17b399107c22ULL,
	0xf6019da07f549b2bULL, 0x99c102844f94e0fbULL, 0xc0314325637a1939ULL, 0xf03d93eebc589f88ULL,
	0x96267c7535b763b5ULL, 0xbbb01b9283253ca2ULL, 0xea9c227723ee8bcbULL, 0x92a1958a7675175fULL,
	0xb749faed14125d36ULL, 0xe51c79a85916f484ULL, 0x8f31cc0937ae58d2ULL, 0xb2fe3f0b8599ef07ULL,
	0xdfbdcece67006ac9ULL, 0x8bd6a141006042bdULL, 0xaecc49914078536dULL, 0xda7f5bf590966848ULL,
	0x888f99797a5e012dULL, 0xaab37fd7d8f58178ULL, 0xd5605fcdcf32e1d6ULL, 0x855c3be0a17fcd26ULL,
	0xa6b34ad8c9dfc06fULL, 0xd0601d8efc57b08bULL, 0x823c12795db6ce57ULL, 0xa2cb1717b52481edULL,
	0xcb7ddcdda26da268ULL, 0xfe5d54150b090b02ULL, 0x9efa548d26e5a6e1ULL, 0xc6b8e9b0709f109aULL,
	0xf867241c8cc6d4c0ULL, 0x9b407691d7fc44f8ULL, 0xc21094364dfb5636ULL, 0xf294b943e17a2bc4ULL,
	0x979cf3ca6cec5b5aULL, 0xbd8430bd08277231ULL, 0xece53cec4a314ebdULL, 0x940f4613ae5ed136ULL,
	0xb913179899f68584ULL, 0xe757dd7ec07426e5ULL, 0x9096ea6f3848984fULL, 0xb4bca50b065abe63ULL,
	0xe1ebce4dc7f16dfbULL, 0x8d3360f09cf6e4bdULL, 0xb080392cc4349decULL, 0xdca04777f541c567ULL,
	0x89e42caaf9491b60ULL, 0xac5d37d5b79b6239ULL, 0xd77485cb25823ac7ULL, 0x86a8d39ef77164bcULL,
	0xa8530886b54dbdebULL, 0xd267caa862a12d66ULL, 0x8380dea93da4bc60ULL, 0xa46116538d0deb78ULL,
	0xcd795be870516656ULL, 0x806bd9714632dff6ULL, 0xa086cfcd97bf97f3ULL, 0xc8a883c0fdaf7df0ULL,
	0xfad2a4b13d1b5d6cULL, 0x9cc3a6eec6311a63ULL, 0xc3f490aa77bd60fcULL, 0xf4f1b4d515acb93bULL,
	0x991711052d8bf3c5ULL, 0xbf5cd54678eef0b6ULL, 0xef340a98172aace4ULL, 0x9580869f0e7aac0eULL,
	0xbae0a846d2195712ULL, 0xe998d258869facd7ULL, 0x91ff83775423cc06ULL, 0xb67f6455292cbf08ULL,
	0xe41f3d6a7377eecaULL, 0x8e938662882af53eULL, 0xb23867fb2a35b28dULL, 0xdec681f9f4c31f31ULL,
	0x8b3c113c38f9f37eULL, 0xae0b158b4738705eULL, 0xd98ddaee19068c76ULL, 0x87f8a8d4cfa417c9ULL,
	0xa9f6d30a038d1dbcULL, 0xd47487cc8470652bULL, 0x84c8d4dfd2c63f3bULL, 0xa5fb0a17c777cf09ULL,
	0xcf79cc9db955c2ccULL, 0x81ac1fe293d599bfULL, 0xa21727db38cb002fULL, 0xca9cf1d206fdc03bULL,
	0xfd442e4688bd304aULL, 0x9e4a9cec15763e2eULL, 0xc5dd44271ad3cdbaULL, 0xf7549530e188c128ULL,
	0x9a94dd3e8cf578b9ULL, 0xc13a148e3032d6e7ULL, 0xf18899b1bc3f8ca1ULL, 0x96f5600f15a7b7e5ULL,
	0xbcb2b812db11a5deULL, 0xebdf661791d60f56ULL, 0x936b9fcebb25c995ULL, 0xb84687c269ef3bfbULL,
	0xe65829b3046b0afaULL, 0x8ff71a0fe2c2e6dcULL, 0xb3f4e093db73a093ULL, 0xe0f218b8d25088b8ULL,
	0x8c974f7383725573ULL, 0xafbd2350644eeacfULL, 0xdbac6c247d62a583ULL, 0x894bc396ce5da772ULL,
	0xab9eb47c81f5114fULL, 0xd686619ba27255a2ULL, 0x8613fd0145877585ULL, 0xa798fc4196e952e7ULL,
	0xd17f3b51fca3a7a0ULL, 0x82ef85133de648c4ULL, 0xa3ab66580d5fdaf5ULL, 0xcc963fee10b7d1b3ULL,
	0xffbbcfe994e5c61fULL, 0x9fd561f1fd0f9bd3ULL, 0xc7caba6e7c5382c8ULL, 0xf9bd690a1b68637bULL,
	0x9c1661a651213e2dULL, 0xc31bfa0fe5698db8ULL, 0xf3e2f893dec3f126ULL, 0x986ddb5c6b3a76b7ULL,
	0xbe89523386091465ULL, 0xee2ba6c0678b597fULL, 0x94db483840b717efULL, 0xba121a4650e4ddebULL,
	0xe896a0d7e51e1566ULL, 0x915e2486ef32cd60ULL, 0xb5b5ada8aaff80b8ULL, 0xe3231912d5bf60e6ULL,
	0x8df5efabc5979c8fULL, 0xb1736b96b6fd83b3ULL, 0xddd0467c64bce4a0ULL, 0x8aa22c0dbef60ee4ULL,
	0xad4ab7112eb3929dULL, 0xd89d64d57a607744ULL, 0x87625f056c7c4a8bULL, 0xa93af6c6c79b5d2dULL,
	0xd389b47879823479ULL, 0x843610cb4bf160cbULL, 0xa54394fe1eedb8feULL, 0xce947a3da6a9273eULL,
	0x811ccc668829b887ULL, 0xa163ff802a3426a8ULL, 0xc9bcff6034c13052ULL, 0xfc2c3f3841f17c67ULL,
	0x9d9ba7832936edc0ULL, 0xc5029163f384a931ULL, 0xf64335bcf065d37dULL, 0x99ea0196163fa42eULL,
	0xc06481fb9bcf8d39ULL, 0xf07da27a82c37088ULL, 0x964e858c91ba2655ULL, 0xbbe226efb628afeaULL,
	0xeadab0aba3b2dbe5ULL, 0x92c8ae6b464fc96fULL, 0xb77ada0617e3bbcbULL, 0xe55990879ddcaabdULL,
	0x8f57fa54c2a9eab6ULL, 0xb32df8e9f3546564ULL, 0xdff9772470297ebdULL, 0x8bfbea76c619ef36ULL,
	0xaefae51477a06b03ULL, 0xdab99e59958885c4ULL, 0x88b402f7fd75539bULL, 0xaae103b5fcd2a881ULL,
	0xd59944a37c0752a2ULL, 0x857fcae62d8493a5ULL, 0xa6dfbd9fb8e5b88eULL, 0xd097ad07a71f26b2ULL,
	0x825ecc24c873782fULL, 0xa2f67f2dfa90563bULL, 0xcbb41ef979346bcaULL, 0xfea126b7d78186bcULL,
	0x9f24b832e6b0f436ULL, 0xc6ede63fa05d3143ULL, 0xf8a95fcf88747d94ULL, 0x9b69dbe1b548ce7cULL,
	0xc24452da229b021bULL, 0xf2d56790ab41c2a2ULL, 0x97c560ba6b0919a5ULL, 0xbdb6b8e905cb600fULL,
	0xed246723473e3813ULL, 0x9436c0760c86e30bULL, 0xb94470938fa89bceULL, 0xe7958cb87392c2c2ULL,
	0x90bd77f3483bb9b9ULL, 0xb4ecd5f01a4aa828ULL, 0xe2280b6c20dd5232ULL, 0x8d590723948a535fULL,
	0xb0af48ec79ace837ULL, 0xdcdb1b2798182244ULL, 0x8a08f0f8bf0f156bULL, 0xac8b2d36eed2dac5ULL,
	0xd7adf884aa879177ULL, 0x86ccbb52ea94baeaULL, 0xa87fea27a539e9a5ULL, 0xd29fe4b18e88640eULL,
	0x83a3eeeef9153e89ULL, 0xa48ceaaab75a8e2bULL, 0xcdb02555653131b6ULL, 0x808e17555f3ebf11ULL,
	0xa0b19d2ab70e6ed6ULL, 0xc8de047564d20a8bULL, 0xfb158592be068d2eULL, 0x9ced737bb6c4183dULL,
	0xc428d05aa4751e4cULL, 0xf53304714d9265dfULL, 0x993fe2c6d07b7fabULL, 0xbf8fdb78849a5f96ULL,
	0xef73d256a5c0f77cULL, 0x95a8637627989aadULL, 0xbb127c53b17ec159ULL, 0xe9d71b689dde71afULL,
	0x9226712162ab070dULL, 0xb6b00d69bb55c8d1ULL, 0xe45c10c42a2b3b05ULL, 0x8eb98a7a9a5b04e3ULL,
	0xb267ed1940f1c61cULL, 0xdf01e85f912e37a3ULL, 0x8b61313bbabce2c6ULL, 0xae397d8aa96c1b77ULL,
	0xd9c7dced53c72255ULL, 0x881cea14545c7575ULL, 0xaa242499697392d2ULL, 0xd4ad2dbfc3d07787ULL,
	0x84ec3c97da624ab4ULL, 0xa6274bbdd0fadd61ULL, 0xcfb11ead453994baULL, 0x81ceb32c4b43fcf4ULL,
	0xa2425ff75e14fc31ULL, 0xcad2f7f5359a3b3eULL, 0xfd87b5f28300ca0dULL, 0x9e74d1b791e07e48ULL,
	0xc612062576589ddaULL, 0xf79687aed3eec551ULL, 0x9abe14cd44753b52ULL, 0xc16d9a0095928a27ULL,
	0xf1c90080baf72cb1ULL, 0x971da05074da7beeULL, 0xbce5086492111aeaULL, 0xec1e4a7db69561a5ULL,
	0x9392ee8e921d5d07ULL, 0xb877aa3236a4b449ULL, 0xe69594bec44de15bULL, 0x901d7cf73ab0acd9ULL,
	0xb424dc35095cd80fULL, 0xe12e13424bb40e13ULL, 0x8cbccc096f5088cbULL, 0xafebff0bcb24aafeULL,
	0xdbe6fecebdedd5beULL, 0x89705f4136b4a597ULL, 0xabcc77118461cefcULL, 0xd6bf94d5e57a42bcULL,
	0x8637bd05af6c69b5ULL, 0xa7c5ac471b478423ULL, 0xd1b71758e219652bULL, 0x83126e978d4fdf3bULL,
	0xa3d70a3d70a3d70aULL, 0xccccccccccccccccULL, 0x8000000000000000ULL, 0xa000000000000000ULL,
	0xc800000000000000ULL, 0xfa00000000000000ULL, 0x9c40000000000000ULL, 0xc350000000000000ULL,
	0xf424000000000000ULL, 0x9896800000000000ULL, 0xbebc200000000000ULL, 0xee6b280000000000ULL,
	0x9502f90000000000ULL, 0xba43b74000000000ULL, 0xe8d4a51000000000ULL, 0x9184e72a00000000ULL,
	0xb5e620f480000000ULL, 0xe35fa931a0000000ULL, 0x8e1bc9bf04000000ULL, 0xb1a2bc2ec5000000ULL,
	0xde0b6b3a76400000ULL, 0x8ac7230489e80000ULL, 0xad78ebc5ac620000ULL, 0xd8d726b7177a8000ULL,
	0x878678326eac9000ULL, 0xa968163f0a57b400ULL, 0xd3c21bcecceda100ULL, 0x84595161401484a0ULL,
	0xa56fa5b99019a5c8ULL, 0xcecb8f27f4200f3aULL, 0x813f3978f8940984ULL, 0xa18f07d736b90be5ULL,
	0xc9f2c9cd04674edeULL, 0xfc6f7c4045812296ULL, 0x9dc5ada82b70b59dULL, 0xc5371912364ce305ULL,
	0xf684df56c3e01bc6ULL, 0x9a130b963a6c115cULL, 0xc097ce7bc90715b3ULL, 0xf0bdc21abb48db20ULL,
	0x96769950b50d88f4ULL, 0xbc143fa4e250eb31ULL, 0xeb194f8e1ae525fdULL, 0x92efd1b8d0cf37beULL,
	0xb7abc627050305adULL, 0xe596b7b0c643c719ULL, 0x8f7e32ce7bea5c6fULL, 0xb35dbf821ae4f38bULL,
	0xe0352f62a19e306eULL, 0x8c213d9da502de45ULL, 0xaf298d050e4395d6ULL, 0xdaf3f04651d47b4cULL,
	0x88d8762bf324cd0fULL, 0xab0e93b6efee0053ULL, 0xd5d238a4abe98068ULL, 0x85a36366eb71f041ULL,
	0xa70c3c40a64e6c51ULL, 0xd0cf4b50cfe20765ULL, 0x82818f1281ed449fULL, 0xa321f2d7226895c7ULL,
	0xcbea6f8ceb02bb39ULL, 0xfee50b7025c36a08ULL, 0x9f4f2726179a2245ULL, 0xc722f0ef9d80aad6ULL,
	0xf8ebad2b84e0d58bULL, 0x9b934c3b330c8577ULL, 0xc2781f49ffcfa6d5ULL, 0xf316271c7fc3908aULL,
	0x97edd871cfda3a56ULL, 0xbde94e8e43d0c8ecULL, 0xed63a231d4c4fb27ULL, 0x945e455f24fb1cf8ULL,
	0xb975d6b6ee39e436ULL, 0xe7d34c64a9c85d44ULL, 0x90e40fbeea1d3a4aULL, 0xb51d13aea4a488ddULL,
	0xe264589a4dcdab14ULL, 0x8d7eb76070a08aecULL, 0xb0de65388cc8ada8ULL, 0xdd15fe86affad912ULL,
	0x8a2dbf142dfcc7abULL, 0xacb92ed9397bf996ULL, 0xd7e77a8f87daf7fbULL, 0x86f0ac99b4e8dafdULL,
	0xa8acd7c0222311bcULL, 0xd2d80db02aabd62bULL, 0x83c7088e1aab65dbULL, 0xa4b8cab1a1563f52ULL,
	0xcde6fd5e09abcf26ULL, 0x80b05e5ac60b6178ULL, 0xa0dc75f1778e39d6ULL, 0xc913936dd571c84cULL,
	0xfb5878494ace3a5fULL, 0x9d174b2dcec0e47bULL, 0xc45d1df942711d9aULL, 0xf5746577930d6500ULL,
	0x9968bf6abbe85f20ULL, 0xbfc2ef456ae276e8ULL, 0xefb3ab16c59b14a2ULL, 0x95d04aee3b80ece5ULL,
	0xbb445da9ca61281fULL, 0xea1575143cf97226ULL, 0x924d692ca61be758ULL, 0xb6e0c377cfa2e12eULL,
	0xe498f455c38b997aULL, 0x8edf98b59a373fecULL, 0xb2977ee300c50fe7ULL, 0xdf3d5e9bc0f653e1ULL,
	0x8b865b215899f46cULL, 0xae67f1e9aec07187ULL, 0xda01ee641a708de9ULL, 0x884134fe908658b2ULL,
	0xaa51823e34a7eedeULL, 0xd4e5e2cdc1d1ea96ULL, 0x850fadc09923329eULL, 0xa6539930bf6bff45ULL,
	0xcfe87f7cef46ff16ULL, 0x81f14fae158c5f6eULL, 0xa26da3999aef7749ULL, 0xcb090c8001ab551cULL,
	0xfdcb4fa002162a63ULL, 0x9e9f11c4014dda7eULL, 0xc646d63501a1511dULL, 0xf7d88bc24209a565ULL,
	0x9ae757596946075fULL, 0xc1a12d2fc3978937ULL, 0xf209787bb47d6b84ULL, 0x9745eb4d50ce6332ULL,
	0xbd176620a501fbffULL, 0xec5d3fa8ce427affULL, 0x93ba47c980e98cdfULL, 0xb8a8d9bbe123f017ULL,
	0xe6d3102ad96cec1dULL, 0x9043ea1ac7e41392ULL, 0xb454e4a179dd1877ULL, 0xe16a1dc9d8545e94ULL,
	0x8ce2529e2734bb1dULL, 0xb01ae745b101e9e4ULL, 0xdc21a1171d42645dULL, 0x899504ae72497ebaULL,
	0xabfa45da0edbde69ULL, 0xd6f8d7509292d603ULL, 0x865b86925b9bc5c2ULL, 0xa7f26836f282b732ULL,
	0xd1ef0244af2364ffULL, 0x8335616aed761f1fULL, 0xa402b9c5a8d3a6e7ULL, 0xcd036837130890a1ULL,
	0x802221226be55a64ULL, 0xa02aa96b06deb0fdULL, 0xc83553c5c8965d3dULL, 0xfa42a8b73abbf48cULL,
	0x9c69a97284b578d7ULL, 0xc38413cf25e2d70dULL, 0xf46518c2ef5b8cd1ULL, 0x98bf2f79d5993802ULL,
	0xbeeefb584aff8603ULL, 0xeeaaba2e5dbf6784ULL, 0x952ab45cfa97a0b2ULL, 0xba756174393d88dfULL,
	0xe912b9d1478ceb17ULL, 0x91abb422ccb812eeULL, 0xb616a12b7fe617aaULL, 0xe39c49765fdf9d94ULL,
	0x8e41ade9fbebc27dULL, 0xb1d219647ae6b31cULL, 0xde469fbd99a05fe3ULL, 0x8aec23d680043beeULL,
	0xada72ccc20054ae9ULL, 0xd910f7ff28069da4ULL, 0x87aa9aff79042286ULL, 0xa99541bf57452b28ULL,
	0xd3fa922f2d1675f2ULL, 0x847c9b5d7c2e09b7ULL, 0xa59bc234db398c25ULL, 0xcf02b2c21207ef2eULL,
	0x8161afb94b44f57dULL, 0xa1ba1ba79e1632dcULL, 0xca28a291859bbf93ULL, 0xfcb2cb35e702af78ULL,
	0x9defbf01b061adabULL, 0xc56baec21c7a1916ULL, 0xf6c69a72a3989f5bULL, 0x9a3c2087a63f6399ULL,
	0xc0cb28a98fcf3c7fULL, 0xf0fdf2d3f3c30b9fULL, 0x969eb7c47859e743ULL, 0xbc4665b596706114ULL,
	0xeb57ff22fc0c7959ULL, 0x9316ff75dd87cbd8ULL, 0xb7dcbf5354e9beceULL, 0xe5d3ef282a242e81ULL,
	0x8fa475791a569d10ULL, 0xb38d92d760ec4455ULL, 0xe070f78d3927556aULL, 0x8c469ab843b89562ULL,
	0xaf58416654a6babbULL, 0xdb2e51bfe9d0696aULL, 0x88fcf317f22241e2ULL, 0xab3c2fddeeaad25aULL,
	0xd60b3bd56a5586f1ULL, 0x85c7056562757456ULL, 0xa738c6bebb12d16cULL, 0xd106f86e69d785c7ULL,
	0x82a45b450226b39cULL, 0xa34d721642b06084ULL, 0xcc20ce9bd35c78a5ULL, 0xff290242c83396ceULL,
	0x9f79a169bd203e41ULL, 0xc75809c42c684dd1ULL, 0xf92e0c3537826145ULL, 0x9bbcc7a142b17ccbULL,
	0xc2abf989935ddbfeULL, 0xf356f7ebf83552feULL, 0x98165af37b2153deULL, 0xbe1bf1b059e9a8d6ULL,
	0xeda2ee1c7064130cULL, 0x9485d4d1c63e8be7ULL, 0xb9a74a0637ce2ee1ULL, 0xe8111c87c5c1ba99ULL,
	0x910ab1d4db9914a0ULL, 0xb54d5e4a127f59c8ULL, 0xe2a0b5dc971f303aULL, 0x8da471a9de737e24ULL,
	0xb10d8e1456105dadULL, 0xdd50f1996b947518ULL, 0x8a5296ffe33cc92fULL, 0xace73cbfdc0bfb7bULL,
	0xd8210befd30efa5aULL, 0x8714a775e3e95c78ULL, 0xa8d9d1535ce3b396ULL, 0xd31045a8341ca07cULL,
	0x83ea2b892091e44dULL, 0xa4e4b66b68b65d60ULL, 0xce1de40642e3f4b9ULL, 0x80d2ae83e9ce78f3ULL,
	0xa1075a24e4421730ULL, 0xc94930ae1d529cfcULL, 0xfb9b7cd9a4a7443cULL, 0x9d412e0806e88aa5ULL,
	0xc491798a08a2ad4eULL, 0xf5b5d7ec8acb58a2ULL, 0x9991a6f3d6bf1765ULL, 0xbff610b0cc6edd3fULL,
	0xeff394dcff8a948eULL, 0x95f83d0a1fb69cd9ULL, 0xbb764c4ca7a4440fULL, 0xea53df5fd18d5513ULL,
	0x92746b9be2f8552cULL, 0xb7118682dbb66a77ULL, 0xe4d5e82392a40515ULL, 0x8f05b1163ba6832dULL,
	0xb2c71d5bca9023f8ULL, 0xdf78e4b2bd342cf6ULL, 0x8bab8eefb6409c1aULL, 0xae9672aba3d0c320ULL,
	0xda3c0f568cc4f3e8ULL, 0x8865899617fb1871ULL, 0xaa7eebfb9df9de8dULL, 0xd51ea6fa85785631ULL,
	0x8533285c936b35deULL, 0xa67ff273b8460356ULL, 0xd01fef10a657842cULL, 0x8213f56a67f6b29bULL,
	0xa298f2c501f45f42ULL, 0xcb3f2f7642717713ULL, 0xfe0efb53d30dd4d7ULL, 0x9ec95d1463e8a506ULL,
	0xc67bb4597ce2ce48ULL, 0xf81aa16fdc1b81daULL, 0x9b10a4e5e9913128ULL, 0xc1d4ce1f63f57d72ULL,
	0xf24a01a73cf2dccfULL, 0x976e41088617ca01ULL, 0xbd49d14aa79dbc82ULL, 0xec9c459d51852ba2ULL,
	0x93e1ab8252f33b45ULL, 0xb8da1662e7b00a17ULL, 0xe7109bfba19c0c9dULL, 0x906a617d450187e2ULL,
	0xb484f9dc9641e9daULL, 0xe1a63853bbd26451ULL, 0x8d07e33455637eb2ULL, 0xb049dc016abc5e5fULL,
	0xdc5c5301c56b75f7ULL, 0x89b9b3e11b6329baULL, 0xac2820d9623bf429ULL, 0xd732290fbacaf133ULL,
	0x867f59a9d4bed6c0ULL, 0xa81f301449ee8c70ULL, 0xd226fc195c6a2f8cULL, 0x83585d8fd9c25db7ULL,
	0xa42e74f3d032f525ULL, 0xcd3a1230c43fb26fULL, 0x80444b5e7aa7cf85ULL, 0xa0555e361951c366ULL,
	0xc86ab5c39fa63440ULL, 0xfa856334878fc150ULL, 0x9c935e00d4b9d8d2ULL, 0xc3b8358109e84f07ULL,
	0xf4a642e14c6262c8ULL, 0x98e7e9cccfbd7dbdULL, 0xbf21e44003acdd2cULL, 0xeeea5d5004981478ULL,
	0x95527a5202df0ccbULL, 0xbaa718e68396cffdULL, 0xe950df20247c83fdULL, 0x91d28b7416cdd27eULL,
	0xb6472e511c81471dULL, 0xe3d8f9e563a198e5ULL, 0x8e679c2f5e44ff8fULL,
// END pow5 table
};

// The correctly rounded R for w * 10^q (w != 0), as its bit pattern, when the result
// is a normal number and the rounding is not in doubt; false otherwise.
//
// With T the table entry, 5^q = (T + eps) * 2^(f - 63), 0 <= eps < 1, f = floor(log2 5^q),
// and f + q = floor(q log2 10) = (q * 217706) >> 16. Let wn = w << lz be the normalised
// w. Then w * 10^q = (wn * (T + eps)) * 2^(f + q - 63 - lz), and the exact product
// wn * (T + eps) lies in [P, P + 2^64) where P = wn * T = hi * 2^64 + lo.
// hi has its top bit at 63 (u = 1) or 62 (u = 0), so the result is 1.x * 2^e with
// e = 63 + u + (f + q) - lz, and the leading nb = mant + 2 bits of hi, m = hi >> sh with
// sh = 63 + u - nb, hold the mantissa and a rounding bit. A halfway point is an odd
// multiple of 2^sh in hi (low half zero); the exact value is on one side of every
// halfway point for sure unless a halfway point lies in [P, P + 2^64): that is
// (lo == 0 and hi is a halfway point) or (lo != 0 and hi + 1 is one), which is
// what the test below says. Then the rounding is the plain one (round bit set means up).
template <typename R>
inline bool el_convert(std::uint64_t w, int q, typename fp_traits<R>::bits& out) noexcept {
	using tr = fp_traits<R>;
	constexpr int nb = tr::mant + 2;
	const int lz = clz64(w);
	std::uint64_t hi, lo;
	mul64(w << lz, pow5_tab[q - pow5_qmin], hi, lo);
	const int u = static_cast<int>(hi >> 63);
	const int sh = 63 + u - nb;
	const std::uint64_t half = 1ULL << sh;
	if (((hi & ((half << 1) - 1)) + (lo != 0)) == half) { return false; }
	std::uint64_t m = ((hi >> sh) + 1) >> 1;  // mant + 1 bits, or 2^(mant+1) after a carry
	int be = ((q * 217706) >> 16) + 63 + u - lz + tr::bias;
	if (be < 1) { return false; }  // below the normal range (or rounds up into it)
	const int carry = static_cast<int>(m >> (tr::mant + 1));
	m >>= carry;
	be += carry;
	if (be >= tr::max_be) { return false; }
	out = (static_cast<typename tr::bits>(be) << tr::mant) | static_cast<typename tr::bits>(m & ((1ULL << tr::mant) - 1));
	return true;
}

// More than 19 digits: w holds the first 19 significant digits and, when any later
// digit is not zero, the exact value lies strictly between w and w + 1 (in units of
// 10^q). Rounding is monotonic, so if both ends round to the same value, so does it.
template <typename R>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline bool fp_long(const char* p, const char* const mend, std::ptrdiff_t q, typename fp_traits<R>::bits& out) noexcept {
	std::uint64_t w = 0;
	int nsig = 0;
	bool sticky = false;
	bool frac = false;
	for (; p != mend; ++p) {
		if (*p == '.') {
			frac = true;
			continue;
		}
		const unsigned d = static_cast<unsigned char>(*p) - '0';
		if (nsig == 0 && d == 0) {  // leading zero: it only shifts the decimal point
			if (frac) { --q; }
		} else if (nsig < 19) {
			w = w * 10 + d;
			++nsig;
			if (frac) { --q; }
		} else {
			sticky |= d != 0;
			if (!frac) { ++q; }
		}
	}
	if (w == 0) {
		out = 0;
		return true;
	}
	if (q < pow5_qmin || q > pow5_qmax) { return false; }
	typename fp_traits<R>::bits a;
	if (!el_convert<R>(w, static_cast<int>(q), a)) { return false; }
	if (sticky) {
		typename fp_traits<R>::bits b;
		if (!el_convert<R>(w + 1, static_cast<int>(q), b) || a != b) { return false; }
	}
	out = a;
	return true;
}

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define STRICT_STOX_SWAR 1
// Eight characters at once. Non-zero bits in a byte of the result mean that byte is not an ASCII digit.
inline std::uint64_t non_digit_bytes(std::uint64_t v) noexcept {
	return ((v & 0xF0F0F0F0F0F0F0F0ULL) ^ 0x3030303030303030ULL) | (((v + 0x0606060606060606ULL) & 0xF0F0F0F0F0F0F0F0ULL) ^ 0x3030303030303030ULL);
}
// The value of eight ASCII digits, the first character most significant (a leading byte of 0 counts as a 0 digit).
inline std::uint64_t parse8(std::uint64_t v) noexcept {
	v &= 0x0F0F0F0F0F0F0F0FULL;
	v = (v * 2561) >> 8;                                         // 10 * a + b in every even byte
	v = ((v & 0x00FF00FF00FF00FFULL) * 6553601) >> 16;           // 100 * x + y in every even 16 bit lane
	v = ((v & 0x0000FFFF0000FFFFULL) * 42949672960001ULL) >> 32;  // 10000 * x + y
	return v;
}
inline int ctz64(std::uint64_t x) noexcept {  // x != 0
#if defined(__GNUC__) || defined(__clang__)
	return __builtin_ctzll(x);
#else
	return std::countr_zero(x);
#endif
}
inline constexpr std::uint64_t pow10_u64[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
#endif

// Accumulate the digits at p into w (wrapping when there are more than 19); returns the first non-digit.
inline const char* scan_digits(const char* p, const char* const e, std::uint64_t& w) noexcept {
#ifdef STRICT_STOX_SWAR
	while (e - p >= 8) {
		std::uint64_t v;
		std::memcpy(&v, p, 8);
		const std::uint64_t t = non_digit_bytes(v);
		if (t == 0) {
			w = w * 100000000ULL + parse8(v);
			p += 8;
			continue;
		}
		const int n = ctz64(t) >> 3;  // digits before the first other character, 0..7
		if (n != 0) { w = w * pow10_u64[n] + parse8(v << (64 - 8 * n)); }
		return p + n;
	}
#endif
	while (p != e) {
		const unsigned d = static_cast<unsigned char>(*p) - '0';
		if (d > 9) { break; }
		w = w * 10 + d;
		++p;
	}
	return p;
}

// strtod reads the decimal point of the current locale, so text with a '.' may be read here only
// when the locale's decimal point is ".". The query is a libc call; nl_langinfo (POSIX) is a few
// loads, localeconv fills a whole structure and costs two to three times as much. A program that
// never leaves the "C" locale can define STRICT_STOX_C_LOCALE to skip it, and then plain decimals
// are read with '.' whatever the locale says (strtod still decides the rest).
inline bool decimal_point_is_dot() noexcept {
#ifdef STRICT_STOX_C_LOCALE
	return true;
#else
#ifdef STRICT_STOX_HAVE_LANGINFO
	const char* const dp = nl_langinfo(RADIXCHAR);
#else
	const char* const dp = std::localeconv()->decimal_point;
#endif
	return dp[0] == '.' && dp[1] == '\0';
#endif
}

// The conversions here round to nearest; strtod follows the current rounding mode. True in the
// default mode (1 + tiny and 1 - tiny both round to 1), false in the directed ones, and with
// extended precision evaluation (where the test is meaningless) the answer is false too.
inline bool rounds_to_nearest() noexcept {
	volatile float tiny = FLT_MIN;
	const float t = tiny;
	return 1.0f + t == 1.0f - t;
}

template <typename R>
inline bool fast_fp(R& out, std::string_view s) noexcept {
	using tr = fp_traits<R>;
	using bits_t = typename tr::bits;
	const char* p = s.data();
	const char* const e = p + s.size();
	if (p == e) { return false; }
	const bool neg = *p == '-';
	p += neg;
	const char* const m0 = p;
	std::uint64_t w = 0;
	p = scan_digits(p, e, w);
	std::ptrdiff_t nd = p - m0;
	std::ptrdiff_t nf = 0;
	bool dot = false;
	if (p != e && *p == '.') {
		dot = true;
		const char* const f0 = ++p;
		p = scan_digits(p, e, w);
		nf = p - f0;
		nd += nf;
	}
	if (nd == 0) { return false; }
	const char* const mend = p;
	std::ptrdiff_t ex = 0;
	if (p != e) {
		if ((static_cast<unsigned char>(*p) | 0x20) != 'e') { return false; }
		if (++p == e) { return false; }
		const bool eneg = *p == '-';
		if ((eneg || *p == '+') && ++p == e) { return false; }
		unsigned d = static_cast<unsigned char>(*p) - '0';
		if (d > 9) { return false; }
		for (;;) {
			// A huge exponent is not read to the end: it cannot be in range, and a clamped value
			// could be cancelled by a mantissa of a hundred thousand digits into a wrong answer.
			ex = ex < 100000 ? ex * 10 + static_cast<std::ptrdiff_t>(d) : std::numeric_limits<std::ptrdiff_t>::max() / 4;
			if (++p == e) { break; }
			d = static_cast<unsigned char>(*p) - '0';
			if (d > 9) { return false; }
		}
		if (eneg) { ex = -ex; }
	}
	bits_t b;
	if (nd > 19) {
		if (!fp_long<R>(m0, mend, ex, b)) { return false; }
	} else if (w == 0) {
		b = 0;
	} else {
		const std::ptrdiff_t q = ex - nf;
		if (q < pow5_qmin || q > pow5_qmax) { return false; }
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD == 0
		// Clinger: w and 10^|q| exactly representable, so one IEEE operation is correctly rounded
		// (in the nearest mode, checked below). Cheaper than the table for short numbers.
		if constexpr (std::is_same_v<R, double>) {
			static constexpr double pow10[23] = {
				1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
				1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
			};
			if (w <= (1ULL << 53) && q <= 22 && q >= -22) {
				b = std::bit_cast<bits_t>(q >= 0 ? static_cast<double>(w) * pow10[q] : static_cast<double>(w) / pow10[-q]);
			} else if (!el_convert<R>(w, static_cast<int>(q), b)) {
				return false;
			}
		} else
#endif
		if (!el_convert<R>(w, static_cast<int>(q), b)) { return false; }
	}
	// Last, so the call overlaps the arithmetic above; the dot is the only text a locale can change.
	if (dot && !decimal_point_is_dot()) { return false; }
	if (!rounds_to_nearest()) { return false; }
	b |= static_cast<bits_t>(neg) << (sizeof(bits_t) * 8 - 1);
	out = std::bit_cast<R>(b);
	return true;
}

}  // namespace strict_stox_detail


// Types conversion strict_stox and strict_nothrow_stox
template <typename F>
class Stox {
	const char * const name;
	F func;

	template <typename T, typename... Args>
	auto _stox(std::true_type, const std::string_view& str, std::size_t* idx, Args&&... args) noexcept {
		// strto* needs a NUL-terminated copy of the whole input: short text goes to a stack
		// buffer, longer text (long zero runs, long decimal strings) to the heap. An embedded
		// NUL ends the number there, as for any C string.
		char sbuf[128];
		std::string heap;
		const std::size_t size = str.size();
		char* buf = sbuf;
		if (size < sizeof(sbuf)) {
			if (size != 0) { std::memcpy(sbuf, str.data(), size); }
			sbuf[size] = '\0';
		} else {
			heap.assign(str.data(), size);
			buf = heap.data();
		}
		auto end = buf + size;
		auto ptr = end;
		auto r = func(buf, &ptr, std::forward<Args>(args)...);
		if (errno) return static_cast<decltype(r)>(0);
		if (ptr == buf) {
			errno = EINVAL;
			return static_cast<decltype(r)>(0);
		}
		if (idx) {
			*idx = static_cast<size_t>(ptr - buf);
		} else if (ptr != end) {
			errno = EINVAL;
			return static_cast<decltype(r)>(0);
		}
		return r;
	}

	template <typename T, typename... Args>
	auto _stox(std::false_type, const std::string_view& str, std::size_t* idx, Args&&... args) noexcept {
		auto r = _stox<void>(std::true_type{}, str, idx, std::forward<Args>(args)...);
		if (errno) return static_cast<T>(0);
		if (r < std::numeric_limits<T>::min() || std::numeric_limits<T>::max() < r) {
			errno = ERANGE;
			return static_cast<T>(0);
		}
		return static_cast<T>(r);
	}

	template <typename T, typename... Args>
	using result_t = std::conditional_t<std::is_void_v<T>, std::invoke_result_t<F, const char*, char**, Args...>, T>;

	template <typename G>
	bool _is(G g) const noexcept {
		if constexpr (std::is_same_v<F, G>) { return func == g; } else { return false; }
	}

	// Try the fast path. On success, `out` holds the final value (already
	// range-checked for T) and the result is exactly what the strto* path
	// returns with errno == 0. Returns false to defer to the strto* path.
	template <typename T, typename Out, typename... Args>
	bool _fast(Out& out, const std::string_view& str, std::size_t* idx, Args&&... args) const noexcept {
		if constexpr (sizeof...(Args) == 1 && std::is_invocable_v<F, const char*, char**, int>) {
			if (!(_is(&std::strtol) || _is(&std::strtoul) || _is(&std::strtoll) || _is(&std::strtoull))) { return false; }
			using R = std::invoke_result_t<F, const char*, char**, int>;
			if constexpr (std::is_integral_v<R>) {
				const int base = (static_cast<int>(args), ...);
				R r;
				if (!strict_stox_detail::fast_int(r, str, base)) { return false; }
				if constexpr (!std::is_void_v<T>) {
					if (r < std::numeric_limits<T>::min() || std::numeric_limits<T>::max() < r) { return false; }
				}
				out = static_cast<Out>(r);
				if (idx) { *idx = str.size(); }
				return true;
			}
		} else if constexpr (sizeof...(Args) == 0 && std::is_invocable_v<F, const char*, char**>) {
			using R = std::invoke_result_t<F, const char*, char**>;
			if constexpr (std::is_same_v<R, float> || std::is_same_v<R, double>) {
				if (!(_is(&std::strtof) || _is(&std::strtod))) { return false; }
				R r;
				if (!strict_stox_detail::fast_fp(r, str)) { return false; }
				out = static_cast<Out>(r);
				if (idx) { *idx = str.size(); }
				return true;
			}
		}
		return false;
	}

public:
	Stox(const char* _name, F _func)
		: name(_name),
		  func(_func) { }

	template <typename T, typename... Args>
	auto stox_nothrow(int* errno_save, const std::string_view& str, std::size_t* idx, Args&&... args) {
		result_t<T, Args...> fast;
		if (_fast<T>(fast, str, idx, args...)) {
			if (errno_save != nullptr) { *errno_save = 0; }
			return fast;
		}
		auto _errno = errno;
		errno = 0;
		auto r = _stox<T>(std::is_same<void, T>{}, str, idx, std::forward<Args>(args)...);
		std::swap(errno, _errno);
		if (errno_save != nullptr) {
			*errno_save = _errno;
		}
		return r;
	}

	template <typename T, typename... Args>
	auto stox_throw(const std::string_view& str, std::size_t* idx, Args&&... args) {
		result_t<T, Args...> fast;
		if (_fast<T>(fast, str, idx, args...)) { return fast; }
		auto _errno = errno;
		errno = 0;
		auto r = _stox<T>(std::is_same<void, T>{}, str, idx, std::forward<Args>(args)...);
		std::swap(errno, _errno);
		switch (_errno) {
			case EINVAL:
				throw std::invalid_argument(std::string(name) + ": Cannot convert value: " + std::string(str));
			case ERANGE:
				throw std::out_of_range(std::string(name) + ": Out of range value: " + std::string(str));
			default:
				break;
		}
		return r;
	}
};
#define STOXIFY_BASE(name, func, T) \
static Stox<decltype(&func)> _strict_##name(#name, func); \
inline auto strict_##name(int* errno_save, std::string_view str, std::size_t* idx = nullptr, int base = 10) noexcept { \
	return _strict_##name.stox_nothrow<T>(errno_save, str, idx, base); \
} \
inline auto strict_##name(std::string_view str, std::size_t* idx = nullptr, int base = 10) { \
	return _strict_##name.stox_throw<T>(str, idx, base); \
}
#define STOXIFY(name, func, T) \
static Stox<decltype(&func)> _strict_##name(#name, func); \
inline auto strict_##name(int* errno_save, std::string_view str, std::size_t* idx = nullptr) noexcept { \
	return _strict_##name.stox_nothrow<T>(errno_save, str, idx); \
} \
inline auto strict_##name(std::string_view str, std::size_t* idx = nullptr) { \
	return _strict_##name.stox_throw<T>(str, idx); \
}
STOXIFY_BASE(stoul, std::strtoul, void)
STOXIFY_BASE(stoull, std::strtoull, void)
STOXIFY_BASE(stoi, std::strtol, int)
STOXIFY_BASE(stou, std::strtoul, unsigned)
STOXIFY_BASE(stol, std::strtol, void)
STOXIFY_BASE(stoll, std::strtoll, void)
STOXIFY_BASE(stoz, std::strtoull, std::size_t)
STOXIFY(stof, std::strtof, void)
STOXIFY(stod, std::strtod, void)
STOXIFY(stold, std::strtold, void)
#undef STOXIFY_BASE
#undef STOXIFY
