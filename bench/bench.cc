// strict_stox benchmark: the clean-input fast path against the frozen old
// header (test/strict_stox_old.hh) and std::from_chars (a speed reference: it
// accepts a narrower syntax). absl::SimpleAtoi/SimpleAtod and fast_float are
// compared when installed (HAVE_ABSL, and -DHAVE_FAST_FLOAT with its include
// path). Build with -DSTRICT_STOX_BUILD_BENCH=ON. Best of 7 samples, ns per
// element over a pool of 1024 inputs.
#include <charconv>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "strict_stox.hh"
namespace old {
#include "strict_stox_old.hh"
}
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
	for (int s = 0; s < 7; ++s) {
		long iters = 0;
		auto t0 = clk::now();
		auto t1 = t0;
		do {
			f();
			++iters;
			t1 = clk::now();
		} while (t1 - t0 < std::chrono::milliseconds(8));
		double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters / per_call;
		if (ns < best) { best = ns; }
	}
	return best;
}

static volatile double sink;

static std::vector<std::string> pool(int kind, std::mt19937_64& rng) {
	std::vector<std::string> v;
	for (int i = 0; i < 1024; ++i) {
		switch (kind) {
			case 0: v.push_back(std::to_string(rng() % 1000000000)); break;                       // 1-9 digits
			case 1: v.push_back(std::to_string(rng() >> (rng() % 45))); break;                      // 1-19 digits
			case 2: { char b[24]; auto r = std::to_chars(b, b + 24, rng() >> (rng() % 45), 16); v.emplace_back(b, r.ptr); break; }
			case 3: v.push_back(std::to_string(rng() % 100000) + "." + std::to_string(rng() % 1000)); break;  // 3.5-like
			default: { char b[40]; std::snprintf(b, sizeof b, "%.17g", static_cast<double>(rng() % 100000000) / 7.0); v.push_back(b); break; }  // 17 digits
		}
	}
	return v;
}

int main() {
	std::mt19937_64 rng(5);
	std::printf("%-28s %10s %10s %10s %10s\n", "workload (ns/element)", "old", "new", "from_chars", "other");
	const char* names[] = {"stoi 1-9 digits", "stoull 1-19 digits", "stoul base 16", "stod 1.234-like", "stod 17 digits (slow path)"};
	for (int kind = 0; kind < 5; ++kind) {
		auto v = pool(kind, rng);
		double o = 0, n = 0, fc = 0, other = 0;
		const size_t N = v.size();
		if (kind == 0) {
			int e;
			o = best_ns([&] { long a = 0; for (auto& s : v) { a += old::strict_stoi(&e, s); } sink = static_cast<double>(a); }, N);
			n = best_ns([&] { long a = 0; for (auto& s : v) { a += strict_stoi(&e, s); } sink = static_cast<double>(a); }, N);
			fc = best_ns([&] { long a = 0; for (auto& s : v) { int x = 0; std::from_chars(s.data(), s.data() + s.size(), x); a += x; } sink = static_cast<double>(a); }, N);
#ifdef HAVE_ABSL
			other = best_ns([&] { long a = 0; for (auto& s : v) { int x = 0; absl::SimpleAtoi(s, &x); a += x; } sink = static_cast<double>(a); }, N);
#endif
		} else if (kind == 1 || kind == 2) {
			int e;
			int base = kind == 2 ? 16 : 10;
			o = best_ns([&] { unsigned long long a = 0; for (auto& s : v) { a += old::strict_stoull(&e, s, nullptr, base); } sink = static_cast<double>(a); }, N);
			n = best_ns([&] { unsigned long long a = 0; for (auto& s : v) { a += strict_stoull(&e, s, nullptr, base); } sink = static_cast<double>(a); }, N);
			fc = best_ns([&] { unsigned long long a = 0; for (auto& s : v) { unsigned long long x = 0; std::from_chars(s.data(), s.data() + s.size(), x, base); a += x; } sink = static_cast<double>(a); }, N);
		} else {
			int e;
			o = best_ns([&] { double a = 0; for (auto& s : v) { a += old::strict_stod(&e, s); } sink = a; }, N);
			n = best_ns([&] { double a = 0; for (auto& s : v) { a += strict_stod(&e, s); } sink = a; }, N);
			fc = best_ns([&] { double a = 0; for (auto& s : v) { double x = 0; std::from_chars(s.data(), s.data() + s.size(), x); a += x; } sink = a; }, N);
#ifdef HAVE_FAST_FLOAT
			other = best_ns([&] { double a = 0; for (auto& s : v) { double x = 0; fast_float::from_chars(s.data(), s.data() + s.size(), x); a += x; } sink = a; }, N);
#endif
		}
		std::printf("%-28s %10.1f %10.1f %10.1f %10.1f\n", names[kind], o, n, fc, other);
	}
	return 0;
}
