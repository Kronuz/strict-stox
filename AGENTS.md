# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
strict_stox.hh               Strict numeric parsing (strict_stoi/stoul/stod/...) over strto*; throws std exceptions. Header.
test/test.cc                 Runnable smoke test: happy paths, throwing cases, nothrow + idx variants.
test/diff_test.cc            Differential test against test/strict_stox_old.hh, the frozen pre-fast-path header (never edit it).
test/long_test.cc            Inputs of any length against libc strto* and std::from_chars.
bench/bench.cc               Benchmark vs the frozen header, std::from_chars, abseil and fast_float when installed (STRICT_STOX_BUILD_BENCH).
CMakeLists.txt               INTERFACE library `strict_stox` (+ alias strict_stox::strict_stox); CTest tests `strict_stox`, `_diff`, `_long`.
LICENSE                      MIT, Copyright (c) 2026 Germán Méndez Bravo and (c) 2015-2020 Dubalu LLC.
CHANGELOG.md                 Behavior and performance changes, dated.
README.md                    What it is, install, quick start.
docs/API.md                  What it provides and usage.
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
  library. Do not add Xapiand's `exception.h` back: `std::invalid_argument` /
  `std::out_of_range` in place of its `THROW` macros is what decouples the
  header. There is no optional seam here.
- **Filename is stable.** The header keeps its original Xapiand name
  (`strict_stox.hh`) so a consumer that already `#include`s it just needs this
  repo on the include path. Don't rename it.
- Tabs for indentation, double quotes in code, no em dashes in prose.

## Load-bearing invariants

- **Strict means "consume the whole string", unless given an `idx`.** With no
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

- **The fast path must give the answer `strto*` gives, or decline.** It is correct only
  for inputs it fully understands (see ARCHITECTURE). When widening it (more syntax,
  bigger exponents), extend `test/diff_test.cc`'s alphabets and edge list, and
  keep `ERANGE`/`EINVAL`/`idx` decisions in the `strto*` path.

## How to extend

- **Add a parser.** Add a `STOXIFY(name, strto_func, T)` (or `STOXIFY_BASE` for
  the base-taking integer forms) line. Pick `T` so the range check narrows to the
  intended type, or `void` to keep the `strto*` return type as-is.
- **Always extend the smoke test.** Cover the happy path, both throwing cases,
  and the nothrow/idx variants in `test/test.cc`, and add the parser to
  `test/diff_test.cc` and `test/long_test.cc`.

## Traps

- **Inputs are copied whole into a NUL-terminated buffer** (128-byte stack
  buffer, heap above that) before `strto*` runs, so any length parses like libc.
  An embedded NUL ends the number there.
- **`errno` is saved and restored around each parse.** The wrapper swaps the
  caller's `errno` out, runs the parse, and swaps it back, reporting through the
  return value / `errno_save`. Don't read the global `errno` after a call
  expecting the parse's value; use the `nothrow` overload's out-param.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). `strict_stox.hh` has no
`#include "exception.h"`; its two throw sites raise `std::invalid_argument` /
`std::out_of_range` with the upstream message text, built by string
concatenation. It also carries the fast path and the any-length fix in
`CHANGELOG.md`.

Keep extraction hygiene separate from behavior changes so they can be reconciled
with upstream.
