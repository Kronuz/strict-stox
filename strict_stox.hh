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
#include <cfloat>             // for FLT_EVAL_METHOD
#include <clocale>            // for std::localeconv
#include <cstdint>            // for std::uint64_t
#include <cstdio>             // for std::size_t
#include <cstdlib>            // for std::strto*
#include <cstring>            // for std::strncpy
#include <limits>             // for std::numeric_limits
#include <stdexcept>          // for std::invalid_argument, std::out_of_range
#include <string>             // for std::string
#include <string_view>        // for std::string_view
#include <errno.h>            // for errno
#include <type_traits>        // for std::true_type, std::false_type
#include <utility>            // for std::swap


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

// float/double: [-]digits[.digits][(e|E)[+-]digits], whole input, at most 17 bytes,
// at most 19 digits, mantissa <= 2^mant_bits and |decimal exponent| <= max_exp.
// Then value = mantissa * or / 10^k with both operands exactly representable,
// and one IEEE operation is correctly rounded (Clinger's fast path).
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD == 0
#define STRICT_STOX_FAST_FP 1
template <typename R>
inline bool fast_fp(R& out, std::string_view s) noexcept {
	constexpr bool is_double = std::is_same_v<R, double>;
	constexpr std::uint64_t max_mant = is_double ? (1ULL << 53) : (1ULL << 24);
	constexpr int max_exp = is_double ? 22 : 10;
	static constexpr R pow10[23] = {
		1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
		R(1e12), R(1e13), R(1e14), R(1e15), R(1e16), R(1e17), R(1e18), R(1e19), R(1e20), R(1e21), R(1e22),
	};
	const std::size_t n = s.size();
	// Longer text is almost always a full-precision (17 digit) value, which needs more than
	// 53 mantissa bits: decline at once instead of parsing it twice.
	if (n == 0 || n > 17) { return false; }
	const char* p = s.data();
	const char* const e = p + n;
	const bool neg = *p == '-';
	if (neg && ++p == e) { return false; }
	std::uint64_t w = 0;
	int ndigits = 0;
	int frac = 0;
	bool dot = false;
	for (; p != e; ++p) {
		const unsigned d = static_cast<unsigned char>(*p) - '0';
		if (d < 10) {
			if (++ndigits > 19) { return false; }
			w = w * 10 + d;
			if (w > max_mant) { return false; }
			if (dot) { ++frac; }
		} else if (*p == '.' && !dot) {
			dot = true;
		} else {
			break;
		}
	}
	if (ndigits == 0) { return false; }
	int ex = 0;
	if (p != e) {
		if (*p != 'e' && *p != 'E') { return false; }
		if (++p == e) { return false; }
		const bool eneg = *p == '-';
		if ((eneg || *p == '+') && ++p == e) { return false; }
		int ed = 0;
		for (; p != e; ++p) {
			const unsigned d = static_cast<unsigned char>(*p) - '0';
			if (d >= 10 || ++ed > 3) { return false; }
			ex = ex * 10 + static_cast<int>(d);
		}
		if (eneg) { ex = -ex; }
	}
	// strtod reads the decimal point of the current locale: only take '.' when it is the one.
	if (dot && *std::localeconv()->decimal_point != '.') { return false; }
	R r;
	if (w == 0) {
		r = 0;
	} else {
		const int k = ex - frac;
		if (w > max_mant || k > max_exp || k < -max_exp) { return false; }
		r = k >= 0 ? static_cast<R>(w) * pow10[k] : static_cast<R>(w) / pow10[-k];
	}
	out = neg ? -r : r;
	return true;
}
#endif

}  // namespace strict_stox_detail


// Types conversion strict_stox and strict_nothrow_stox
template <typename F>
class Stox {
	const char * const name;
	F func;

	template <typename T, typename... Args>
	auto _stox(std::true_type, const std::string_view& str, std::size_t* idx, Args&&... args) noexcept {
		char buf[64]{};
		size_t size = std::min(str.size(), sizeof(buf) - 1);
		std::strncpy(buf, str.data(), size);
		auto end = buf + size;
		auto ptr = const_cast<char*>(end);
		auto r = func(buf, &ptr, std::forward<Args>(args)...);
		if (errno) return static_cast<decltype(r)>(0);
		if (ptr == buf) {
			errno = EINVAL;
			return static_cast<decltype(r)>(0);
		}
		if (idx) {
			*idx = static_cast<size_t>(ptr - buf);
		} else if (ptr != end || size != str.size()) {
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
#ifdef STRICT_STOX_FAST_FP
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
#endif
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
