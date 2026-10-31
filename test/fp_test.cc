// Floating point fast path against libc strtod/strtof and std::from_chars.
//
// The property under test: whenever strict_stox_detail::fast_fp accepts a text, its
// value is bit for bit what strtod (strtof) returns for it, strtod consumes the
// whole text and sets no errno (no ERANGE: normal results only). A decline is always
// allowed (the strtod path decides), but the plain formats must be accepted, which
// the acceptance counts check. The public strict_stod/strict_stof are compared with
// libc end to end as well (value, errno, idx, trailing junk).
//
//   fp_test                     quick run (the CTest test)
//   fp_test --stress N [T]      N random doubles in each of many formats, on T threads
//   fp_test --f32 [T]           every float32: shortest and 17 digit text, as float and double
//   fp_test --seed S            seed (default 1)
//
// Inputs: the table is checked against exact big integer arithmetic; the classic hard
// cases (Paxson, Clinger, 2^-1074, DBL_MAX and DBL_MIN boundaries, long mantissas,
// 800 digits); every binade's exact halfway point and its neighbours at many lengths
// (built with big integers); random bit patterns printed in several formats; random
// digit strings; and every short string over a syntax alphabet.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cfenv>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "strict_stox.hh"

// std::from_chars for floating point as a second oracle: libstdc++ 11+ defines the feature macro;
// libc++ implements it without defining it, so -DSTRICT_STOX_TEST_FROM_CHARS opts in.
#if (defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L) || defined(STRICT_STOX_TEST_FROM_CHARS)
#define FP_TEST_FROM_CHARS 1
#endif

namespace D = strict_stox_detail;

static std::atomic<long> failures{0};
static std::mutex out_mutex;

#define FAIL(...)                                                                \
	do {                                                                         \
		if (failures.fetch_add(1) < 30) {                                        \
			std::lock_guard<std::mutex> lk(out_mutex);                           \
			std::fprintf(stderr, __VA_ARGS__);                                   \
			std::fprintf(stderr, "\n");                                          \
		}                                                                        \
	} while (0)

// --- A tiny unsigned big integer (base 2^32), enough to build exact decimal text. ---
struct Big {
	std::vector<std::uint32_t> l;  // little endian, no leading zero limbs
	Big() = default;
	explicit Big(std::uint64_t v) {
		while (v != 0) { l.push_back(static_cast<std::uint32_t>(v)); v >>= 32; }
	}
	bool zero() const { return l.empty(); }
	void trim() { while (!l.empty() && l.back() == 0) { l.pop_back(); } }
	void mul_small(std::uint32_t m) {
		std::uint64_t c = 0;
		for (auto& x : l) {
			const std::uint64_t t = static_cast<std::uint64_t>(x) * m + c;
			x = static_cast<std::uint32_t>(t);
			c = t >> 32;
		}
		if (c != 0) { l.push_back(static_cast<std::uint32_t>(c)); }
		trim();
	}
	void add_small(std::uint32_t a) {
		std::uint64_t c = a;
		for (std::size_t i = 0; c != 0; ++i) {
			if (i == l.size()) { l.push_back(0); }
			const std::uint64_t t = static_cast<std::uint64_t>(l[i]) + c;
			l[i] = static_cast<std::uint32_t>(t);
			c = t >> 32;
		}
	}
	void sub_small(std::uint32_t a) {  // requires *this >= a
		std::uint64_t b = a;
		for (std::size_t i = 0; b != 0 && i < l.size(); ++i) {
			const std::uint64_t x = l[i];
			if (x >= b) { l[i] = static_cast<std::uint32_t>(x - b); b = 0; }
			else { l[i] = static_cast<std::uint32_t>(x + (1ULL << 32) - b); b = 1; }
		}
		trim();
	}
	void shl(unsigned n) {
		if (zero()) { return; }
		const unsigned limbs = n / 32, bits = n % 32;
		if (bits != 0) {
			std::uint32_t carry = 0;
			for (auto& x : l) {
				const std::uint32_t nx = (x << bits) | carry;
				carry = x >> (32 - bits);
				x = nx;
			}
			if (carry != 0) { l.push_back(carry); }
		}
		l.insert(l.begin(), limbs, 0);
	}
	unsigned bit_length() const {
		if (l.empty()) { return 0; }
		return static_cast<unsigned>(l.size() - 1) * 32 + (32 - static_cast<unsigned>(__builtin_clz(l.back())));
	}
	static int cmp(const Big& a, const Big& b) {
		if (a.l.size() != b.l.size()) { return a.l.size() < b.l.size() ? -1 : 1; }
		for (std::size_t i = a.l.size(); i-- > 0;) {
			if (a.l[i] != b.l[i]) { return a.l[i] < b.l[i] ? -1 : 1; }
		}
		return 0;
	}
	void mul_u64(std::uint64_t m) {
		Big hi = *this;
		hi.mul_small(static_cast<std::uint32_t>(m >> 32));
		hi.shl(32);
		mul_small(static_cast<std::uint32_t>(m));
		// add hi
		if (l.size() < hi.l.size()) { l.resize(hi.l.size(), 0); }
		std::uint64_t c = 0;
		for (std::size_t i = 0; i < l.size(); ++i) {
			const std::uint64_t t = static_cast<std::uint64_t>(l[i]) + (i < hi.l.size() ? hi.l[i] : 0) + c;
			l[i] = static_cast<std::uint32_t>(t);
			c = t >> 32;
		}
		if (c != 0) { l.push_back(static_cast<std::uint32_t>(c)); }
		trim();
	}
	std::string decimal() const {
		if (zero()) { return "0"; }
		Big t = *this;
		std::vector<std::uint32_t> chunks;
		while (!t.zero()) {
			std::uint64_t rem = 0;
			for (std::size_t i = t.l.size(); i-- > 0;) {
				const std::uint64_t cur = (rem << 32) | t.l[i];
				t.l[i] = static_cast<std::uint32_t>(cur / 1000000000ULL);
				rem = cur % 1000000000ULL;
			}
			t.trim();
			chunks.push_back(static_cast<std::uint32_t>(rem));
		}
		std::string s = std::to_string(chunks.back());
		char b[16];
		for (std::size_t i = chunks.size() - 1; i-- > 0;) {
			std::snprintf(b, sizeof b, "%09u", chunks[i]);
			s += b;
		}
		return s;
	}
};

