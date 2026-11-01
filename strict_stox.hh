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

#include <algorithm>          // for std::min
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
#include <utility>            // for std::forward
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

// Integers: [-]digits in `base` (2..36), whole input, < 64 bytes. 1: out holds the value. 0: declined (strto* decides).
// 2: a value out of range of R. The digits up to where the range was left are all valid, and strto* reads the same digits,
// so it reports ERANGE whatever follows them.
template <typename R>
inline int fast_int(R& out, std::string_view s, int base) noexcept {
	using U = std::make_unsigned_t<R>;
	const std::size_t n = s.size();
	if (n == 0 || n >= 64 || base < 2 || base > 36) { return 0; }
	const auto* p = reinterpret_cast<const unsigned char*>(s.data());
	const auto* e = p + n;
	bool neg = false;
	if constexpr (std::is_signed_v<R>) {
		if (*p == '-') {
			neg = true;
			if (++p == e) { return 0; }
		}
	}
	U acc = 0;
	const auto ub = static_cast<U>(base);
	do {
		const unsigned d = digits.v[*p];
		if (d >= static_cast<unsigned>(base)) { return 0; }
#if defined(__GNUC__) || defined(__clang__)
		if (__builtin_mul_overflow(acc, ub, &acc) || __builtin_add_overflow(acc, static_cast<U>(d), &acc)) { return 2; }
#else
		if (acc > (std::numeric_limits<U>::max() - d) / ub) { return 2; }
		acc = acc * ub + d;
#endif
	} while (++p != e);
	if constexpr (std::is_signed_v<R>) {
		const U lim = static_cast<U>(std::numeric_limits<R>::max());
		if (neg ? acc > lim + 1 : acc > lim) { return 2; }
		out = neg ? static_cast<R>(U(0) - acc) : static_cast<R>(acc);
	} else {
		out = static_cast<R>(acc);
	}
	return 1;
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
	static constexpr int qsub = -1074;   // exponent of the unit of a subnormal
	static constexpr int dec_hi = 309;   // 10^dec_hi is more than the largest finite value
	static constexpr int dec_lo = -324;  // 10^dec_lo is less than half the smallest subnormal
};
template <>
struct fp_traits<float> {
	using bits = std::uint32_t;
	static constexpr int mant = 23;
	static constexpr int bias = 127;
	static constexpr int max_be = 255;
	static constexpr int qsub = -149;
	static constexpr int dec_hi = 39;
	static constexpr int dec_lo = -46;
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

// The digits of [p, e) are known to be digits ('0'..'9') in the helpers below.

// The first character of [p, e) that is not '0' (e when there is none).
inline const char* skip_zeros(const char* p, const char* const e) noexcept {
#ifdef STRICT_STOX_SWAR
	while (e - p >= 8) {
		std::uint64_t v;
		std::memcpy(&v, p, 8);
		v ^= 0x3030303030303030ULL;
		if (v != 0) { return p + (ctz64(v) >> 3); }
		p += 8;
	}
#endif
	while (p != e && *p == '0') { ++p; }
	return p;
}

// True when a digit of [p, e) is not '0'.
inline bool any_nonzero(const char* p, const char* const e) noexcept {
#ifdef STRICT_STOX_SWAR
	while (e - p >= 8) {
		std::uint64_t v;
		std::memcpy(&v, p, 8);
		if (v != 0x3030303030303030ULL) { return true; }
		p += 8;
	}
#endif
	for (; p != e; ++p) {
		if (*p != '0') { return true; }
	}
	return false;
}

// w * 10^n + the n digits at p (n <= 19, and w * 10^n < 2^64).
inline std::uint64_t append_digits(const char* p, std::size_t n, std::uint64_t w) noexcept {
#ifdef STRICT_STOX_SWAR
	while (n >= 8) {
		std::uint64_t v;
		std::memcpy(&v, p, 8);
		w = w * 100000000ULL + parse8(v);
		p += 8;
		n -= 8;
	}
#endif
	while (n-- != 0) { w = w * 10 + static_cast<unsigned>(*p++ - '0'); }
	return w;
}

// More than 19 digits: [p, mend) holds the integer digits, an optional '.' and the fraction digits. The first
// 19 significant digits give w and, when any later digit is not zero, the exact value lies strictly between w
// and w + 1 (in units of 10^q). Rounding is monotonic, so if both ends round to the same value, so does the
// text. An all-zero mantissa is zero. The point is found here, not passed in, so that fast_fp keeps its
// registers for the short texts it is written for.
template <typename R>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline bool fp_long(const char* const ib, const char* const mend, std::ptrdiff_t q, typename fp_traits<R>::bits& out) noexcept {
	const char* ie = static_cast<const char*>(std::memchr(ib, '.', static_cast<std::size_t>(mend - ib)));
	const char* fb = mend;  // the fraction digits [fb, fe)
	const char* const fe = mend;
	if (ie != nullptr) { fb = ie + 1; } else { ie = mend; }
	std::uint64_t w;
	bool sticky;
	const char* const a = skip_zeros(ib, ie);
	if (a != ie) {  // the first significant digit is an integer digit
		const std::size_t ni = static_cast<std::size_t>(ie - a);
		if (ni >= 19) {
			w = append_digits(a, 19, 0);
			q += static_cast<std::ptrdiff_t>(ni - 19);  // the integer digits not read are factors of 10
			sticky = any_nonzero(a + 19, ie) || any_nonzero(fb, fe);
		} else {
			const std::size_t k = std::min<std::size_t>(19 - ni, static_cast<std::size_t>(fe - fb));
			w = append_digits(fb, k, append_digits(a, ni, 0));
			q -= static_cast<std::ptrdiff_t>(k);
			sticky = any_nonzero(fb + k, fe);
		}
	} else {  // zeros, then the first significant digit in the fraction
		const char* const b = skip_zeros(fb, fe);
		if (b == fe) {
			out = 0;
			return true;
		}
		const std::size_t k = std::min<std::size_t>(19, static_cast<std::size_t>(fe - b));
		w = append_digits(b, k, 0);
		q -= static_cast<std::ptrdiff_t>(static_cast<std::size_t>(b - fb) + k);
		sticky = any_nonzero(b + k, fe);
	}
	if (q < pow5_qmin || q > pow5_qmax) { return false; }
	typename fp_traits<R>::bits x;
	if (!el_convert<R>(w, static_cast<int>(q), x)) { return false; }
	if (sticky) {
		typename fp_traits<R>::bits y;
		if (!el_convert<R>(w + 1, static_cast<int>(q), y) || x != y) { return false; }
	}
	out = x;
	return true;
}

// strtod reads the decimal point of the current locale, so text with a '.' may be read here only
// when the locale's decimal point is ".". The query is a libc call; nl_langinfo (POSIX) is a few
// loads, localeconv fills a whole structure and costs two to three times as much. A program that
// never leaves the "C" locale can define STRICT_STOX_C_LOCALE to skip it, and then plain decimals
// are read with '.' whatever the locale says (strtod still decides the rest). A test can define
// STRICT_STOX_DECIMAL_POINT as an expression for the decimal point string, to run the check
// without an installed locale.
inline bool decimal_point_is_dot() noexcept {
#ifdef STRICT_STOX_C_LOCALE
	return true;
#else
#if defined(STRICT_STOX_DECIMAL_POINT)
	const char* const dp = STRICT_STOX_DECIMAL_POINT;
#elif defined(STRICT_STOX_HAVE_LANGINFO)
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

// ---- The exact conversion --------------------------------------------------------------------------------
//
// fast_fp declines the texts whose rounding its 64-bit table cannot decide and the texts whose result is
// subnormal, zero or infinite. fp_slow converts those exactly, so that for decimal text in the default
// rounding mode libc is asked only for what it alone defines: the rounding modes, the locale, hex floats,
// inf and nan, and the errno of an exactly representable subnormal (glibc sets ERANGE only when the result
// is inexact, Apple's libc always).
//
// The method is the one of Lemire's "Number Parsing at a Gigabyte per Second" (section on the slow path), the
// comparison of the exact decimal value with the halfway point between two floats, in big integers. The text
// is D * 10^q with D the first 768 significant digits and, when any later digit is not zero, one more digit 1
// (a halfway point has at most 767 significant digits, so the value is on the same side of it as before). The
// 64-bit table gives a candidate mantissa m that the rounded result cannot fall below; then
//     D * 10^q  <,  =,  >  (2m + 1) * 2^(e - 1)      with e the exponent of the unit of m
// is decided exactly (a tie goes to the even mantissa), and the candidate moves up one unit at a time. Texts
// whose result is subnormal, and q outside the table, are divided: D * 10^q / 2^e by shift and subtract.
// A conversion that does not fit the fixed big integers is declined.

inline constexpr int slow_digits = 768;

#ifdef STRICT_STOX_COUNT_LIBC
// Test seam: the texts that enter fp_decline (fast_fp declined them), the calls of el_candidate (the table in the exact path), and the
// calls of the strto* path of the wrapper.
inline thread_local std::size_t decline_calls = 0;
inline thread_local std::size_t candidate_calls = 0;
inline thread_local std::size_t strto_calls = 0;
#endif

struct Pow5Small {
	std::uint64_t v[28];  // 5^0 .. 5^27
	constexpr Pow5Small() : v() {
		std::uint64_t x = 1;
		for (int i = 0; i < 28; ++i) {
			v[i] = x;
			x *= 5;
		}
	}
};
inline constexpr Pow5Small pow5_small{};

struct Pow10Small {
	std::uint64_t v[20];  // 10^0 .. 10^19
	constexpr Pow10Small() : v() {
		std::uint64_t x = 1;
		for (int i = 0; i < 20; ++i) {
			v[i] = x;
			x *= 10;
		}
	}
};
inline constexpr Pow10Small pow10_small{};

// An unsigned integer of at most 80 * 64 bits. An operation that would need more sets `ovf` and the
// result is not to be used.
struct Big {
	static constexpr int cap = 48;  // the largest value is below 2^2650 (41 limbs): 769 digits and a power of 5 up to 5^1093
	std::uint64_t l[cap];
	int n;
	bool ovf;
	Big() noexcept : n(0), ovf(false) { }
	void copy(const Big& o) noexcept {
		n = o.n;
		ovf = o.ovf;
		for (int i = 0; i < n; ++i) { l[i] = o.l[i]; }
	}
	void set(std::uint64_t v) noexcept {
		l[0] = v;
		n = v != 0;
	}
	bool zero() const noexcept { return n == 0; }
	void mul(std::uint64_t m) noexcept {
		std::uint64_t carry = 0;
		for (int i = 0; i < n; ++i) {
			std::uint64_t hi, lo;
			mul64(l[i], m, hi, lo);
			lo += carry;
			hi += lo < carry;
			l[i] = lo;
			carry = hi;
		}
		if (carry != 0) {
			if (n == cap) { ovf = true; return; }
			l[n++] = carry;
		}
	}
	void add(std::uint64_t a) noexcept {
		for (int i = 0; i < n && a != 0; ++i) {
			l[i] += a;
			a = l[i] < a;
		}
		if (a != 0) {
			if (n == cap) { ovf = true; return; }
			l[n++] = a;
		}
	}
	void mul_pow5(unsigned k) noexcept {
		while (k >= 27) { mul(pow5_small.v[27]); k -= 27; }
		if (k != 0) { mul(pow5_small.v[k]); }
	}
	void shl(std::ptrdiff_t k) noexcept {
		if (n == 0 || k == 0) { return; }
		const std::ptrdiff_t w = k / 64;
		const unsigned b = static_cast<unsigned>(k % 64);
		if (n + w + 1 > cap) { ovf = true; return; }
		const int top = n + static_cast<int>(w);
		if (b == 0) {
			for (int i = n - 1; i >= 0; --i) { l[i + w] = l[i]; }
			for (std::ptrdiff_t i = 0; i < w; ++i) { l[i] = 0; }
			n = top;
			return;
		}
		l[top] = l[n - 1] >> (64 - b);
		for (int i = n - 1; i > 0; --i) { l[i + w] = (l[i] << b) | (l[i - 1] >> (64 - b)); }
		l[w] = l[0] << b;
		for (std::ptrdiff_t i = 0; i < w; ++i) { l[i] = 0; }
		n = top + (l[top] != 0);
	}
	void shr1() noexcept {
		for (int i = 0; i < n; ++i) { l[i] = (l[i] >> 1) | (i + 1 < n ? l[i + 1] << 63 : 0); }
		if (n != 0 && l[n - 1] == 0) { --n; }
	}
	std::ptrdiff_t bitlen() const noexcept { return n == 0 ? 0 : 64 * static_cast<std::ptrdiff_t>(n - 1) + (64 - clz64(l[n - 1])); }
	static int cmp(const Big& a, const Big& b) noexcept {
		if (a.n != b.n) { return a.n < b.n ? -1 : 1; }
		for (int i = a.n - 1; i >= 0; --i) {
			if (a.l[i] != b.l[i]) { return a.l[i] < b.l[i] ? -1 : 1; }
		}
		return 0;
	}
	void sub(const Big& b) noexcept {  // *this -= b, *this >= b
		std::uint64_t borrow = 0;
		for (int i = 0; i < n; ++i) {
			const std::uint64_t bi = i < b.n ? b.l[i] : 0;
			const std::uint64_t d = l[i] - bi - borrow;
			borrow = (l[i] < bi) || (l[i] - bi < borrow);
			l[i] = d;
		}
		while (n != 0 && l[n - 1] == 0) { --n; }
	}
	void from_digits(const std::uint8_t* d, std::size_t nd) noexcept {
		n = 0;
		for (std::size_t i = 0; i < nd;) {
			const std::size_t k = nd - i < 19 ? nd - i : 19;
			std::uint64_t chunk = 0;
			for (std::size_t j = 0; j < k; ++j) { chunk = chunk * 10 + d[i + j]; }
			mul(pow10_small.v[k]);
			add(chunk);
			i += k;
		}
	}
};

// The same comparison in 128 bit arithmetic, for D * 10^q with D below 2^64 (at most 19 digits) and |q| <= 27 (5^|q| < 2^63):
// V = D * 10^q against H = t * 2^g, t > 0 below 2^55. Both sides are scaled to integers, (D 5^q) 2^q and t 2^g for q >= 0,
// D 2^q and (t 5^-q) 2^g for q < 0, and the one with the larger power of two is shifted. 2 when a shifted side does not fit.
struct U128 {
	std::uint64_t hi, lo;
};
inline int bitlen128(const U128& v) noexcept { return v.hi != 0 ? 128 - clz64(v.hi) : (v.lo != 0 ? 64 - clz64(v.lo) : 0); }
inline U128 shl128(const U128& v, unsigned s) noexcept {  // s < 128
	if (s == 0) { return v; }
	if (s >= 64) { return U128{v.lo << (s - 64), 0}; }
	return U128{(v.hi << s) | (v.lo >> (64 - s)), v.lo << s};
}
inline int cmp128(const U128& a, const U128& b) noexcept {
	if (a.hi != b.hi) { return a.hi < b.hi ? -1 : 1; }
	if (a.lo != b.lo) { return a.lo < b.lo ? -1 : 1; }
	return 0;
}
inline int cmp_small(std::uint64_t D, int q, std::uint64_t t, std::ptrdiff_t g) noexcept {
	U128 a, b;
	if (q >= 0) {
		mul64(D, pow5_small.v[q], a.hi, a.lo);
		b = U128{0, t};
	} else {
		a = U128{0, D};
		mul64(t, pow5_small.v[-q], b.hi, b.lo);
	}
	const std::ptrdiff_t sa = q, sb = g;  // a * 2^sa against b * 2^sb
	// A candidate of the table has both sides within a few units of each other, so the shifted side always fits; if it does
	// not, the caller compares in big integers.
	if (sa >= sb) {
		const std::ptrdiff_t s = sa - sb;
		if (bitlen128(a) - 1 + s >= 128) { return 2; }
		return cmp128(shl128(a, static_cast<unsigned>(s)), b);
	}
	const std::ptrdiff_t s = sb - sa;
	if (bitlen128(b) - 1 + s >= 128) { return 2; }
	return cmp128(a, shl128(b, static_cast<unsigned>(s)));
}

// How the table places the value of w * 10^q (w != 0, at most the leading 19 digits of the text, so the value of the text is
// in [P, P (1 + 2^-59)) for the table product P below):
//   cand_normal   in the normal range: m0 is the mantissa the rounded value cannot be below, eq the exponent of its unit
//   cand_below    surely below T = 2^emin (1 - 2^-(mant + 1)), the point from which a value rounds up to the smallest normal one:
//                 a subnormal or zero result
//   cand_unknown  the table does not decide
enum : int { cand_normal = 0, cand_below = 1, cand_unknown = 2 };
template <typename R>
inline int el_candidate(std::uint64_t w, int q, std::uint64_t& m0, int& eq) noexcept {
#ifdef STRICT_STOX_COUNT_LIBC
	++candidate_calls;
#endif
	using tr = fp_traits<R>;
	constexpr int nb = tr::mant + 2;
	const int lz = clz64(w);
	std::uint64_t hi, lo;
	mul64(w << lz, pow5_tab[q - pow5_qmin], hi, lo);
	const int u = static_cast<int>(hi >> 63);
	const int E = ((q * 217706) >> 16) + 63 + u - lz;  // the binary exponent of the leading bit
	if (E >= 1 - tr::bias) {
		m0 = (hi >> (63 + u - nb)) >> 1;
		eq = E - tr::mant;
		return cand_normal;
	}
	if (E < -tr::bias) { return cand_below; }  // below 2^(emin - 1) (1 + 2^-59): well below T
	// E = emin - 1: P is hn * 2^(emin - 64) with hn its leading 64 bits, and T is (2^64 - 2^(63 - mant)) * 2^(emin - 64); the
	// error of P is below 2^5 units of hn
	const std::uint64_t hn = u != 0 ? hi : (hi << 1) | (lo >> 63);
	const std::uint64_t t_hn = 0 - (1ULL << (63 - tr::mant));
	return hn < t_hn - 256 ? cand_below : cand_unknown;
}

// V = (X / Y) * 2^q against H = t * 2^g, t > 0: -1, 0 or 1; 2 when a big integer overflowed.
inline int cmp_scaled(const Big& X, const Big& Y, std::ptrdiff_t q, std::uint64_t t, std::ptrdiff_t g) noexcept {
	const std::ptrdiff_t mn = q < g ? q : g;
	Big a, b;
	a.copy(X);
	a.shl(q - mn);
	b.copy(Y);
	b.mul(t);
	b.shl(g - mn);
	if (a.ovf || b.ovf) { return 2; }
	return Big::cmp(a, b);
}

// The number of decimal digits of w != 0.
inline std::size_t digits10(std::uint64_t w) noexcept {
	const int t = ((64 - clz64(w)) * 1233) >> 12;  // floor(log10(2^bits))
	return static_cast<std::size_t>(t + 1 - (w < pow10_small.v[t] ? 1 : 0));
}

// 0: not converted (libc decides), 1: out holds the value, 2: out of range (infinity, or zero from a non-zero text, or
// an inexact subnormal: strtod sets ERANGE).
template <typename R, bool Division = false>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline int fp_slow(R& out, std::string_view s) noexcept {
	using tr = fp_traits<R>;
	using bits_t = typename tr::bits;
	constexpr int top = static_cast<int>(sizeof(bits_t) * 8 - 1);
	const char* p = s.data();
	const char* const e = p + s.size();
	if (p == e) { return 0; }
	const bool neg = *p == '-';
	p += neg;
	std::uint64_t wall = 0;  // all the digits as one number: the value of the text, when it has at most 19 digits
	const char* const ib = p;
	p = scan_digits(p, e, wall);
	const char* const ie = p;
	const char* fb = p;
	const char* fe = p;
	bool dot = false;
	if (p != e && *p == '.') {
		dot = true;
		fb = ++p;
		p = scan_digits(p, e, wall);
		fe = p;
	}
	if (ie == ib && fb == fe) { return 0; }
	std::ptrdiff_t q = 0;
	if (p != e) {
		if ((static_cast<unsigned char>(*p) | 0x20) != 'e') { return 0; }
		if (++p == e) { return 0; }
		const bool eneg = *p == '-';
		if ((eneg || *p == '+') && ++p == e) { return 0; }
		for (; p != e; ++p) {
			const unsigned d = static_cast<unsigned char>(*p) - '0';
			if (d > 9) { return 0; }
			if (q < 1000000000000000LL) { q = q * 10 + static_cast<std::ptrdiff_t>(d); }  // saturates: no text is long enough to undo it
		}
		if (eneg) { q = -q; }
	}
	if (!rounds_to_nearest()) { return 0; }
	if (dot && !decimal_point_is_dot()) { return 0; }

	// The significant digits as D * 10^q. A text of at most 19 digits is the number the scan made. A longer one keeps its first
	// 768 significant digits, and a final digit 1 when a later one is not zero (see above).
	std::uint8_t dg[slow_digits + 1];
	std::size_t nd = 0;
	std::uint64_t w = 0;  // the leading (at most 19) significant digits
	bool have_dg = false;
	const std::size_t nall = static_cast<std::size_t>(ie - ib) + static_cast<std::size_t>(fe - fb);
	if (nall <= 19) {
		if (wall == 0) {  // all zeros: zero, whatever the exponent
			out = std::bit_cast<R>(static_cast<bits_t>(static_cast<bits_t>(neg) << top));
			return 1;
		}
		w = wall;
		nd = digits10(wall);
		q -= static_cast<std::ptrdiff_t>(fe - fb);
		if (q == 0) {
			// An integer of at most 19 digits is below 2^64: one correctly rounded conversion (the mode is to nearest, ties to even), so
			// the exact halfway points the table declined need no comparison.
			out = std::bit_cast<R>(static_cast<bits_t>(std::bit_cast<bits_t>(static_cast<R>(wall)) | (static_cast<bits_t>(neg) << top)));
			return 1;
		}
	} else {
		have_dg = true;
		bool sticky = false;
		const auto take = [&](const char* from, std::size_t n) {
			for (std::size_t i = 0; i < n; ++i) { dg[nd + i] = static_cast<std::uint8_t>(from[i] - '0'); }
			nd += n;
		};
		const char* const a = skip_zeros(ib, ie);
		if (a != ie) {
			const std::size_t ni = static_cast<std::size_t>(ie - a);
			const std::size_t ti = ni < slow_digits ? ni : slow_digits;
			take(a, ti);
			q += static_cast<std::ptrdiff_t>(ni - ti);  // integer digits not taken are factors of 10
			sticky = ni > ti && any_nonzero(a + ti, ie);
			const std::size_t nf = static_cast<std::size_t>(fe - fb);
			const std::size_t tf = nf < slow_digits - nd ? nf : slow_digits - nd;
			take(fb, tf);
			q -= static_cast<std::ptrdiff_t>(tf);
			sticky = sticky || (nf > tf && any_nonzero(fb + tf, fe));
		} else {
			const char* const b = skip_zeros(fb, fe);
			if (b == fe) {  // all zeros: zero, whatever the exponent
				out = std::bit_cast<R>(static_cast<bits_t>(static_cast<bits_t>(neg) << top));
				return 1;
			}
			const std::size_t nf = static_cast<std::size_t>(fe - b);
			const std::size_t tf = nf < slow_digits ? nf : slow_digits;
			take(b, tf);
			q -= static_cast<std::ptrdiff_t>(static_cast<std::size_t>(b - fb) + tf);
			sticky = nf > tf && any_nonzero(b + tf, fe);
		}
		if (sticky) {
			dg[nd++] = 1;
			--q;
		}
		const std::size_t nlead = nd < 19 ? nd : 19;
		for (std::size_t i = 0; i < nlead; ++i) { w = w * 10 + dg[i]; }
	}
	// The value is in [10^(lg - 1), 10^lg).
	const std::ptrdiff_t lg = q + static_cast<std::ptrdiff_t>(nd);
	if (lg - 1 >= tr::dec_hi || lg <= tr::dec_lo) { return 2; }

	// The candidate from the table, from the leading (at most 19) digits.
	std::uint64_t m = 0;  // the mantissa, in units of 2^eu
	std::ptrdiff_t eu = 0;
	bool done = false;
	const std::size_t nlead = nd < 19 ? nd : 19;
	int cand = cand_unknown;
	int eq = 0;
	if (!Division) {
		const std::ptrdiff_t q19 = q + static_cast<std::ptrdiff_t>(nd - nlead);
		if (q19 >= pow5_qmin && q19 <= pow5_qmax) { cand = el_candidate<R>(w, static_cast<int>(q19), m, eq); }
	}
	if (cand == cand_below) {
		// A subnormal or zero result is ERANGE, unless the value is exactly representable (libc's errno then). That needs
		// 5^-q to divide D, the digits as a number, so D >= 5^-q and the digits number more than 0.69897 * -q.
		if (static_cast<std::ptrdiff_t>(nd) * 100000 <= -q * 69897) { return 2; }
		cand = cand_unknown;
	}

	// V = (X / Y) * 2^q, in big integers, when the comparison needs them
	Big X, Y;
	bool have_bigs = false;
	const auto bigs = [&]() noexcept {
		if (!have_bigs) {
			have_bigs = true;
			if (have_dg) { X.from_digits(dg, nd); } else { X.set(w); }
			if (q > 0) { X.mul_pow5(static_cast<unsigned>(q)); }
			Y.set(1);
			if (q < 0) { Y.mul_pow5(static_cast<unsigned>(-q)); }
		}
		return !(X.ovf || Y.ovf);
	};

	if (cand == cand_normal) {
		eu = eq;
		const bool small = nd <= 19 && q >= -27 && q <= 27;  // w is all of D, and the comparison fits in 128 bits
		// The value is below P (1 + 2^-59), a small fraction of a unit above the candidate: the rounded mantissa is m0 or
		// m0 + 1. When that is 2^(mant + 1), the value is within 2^-59 of the next power of two, so it rounds to it and the
		// renormalisation below applies.
		for (int i = 0; i < 4 && !done; ++i) {
			int c = 2;
			if (small) { c = cmp_small(w, static_cast<int>(q), 2 * m + 1, eu - 1); }
			if (c == 2) {
				if (!bigs()) { return 0; }
				c = cmp_scaled(X, Y, q, 2 * m + 1, eu - 1);
				if (c == 2) { return 0; }
			}
			if (c < 0) {
				done = true;
			} else if (c == 0) {
				m += m & 1;  // a tie: the even mantissa
				done = true;
			} else {
				++m;
			}
		}
		if (!done) { return 0; }
	}
	bool normal = done;
	if (!done) {
		if (!bigs()) { return 0; }
		// The exponent of the leading bit, exactly, then the quotient of V by the unit of the result.
		const std::ptrdiff_t r0 = X.bitlen() - Y.bitlen();
		Big x, y;
		x.copy(X);
		y.copy(Y);
		if (r0 >= 0) { y.shl(r0); } else { x.shl(-r0); }
		if (x.ovf || y.ovf) { return 0; }
		const std::ptrdiff_t E = (Big::cmp(x, y) >= 0 ? r0 : r0 - 1) + q;
		eu = E - tr::mant;
		if (eu < tr::qsub) { eu = tr::qsub; }
		const std::ptrdiff_t t = q - eu;
		Big N, Dn;
		N.copy(X);
		Dn.copy(Y);
		if (t >= 0) { N.shl(t); } else { Dn.shl(-t); }
		if (N.ovf || Dn.ovf) { return 0; }
		const std::ptrdiff_t k = N.bitlen() - Dn.bitlen();
		if (k > 62) { return 0; }
		m = 0;
		if (k >= 0) {
			Big Ds;
			Ds.copy(Dn);
			Ds.shl(k);
			if (Ds.ovf) { return 0; }
			for (std::ptrdiff_t i = k; i >= 0; --i) {
				if (Big::cmp(N, Ds) >= 0) {
					N.sub(Ds);
					m |= std::uint64_t(1) << i;
				}
				if (i != 0) { Ds.shr1(); }
			}
		}
		const bool exact = N.zero();
		N.shl(1);
		if (N.ovf) { return 0; }
		const int c = Big::cmp(N, Dn);
		if (c > 0 || (c == 0 && (m & 1) != 0)) { ++m; }
		normal = E >= 1 - tr::bias;
		if (!normal) {
			if (m >= (std::uint64_t(1) << tr::mant)) { return 0; }  // rounded up to the smallest normal value: libc's errno
			if (m == 0) { return 2; }                               // zero from a non-zero text
			return exact ? 0 : 2;                                   // an exact subnormal: libc's errno; an inexact one: ERANGE
		}
	}
	// A normal value: m is in [2^mant, 2^(mant + 1)], the last when it rounded up to the next power of two.
	if (m == (std::uint64_t(1) << (tr::mant + 1))) {
		m >>= 1;
		++eu;
	}
	const std::ptrdiff_t be = eu + tr::mant + tr::bias;
	if (be >= tr::max_be) { return 2; }
	const bits_t b = static_cast<bits_t>((static_cast<bits_t>(be) << tr::mant) | static_cast<bits_t>(m & ((std::uint64_t(1) << tr::mant) - 1)));
	out = std::bit_cast<R>(static_cast<bits_t>(b | (static_cast<bits_t>(neg) << top)));
	return 1;
}

// True when the text can be decimal: an optional '-', then a digit or '.'. fast_fp and fp_slow decline every other first byte
// (white space, '+', the letters of inf and nan, nothing at all), so such text goes to strto* without trying either.
inline bool starts_decimal(const char* p, std::size_t n) noexcept {
	if (n == 0) { return false; }
	unsigned c = static_cast<unsigned char>(*p);
	if (c == '-') {
		if (n == 1) { return false; }
		c = static_cast<unsigned char>(p[1]);
	}
	return c - '0' <= 9 || c == '.';
}

// What follows a declined fast_fp, out of line so that the code the short texts run (the wrapper around fast_fp) is small and as
// it was: the exact path, then its result for the caller. 0: the strto* path decides; 1: out holds the value and idx the whole
// text; 2: ERANGE (out is 0, idx untouched).
template <typename R>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
inline int fp_decline(R& out, std::string_view s, std::size_t* idx) noexcept {
#ifdef STRICT_STOX_COUNT_LIBC
	++decline_calls;
#endif
	const int status = fp_slow<R>(out, s);
	if (status == 1) {
		if (idx != nullptr) { *idx = s.size(); }
	} else if (status == 2) {
		out = 0;
	}
	return status;
}


}  // namespace strict_stox_detail


#ifdef STRICT_STOX_COUNT_LIBC
// Test seam: stof and stod call these, which count the texts that reach libc.
namespace strict_stox_detail {
inline thread_local std::size_t libc_calls = 0;
inline float counted_strtof(const char* s, char** e) noexcept {
	++libc_calls;
	return std::strtof(s, e);
}
inline double counted_strtod(const char* s, char** e) noexcept {
	++libc_calls;
	return std::strtod(s, e);
}
}  // namespace strict_stox_detail
#define STRICT_STOX_STRTOF strict_stox_detail::counted_strtof
#define STRICT_STOX_STRTOD strict_stox_detail::counted_strtod
#else
#define STRICT_STOX_STRTOF std::strtof
#define STRICT_STOX_STRTOD std::strtod
#endif

// Types conversion strict_stox and strict_nothrow_stox
template <typename F>
class Stox {
	const char * const name;
	F func;

