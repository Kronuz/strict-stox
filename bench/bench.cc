// strict_stox benchmark: the fast path against the frozen old header
// (test/strict_stox_old.hh), optionally an earlier revision of strict_stox.hh
// (-DHAVE_PREV, with its text saved as strict_stox_prev.hh on the include path:
// `git show REV:strict_stox.hh > strict_stox_prev.hh`), std::from_chars, and, when
// available, fast_float (-DHAVE_FAST_FLOAT with its include path) and
// absl::SimpleAtoi/SimpleAtod (HAVE_ABSL). from_chars, fast_float and abseil
// accept a narrower syntax or ignore the locale and errno, so they are speed
// references, not equivalents.
// Build with -DSTRICT_STOX_BUILD_BENCH=ON. Best of 9 samples, ns per element over a
// pool of 1024 inputs; "new A/A" repeats the new measurement in the same process,
// and the percentage is its distance from the first one (the noise floor).
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "strict_stox.hh"
namespace old {
#include "strict_stox_old.hh"
}
#ifdef HAVE_PREV
namespace prev {
#include "strict_stox_prev.hh"
}
#endif
#ifdef HAVE_ABSL
#include "absl/strings/numbers.h"
#endif
#ifdef HAVE_FAST_FLOAT
#include "fast_float/fast_float.h"
#endif

template <typename F>
static double best_ns(F&& f, size_t per_call) {
	using clk = std::chrono::steady_clock;
	double best = 1e18;
	for (int s = 0; s < 9; ++s) {
		long iters = 0;
		auto t0 = clk::now();
		auto t1 = t0;
		do {
			f();
			++iters;
			t1 = clk::now();
		} while (t1 - t0 < std::chrono::milliseconds(10));
		double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters / per_call;
		if (ns < best) { best = ns; }
	}
	return best;
}

static volatile double sink;
using Pool = std::vector<std::string>;

template <typename T, typename P>
static double run(const Pool& v, P parse) {
	return best_ns([&] {
		T a = 0;
		for (auto& s : v) { a += parse(s); }
		sink = static_cast<double>(a);
	}, v.size());
}

static void header() {
	std::printf("%-34s %8s %8s %8s %8s %8s %8s %8s %6s\n", "workload (ns/element)", "old", "before", "new", "new A/A", "from_chars", "fastfloat", "absl", "fast%");
}

static void row(const char* name, double o, double pv, double n, double n2, double fc, double ff, double ab, double taken = -1) {
	auto cell = [](double v) { if (v > 0) { std::printf(" %8.1f", v); } else { std::printf(" %8s", "-"); } };
	std::printf("%-34s", name);
	cell(o);
	cell(pv);
	cell(n);
	std::printf(" %+7.1f%%", 100.0 * (n2 - n) / n);
	cell(fc);
	cell(ff);
	cell(ab);
	if (taken >= 0) { std::printf(" %5.0f%%", 100.0 * taken); }
	std::printf("\n");
}

// The share of the pool the fast path takes: a row whose inputs mostly go to strtod measures strtod.
template <typename R>
static double taken_share(const Pool& v) {
	std::size_t n = 0;
	for (auto& s : v) {
		R x;
		if (strict_stox_detail::fast_fp<R>(x, s)) { ++n; }
	}
	return static_cast<double>(n) / static_cast<double>(v.size());
}

static void int_rows(std::mt19937_64& rng) {
	struct K { const char* name; int kind; };
	for (K k : {K{"stoi 1-9 digits", 0}, K{"stoull 1-19 digits", 1}, K{"stoul base 16", 2}}) {
		Pool v;
		for (int i = 0; i < 1024; ++i) {
			switch (k.kind) {
				case 0: v.push_back(std::to_string(rng() % 1000000000)); break;
				case 1: v.push_back(std::to_string(rng() >> (rng() % 45))); break;
				default: { char b[24]; auto r = std::to_chars(b, b + 24, rng() >> (rng() % 45), 16); v.emplace_back(b, r.ptr); }
			}
		}
		int e;
		const int base = k.kind == 2 ? 16 : 10;
		double o, n, n2, fc, ab = 0;
		if (k.kind == 0) {
			o = run<long>(v, [&](const std::string& s) { return static_cast<long>(old::strict_stoi(&e, s)); });
			n = run<long>(v, [&](const std::string& s) { return static_cast<long>(strict_stoi(&e, s)); });
			n2 = run<long>(v, [&](const std::string& s) { return static_cast<long>(strict_stoi(&e, s)); });
			fc = run<long>(v, [&](const std::string& s) { int x = 0; std::from_chars(s.data(), s.data() + s.size(), x); return static_cast<long>(x); });
#ifdef HAVE_ABSL
			ab = run<long>(v, [&](const std::string& s) { int x = 0; absl::SimpleAtoi(s, &x); return static_cast<long>(x); });
#endif
		} else {
			o = run<unsigned long long>(v, [&](const std::string& s) { return old::strict_stoull(&e, s, nullptr, base); });
			n = run<unsigned long long>(v, [&](const std::string& s) { return strict_stoull(&e, s, nullptr, base); });
			n2 = run<unsigned long long>(v, [&](const std::string& s) { return strict_stoull(&e, s, nullptr, base); });
			fc = run<unsigned long long>(v, [&](const std::string& s) { unsigned long long x = 0; std::from_chars(s.data(), s.data() + s.size(), x, base); return x; });
		}
		row(k.name, o, 0, n, n2, fc, 0, ab);
	}
}