static Big pow_small(std::uint32_t base, unsigned n) {
	Big b(1);
	for (unsigned i = 0; i < n; ++i) { b.mul_small(base); }
	return b;
}

// --- The 5^q table and the binary exponent formula against exact arithmetic. ---
static void check_table() {
	for (int q = D::pow5_qmin - 20; q <= D::pow5_qmax + 20; ++q) {
		// floor(q * log2(10)) == (q * 217706) >> 16
		const int g = (q * 217706) >> 16;
		const int exact = q >= 0 ? static_cast<int>(pow_small(10, q).bit_length()) - 1 : -static_cast<int>(pow_small(10, -q).bit_length());
		if (g != exact) { FAIL("g(%d) = %d, exact %d", q, g, exact); }
		if (q < D::pow5_qmin || q > D::pow5_qmax) { continue; }
		const std::uint64_t T = D::pow5_tab[q - D::pow5_qmin];
		if (T < (1ULL << 63)) { FAIL("table[%d] not normalised", q); }
		if (q >= 0) {
			Big p = pow_small(5, q);
			const unsigned bl = p.bit_length();
			if (bl >= 64) {
				// T * 2^(bl-64) <= 5^q < (T+1) * 2^(bl-64)
				Big lo(T), hi(T);
				hi.add_small(1);
				lo.shl(bl - 64);
				hi.shl(bl - 64);
				if (Big::cmp(lo, p) > 0 || Big::cmp(p, hi) >= 0) { FAIL("table[%d] != top 64 bits of 5^q", q); }
			} else {
				p.shl(64 - bl);
				if (Big::cmp(p, Big(T)) != 0) { FAIL("table[%d] != 5^q << %u", q, 64 - bl); }
			}
		} else {
			Big d = pow_small(5, -q);
			const unsigned bl = d.bit_length();
			// T * 5^-q <= 2^(63+bl) < (T+1) * 5^-q
			Big lo = d, hi = d;
			lo.mul_u64(T);
			hi.mul_u64(T);
			// hi = T*d + d
			std::uint64_t c = 0;
			for (std::size_t i = 0; i < d.l.size() || c != 0; ++i) {
				if (i == hi.l.size()) { hi.l.push_back(0); }
				const std::uint64_t t = static_cast<std::uint64_t>(hi.l[i]) + (i < d.l.size() ? d.l[i] : 0) + c;
				hi.l[i] = static_cast<std::uint32_t>(t);
				c = t >> 32;
			}
			Big e(1);
			e.shl(63 + bl);
			if (Big::cmp(lo, e) > 0 || Big::cmp(e, hi) >= 0) { FAIL("table[%d] != floor(2^(63+bl) / 5^%d)", q, -q); }
		}
	}
}

// --- Oracles. ---
template <typename R> static R libc_to(const char* s, char** end) { return std::is_same_v<R, double> ? static_cast<R>(std::strtod(s, end)) : static_cast<R>(std::strtof(s, end)); }

struct Stats {
	long checks = 0, accepted = 0;
	void add(const Stats& o) { checks += o.checks; accepted += o.accepted; }
};

