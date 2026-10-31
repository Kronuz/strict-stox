# Architecture

The internal design of `strict-stox`: a single header holding strict numeric
parsing over the C `strto*` family. For usage see `README.md`; for the repo map
and invariants see `AGENTS.md`.

## Shape

One header, header-only, zero dependencies beyond the C++/C standard library:

```
  strict_stox.hh   strict strto* wrappers, one throwing + one nothrow entry per parser
```

## The `Stox` wrapper

`Stox<F>` holds a name and a `strto*` function pointer. Its `_stox` core copies
the input into a 64-byte stack buffer (truncated to 63 chars), runs the `strto*`
function, then enforces *strict* semantics:

- If no `idx` out-param was given, the parse must consume the entire string —
  otherwise it's `EINVAL`.
- With an `idx`, it reports how many characters were consumed and allows a prefix
  parse.
- Integer variants additionally range-check the result against the target type's
  `numeric_limits` and report `ERANGE` on overflow. This is what lets
  `strict_stoi` (which parses with `strtol` and narrows to `int`) reject a value
  past `INT_MAX`.

`errno` is saved and restored around each parse: the wrapper swaps the caller's
`errno` out, runs the parse, and swaps it back, surfacing the parse's `errno`
through the return path rather than leaving it in the global.

Two public methods sit on top: `stox_nothrow` writes the parse's `errno` into an
`errno_save` out-param and returns `0` on failure; `stox_throw` turns `EINVAL`
into `std::invalid_argument` and `ERANGE` into `std::out_of_range`, with the
message built by plain string concatenation.

## The clean-input fast path

Before the `strto*` path runs, `stox_nothrow` and `stox_throw` try a direct parse
(`strict_stox_detail::fast_int`, `fast_fp`) that touches neither `errno` nor a
buffer. It only accepts what every `strto*` reads the same way, and declines
(falls back to the path above, which then decides everything) otherwise:

- integers (`stoi`, `stou`, `stol`, `stoul`, `stoll`, `stoull`, `stoz`): an optional
  `-` (signed types only), then digits of the requested base 2..36, the whole input,
  1..63 bytes, no overflow of the function's own type. Whitespace, `+`, `0x`/octal
  prefixes, base 0 and invalid bases, trailing text, an embedded NUL, `-` on an
  unsigned function (`strtoul("-1")` wraps) and every overflow take the old path.
  A value outside the narrower target type (`stoi` of 3000000000) also takes it, so
  `ERANGE` comes from the same code as before;
- `stof`/`stod`: `[-]digits[.digits][e[+-]digits]` of at most 17 bytes whose
  mantissa is at most 2^53 (2^24 for `float`) and whose decimal exponent is within
  +-22 (+-10 for `float`). Then `mantissa * 10^k` or `mantissa / 10^k` is a single
  IEEE operation on exactly representable operands, which is correctly rounded
  (Clinger's fast path), so the result equals `strtod`'s bit for bit. `inf`, `nan`,
  hex floats, subnormal results, longer mantissas and a locale whose decimal point is
  not `.` take the old path. Longer text (typically a full-precision 17 digit value) is
  declined at once. `stold` has no fast path;
- the fast path applies only when the function is exactly `std::strtol` etc.; a
  `Stox` built around a custom function keeps its own semantics.

`test/diff_test.cc` compares the fast path with the frozen previous header
(`test/strict_stox_old.hh`) on random and edge inputs: value (bitwise), `errno_save`,
`idx`, `errno` preservation and exception text.

## The `STOXIFY` macros

`STOXIFY` and `STOXIFY_BASE` stamp out the two free-function entry points for each
parser from one line. `STOXIFY_BASE` is for the integer forms that take a `base`
argument (`strict_stoul`, `strict_stoi`, ...); `STOXIFY` is for the
float/`base`-less forms (`strict_stof`, `strict_stod`, `strict_stold`). The `T`
template argument selects the type to narrow and range-check to, or `void` to keep
the `strto*` return type. So:

```cpp
STOXIFY_BASE(stoi, std::strtol, int)   // parse with strtol, range-check to int
STOXIFY(stod, std::strtod, void)       // parse with strtod, keep double
```

each expands to a throwing `strict_stoX(str, idx, base)` and a nothrow
`strict_stoX(errno_save, str, idx, base)`.

## Why this shape

`std::stoi` and friends are almost what you want but quietly accept trailing
garbage (`std::stoi("42abc")` returns `42`), which hides bugs in config and query
parsing. This header's contract is the strict one: with no `idx`, the whole string
must be a number. Building it as a thin layer over `strto*` keeps it tiny and
fast, and routing every parser through one `Stox` core means the strict check, the
range check, and the `errno` discipline are written once. The throwing/nothrow
split lets call sites that already sit inside a `try` use exceptions while hot,
allocation-sensitive paths use the `errno_save` form. Swapping Xapiand's `THROW`
macros for standard exceptions is what makes it stand alone with no project
dependency.
