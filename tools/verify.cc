// Independent verification of strict-stox's floating point conversion. It shares no code with
// test/fp_test.cc: its own text generators, its own big integer for exact halfway points and its own oracles.
// The sweeps are long (billions of texts) and are not part of ctest; docs/perf/verification.md records what
// has been run.
//
// What is checked, per text and per type (double and float), in each rounding mode that is asked for:
//   FAST   if strict_stox_detail::fast_fp accepts the text, its bits are exactly libc's strtod/strtof, libc sets
//          no errno and consumed the whole text.
//   API    strict_stod / strict_stof (nothrow, with and without idx) equal libc under the strict rules: value
//          bits (0 on error), errno, idx; the caller's errno survives. Every 16th text also the throwing form
//          (invalid_argument for EINVAL, out_of_range for ERANGE, nothing otherwise).
//   ORACLE in the default mode, where libc, std::from_chars and fast_float all report a value for the whole
//          text, they must agree with each other and with the fast path (a disagreement between oracles
//          is reported separately: it would be a libc or library bug, not ours).
//
// Text sources: random doubles and floats printed in many formats, random digit strings (up to 40 digits,
// all exponents), long mantissas (20..800 digits, tails of zeros and a final 1), the exact decimal expansion of
// the midpoint between neighbouring doubles / floats and of the binary range limits (built with an own big
// integer), every finite float's shortest, 9 and 17 digit text, every short string over a syntax alphabet.
//
// usage: verify hard|syntax|rand|modes|f32|f32mid|dmid [options]   or   verify text STRING...   (see main)
// build: c++ -std=c++20 -O2 -Wno-ignored-attributes -I<strict-stox> [-I<fast_float>/include -DHAVE_FAST_FLOAT]
//            [-DHAVE_FROM_CHARS] [-DHAVE_TO_CHARS] [-DSTRICT_STOX_COUNT_LIBC=1] [-DHAVE_PREV] tools/verify.cc -o verify -pthread
// -DSTRICT_STOX_COUNT_LIBC=1 counts the calls that reach libc (the REACH category). -DHAVE_PREV needs the revision to
// compare with saved as strict_stox_prev.hh on the include path, and --ab or --ab-only to use it.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "strict_stox.hh"
#ifdef HAVE_FAST_FLOAT
#include "fast_float/fast_float.h"
#endif

#ifdef HAVE_PREV
// The previous revision of the header (git show REV:strict_stox.hh > strict_stox_prev.hh): --ab compares it with the current one.
namespace prev {
#include "strict_stox_prev.hh"
}
#endif

namespace D = strict_stox_detail;

// ------------------------------------------------------------------------------------------------ abort trap
// libc can abort inside strtod (Apple's libc asserts in strtofp.c on some texts): say which text it was, in a form that can be pasted back.
static thread_local const std::string* t_cur = nullptr;
static thread_local sigjmp_buf t_jb;
static thread_local bool t_armed = false;
static std::atomic<std::uint64_t> g_libc_aborts{0};
static void on_abort(int) {
	if (t_armed) { siglongjmp(t_jb, 1); }
	char b[128];
	const std::string* s = t_cur;
	int n = std::snprintf(b, sizeof b, "\nABORT (SIGABRT) while checking a text of %zu bytes; first 600 bytes follow, then a newline\n", s ? s->size() : static_cast<std::size_t>(0));
	if (n > 0) { (void)!write(2, b, static_cast<std::size_t>(n)); }
	if (s) {
		(void)!write(2, s->data(), s->size() < 600 ? s->size() : 600);
		(void)!write(2, "\n", 1);
	}
	_exit(134);
}
// Runs f(); false when libc aborted inside it (Apple's strtod asserts on some texts: it is a libc bug, reported, not a mismatch).
template <typename F>
static bool guarded(F&& f) {
	if (sigsetjmp(t_jb, 1) != 0) {
		t_armed = false;
		g_libc_aborts.fetch_add(1);
		return false;
	}
	t_armed = true;
	f();
	t_armed = false;
	return true;
}

// ------------------------------------------------------------------------------------------------ reporting
enum Cat { C_FAST, C_API, C_THROW, C_ORACLE, C_FROM_CHARS, C_FAST_FLOAT, C_ERRNO, C_AB, C_LIBC_ABORT, C_SLOW, C_REACH, C_N };
static const char* const cat_name[C_N] = {"FAST", "API", "THROW", "ORACLE-libc-vs-others", "FROM_CHARS-vs-fast", "FAST_FLOAT-vs-fast", "ERRNO", "AB-new-vs-prev", "libc-aborted (a libc bug: not counted)", "SLOW (exact path)", "REACH (libc reached by decimal text)"};
static std::atomic<std::uint64_t> g_fail[C_N];
static std::mutex g_out;
static void report(Cat c, const char* fmt, ...) {
	if (g_fail[c].fetch_add(1) >= 20) { return; }
	std::lock_guard<std::mutex> lk(g_out);
	std::fprintf(stderr, "[%s] ", cat_name[c]);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fprintf(stderr, "\n");
}
static std::string clip(const std::string& s) {
	if (s.size() <= 70) { return s; }
	return s.substr(0, 40) + "..." + s.substr(s.size() - 20) + " (len " + std::to_string(s.size()) + ")";
}

struct Mode {
	int fe;
	const char* name;
};
static const Mode kModes[4] = {{FE_TONEAREST, "nearest"}, {FE_UPWARD, "upward"}, {FE_DOWNWARD, "downward"}, {FE_TOWARDZERO, "toward-zero"}};

struct Stats {
	// [mode][type 0=double 1=float]
	std::uint64_t checks[4][2] = {}, taken[4][2] = {}, erange[4][2] = {}, einval[4][2] = {}, differs_from_nearest[4][2] = {};
	std::uint64_t cross[2] = {};  // texts compared against from_chars / fast_float
	std::uint64_t slow_ok[2] = {}, slow_range[2] = {}, slow_decl[2] = {}, reach[2] = {};  // the exact path: converted, ERANGE, declined (plain decimal text); API calls that reached libc (plain decimal text)
	std::uint64_t ab[2] = {}, ab_taken[2] = {};  // texts compared with the previous header (double, float), and taken by both
	std::uint64_t texts = 0;
	void add(const Stats& o) {
		for (int m = 0; m < 4; ++m) {
			for (int t = 0; t < 2; ++t) {
				checks[m][t] += o.checks[m][t];
				taken[m][t] += o.taken[m][t];
				erange[m][t] += o.erange[m][t];
				einval[m][t] += o.einval[m][t];
				differs_from_nearest[m][t] += o.differs_from_nearest[m][t];
			}
		}
		cross[0] += o.cross[0];
		cross[1] += o.cross[1];
		for (int t = 0; t < 2; ++t) {
			slow_ok[t] += o.slow_ok[t];
			slow_range[t] += o.slow_range[t];
			slow_decl[t] += o.slow_decl[t];
			reach[t] += o.reach[t];
		}
		for (int t = 0; t < 2; ++t) { ab[t] += o.ab[t]; ab_taken[t] += o.ab_taken[t]; }
		texts += o.texts;
	}
};