template <typename R>
static bool bits_equal(R a, R b) { return std::memcmp(&a, &b, sizeof(R)) == 0; }

// Accepted implies identical to libc (and from_chars).
template <typename R>
static void check_fast(const std::string& s, Stats& st, bool second = true) {
	R got = 0;
	const bool acc = D::fast_fp<R>(got, s);
	++st.checks;
	if (!acc) { return; }
	++st.accepted;
	char* end = nullptr;
	errno = 0;
	const R ref = libc_to<R>(s.c_str(), &end);
	const int err = errno;
	if (err != 0 || end != s.c_str() + s.size() || !bits_equal(got, ref)) {
		FAIL("%s \"%.60s\"%s len=%zu: fast %a libc %a errno %d end-off %td", std::is_same_v<R, double> ? "double" : "float", s.c_str(),
			s.size() > 60 ? "..." : "", s.size(), static_cast<double>(got), static_cast<double>(ref), err, end - s.c_str());
		return;
	}
#ifdef FP_TEST_FROM_CHARS
	if (second && s.size() <= 400) {
		R fc = 0;
		const auto r = std::from_chars(s.data(), s.data() + s.size(), fc);
		if (r.ec == std::errc() && r.ptr == s.data() + s.size() && !bits_equal(got, fc)) {
			FAIL("%s \"%.60s\": fast %a from_chars %a", std::is_same_v<R, double> ? "double" : "float", s.c_str(), static_cast<double>(got), static_cast<double>(fc));
		}
	}
#else
	(void)second;
#endif
}

// The public API against libc: value, errno, idx and the strict trailing text rule.
template <typename R>
static void check_api(const std::string& s) {
	for (int useidx = 0; useidx < 2; ++useidx) {
		char* end = nullptr;
		errno = 0;
		R ref = libc_to<R>(s.c_str(), &end);
		int rerr = errno;
		std::size_t ridx = 0;
		if (rerr == 0 && end == s.c_str()) { rerr = EINVAL; }
		if (rerr == 0) {
			if (useidx) { ridx = static_cast<std::size_t>(end - s.c_str()); }
			else if (end != s.c_str() + s.size()) { rerr = EINVAL; }
		}
		if (rerr != 0) { ref = 0; }
		int e = -1;
		std::size_t idx = 0;
		errno = 4242;
		R got;
		if constexpr (std::is_same_v<R, double>) { got = strict_stod(&e, s, useidx ? &idx : nullptr); }
		else { got = strict_stof(&e, s, useidx ? &idx : nullptr); }
		const bool keep = errno == 4242;
		if (!keep || e != rerr || (rerr == 0 && useidx && idx != ridx) || (!bits_equal(got, ref) && !(got != got && ref != ref))) {
			FAIL("api %s \"%.60s\" len=%zu idx=%d: err %d/%d idx %zu/%zu val %a/%a errno-kept %d", std::is_same_v<R, double> ? "stod" : "stof", s.c_str(), s.size(), useidx, e, rerr, idx, ridx,
				static_cast<double>(got), static_cast<double>(ref), keep);
		}
	}
}

static void check_text(const std::string& s, Stats& sd, Stats& sf, bool api = false) {
	check_fast<double>(s, sd);
	check_fast<float>(s, sf);
	if (api) {
		check_api<double>(s);
		check_api<float>(s);
	}
}

// --- Exact decimal text for the midpoint between two doubles (or floats), and neighbours. ---
// value = N * 10^x10 exactly.
struct Dec {
	Big n;
	int x10 = 0;
};
static Dec midpoint(std::uint64_t m, int e) {  // (2m+1) * 2^(e-1)
	Dec d;
	d.n = Big(2 * m + 1);
	if (e - 1 >= 0) {
		d.n.shl(e - 1);
	} else {
		for (int i = 0; i < 1 - e; ++i) { d.n.mul_small(5); }
		d.x10 = e - 1;
	}
	return d;
}

static std::string render(const std::string& digits, long x10, int style, bool neg) {
	// style 0: digits e x10; style 1: dotted decimal when the exponent is small enough, else style 0.
	std::string out = neg ? "-" : "";
	if (style == 1 && x10 <= 0 && -x10 < 1200) {
		const long k = -x10;
		const long L = static_cast<long>(digits.size());
		if (L > k) { out += digits.substr(0, L - k); if (k > 0) { out += "." + digits.substr(L - k); } }
		else { out += "0." + std::string(k - L, '0') + digits; }
		return out;
	}
	if (style == 2 && digits.size() > 1) {  // d.ddd e (x10 + L - 1)
		out += digits.substr(0, 1) + "." + digits.substr(1) + "e" + std::to_string(x10 + static_cast<long>(digits.size()) - 1);
		return out;
	}
	return out + digits + "e" + std::to_string(x10);
}

