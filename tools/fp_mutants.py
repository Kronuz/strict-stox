#!/usr/bin/env python3
"""Mutation check for the floating point fast path.

For each mutant: copy strict_stox.hh with one deliberate bug into a scratch dir, build
test/fp_test.cc against it and require that the test FAILS. A mutant that survives means
the test would not catch that bug.

usage: tools/fp_mutants.py TREE_DIR [CXX] [only-name ...]     (scratch dir: $MUT_DIR or a temp dir)
"""
import os
import subprocess
import sys
import shutil
import tempfile

tree = os.path.abspath(sys.argv[1])
cxx = sys.argv[2] if len(sys.argv) > 2 else "g++"
only = set(sys.argv[3:])
src = open(os.path.join(tree, "strict_stox.hh"), encoding="utf-8").read()

MUTANTS = {
    # no ambiguity check at all
    "no_ambiguity": ("if (((hi & ((half << 1) - 1)) + (lo != 0)) == half) { return false; }", ""),
    # only the lo != 0 half of the check
    "ambiguity_lo_only": ("if (((hi & ((half << 1) - 1)) + (lo != 0)) == half) { return false; }",
                          "if ((hi & ((half << 1) - 1)) == half - 1 && lo != 0) { return false; }"),
    # only the lo == 0 half (exact halfway)
    "ambiguity_tie_only": ("if (((hi & ((half << 1) - 1)) + (lo != 0)) == half) { return false; }",
                           "if ((hi & ((half << 1) - 1)) == half && lo == 0) { return false; }"),
    # truncate instead of round
    "truncate": ("std::uint64_t m = ((hi >> sh) + 1) >> 1;", "std::uint64_t m = (hi >> sh) >> 1;"),
    # accept subnormal results
    "subnormal_ok": ("if (be < 1) { return false; }", "if (be < 0) { return false; }"),
    # round up into the normal range from below
    "subnormal_after_carry": ("if (be < 1) { return false; }", "if (be < 0 || (be == 0 && false)) { return false; }"),
    # accept overflow
    "overflow_ok": ("if (be >= tr::max_be) { return false; }", "if (be > tr::max_be) { return false; }"),
    # exponent off by one for positive q
    "exp_off_by_one": ("int be = ((q * 217706) >> 16) + 63 + u - lz + tr::bias;", "int be = ((q * 217706) >> 16) + 63 + u - lz + tr::bias + (q > 200 ? 1 : 0);"),
    # wrong log2(10) constant
    "log2_const": ("int be = ((q * 217706) >> 16)", "int be = ((q * 217690) >> 16)"),
    # carry ignored
    "no_carry": ("const int carry = static_cast<int>(m >> (tr::mant + 1));", "const int carry = 0;"),
    # sticky digits ignored
    "sticky_ignored": ("sticky |= d != 0;", "sticky |= false;"),
    # w + 1 comparison skipped
    "sticky_no_upper": ("if (!el_convert<R>(w + 1, static_cast<int>(q), b) || a != b) { return false; }", "(void)b;"),
    # leading zero in the fraction does not shift the decimal point (long path)
    "long_leading_zero": ("if (nsig == 0 && d == 0) {  // leading zero: it only shifts the decimal point\n\t\t\tif (frac) { --q; }", "if (nsig == 0 && d == 0) {  // leading zero: it only shifts the decimal point\n\t\t\tif (false) { --q; }"),
    # dropped integer digits do not raise the exponent
    "long_dropped_int": ("if (!frac) { ++q; }", "if (false) { ++q; }"),
    # table corruption: one entry off by one
    "table_entry": ("0xcccccccccccccccdULL", "0xccccccccccccccccULL") if "0xcccccccccccccccdULL" in src else ("0xccccccccccccccccULL", "0xccccccccccccccbbULL"),
    # swapped normalisation of negative exponents
    "neg_q_index": ("mul64(w << lz, pow5_tab[q - pow5_qmin], hi, lo);", "mul64(w << lz, pow5_tab[q - pow5_qmin + (q == -1 ? 1 : 0)], hi, lo);"),
    # locale check dropped
    "no_locale": ("if (dot && !decimal_point_is_dot()) { return false; }", ""),
    # rounding mode check dropped
    "no_rounding_guard": ("if (!rounds_to_nearest()) { return false; }", ""),
    # negative zero lost
    "neg_zero": ("b |= static_cast<bits_t>(neg) << (sizeof(bits_t) * 8 - 1);", "if (b != 0) { b |= static_cast<bits_t>(neg) << (sizeof(bits_t) * 8 - 1); }"),
    # exponent digits: junk after exponent accepted
    "exp_junk": ("if (d > 9) { return false; }\n\t\t}\n\t\tif (eneg) { ex = -ex; }", "if (d > 9) { break; }\n\t\t}\n\t\tif (eneg) { ex = -ex; }"),
    # a '+' sign skipped as if it were part of the digits is harmless (strtod accepts '+'), so
    # mutate something that is not: a second '.' taken for the exponent marker
    "second_dot": ("if ((static_cast<unsigned char>(*p) | 0x20) != 'e') { return false; }", "if ((static_cast<unsigned char>(*p) | 0x20) != 'e' && *p != '.') { return false; }"),
    "exp_sign_lost": ("if (eneg) { ex = -ex; }", ""),
    "frac_count": ("nf = p - f0;", "nf = p - f0 - (p - f0 > 7 ? 1 : 0);"),
    "parse8_const": ("42949672960001ULL", "42949672960000ULL"),
    "table_pos_entry": ("0xa56fa5b99019a5c8ULL", "0xa56fa5b99019a5c9ULL"),
    "float_window": ("constexpr int nb = tr::mant + 2;", "constexpr int nb = tr::mant + 2 + (tr::mant == 23 ? 1 : 0);"),
    "no_empty_check": ("if (nd == 0) { return false; }", ""),
    # Clinger front end: mantissa limit, exponent limit, and the operation
    "clinger_mant": ("if (w <= (1ULL << 53) && q <= 22 && q >= -22) {", "if (w <= (1ULL << 55) && q <= 22 && q >= -22) {"),
    "clinger_exp": ("if (w <= (1ULL << 53) && q <= 22 && q >= -22) {", "if (w <= (1ULL << 53) && q <= 23 && q >= -22) {"),
    "clinger_swap": ("b = std::bit_cast<bits_t>(q >= 0 ? static_cast<double>(w) * pow10[q] : static_cast<double>(w) / pow10[-q]);",
                     "b = std::bit_cast<bits_t>(q >= 0 ? static_cast<double>(w) * pow10[q] : static_cast<double>(w) * (1.0 / pow10[-q]));"),
    # swar: use 7 instead of 8 digit loads' shift
    "swar_shift": ("parse8(v << (64 - 8 * n))", "parse8(v << (64 - 8 * n + (n == 3 ? 8 : 0)))"),
    # 19 digit limit: 20 digits accepted into w
    "nd_limit": ("if (nd > 19) {", "if (nd > 20) {"),
    # the exponent accumulator
    "exp_overflow": ("ex = ex < 100000 ? ex * 10 + static_cast<std::ptrdiff_t>(d) : std::numeric_limits<std::ptrdiff_t>::max() / 4;", "ex = ex * 10 + static_cast<std::ptrdiff_t>(d);"),
    # a clamped exponent kept instead of poisoned (the 7-digit exponent case)
    "exp_clamp": ("ex = ex < 100000 ? ex * 10 + static_cast<std::ptrdiff_t>(d) : std::numeric_limits<std::ptrdiff_t>::max() / 4;", "ex = ex < 100000 ? ex * 10 + static_cast<std::ptrdiff_t>(d) : ex;"),
}

