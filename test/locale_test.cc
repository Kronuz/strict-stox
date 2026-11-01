// The locale guard of strict_stod / strict_stof.
//
// strtod reads the decimal point of the current locale (LC_NUMERIC). In a locale whose decimal
// point is ',', the text "1.5" is the number 1 followed by ".5", and "1,5" is 1.5. The fast path
// reads '.', so it must decline every text with a '.' there and leave the answer to strtod, whatever
// the text or the type; text without a '.' it still takes. STRICT_STOX_C_LOCALE removes the query on
// purpose: the fast path then reads '.' in every locale.
//
// Two builds of this file (CMake registers both):
//   mock  (LOCALE_TEST_MOCK)  The decimal point the header sees is a variable of this test
//                             (STRICT_STOX_DECIMAL_POINT), so the guard is tested on a machine with no
//                             comma locale installed (CI Linux images have none).
//   real                      The header asks the C library. The test needs an installed locale whose
//                             decimal point is not '.' (de_DE, fr_FR, ...; or the one named in the
//                             environment variable STRICT_STOX_TEST_LOCALE) and says so when it skips.
//                             It checks the global locale and a thread's own locale (uselocale)
//                             against libc: value, errno, idx.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <clocale>
#include <locale.h>

#ifdef LOCALE_TEST_MOCK
static const char* g_radix = ".";
#define STRICT_STOX_DECIMAL_POINT g_radix
#endif

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

// Texts with a '.', and texts without one. The plain ones must be taken by the fast path in any locale.
static const char* const dotted[] = {"1.5", "-1.5", ".5", "5.", "-.5", "0.1", "-0.1", "1.5e2", "1.5E-2", "123456789.123456789", "0.30000000000000004", "1234567890123456789.5",
	"-0.", "00000000000000000000001.5", "2.5e30"};
static const char* const plain[] = {"15", "-15", "1e5", "1E-5", "-7e+3", "0", "-0", "1234567", "9007199254740991", "1e30", "12345678901234567890"};
// Spelled with the comma: text for a comma locale (libc reads it as a number there, as a prefix elsewhere).
static const char* const commas[] = {"1,5", "-1,5", ",5", "5,", "-0,25", "1,5e2", "1,5,5", "1.5,5"};

template <typename R>
static void check_taken(const char* where, const char* s, bool expect, int line) {
	R v = 0;
	const bool got = D::fast_fp<R>(v, s);
	if (got != expect) {
		FAIL("%s: fast_fp<%s>(\"%s\") %s, expected %s (line %d)", where, std::is_same_v<R, double> ? "double" : "float", s, got ? "taken" : "declined", expect ? "taken" : "declined", line);
		return;
	}
	if (got) {
		const oracle::Result want = oracle::libc_result<R>(s, false);
		// In a comma locale a taken text has no '.', so libc reads all of it; in the mock the libc locale is "C".
		if (want.err != 0 || want.bits != oracle::to_bits(v)) { FAIL("%s: fast_fp<%s>(\"%s\") bits differ from libc", where, std::is_same_v<R, double> ? "double" : "float", s); }
	}
}

// The same for the exact path, which sees every text the fast path declines: in a locale whose decimal
// point is not '.', it must decline the dotted texts too (status 0), and still convert the plain ones.
template <typename R>
static void check_slow(const char* where, const char* s, bool expect, int line) {
	R v = 0;
	const int st = D::fp_slow<R>(v, s);
	if ((st != 0) != expect) {
		FAIL("%s: fp_slow<%s>(\"%s\") %s, expected %s (line %d)", where, std::is_same_v<R, double> ? "double" : "float", s, st != 0 ? "converted" : "declined", expect ? "converted" : "declined", line);
		return;
	}
	if (st == 1) {
		const oracle::Result want = oracle::libc_result<R>(s, false);
		if (want.err != 0 || want.bits != oracle::to_bits(v)) { FAIL("%s: fp_slow<%s>(\"%s\") bits differ from libc", where, std::is_same_v<R, double> ? "double" : "float", s); }
	}
}