// ------------------------------------------------------------------------------------------------ one text
template <typename R>
static std::uint64_t bits_of(R v) {
	std::uint64_t b = 0;
	std::memcpy(&b, &v, sizeof(R));
	return b;
}

template <typename R>
struct Libc {
	std::uint64_t bits;
	int err;
	std::ptrdiff_t end;  // offset of the end pointer
};
template <typename R>
static Libc<R> libc_parse(const std::string& s) {
	char* end = nullptr;
	errno = 0;
	R v;
	if constexpr (std::is_same_v<R, double>) { v = std::strtod(s.c_str(), &end); } else { v = std::strtof(s.c_str(), &end); }
	return {bits_of(v), errno, end - s.c_str()};
}

struct Opt {
	int threads = 4;
	std::uint64_t seed = 1;
	std::uint64_t n = 1000000;
	int stride = 1;
	int api_every = 1;    // the API / throw checks on every k-th text (the fast path and the oracles on all)
	bool cross = true;    // from_chars / fast_float (default mode only)
	int ab = 0;  // 1: also compare fast_fp with the previous header's (needs -DHAVE_PREV); 2: only that
	int fmt_lo = 0, fmt_hi = 1000;  // formats of `rand` / `modes` to run (clamped to the number of formats)
	std::size_t fc_max = 1200;  // longest text given to std::from_chars (libstdc++ 11 asserts on long input)
};

template <typename R>
static bool is_finite_bits(std::uint64_t b) {
	if constexpr (std::is_same_v<R, double>) { return ((b >> 52) & 0x7ff) != 0x7ff; } else { return ((b >> 23) & 0xff) != 0xff; }
}

// A text of the plain decimal syntax: [-]digits[.digits][(e|E)[+-]digits] with at least one digit.
static bool plain_decimal(const std::string& s) {
	std::size_t i = 0, d = 0;
	if (i < s.size() && s[i] == '-') { ++i; }
	while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i, ++d; }
	if (i < s.size() && s[i] == '.') {
		++i;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i, ++d; }
	}
	if (d == 0) { return false; }
	if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
		++i;
		if (i < s.size() && (s[i] == '+' || s[i] == '-')) { ++i; }
		std::size_t e = 0;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i, ++e; }
		if (e == 0) { return false; }
	}
	return i == s.size();
}

// A non-zero subnormal or the smallest normal value: the results for which libc is reached by design (its errno differs between libcs).
template <typename R>
static bool is_small_nonzero(std::uint64_t b) {
	constexpr int mant = std::is_same_v<R, double> ? 52 : 23;
	const std::uint64_t mag = b & ((1ULL << (sizeof(R) * 8 - 1)) - 1);
	return mag != 0 && mag <= (1ULL << mant);
}

