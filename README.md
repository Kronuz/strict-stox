# strict-stox

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries.

[![CI](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Header-only](https://img.shields.io/badge/header--only-yes-green.svg)

Strict whole-string number parsing for C++20: trailing text and overflow are errors, in a throwing or an errno form.
`strict_stoi("42abc")` is an error, not `42`. Plain integers and decimals are
parsed by the header itself, with no copy and no `errno` traffic, and every
`double` and `float` comes out bit for bit what libc's `strtod` and `strtof`
return. Reach for it to read configuration, query strings, command lines and
anything else people type.

## ✨ Highlights

- **Strict by default.** Trailing text throws `std::invalid_argument`; overflow
  throws `std::out_of_range`.
- **Prefixes on request.** Pass `idx` to parse a prefix and learn where it ended.
- **No-throw form.** `strict_stoi(&err, "nope")` returns 0 and sets `err` to
  `EINVAL` or `ERANGE`.
- **The whole family.** `strict_stoi`, `stou`, `stol`, `stoul`, `stoll`, `stoull`,
  `stoz` (to `size_t`), `stof`, `stod` and `stold`, taking a `std::string_view`,
  with a base for the integer ones.
- **Its own decimal parser.** Floats go through a table of powers of five, and
  what the table cannot decide through an exact big-integer comparison with the
  halfway point. libc's `strto*` decides the rest: trailing text, white space,
  `+`, base prefixes, integer overflow, `inf`, `nan`, hex floats, `stold`, a
  directed rounding mode, a locale whose decimal point is not `.`, and two
  subnormal results whose `errno` differs between libcs.
- **Checked against libc.** `ctest` compares both paths with libc and with the
  frozen previous header, and the floating point path also with
  `std::from_chars` and in all four rounding modes;
  `tools/verify.cc` sweeps billions of texts
  ([docs/perf/verification.md](docs/perf/verification.md)).
- **One header, no dependencies.**

## 🚀 Quick start

```cpp
#include "strict_stox.hh"

#include <cerrno>
#include <cstdio>
#include <stdexcept>

int main() {
	int port = strict_stoi("8080");                        // 8080
	unsigned long mask = strict_stoul("1f", nullptr, 16);  // 31
	double ratio = strict_stod("0.25");                    // 0.25

	std::size_t idx = 0;
	long width = strict_stol("42px", &idx);                // 42 and idx == 2: a prefix, on request

	int err = 0;
	strict_stoi(&err, "8080/tcp");                         // no exception; err == EINVAL

	try {
		strict_stoi("99999999999");
	} catch (const std::out_of_range&) {
		std::puts("overflow throws std::out_of_range");
	}
	std::printf("%d %lu %g %ld %zu %d\n", port, mask, ratio, width, idx, err == EINVAL);
	// 8080 31 0.25 42 2 1
}
```

[`examples/demo.cc`](examples/demo.cc) contrasts it with `std::stoi` on bad input.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; no dependencies. CI builds it
on Linux (x86_64, arm64), macOS (arm64, x86_64), and FreeBSD, plus ASan, UBSan,
and TSan builds; a manual workflow runs the long verification on four of them.

```cmake
include(FetchContent)
FetchContent_Declare(strict_stox
  GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git
  GIT_TAG        5a68fc49888929be8bc0b5aaf21100294496f0a1)  # or main for the tip
FetchContent_MakeAvailable(strict_stox)

target_link_libraries(my_app PRIVATE strict_stox::strict_stox)
```

```cpp
#include "strict_stox.hh"
```

Without CMake, copy `strict_stox.hh` onto your include path.

## 🎯 When to use it

Use it wherever a number arrives as text and `"42abc"`, `"1e999"` or `"99999999999"`
must fail instead of turning into something.

Not for:

- **Rejecting leading white space or `+`.** The C `strto*` rules accept them, so
  `strict_stoi(" 42")` and `strict_stoi("+42")` are 42; trailing white space is an
  error.
- **Grouped or localized input.** `"1,000"` is an error. The decimal point follows
  the current locale, as in `strtod`: where it is `,`, `strict_stod("1.5")` fails. A
  program that stays in the `"C"` locale can build with `-DSTRICT_STOX_C_LOCALE=ON`
  to skip the locale query (3 to 5 ns per call) and read `.` everywhere.
- **Text with an embedded NUL.** The number ends at the NUL, as it does for
  `strto*`.
- **The fastest halfway points.** On exact ties, subnormals and overflow
  `fast_float` is 1.5x to 2.6x faster than the exact path.

## ⚡ Performance

ns per element over 1,024 inputs, the best of 9 samples, mean of two runs; `-O3`,
Apple clang 17, `fast_float` 3.10.1 (`6a3fabc`, its best result over builds). The
C locale column is built with `-DSTRICT_STOX_C_LOCALE`:

| workload | Apple M4 Pro: strict-stox / C locale / `fast_float` | Intel Core i9-9980HK: strict-stox / C locale / `fast_float` |
|---|---:|---:|
| `stod`, short (`12345.678`) | 8.7 / 5.9 / 7.5 | 19.1 / 13.4 / 15.3 |
| `stod`, 17 digits (`%.17g`) | 10.8 / 7.2 / 10.6 | 25.2 / 19.9 / 23.2 |
| `stod`, shortest repr (JSON) | 10.9 / 8.5 / 10.4 | 32.9 / 24.0 / 27.8 |
| `stod`, 100 digits | 28.7 / 27.1 / 37.7 | 79.1 / 69.8 / 80.4 |
| `stof`, 9 digits | 6.8 / 5.8 / 7.6 | 20.4 / 16.7 / 18.0 |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON \
  -DSTRICT_STOX_FAST_FLOAT_DIR=/path/to/fast_float/include
cmake --build build --target strict_stox_bench
./build/strict_stox_bench fp
```

Integers on the M4 Pro: `stoi` of 1 to 9 digits takes 6.4 ns and
`std::from_chars` 7.2; `stoul` in base 16, 5.3 and 11.0. Text the fast path
declines (leading white space, `inf`, `nan`) pays 4 to 5 ns (M4 Pro) and 9 to
11 ns (i9) on top of `strtod`.
[docs/perf/README.md](docs/perf/README.md) has every row: long mantissas, the
exact path against `strtod` and `fast_float`, declined text and integers.

## 📚 Documentation

- [docs/API.md](docs/API.md): the functions, `idx`, the no-throw form, and the
  locale.
- [ARCHITECTURE.md](ARCHITECTURE.md): the wrapper, the fast path, the floating
  point conversion and the exact path.
- [docs/perf/README.md](docs/perf/README.md): the benchmark tables.
- [docs/perf/verification.md](docs/perf/verification.md): the independent
  harness (`tools/verify.cc`) and what has run through it.
- [CHANGELOG.md](CHANGELOG.md): what changed, with numbers.
- [AGENTS.md](AGENTS.md): repository map, invariants, and traps.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#strict-stox): the catalog entry.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

`./build/strict_stox_demo` runs the demo. Add `-DSTRICT_STOX_BUILD_BENCH=ON` to
build `strict_stox_bench` (the `absl` column appears when abseil is installed),
and `-DSTRICT_STOX_BUILD_VERIFY=ON` to build `strict_stox_verify` with two short
runs of it under `ctest`.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo and Copyright (c) 2015-2020 Dubalu
LLC. See [LICENSE](LICENSE). Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).
