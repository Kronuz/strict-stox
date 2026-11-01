# strict-stox: verification of the floating point path

Run 2026-10-31 and 2026-11-01 at `strict-stox:d5d133d` unless a row names another commit.
The harness is [`tools/verify.cc`](../../tools/verify.cc); the runs below are the evidence behind
the claim in the README that `stod` and `stof` return what libc returns.

## What is checked

`tools/verify.cc` shares no code with `test/fp_test.cc`: it has its own text generators, its own big
integer for exact halfway points and its own oracles. For each text, and for `double` and `float`
(in each rounding mode that is asked for):

| category | what must hold |
|---|---|
| FAST | when `fast_fp` accepts the text, its bits are libc's `strtod` / `strtof`, libc sets no `errno` and consumed the whole text |
| API | `strict_stod` / `strict_stof` (nothrow, with and without `idx`) equal libc under the strict rules: value bits (0 on error), `errno`, `idx`; the caller's `errno` survives |
| THROW | every 16th text also the throwing form: `invalid_argument` for `EINVAL`, `out_of_range` for `ERANGE`, nothing otherwise |
| ORACLE | where libc, `std::from_chars` and `fast_float` all give a value for the whole text, they agree with each other and with the fast path (a disagreement between oracles is reported apart: it is a libc or library bug, not strict-stox's) |
| AB | with `--ab`, the previous revision of the header and the current one take or decline the same texts with the same bits |
| SLOW | the exact path (`fp_slow`) gives the same answer with the table and without it, and stands aside in a directed rounding mode |
| REACH | with `-DSTRICT_STOX_COUNT_LIBC=1`, decimal text reaches libc only for the results whose `errno` differs between libcs (an exactly representable subnormal, the smallest normal value) |

A libc that aborts inside `strtod` (Apple's does) is trapped: the text is printed, counted as a libc
bug and not as a mismatch.

The texts: random doubles and floats printed in 17 formats (`%.17g`, `%.15g`, shortest, `%e`, fixed, prices,
integers, float texts, digit strings with any exponent, mantissas of 20 to 800 digits); the exact decimal
midpoint between neighbouring doubles and floats and the binary range limits, each with tails of digits that
put the text just above or below the tie; every finite `float` as shortest, `%.9g` and `%.17g` text; every
string of up to 7 characters over `019-+ .eE`; and the hard cases (Paxson, 2^-1074, `DBL_MAX`, the overflow
midpoint, 800 digits, exponents of 20 digits).

## Method

```sh
c++ -std=c++20 -O2 -Wno-ignored-attributes -I. -I<fast_float>/include -DHAVE_FAST_FLOAT \
    -DHAVE_FROM_CHARS -DHAVE_TO_CHARS -DSTRICT_STOX_COUNT_LIBC=1 tools/verify.cc -o verify -pthread
sh tools/verify_chain.sh ./verify out THREADS RAND_N MODES_N F32_STRIDE F32MID_STRIDE DMID_N --fc-max 40
```

`verify_chain.sh` runs `hard`, `syntax`, `dmid`, `rand`, `modes`, `f32` and `f32mid`, one log each (`CHAIN_SET=long`
runs only the digit-string formats 14 to 16, for a change confined to the code for more than 19 digits).
`-DHAVE_FROM_CHARS` and `-DHAVE_TO_CHARS` need a standard library that has the floating point forms.
[`.github/workflows/verify.yml`](../../.github/workflows/verify.yml) runs the chain on four GitHub Actions
platforms, and `tools/fp_mutants.py` on three shards, by hand (`workflow_dispatch`); the sizes in it give about
2.3 billion texts on each Linux runner and 1.05 billion on each macOS runner.

## Results

Every run: 0 mismatches. "Texts" counts a text once per type.

| platform | what ran |
|---|---|
| GitHub Actions Linux arm64, glibc 2.39, g++ 13 | every positive `float` as three texts (6.4 billion texts, read as `float` and as `double`); 4.7 billion midpoint variants of every 8th `float`; 83.7 million double midpoints; 1.7 billion random texts (100 million in each of 17 formats); 425 million in all four rounding modes; the hard cases and the syntax strings; both headers against libc and each other. Exact decimal path (`d5d133d`): 2.3 billion texts |
| GitHub Actions Linux x86_64, glibc 2.39, g++ 13 | the same chain as arm64; exact decimal path: 2.3 billion texts |
| GitHub Actions macOS arm64 (15.8) and macOS Intel (15.7), Apple libc | hard cases, syntax strings, 41.8 million double midpoints, 30 million texts in each of the formats 0 to 14 and 60 million in 15 and 16, 24 million in the rounding modes, every 3rd `float` (2.1 billion texts), every 32nd midpoint (1.2 billion); exact decimal path: 1.05 billion texts each |
| Intel Core i9-9980HK, macOS 15.8, LLVM 23 with libc++ `from_chars` | hard cases, syntax strings, 1.5 billion random texts in the formats 0 to 14; exact decimal path: 1.87 billion texts |
| shared Linux x86-64 VM, glibc 2.35, g++ 11 (and clang 12) | exact decimal path: 1.78 billion texts with g++ 11; six of seven phases with clang 12; `slow_test` under ASan and UBSan (g++ 11) and TSan (clang 12) |
| GitHub Actions Linux arm64, `88cb8bb` (the final form of the long-text reader) | the long-text formats (90 million, 30 million in the rounding modes), 41.8 million double midpoints and 1.2 billion `float` midpoint texts against libc, `from_chars`, `fast_float` and the previous header; 1.7 billion random texts and 9.5 billion midpoint texts compared with the previous header without libc: 0 differences |

### Defects of other libraries the harness found

- Apple's libc `strtod` aborts on valid text on macOS 15.7, 15.8 and 26.7:
  `99004559013337762701442583720290300000000000.00000000000000` (`divideMPByMP`, `strtofp.c:1390`); `strtof` is
  fine. 29 of 60 million texts of formats 15 and 16 trigger it, the same 29 on arm64 and Intel. The exact decimal
  path converts these texts without libc, and `test/slow_test.cc` has this text as a case that must not reach it.
- libstdc++ 11.2's `std::from_chars(double)` asserts on long input.
- FreeBSD's `strtof` returns the smallest normal value with no `errno` for four texts near 2.1e-44 and 4.9e-44.
  `slow_test` counts them as `libc_bugs`.

## Proof that the checks can fail

- `tools/fp_mutants.py` plants 112 bugs (61 in the exact path) and builds each against `fp_test`, `rounding_test`,
  `slow_test` and the mock build of `locale_test`: all are killed (a GitHub Actions Linux run, g++). Two cases of
  `test/fp_test.cc` kill the mutants of the long-text reader that a plain comparison with libc cannot reach
  (`long_window_18`, `skip_zeros_shift`, `any_nonzero_first_byte`): a halfway point cut after 19 digits followed by
  the next digit and 0 to 23 zeros (a value just above the tie, which only a non-zero digit at the start of an
  eight-byte group of zeros reveals), and a 99.5% floor on the share of in-range 20 to 40 digit text that the
  fast path takes (a reader that declines more is correct and slow, which no correctness test sees).
- `tools/verify_mutants.py` builds `verify.cc` against each of those mutants and runs `hard`, `syntax`, `dmid` and
  `rand`. It kills every mutant except the ones that only make the reader decline more (the text then goes to libc,
  which is right) and the locale mutants (the harness never changes the locale); `fp_test`, `locale_test` and
  `slow_test` kill those.
- Rounding mode: on the header before the guard (`6d8731f`), 2,358 of 30,012 random texts differ from libc in
  `FE_UPWARD` and 2,448 in `FE_DOWNWARD`; with the guard, none. `test/rounding_test.cc` fails against that header
  (`[upward] stod("-0.1") bits bfb999999999999a, libc bfb9999999999999`); the mutants `no_rounding_guard` and
  `rounding_guard_neg_only` are killed by it alone.
- Locale guard: with the guard line deleted, `strict_stox_locale_mock` fails while `fp_test`, `diff_test` and
  `long_test` pass on a runner with no comma locale; `no_locale`, `locale_first_byte_only` and `locale_always_checked`
  are killed by the mock test alone.
- The generated code of `fast_fp` is identical before and after the long-text and exact-path changes: the
  instruction streams (labels and numbers stripped) are the same on clang 17 and 21 and g++ 13, arm64 and x86_64.

## Timing evidence

The tables are in the README and `CHANGELOG.md`; this is what decided the form of the code.

- The first form of the long-text reader (`d611a3a`) took the integer and fraction runs from `fast_fp`. On the
  i9 it made short rows 3% to 17% slower in the same process as the previous header (`stod` 1-9 digit integers 11.2
  to 12.9 ns); the M4 showed nothing. `fast_fp` was restored and `fp_long` finds the decimal point itself. In one
  process two copies of the same code differ by where each lands (the previous header measured 11.2 ns in that
  process and 12.2 as the only header of its binary), so the rows are compared as the only header of separate
  binaries.
- Placement (i9, one hold, each header the only one of its binary): the previous header (A), the final one (Y) and
  the final one with `fp_long` defined after `fast_fp` (Z). Y against A: -2.5% to +3.1% on every row of 17 digits
  and fewer (17 digits with any exponent +3.1% with an A/A spread of 3%); Z is no better.
- Exact path: the first form built big integers for every text it decided and was slower than `strtod` on the new
  rows (428 ns for `stod` of subnormal text against 27 on the M4). Deciding from the table first, comparing in 128
  bits and reading a short text once more with the eight-byte loads gives 18.8 ns for exact halfway points, 18.4 for
  subnormals and 12.2 for `float` odd integers on the M4.
- Rows that were there before the exact path: M4 -1.9% to +2.4%, M1 -1.4% to +3.2%, i9 -13.6% to +4.1% on a
  shared host. Two i9 rows stay about 4% slower in every run while `fast_fp` is instruction for instruction the
  same: the wrapper around it is larger now that `_fast` has three results, and Apple clang no longer inlines
  `stox_nothrow` into the entry points.

## Decisions and open items

- Moving the `strto*` fallback of `stox_nothrow` and `stox_throw` into `noinline` helpers restores the inlining but
  makes the entry points larger and the old rows slower on the M4 (+1% to +4.7%); it is not done.
- `fast_float` is faster than strict-stox on the exact-tie rows (5.7 to 11.3 ns on the M4): its second table makes a
  halfway point cheap. Open.
- The harness runs in the C locale; the locale guard is checked by `test/locale_test.cc`, not by the sweeps.
- The macOS runs trap Apple's `strtod` aborts, so those texts are checked against `fast_float` and `from_chars`
  where libc has no answer.