// The API must equal libc, in whatever locale is current, for every text.
static void check_api_all(const char* where, const std::vector<std::string>& texts) {
	for (const std::string& s : texts) {
		for (int useidx = 0; useidx < 2; ++useidx) {
			const oracle::Result wd = oracle::libc_result<double>(s, useidx != 0), gd = oracle::api_result<double>(s, useidx != 0);
			if (!oracle::same(gd, wd, useidx != 0)) {
				FAIL("%s: stod(\"%s\"%s): bits %llx errno %d idx %zu, libc bits %llx errno %d idx %zu", where, s.c_str(), useidx ? ", idx" : "", static_cast<unsigned long long>(gd.bits), gd.err,
					gd.idx, static_cast<unsigned long long>(wd.bits), wd.err, wd.idx);
			}
			const oracle::Result wf = oracle::libc_result<float>(s, useidx != 0), gf = oracle::api_result<float>(s, useidx != 0);
			if (!oracle::same(gf, wf, useidx != 0)) {
				FAIL("%s: stof(\"%s\"%s): bits %llx errno %d idx %zu, libc bits %llx errno %d idx %zu", where, s.c_str(), useidx ? ", idx" : "", static_cast<unsigned long long>(gf.bits), gf.err,
					gf.idx, static_cast<unsigned long long>(wf.bits), wf.err, wf.idx);
			}
		}
	}
}

static std::vector<std::string> all_texts() {
	std::vector<std::string> v;
	for (const char* s : dotted) { v.push_back(s); }
	for (const char* s : plain) { v.push_back(s); }
	for (const char* s : commas) { v.push_back(s); }
	return v;
}

#ifdef LOCALE_TEST_MOCK

static void mock_radixes() {
	// '.' is the only decimal point the fast path reads; anything else (a comma, a two byte separator,
	// a '.' followed by more, an empty string) sends the dotted texts to strtod.
	const char* const radixes[] = {".", ",", "\xd9\xab", "\xc2\xb7", "..", ".,", ".x", ""};
	for (const char* r : radixes) {
		g_radix = r;
#ifdef STRICT_STOX_C_LOCALE
		const bool reads_dot = true;  // the query is compiled out: '.' whatever the locale says
#else
		const bool reads_dot = std::strcmp(r, ".") == 0;
#endif
		char where[48];
		std::snprintf(where, sizeof where, "mock decimal point \"%s\"", r);
		for (const char* s : dotted) {
			check_taken<double>(where, s, reads_dot, __LINE__);
			check_taken<float>(where, s, reads_dot, __LINE__);
			check_slow<double>(where, s, reads_dot, __LINE__);
			check_slow<float>(where, s, reads_dot, __LINE__);
		}
		for (const char* s : plain) {
			check_taken<double>(where, s, true, __LINE__);
			check_taken<float>(where, s, true, __LINE__);
			check_slow<double>(where, s, true, __LINE__);
			check_slow<float>(where, s, true, __LINE__);
		}
		check_api_all(where, all_texts());  // the mock changes no libc answer; whoever answers, it is libc's
	}
	g_radix = ".";
	std::printf("  mock decimal points: %zu values, %zu dotted and %zu plain texts each\n", sizeof(radixes) / sizeof(radixes[0]), sizeof(dotted) / sizeof(dotted[0]),
		sizeof(plain) / sizeof(plain[0]));
}

#else  // real locales

static std::string find_comma_locale() {
	std::vector<std::string> names;
	if (const char* env = std::getenv("STRICT_STOX_TEST_LOCALE")) { names.push_back(env); }
	for (const char* n : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "fr_FR", "de_DE.ISO8859-1", "de_DE.ISO8859-15", "fr_FR.ISO8859-1", "es_ES.UTF-8", "pt_BR.UTF-8", "it_IT.UTF-8"}) {
		names.push_back(n);
	}
	const char* saved = std::setlocale(LC_NUMERIC, nullptr);
	const std::string keep = saved ? saved : "C";
	std::string found;
	for (const std::string& n : names) {
		if (std::setlocale(LC_NUMERIC, n.c_str()) == nullptr) { continue; }
		if (*std::localeconv()->decimal_point != '.') { found = n; break; }
	}
	std::setlocale(LC_NUMERIC, keep.c_str());
	return found;
}

