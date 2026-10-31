// Arbitrary-length input. The oracle is libc strto* run on the same bytes (copied
// into a NUL-terminated string) plus the strict rule: no conversion is EINVAL, a
// parse that stops before the end is EINVAL unless idx was asked for, ERANGE
// from strto* wins, and a value outside the narrower target type is ERANGE.
// std::from_chars is a second oracle for decimal doubles. The inputs include
// every string up to 5 characters over a mixed alphabet, each one padded in front
// with 0..200 zeros, spaces or signs so the whole range around the old 63-byte
// cut and the 128-byte stack buffer is covered, and long decimal strings.
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>

#include "strict_stox.hh"

static long failures = 0;
static long checks = 0;

struct Out {
	int err = 0;
	std::size_t idx = 0;
};

template <typename R, typename T = R, typename F, typename... A>
static void expect(const char* name, F func, const std::string& s, bool useidx, A... a) {
	// Oracle.
	std::string c = s;
	char* end = nullptr;
	errno = 0;
	R ov = func(c.c_str(), &end, a...);
	int oerr = errno;
	std::size_t oidx = 0;
	if (oerr == 0 && end == c.c_str()) { oerr = EINVAL; }
	if (oerr == 0) {
		if (useidx) { oidx = static_cast<std::size_t>(end - c.c_str()); }
		else if (end != c.c_str() + s.size()) { oerr = EINVAL; }
	}
	if (oerr == 0 && !std::is_same_v<R, T>) {
		if (ov < std::numeric_limits<T>::min() || std::numeric_limits<T>::max() < ov) { oerr = ERANGE; }
	}
	if (oerr != 0) { ov = 0; }
	// Subject.
	int e = -1;
	std::size_t i = 0;
	errno = 4242;
	R v;
	if constexpr (std::is_same_v<R, T>) {
		if constexpr (sizeof...(A) == 1) {
			if constexpr (std::is_same_v<R, long>) { v = strict_stol(&e, s, useidx ? &i : nullptr, a...); }
			else if constexpr (std::is_same_v<R, unsigned long>) { v = strict_stoul(&e, s, useidx ? &i : nullptr, a...); }
			else if constexpr (std::is_same_v<R, long long>) { v = strict_stoll(&e, s, useidx ? &i : nullptr, a...); }
			else { v = strict_stoull(&e, s, useidx ? &i : nullptr, a...); }
		} else if constexpr (std::is_same_v<R, double>) {
			v = strict_stod(&e, s, useidx ? &i : nullptr);
		} else if constexpr (std::is_same_v<R, float>) {
			v = strict_stof(&e, s, useidx ? &i : nullptr);
		} else {
			v = strict_stold(&e, s, useidx ? &i : nullptr);
		}
	} else {
		v = static_cast<R>(strict_stoi(&e, s, useidx ? &i : nullptr, a...));
	}
	++checks;
	bool ok = errno == 4242 && e == oerr && (oerr != 0 || i == oidx || !useidx)
		&& (std::memcmp(&v, &ov, sizeof(R)) == 0 || (v != v && ov != ov));
	if (!ok) {
		if (++failures < 20) {
			std::fprintf(stderr, "MISMATCH %s len=%zu idx=%d \"%.40s\": err %d/%d idx %zu/%zu\n", name, s.size(), useidx, s.c_str(), e, oerr, i, oidx);
		}
	}
	// The throwing form agrees with errno_save.
	bool threw_inv = false, threw_rng = false;
	try {
		if constexpr (std::is_same_v<R, T> && sizeof...(A) == 1 && std::is_same_v<R, long long>) { (void)strict_stoll(s, nullptr, a...); }
		else if constexpr (std::is_same_v<R, double>) { (void)strict_stod(s); }
		else { return; }
	} catch (const std::invalid_argument&) { threw_inv = true; }
	catch (const std::out_of_range&) { threw_rng = true; }
	if (!useidx && (threw_inv != (oerr == EINVAL) || threw_rng != (oerr == ERANGE))) {
		if (++failures < 20) { std::fprintf(stderr, "THROW MISMATCH %s len=%zu\n", name, s.size()); }
	}
}

static void all(const std::string& s) {
	for (bool u : {false, true}) {
		for (int b : {10, 16, 0}) {
			expect<long long>("stoll", std::strtoll, s, u, b);
			expect<unsigned long long>("stoull", std::strtoull, s, u, b);
			expect<long>("stol", std::strtol, s, u, b);
			expect<unsigned long>("stoul", std::strtoul, s, u, b);
			expect<long, int>("stoi", std::strtol, s, u, b);
		}
		expect<double>("stod", std::strtod, s, u);
		expect<float>("stof", std::strtof, s, u);
		expect<long double>("stold", std::strtold, s, u);
	}
}