// `mi` is the index of the current rounding mode (the mode is already set), `nearest_bits` is libc's answer in
// the default mode when known (0 / false otherwise), for counting the texts the mode changes.
template <typename R>
static void check_type(const std::string& s, int mi, bool do_api, bool do_throw, bool do_cross, Stats& st, const Opt& o) {
	constexpr int T = std::is_same_v<R, double> ? 0 : 1;
	const char* tn = T == 0 ? "double" : "float";
	++st.checks[mi][T];
	Libc<R> L{0, 0, 0};
	const bool lib_ok = guarded([&] { L = libc_parse<R>(s); });
	if (!lib_ok) { report(C_LIBC_ABORT, "%s mode=%s \"%s\": libc aborted", tn, kModes[mi].name, clip(s).c_str()); }
	if (L.err == ERANGE) { ++st.erange[mi][T]; }
	// ---- the fast path alone
	R g = 0;
	const bool acc = D::fast_fp<R>(g, s);
	if (acc) {
		++st.taken[mi][T];
		if (lib_ok && (L.err != 0 || L.end != static_cast<std::ptrdiff_t>(s.size()) || bits_of(g) != L.bits)) {
			report(C_FAST, "%s mode=%s \"%s\": fast %016llx; libc %016llx errno %d end %td/%zu", tn, kModes[mi].name, clip(s).c_str(), (unsigned long long)bits_of(g),
				(unsigned long long)L.bits, L.err, L.end, s.size());
		}
	}
	// ---- the exact path alone (fp_slow): with and without the table it must agree, in a directed rounding mode it must stand aside,
	// and in the default mode its answer is libc's (value, or ERANGE); it declines a plain decimal text only for a subnormal or smallest normal result
	R sv = 0;
	int s1 = 0;
	{
		R sd = 0;
		s1 = D::fp_slow<R, false>(sv, s);
		const int s2 = D::fp_slow<R, true>(sd, s);
		if (s1 != s2 || (s1 == 1 && bits_of(sv) != bits_of(sd))) {
			report(C_SLOW, "%s mode=%s \"%s\": table status %d bits %016llx; division status %d bits %016llx", tn, kModes[mi].name, clip(s).c_str(), s1, (unsigned long long)bits_of(sv), s2, (unsigned long long)bits_of(sd));
		}
		if (mi != 0) {
			if (s1 != 0) { report(C_SLOW, "%s mode=%s \"%s\": the exact path converted (status %d) in a directed rounding mode", tn, kModes[mi].name, clip(s).c_str(), s1); }
		} else {
			if (s1 == 1) { ++st.slow_ok[T]; } else if (s1 == 2) { ++st.slow_range[T]; } else if (plain_decimal(s)) { ++st.slow_decl[T]; }
			if (lib_ok) {
				const bool whole = L.end == static_cast<std::ptrdiff_t>(s.size());
				if (s1 == 1 && (L.err != 0 || !whole || bits_of(sv) != L.bits)) {
					report(C_SLOW, "%s \"%s\": exact path %016llx; libc %016llx errno %d end %td/%zu", tn, clip(s).c_str(), (unsigned long long)bits_of(sv), (unsigned long long)L.bits, L.err, L.end, s.size());
				}
				if (s1 == 2 && (L.err != ERANGE || !whole)) { report(C_SLOW, "%s \"%s\": exact path says ERANGE; libc errno %d bits %016llx end %td/%zu", tn, clip(s).c_str(), L.err, (unsigned long long)L.bits, L.end, s.size()); }
				if (s1 == 0 && plain_decimal(s) && !is_small_nonzero<R>(L.bits)) {
					report(C_SLOW, "%s \"%s\": exact path declined a decimal text; libc %016llx errno %d", tn, clip(s).c_str(), (unsigned long long)L.bits, L.err);
				}
			}
		}
	}
#ifdef HAVE_PREV
	// ---- the previous header's fast path against libc as well (--ab): both revisions are verified in the same run
	if (o.ab == 1) {
		R pv = 0;
		if (prev::strict_stox_detail::fast_fp<R>(pv, s) && lib_ok && (L.err != 0 || L.end != static_cast<std::ptrdiff_t>(s.size()) || bits_of(pv) != L.bits)) {
			report(C_FAST, "PREV %s mode=%s \"%s\": prev fast %016llx; libc %016llx errno %d end %td/%zu", tn, kModes[mi].name, clip(s).c_str(), (unsigned long long)bits_of(pv),
				(unsigned long long)L.bits, L.err, L.end, s.size());
		}
	}
#endif
	// ---- the cross oracles (default mode only)
	if (do_cross && mi == 0 && s.size() <= 1200) {
		const char* const f = s.data();
		const char* const l = f + s.size();
		const bool libc_ok = lib_ok && L.end == static_cast<std::ptrdiff_t>(s.size()) && is_finite_bits<R>(L.bits) && (L.err == 0 || L.err == ERANGE);
		// libc reports ERANGE on underflow too: only compare when the others return a value as well.
#ifdef HAVE_FROM_CHARS
		if (s.size() <= o.fc_max) {
			R v = 0;
			const auto r = std::from_chars(f, l, v);
			if (r.ec == std::errc() && r.ptr == l) {
				++st.cross[0];
				if (libc_ok && bits_of(v) != L.bits) { report(C_ORACLE, "%s \"%s\": libc %016llx errno %d, from_chars %016llx", tn, clip(s).c_str(), (unsigned long long)L.bits, L.err, (unsigned long long)bits_of(v)); }
				if (acc && bits_of(v) != bits_of(g)) { report(C_FROM_CHARS, "%s \"%s\": fast %016llx, from_chars %016llx", tn, clip(s).c_str(), (unsigned long long)bits_of(g), (unsigned long long)bits_of(v)); }
				if (mi == 0 && s1 == 1 && bits_of(v) != bits_of(sv)) { report(C_FROM_CHARS, "%s \"%s\": exact path %016llx, from_chars %016llx", tn, clip(s).c_str(), (unsigned long long)bits_of(sv), (unsigned long long)bits_of(v)); }
			} else if (acc) {
				report(C_FROM_CHARS, "%s \"%s\": fast took it, from_chars ec %d consumed %td/%zu", tn, clip(s).c_str(), static_cast<int>(r.ec), r.ptr - f, s.size());
			}
		}
#endif
#ifdef HAVE_FAST_FLOAT
		{
			R v = 0;
			const auto r = fast_float::from_chars(f, l, v);
			if (r.ec == std::errc() && r.ptr == l) {
				++st.cross[1];
				if (libc_ok && bits_of(v) != L.bits) { report(C_ORACLE, "%s \"%s\": libc %016llx errno %d, fast_float %016llx", tn, clip(s).c_str(), (unsigned long long)L.bits, L.err, (unsigned long long)bits_of(v)); }
				if (acc && bits_of(v) != bits_of(g)) { report(C_FAST_FLOAT, "%s \"%s\": fast %016llx, fast_float %016llx", tn, clip(s).c_str(), (unsigned long long)bits_of(g), (unsigned long long)bits_of(v)); }
				if (mi == 0 && s1 == 1 && bits_of(v) != bits_of(sv)) { report(C_FAST_FLOAT, "%s \"%s\": exact path %016llx, fast_float %016llx", tn, clip(s).c_str(), (unsigned long long)bits_of(sv), (unsigned long long)bits_of(v)); }
			} else if (acc) {
				report(C_FAST_FLOAT, "%s \"%s\": fast took it, fast_float ec %d consumed %td/%zu", tn, clip(s).c_str(), static_cast<int>(r.ec), r.ptr - f, s.size());
			}
		}
#endif
	}
	// ---- the public API
	if (!do_api) { return; }
	// The reference answer: libc's; when libc aborted on the text (Apple, a libc bug) and the API still must not reach libc for it, fast_float's, for a normal finite value.
	Libc<R> A = L;
	bool have_ref = lib_ok;
	if (!lib_ok) {
#ifdef HAVE_FAST_FLOAT
		if (mi == 0) {
			R fv = 0;
			const auto r = fast_float::from_chars(s.data(), s.data() + s.size(), fv);
			const std::uint64_t fb = bits_of(fv);
			if (r.ec == std::errc() && r.ptr == s.data() + s.size() && is_finite_bits<R>(fb) && !is_small_nonzero<R>(fb) && fb != 0) {
				A = Libc<R>{fb, 0, static_cast<std::ptrdiff_t>(s.size())};
				have_ref = true;
			}
		}
#endif
		if (!have_ref) { return; }
	}
	for (int useidx = 0; useidx < 2; ++useidx) {
		int err = A.err;
		if (err == 0 && A.end == 0) { err = EINVAL; }
		std::size_t idx = 0;
		if (err == 0) {
			if (useidx) { idx = static_cast<std::size_t>(A.end); }
			else if (A.end != static_cast<std::ptrdiff_t>(s.size())) { err = EINVAL; }
		}
		const std::uint64_t want = err ? 0 : A.bits;
		if (!useidx && err == EINVAL) { ++st.einval[mi][T]; }
		std::size_t gidx = 0;
		int e = -1;
		errno = 4242;
		R v = 0;
#ifdef STRICT_STOX_COUNT_LIBC
		const std::size_t libc_before = D::libc_calls;
#endif
		const bool api_ok = guarded([&] {
			if constexpr (T == 0) { v = strict_stod(&e, s, useidx ? &gidx : nullptr); } else { v = strict_stof(&e, s, useidx ? &gidx : nullptr); }
		});
		if (!api_ok) {
			// the API reached libc and libc aborted: by design only for an exact subnormal or the smallest normal value
			if (have_ref && lib_ok == false) { report(C_REACH, "%s \"%s\": the API reached libc, which aborted, for a text with a normal result", tn, clip(s).c_str()); }
			continue;
		}
#ifdef STRICT_STOX_COUNT_LIBC
		if (!useidx && mi == 0 && plain_decimal(s)) {
			if (D::libc_calls != libc_before) {
				++st.reach[T];
				if (err != EINVAL && !(lib_ok && is_small_nonzero<R>(A.bits)) ) { report(C_REACH, "%s \"%s\": the API reached libc; libc %016llx errno %d", tn, clip(s).c_str(), (unsigned long long)A.bits, A.err); }
			}
		}
#endif
		const bool kept = errno == 4242;
		if (!kept) { report(C_ERRNO, "%s \"%s\": errno of the caller changed", tn, clip(s).c_str()); }
		if (bits_of(v) != want || e != err || (err == 0 && useidx && gidx != idx)) {
			report(C_API, "%s mode=%s \"%s\"%s: got %016llx errno %d idx %zu; libc %016llx errno %d idx %zu", tn, kModes[mi].name, clip(s).c_str(), useidx ? " idx" : "", (unsigned long long)bits_of(v), e,
				gidx, (unsigned long long)want, err, idx);
		}
	}
	if (do_throw) {
		int err = A.err;
		if (err == 0 && A.end == 0) { err = EINVAL; }
		if (err == 0 && A.end != static_cast<std::ptrdiff_t>(s.size())) { err = EINVAL; }
		int kind = 0;
		std::uint64_t got = 0;
		const bool thr_ok = guarded([&] {
			try {
				if constexpr (T == 0) { got = bits_of(strict_stod(s)); } else { got = bits_of(strict_stof(s)); }
			} catch (const std::invalid_argument&) { kind = EINVAL; }
			catch (const std::out_of_range&) { kind = ERANGE; }
		});
		if (!thr_ok) { return; }
		if (kind != err || (err == 0 && got != A.bits)) { report(C_THROW, "%s \"%s\": kind %d want %d", tn, clip(s).c_str(), kind, err); }
	}
}

