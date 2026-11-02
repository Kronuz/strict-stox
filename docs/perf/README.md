# strict-stox performance

`bench/bench.cc` times the parsers in ns per number over a pool of 1,024 inputs, the best of 9 samples, one
measurement at a time on the host. `-O3`, Apple clang 17, on an Apple M4 Pro (macOS 26) and an Intel Core i9-9980HK (macOS 15). Every cell is the mean of two runs.
The columns are strict-stox, the same header built with `-DSTRICT_STOX_C_LOCALE` (a build mode that reads `.`
as the decimal point without a locale query), libc `strto*` on a NUL-terminated copy, `std::from_chars` (its
floating-point form needs macOS 26) and [fast_float v8.3.3](https://github.com/fastfloat/fast_float/tree/v8.3.3).
The `fast%` column of the benchmark is the share of a row's inputs that the fast path takes: a row whose inputs
mostly go to `strtod` measures `strtod`. Bold is the fastest implementation of a row (the C locale build is a mode of strict-stox, not counted).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON
cmake --build build --target strict_stox_bench
./build/strict_stox_bench        # integers
./build/strict_stox_bench fp     # floating point
./build/strict_stox_bench --check   # compare results, no timing
```

[verification.md](verification.md) has the correctness runs of the floating point path.

## Floating point

| Workload | strict-stox | C locale | libc | `std::from_chars` | `fast_float` |
|---|---:|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | | |
| `stod`, short (`12345.678`) | 8.8 | 6.0 | 12.2 | 14.9 | **6.4** |
| `stod`, 1 to 9 digit integers | **4.8** | 4.6 | 9.4 | 13.1 | 5.7 |
| `stod`, 17 digits (`%.17g`) | 10.7 | 7.5 | 18.8 | 25.2 | **8.4** |
| `stod`, 17 digits, any exponent | 12.1 | 9.2 | 21.0 | 44.5 | **8.5** |
| `stod`, shortest repr (JSON) | 11.1 | 8.3 | 19.4 | 56.5 | **8.7** |
| `stod`, `1.5e-7`-like | 10.6 | 7.5 | 17.6 | 23.4 | **6.6** |
| `stod`, 25 to 40 digits | **20.4** | 18.4 | 35.4 | 52.0 | 46.6 |
| `stod`, 100 digits | **28.4** | 26.8 | 92.2 | 141.9 | 122.6 |
| `stod`, 800 digits | **125.8** | 124.7 | 927.1 | 1882.2 | 499.5 |
| `stod`, near `DBL_MAX` | 11.1 | 8.2 | 16.4 | 31.3 | **7.6** |
| `stof`, short | 9.5 | 7.0 | 13.8 | 19.8 | **8.0** |
| `stof`, 9 digits | 7.3 | 6.0 | 15.2 | 162.8 | **6.2** |
| `stof`, 25 to 40 digits | **22.5** | 20.9 | 33.5 | 85.5 | 46.8 |
| `stof`, near `FLT_MAX` | 9.6 | 6.8 | 12.7 | 20.3 | **6.7** |
| **Intel Core i9-9980HK** | | | | | |
| `stod`, short (`12345.678`) | 20.8 | 14.7 | 28.4 | n/a | **13.8** |
| `stod`, 1 to 9 digit integers | 13.8 | 13.1 | 19.7 | n/a | **13.3** |
| `stod`, 17 digits (`%.17g`) | 27.0 | 21.0 | 40.6 | n/a | **21.1** |
| `stod`, 17 digits, any exponent | 33.7 | 24.8 | 48.5 | n/a | **22.6** |
| `stod`, shortest repr (JSON) | 34.3 | 26.4 | 53.8 | n/a | **22.0** |
| `stod`, `1.5e-7`-like | 28.8 | 21.8 | 40.8 | n/a | **17.9** |
| `stod`, 25 to 40 digits | **67.1** | 60.5 | 95.6 | n/a | 126.6 |
| `stod`, 100 digits | **77.4** | 71.5 | 145.2 | n/a | 162.8 |
| `stod`, 800 digits | **274.2** | 243.1 | 716.8 | n/a | 550.4 |
| `stod`, near `DBL_MAX` | 29.4 | 23.1 | 43.4 | n/a | **20.1** |
| `stof`, short | 23.3 | 17.7 | 31.8 | n/a | **17.9** |
| `stof`, 9 digits | 23.4 | 17.0 | 31.9 | n/a | **17.9** |
| `stof`, 25 to 40 digits | **76.4** | 70.2 | 90.0 | n/a | 120.1 |
| `stof`, near `FLT_MAX` | 26.6 | 20.9 | 33.6 | n/a | **16.9** |

## The exact path

Text the table cannot decide (an exact halfway point) and text `strtod` answers with `ERANGE` (subnormal,
overflow, underflow) go through the exact path.

| Workload | strict-stox | C locale | libc | `std::from_chars` | `fast_float` |
|---|---:|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 19.6 | 16.5 | 48.9 | 1044.1 | **13.6** |
| `stod`, odd integers `2^53..2^54` (halfway points) | 11.7 | 11.6 | **11.3** | 589.6 | 13.0 |
| `stod`, subnormal | 18.1 | 15.8 | 14.9 | 65203.7 | **6.9** |
| `stod`, overflow and underflow | 22.1 | 19.4 | 18.3 | 29.4 | **6.5** |
| `stof`, odd integers `2^24..2^25` (halfway points) | 8.6 | 8.5 | 26.8 | 270.4 | **6.5** |
| `stof`, subnormal | 18.5 | 16.4 | 53.5 | 2130.9 | **6.5** |
| `stof`, overflow | 17.6 | 15.7 | 15.0 | 33.2 | **7.5** |
| **Intel Core i9-9980HK** | | | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 49.8 | 41.0 | 109.0 | n/a | **25.5** |
| `stod`, odd integers `2^53..2^54` (halfway points) | 30.2 | 28.1 | **24.2** | n/a | 25.4 |
| `stod`, subnormal | 48.4 | 44.3 | 39.4 | n/a | **17.2** |
| `stod`, overflow and underflow | 46.6 | 41.1 | 37.2 | n/a | **17.1** |
| `stof`, odd integers `2^24..2^25` (halfway points) | 22.4 | 20.8 | 64.6 | n/a | **16.5** |
| `stof`, subnormal | 48.4 | 41.0 | 128.2 | n/a | **16.6** |
| `stof`, overflow | 43.2 | 36.3 | 35.4 | n/a | **17.8** |

## Declined and failing text

Text the fast path declines or that fails. It reaches `strtod` or `strtol` through the wrapper: a copy into a
NUL-terminated buffer, `errno` saved and restored, and the strict rules applied to the result. That costs more than
the bare libc call of the libc column; there is no failed attempt on top of it. A text whose first byte cannot start
a decimal (white space, `+`, `i`, `n`) goes there directly, and an integer that overflows is `ERANGE` from the digit
loop, without libc.

| Workload | strict-stox | libc |
|---|---:|---:|
| **Apple M4 Pro** | | |
| `stoll`, `" 42"` (white space) | 12.9 | **8.3** |
| `stoll`, `"abc"` (`EINVAL`) | 8.1 | **3.9** |
| `stoll`, 20 digits (`ERANGE`) | **20.2** | 21.4 |
| `stod`, `" 1.5"` (white space) | 14.8 | **11.8** |
| `stod`, `inf` and `nan` | 12.7 | **7.0** |
| `stof`, `" 1.5"` (white space) | 16.9 | **13.7** |
| `stof`, `inf` and `nan` | 13.1 | **7.4** |
| **Intel Core i9-9980HK** | | |
| `stoll`, `" 42"` (white space) | 38.8 | **29.4** |
| `stoll`, `"abc"` (`EINVAL`) | 23.8 | **14.5** |
| `stoll`, 20 digits (`ERANGE`) | **42.2** | 56.6 |
| `stod`, `" 1.5"` (white space) | 37.5 | **31.2** |
| `stod`, `inf` and `nan` | 28.6 | **18.1** |
| `stof`, `" 1.5"` (white space) | 40.2 | **32.9** |
| `stof`, `inf` and `nan` | 28.4 | **18.3** |

## Integers

| Workload | strict-stox | libc | `std::from_chars` | abseil |
|---|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | |
| `stoi`, 1 to 9 digits | **5.3** | 11.0 | 7.3 | 5.8 |
| `stoull`, 1 to 19 digits | **6.3** | 15.4 | 8.8 | n/a |
| `stoul`, base 16 | **5.6** | 16.9 | 10.6 | n/a |
| **Intel Core i9-9980HK** | | | | |
| `stoi`, 1 to 9 digits | **14.2** | 28.2 | 14.7 | 18.9 |
| `stoull`, 1 to 19 digits | **14.6** | 47.2 | 30.6 | n/a |
| `stoul`, base 16 | **13.0** | 77.2 | 49.0 | n/a |