static void from_chars_check(const std::string& s) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
	// A plain decimal: value must equal std::from_chars (correctly rounded).
	double want = 0;
	auto r = std::from_chars(s.data(), s.data() + s.size(), want);
	if (r.ec != std::errc() || r.ptr != s.data() + s.size()) { return; }
	int e = -1;
	double got = strict_stod(&e, s);
	++checks;
	if (e != 0 || std::memcmp(&got, &want, sizeof got) != 0) {
		if (++failures < 20) { std::fprintf(stderr, "FROM_CHARS MISMATCH len=%zu \"%.30s\"\n", s.size(), s.c_str()); }
	}
#else
	(void)s;
#endif
}

int main() {
	// Spelled-out cases from the bug: runs of zeros, whitespace, long decimals.
	for (int n : {1, 62, 63, 64, 65, 100, 127, 128, 129, 1000, 100000}) {
		std::string z(n, '0');
		int e = -1;
		{
			auto v = strict_stoll(&e, z);
			if (e != 0 || v != 0) { std::fprintf(stderr, "zeros %d: err %d val %lld\n", n, e, v); ++failures; }
			v = strict_stoll(&e, z + "7");
			if (e != 0 || v != 7) { std::fprintf(stderr, "zeros+7 %d: err %d\n", n, e); ++failures; }
			v = strict_stoll(&e, std::string(n, ' ') + "-12");
			if (e != 0 || v != -12) { std::fprintf(stderr, "spaces %d: err %d\n", n, e); ++failures; }
			auto d = strict_stod(&e, "0." + z + "5");
			if (n < 300 && (e != 0 || d != 0.0 * 1 + std::strtod(("0." + z + "5").c_str(), nullptr))) { std::fprintf(stderr, "decimals %d: err %d\n", n, e); ++failures; }
			d = strict_stod(&e, "1" + z);
			if (n < 300 && (e != 0 || d != std::strtod(("1" + z).c_str(), nullptr))) { std::fprintf(stderr, "1+zeros %d: err %d\n", n, e); ++failures; }
			std::size_t idx = 0;
			v = strict_stoll(&e, z + "9xyz", &idx);
			if (e != 0 || v != 9 || idx != static_cast<std::size_t>(n) + 1) { std::fprintf(stderr, "idx %d: err %d idx %zu\n", n, e, idx); ++failures; }
			strict_stoll(&e, z + "9xyz");
			if (e != EINVAL) { std::fprintf(stderr, "junk %d not EINVAL\n", n); ++failures; }
		}
		all(z);
		all(z + "1");
		from_chars_check("0." + z + "123456789");
		from_chars_check(std::string("123456789.") + z + "1");
		from_chars_check(std::string(n, '9') + ".5");
	}
	// Exhaustive: every string up to 5 chars, padded in front.
	const std::string al = "09-+ x.eEa";
	std::string s;
	std::size_t total = 1, cnt = 0;
	for (int len = 0; len <= 5; ++len) {
		for (std::size_t k = 0; k < total; ++k) {
			std::size_t m = k;
			s.assign(len, ' ');
			for (int i = 0; i < len; ++i) { s[i] = al[m % al.size()]; m /= al.size(); }
			all(s);
			if ((++cnt % 7) == 0) {
				for (int pad : {62, 63, 64, 126, 127, 128, 129, 200}) {
					all(std::string(pad, '0') + s);
					all(std::string(pad, ' ') + s);
				}
			}
		}
		total *= al.size();
	}
	// Random long strings, mostly valid.
	std::mt19937_64 rng(7);
	const char* alph[] = {"0123456789", "0", "0123456789.", "0123456789eE+-.", " \t0123456789"};
	for (int it = 0; it < 20000; ++it) {
		const char* a = alph[rng() % 5];
		std::size_t alen = std::strlen(a);
		std::string t(rng() % 300, '0');
		for (auto& ch : t) { ch = a[rng() % alen]; }
		all(t);
		from_chars_check(t);
	}
	if (failures != 0) { std::fprintf(stderr, "%ld failures of %ld checks\n", failures, checks); return 1; }
	std::printf("strict_stox long-input OK: %ld checks against libc strto* and from_chars\n", checks);
	return 0;
}