static void midpoint_cases(std::uint64_t m, int e, std::mt19937_64& rng, Stats& sd, Stats& sf, bool api) {
	const Dec d = midpoint(m, e);
	const std::string dg = d.n.decimal();
	std::vector<std::pair<std::string, long>> v;  // digits, x10
	v.emplace_back(dg, d.x10);  // the exact halfway point
	{ Big t = d.n; t.sub_small(1); v.emplace_back(t.decimal(), d.x10); }
	{ Big t = d.n; t.add_small(1); v.emplace_back(t.decimal(), d.x10); }
	{ Big t = d.n; t.mul_small(10); t.sub_small(1); v.emplace_back(t.decimal(), d.x10 - 1); }
	{ Big t = d.n; t.mul_small(10); t.add_small(1); v.emplace_back(t.decimal(), d.x10 - 1); }
	for (int i = 0; i < 4; ++i) {  // truncated (below halfway), and truncated + a trailing 1 (above)
		const std::size_t cut = 17 + rng() % 40;
		if (dg.size() > cut) {
			v.emplace_back(dg.substr(0, cut), d.x10 + static_cast<long>(dg.size() - cut));
			v.emplace_back(dg.substr(0, cut) + "1", d.x10 + static_cast<long>(dg.size() - cut) - 1);
		}
	}
	for (auto& [digits, x10] : v) {
		check_text(render(digits, x10, static_cast<int>(rng() % 3), (rng() & 1) != 0), sd, sf, api);
	}
}

// --- Classic hard cases. ---
static const char* const hard[] = {
	// Paxson, Clinger, Java/PHP hangs, boundaries
	"2.2250738585072011e-308", "2.2250738585072012e-308", "2.2250738585072013e-308", "2.2250738585072014e-308", "2.2250738585072015e-308",
	"2.2250738585072008890245868760858598876504231122409594654935248025624400092282356951787758888037591552642309780950434312085877387158357291821993020294379224223559819827501242041788969571311791082261043971979604000454897391938079198936081525613113376800146154100587e-308",
	"4.9406564584124654e-324", "4.9406564584124654417656879286822137236505980e-324", "2.4703282292062327208e-324", "2.4703282292062327209e-324",
	"2.4703282292062328e-324", "2.4703282292062327e-324", "1e-324", "5e-324", "1e-323", "1.5e-323", "0.000000000000000000000000000000000000000000000001e-276",
	"1.7976931348623157e308", "1.7976931348623158e308", "1.7976931348623159e308", "1.797693134862315807e308", "1.7976931348623158079e308",
	"1.797693134862315708145274237317043567981e308", "1.7976931348623157081452742373170435679807056752584499659891747680315726078002853876058955863276687817154045895351438246423432132688946418276846754670353751698604991057655128207624549009038932894407586850845513394230458323690322294816580855933212334827479782620414472316873817718091929988125040402618412485836e308",
	"1e308", "1e309", "-1e309", "1e-400", "0e0", "-0e0", "0.0", "-0.0", "0e999999999999999999999", "1e99999999999999999999", "1e-99999999999999999999",
	"9007199254740991", "9007199254740992", "9007199254740993", "9007199254740994", "9007199254740995", "9007199254740992.5", "9007199254740993.0",
	"18014398509481985", "18014398509481984", "18014398509481986", "1.00000000000000011102230246251565404236316680908203125",
	"1.00000000000000011102230246251565404236316680908203124", "1.00000000000000011102230246251565404236316680908203126",
	"1.00000000000000011102230246251565404236316680908203125000000000000000000000000000000000001",
	"0.99999999999999994448884876874217297882", "0.9999999999999999444888487687421729788", "1.0000000000000002220446049250313080847263336181640625",
	"8.5e-5", "1e23", "1e22", "1e21", "9.999999999999999e22", "9.9999999999999999e22", "8.41e21", "8.5e22", "1.0000000000000001e23", "4.35e-322",
	"0.1", "0.2", "0.3", "0.7", "3.14159265358979323846", "2.718281828459045", "1.1", "5e-5", "123456789012345678", "1234567890123456789",
	"12345678901234567890", "123456789012345678901", "9223372036854775807", "9223372036854775808", "18446744073709551615", "18446744073709551616",
	"18446744073709551617", "9999999999999999999", "10000000000000000000", "99999999999999999999", "0.0000000000000000001", "100000000000000000000000",
	"6.02214076e23", "1.602176634e-19", "299792458", "1.7976931348623157e+308", "2.2250738585072014E-308", "1E5", "1e+5", "1e-5", "5.", ".5", "-.5", "5.e3", ".5e-3",
	// float boundaries
	"3.4028235e38", "3.4028236e38", "3.40282357e38", "3.4028235677973366e38", "3.4028235677973362e38", "3.4028235677973363e38", "1.17549435e-38", "1.1754943e-38",
	"1.1754942e-38", "1.4e-45", "1.401298464324817e-45", "7.006492321624085e-46", "7.0064923216240854e-46", "16777216", "16777217", "16777218", "33554433",
	"8388609", "1e10", "1e11", "1e-10", "3e38", "1e39", "1e-46", "0.1", "0.5", "340282346638528859811704183484516925440", "340282356779733661637539395458142568448",
	"340282356779733661637539395458142568447",
	// syntax the fast path must leave alone
	"", "-", "+", "+1", " 1", "1 ", "1e", "1e+", "1e-", "e1", ".e1", ".", "-.", "1..2", "1.2.3", "--1", "1e5x", "1x", "0x1p3", "0x10", "inf", "-inf", "nan", "infinity",
	"1,5", "1_000", "1e5.5", "1ee5", "١٢٣", "1e0000000000000000000005", "1e+0000000000000000000005", "0.00000000000000000000000000000000000001e40",
	// exponents that wrap around a 32 or 64 bit accumulator
	"1e4294967296", "1e4294967297", "1e-4294967296", "1e18446744073709551616", "1e18446744073709551617", "1e-18446744073709551616",
	"1e00018446744073709551616", "5e9223372036854775808", "5e-9223372036854775808", "0e18446744073709551616", "1.5e4294967306",
};