#ifdef HAVE_PREV
template <typename R>
static void ab_compare(const std::string& s, Stats& st) {
	constexpr int T = std::is_same_v<R, double> ? 0 : 1;
	R a = 0, b = 0;
	const bool ra = D::fast_fp<R>(a, s);
	const bool rb = prev::strict_stox_detail::fast_fp<R>(b, s);
	++st.ab[T];
	if (ra) { ++st.ab_taken[T]; }
	if (ra != rb || (ra && bits_of(a) != bits_of(b))) {
		report(C_AB, "%s \"%s\": new %s %016llx, prev %s %016llx", T == 0 ? "double" : "float", clip(s).c_str(), ra ? "took" : "declined", (unsigned long long)(ra ? bits_of(a) : 0),
			rb ? "took" : "declined", (unsigned long long)(rb ? bits_of(b) : 0));
	}
}
#endif

// All of a text's checks in the modes selected by `mode_mask` (bit i = kModes[i]). The caller is in the default mode.
static void check_text(const std::string& s, unsigned mode_mask, bool do_api, bool do_throw, Stats& st, const Opt& o) {
	++st.texts;
	t_cur = &s;
#ifdef HAVE_PREV
	if (o.ab != 0) {
		ab_compare<double>(s, st);
		ab_compare<float>(s, st);
		if (o.ab == 2) { return; }
	}
#endif
	for (int mi = 0; mi < 4; ++mi) {
		if (!(mode_mask & (1u << mi))) { continue; }
		if (mi != 0 && std::fesetround(kModes[mi].fe) != 0) { continue; }
		check_type<double>(s, mi, do_api, do_throw, o.cross, st, o);
		check_type<float>(s, mi, do_api, do_throw, o.cross, st, o);
		if (mi != 0) { std::fesetround(FE_TONEAREST); }
	}
}

// ------------------------------------------------------------------------------------------------ big integers and exact decimals
struct Big {
	std::vector<std::uint32_t> d;  // base 1e9, little endian
	explicit Big(std::uint64_t v = 0) {
		while (v != 0) {
			d.push_back(static_cast<std::uint32_t>(v % 1000000000u));
			v /= 1000000000u;
		}
	}
	void mul(std::uint32_t m) {  // m < 2^32
		std::uint64_t c = 0;
		for (auto& x : d) {
			const std::uint64_t t = static_cast<std::uint64_t>(x) * m + c;
			x = static_cast<std::uint32_t>(t % 1000000000u);
			c = t / 1000000000u;
		}
		while (c != 0) {
			d.push_back(static_cast<std::uint32_t>(c % 1000000000u));
			c /= 1000000000u;
		}
	}
	void mul_pow2(unsigned k) {
		while (k >= 29) { mul(1u << 29); k -= 29; }
		if (k) { mul(1u << k); }
	}
	void mul_pow5(unsigned k) {
		while (k >= 13) { mul(1220703125u); k -= 13; }
		if (k) { std::uint32_t p = 1; while (k--) { p *= 5; } mul(p); }
	}
	std::string str() const {
		if (d.empty()) { return "0"; }
		std::string s = std::to_string(d.back());
		char b[16];
		for (std::size_t i = d.size() - 1; i-- > 0;) {
			std::snprintf(b, sizeof b, "%09u", d[i]);
			s += b;
		}
		return s;
	}
};

// m * 2^e2 = digits * 10^x10 exactly.
static void exact_decimal(std::uint64_t m, int e2, std::string& digits, int& x10) {
	Big b(m);
	if (e2 >= 0) { b.mul_pow2(static_cast<unsigned>(e2)); x10 = 0; }
	else { b.mul_pow5(static_cast<unsigned>(-e2)); x10 = e2; }
	digits = b.str();
}

static std::string decrement(std::string d) {  // d > 0, decimal digits
	std::size_t i = d.size();
	while (i-- > 0) {
		if (d[i] != '0') { --d[i]; break; }
		d[i] = '9';
	}
	const std::size_t nz = d.find_first_not_of('0');
	return nz == std::string::npos ? "0" : d.substr(nz);
}

// digits * 10^x10 as text in one of several styles.
static std::string render(const std::string& digits, long x10, int style, bool neg) {
	std::string s = neg ? "-" : "";
	const long n = static_cast<long>(digits.size());
	switch (style % 5) {
		case 0:  // d.ddd e X
			s += digits[0];
			if (n > 1) { s += '.'; s.append(digits, 1, std::string::npos); }
			s += "e" + std::to_string(x10 + n - 1);
			break;
		case 1:  // positional
			if (x10 >= 0) { s += digits + std::string(static_cast<std::size_t>(x10), '0'); }
			else if (n > -x10) { s.append(digits, 0, static_cast<std::size_t>(n + x10)); s += '.'; s.append(digits, static_cast<std::size_t>(n + x10), std::string::npos); }
			else { s += "0." + std::string(static_cast<std::size_t>(-x10 - n), '0') + digits; }
			break;
		case 2:  // integer mantissa and exponent
			s += digits + "e" + std::to_string(x10);
			break;
		case 3:  // .ddd e X
			s += "." + digits + "E" + (x10 + n >= 0 ? "+" : "") + std::to_string(x10 + n);
			break;
		default:  // leading zeros and a signed exponent
			s += "000" + digits + "e" + (x10 < 0 ? "-" : "+") + std::to_string(std::labs(x10));
			break;
	}
	return s;
}

// The text variants around the exact decimal value digits * 10^x10 (a binary midpoint or range limit): the value itself
// (a tie), just below and just above it by one unit in the last digit, just above it by a 1 after 30 zeros, the same
// value with 40 trailing zeros (a tie in a long text), and the digits cut at 17..21 places with and without one more.
static void variants(const std::string& digits, int x10, std::mt19937_64& rng, std::vector<std::string>& out) {
	const bool neg = (rng() & 3) == 0;
	auto push = [&](const std::string& d, long x) { out.push_back(render(d, x, static_cast<int>(rng() % 5), neg)); };
	push(digits, x10);
	push(decrement(digits), x10);
	push(digits + "1", x10 - 1);
	push(digits + std::string(30, '0') + "1", x10 - 31);
	push(digits + std::string(40, '0'), x10 - 40);
	push(decrement(digits) + std::string(25, '9'), x10 - 25);  // just below the tie, by a hair
	for (int cut = 17; cut <= 21; ++cut) {
		if (static_cast<std::size_t>(cut) < digits.size()) {
			const std::string head = digits.substr(0, static_cast<std::size_t>(cut));
			const long x = x10 + static_cast<long>(digits.size()) - cut;
			push(head, x);
			push(head + "1", x - 1);
			std::string inc = head;
			for (std::size_t i = inc.size(); i-- > 0;) { if (inc[i] == '9') { inc[i] = '0'; if (i == 0) { inc = "1" + inc; } } else { ++inc[i]; break; } }
			push(inc, x);
		}
	}
}

// ------------------------------------------------------------------------------------------------ generators
struct Gen {
	std::mt19937_64 rng;
	explicit Gen(std::uint64_t seed) : rng(seed) { }

