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

Plain input (an optional `-`, digits, and for floating point a fraction and
exponent that one correctly rounded multiply or divide can handle) is parsed
directly, with no buffer copy and no `errno` traffic. Everything else goes
through `strto*`, with the same results, `errno` values, `idx` and exceptions.
`ctest` checks both paths against the frozen previous header and libc.

Apple M4 Pro, Apple clang 17 with `-O3 -DNDEBUG -std=gnu++20`, ns per element
(median of five runs under `quiet strict` at `6d8731f`; each run is the best of 7
samples over a pool of 1024 inputs):

| workload | strict-stox | `std::from_chars` |
|---|---|---|
| `stoi`, 1-9 digits | 5.9 | 7.1 (`absl::SimpleAtoi` 5.9) |
| `stoull`, 1-19 digits | 5.8 | 8.7 |
| `stoul`, base 16 | 4.7 | 11.0 |
| `stod`, short decimals | 10.3 | 15.7 (`fast_float` 7.6) |
| `stod`, 17-digit doubles (`strto*` path) | 22.8 | 26.2 (`fast_float` 10.5) |

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSTRICT_STOX_BUILD_BENCH=ON \
  -DCMAKE_CXX_FLAGS="-DHAVE_FAST_FLOAT -I/path/to/fast_float/include"
cmake --build build && ./build/strict_stox_bench
```

The `absl::SimpleAtoi` figure appears when abseil is installed, and the `fast_float`
figures need `-DHAVE_FAST_FLOAT` and its include path. The runs agree within 5%,
except one `stoull` run (6.9).

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
