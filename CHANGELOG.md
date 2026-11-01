# Changelog

All notable changes to this library. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The library has no
release tags yet, so entries are dated.

## 2026-11-01

### Added

- **The independent harness and its record.** `tools/verify.cc` (checks `stod` and `stof` against libc,
  `std::from_chars`, `fast_float` and the previous header, in all four rounding modes, on texts of 17
  formats, exact halfway points, every `float` and every short syntax string), `tools/verify_chain.sh`
  (the commands in sequence), `tools/verify_mutants.py` (does the harness catch the mutants of
  `tools/fp_mutants.py`) and `.github/workflows/verify.yml` (the sweeps and the mutation check on four
  platforms, started by hand). `docs/perf/verification.md` records what has run: 0 mismatches over about
  2.3 billion texts on each Linux runner, 1.05 billion on each macOS runner, and 112 of 112 mutants killed.
- `bench/bench.cc` has rows for input that never takes the fast path or sits at the edge: integer text with white
  space, a sign, a prefix, base 0, no digits and an out-of-range value, and `stod` / `stof` of text with white space,
  `inf` and `nan`, and values near `DBL_MAX` and `FLT_MAX`.
- In the tables below, M4 is the Apple M4 Pro, M1 the Apple M1 Pro and i9 the Intel Core i9-9980HK.
- **An exact path for decimal `stod` and `stof` text, so libc is reached only for what it alone
  defines.** `fast_fp` declined every text its 64-bit table could not decide (about one 17 digit
  text in a thousand) and every subnormal, zero or infinite result, and `strtod` took them.
  `fp_slow` now converts them after `fast_fp` declines, with the method of the slow path of
  Lemire's "Number Parsing at a Gigabyte per Second": the exact decimal value is compared with
  the halfway point between two floats. The table gives a mantissa the rounded value cannot be
  below (the product is a lower bound, within 2^-59 of the value); one exact comparison decides
  whether the result is that mantissa or the next, and a tie goes to the even one. Texts of at
  most 19 digits with `|q| <= 27` are compared in 128 bits, the others in fixed 48-limb big
  integers; a text of more than 768 significant digits keeps the first 768 and a final digit 1
  (a halfway point has at most 767 digits, so none lies between the two). A result below the
  point where it rounds up to the smallest normal value is `ERANGE` without arithmetic unless it
  could be exactly representable, which needs `5^-q` to divide the digits, so 713 digits or more
  for a `double` (86 for a `float`); the rest is divided. `fast_fp` is untouched: its generated
  code is identical to the previous header's (clang 17 and 21 on arm64 and x86_64, instruction
  streams compared with labels and numbers stripped).

  What still reaches libc, besides what is not decimal text (hex floats, `inf`, `nan`, whitespace,
  `+`, trailing text), a rounding mode other than to-nearest and a decimal point other than `.`:
  an exactly representable subnormal, and a text that rounds up from below the subnormal range
  into the smallest normal value, the two results whose `errno` differs between libcs (glibc sets
  `ERANGE` for a subnormal only if it is inexact, Apple's libc for every one). `ERANGE` for an
  inexact subnormal, an overflow and a zero from a non-zero text is the same on glibc, Apple's
  libc and FreeBSD's (but see Known issues).

  `test/slow_test.cc` is built with `-DSTRICT_STOX_COUNT_LIBC`, which makes `stod` and `stof` call
  a counting wrapper of libc, and checks: a table of cases with expected values from exact
  rational arithmetic (`tools/slow_cases.py`, no libc, Apple's abort text among them, which must
  not reach libc); the exact halfway point of neighbouring floats (built with the test's own
  big-integer routine) with tails of digits added or taken so the text lies just above or below;
  all 2,300 binade boundaries; the subnormal threshold with texts of 18 to 700 digits; exactly
  representable subnormals; random text of every length and exponent with `fp_slow` computed
  with and without the table and against libc; `cmp_small` against `cmp_scaled`; and that libc is
  reached only for the cases above. `tools/fp_mutants.py` has 112 mutants (61 for the exact
  path), all killed; two survivors of the first run were a branch that only a direct call can
  reach and a renormalisation that cannot matter (removed, with the argument in a comment), and
  a third (the margin around the rounding threshold) needed texts of 18 to 700 digits just
  below and above it.
  `test/locale_test.cc` now tests the decimal point guard of `fp_slow` too. `bench/bench.cc` has
  rows for the new path (exact halfway points, subnormal, overflow and underflow).

  ns per element, `bench/bench.cc` with the previous header and the new one each the only header of
  its binary (alternating runs, one measurement at a time on the host, Apple clang `-O3`; the minimum of four runs on the
  M4 and the M1, the mean of eight on the i9). "Before" is the same text through `strtod`:

  | workload | M4 before | M4 now | M1 before | M1 now | i9 before | i9 now |
  |---|---|---|---|---|---|---|
  | `stod`, exact halfway points (`2^52..2^53 + .5`) | 58.0 | 18.8 | 81.2 | 27.2 | 128.1 | 43.3 |
  | `stod`, odd integers `2^53..2^54` (halfway points) | 21.5 | 16.5 | 28.6 | 24.4 | 42.8 | 36.8 |
  | `stod`, subnormal | 26.6 | 18.4 | 45.8 | 31.8 | 66.6 | 48.7 |
  | `stod`, overflow and underflow | 30.5 | 22.0 | 49.1 | 33.4 | 60.5 | 46.8 |
  | `stof`, odd integers `2^24..2^25` (halfway points) | 34.2 | 12.2 | 52.2 | 17.8 | 73.6 | 32.4 |
  | `stof`, subnormal | 63.3 | 18.4 | 97.7 | 30.6 | 151.4 | 46.5 |
  | `stof`, overflow | 26.4 | 17.3 | 44.6 | 29.3 | 60.1 | 42.4 |

  The first form of the exact path built big integers for every text it decided and was slower
  than `strtod` on the new rows (428 ns for `stod` of subnormal text against 27, on the M4; 36
  against 21 for the odd integers); deciding from the table first, comparing in 128 bits and
  reading a short text once more with the eight-byte loads gave the figures above. The rows that
  were there before are unchanged within the spread (M4 -1.9% to +2.4%, M1 -1.4% to +3.2%, i9
  -13.6% to +4.1% on a shared host); `stod` of 800 digits is 8% to 12% faster, because its rare
  ambiguous texts no longer go to `strtod`. On the i9 two rows (`1.5e-7`-like and the shortest
  representations) stay about 4% slower in every run, inside that host's spread between identical
  binaries. Their `fast_fp` code is instruction for instruction the previous one; the wrapper around
  it is larger now that `_fast` has three results, and Apple clang no longer inlines
  `stox_nothrow` into the entry points. Moving the `strto*` fallback out of line restored the inlining and measured
  no better on the M4, so it was not kept. `fast_float` is faster on the new rows (5.7 to 11.3 ns on
  the M4): its second table makes a halfway point cheap.

  Verified with `tools/verify.cc`, an independent harness that is not part of ctest (`docs/perf/verification.md` has the runs): for every text, `stod`
  and `stof` (nothrow with and without `idx`, and the throwing form) against libc, `std::from_chars`
  where the platform has it and `fast_float`; a separate phase of each run repeats the random texts under each of the four rounding
  modes; and the exact path against itself with and without the table. The texts: random doubles and floats in 17 formats, random
  digit strings with all exponents, long mantissas, the exact halfway point between neighbouring doubles
  and floats, every short string over the syntax alphabet, and a stride through all float32's shortest, 9 and 17
  digit texts and their midpoints. No mismatch on any of: Linux x86_64 and arm64 (GitHub, glibc 2.39, GCC 13), 2.3 billion
  texts each; the VM (glibc 2.35, GCC 11, libstdc++ 11), 1.8 billion; macOS 15 on arm64 and on Intel (Apple libc), 1.05 billion each;
  and the i9 (macOS 15, LLVM 23 with libc++'s `std::from_chars`), 1.87 billion. Apple's `strtod` aborted
  on 53 of the arm64 texts and 65 of the Intel texts (trapped by the harness); the API did not reach libc for
  any of them. A shorter run behind the last refactor (the result handling out of line) covers hundreds of
  millions of texts on each of the four GitHub platforms, also without a mismatch.

### Changed

- `Stox::_fast` returns 0 (take the `strto*` path), 1 (the value) or 2 (`ERANGE`). On 2 the nothrow
  forms return 0 and store `ERANGE` without touching `idx`, and the throwing forms throw
  `std::out_of_range` with the same text, as the `strto*` path does.

### Fixed

- **Apple's libc no longer aborts on decimal text that the fast path declined.** The abort
  (`divideMPByMP`, `strtofp.c:1390`, macOS 15.7, 15.8 and 26.7) needs a text that reaches `strtod`;
  decimal text no longer does, except an exactly representable subnormal. The text
  `99004559013337762701442583720290300000000000.00000000000000` is a test case that must not reach
  libc. The 19 and 34 texts (of 340 and 102 million) on which Apple's `strtod` aborted in the
  macOS arm64 verification run were all converted without it.

### Known issues

- FreeBSD's `strtof` returns the smallest normal value (1.17549435e-38) with no `errno` for some
  texts between two subnormal floats (for example near 2.1e-44 and 4.9e-44). `stof` of those
  texts used to return that value on FreeBSD; it now gives `ERANGE`, which is right.
  `test/slow_test.cc` counts such texts as libc bugs instead of mismatches.

## 2026-10-31

### Documentation

- The README table is measured on an Apple M4 Pro (Apple clang 17, `-O3`) at `6d8731f`,
  with the benchmark command and its fast_float flags. It replaces the 2026-10-30
  figures on an unrecorded Apple-silicon machine: `stoi` 6.0, `stoull` 6.5, `stoul`
  base 16 4.7, short-decimal `stod` 10.2 and 17-digit `stod` 24.3 ns (`std::from_chars`
  7.0, 8.5, 10.5 and 15.6; `fast_float` 7.5 on short decimals). The same M4 run
  measures the previous header (`test/strict_stox_old.hh`) at 19.8, 27.4, 27.0, 19.6
  and 26.3 ns on those five rows.

### Added

- **A table-driven fast path for `stod` and `stof`.** The Clinger-only path declined
  every text of more than 17 bytes, with a mantissa above 2^53 or an exponent beyond
  1e22, so a full-precision double (17 digits, `%.17g` or JSON output) went to
  `strtod` at 23 to 48 ns. Now `[-]digits[.digits][e[+-]digits]` of any length is
  converted directly. The digits are read eight at a time; `w * 10^q` (`w` the first 19
  significant digits) is rounded from the 64 leading bits of `5^q` (a 5 KB table,
  generated by `tools/gen_pow5.py`) with one 64 x 64 bit multiply; Clinger's exact
  multiply or divide still serves `double` text with `w <= 2^53` and `|q| <= 22`
  (cheaper than the table); more than 19 digits are decided by rounding both `w` and
  `w + 1`. These take `strtod` as before: a result that would be subnormal, zero or
  infinite (where `strtod` sets `ERANGE`), a rounding the truncated table cannot decide
  (about one 17 digit text in a thousand, essentially never for `float`), `inf`, `nan`,
  hex floats, trailing text, a rounding mode other than to-nearest (`strtod` honors
  `fesetround`, the fast path does not) and a locale whose decimal point is not `.`.
  Value bits, end pointer, `errno` and exceptions are identical.

  Oracle: libc `strtod`/`strtof` and `std::from_chars` on the same bytes. `test/fp_test.cc`
  checks the table against big integers; the classic hard cases (Paxson, Java/PHP hangs,
  2^-1074, the `DBL_MIN`, `DBL_MAX` and `FLT_MAX` boundaries, 2^53 + 1, 1e23, exponents
  that wrap a 32 or 64 bit accumulator, and 7 digit exponents next to a mantissa of
  ~10^5 digits, which a clamped exponent would cancel into an in-range answer); the exact halfway point of every binade of
  `double` and `float` with its neighbours at 17 to 56 digits and at full length; 800 digit
  inputs; every string of up to 5 characters over a syntax alphabet; and random text in
  nine formats. `tools/fp_mutants.py` breaks the code in 34 ways and each must fail it.
  Long runs (`fp_test --stress N`, `--f32`), all clean: 100.9M `double` and 50.9M `float`
  random-text checks on glibc (x86_64) and on macOS arm64, and every finite `float` as
  shortest and as 17 digit text, parsed as `float` and as `double` on glibc (12.8G and 8.6G
  checks, 99.6% and 99.98% taken by the fast path).

  Floating point, ns per element (Apple M4 Pro / Intel i9-9980HK, one measurement at a time on the host, `-O3`,
  before this change, then now with the locale check, then without it, and `fast_float`):

  | workload | before | now | now, `STRICT_STOX_C_LOCALE` | `fast_float` |
  |---|---|---|---|---|
  | `stod` short | 10.4 / 28.6 | 8.9 / 18.4 | 6.0 / 13.7 | 7.7 / 15.6 |
  | `stod` 17 digits | 23.5 / 47.2 | 10.9 / 24.9 | 7.6 / 19.2 | 11.0 / 22.5 |
  | `stof` short | 22.8 / 46.7 | 9.8 / 22.7 | 7.5 / 16.7 | 10.1 / 19.2 |

- `STRICT_STOX_C_LOCALE` (a macro, and a CMake option): skip the decimal point query.
  `strtod` reads the locale's decimal point, so the fast path asks the C library
  (`nl_langinfo`, or `localeconv` where there is no `langinfo.h`) before it reads a `.`.
  That call costs 3 ns on Apple silicon and 5 ns on the Intel laptop, a third of a short
  parse. A program that never leaves the "C" locale can define the macro: plain decimals
  are then read with `.` in every locale (in a `,` locale `strict_stod("1.5")` becomes
  `1.5` instead of `1` plus trailing text; everything `strtod` handles itself still
  follows the locale). It is off by default so behavior does not change.
- `bench/bench.cc` has rows for `fast_float`, a "before" column (`-DHAVE_PREV`), `float`
  rows, 17 digit and JSON-like rows, 25 to 800 digit rows and an A/A column.
- **Tests for the rounding mode and the decimal point guard** (both were untested or
  covered by one case):
  - `test/rounding_test.cc` (`strict_stox_rounding`) runs `strict_stod` / `strict_stof`
    in each of the four rounding modes against libc (value bits, `errno`, `idx`, with and
    without `idx`; 15,524 texts per mode, negative and positive, 17 digit, 9 digit,
    long mantissas, range limits) and checks that a text taken by the fast path equals libc
    in that mode. It requires libc to round differently from nearest on thousands of the
    texts (so it fails if `fesetround` has no effect), the fast path to take 90% of the
    texts in the default mode, five hand-derived values (`0.1`, `-0.1`, `-.10`, `0.3`,
    `-0.3` as `double` and `float`, in every mode) and the mode to be per thread. The
    fast path before `199e0be` rounded `|x|` and negated, which gave `-0.1` where libc
    gives `-0.099999999999999991` in `FE_UPWARD`, in 2,358 of 30,012 random texts.
  - `test/locale_test.cc` (`strict_stox_locale_mock`, `_locale_c`, `_locale`) tests the
    guard without a comma locale installed: `STRICT_STOX_DECIMAL_POINT` is a macro for
    tests only that replaces the `nl_langinfo` call by an expression, and the mock build
    feeds it `.`, `,`, a two byte separator, `..`, `.,`, `.x` and an empty string. A text
    with a `.` must be declined unless the decimal point is exactly `.`, a text without
    one must be taken in every case, and with `STRICT_STOX_C_LOCALE` the `.` is read
    whatever the decimal point is. Where a comma locale is installed (`de_DE`, `fr_FR`, or the
    one in `STRICT_STOX_TEST_LOCALE`), `strict_stox_locale` also checks the global locale
    and a thread's own locale (`uselocale`) against libc.
  - `tools/fp_mutants.py` has three more mutants and builds each against
    the three tests; the rounding and locale mutants must be killed by their own test.
    Before, deleting the locale check passed every test on a machine with no comma
    locale.

### Changed

- `fast_fp` no longer limits the input to 17 bytes, and `float` text goes through the
  table (Clinger's `float` operation is gone).

### Fixed

- **Text of more than 19 digits was no faster than `strtod`.** After the digit scan, the
  conversion of 199e0be read the mantissa again byte by byte (four branches per digit), so
  25 to 40 digit text took 55 ns against 45 for the previous `strtod` path, and 800
  digits 1.36 us. `fp_long` now finds the decimal point with `memchr`, skips leading zeros
  eight at a time, takes the first 19 significant digits with eight-byte loads and tests the
  rest of each run for a non-zero digit eight bytes at a time. `fast_fp`, the code that short
  texts run, is the text of 199e0be, and its generated code is identical to it (clang and g++,
  x86_64 and arm64, instruction streams compared). Results are unchanged: for every text of
  the runs below, the new and the 199e0be `fast_fp` take or decline the text alike and return
  the same bits.

  ns per element, `bench/bench.cc` with 199e0be as "before" in the same process, Apple M4 Pro
  (Apple clang 17 `-O3`, one measurement at a time on the host, mean of two runs of best of 9; `fast_float` best result),
  and Intel i9-9980HK (the same compiler, 199e0be and the new header each the only
  header of its binary, mean of two runs):

  | workload | M4 199e0be | M4 now | M4 `fast_float` | i9 199e0be | i9 now | i9 `fast_float` |
  |---|---|---|---|---|---|---|
  | `stod`, 25-40 digits | 55.3 | 20.3 | 29.6 | 106.8 | 66.8 | 74.1 |
  | `stod`, 100 digits | 173.4 | 28.7 | 37.7 | 195.8 | 79.1 | 80.4 |
  | `stod`, 800 digits | 1,371 | 138 | 130 | 1,208 | 262 | 257 |
  | `stof`, 25-40 digits | 58.5 | 23.6 | 28.9 | 140.1 | 77.6 | 68.3 |

  The other rows (17 digits and fewer) are within the A/A spread: on the M4 -10.7% to +2.0%
  against 199e0be in the same process (`stod` short 8.6 -> 8.7, 1-9 digit integers 4.5 -> 4.5, 17 digits
  10.7 -> 10.8, `stof` short 9.4 -> 9.2; with `STRICT_STOX_C_LOCALE` -2.0% to +0.0%), and on the
  i9 in separate binaries -2.5% to +3.1% (`stod` short 19.2 -> 19.1, 1-9 digit integers 12.2 ->
  12.2, 17 digits 25.6 -> 25.2, 17 digits with any exponent 30.4 -> 31.4 with an A/A spread of
  3%, `stof` short 22.6 -> 22.6).

  On the Apple M1 Pro (Apple clang 21), 199e0be and the new header as the only header of
  separate binaries, three runs: rows of 17 digits and fewer -7.9% to +0.9% (`stod` short 13.0 -> 13.0,
  17 digits 17.7 -> 17.9, `stof` short 15.6 -> 15.7); with `STRICT_STOX_C_LOCALE` -11.7% to +0.0%, except
  `stof` short +6.2% (12.3 -> 13.1 ns; the three runs of that row spread from 12.1 to 13.9 ns, its A/A
  is 10%, and the same row of the default build is +0.4% with an A/A of 0.1%). Long rows
  25-40 digits 91.5 -> 35.1 ns (`fast_float` 46.2), 100 digits 249.7 -> 49.0 (57.0), 800 digits
  1,953 -> 227 (193), `stof` 25-40 digits 102.6 -> 43.8 (45.8).

  An earlier form of this change (`d611a3a`) passed the integer and fraction runs from
  `fast_fp` to `fp_long`. That added two live values to `fast_fp`, its code changed on x86_64,
  and the i9 measured several short rows 3% to 17% slower in the same process as 199e0be
  (`stod` 1-9 digit integers 11.2 -> 12.9 ns, `1.5e-7`-like 27.4 -> 28.9, short with
  `STRICT_STOX_C_LOCALE` 13.4 -> 14.7, the same in two runs) while the M4 showed nothing.
  `fast_fp` was restored to the text of 199e0be. A same-process comparison of two copies
  of the same code on the i9 is a matter of where each copy lands (the 199e0be copy measured
  11.2 ns in that process and 12.2 as the only header of a binary), so the rows above are
  compared in separate binaries. Defining `fp_long` after `fast_fp`, to give `fast_fp` the
  neighbours it has in 199e0be, made no difference and is not done.

  Verification: `tools/fp_mutants.py` has 51 mutants (18 for this path); three of them
  survived the existing tests and are now killed by two additions to `test/fp_test.cc`: a
  halfway point cut after 19 digits followed by the next digit and 0 to 23 zeros (a value just
  above the tie that only a non-zero digit at the start of an eight-byte group of zeros
  reveals), and a floor of 99.5% on the share of in-range 20 to 40 digit text that the fast
  path takes (a reader with a window of 18 digits, or that counts leading zeros as
  digits, declines more, which no correctness test can see; the share is 99.7% to 100%).

  Long runs, all with 0 mismatches: `tools/verify.cc` checks every text against libc `strtod`/`strtof` (bits, `errno`, end
  pointer, `idx`, the throwing form, in all four rounding modes), `std::from_chars`, `fast_float`
  and the previous header's `fast_fp` (same take or decline, same bits). On Linux arm64
  (glibc 2.39, g++ 13), for 199e0be and the `d611a3a` form: every positive float32 as shortest,
  `%.9g` and `%.17g` text (6.4G texts, each read as `float` and as `double`), 4.7G texts around the
  exact midpoint of every 8th float32 (the midpoint, one unit below and above it, a 1 after 30
  zeros, 40 trailing zeros, cut at 17 to 21 digits), 83.7M of the same around random doubles (up
  to 770 digits), 1.7G random texts (100M in each of 17 formats: `%.17g`, `%.15g`, shortest, `%e`,
  fixed, prices, integers, float texts, digit strings with any exponent, mantissas of 20 to 800
  digits), 425M more in all four rounding modes, every string of up to 7 characters over `019-+ .eE`
  (5.4M) and the hard cases (Paxson, 2^-1074, `DBL_MAX`, the overflow midpoint, 800 digits,
  exponents of 20 digits). Linux x86_64 (glibc 2.39, g++ 13) ran the same chain; macOS arm64
  15.8 and x86_64 15.7 (Apple libc) every 3rd float32 for the sweep (2.1G texts) and every 32nd
  for the midpoints (1.2G), 30M texts in each of the formats 0 to 14 and 60M in 15 and 16, 24M in
  the rounding modes, and 41.8M double midpoints; the Intel i9 (macOS 15.8, libc++ 23) 1.5G
  random texts in the formats 0 to 14. For the final form (`fast_fp` unchanged, `fp_long` with
  `memchr`), on Linux arm64: the chain of the long-text formats, 41.8M double midpoints and 1.2G
  float32 midpoints against all four oracles, and 1.7G random texts and 9.5G midpoint texts
  compared with 199e0be's `fast_fp` (0 differences). The commit message of `d611a3a` said that
  the full float32 sweeps ran on macOS arm64; they ran on Linux arm64 and x86_64, and the macOS
  runners took every 3rd float.

- **Input of any length parses.** The wrapper copied the text into a 64-byte
  buffer, and a later check treated the cut as trailing junk. Valid input of 64
  bytes or more (100 leading zeros, long whitespace, a long decimal string) was
  rejected as `EINVAL`, or with an `idx` parsed as its first 63 bytes with a
  wrong `idx`. The whole input is copied into a NUL-terminated buffer (128-byte
  stack buffer, heap above that) and parses exactly like libc `strto*` on the
  same bytes: value, end pointer, `errno` and the strict trailing-junk rule. The
  fast path still handles only short clean input.

  Oracle: libc `strto*` on the same bytes, and `std::from_chars` for decimal
  doubles. `test/long_test.cc` runs every string up to five characters over a
  mixed alphabet, each also padded with 62 to 200 zeros or spaces across the old
  63-byte and the 128-byte buffer boundaries, plus long random strings (13.9M
  checks); it fails on the old header.
- `test/long_test.cc` is portable to libstdc++ 11 and GCC: it compares
  `long double` by value, skips `std::from_chars` on text over 400 bytes, and
  includes the headers the frozen old header needs.

### Known issues

- **Apple's libc aborts in `strtod` on some valid text.** On macOS 15.7, 15.8 and 26.7,
  `strtod("99004559013337762701442583720290300000000000.00000000000000")` fails an
  assertion (`divideMPByMP`, `strtofp.c:1390`) and kills the process; `strtof` is fine.
  Random digit strings and long mantissas trigger it about once in 2 million texts (29 in
  60M, the same 29 on x86_64 and arm64). `strict_stod` takes this text on the fast path and
  returns its value, so it does not abort; a text that the fast path declines goes to
  `strtod` and can. The previous header called `strtod` for every text of more than 17 bytes.

## 2026-10-30

### Added

- **A clean-input fast path.** Each call copied the text into a 64-byte buffer,
  saved and restored `errno` three times, and went through the locale-aware
  `strto*`. Plain input skips all of that: an optional `-` and digits of the base
  (no overflow, whole input, under 64 bytes) for the integer functions, and
  `[-]digits[.digits][e[+-]digits]` with a mantissa of at most 2^53 (2^24 for
  `float`) and a decimal exponent within +-22 (+-10) for `stof`/`stod`, which is
  one correctly rounded IEEE operation (Clinger's fast path). Everything else
  takes the unchanged `strto*` path, so results, `errno` values, `idx` and
  exceptions are identical.
- `test/diff_test.cc` compares every function against the frozen previous header
  (`test/strict_stox_old.hh`) on random and edge inputs, values bitwise.
- `bench/bench.cc` (`-DSTRICT_STOX_BUILD_BENCH=ON`).

  Apple silicon, Apple clang 17, `-O3`, ns per element (`std::from_chars` for
  reference):

  | workload | before | after | `std::from_chars` |
  |---|---|---|---|
  | `stoi`, 1-9 digits | 19.5 | 6.0 | 7.0 |
  | `stoull`, 1-19 digits | 26.6 | 6.5 | 8.5 |
  | `stoul`, base 16 | 25.8 | 4.7 | 10.5 |
  | `stod`, short decimals | 19.0 | 10.2 | 15.6 (`fast_float` 7.5) |
  | `stod`, 17-digit doubles (`strto*` path) | 26.2 | 24.3 | |
