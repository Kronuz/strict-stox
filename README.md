# strict-stox

[![CI](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/strict-stox/actions/workflows/ci.yml)

A small, header-only, dependency-free **strict numeric parsing** library for
C++20, extracted from [Xapiand](https://github.com/Kronuz/Xapiand).

## What it is

One header, `strict_stox.hh`: strict wrappers over the C `strto*` family.
`strict_stoi("42")` succeeds; `strict_stoi("42abc")` and an overflow each throw a
standard exception. "Strict" means that with no `idx` argument the entire string
must be a valid number, or it's an error — unlike `std::stoi`, which happily
ignores trailing garbage. A `nothrow` variant reports via `errno` instead of
throwing. Nothing behind it but the C++ (and C) standard library.

It provides the usual set: `strict_stoi`, `strict_stou`, `strict_stol`,
`strict_stoul`, `strict_stoll`, `strict_stoull`, `strict_stoz` (to `size_t`),
`strict_stof`, `strict_stod`, `strict_stold`.

## Install

CMake with `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  strict_stox
  GIT_REPOSITORY https://github.com/Kronuz/strict-stox.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(strict_stox)

target_link_libraries(your_target PRIVATE strict_stox::strict_stox)
```

The `strict_stox` target is a pure `INTERFACE` library: it compiles nothing,
requests `cxx_std_20`, and puts the header directory on your include path. Then:

```cpp
#include "strict_stox.hh"  // strict_stoi, strict_stod, ...
```

Requires C++20. On macOS it builds with AppleClang/libc++, the same toolchain
Xapiand uses. The header keeps its original filename, so a codebase that already
`#include "strict_stox.hh"` just needs this repo on its include path.

## Usage

```cpp
#include "strict_stox.hh"

int    a = strict_stoi("42");                 // 42
long   b = strict_stoul("1f", nullptr, 16);   // 31
double d = strict_stod("3.5");                // 3.5

strict_stoi("42abc");                // throws std::invalid_argument (strict: must consume all input)
strict_stoi("99999999999999999999"); // throws std::out_of_range

int err = 0;
auto v = strict_stoi(&err, "nope");  // nothrow variant: v == 0, err == EINVAL

std::size_t idx = 0;
auto n = strict_stoi("123rest", &idx);        // n == 123, idx == 3
```

With no `idx` argument, the entire string must be a valid number or it's an error;
pass an `idx` to parse a prefix and learn how many characters were consumed. The
throwing overload raises `std::invalid_argument` on a bad parse and
`std::out_of_range` on overflow; the `nothrow` overload (first argument an
`int* errno_save`) surfaces `EINVAL` / `ERANGE` through that out-param instead.

## Build & test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The test checks the happy paths (including base 16), the two throwing error cases
(`std::invalid_argument` for invalid input, `std::out_of_range` for overflow), the
`nothrow` variant's `errno_save` reporting, and the `idx` prefix parse. It prints
`all strict-stox tests passed` and exits 0.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour. A top-level CMake build
produces it next to the test:

```sh
cmake -B build && cmake --build build && ./build/strict_stox_demo
```

It parses the valid inputs, then puts `std::stoi` / `strtol` and `strict_stoi` side
by side on the inputs the loose parsers wave through (trailing junk, leading-then-junk,
overflow, empty) so you can see exactly where strict rejects what loose accepts. It
also shows the `nothrow` `errno_save` form for hot paths, and the opt-in `idx`
prefix parse for when a prefix really is what you want.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand). The standalone delta
is pure decoupling: `strict_stox.hh` dropped `#include "exception.h"` and its
`THROW(InvalidArgument/OutOfRange, ...)` macros for plain
`std::invalid_argument` / `std::out_of_range`, with the same message text built by
string concatenation (no `std::format` dependency). The strict-parse semantics are
otherwise identical. See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and
[AGENTS.md](AGENTS.md) for the repo map and invariants.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