// Integer text the fast path declines or that fails: leading white space, a sign, a prefix, a base of 0, no digits at all
// and a value out of range. These rows pay for the attempt (the fast path) on top of strto*, so they are the ones a
// regression shows on first.
static void int_edge_rows(std::mt19937_64& rng) {
	struct K { const char* name; int kind; int base; };
	for (K k : {K{"stoll \" 42\" (white space)", 0, 10}, K{"stoll \"+42\"", 1, 10}, K{"stoll \"42\", base 0", 2, 0},
	            K{"stoll \"0x1f\", base 16", 3, 16}, K{"stoll \"abc\" (EINVAL)", 4, 10}, K{"stoll 20 digits (ERANGE)", 5, 10}}) {
		Pool v;
		for (int i = 0; i < 1024; ++i) {
			const std::string d = std::to_string(1 + rng() % 999999);
			switch (k.kind) {
				case 0: v.push_back(" " + d); break;
				case 1: v.push_back("+" + d); break;
				case 2: v.push_back(d); break;
				case 3: v.push_back("0x" + d); break;
				case 4: v.push_back("abc" + d); break;
				default: v.push_back(std::to_string(9 + rng() % 1) + "9999999999999999999"); break;
			}
		}
		int e;
		const int base = k.base;
		const double o = run<long long>(v, [&](const std::string& s) { return old::strict_stoll(&e, s, nullptr, base); });
		const double n = run<long long>(v, [&](const std::string& s) { return strict_stoll(&e, s, nullptr, base); });
		const double n2 = run<long long>(v, [&](const std::string& s) { return strict_stoll(&e, s, nullptr, base); });
		row(k.name, o, 0, n, n2, 0, 0, 0);
	}
}

static std::string to_s(double d, const char* f) {
	char b[64];
	std::snprintf(b, sizeof b, f, d);
	return b;
}

template <typename R>
static void fp_row(const char* name, const Pool& v) {
	int e;
	double o, pv = 0, n, n2, fc = 0, ff = 0, ab = 0;
	if constexpr (std::is_same_v<R, double>) {
		o = run<double>(v, [&](const std::string& s) { return old::strict_stod(&e, s); });
#ifdef HAVE_PREV
		pv = run<double>(v, [&](const std::string& s) { return prev::strict_stod(&e, s); });
#endif
		n = run<double>(v, [&](const std::string& s) { return strict_stod(&e, s); });
		n2 = run<double>(v, [&](const std::string& s) { return strict_stod(&e, s); });
	} else {
		o = run<double>(v, [&](const std::string& s) { return static_cast<double>(old::strict_stof(&e, s)); });
#ifdef HAVE_PREV
		pv = run<double>(v, [&](const std::string& s) { return static_cast<double>(prev::strict_stof(&e, s)); });
#endif
		n = run<double>(v, [&](const std::string& s) { return static_cast<double>(strict_stof(&e, s)); });
		n2 = run<double>(v, [&](const std::string& s) { return static_cast<double>(strict_stof(&e, s)); });
	}
#ifndef STRICT_STOX_BENCH_NO_FC
	fc = run<double>(v, [&](const std::string& s) { R x = 0; std::from_chars(s.data(), s.data() + s.size(), x); return static_cast<double>(x); });
#endif
#ifdef HAVE_FAST_FLOAT
	ff = run<double>(v, [&](const std::string& s) { R x = 0; fast_float::from_chars(s.data(), s.data() + s.size(), x); return static_cast<double>(x); });
#endif
#ifdef HAVE_ABSL
	if constexpr (std::is_same_v<R, double>) { ab = run<double>(v, [&](const std::string& s) { double x = 0; absl::SimpleAtod(s, &x); return x; }); }
	else { ab = run<double>(v, [&](const std::string& s) { float x = 0; absl::SimpleAtof(s, &x); return static_cast<double>(x); }); }
#endif
	row(name, o, pv, n, n2, fc, ff, ab, taken_share<R>(v));
}