	double rand_double() {
		const unsigned k = static_cast<unsigned>(rng() % 20);
		std::uint64_t u = rng();
		if (k < 9) { /* uniform bits */ }
		else if (k < 13) { u = (u & 0x800fffffffffffffULL) | (static_cast<std::uint64_t>(1023 - 40 + rng() % 80) << 52); }  // around 1
		else if (k < 15) { u &= 0x800fffffffffffffULL; }                                                           // subnormal (or zero)
		else if (k < 17) { u = (u & 0x800fffffffffffffULL) | (static_cast<std::uint64_t>((rng() & 1) ? 2046 - rng() % 3 : 1 + rng() % 3) << 52); }
		else if (k < 18) { return std::ldexp(static_cast<double>(rng() >> 11), -static_cast<int>(rng() % 70)); }   // short mantissa
		else if (k < 19) { return static_cast<double>(static_cast<std::int64_t>(rng() % 20000000000ULL) - 10000000000LL) / std::pow(10.0, static_cast<double>(rng() % 12)); }
		else { return std::nextafter(std::pow(10.0, static_cast<double>(static_cast<int>(rng() % 600) - 300)), (rng() & 1) ? 1e308 : 0.0); }
		double d;
		std::memcpy(&d, &u, 8);
		return std::isfinite(d) ? d : 1.5;
	}
	float rand_float() {
		const unsigned k = static_cast<unsigned>(rng() % 10);
		std::uint32_t u = static_cast<std::uint32_t>(rng());
		if (k < 5) { }
		else if (k < 7) { u = (u & 0x807fffffU) | (static_cast<std::uint32_t>(127 - 20 + rng() % 40) << 23); }
		else if (k < 8) { u &= 0x807fffffU; }
		else if (k < 9) { u = (u & 0x807fffffU) | (static_cast<std::uint32_t>((rng() & 1) ? 254 - rng() % 3 : 1 + rng() % 3) << 23); }
		else { return static_cast<float>(static_cast<std::int64_t>(rng() % 2000000) - 1000000) / std::pow(10.0f, static_cast<float>(rng() % 6)); }
		float f;
		std::memcpy(&f, &u, 4);
		return std::isfinite(f) ? f : 1.5f;
	}

	static std::string shortest(double d) {
		char b[64];
#ifdef HAVE_TO_CHARS
		auto r = std::to_chars(b, b + sizeof b, d);
		return std::string(b, r.ptr);
#else
		for (int p = 1; p <= 17; ++p) { std::snprintf(b, sizeof b, "%.*g", p, d); if (std::strtod(b, nullptr) == d) { break; } }
		return b;
#endif
	}
	static std::string shortest(float f) {
		char b[64];
#ifdef HAVE_TO_CHARS
		auto r = std::to_chars(b, b + sizeof b, f);
		return std::string(b, r.ptr);
#else
		for (int p = 1; p <= 9; ++p) { std::snprintf(b, sizeof b, "%.*g", p, static_cast<double>(f)); if (std::strtof(b, nullptr) == f) { break; } }
		return b;
#endif
	}

	std::string digit_string(int max_digits, int max_exp) {
		std::string s;
		if (rng() % 4 == 0) { s += '-'; }
		int nd = 1 + static_cast<int>(rng() % static_cast<unsigned>(max_digits));
		std::string dg;
		for (int i = 0; i < nd; ++i) { dg += static_cast<char>('0' + rng() % 10); }
		if (rng() % 10 == 0) { dg = std::string(1 + rng() % 5, '0') + dg; }
		if (rng() % 10 == 0) { dg += std::string(1 + rng() % 30, '0'); }
		const int dot = (rng() % 5 == 0) ? -1 : static_cast<int>(rng() % (dg.size() + 1));
		for (int i = 0; i <= static_cast<int>(dg.size()); ++i) {
			if (i == dot) { s += '.'; }
			if (i < static_cast<int>(dg.size())) { s += dg[static_cast<std::size_t>(i)]; }
		}
		if (rng() % 10 < 6) {
			s += (rng() & 1) ? 'e' : 'E';
			const unsigned sg = rng() % 3;
			if (sg == 1) { s += '-'; } else if (sg == 2) { s += '+'; }
			if (rng() % 200 == 0) { s += std::to_string(rng() % 4000000000ULL); }
			else { s += std::to_string(rng() % static_cast<unsigned>(max_exp + 1)); }
		}
		return s;
	}

	// Format families. 0..9 double, 10..14 float, 15..16 digit strings and long mantissas.
	static constexpr int kFormats = 17;
	std::string text(int fmt) {
		char b[512];
		switch (fmt) {
			case 0: std::snprintf(b, sizeof b, "%.17g", rand_double()); return b;
			case 1: std::snprintf(b, sizeof b, "%.16g", rand_double()); return b;
			case 2: std::snprintf(b, sizeof b, "%.15g", rand_double()); return b;
			case 3: return shortest(rand_double());
			case 4: std::snprintf(b, sizeof b, "%.17e", rand_double()); return b;
			case 5: {  // fixed notation of moderate numbers
				const double d = std::ldexp(static_cast<double>(rng() >> 11), -static_cast<int>(rng() % 70)) * ((rng() & 1) ? 1.0 : -1.0);
				std::snprintf(b, sizeof b, "%.*f", static_cast<int>(rng() % 19), std::fmod(d, 1e10));
				return b;
			}
			case 6: std::snprintf(b, sizeof b, "%.*e", static_cast<int>(rng() % 20), rand_double()); return b;
			case 7: {  // short prices and measures: d{1,8}.d{1,3}
				std::string s = std::to_string(rng() % 100000000ULL);
				s += '.';
				s += std::to_string(rng() % 1000);
				return (rng() % 5 == 0 ? "-" : "") + s;
			}
			case 8: {  // integers
				std::string s = std::to_string(rng());
				if (rng() % 3 == 0) { s += std::to_string(rng() % 1000000); }
				return (rng() % 5 == 0 ? "-" : "") + s;
			}
			case 9: std::snprintf(b, sizeof b, "%.17g", static_cast<double>(rand_float())); return b;  // exact floats as 17 digit text
			case 10: std::snprintf(b, sizeof b, "%.9g", static_cast<double>(rand_float())); return b;
			case 11: return shortest(rand_float());
			case 12: std::snprintf(b, sizeof b, "%.8g", static_cast<double>(rand_float())); return b;
			case 13: std::snprintf(b, sizeof b, "%.9e", static_cast<double>(rand_float())); return b;
			case 14: return digit_string(9, 50);
			case 15: return digit_string(40, 340);
			default: {  // long mantissas: a random head, then zeros / random digits and sometimes a final 1
				std::string s = (rng() & 1) ? "-" : "";
				const int head = 1 + static_cast<int>(rng() % 25);
				for (int i = 0; i < head; ++i) { s += static_cast<char>('0' + rng() % 10); }
				const int tail = 1 + static_cast<int>(rng() % (rng() % 8 == 0 ? 800 : 60));
				const unsigned kind = rng() % 3;
				for (int i = 0; i < tail; ++i) { s += kind == 0 ? '0' : static_cast<char>('0' + rng() % 10); }
				if (kind == 0 && (rng() & 1)) { s += '1'; }
				const int dot = static_cast<int>(rng() % (s.size() - (s[0] == '-'))) + (s[0] == '-');
				s.insert(static_cast<std::size_t>(dot), ".");
				if (rng() & 1) { s += "e" + std::to_string(static_cast<int>(rng() % 700) - 350); }
				return s;
			}
		}
	}
};