work = os.environ.get("MUT_DIR") or tempfile.mkdtemp(prefix="stox-mut-")
os.makedirs(work, exist_ok=True)
survived = []
for name, (old, new) in MUTANTS.items():
    if only and name not in only:
        continue
    if old not in src:
        print("%-24s PATCH DID NOT APPLY" % name)
        survived.append(name + "(patch)")
        continue
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    open(os.path.join(d, "strict_stox.hh"), "w", encoding="utf-8").write(src.replace(old, new, 1))
    exe = os.path.join(d, "fp")
    r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wno-ignored-attributes", "-I" + d, "-I" + os.path.join(tree, "test"), os.path.join(tree, "test", "fp_test.cc"), "-o", exe, "-pthread"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("%-24s BUILD FAILED: %s" % (name, r.stderr[:300]))
        survived.append(name + "(build)")
        continue
    t = subprocess.run([exe], capture_output=True, text=True, cwd=d)
    killed = t.returncode != 0
    first = [l for l in t.stderr.splitlines() if l][:1]
    print("%-24s %s %s" % (name, "KILLED  " if killed else "SURVIVED", first[0][:100] if first else ""))
    if not killed:
        survived.append(name)
    sys.stdout.flush()
print("survivors:", survived if survived else "none")
sys.exit(1 if survived else 0)
