// strict_stod / strict_stof under every IEEE rounding mode.
//
// strtod and strtof follow the current rounding mode (fesetround): in FE_UPWARD the text
// "0.1" is the double just above 0.1 and "-0.1" the double just above -0.1, which is the one
// nearer to zero. The fast path converts to nearest, so it must stand aside in any other mode
// and the answer must be whatever libc gives, for positive and negative texts alike.
//
// Per mode (nearest, upward, downward, toward zero) and per text of the corpus, the public API
// (value bits, errno, idx, with and without idx) must equal libc, and a text taken by the fast
// path must equal libc's bits with no errno and the whole text consumed. The corpus is built once,
// in the default mode (printf follows the rounding mode too). The test is not vacuous: libc must
// give a different answer in the directed modes for many texts, the fast path must take most texts
// in the default mode, and a few results are checked against values derived by hand.
//
// The rounding mode belongs to the calling thread: a thread in FE_DOWNWARD must not change what a
// thread in the default mode gets.
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "oracle.hh"
#include "strict_stox.hh"

namespace D = strict_stox_detail;

static long failures = 0;
#define FAIL(...)                                  \
	do {                                           \
		if (++failures <= 30) {                    \
			std::fprintf(stderr, __VA_ARGS__);     \
			std::fprintf(stderr, "\n");            \
		}                                          \
	} while (0)

struct Mode {
	int value;
	const char* name;
};
static const Mode modes[] = {{FE_TONEAREST, "to nearest"}, {FE_UPWARD, "upward"}, {FE_DOWNWARD, "downward"}, {FE_TOWARDZERO, "toward zero"}};

// Sets a rounding mode for the current scope.
struct ScopedMode {
	int saved;
	bool ok;
	explicit ScopedMode(int m) : saved(std::fegetround()), ok(std::fesetround(m) == 0 && std::fegetround() == m) { }
	~ScopedMode() { std::fesetround(saved); }
};

template <typename R>
static bool bits_equal(R a, R b) { return std::memcmp(&a, &b, sizeof(R)) == 0; }

using oracle::api_result;
using oracle::libc_result;
using oracle::Result;
using oracle::to_bits;

struct Counts {
	long checks = 0, accepted = 0, differs = 0;
};

// One text, one type, in the current mode. `nearest` is libc's answer to the same text in the default mode.
template <typename R>
static void check(const std::string& s, const char* mode, std::uint64_t nearest, Counts& c) {
	const char* type = std::is_same_v<R, double> ? "stod" : "stof";
	for (int useidx = 0; useidx < 2; ++useidx) {
		const Result want = libc_result<R>(s, useidx != 0);
		const Result got = api_result<R>(s, useidx != 0);
		if (!oracle::same(got, want, useidx != 0)) {
			FAIL("[%s] %s(\"%.60s\"%s) len=%zu: bits %llx errno %d idx %zu kept %d, libc bits %llx errno %d idx %zu", mode, type, s.c_str(), useidx ? ", idx" : "", s.size(),
				static_cast<unsigned long long>(got.bits), got.err, got.idx, got.errno_kept, static_cast<unsigned long long>(want.bits), want.err, want.idx);
		}
	}
	++c.checks;
	const Result ref = libc_result<R>(s, false);
	if (ref.err == 0 && ref.bits != nearest) { ++c.differs; }
	R fast = 0;
	if (D::fast_fp<R>(fast, s)) {
		++c.accepted;
		if (ref.err != 0 || to_bits(fast) != ref.bits) {
			FAIL("[%s] fast path took %s \"%.60s\" len=%zu: bits %llx, libc bits %llx errno %d", mode, type, s.c_str(), s.size(), static_cast<unsigned long long>(to_bits(fast)),
				static_cast<unsigned long long>(ref.bits), ref.err);
		}
	}
}

