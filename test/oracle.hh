// The oracle for the floating point tests: libc strtod / strtof on a NUL-terminated copy of the text,
// under the strict rules (value 0 on error, EINVAL without a number, or with trailing text when no
// idx is wanted), and the public API in the same terms. Header-only, for test code.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#include "strict_stox.hh"

namespace oracle {

struct Result {
	std::uint64_t bits = 0;  // the value as bits (zero on error)
	int err = 0;
	std::size_t idx = 0;     // only with useidx and no error
	bool errno_kept = true;  // the caller's errno survived the call
};

template <typename R>
inline std::uint64_t to_bits(R v) {
	std::uint64_t b = 0;
	std::memcpy(&b, &v, sizeof(R));
	return b;
}

template <typename R>
inline Result libc_result(const std::string& s, bool useidx) {
	Result r;
	char* end = nullptr;
	errno = 0;
	R v;
	if constexpr (std::is_same_v<R, double>) { v = std::strtod(s.c_str(), &end); } else { v = std::strtof(s.c_str(), &end); }
	r.err = errno;
	if (r.err == 0 && end == s.c_str()) { r.err = EINVAL; }
	if (r.err == 0) {
		if (useidx) { r.idx = static_cast<std::size_t>(end - s.c_str()); }
		else if (end != s.c_str() + s.size()) { r.err = EINVAL; }
	}
	r.bits = r.err != 0 ? 0 : to_bits(v);
	return r;
}

template <typename R>
inline Result api_result(const std::string& s, bool useidx) {
	Result r;
	std::size_t idx = 0;
	int e = -1;
	errno = 4242;
	R v;
	if constexpr (std::is_same_v<R, double>) { v = strict_stod(&e, s, useidx ? &idx : nullptr); } else { v = strict_stof(&e, s, useidx ? &idx : nullptr); }
	r.errno_kept = errno == 4242;
	r.err = e;
	r.idx = idx;
	r.bits = to_bits(v);
	return r;
}

// True when the API and libc agree on value, errno and idx (and the caller's errno survived).
inline bool same(const Result& got, const Result& want, bool useidx) {
	return got.errno_kept && got.bits == want.bits && got.err == want.err && !(want.err == 0 && useidx && got.idx != want.idx);
}

}  // namespace oracle