static std::string long_string(std::mt19937_64& rng, int kind) {
	std::string s;
	const int n = 400 + static_cast<int>(rng() % 700);
	switch (kind) {
		case 0:  // random digits, dot somewhere, optional exponent
			for (int i = 0; i < n; ++i) { s += static_cast<char>('0' + rng() % 10); }
			s.insert(rng() % s.size(), ".");
			if (rng() & 1) { s += "e" + std::to_string(static_cast<int>(rng() % 700) - 350); }
			return s;
		case 1:  // 0.000...0ddd
			return "0." + std::string(n, '0') + std::to_string(rng() % 100000000);
		case 2:  // 1000...0001
			return "1" + std::string(n, '0') + "1" + (rng() & 1 ? "e-" + std::to_string(n + 2) : "");
		case 3:  // 0.99999...
			return "0." + std::string(n, '9');
		case 4:  // exactly 1.0 padded
			return "1." + std::string(n, '0');
		case 5:  // a double plus a long tail of zeros then a 1
			return "1.0000000000000002" + std::string(n, '0') + "1";
		default:
			return std::string(n, '9') + "e-" + std::to_string(n);
	}
}

template <typename R>
static std::string fmt(std::mt19937_64& rng, R v) {
	char b[1500];
	switch (rng() % 9) {
		case 0: std::snprintf(b, sizeof b, "%.17g", static_cast<double>(v)); break;
		case 1: std::snprintf(b, sizeof b, "%.16e", static_cast<double>(v)); break;
		case 2: std::snprintf(b, sizeof b, "%.15g", static_cast<double>(v)); break;
		case 3: std::snprintf(b, sizeof b, "%.9g", static_cast<double>(v)); break;
		case 4: std::snprintf(b, sizeof b, "%.20e", static_cast<double>(v)); break;
		case 5: std::snprintf(b, sizeof b, "%.40e", static_cast<double>(v)); break;
		case 6: std::snprintf(b, sizeof b, "%.18g", static_cast<double>(v)); break;
		case 7: std::snprintf(b, sizeof b, "%g", static_cast<double>(v)); break;
		default: {
#ifdef FP_TEST_FROM_CHARS
			auto r = std::to_chars(b, b + sizeof b, v);
			*r.ptr = '\0';
#else
			std::snprintf(b, sizeof b, "%.17g", static_cast<double>(v));
#endif
		}
	}
	return b;
}

// A random decimal text: 1..30 digits, a dot in a random place, an exponent that sweeps the whole range.
static std::string random_decimal(std::mt19937_64& rng) {
	std::string s;
	if (rng() % 8 == 0) { s += '-'; }
	const int nd = 1 + static_cast<int>(rng() % (rng() % 4 == 0 ? 30 : 19));
	std::string digits;
	for (int i = 0; i < nd; ++i) { digits += static_cast<char>('0' + rng() % 10); }
	if (rng() % 5 == 0) { digits[0] = '0' + 1 + static_cast<char>(rng() % 9); }
	const unsigned dot = static_cast<unsigned>(rng() % (nd + 1));
	std::string t = digits.substr(0, dot);
	if (rng() % 4 != 0 || dot != static_cast<unsigned>(nd)) { t += "."; }
	t += digits.substr(dot);
	if (t == ".") { t = "0."; }
	s += t;
	if (rng() % 3 != 0) {
		s += (rng() & 1) ? "e" : "E";
		const int r = static_cast<int>(rng() % 4);
		const int ex = r == 0 ? static_cast<int>(rng() % 60) - 30 : r == 1 ? static_cast<int>(rng() % 700) - 350 : r == 2 ? static_cast<int>(rng() % 100) - 50 : static_cast<int>(rng() % 20) - 10;
		if (ex < 0) { s += "-"; } else if (rng() & 1) { s += "+"; }
		s += std::to_string(std::abs(ex));
	}
	return s;
}