// ------------------------------------------------------------------------------------------------ driver helpers
static std::string junk_suffix(std::mt19937_64& rng) {
	static const char* const junk[] = {"x", " ", "e", "e+", "E-", ".", "..", "f", "L", "0x", "_", " 1", "e5x"};
	const unsigned k = static_cast<unsigned>(rng() % 14);
	if (k == 13) { return std::string("\0abc", 4); }
	return junk[k];
}

static void print_stats(const char* what, const Stats& st, double secs) {
	std::printf("%s: %llu texts in %.1fs\n", what, (unsigned long long)st.texts, secs);
	for (int mi = 0; mi < 4; ++mi) {
		if (st.checks[mi][0] == 0) { continue; }
		std::printf("  %-11s double: %llu checks, %.2f%% by the fast path, %llu ERANGE, %llu EINVAL | float: %llu checks, %.2f%% by the fast path, %llu ERANGE, %llu EINVAL\n", kModes[mi].name,
			(unsigned long long)st.checks[mi][0], 100.0 * st.taken[mi][0] / st.checks[mi][0], (unsigned long long)st.erange[mi][0], (unsigned long long)st.einval[mi][0],
			(unsigned long long)st.checks[mi][1], 100.0 * st.taken[mi][1] / st.checks[mi][1], (unsigned long long)st.erange[mi][1], (unsigned long long)st.einval[mi][1]);
	}
	std::printf("  cross-checked against from_chars: %llu, fast_float: %llu\n", (unsigned long long)st.cross[0], (unsigned long long)st.cross[1]);
	std::printf("  exact path (default mode): double converted %llu, ERANGE %llu, declined (decimal text) %llu; float converted %llu, ERANGE %llu, declined %llu\n", (unsigned long long)st.slow_ok[0],
		(unsigned long long)st.slow_range[0], (unsigned long long)st.slow_decl[0], (unsigned long long)st.slow_ok[1], (unsigned long long)st.slow_range[1], (unsigned long long)st.slow_decl[1]);
#ifdef STRICT_STOX_COUNT_LIBC
	std::printf("  API calls on decimal text that reached libc (by design: a subnormal or the smallest normal value): double %llu, float %llu\n", (unsigned long long)st.reach[0], (unsigned long long)st.reach[1]);
#endif
	if (st.ab[0] != 0) {
		std::printf("  compared with the previous header: double %llu (%.2f%% taken), float %llu (%.2f%% taken)\n", (unsigned long long)st.ab[0], 100.0 * st.ab_taken[0] / st.ab[0],
			(unsigned long long)st.ab[1], 100.0 * st.ab_taken[1] / st.ab[1]);
	}
}

static int finish() {
	std::uint64_t total = 0;
	for (int c = 0; c < C_N; ++c) {
		if (g_fail[c]) { std::printf("  %s %-24s %llu\n", c == C_LIBC_ABORT ? "NOTE      " : "MISMATCHES", cat_name[c], (unsigned long long)g_fail[c].load()); }
		if (c != C_LIBC_ABORT) { total += g_fail[c]; }
	}
	if (g_libc_aborts.load() != 0) { std::printf("  NOTE: libc aborted (SIGABRT, trapped) on %llu text(s); the fast path was still checked against the other oracles\n", (unsigned long long)g_libc_aborts.load()); }
	std::printf(total == 0 ? "RESULT: 0 mismatches\n" : "RESULT: %llu MISMATCHES\n", (unsigned long long)total);
	return total == 0 ? 0 : 1;
}

// ------------------------------------------------------------------------------------------------ commands
static const char* const kDoubleHard[] = {
	"0", "-0", "0.0", "-0.0e5", "1", "-1", "0.1", "0.2", "0.3", "4.35", "2.675", "1.005", "123456789012345678", "9007199254740992", "9007199254740993", "9007199254740994",
	"9007199254740993.0000000000000000001", "9007199254740992.9999999999999999999", "18014398509481985", "18014398509481987", "1e23", "9.999999999999999e22", "1.0000000000000001e23",
	"8.41e21", "5e-324", "4.9406564584124654e-324", "4.9406564584124653e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "1e-323", "2.2250738585072011e-308",
	"2.2250738585072012e-308", "2.2250738585072013e-308", "2.2250738585072014e-308", "2.2250738585072009e-308", "2.225073858507201e-308", "2.2250738585072008e-308",
	"1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1.797693134862315807e308", "1.7976931348623158e+308", "1.8e308", "1e309", "-1e309", "1e-400", "-1e-400",
	"0e999999999999999999999", "0.0e-9999999999999999999", "1e999999999999999999999", "1e-99999999999999999999", "1e00000000000000000000000000000000000000000308", "1e-00000000000000000000000000000000000324",
	"6.02214076e23", "1.602176634e-19", "299792458", "3.141592653589793238462643383279502884197", "2.718281828459045235360287471352662497757", "0.000000000000000000000000000000000000000001",
	"100000000000000000000", ("1" "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"),
	"4.4501477170144028e-308", "4.4501477170144023e-308", "7.2057594037927933e16", "7.2057594037927936e16", "1.00000000000000011102230246251565404236316680908203125", "1.00000000000000011102230246251565404236316680908203124",
	"1.00000000000000011102230246251565404236316680908203126", "0.99999999999999994448884876874217297882", "0.999999999999999944488848768742172978818416595458984375", "5e-325", "2e-324", "3e-324", "1e308", "9e307", "1.2345678901234567890123456789e-5",
	"  1.5", "1.5 ", "+1.5", "-+1.5", "1.5e", "1.5e+", "1.5e-", ".", "-.", ".e1", "1..5", "1.5.5", "0x10", "0x1p3", "inf", "-inf", "infinity", "nan", "-nan", "nan(123)", "1e1.5", "1,5", "1_0", "\xd9\xa1", "e5", "E5", "5e5e5",
	"1e18446744073709551616", "1e18446744073709551617", "1e-18446744073709551616", "1e36893488147419103232", "1e-36893488147419103232", "0.0000001e18446744073709551623", "1e99999999999999999999999",
	"1e-99999999999999999999999", "1e4294967296", "1e-4294967296", "1e2147483648", "1e-2147483649", "1e9223372036854775808", "1e-9223372036854775809", "1.5e0000000000000000000000000000000000000000002",
	"1234567890123456789012345678901234567890e-30", "00000000000000000000000000000000000000001.5", "0.00000000000000000000000000000000000000000000000000000000000000000001e68",
};
static const char* const kFloatHard[] = {
	"16777216", "16777217", "16777218", "16777217.0000001", "16777216.9999999", "33554434", "33554435", "8388608.5", "8388609.5", "1.17549435e-38", "1.1754943508222875e-38", "1.1754942807573643e-38",
	"1.1754942106924411e-38", "1.4e-45", "1.401298464324817e-45", "7.006492321624085e-46", "7.0064923216240854e-46", "7.0064923216240856e-46", "3.4028235e38", "3.4028234663852886e38", "3.4028235677973366e38",
	"3.4028235677973363e38", "3.4028235677973367e38", "3.4028236e38", "3.4028237e38", "1e39", "1e-46", "1e-45", "0.1", "0.3", "0.7", "16777216.5", "1e10", "1e-10", "3.4e38", "3.5e38", "1.5e-45", "2.1e-45", "9.99999e-39",
};

