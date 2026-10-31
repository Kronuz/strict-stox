// Differential test: the strict_stox fast path must match the frozen
// pre-optimization header (test/strict_stox_old.hh, namespace old) in result
// (bit for bit), errno_save, idx, errno preservation and thrown exception, on
// random and edge inputs for every function and several bases.
//
// One input class changed on purpose: text of 64 bytes or more. The frozen
// header cut it at 63 bytes (a long run of zeros or spaces was rejected, a long
// decimal parsed as its first 63 bytes with the wrong idx); the current header
// parses every byte like libc strto*. For that class the comparison with the old
// result is skipped here, test/long_test.cc checks the new result against libc,
// and `deliberate_differences` below asserts old != new on constructed valid
// inputs, so the exception cannot widen unnoticed.
#include <algorithm>
#include <cerrno>
#include <iterator>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "strict_stox.hh"
namespace old {
#include "strict_stox_old.hh"
}

static long failures = 0;
static bool longcls = false;  // input of 64 bytes or more: result deliberately differs

template <typename A>
static bool same(const A& a, const A& b) {
	return std::memcmp(&a, &b, sizeof(A)) == 0;
}
template <>
bool same<long double>(const long double& a, const long double& b) {
	return (std::isnan(a) && std::isnan(b)) || a == b;  // x87 padding bytes
}