int main(int argc, char** argv) {
	std::mt19937_64 rng(5);
	const bool ints = argc < 2 || std::strcmp(argv[1], "fp") != 0;
	header();
	if (ints) {
		int_rows(rng);
		int_edge_rows(rng);
	}
	Pool v;
	auto fill = [&](auto gen) { v.clear(); for (int i = 0; i < 1024; ++i) { v.push_back(gen()); } return v; };
	fp_row<double>("stod 1.234-like (short)", fill([&] { return std::to_string(rng() % 100000) + "." + std::to_string(rng() % 1000); }));
	fp_row<double>("stod 1-9 digit integers", fill([&] { return std::to_string(rng() % 1000000000); }));
	fp_row<double>("stod 17 digits (x/7)", fill([&] { return to_s(static_cast<double>(rng() % 100000000) / 7.0, "%.17g"); }));
	fp_row<double>("stod 17 digits, any exponent", fill([&] { return to_s(std::ldexp(static_cast<double>(rng() >> 11), static_cast<int>(rng() % 200) - 150), "%.17g"); }));
	fp_row<double>("stod shortest repr (JSON-like)", fill([&] {
		char b[40];
		auto r = std::to_chars(b, b + sizeof b, std::ldexp(static_cast<double>(rng() >> 11), -static_cast<int>(rng() % 70)));
		return std::string(b, r.ptr);
	}));
	fp_row<double>("stod 1.5e-7-like", fill([&] { return std::to_string(1 + rng() % 9) + "." + std::to_string(rng() % 100) + "e" + (rng() & 1 ? "-" : "+") + std::to_string(rng() % 30); }));
	// Long mantissas: n random digits with a decimal point among the first 250 (so the value is in range:
	// a result that overflows goes to strtod, which reports ERANGE) and, half the time, an exponent.
	auto digits = [&](int lo, int hi) {
		return [&, lo, hi] {
			std::string t;
			const int n = lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
			const int dot = 1 + static_cast<int>(rng() % static_cast<unsigned>(std::min(n, 250)));
			for (int i = 0; i < n; ++i) {
				if (i == dot) { t += '.'; }
				t += static_cast<char>('0' + (i == 0 ? 1 + rng() % 9 : rng() % 10));
			}
			if (rng() & 1) { t += "e" + std::to_string(static_cast<int>(rng() % 60) - 30); }
			return t;
		};
	};
	fp_row<double>("stod 25-40 digits", fill(digits(25, 40)));
	fp_row<double>("stod 100 digits", fill(digits(100, 100)));
	fp_row<double>("stod 800 digits", fill(digits(800, 800)));
	fp_row<float>("stof 1.234-like (short)", fill([&] { return std::to_string(rng() % 100000) + "." + std::to_string(rng() % 1000); }));
	fp_row<float>("stof 9 digits", fill([&] { return to_s(static_cast<float>(static_cast<double>(rng() % 100000000) / 7.0), "%.9g"); }));
	fp_row<float>("stof 25-40 digits", fill(digits(25, 40)));
	// Text that never takes the fast path (white space, inf, nan) or sits at the top of the range.
	fp_row<double>("stod \" 1.5\" (white space)", fill([&] { return " " + std::to_string(rng() % 100000) + "." + std::to_string(rng() % 1000); }));
	fp_row<double>("stod inf, nan", fill([&] { static const char* t[] = {"inf", "-inf", "infinity", "nan", "-nan"}; return std::string(t[rng() % 5]); }));
	fp_row<double>("stod near DBL_MAX", fill([&] { return "1.797693134862315" + std::to_string(rng() % 8) + "e308"; }));
	fp_row<float>("stof \" 1.5\" (white space)", fill([&] { return " " + std::to_string(rng() % 100000) + "." + std::to_string(rng() % 1000); }));
	fp_row<float>("stof inf, nan", fill([&] { static const char* t[] = {"inf", "-inf", "infinity", "nan", "-nan"}; return std::string(t[rng() % 5]); }));
	fp_row<float>("stof near FLT_MAX", fill([&] { return "3.40282" + std::to_string(rng() % 4) + "e38"; }));
	// Texts the 64-bit table cannot decide (exact halfway points) and the ones libc answers with ERANGE (subnormal, overflow,
	// underflow): the exact path, with no libc call.
	fp_row<double>("stod exact ties 2^52..2^53 + .5", fill([&] { return std::to_string((1ULL << 52) + rng() % (1ULL << 52)) + ".5"; }));
	fp_row<double>("stod odd integers 2^53..2^54 (ties)", fill([&] { return std::to_string((1ULL << 53) + 2 * (rng() % (1ULL << 52)) + 1); }));
	fp_row<double>("stod subnormal (ERANGE)", fill([&] { return std::to_string(1 + rng() % 9) + "." + std::to_string(rng() % 10000) + "e-" + std::to_string(310 + rng() % 13); }));
	fp_row<double>("stod overflow, underflow (ERANGE)", fill([&] { return std::to_string(1 + rng() % 9) + "." + std::to_string(rng() % 10000) + "e" + (rng() & 1 ? "" : "-") + std::to_string(325 + rng() % 40); }));
	fp_row<float>("stof odd integers 2^24..2^25 (ties)", fill([&] { return std::to_string((1ULL << 24) + 2 * (rng() % (1ULL << 23)) + 1); }));
	fp_row<float>("stof subnormal (ERANGE)", fill([&] { return std::to_string(1 + rng() % 9) + "." + std::to_string(rng() % 10000) + "e-" + std::to_string(40 + rng() % 5); }));
	fp_row<float>("stof overflow (ERANGE)", fill([&] { return std::to_string(1 + rng() % 9) + "." + std::to_string(rng() % 10000) + "e" + std::to_string(39 + rng() % 7); }));
	return 0;
}