static int cmd_hard(const Opt& o) {
	std::vector<std::string> texts;
	for (const char* s : kDoubleHard) { texts.push_back(s); }
	for (const char* s : kFloatHard) { texts.push_back(s); }
	for (const char* s : {"1", "-1", "0.1", "1e-400", "1e400"}) { texts.push_back(std::string(s)); }
	// embedded NUL, junk after a number
	texts.push_back(std::string("12\0" "34", 5));
	texts.push_back(std::string("1.5\0e3", 6));
	texts.push_back(std::string("\0" "12", 3));
	texts.push_back(std::string(""));
	texts.push_back(std::string(" "));
	// Range limits and neighbours, exactly: double and float, min subnormal, its half, the subnormal/normal limit, DBL_MAX and the overflow midpoint.
	std::mt19937_64 rng(o.seed);
	struct Lim { std::uint64_t m; int e2; };
	const Lim lims[] = {
		{1, -1074}, {1, -1075}, {3, -1075}, {(1ULL << 52) - 1, -1074}, {(1ULL << 53) - 1, -1075}, {1ULL << 52, -1074}, {(1ULL << 53) - 1, 971}, {(1ULL << 54) - 1, 970}, {(1ULL << 54) - 3, 970}, {(1ULL << 54) - 5, 970},
		{1, -149}, {1, -150}, {3, -150}, {(1ULL << 23) - 1, -149}, {(1ULL << 24) - 1, -150}, {1ULL << 23, -149}, {(1ULL << 24) - 1, 104}, {(1ULL << 25) - 1, 103}, {(1ULL << 25) - 3, 103},
		{1ULL << 24, 0}, {(1ULL << 24) + 1, 0}, {(1ULL << 53), 0}, {(1ULL << 53) + 1, 0}, {(1ULL << 53) + 2, 0}, {(1ULL << 53) + 3, 0}, {(1ULL << 63), 0}, {(1ULL << 63) + 1024, 0}, {(1ULL << 63) + 1025, 0},
	};
	for (const Lim& l : lims) {
		std::string digits;
		int x10;
		exact_decimal(l.m, l.e2, digits, x10);
		for (int rep = 0; rep < 3; ++rep) { variants(digits, x10, rng, texts); }
	}
	// 100000 digit mantissas with exponents that a clamped accumulator would cancel; 800 digit inputs.
	for (unsigned e : {100000u, 1000000u, 9999999u}) {
		const std::string z(e < 1000000 ? e - 1 : 99999, '0');
		for (const char* x : {"1e", "1e-", "1e+"}) {
			texts.push_back("0." + z + x + std::to_string(e + 900000));
			texts.push_back("0." + z + x + std::to_string(e + 900000 + 1));
			texts.push_back("1" + z + "0" + x + std::to_string(e));
			texts.push_back("1" + z + "0" + x + std::to_string(e * 10));
		}
	}
	texts.push_back("0." + std::string(99999, '0') + "1e1000001");
	texts.push_back("0." + std::string(99999, '0') + "1e100000");
	texts.push_back("0." + std::string(310, '0') + "1" + std::string(800, '0') + "5");
	texts.push_back("1" + std::string(308, '0'));
	texts.push_back("1" + std::string(309, '0'));
	texts.push_back("1." + std::string(800, '0'));
	texts.push_back("1." + std::string(800, '0') + "1");
	texts.push_back("0." + std::string(323, '0') + "5");
	std::printf("hard cases: %zu texts\n", texts.size());
	Opt oo = o;
	std::atomic<std::size_t> next{0};
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<Stats> sts(static_cast<std::size_t>(o.threads));
	std::vector<std::thread> ts;
	for (int t = 0; t < o.threads; ++t) {
		ts.emplace_back([&, t] {
			for (;;) {
				const std::size_t i = next.fetch_add(1);
				if (i >= texts.size()) { break; }
				check_text(texts[i], 15, true, true, sts[static_cast<std::size_t>(t)], oo);
				// each with junk after it and with a sign in front
				std::mt19937_64 r(i);
				check_text(texts[i] + junk_suffix(r), 15, true, true, sts[static_cast<std::size_t>(t)], oo);
				check_text("-" + texts[i], 15, true, true, sts[static_cast<std::size_t>(t)], oo);
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats all;
	for (auto& s : sts) { all.add(s); }
	print_stats("hard", all, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return finish();
}

static int cmd_syntax(const Opt& o) {
	// Every string of up to `maxlen` characters over a syntax alphabet.
	const std::string al = "019-+ .eE";
	const int maxlen = static_cast<int>(o.n);
	std::uint64_t total = 1;
	std::vector<std::uint64_t> starts;
	std::uint64_t count = 0;
	for (int len = 0; len <= maxlen; ++len) { count += total; total *= al.size(); }
	std::atomic<std::uint64_t> next{0};
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<Stats> sts(static_cast<std::size_t>(o.threads));
	std::vector<std::thread> ts;
	for (int t = 0; t < o.threads; ++t) {
		ts.emplace_back([&, t] {
			constexpr std::uint64_t chunk = 4096;
			for (;;) {
				const std::uint64_t lo = next.fetch_add(chunk);
				if (lo >= count) { break; }
				for (std::uint64_t k = lo; k < std::min(count, lo + chunk); ++k) {
					// decode k into (length, index)
					std::uint64_t idx = k, per = 1;
					int len = 0;
					while (idx >= per) { idx -= per; per *= al.size(); ++len; }
					std::string s(static_cast<std::size_t>(len), ' ');
					for (int i = 0; i < len; ++i) { s[static_cast<std::size_t>(i)] = al[idx % al.size()]; idx /= al.size(); }
					check_text(s, k % 16 == 0 ? 15 : 1, true, k % 4 == 0, sts[static_cast<std::size_t>(t)], o);
				}
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats all;
	for (auto& s : sts) { all.add(s); }
	print_stats("syntax (all strings up to the length over 019-+ .eE)", all, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return finish();
}

// N texts of each format, each checked in the default mode and (mode_mask) in the directed ones.
static int cmd_rand(const Opt& o, unsigned mode_mask) {
	const auto t0 = std::chrono::steady_clock::now();
	Stats all;
	for (int fmt = o.fmt_lo; fmt < Gen::kFormats && fmt <= o.fmt_hi; ++fmt) {
		std::vector<Stats> sts(static_cast<std::size_t>(o.threads));
		std::vector<std::thread> ts;
		for (int t = 0; t < o.threads; ++t) {
			ts.emplace_back([&, t, fmt] {
				Gen g(o.seed * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(fmt) * 1000003ULL + static_cast<std::uint64_t>(t));
				Stats& st = sts[static_cast<std::size_t>(t)];
				const std::uint64_t per = o.n / static_cast<std::uint64_t>(o.threads);
				for (std::uint64_t i = 0; i < per; ++i) {
					std::string s = g.text(fmt);
					if (g.rng() % 8 == 0) { s += junk_suffix(g.rng); }
					check_text(s, mode_mask, i % static_cast<std::uint64_t>(o.api_every) == 0, i % 16 == 0, st, o);
				}
			});
		}
		for (auto& t : ts) { t.join(); }
		Stats fs;
		for (auto& s : sts) { fs.add(s); }
		char what[64];
		std::snprintf(what, sizeof what, "format %d", fmt);
		print_stats(what, fs, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
		all.add(fs);
		std::fflush(stdout);
	}
	print_stats("rand total", all, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return finish();
}

// Every positive finite float32 (stride k): shortest, 9 and 17 digit text, as float and as double.
static int cmd_f32(const Opt& o, bool mid) {
	const auto t0 = std::chrono::steady_clock::now();
	std::atomic<std::uint64_t> next{1};
	constexpr std::uint64_t chunk = 1ULL << 16;
	const std::uint64_t limit = 0x7f800000ULL;
	std::vector<Stats> sts(static_cast<std::size_t>(o.threads));
	std::vector<std::thread> ts;
	for (int t = 0; t < o.threads; ++t) {
		ts.emplace_back([&, t] {
			Stats& st = sts[static_cast<std::size_t>(t)];
			std::mt19937_64 rng(o.seed + static_cast<std::uint64_t>(t));
			std::vector<std::string> v;
			char b[64];
			for (;;) {
				const std::uint64_t lo = next.fetch_add(chunk * static_cast<std::uint64_t>(o.stride));
				if (lo >= limit) { break; }
				std::uint64_t n = 0;
				for (std::uint64_t i = lo; i < std::min(limit, lo + chunk * static_cast<std::uint64_t>(o.stride)); i += static_cast<std::uint64_t>(o.stride), ++n) {
					const std::uint32_t bits = static_cast<std::uint32_t>(i);
					float f;
					std::memcpy(&f, &bits, 4);
					const bool neg = (i % 61) == 0;
					const bool api = (i / static_cast<std::uint64_t>(o.stride)) % static_cast<std::uint64_t>(o.api_every) == 0;
					v.clear();
					if (!mid) {
						v.push_back(Gen::shortest(f));
						std::snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
						v.push_back(b);
						std::snprintf(b, sizeof b, "%.17g", static_cast<double>(f));
						v.push_back(b);
					} else {
						std::uint64_t mant = bits & 0x7fffff;
						int e = static_cast<int>(bits >> 23);
						if (e == 0) { e = -149; } else { mant |= 1u << 23; e -= 150; }
						std::string digits;
						int x10;
						exact_decimal(2 * mant + 1, e - 1, digits, x10);
						variants(digits, x10, rng, v);
					}
					for (std::string& s : v) {
						if (neg) { s = "-" + s; }
						check_text(s, 1, api, false, st, o);
					}
				}
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats all;
	for (auto& s : sts) { all.add(s); }
	print_stats(mid ? "float32 midpoints (stride)" : "float32 sweep: shortest, %.9g, %.17g (stride)", all, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return finish();
}

// Midpoints between neighbouring doubles (random doubles of every kind), exact and around, in all four modes.
static int cmd_dmid(const Opt& o) {
	const auto t0 = std::chrono::steady_clock::now();
	std::vector<Stats> sts(static_cast<std::size_t>(o.threads));
	std::vector<std::thread> ts;
	for (int t = 0; t < o.threads; ++t) {
		ts.emplace_back([&, t] {
			Stats& st = sts[static_cast<std::size_t>(t)];
			Gen g(o.seed * 7919 + static_cast<std::uint64_t>(t));
			std::vector<std::string> v;
			const std::uint64_t per = o.n / static_cast<std::uint64_t>(o.threads);
			for (std::uint64_t i = 0; i < per; ++i) {
				const double d = std::fabs(g.rand_double());
				std::uint64_t bits;
				std::memcpy(&bits, &d, 8);
				std::uint64_t mant = bits & ((1ULL << 52) - 1);
				int e = static_cast<int>(bits >> 52);
				if (e == 0) { e = -1074; } else { mant |= 1ULL << 52; e -= 1075; }
				std::string digits;
				int x10;
				exact_decimal(2 * mant + 1, e - 1, digits, x10);
				v.clear();
				variants(digits, x10, g.rng, v);
				for (const std::string& s : v) { check_text(s, i % 8 == 0 ? 15 : 1, true, false, st, o); }
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats all;
	for (auto& s : sts) { all.add(s); }
	print_stats("double midpoints", all, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
	return finish();
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: verify hard|syntax|rand|modes|f32|f32mid|dmid [-t threads] [-n count] [-s seed] [--stride k] [--api-every k] [--no-cross] [--fc-max bytes] [--fmt lo-hi] [--ab | --ab-only]\n");
		return 2;
	}
	Opt o;
	std::signal(SIGABRT, on_abort);
	const std::string cmd = argv[1];
	for (int i = 2; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "-t" && i + 1 < argc) { o.threads = std::atoi(argv[++i]); }
		else if (a == "-n" && i + 1 < argc) { o.n = std::strtoull(argv[++i], nullptr, 10); }
		else if (a == "-s" && i + 1 < argc) { o.seed = std::strtoull(argv[++i], nullptr, 10); }
		else if (a == "--stride" && i + 1 < argc) { o.stride = std::atoi(argv[++i]); }
		else if (a == "--api-every" && i + 1 < argc) { o.api_every = std::atoi(argv[++i]); }
		else if (a == "--no-cross") { o.cross = false; }
		else if (a == "--ab") { o.ab = 1; }
		else if (a == "--ab-only") { o.ab = 2; }
		else if (a == "--fmt" && i + 1 < argc) { std::sscanf(argv[++i], "%d-%d", &o.fmt_lo, &o.fmt_hi); }
		else if (a == "--fc-max" && i + 1 < argc) { o.fc_max = static_cast<std::size_t>(std::atoll(argv[++i])); }
	}
	std::printf("strict_stox verify %s: threads %d seed %llu n %llu stride %d | oracles: libc", cmd.c_str(), o.threads, (unsigned long long)o.seed, (unsigned long long)o.n, o.stride);
#ifdef HAVE_FROM_CHARS
	std::printf(", from_chars");
#endif
#ifdef HAVE_FAST_FLOAT
	std::printf(", fast_float");
#endif
	std::printf("\n");
	if (cmd == "text") {  // verify text STRING...: every check, every rounding mode, on the given texts
		Stats st;
		for (int i = 2; i < argc; ++i) { if (argv[i][0] != '-' || argv[i][1] != '-') { check_text(argv[i], 15, true, true, st, o); } }
		print_stats("text", st, 0);
		return finish();
	}
	if (cmd == "hard") { return cmd_hard(o); }
	if (cmd == "syntax") { return cmd_syntax(o); }
	if (cmd == "rand") { return cmd_rand(o, 1); }
	if (cmd == "modes") { return cmd_rand(o, 15); }
	if (cmd == "f32") { return cmd_f32(o, false); }
	if (cmd == "f32mid") { return cmd_f32(o, true); }
	if (cmd == "dmid") { return cmd_dmid(o); }
	std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
	return 2;
}