#define CMP(NAME, ...)                                                                              \
	do {                                                                                            \
		int e1 = -1, e2 = -1;                                                                       \
		std::size_t i1 = 12345, i2 = 12345;                                                         \
		bool useidx = (rng() & 1) != 0;                                                             \
		errno = 77;                                                                                 \
		auto r1 = strict_##NAME(&e1, s, useidx ? &i1 : nullptr __VA_OPT__(,) __VA_ARGS__);          \
		bool k1 = errno == 77;                                                                      \
		errno = 77;                                                                                 \
		auto r2 = old::strict_##NAME(&e2, s, useidx ? &i2 : nullptr __VA_OPT__(,) __VA_ARGS__);     \
		bool k2 = errno == 77;                                                                      \
		if (!longcls && (!same(r1, r2) || e1 != e2 || i1 != i2 || !k1 || !k2)) {                                  \
			if (++failures < 20) {                                                                  \
				std::fprintf(stderr, "MISMATCH %s \"%s\" len=%zu idx=%d: err %d/%d idx %zu/%zu errno-kept %d/%d\n", #NAME, s.c_str(), s.size(), useidx, e1, e2, i1, i2, k1, k2); \
			}                                                                                       \
		}                                                                                           \
		std::string w1 = "none", w2 = "none";                                                       \
		try { (void)strict_##NAME(s, nullptr __VA_OPT__(,) __VA_ARGS__); } catch (const std::exception& x) { w1 = x.what(); } \
		try { (void)old::strict_##NAME(s, nullptr __VA_OPT__(,) __VA_ARGS__); } catch (const std::exception& x) { w2 = x.what(); } \
		if (!longcls && w1 != w2) { if (++failures < 20) { std::fprintf(stderr, "THROW MISMATCH %s \"%s\": %s | %s\n", #NAME, s.c_str(), w1.c_str(), w2.c_str()); } } \
	} while (0)

static void check_all(std::mt19937_64& rng, const std::string& s) {
	longcls = s.size() >= 64;
	static const int bases[] = {10, 16, 2, 8, 36, 0, 1, 37, -1};
	for (int b : bases) {
		if (b != 10 && b != 16 && (rng() % 4) != 0) { continue; }
		CMP(stoi, b);
		CMP(stou, b);
		CMP(stol, b);
		CMP(stoul, b);
		CMP(stoll, b);
		CMP(stoull, b);
		CMP(stoz, b);
	}
	CMP(stof);
	CMP(stod);
	CMP(stold);
}

// Valid long inputs: the old header rejects or truncates them, the new one accepts.
static void deliberate_differences() {
	for (int n : {64, 65, 100, 200}) {
		std::string z(n, '0');
		int en = -1, eo = -1;
		long long vn = strict_stoll(&en, z), vo = old::strict_stoll(&eo, z);
		if (!(en == 0 && vn == 0 && eo == EINVAL && vo == 0)) { std::fprintf(stderr, "zeros %d: new %d/%lld old %d/%lld\n", n, en, vn, eo, vo); ++failures; }
		std::string d = "1." + std::string(n, '0') + "5";   // 1.0...05 == 1.0 in double
		double dn = strict_stod(&en, d), dold = old::strict_stod(&eo, d);
		if (!(en == 0 && dn == 1.0 && eo == EINVAL && dold == 0.0)) { std::fprintf(stderr, "decimal %d: new %d/%g old %d/%g\n", n, en, dn, eo, dold); ++failures; }
		std::size_t in = 0, io = 0;
		strict_stoll(&en, z + "7xy", &in);
		old::strict_stoll(&eo, z + "7xy", &io);
		if (!(en == 0 && in == z.size() + 1 && io != in)) { std::fprintf(stderr, "idx %d: new %zu old %zu\n", n, in, io); ++failures; }
	}
}

int main() {
	deliberate_differences();
	std::mt19937_64 rng(2024);
	const char* alphabets[] = {
		"0123456789", "0123456789", "0123456789-", "0123456789abcdefABCDEF", "0123456789abcdefxX+- ",
		"0123456789.eE+-", "0123456789.", "0123456789eE.-+ \t\n", "xXpP0123456789abcdef.-+", "01", "zZ09",
		"infINFnaNAN0-+.e",
	};
	for (int iter = 0; iter < 60000; ++iter) {
		const char* al = alphabets[rng() % (sizeof(alphabets) / sizeof(*alphabets))];
		size_t alen = std::strlen(al);
		size_t n = rng() % 24;
		if (iter % 97 == 0) { n = 55 + rng() % 16; }  // around the 63-byte truncation
		std::string s;
		for (size_t i = 0; i < n; ++i) { s += al[rng() % alen]; }
		if (iter % 211 == 0 && !s.empty()) { s[rng() % s.size()] = '\0'; }
		check_all(rng, s);
	}
	// Edge list.
	const char* edges[] = {
		"", "-", "+", "0", "-0", "00", "1", "-1", "2147483647", "2147483648", "-2147483648", "-2147483649",
		"4294967295", "4294967296", "9223372036854775807", "9223372036854775808", "-9223372036854775808",
		"-9223372036854775809", "18446744073709551615", "18446744073709551616", "-18446744073709551615",
		"99999999999999999999999", " 5", "5 ", "+5", "0x1f", "0X1F", "1f", "ff", "0xzz", "010", "0b11",
		"1.5", "-1.5", ".5", "5.", ".", "-.", "1e5", "1e", "1e+", "1e-5", "1E22", "1e23", "1e-22", "1e-23",
		"9007199254740992", "9007199254740993", "16777216", "16777217", "123456789012345678", "1234567890123456789",
		"12345678901234567890", "0.1", "0.3", "3.14159", "2.2250738585072014e-308", "1e308", "1e309", "1e-400",
		"inf", "-inf", "nan", "0x1p3", "1.7976931348623157e308", "-0.0", "0e999", "0.000000000000000000001",
		"4.9e-324", "123.456e2", "1.0000000000000002", "8.5", "1,5", "١٢٣",
	};
	for (const char* e : edges) { std::string s(e); check_all(rng, s); }
	// Digit runs of every length around the overflow and 63-byte limits.
	for (int len = 1; len <= 70; ++len) {
		for (char d : {'0', '1', '9', '7'}) {
			std::string s(len, d);
			check_all(rng, s);
			check_all(rng, "-" + s);
		}
	}
	if (failures != 0) { std::fprintf(stderr, "%ld mismatches\n", failures); return 1; }
	std::printf("strict_stox differential OK\n");
	return 0;
}
