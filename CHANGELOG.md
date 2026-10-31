# Changelog

All notable changes to this library. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The library has no
release tags yet, so entries are dated.

## 2026-10-31

### Documentation

- The README table is measured on an Apple M4 Pro (Apple clang 17, `-O3`) at `6d8731f`,
  with the benchmark command and its fast_float flags. It replaces the 2026-10-30
  figures on an unrecorded Apple-silicon machine: `stoi` 6.0, `stoull` 6.5, `stoul`
  base 16 4.7, short-decimal `stod` 10.2 and 17-digit `stod` 24.3 ns (`std::from_chars`
  7.0, 8.5, 10.5 and 15.6; `fast_float` 7.5 on short decimals). The same M4 run
  measures the previous header (`test/strict_stox_old.hh`) at 19.8, 27.4, 27.0, 19.6
  and 26.3 ns on those five rows.

### Fixed

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
