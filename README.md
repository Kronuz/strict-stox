# strict-stox

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Strict number parsing for C++20: `strict_stoi("42abc")` is an error, not `42`.
The whole string must be a number unless you ask for a prefix, overflow is an
error, and a no-throw form reports through `errno`. For configuration, query
strings, and anything users type.

![strict-stox: the demo's output](docs/strict-stox.svg)

## ✨ Highlights

- **Strict by default.** Trailing garbage throws `std::invalid_argument`;
  overflow throws `std::out_of_range`.
- **Prefixes on request.** Pass `idx` to parse a prefix and learn where it ended.
- **No-throw form.** `strict_stoi(&err, "nope")` sets `err` to `EINVAL` or `ERANGE`.
- **The whole family**: `strict_stoi`, `stou`, `stol`, `stoul`, `stoll`,
  `stoull`, `stoz` (to `size_t`), `stof`, `stod`, `stold`, taking
  `std::string_view`, with a base for the integer ones.
- **One header, no dependencies.**

## 📦 Installation

Requires a C++20 compiler; CMake 3.14 or newer for the CMake route. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan,
and TSan builds.

```cmake
include(FetchContent)
FetchContent_Declare(strict_stox
  GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git
  GIT_TAG        main)
FetchContent_MakeAvailable(strict_stox)

target_link_libraries(my_app PRIVATE strict_stox::strict_stox)
```

Or copy `strict_stox.hh` onto your include path.

## 🚀 Quick start

```cpp
#include "strict_stox.hh"

#include <cstdio>

int main() {
	int port = strict_stoi("8080");                  // 8080
	unsigned long mask = strict_stoul("1f", nullptr, 16);   // 31

	int err = 0;
	strict_stoi(&err, "8080/tcp");                   // err == EINVAL: not a whole number
	std::printf("%d %lu %d\n", port, mask, err);
}
```

[`examples/demo.cc`](examples/demo.cc) contrasts it with `std::stoi` on bad input.

## 🎯 When to use it

- ✅ Parsing numbers from users, files, or the network, where `"42abc"` must fail.
- ❌ Locale-aware or grouped input (`"1,000"`): it uses the C `strto*` rules.

## ⚡ Performance

Plain input is parsed directly, with no buffer copy and no `errno` traffic. Everything
else goes through `strto*`, with the same results, `errno` values, `idx` and exceptions.

- Integers: an optional `-` and digits of the base that do not overflow.
- `stod` and `stof`: `[-]digits[.digits][e[+-]digits]` of any length. The digits are read
  eight at a time. Short numbers take Clinger's exact multiply or divide. The rest (17
  digit doubles, long mantissas) take a 5 KB table of the 64 leading bits of the powers of
  five and one 64-bit multiply, rounded to nearest. A rounding the table cannot decide
  (about one 17 digit text in a thousand), a subnormal or overflowing result, `inf`,
  `nan`, hex floats and a rounding mode other than to-nearest go to `strtod`.

`ctest` checks both paths against libc and the frozen previous header; the floating
point path also against `std::from_chars`, on the exact halfway point of every binade,
the classic hard cases and random text (`test/fp_test.cc`, `tools/fp_mutants.py`).

**The decimal point.** `strtod` reads the locale's decimal point, so in a locale where it
is `,`, `strict_stod("1.5")` is `1` plus trailing text. The fast path keeps that: it asks
`nl_langinfo` before it reads a `.` (3 ns on Apple silicon, 5 ns on the Intel laptop). A
program that never leaves the `"C"` locale can skip the question with
`-DSTRICT_STOX_C_LOCALE` (CMake: `-DSTRICT_STOX_C_LOCALE=ON`): plain decimals are then
read with `.` in every locale. Both columns are below.

Floating point, ns per element, best of 9 under `quiet strict`, `-O3`, Apple clang 17
(measured 2026-10-31; repeating the run moves a cell by at most 4%, and `fast_float`
moves by up to 10% between builds, so its best result is shown). "before" is the
Clinger-only fast path at `6d8731f` (17 digit doubles went to `strtod`).

| workload | before | now | now, C locale | `fast_float` |
|---|---|---|---|---|
| **Apple M4 Pro** | | | | |
| `stod`, short (`12345.678`) | 10.4 | 8.9 | **6.0** | 7.7 |
| `stod`, 17 digits (`%.17g`) | 23.5 | 10.9 | **7.6** | 11.0 |
| `stod`, shortest repr (JSON) | 24.2 | 12.4 | **9.4** | 10.3 |
| `stof`, short | 22.8 | 9.8 | **7.5** | 10.1 |
| `stof`, 9 digits | 17.1 | **7.2** | **6.3** | 7.6 |
| **Intel i9-9980HK** | | | | |
| `stod`, short | 28.6 | 18.4 | **13.7** | 15.6 |
| `stod`, 17 digits | 47.2 | 24.9 | **19.2** | 22.5 |
| `stod`, shortest repr (JSON) | 59.4 | 33.7 | **23.6** | 27.1 |
| `stof`, short | 46.7 | 22.7 | **16.7** | 19.2 |
| `stof`, 9 digits | 36.3 | 21.2 | **16.5** | 16.6 |

Integers, Apple M4 Pro, Apple clang 17 with `-O3 -DNDEBUG -std=gnu++20`, ns per element
(median of five runs under `quiet strict` at `6d8731f`; each run is the best of 7
samples over a pool of 1024 inputs):

| workload | strict-stox | `std::from_chars` |
|---|---|---|
| `stoi`, 1-9 digits | 5.9 | 7.1 (`absl::SimpleAtoi` 5.9) |
| `stoull`, 1-19 digits | 5.8 | 8.7 |
| `stoul`, base 16 | 4.7 | 11.0 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON \
  -DCMAKE_CXX_FLAGS="-DHAVE_FAST_FLOAT -I/path/to/fast_float/include"
cmake --build build && ./build/strict_stox_bench
```

The `absl::SimpleAtoi` figure appears when abseil is installed, and the `fast_float`
figures need `-DHAVE_FAST_FLOAT` and its include path. The runs agree within 5%,
except one `stoull` run (6.9). The floating point rows above come from
`./build/strict_stox_bench fp` (the CMake build finds `fast_float` with
`-DSTRICT_STOX_FAST_FLOAT_DIR=/path/to/fast_float/include`); `-DHAVE_PREV` adds the
"before" column from a saved older header (see `bench/bench.cc`).

[CHANGELOG.md](CHANGELOG.md) has the timings before each change.

## 📚 Documentation

- [docs/API.md](docs/API.md): the functions, `idx`, and the no-throw form.
- [ARCHITECTURE.md](ARCHITECTURE.md): how the wrappers are generated.
- [AGENTS.md](AGENTS.md): repository map and traps.
- [CHANGELOG.md](CHANGELOG.md): what changed, and when.

## 🛠️ Building and testing

```sh
cmake -B build && cmake --build build && ctest --test-dir build
./build/strict_stox_demo
```

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first. CI must
pass on every platform.

## 📄 License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE). Extracted from
[Xapiand](https://github.com/Kronuz/Xapiand).
