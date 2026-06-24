# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
strict_stox.hh               Strict numeric parsing (strict_stoi/stoul/stod/...) over strto*; throws std exceptions. Header.
test/test.cc                 Runnable smoke test: happy paths, throwing cases, nothrow + idx variants.
CMakeLists.txt               INTERFACE library `strict_stox` (+ alias strict_stox::strict_stox); CTest test `strict_stox`.
LICENSE                      MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                    What it is, install, usage.
ARCHITECTURE.md              Internal design of the Stox wrapper + STOXIFY macros, trade-offs.
```

Everything is header-only. There is no `.cc` to compile except the test. The
CMake target is a pure `INTERFACE` library that only adds the source dir to the
include path and requests `cxx_std_20`.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Expected output ends with `all strict-stox tests passed`, exit 0. The test target
is `strict_stox_test`; the registered CTest name is `strict_stox`.

## Conventions

- **C++20.** The target requests `cxx_std_20` to stay uniform with the sibling
  libraries. Don't drop it below.
- **Zero external dependencies.** The only includes are the C++ and C standard
  library. Do not add Xapiand's `exception.h` back — replacing its `THROW` macros
  with `std::invalid_argument` / `std::out_of_range` is exactly the decoupling
  this extraction did. There is no optional seam here.
- **Filename is stable.** The header keeps its original Xapiand name
  (`strict_stox.hh`) so a consumer that already `#include`s it just needs this
  repo on the include path. Don't rename it.
- Tabs for indentation, double quotes in code, no em dashes in prose.

## Load-bearing invariants

- **Strict means "consume the whole string" — unless given an `idx`.** With no
  `idx` out-param, a parse that leaves any unconsumed input (or a non-numeric
  input) is `EINVAL`. With an `idx`, the parse may stop early and reports how many
  characters it consumed. That is the distinguishing behavior from `std::stoi` &
  friends; don't relax it.
- **Two entry points per function: throwing and nothrow.** The `STOXIFY` /
  `STOXIFY_BASE` macros stamp out both. The throwing overload turns `EINVAL` into
  `std::invalid_argument` and `ERANGE` into `std::out_of_range`; the `nothrow`
  overload (first arg `int* errno_save`) surfaces the same `errno` through the
  out-param and returns `0`. Keep both for every function.
- **The throwing exceptions are `std::invalid_argument` / `std::out_of_range`.**
  This is the decoupling swap from Xapiand's `THROW(InvalidArgument/OutOfRange,
  ...)`. The message text is built by plain string concatenation (no
  `std::format` dependency). Don't reintroduce a custom exception type.
- **Integer variants range-check against the target type.** `strict_stoi` parses
  with `strtol` and then narrows to `int`, reporting `ERANGE` if the value is
  outside `int`'s `numeric_limits`. The `T` template argument on each `STOXIFY`
  is what drives that check; keep it correct per function.

## How to extend

- **Add a parser.** Add a `STOXIFY(name, strto_func, T)` (or `STOXIFY_BASE` for
  the base-taking integer forms) line. Pick `T` so the range check narrows to the
  intended type, or `void` to keep the `strto*` return type as-is.
- **Always extend the smoke test.** `test/test.cc` is the only executable check.
  Cover the happy path, both throwing cases, and the nothrow/idx variants.

## Traps

- **Inputs are copied into a 64-byte stack buffer and truncated to 63 chars**
  before parsing (it uses `strto*` under the hood). That's original behavior;
  don't be surprised by it, and don't grow the buffer without reason.
- **`errno` is saved and restored around each parse.** The wrapper swaps the
  caller's `errno` out, runs the parse, and swaps it back, reporting through the
  return value / `errno_save`. Don't read the global `errno` after a call
  expecting the parse's value — use the `nothrow` overload's out-param.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). The delta from the original is pure
decoupling: `strict_stox.hh` dropped `#include "exception.h"`; the two `THROW(...)`
sites became `throw std::invalid_argument(...)` / `throw std::out_of_range(...)`
with the same message text built by string concatenation. Behavior and the
strict-parse semantics are otherwise identical.

Keep extraction hygiene separate from behavior changes so they can be reconciled
with upstream.