	template <typename T, typename... Args>
	auto _stox(std::true_type, int& err, const std::string_view& str, std::size_t* idx, Args&&... args) noexcept {
#ifdef STRICT_STOX_COUNT_LIBC
		++strict_stox_detail::strto_calls;
#endif
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
		if (err) return static_cast<decltype(r)>(0);
		if (ptr == buf) {
			err = EINVAL;
			return static_cast<decltype(r)>(0);
		}
		if (idx) {
			*idx = static_cast<size_t>(ptr - buf);
		} else if (ptr != end) {
			err = EINVAL;
			return static_cast<decltype(r)>(0);
		}
		return r;
	}

	template <typename T, typename... Args>
	auto _stox(std::false_type, int& err, const std::string_view& str, std::size_t* idx, Args&&... args) noexcept {
		auto r = _stox<void>(std::true_type{}, err, str, idx, std::forward<Args>(args)...);
		if (err) return static_cast<T>(0);
		if (r < std::numeric_limits<T>::min() || std::numeric_limits<T>::max() < r) {
			err = ERANGE;
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

	// Try the direct conversion. 1: `out` holds the final value (already range-checked for T),
	// exactly what the strto* path returns with errno == 0. 2: the strto* path would set ERANGE
	// (`out` is 0, `idx` untouched). 0: defer to the strto* path.
	template <typename T, typename Out, typename... Args>
	int _fast(Out& out, const std::string_view& str, std::size_t* idx, Args&&... args) const noexcept {
		if constexpr (sizeof...(Args) == 1 && std::is_invocable_v<F, const char*, char**, int>) {
			if (!(_is(&std::strtol) || _is(&std::strtoul) || _is(&std::strtoll) || _is(&std::strtoull))) { return 0; }
			using R = std::invoke_result_t<F, const char*, char**, int>;
			if constexpr (std::is_integral_v<R>) {
				const int base = (static_cast<int>(args), ...);
				R r;
				const int status = strict_stox_detail::fast_int(r, str, base);
				if (status != 1) {
					if (status == 0) { return 0; }
					out = static_cast<Out>(0);
					return 2;
				}
				if constexpr (!std::is_void_v<T>) {
					if (r < std::numeric_limits<T>::min() || std::numeric_limits<T>::max() < r) { return 0; }
				}
				out = static_cast<Out>(r);
				if (idx) { *idx = str.size(); }
				return 1;
			}
		} else if constexpr (sizeof...(Args) == 0 && std::is_invocable_v<F, const char*, char**>) {
			using R = std::invoke_result_t<F, const char*, char**>;
			if constexpr (std::is_same_v<R, float> || std::is_same_v<R, double>) {
				if (!(_is(&STRICT_STOX_STRTOF) || _is(&STRICT_STOX_STRTOD))) { return 0; }
				R r;
				if (strict_stox_detail::fast_fp(r, str)) {
					out = static_cast<Out>(r);
					if (idx) { *idx = str.size(); }
					return 1;
				}
				static_assert(std::is_same_v<Out, R>);
				if (!strict_stox_detail::starts_decimal(str.data(), str.size())) { return 0; }
				return strict_stox_detail::fp_decline<R>(out, str, idx);
			}
		}
		return 0;
	}

public:
	Stox(const char* _name, F _func)
		: name(_name),
		  func(_func) { }

	// The caller's errno is saved and cleared around the strto* path. `errno` is one object per thread, but a libc may spell it as a call
	// (macOS: `*__error()`), so it is named once here and passed down.
	template <typename T, typename... Args>
	auto stox_nothrow(int* errno_save, const std::string_view& str, std::size_t* idx, Args&&... args) {
		result_t<T, Args...> fast;
		if (const int status = _fast<T>(fast, str, idx, args...)) {
			if (errno_save != nullptr) { *errno_save = status == 2 ? ERANGE : 0; }
			return fast;
		}
		int& err = errno;
		const int saved = err;
		err = 0;
		auto r = _stox<T>(std::is_same<void, T>{}, err, str, idx, std::forward<Args>(args)...);
		const int got = err;
		err = saved;
		if (errno_save != nullptr) {
			*errno_save = got;
		}
		return r;
	}

	template <typename T, typename... Args>
	auto stox_throw(const std::string_view& str, std::size_t* idx, Args&&... args) {
		result_t<T, Args...> fast;
		if (const int status = _fast<T>(fast, str, idx, args...)) {
			if (status == 2) { throw std::out_of_range(std::string(name) + ": Out of range value: " + std::string(str)); }
			return fast;
		}
		int& err = errno;
		const int saved = err;
		err = 0;
		auto r = _stox<T>(std::is_same<void, T>{}, err, str, idx, std::forward<Args>(args)...);
		const int got = err;
		err = saved;
		switch (got) {
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
STOXIFY(stof, STRICT_STOX_STRTOF, void)
STOXIFY(stod, STRICT_STOX_STRTOD, void)
STOXIFY(stold, std::strtold, void)
#undef STOXIFY_BASE
#undef STOXIFY
#undef STRICT_STOX_STRTOF
#undef STRICT_STOX_STRTOD
