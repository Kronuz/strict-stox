#!/usr/bin/env python3
"""Expected results for the curated texts of test/slow_test.cc, by exact rational arithmetic
(round half to even), independent of every libc.

For each text and each of double and float it prints the kind of the result:
  N bits   a normal finite value (errno 0), the correctly rounded bits
  E        ERANGE, value 0 through the API: overflow, zero from a non-zero text, or an inexact subnormal
  L bits   libc decides (exact subnormal, or rounded up into the smallest normal value): the bits of the value
"""
import sys
from fractions import Fraction

FORMATS = {
    "double": dict(mant=52, bias=1023, emin=-1022, emax=1023, qsub=-1074, width=64),
    "float": dict(mant=23, bias=127, emin=-126, emax=127, qsub=-149, width=32),
}


def exact(text):
    """The exact value of a decimal text, or +-1 * infinity / 0 as a string marker for absurd exponents."""
    neg = text.startswith("-")
    body = text.lstrip("-")
    mant, _, ex = body.lower().partition("e")
    ip, _, fp = mant.partition(".")
    n = int((ip + fp) or "0")
    e10 = int(ex or "0") - len(fp)
    if n == 0:
        return Fraction(0), neg
    nd = len(str(n))
    if e10 + nd > 400:
        return "overflow", neg
    if e10 + nd < -400:
        return "underflow", neg
    f = Fraction(n) * (Fraction(10) ** e10)
    return (-f if neg else f), neg


def classify(text, fmt):
    f = FORMATS[fmt]
    v, neg = exact(text)
    if v == "overflow" or v == "underflow":
        return ("E", None)
    if v == 0:
        return ("N", (1 << (f["width"] - 1)) if neg else 0)
    a = abs(v)
    # exponent of the leading bit
    e = a.numerator.bit_length() - a.denominator.bit_length()
    if Fraction(2) ** e > a:
        e -= 1
    assert Fraction(2) ** e <= a < Fraction(2) ** (e + 1)
    q = max(e - f["mant"], f["qsub"])
    scaled = a / (Fraction(2) ** q)
    m = scaled.numerator // scaled.denominator
    rem = scaled - m
    exactly = rem == 0
    if rem > Fraction(1, 2) or (rem == Fraction(1, 2) and m & 1):
        m += 1
    sbit = (1 << (f["width"] - 1)) if neg else 0
    if m == 1 << (f["mant"] + 1):
        m >>= 1
        q += 1
    be = q + f["mant"] + f["bias"]
    if m < 1 << f["mant"]:  # subnormal or zero
        if m == 0:
            return ("E", None)
        return ("L", sbit | m) if exactly else ("E", None)
    if be >= (1 << (f["width"] - 1 - f["mant"])) - 1:
        return ("E", None)
    bits = sbit | (be << f["mant"]) | (m & ((1 << f["mant"]) - 1))
    # a normal result reached by rounding up from below the smallest normal value is libc's
    below = a < Fraction(2) ** f["emin"]
    if below:
        return ("L", bits)
    return ("N", bits)


def main():
    for t in sys.argv[1:] or [l.rstrip("\n") for l in sys.stdin if l.strip()]:
        d = classify(t, "double")
        s = classify(t, "float")
        print(f'{{"{t}", {fmt(d)}, {fmt(s)}}},')


def fmt(c):
    k, b = c
    return f"{{'{k}', {hex(b) + 'ULL' if b is not None else '0'}}}"


if __name__ == "__main__":
    main()