// Texts. Built in the default rounding mode.
static std::vector<std::string> make_corpus() {
	std::vector<std::string> v = {
		"0.1", "-0.1", ".10", "-.10", "0.2", "-0.2", "0.3", "-0.3", "1e23", "-1e23", "8.5", "-8.5", "4.35", "-4.35", "2.675", "-2.675",
		"0.30000000000000004", "-0.30000000000000004", "9007199254740993", "-9007199254740993", "9007199254740992.5", "-9007199254740992.5",
		"123456789012345678901234567890", "-123456789012345678901234567890", "1.7976931348623157e308", "-1.7976931348623157e308",
		"1.7976931348623158e308", "2.2250738585072014e-308", "-2.2250738585072014e-308", "4.9e-324", "-4.9e-324", "1e-400", "-1e-400",
		"1e400", "-1e400", "3.4028235e38", "-3.4028235e38", "1.17549435e-38", "1e-46", "16777217", "-16777217", "0.1e1", "-0e0", "0", "42", "-42", "1e5", "-1.5e-3",
		"0.9999999999999999", "1.0000000000000001", "-1.0000000000000001", "1.00000000000000011102230246251565404236316680908203125",
		"0.100000000000000005551115123125782702118158340454101562500000000000000000000000000001",
	};
	std::mt19937_64 rng(20261031);
	char b[96];
	for (int i = 0; i < 6000; ++i) {
		// decimal text: [-]d{1,17}[.d*][e[+-]d{1,3}]
		std::string s;
		if (rng() % 3 == 0) { s += '-'; }
		const int nd = 1 + static_cast<int>(rng() % 17);
		const int dot = static_cast<int>(rng() % (nd + 2)) - 1;
		for (int k = 0; k < nd; ++k) {
			if (k == dot) { s += '.'; }
			s += static_cast<char>('0' + rng() % 10);
		}
		if (rng() % 3 == 0) {
			s += (rng() & 1) ? 'e' : 'E';
			const int sg = static_cast<int>(rng() % 3);
			if (sg == 1) { s += '-'; } else if (sg == 2) { s += '+'; }
			const int ed = 1 + static_cast<int>(rng() % 3);
			for (int k = 0; k < ed; ++k) { s += static_cast<char>('0' + rng() % (k == 0 ? 3 : 10)); }
		}
		v.push_back(s);
	}
	for (int i = 0; i < 4000; ++i) {
		// printed doubles and floats: 17 digits, 15 digits, 9 digits, fixed notation
		const std::uint64_t u = rng();
		double d;
		std::memcpy(&d, &u, 8);
		if (!std::isfinite(d)) { continue; }
		if (i % 4 == 0) { d = std::ldexp(static_cast<double>(rng() >> 11), -static_cast<int>(rng() % 70)); }
		if (i % 4 == 1) { d = static_cast<double>(static_cast<std::int64_t>(rng() % 2000000000) - 1000000000) / 1000.0; }
		std::snprintf(b, sizeof b, i % 2 ? "%.17g" : "%.15g", d);
		v.push_back(b);
		// 9 digits of a float (from float bits: a double outside the float range cannot be converted)
		const std::uint32_t fu = static_cast<std::uint32_t>(rng());
		float f;
		std::memcpy(&f, &fu, 4);
		if (std::isfinite(f)) {
			std::snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
			v.push_back(b);
		}
		if (i % 4 == 1) { std::snprintf(b, sizeof b, "%.3f", d); v.push_back(b); }
	}
	for (int i = 0; i < 500; ++i) {
		// more than 19 significant digits
		std::string s = (rng() & 1) ? "-" : "";
		const int n = 20 + static_cast<int>(rng() % 40);
		const int dot = static_cast<int>(rng() % n);
		for (int k = 0; k < n; ++k) {
			if (k == dot) { s += '.'; }
			s += static_cast<char>('0' + rng() % 10);
		}
		v.push_back(s);
	}
	return v;
}

template <typename R>
static std::vector<std::uint64_t> nearest_bits(const std::vector<std::string>& corpus) {
	std::vector<std::uint64_t> r;
	for (const auto& s : corpus) {
		const Result x = libc_result<R>(s, false);
		r.push_back(x.err == 0 ? x.bits : ~0ULL);
	}
	return r;
}

// In mode `m`, "0.1" is the double (float) below or above the real 0.1 and a negative text mirrors it.
struct Known {
	const char* text;
	double d[4];  // nearest, upward, downward, toward zero (hex floats, exact)
	float f[4];
};
static const Known known[] = {
	{"0.1", {0x1.999999999999ap-4, 0x1.999999999999ap-4, 0x1.9999999999999p-4, 0x1.9999999999999p-4}, {0x1.99999ap-4f, 0x1.99999ap-4f, 0x1.999998p-4f, 0x1.999998p-4f}},
	{"-0.1", {-0x1.999999999999ap-4, -0x1.9999999999999p-4, -0x1.999999999999ap-4, -0x1.9999999999999p-4}, {-0x1.99999ap-4f, -0x1.999998p-4f, -0x1.99999ap-4f, -0x1.999998p-4f}},
	{"-.10", {-0x1.999999999999ap-4, -0x1.9999999999999p-4, -0x1.999999999999ap-4, -0x1.9999999999999p-4}, {-0x1.99999ap-4f, -0x1.999998p-4f, -0x1.99999ap-4f, -0x1.999998p-4f}},
	// 0.3 is 0x1.3333333333333p-2 with the rest 0.4 of an ulp: nearest and downward agree, upward is one above.
	{"0.3", {0x1.3333333333333p-2, 0x1.3333333333334p-2, 0x1.3333333333333p-2, 0x1.3333333333333p-2}, {0x1.333334p-2f, 0x1.333334p-2f, 0x1.333332p-2f, 0x1.333332p-2f}},
	{"-0.3", {-0x1.3333333333333p-2, -0x1.3333333333333p-2, -0x1.3333333333334p-2, -0x1.3333333333333p-2}, {-0x1.333334p-2f, -0x1.333332p-2f, -0x1.333334p-2f, -0x1.333332p-2f}},
};

