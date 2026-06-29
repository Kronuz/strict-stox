// A runnable tour of strict-stox.
//
// Build (when this repo is the top-level project):
//   cmake -B build && cmake --build build && ./build/strict_stox_demo
//
// The one idea worth taking away: strictness is the whole point. std::stoi and
// strtol quietly accept input that is only PARTLY a number ("42abc" -> 42) and
// leave overflow to errno you have to remember to check. strict_stoi with no idx
// demands the entire string be a number, throws on overflow, and offers a nothrow
// errno_save form. This demo parses the valid inputs, then runs the loose stdlib
// parsers and the strict ones side by side on the inputs the loose ones wave
// through, so you can see exactly where they diverge.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "strict_stox.hh"

static void rule(const char* title) {
	std::printf("\n\033[1m── %s ──\033[0m\n", title);
}

// What std::stoi does with an input: either a value, or the word it throws.
static std::string loose_stoi(const char* s) {
	try {
		std::size_t pos = 0;
		int v = std::stoi(s, &pos);
		// std::stoi ignores everything past the number, so report what it kept.
		return "accepted -> " + std::to_string(v) +
		       (s[pos] ? "  (ignored trailing \"" + std::string(s + pos) + "\")" : "");
	} catch (const std::invalid_argument&) {
		return "invalid_argument";
	} catch (const std::out_of_range&) {
		return "out_of_range";
	}
}

// What strict_stoi does with the same input, throwing form.
static std::string strict(const char* s) {
	try {
		return "accepted -> " + std::to_string(strict_stoi(s));
	} catch (const std::invalid_argument&) {
		return "REJECTED (invalid_argument)";
	} catch (const std::out_of_range&) {
		return "REJECTED (out_of_range)";
	}
}

int main() {
	std::puts("strict-stox demo");

	// --- 1. the valid inputs all parse --------------------------------------
	rule("happy paths: a number that consumes the whole string");
	std::printf("  strict_stoi(\"42\")               = %d\n", strict_stoi("42"));
	std::printf("  strict_stoi(\"-7\")               = %d\n", strict_stoi("-7"));
	std::printf("  strict_stoul(\"100\")             = %lu\n", strict_stoul("100"));
	std::printf("  strict_stoll(\"9000000000\")      = %lld\n", strict_stoll("9000000000"));
	std::printf("  strict_stod(\"3.5\")              = %g\n", strict_stod("3.5"));
	std::printf("  strict_stoul(\"1f\", nullptr, 16) = %lu   (base 16)\n",
	            strict_stoul("1f", nullptr, 16));

	// --- 2. the selling point: strict rejects what loose accepts --------------
	rule("loose vs strict, on the inputs the loose parsers wave through");
	// These are exactly the cases where std::stoi / strtol are too forgiving:
	// trailing junk, leading-then-junk, overflow, and empty / non-numeric input.
	const char* cases[] = {"42abc", "0x1f", "  12  ", "99999999999999999999", "", "nope"};
	std::printf("  %-24s %-40s %s\n", "input", "std::stoi", "strict_stoi");
	std::printf("  %-24s %-40s %s\n", "-----", "---------", "-----------");
	for (const char* s : cases) {
		std::string shown = std::string("\"") + s + "\"";
		std::printf("  %-24s %-40s %s\n", shown.c_str(), loose_stoi(s).c_str(), strict(s).c_str());
	}
	std::puts("  (std::stoi keeps a prefix and silently drops the rest; strict_stoi insists on all-or-nothing)");

	// --- 3. how strtol leaves the same mess behind ---------------------------
	rule("strtol: the C layer underneath, just as loose");
	// strtol parses a prefix and tells you where it stopped via endptr, but most
	// call sites never check endptr, so "42abc" silently becomes 42. Overflow only
	// shows up if you remembered to clear and re-read errno.
	const char* junk = "42abc";
	char* end = nullptr;
	errno = 0;
	long v = std::strtol(junk, &end, 10);
	std::printf("  strtol(\"42abc\") = %ld, stopped at \"%s\"  (you must check endptr yourself)\n", v, end);
	const char* big = "99999999999999999999";
	errno = 0;
	long ov = std::strtol(big, nullptr, 10);
	std::printf("  strtol(\"99999999999999999999\") = %ld, errno %s  (you must check errno yourself)\n",
	            ov, errno == ERANGE ? "= ERANGE" : "unchecked");

	// --- 4. the nothrow form, for hot / allocation-sensitive paths -----------
	rule("nothrow: report via errno_save instead of throwing");
	// First argument is an int* errno_save. On failure it returns 0 and writes
	// EINVAL / ERANGE; on success it clears errno_save. No exception machinery.
	int err = 0;
	auto bad = strict_stoi(&err, "nope");
	std::printf("  strict_stoi(&err, \"nope\") -> value %d, err = %s\n",
	            bad, err == EINVAL ? "EINVAL" : "?");
	err = -1;
	auto good = strict_stoi(&err, "55");
	std::printf("  strict_stoi(&err, \"55\")   -> value %d, err = %d  (cleared on success)\n", good, err);

	// --- 5. opting in to a prefix parse, explicitly --------------------------
	rule("idx: when you DO want a prefix, ask for it");
	// Passing an idx says "a prefix is fine, tell me how far you got". The
	// strictness is the default; the loose behavior is opt-in, not the surprise.
	std::size_t idx = 0;
	auto n = strict_stoi("123rest", &idx);
	std::printf("  strict_stoi(\"123rest\", &idx) -> value %d, consumed %zu chars (\"%s\" left)\n",
	            n, idx, "123rest" + idx);

	std::puts("\ndone.");
	return 0;
}
