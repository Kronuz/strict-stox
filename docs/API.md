# strict-stox API reference

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