static void known_values() {
	int honored = 0;
	for (int mi = 0; mi < 4; ++mi) {
		ScopedMode sm(modes[mi].value);
		if (!sm.ok) { continue; }
		for (const Known& k : known) {
			const double want = k.d[mi];
			const float wantf = k.f[mi];
			const bool libc_ok = std::strtod(k.text, nullptr) == want && std::strtof(k.text, nullptr) == wantf;
			const double got = strict_stod(k.text);
			const float gotf = strict_stof(k.text);
			if (!libc_ok) {
				// A libc that does not follow the rounding mode: the API still has to equal it (checked by the corpus).
				continue;
			}
			++honored;
			if (!bits_equal(got, want)) { FAIL("[%s] stod(\"%s\") = %a, want %a", modes[mi].name, k.text, got, want); }
			if (!bits_equal(gotf, wantf)) { FAIL("[%s] stof(\"%s\") = %a, want %a", modes[mi].name, k.text, static_cast<double>(gotf), static_cast<double>(wantf)); }
		}
	}
	std::printf("  known values: %d of %zu checked against hand-derived bits\n", honored, sizeof(known) / sizeof(known[0]) * 4);
}

static void run_modes(const std::vector<std::string>& corpus, const std::vector<std::uint64_t>& nd, const std::vector<std::uint64_t>& nf) {
	for (const Mode& m : modes) {
		ScopedMode sm(m.value);
		if (!sm.ok) {
			std::printf("  %-12s not supported here, skipped\n", m.name);
			continue;
		}
		Counts cd, cf;
		for (std::size_t i = 0; i < corpus.size(); ++i) {
			check<double>(corpus[i], m.name, nd[i], cd);
			check<float>(corpus[i], m.name, nf[i], cf);
		}
		std::printf("  %-12s double: %ld checks, %ld by the fast path, %ld differ from nearest | float: %ld checks, %ld by the fast path, %ld differ\n", m.name, cd.checks,
			cd.accepted, cd.differs, cf.checks, cf.accepted, cf.differs);
		if (m.value == FE_TONEAREST) {
			// The fast path must be alive in the default mode: a path that declines everything is "correct".
			if (cd.accepted < cd.checks * 90 / 100 || cf.accepted < cf.checks * 70 / 100) { FAIL("fast path acceptance too low to number-check: double %ld/%ld float %ld/%ld", cd.accepted, cd.checks, cf.accepted, cf.checks); }
		} else if (cd.differs < 500 || cf.differs < 500) {
			// libc must really round differently here, or the check proves nothing.
			FAIL("[%s] libc gives the nearest answer for all but %ld doubles and %ld floats: the mode is not in effect", m.name, cd.differs, cf.differs);
		}
	}
}

// The mode is per thread: a thread in FE_DOWNWARD and one in FE_UPWARD, running at once with the default
// mode of the main thread, each get libc's answer in their own mode.
static void threads_and_modes(const std::vector<std::string>& corpus, const std::vector<std::uint64_t>& nd) {
	const int tm[3] = {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
	long thread_failures[4] = {0, 0, 0, 0};  // one slot per thread, the main thread's last
	std::vector<std::thread> ts;
	for (int t = 0; t < 3; ++t) {
		ts.emplace_back([&, t] {
			ScopedMode sm(tm[t]);
			if (!sm.ok) { return; }
			for (int round = 0; round < 3; ++round) {
				for (std::size_t i = static_cast<std::size_t>(t); i < corpus.size(); i += 3) {
					const Result want = libc_result<double>(corpus[i], false);
					const Result got = api_result<double>(corpus[i], false);
					if (!oracle::same(got, want, false)) { ++thread_failures[t]; }
				}
			}
		});
	}
	// Meanwhile the main thread stays in the default mode and must get the nearest answers.
	for (int round = 0; round < 3; ++round) {
		for (std::size_t i = 0; i < corpus.size(); ++i) {
			const Result got = api_result<double>(corpus[i], false);
			if (nd[i] != ~0ULL && got.bits != nd[i]) { ++thread_failures[3]; }
		}
	}
	for (auto& t : ts) { t.join(); }
	for (int t = 0; t < 4; ++t) {
		if (thread_failures[t] != 0) { FAIL("%s: %ld answers differ from libc", t < 3 ? "a thread in a directed rounding mode" : "the main thread in the default mode", thread_failures[t]); }
	}
	std::printf("  threads: three threads in directed modes and the main thread in the default mode agree with libc\n");
}

int main() {
	std::printf("strict_stox rounding modes\n");
	if (std::fegetround() != FE_TONEAREST) { std::fesetround(FE_TONEAREST); }
	const std::vector<std::string> corpus = make_corpus();
	const std::vector<std::uint64_t> nd = nearest_bits<double>(corpus);
	const std::vector<std::uint64_t> nf = nearest_bits<float>(corpus);
	run_modes(corpus, nd, nf);
	known_values();
	threads_and_modes(corpus, nd);
	if (std::fegetround() != FE_TONEAREST) { FAIL("a parse left the rounding mode changed"); }
	if (failures != 0) {
		std::fprintf(stderr, "%ld failures\n", failures);
		return 1;
	}
	std::printf("strict_stox rounding OK\n");
	return 0;
}
