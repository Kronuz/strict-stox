#!/usr/bin/env python3
"""Mutation check for the floating point fast path.

For each mutant: copy strict_stox.hh with one deliberate bug into a scratch dir, build
the floating point tests against it (test/fp_test.cc, test/rounding_test.cc and the mock build of
test/locale_test.cc) and require that at least one FAILS. A mutant that survives means the tests
would not catch that bug. Some mutants name the test that has to kill them (REQUIRED_KILLER): the
locale guard must not depend on an installed comma locale, which fp_test uses when there is one.

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
    # the long path (more than 19 digits): sticky digits ignored in each place they can be, and w + 1 not compared
    "sticky_after_int19": ("sticky = any_nonzero(a + 19, ie) || any_nonzero(fb, fe);", "sticky = any_nonzero(fb, fe);"),
    "sticky_frac_of_int19": ("sticky = any_nonzero(a + 19, ie) || any_nonzero(fb, fe);", "sticky = any_nonzero(a + 19, ie);"),
    "sticky_after_int": ("sticky = any_nonzero(fb + k, fe);", "sticky = false;"),
    "sticky_after_zeros": ("sticky = any_nonzero(b + k, fe);", "sticky = false;"),
    "sticky_no_upper": ("if (!el_convert<R>(w + 1, static_cast<int>(q), y) || x != y) { return false; }", "(void)y;"),
    # leading zeros of the fraction do not shift the decimal point; dropped integer digits do not raise the exponent
    "long_leading_zero": ("q -= static_cast<std::ptrdiff_t>(static_cast<std::size_t>(b - fb) + k);", "q -= static_cast<std::ptrdiff_t>(k);"),
    "long_dropped_int": ("q += static_cast<std::ptrdiff_t>(ni - 19);", ""),
    "long_frac_digits": ("q -= static_cast<std::ptrdiff_t>(k);\n\t\t\tsticky = any_nonzero(fb + k, fe);", "sticky = any_nonzero(fb + k, fe);"),
    "long_window_20": ("w = append_digits(a, 19, 0);", "w = append_digits(a, 20, 0);"),
    "long_window_18": ("const std::size_t k = std::min<std::size_t>(19, static_cast<std::size_t>(fe - b));", "const std::size_t k = std::min<std::size_t>(18, static_cast<std::size_t>(fe - b));"),
    "long_window_int": ("const std::size_t k = std::min<std::size_t>(19 - ni, static_cast<std::size_t>(fe - fb));", "const std::size_t k = std::min<std::size_t>(20 - ni, static_cast<std::size_t>(fe - fb));"),
    "long_all_zero": ("if (b == fe) {\n\t\t\tout = 0;", "if (b == fe) {\n\t\t\tout = 1;"),
    "long_fraction_start": ("if (ie != nullptr) { fb = ie + 1; } else { ie = mend; }", "if (ie != nullptr) { fb = ie; } else { ie = mend; }"),
    "long_no_point": ("if (ie != nullptr) { fb = ie + 1; } else { ie = mend; }", "if (ie != nullptr) { fb = ie + 1; } else { ie = ib; }"),
    # the digit helpers: the zero scan, the non-zero scan and the digit accumulation
    "skip_zeros_shift": ("return p + (ctz64(v) >> 3);", "return p + (ctz64(v) >> 4);"),
    "any_nonzero_first_byte": ("if (v != 0x3030303030303030ULL) { return true; }", "if ((v >> 8) != 0x0030303030303030ULL) { return true; }"),
    "append_tail": ("while (n-- != 0) { w = w * 10 + static_cast<unsigned>(*p++ - '0'); }", "while (n-- > 1) { w = w * 10 + static_cast<unsigned>(*p++ - '0'); }"),
    "append_chunk": ("w = w * 100000000ULL + parse8(v);\n\t\tp += 8;\n\t\tn -= 8;", "w = w * 10000000ULL + parse8(v);\n\t\tp += 8;\n\t\tn -= 8;"),
    # table corruption: one entry off by one
    "table_entry": ("0xcccccccccccccccdULL", "0xccccccccccccccccULL") if "0xcccccccccccccccdULL" in src else ("0xccccccccccccccccULL", "0xccccccccccccccbbULL"),
    # swapped normalisation of negative exponents
    "neg_q_index": ("mul64(w << lz, pow5_tab[q - pow5_qmin], hi, lo);", "mul64(w << lz, pow5_tab[q - pow5_qmin + (q == -1 ? 1 : 0)], hi, lo);"),
    # locale check dropped
    "no_locale": ("if (dot && !decimal_point_is_dot()) { return false; }", ""),
    # rounding mode check dropped, or only for negative text (the first form of the bug)
    "no_rounding_guard": ("if (!rounds_to_nearest()) { return false; }", ""),
    "rounding_guard_neg_only": ("if (!rounds_to_nearest()) { return false; }", "if (neg && !rounds_to_nearest()) { return false; }"),
    # the decimal point is only looked at in its first byte, or asked for texts without a dot as well
    "locale_first_byte_only": ("return dp[0] == '.' && dp[1] == '\\0';", "return dp[0] == '.';"),
    "locale_always_checked": ("if (dot && !decimal_point_is_dot()) { return false; }", "if (!decimal_point_is_dot()) { return false; }"),
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

# The tests every mutant is built against: name -> (source, extra flags).
TESTS = {
    "fp_test": ("fp_test.cc", []),
    "rounding_test": ("rounding_test.cc", []),
    "locale_mock_test": ("locale_test.cc", ["-DLOCALE_TEST_MOCK=1"]),
}
# Mutants that one named test has to kill by itself.
REQUIRED_KILLER = {
    "no_rounding_guard": "rounding_test",
    "rounding_guard_neg_only": "rounding_test",
    "no_locale": "locale_mock_test",
    "locale_first_byte_only": "locale_mock_test",
    "locale_always_checked": "locale_mock_test",
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
    killers = []
    first = ""
    broken = False
    want = REQUIRED_KILLER.get(name)
    # A mutant with a named killer is run against that test alone; any other mutant against each test in turn, until one fails.
    for tname in ([want] if want else list(TESTS)):
        srcfile, flags = TESTS[tname]
        exe = os.path.join(d, tname)
        r = subprocess.run([cxx, "-std=c++20", "-O1", "-Wno-ignored-attributes", "-I" + d, "-I" + os.path.join(tree, "test"), *flags, os.path.join(tree, "test", srcfile), "-o", exe, "-pthread"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("%-24s BUILD FAILED (%s): %s" % (name, tname, r.stderr[:300]))
            broken = True
            break
        t = subprocess.run([exe], capture_output=True, text=True, cwd=d)
        if t.returncode != 0:
            killers.append(tname)
            first = ([l for l in t.stderr.splitlines() if l] or [""])[0][:90]
            break
    if broken:
        survived.append(name + "(build)")
        continue
    ok = bool(killers)
    print("%-26s %s %s %s" % (name, "KILLED  " if ok else "SURVIVED", ",".join(killers) or "-", first))
    if not ok:
        survived.append(name)
    sys.stdout.flush()
print("survivors:", survived if survived else "none")
sys.exit(1 if survived else 0)