static void random_work(std::uint64_t seed, long n, Stats& sd, Stats& sf, bool api) {
	std::mt19937_64 rng(seed);
	for (long i = 0; i < n; ++i) {
		const std::uint64_t bits = rng();
		double d;
		std::memcpy(&d, &bits, 8);
		if (std::isfinite(d)) {
			check_fast<double>(fmt<double>(rng, d), sd, (i & 7) == 0);
			if (api && (i & 15) == 0) { check_api<double>(fmt<double>(rng, d)); }
		}
		const std::uint32_t fb = static_cast<std::uint32_t>(rng());
		float f;
		std::memcpy(&f, &fb, 4);
		if (std::isfinite(f)) {
			const std::string s = fmt<float>(rng, f);
			check_fast<float>(s, sf, (i & 7) == 0);
			check_fast<double>(s, sd, false);
		}
		// "random" values in a plain range: ordinary numbers as people write them
		const double u = std::ldexp(static_cast<double>(rng() >> 11), -static_cast<int>(rng() % 70));
		check_fast<double>(fmt<double>(rng, u), sd, (i & 7) == 0);
		const std::string r = random_decimal(rng);
		check_fast<double>(r, sd, (i & 7) == 0);
		check_fast<float>(r, sf, (i & 7) == 0);
		if (api && (i & 15) == 1) { check_api<double>(r); check_api<float>(r); }
	}
}

// Sanitizer builds run this test about 20 times slower: sample every `slow`-th binade and fewer random texts.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
static constexpr int slow = 8;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
static constexpr int slow = 8;
#else
static constexpr int slow = 1;
#endif
#else
static constexpr int slow = 1;
#endif

// Exponents of seven digits or more next to a mantissa of ~10^5 digits: a reader that stops at a
// clamped exponent can cancel the digit count into an in-range q and accept 1e900000 as 0.1.
static void huge_exponent_cases(Stats& sd, Stats& sf) {
	for (std::size_t e : {100000u, 100001u, 123456u, 999999u}) {
		if (slow > 1 && e != 100000u) { continue; }  // megabyte strings: native builds run them all
		const std::string ex = std::to_string(e) + "5";  // seven digits: ten times larger plus 5
		const std::string z(e - 1, '0');
		check_text("0." + z + "1e" + ex, sd, sf, true);       // true value 10^(9e+5): overflow
		check_text("0." + z + "1e-" + ex, sd, sf, true);      // underflow to zero
		check_text("1" + std::string(e, '0') + "e-" + ex, sd, sf, true);  // 10^(e - 10e - 5): zero
		check_text("1" + std::string(e, '0') + "e" + ex, sd, sf, true);   // overflow
		check_text("0." + z + "1e+" + ex, sd, sf, true);
		check_text("0." + z + "0e" + ex, sd, sf, true);      // zero stays zero
	}
}

static void syntax_exhaustive(Stats& sd, Stats& sf, int maxlen) {
	const std::string al = "019-+ .eE";
	std::size_t total = 1;
	std::string s;
	for (int len = 0; len <= maxlen; ++len) {
		for (std::size_t k = 0; k < total; ++k) {
			std::size_t m = k;
			s.assign(len, ' ');
			for (int i = 0; i < len; ++i) { s[i] = al[m % al.size()]; m /= al.size(); }
			check_text(s, sd, sf, true);
		}
		total *= al.size();
	}
	// embedded NUL
	for (const char* t : {"1\0" "2", "12\0", "\0" "12", "1.5\0" "e3"}) { check_text(std::string(t, 4), sd, sf, true); }
}

