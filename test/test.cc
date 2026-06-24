// Smoke test for the standalone strict-stox library.
//
// Exercises strict_stox.hh: strict numeric parsing over the C strto* family. The
// happy paths, the throwing error cases (which now raise std::invalid_argument /
// std::out_of_range instead of Xapiand's THROW macros), the nothrow variant that
// reports via errno_save, and the idx prefix-parse variant.
//
// Build via CMake: cmake -B build && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <stdexcept>

#include "strict_stox.hh"


// ---------------------------------------------------------------------------
// Happy paths: a valid number that consumes the whole input.
// ---------------------------------------------------------------------------

static void test_happy_path() {
	assert(strict_stoi("42") == 42);
	assert(strict_stoi("-7") == -7);
	assert(strict_stoul("100") == 100UL);
	assert(strict_stoll("9000000000") == 9000000000LL);
	assert(strict_stod("3.5") == 3.5);

	// Base argument is honored (0x1f == 31).
	assert(strict_stoul("1f", nullptr, 16) == 31UL);

	std::printf("strict_stox happy-path OK: stoi/stoul/stoll/stod + base 16\n");
}


// ---------------------------------------------------------------------------
// Throwing error cases: invalid input and out-of-range value.
// ---------------------------------------------------------------------------

static void test_throwing() {
	// Trailing garbage with no idx is rejected (strict: must consume the whole input).
	bool threw = false;
	try {
		(void)strict_stoi("42abc");
	} catch (const std::invalid_argument&) {
		threw = true;
	}
	assert(threw && "trailing garbage throws invalid_argument");

	// Empty / non-numeric input throws invalid_argument.
	threw = false;
	try {
		(void)strict_stoi("notanumber");
	} catch (const std::invalid_argument&) {
		threw = true;
	}
	assert(threw && "non-numeric throws invalid_argument");

	// Out-of-range value throws out_of_range. strict_stoi narrows to int, so a
	// value well past INT_MAX overflows the int range check.
	threw = false;
	try {
		(void)strict_stoi("99999999999999999999");
	} catch (const std::out_of_range&) {
		threw = true;
	}
	assert(threw && "overflow throws out_of_range");

	std::printf("strict_stox throwing OK: invalid_argument + out_of_range\n");
}


// ---------------------------------------------------------------------------
// The nothrow + idx variants.
// ---------------------------------------------------------------------------

static void test_nothrow_and_idx() {
	// The nothrow variant reports via errno_save instead of throwing.
	int err = 0;
	auto v = strict_stoi(&err, "nope");
	assert(err == EINVAL && v == 0);

	// A happy-path nothrow parse leaves errno_save clear.
	err = -1;
	auto ok = strict_stoi(&err, "55");
	assert(err == 0 && ok == 55);

	// idx variant: parse a prefix and report how many chars were consumed.
	std::size_t idx = 0;
	auto n = strict_stoi("123rest", &idx);
	assert(n == 123 && idx == 3);

	std::printf("strict_stox nothrow/idx OK: errno_save reporting + prefix parse\n");
}


int main() {
	test_happy_path();
	test_throwing();
	test_nothrow_and_idx();
	std::printf("all strict-stox tests passed\n");
	return 0;
}
