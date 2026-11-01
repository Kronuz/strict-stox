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
  to skip the locale query (about 3 ns per call on the M4 Pro) and read `.` everywhere.
- **Text with an embedded NUL.** The number ends at the NUL, as it does for
  `strto*`.
- **The fastest decimals.** `fast_float` parses short decimals in 6.4 ns against 8.9 for
  strict-stox (`12345.678`, M4 Pro), and on exact ties, subnormals and overflow it is 1.3x to
  3.5x faster than the exact path.

## ⚡ Performance

ns per number over 1,024 inputs, the best of 9 samples, the mean of two runs of `bench/bench.cc`.
*strict-stox* is `strict_stod` and `strict_stof` of this library. *C locale* is the same header
built with `-DSTRICT_STOX_C_LOCALE`, which reads `.` as the decimal point without asking the locale
(a build mode, not another library). *libc* is `strtod` and `strtof` on a NUL-terminated copy, which
is what strict-stox falls back to. `std::from_chars` is the C++17 function of the platform's
standard library (its floating-point form needs macOS 26, so only the M4 Pro runs it), and
`fast_float` is [fast_float v8.3.3](https://github.com/fastfloat/fast_float/tree/v8.3.3), the widely
used header-only parser behind several standard libraries' `from_chars`. The bench compares the
inputs each of them accepts bit for bit: libc returns the same value as strict-stox on every input
strict-stox accepts, and `std::from_chars` and `fast_float` the same except for text outside their
syntax (leading white space) and text that strict-stox reports an error on (out of range);
`strict_stox_bench --check` runs the comparison. Apple clang 17, `-O3`, on an Apple M4 Pro (macOS
26). Bold is the fastest implementation of a row (the C locale build is a mode of strict-stox, not
counted).

| Workload | strict-stox | C locale | libc | `std::from_chars` | `fast_float` |
|---|---:|---:|---:|---:|---:|
| **Apple M4 Pro** | | | | | |
| `stod`, short (`12345.678`) | 8.9 | 6.0 | 12.1 | 15.3 | **6.4** |
| `stod`, 17 digits (`%.17g`) | 11.1 | 8.0 | 18.8 | 25.5 | **8.6** |
| `stod`, shortest repr (JSON) | 11.3 | 8.7 | 19.1 | 48.7 | **8.7** |
| `stod`, 100 digits | **28.4** | 27.1 | 92.3 | 147.0 | 121.7 |
| `stof`, 9 digits | 7.3 | 5.8 | 15.7 | 174.3 | **6.2** |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON
cmake --build build --target strict_stox_bench
./build/strict_stox_bench fp
```

The bench fetches `fast_float` at that tag (or reads `-DSTRICT_STOX_FAST_FLOAT_DIR=<include dir>`) and
uses abseil when CMake finds it; `-DSTRICT_STOX_C_LOCALE=ON` builds the C locale column.
[docs/perf/](docs/perf/README.md) has every row: long mantissas, the exact path, declined text and integers.

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