static void rounding_and_locale() {
	// Non-default rounding modes: the fast path must stand aside; the API follows libc.
	const int modes[] = {FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
	std::mt19937_64 rng(99);
	for (int mode : modes) {
		if (std::fesetround(mode) != 0) { continue; }
		long accepted = 0;
		for (int i = 0; i < 2000; ++i) {
			const std::string s = random_decimal(rng);
			double d = 0;
			if (D::fast_fp<double>(d, s)) { ++accepted; }
			check_api<double>(s);
			check_api<float>(s);
		}
		std::fesetround(FE_TONEAREST);
		if (accepted != 0) { FAIL("fast path accepted %ld texts in rounding mode %d", accepted, mode); }
	}
	// A locale whose decimal point is not '.': "1.5" is "1" then trailing text, "1,5" is 1.5.
	// (With STRICT_STOX_C_LOCALE the fast path reads '.' anyway, by design: nothing to check.)
#ifndef STRICT_STOX_C_LOCALE
	const char* names[] = {"de_DE.UTF-8", "de_DE", "fr_FR.UTF-8", "fr_FR", "de_DE.ISO8859-1"};
	std::string saved = std::setlocale(LC_NUMERIC, nullptr);
	bool done = false;
	for (const char* n : names) {
		if (std::setlocale(LC_NUMERIC, n) == nullptr) { continue; }
		if (*std::localeconv()->decimal_point == '.') { continue; }
		for (const char* s : {"1.5", "1,5", "12345.678", "0.1", "-3.25e2", "-3,25e2", "1e5", "7", "1.5,2"}) {
			check_api<double>(s);
			check_api<float>(s);
			double d = 0;
			if (std::strchr(s, '.') != nullptr && D::fast_fp<double>(d, s)) { FAIL("fast path accepted \"%s\" in locale %s", s, n); }
		}
		done = true;
		break;
	}
	std::setlocale(LC_NUMERIC, saved.c_str());
	std::printf("  locale check: %s\n", done ? "ran with a comma locale" : "no comma locale installed, skipped");
#else
	std::printf("  locale check: skipped (STRICT_STOX_C_LOCALE)\n");
#endif
}

// --- Modes. ---
static void quick(std::uint64_t seed) {
	check_table();
	Stats sd, sf;
	for (const char* h : hard) { check_text(h, sd, sf, true); }
	std::mt19937_64 rng(seed);
	for (int i = 0; i < 400 / slow; ++i) { for (int k = 0; k < 7; ++k) { check_text(long_string(rng, k), sd, sf, true); } }
	syntax_exhaustive(sd, sf, 5);
	huge_exponent_cases(sd, sf);
	// every binade's halfway points: doubles
	for (int e = -1074; e <= 971; e += slow) {
		const std::uint64_t base = e == -1074 ? 1ULL << 51 : 1ULL << 52;
		const std::uint64_t ms[] = {base, base + 1, (1ULL << 53) - 1, base + (rng() % (1ULL << 52)), (1ULL << 52) + (rng() % (1ULL << 52))};
		for (std::uint64_t m : ms) {
			if (e == -1074 && m >= (1ULL << 52)) { continue; }
			midpoint_cases(m, e, rng, sd, sf, e % 7 == 0);
		}
	}
	// subnormal midpoints
	for (int i = 0; i < 300 / slow; ++i) { midpoint_cases(1 + rng() % ((1ULL << 52) - 1), -1074, rng, sd, sf, false); }
	// floats
	for (int e = -149; e <= 104; e += slow) {
		const std::uint64_t base = e == -149 ? 1ULL << 22 : 1ULL << 23;
		const std::uint64_t ms[] = {base, base + 1, (1ULL << 24) - 1, base + (rng() % (1ULL << 23))};
		for (std::uint64_t m : ms) {
			if (e == -149 && m >= (1ULL << 23)) { continue; }
			midpoint_cases(m, e, rng, sd, sf, false);
		}
	}
	random_work(seed, 200000 / slow, sd, sf, true);
	rounding_and_locale();
	std::printf("  double: %ld checks, %ld taken by the fast path (%.1f%%)\n", sd.checks, sd.accepted, 100.0 * sd.accepted / sd.checks);
	std::printf("  float:  %ld checks, %ld taken by the fast path (%.1f%%)\n", sf.checks, sf.accepted, 100.0 * sf.accepted / sf.checks);
	// The plain formats must really use it: a regression that declines everything would still be "correct".
	Stats pd, pf;
	std::mt19937_64 r2(seed + 5);
	for (int i = 0; i < 20000; ++i) {
		const double d = std::ldexp(static_cast<double>(r2() >> 11), -static_cast<int>(r2() % 60));
		char b[64];
		std::snprintf(b, sizeof b, "%.17g", d);
		check_fast<double>(b, pd, false);
		std::snprintf(b, sizeof b, "%.9g", static_cast<float>(d));
		check_fast<float>(b, pf, false);
	}
	if (pd.accepted < pd.checks * 99 / 100 || pf.accepted < pf.checks * 99 / 100) {
		FAIL("fast path acceptance too low: double %ld/%ld float %ld/%ld", pd.accepted, pd.checks, pf.accepted, pf.checks);
	}
}

static void stress(std::uint64_t seed, long n, int threads) {
	std::vector<std::thread> ts;
	std::vector<Stats> sds(threads), sfs(threads);
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			random_work(seed * 1000003 + t, n / threads, sds[t], sfs[t], true);
			std::mt19937_64 rng(seed + 77 * t);
			for (int e = -1074 + t; e <= 971; e += threads) {
				for (int k = 0; k < 40; ++k) {
					const std::uint64_t base = e == -1074 ? 1ULL << 51 : 1ULL << 52;
					midpoint_cases(base + (rng() % base), e, rng, sds[t], sfs[t], false);
				}
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats sd, sf;
	for (int t = 0; t < threads; ++t) { sd.add(sds[t]); sf.add(sfs[t]); }
	std::printf("  double: %ld checks, %.2f%% taken by the fast path\n", sd.checks, 100.0 * sd.accepted / sd.checks);
	std::printf("  float:  %ld checks, %.2f%% taken by the fast path\n", sf.checks, 100.0 * sf.accepted / sf.checks);
}

static void f32_exhaustive(int threads) {
	std::vector<std::thread> ts;
	std::vector<Stats> sds(threads), sfs(threads);
	std::atomic<std::uint64_t> next{0};
	constexpr std::uint64_t chunk = 1ULL << 20;
	for (int t = 0; t < threads; ++t) {
		ts.emplace_back([&, t] {
			char b[64];
			for (;;) {
				const std::uint64_t lo = next.fetch_add(chunk);
				if (lo >= (1ULL << 32)) { break; }
				for (std::uint64_t i = lo; i < lo + chunk; ++i) {
					const std::uint32_t bits = static_cast<std::uint32_t>(i);
					float f;
					std::memcpy(&f, &bits, 4);
					if (!std::isfinite(f)) { continue; }
					// shortest round-trip text
#ifdef FP_TEST_FROM_CHARS
					auto r = std::to_chars(b, b + sizeof b, f);
					*r.ptr = '\0';
#else
					for (int p = 1; p <= 9; ++p) { std::snprintf(b, sizeof b, "%.*g", p, static_cast<double>(f)); if (std::strtof(b, nullptr) == f) { break; } }
#endif
					{
						float g = 0;
						std::string s(b);
						if (D::fast_fp<float>(g, s) && !bits_equal(g, f)) { FAIL("float shortest \"%s\" did not round trip: %a vs %a", b, static_cast<double>(g), static_cast<double>(f)); }
						check_fast<float>(s, sfs[t], false);
						check_fast<double>(s, sds[t], false);
					}
					std::snprintf(b, sizeof b, "%.17g", static_cast<double>(f));
					{
						std::string s(b);
						check_fast<float>(s, sfs[t], false);
						check_fast<double>(s, sds[t], false);
					}
					std::snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
					check_fast<float>(b, sfs[t], false);
				}
			}
		});
	}
	for (auto& t : ts) { t.join(); }
	Stats sd, sf;
	for (int t = 0; t < threads; ++t) { sd.add(sds[t]); sf.add(sfs[t]); }
	std::printf("  float32 sweep: double parses %ld (%.2f%% fast), float parses %ld (%.2f%% fast)\n", sd.checks, 100.0 * sd.accepted / sd.checks, sf.checks, 100.0 * sf.accepted / sf.checks);
}

int main(int argc, char** argv) {
	std::uint64_t seed = 1;
	long stress_n = 0;
	int threads = 4;
	bool f32 = false;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) { seed = std::strtoull(argv[++i], nullptr, 10); }
		else if (!std::strcmp(argv[i], "--stress") && i + 1 < argc) { stress_n = std::strtol(argv[++i], nullptr, 10); if (i + 1 < argc && argv[i + 1][0] != '-') { threads = std::atoi(argv[++i]); } }
		else if (!std::strcmp(argv[i], "--f32")) { f32 = true; if (i + 1 < argc && argv[i + 1][0] != '-') { threads = std::atoi(argv[++i]); } }
	}
	if (stress_n > 0) {
		std::printf("stress: %ld random values per format family, seed %llu, %d threads\n", stress_n, static_cast<unsigned long long>(seed), threads);
		stress(seed, stress_n, threads);
	} else if (f32) {
		std::printf("float32 exhaustive, %d threads\n", threads);
		f32_exhaustive(threads);
	} else {
		std::printf("strict_stox floating point fast path\n");
		quick(seed);
	}
	if (failures.load() != 0) {
		std::fprintf(stderr, "%ld failures\n", failures.load());
		return 1;
	}
	std::printf("strict_stox fp OK\n");
	return 0;
}