static void in_comma_locale(const char* where) {
	for (const char* s : dotted) {
		check_taken<double>(where, s, false, __LINE__);
		check_taken<float>(where, s, false, __LINE__);
		check_slow<double>(where, s, false, __LINE__);
		check_slow<float>(where, s, false, __LINE__);
	}
	for (const char* s : plain) {
		check_taken<double>(where, s, true, __LINE__);
		check_taken<float>(where, s, true, __LINE__);
		check_slow<double>(where, s, true, __LINE__);
		check_slow<float>(where, s, true, __LINE__);
	}
	check_api_all(where, all_texts());
	// What the comma locale means for the strict API (libc says the same).
	if (strict_stod("1,5") != 1.5) { FAIL("%s: stod(\"1,5\") is not 1.5", where); }
	std::size_t idx = 0;
	int err = -1;
	if (strict_stod(&err, "1.5", &idx) != 1.0 || err != 0 || idx != 1) { FAIL("%s: stod(\"1.5\", idx) is not 1 with idx 1", where); }
	strict_stod(&err, "1.5");
	if (err != EINVAL) { FAIL("%s: stod(\"1.5\") without idx is not EINVAL (%d)", where, err); }
}

#endif

int main() {
	std::printf("strict_stox locale guard\n");
#ifdef LOCALE_TEST_MOCK
	mock_radixes();
#elif defined(STRICT_STOX_C_LOCALE)
	std::printf("  real locales: skipped (STRICT_STOX_C_LOCALE reads '.' in every locale)\n");
#else
	const std::string loc = find_comma_locale();
	if (loc.empty()) {
		std::printf("  real locales: no locale with a decimal point other than '.' is installed, skipped (set STRICT_STOX_TEST_LOCALE)\n");
	} else {
		std::printf("  real locales: using %s\n", loc.c_str());
		// Global locale.
		std::setlocale(LC_NUMERIC, loc.c_str());
		in_comma_locale("global locale");
		std::setlocale(LC_NUMERIC, "C");
		// Back in "C": the dotted texts are taken again.
		for (const char* s : dotted) { check_taken<double>("global locale back to C", s, true, __LINE__); }
		check_api_all("global C", all_texts());
#if defined(__unix__) || defined(__APPLE__)
		// A thread's own locale (uselocale) with the global one still "C", and the other way round.
		locale_t l = newlocale(LC_NUMERIC_MASK, loc.c_str(), static_cast<locale_t>(nullptr));
		if (l == static_cast<locale_t>(nullptr)) {
			FAIL("newlocale(%s) failed", loc.c_str());
		} else {
			std::thread th([&] {
				uselocale(l);
				in_comma_locale("thread locale");
				uselocale(LC_GLOBAL_LOCALE);
			});
			th.join();
			for (const char* s : dotted) { check_taken<double>("main thread while another thread has a comma locale", s, true, __LINE__); }
			std::setlocale(LC_NUMERIC, loc.c_str());
			std::thread th2([&] {
				locale_t c = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(nullptr));
				uselocale(c);
				for (const char* s : dotted) { check_taken<double>("thread locale C, global comma", s, true, __LINE__); }
				check_api_all("thread locale C, global comma", all_texts());
				uselocale(LC_GLOBAL_LOCALE);
				freelocale(c);
			});
			th2.join();
			std::setlocale(LC_NUMERIC, "C");
			freelocale(l);
		}
#endif
	}
#endif
	if (failures != 0) {
		std::fprintf(stderr, "%ld failures\n", failures);
		return 1;
	}
	std::printf("strict_stox locale OK\n");
	return 0;
}
