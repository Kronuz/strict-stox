# strict-stox performance

`bench/bench.cc` times the parsers in ns per element over a pool of 1024 inputs, the best of
9 samples, one measurement at a time on the host. `-O3`, Apple clang 17, on an Apple M4 Pro
(arm64) and an Intel Core i9-9980HK (x86_64). The comparators are `std::from_chars`,
`fast_float` 3.10.1 (`6a3fabc`) and abseil when it is installed. The `fast%` column of the
benchmark is the share of a row's inputs that the fast path takes: a row whose inputs mostly
go to `strtod` measures `strtod`.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON \
  -DSTRICT_STOX_FAST_FLOAT_DIR=/path/to/fast_float/include
cmake --build build --target strict_stox_bench
./build/strict_stox_bench        # integers
./build/strict_stox_bench fp     # floating point
```

`-DHAVE_PREV` adds a column for a saved older header (`strict_stox_prev.hh` on the include
path; see the top of `bench/bench.cc`).

[verification.md](verification.md) has the correctness runs of the floating point path.

## Floating point

Mean of two runs (one for the i9 C locale column). `fast_float` moves by up to 10% between
builds, so its best result is shown. The C locale column is built with
`-DSTRICT_STOX_C_LOCALE`, which skips the decimal point query (3 ns on the M4 Pro, 5 ns on
the i9).

| workload | strict-stox | C locale | `fast_float` |
|---|---|---|---|
| **Apple M4 Pro** | | | |
| `stod`, short (`12345.678`) | 8.7 | **5.9** | 7.5 |
| `stod`, 17 digits (`%.17g`) | 10.8 | **7.2** | 10.6 |
| `stod`, shortest repr (JSON) | 10.9 | **8.5** | 10.4 |
| `stod`, 25-40 digits | 20.3 | **18.6** | 29.6 |
| `stod`, 100 digits | 28.7 | **27.1** | 37.7 |
| `stod`, 800 digits | 138 | 134 | **130** |
| `stof`, short | 9.2 | **7.5** | 9.8 |
| `stof`, 9 digits | 6.8 | **5.8** | 7.6 |
| `stof`, 25-40 digits | 23.6 | **22.9** | 28.9 |
| **Intel i9-9980HK** | | | |
| `stod`, short | 19.1 | **13.4** | 15.3 |
| `stod`, 17 digits | 25.2 | **19.9** | 23.2 |
| `stod`, shortest repr (JSON) | 32.9 | **24.0** | 27.8 |
| `stod`, 25-40 digits | 66.8 | **58.8** | 74.1 |
| `stod`, 100 digits | 79.1 | **69.8** | 80.4 |
| `stod`, 800 digits | 262 | 257 | 257 |
| `stof`, short | 22.6 | **17.5** | 20.2 |
| `stof`, 9 digits | 20.4 | **16.7** | 18.0 |
| `stof`, 25-40 digits | 77.6 | 69.6 | **68.3** |

## The exact path

Text the table cannot decide (an exact halfway point) and text `strtod` answers with `ERANGE`
(subnormal, overflow, underflow) go through the exact path. The `strtod` column is the same text
through `strtod` (a saved header without the exact path, `-DHAVE_PREV`). `fast_float` is faster
here: its second table makes a halfway point cheap. The M4 Pro column is the minimum of four
runs, the i9 column the mean of eight:

| workload | strict-stox | `strtod` | `fast_float` |
|---|---|---|---|
| **Apple M4 Pro** | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 18.8 | 58.0 | 7.3 |
| `stod`, odd integers `2^53..2^54` (halfway points) | 16.5 | 21.5 | 7.6 |
| `stod`, subnormal | 18.4 | 26.6 | 9.1 |
| `stod`, overflow and underflow | 22.0 | 30.5 | 10.3 |
| `stof`, odd integers `2^24..2^25` (halfway points) | 12.2 | 34.2 | 5.7 |
| `stof`, subnormal | 18.4 | 63.3 | 8.7 |
| `stof`, overflow | 17.3 | 26.4 | 11.3 |
| **Intel i9-9980HK** | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 43.3 | 128.1 | 18.8 |
| `stod`, odd integers `2^53..2^54` (halfway points) | 36.8 | 42.8 | 17.6 |
| `stod`, subnormal | 48.7 | 66.6 | 21.5 |
| `stod`, overflow and underflow | 46.8 | 60.5 | 21.1 |
| `stof`, odd integers `2^24..2^25` (halfway points) | 32.4 | 73.6 | 13.9 |
| `stof`, subnormal | 46.5 | 151.4 | 21.8 |
| `stof`, overflow | 42.4 | 60.1 | 23.2 |

## Declined and failing text

Text the fast path declines or that fails, and text at the top of the range. The first column is
the frozen header that sends every text to `strto*` (`test/strict_stox_old.hh`), so a declined
text pays the attempt on top of `strtod`: 4 to 5 ns on the M4 Pro and 9 to 11 ns on the i9 for
leading white space, `inf` and `nan`. One run; two runs of the strict-stox column differ by 0%
to 7% (the `stoll` 20 digit row on the i9, 10%):

| workload | M4 Pro: `strto*` only / strict-stox | i9-9980HK: `strto*` only / strict-stox |
|---|---:|---:|
| `stoll`, `" 42"` (white space) | 16.8 / 15.0 | 40.6 / 39.7 |
| `stoll`, `"abc"` (`EINVAL`) | 12.4 / 10.4 | 28.6 / 24.5 |
| `stoll`, 20 digits (`ERANGE`) | 28.1 / 32.6 | 66.3 / 75.9 |
| `stod`, `" 1.5"` (white space) | 19.0 / 23.7 | 42.1 / 50.8 |
| `stod`, `inf` and `nan` | 17.4 / 21.6 | 30.9 / 41.1 |
| `stod`, near `DBL_MAX` | 24.6 / 10.2 | 54.8 / 28.2 |
| `stof`, `" 1.5"` (white space) | 20.1 / 23.9 | 47.3 / 55.9 |
| `stof`, `inf` and `nan` | 16.9 / 21.9 | 32.0 / 43.2 |
| `stof`, near `FLT_MAX` | 20.4 / 9.4 | 47.6 / 26.1 |

## Integers

Apple M4 Pro, `-O3 -DNDEBUG -std=gnu++20`, mean of two runs:

| workload | strict-stox | `std::from_chars` |
|---|---|---|
| `stoi`, 1-9 digits | 6.4 | 7.2 |
| `stoull`, 1-19 digits | 6.5 | 8.8 |
| `stoul`, base 16 | 5.3 | 11.0 |
