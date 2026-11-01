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
| `stod`, short (`12345.678`) | 8.9 | 6.0 | 12.1 | 15.3 | **6.4** |
| `stod`, 1 to 9 digit integers | **4.9** | 4.8 | 9.4 | 15.3 | 5.7 |
| `stod`, 17 digits (`%.17g`) | 11.1 | 8.0 | 18.8 | 25.5 | **8.6** |
| `stod`, 17 digits, any exponent | 12.1 | 9.4 | 21.2 | 49.0 | **8.4** |
| `stod`, shortest repr (JSON) | 11.3 | 8.7 | 19.1 | 48.7 | **8.7** |
| `stod`, `1.5e-7`-like | 10.4 | 7.3 | 17.6 | 22.8 | **6.5** |
| `stod`, 25 to 40 digits | **20.3** | 18.5 | 36.0 | 52.9 | 47.0 |
| `stod`, 100 digits | **28.4** | 27.1 | 92.3 | 147.0 | 121.7 |
| `stod`, 800 digits | **126.8** | 125.2 | 938.8 | 1503.8 | 513.3 |
| `stod`, near `DBL_MAX` | 11.1 | 8.4 | 16.2 | 32.5 | **7.7** |
| `stof`, short | 9.4 | 7.2 | 13.4 | 19.4 | **8.0** |
| `stof`, 9 digits | 7.3 | 5.8 | 15.7 | 174.3 | **6.2** |
| `stof`, 25 to 40 digits | **23.0** | 21.4 | 33.5 | 99.1 | 47.2 |
| `stof`, near `FLT_MAX` | 9.6 | 7.0 | 12.9 | 20.4 | **6.8** |
| **Intel Core i9-9980HK** | | | | | |
| `stod`, short (`12345.678`) | 21.2 | 14.6 | 29.9 | n/a | **13.5** |
| `stod`, 1 to 9 digit integers | 13.2 | 12.8 | 20.1 | n/a | **12.2** |
| `stod`, 17 digits (`%.17g`) | 27.0 | 20.5 | 41.5 | n/a | **19.4** |
| `stod`, 17 digits, any exponent | 32.5 | 24.1 | 48.1 | n/a | **20.5** |
| `stod`, shortest repr (JSON) | 33.8 | 25.3 | 55.4 | n/a | **21.9** |
| `stod`, `1.5e-7`-like | 28.6 | 21.7 | 40.2 | n/a | **16.9** |
| `stod`, 25 to 40 digits | **70.6** | 61.0 | 96.8 | n/a | 121.2 |
| `stod`, 100 digits | **78.8** | 71.7 | 145.8 | n/a | 164.2 |
| `stod`, 800 digits | **245.6** | 239.1 | 713.5 | n/a | 529.3 |
| `stod`, near `DBL_MAX` | 29.3 | 22.6 | 43.7 | n/a | **17.8** |
| `stof`, short | 23.5 | 18.3 | 31.5 | n/a | **18.2** |
| `stof`, 9 digits | 21.5 | 16.9 | 31.6 | n/a | **18.2** |
| `stof`, 25 to 40 digits | **75.5** | 72.8 | 88.3 | n/a | 122.9 |
| `stof`, near `FLT_MAX` | 26.2 | 21.0 | 32.9 | n/a | **18.4** |

## The exact path

Text the table cannot decide (an exact halfway point) and text `strtod` answers with `ERANGE` (subnormal,
overflow, underflow) go through the exact path.

| Workload | strict-stox | C locale | libc | `std::from_chars` | `fast_float` |
|---|---:|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 19.2 | 16.6 | 49.7 | 1067.2 | **13.6** |
| `stod`, odd integers `2^53..2^54` (halfway points) | 16.5 | 5.3 | **11.5** | 620.0 | 13.1 |
| `stod`, subnormal | 18.2 | 15.6 | 15.2 | 66215.5 | **7.0** |
| `stod`, overflow and underflow | 21.9 | 19.2 | 18.3 | 30.9 | **6.3** |
| `stof`, odd integers `2^24..2^25` (halfway points) | 12.4 | 12.7 | 26.6 | 265.2 | **6.7** |
| `stof`, subnormal | 18.6 | 16.1 | 54.9 | 2141.4 | **6.8** |
| `stof`, overflow | 17.4 | 15.6 | 15.2 | 33.6 | **7.6** |
| **Intel Core i9-9980HK** | | | | | |
| `stod`, exact halfway points (`2^52..2^53 + .5`) | 47.2 | 41.0 | 105.3 | n/a | **21.6** |
| `stod`, odd integers `2^53..2^54` (halfway points) | 39.1 | 39.4 | 25.4 | n/a | **19.4** |
| `stod`, subnormal | 48.8 | 44.2 | 39.6 | n/a | **15.8** |
| `stod`, overflow and underflow | 46.6 | 41.5 | 36.2 | n/a | **16.1** |
| `stof`, odd integers `2^24..2^25` (halfway points) | 35.6 | 29.8 | 56.2 | n/a | **16.6** |
| `stof`, subnormal | 46.7 | 40.1 | 123.7 | n/a | **18.2** |
| `stof`, overflow | 42.8 | 37.2 | 34.1 | n/a | **18.9** |

## Declined and failing text

Text the fast path declines or that fails. A declined text pays the attempt on top of `strtod`, so
`strict-stox` loses to libc here.

| Workload | strict-stox | libc |
|---|---:|---:|
| **Apple M4 Pro** | | |
| `stoll`, `" 42"` (white space) | 15.8 | **8.9** |
| `stoll`, `"abc"` (`EINVAL`) | 10.4 | **4.0** |
| `stoll`, 20 digits (`ERANGE`) | 33.1 | **20.4** |
| `stod`, `" 1.5"` (white space) | 23.4 | **12.4** |
| `stod`, `inf` and `nan` | 21.9 | **6.9** |
| `stof`, `" 1.5"` (white space) | 25.0 | **13.9** |
| `stof`, `inf` and `nan` | 22.0 | **7.3** |
| **Intel Core i9-9980HK** | | |
| `stoll`, `" 42"` (white space) | 40.4 | **27.9** |
| `stoll`, `"abc"` (`EINVAL`) | 23.7 | **15.2** |
| `stoll`, 20 digits (`ERANGE`) | 78.8 | **58.0** |
| `stod`, `" 1.5"` (white space) | 52.2 | **30.8** |
| `stod`, `inf` and `nan` | 43.2 | **19.2** |
| `stof`, `" 1.5"` (white space) | 60.0 | **32.5** |
| `stof`, `inf` and `nan` | 45.2 | **17.6** |

## Integers

| Workload | strict-stox | libc | `std::from_chars` | abseil |
|---|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | |
| `stoi`, 1 to 9 digits | **6.1** | 11.1 | 7.5 | 6.2 |
| `stoull`, 1 to 19 digits | **6.3** | 15.4 | 8.9 | n/a |
| `stoul`, base 16 | **5.2** | 17.6 | 10.9 | n/a |
| **Intel Core i9-9980HK** | | | | |
| `stoi`, 1 to 9 digits | **14.2** | 27.8 | 15.2 | 17.9 |
| `stoull`, 1 to 19 digits | **13.9** | 47.0 | 23.4 | n/a |
| `stoul`, base 16 | **12.5** | 77.1 | 39.4 | n/a |
