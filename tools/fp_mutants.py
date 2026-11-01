#!/usr/bin/env python3
"""Mutation check for the floating point fast path.

For each mutant: copy strict_stox.hh with one deliberate bug into a scratch dir, build
the floating point tests against it (test/fp_test.cc, test/rounding_test.cc and the mock build of
test/locale_test.cc) and require that at least one FAILS. A mutant that survives means the tests
would not catch that bug. Some mutants name the test that has to kill them (REQUIRED_KILLER): the
locale guard must not depend on an installed comma locale, which fp_test uses when there is one.

usage: tools/fp_mutants.py TREE_DIR [CXX] [pattern ...]     (scratch dir: $MUT_DIR or a temp dir)
patterns select mutants by name (shell patterns: slow_*); a pattern starting with ! excludes (!slow_*).
"""
import fnmatch
import os
import subprocess
import sys
import shutil
import tempfile

tree = os.path.abspath(sys.argv[1])
cxx = sys.argv[2] if len(sys.argv) > 2 else "g++"
only = sys.argv[3:]


def selected(name):
    include = [p for p in only if not p.startswith("!")]
    exclude = [p[1:] for p in only if p.startswith("!")]
    if include and not any(fnmatch.fnmatch(name, p) for p in include):
        return False
    return not any(fnmatch.fnmatch(name, p) for p in exclude)

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

    # ---- the exact path (fp_slow and Big): test/slow_test.cc has to kill each of these by itself ----
    # the digits: the sticky digit for what follows the first 768, the cap, the point, leading zeros
    "slow_no_sticky": ("\t\tif (sticky) {\n\t\t\tdg[nd++] = 1;\n\t\t\t--q;\n\t\t}\n", ""),
    "slow_digit_cap": ("inline constexpr int slow_digits = 768;", "inline constexpr int slow_digits = 700;"),
    "slow_sticky_frac": ("sticky = sticky || (nf > tf && any_nonzero(fb + tf, fe));", "sticky = sticky;"),
    "slow_sticky_zero": ("sticky = nf > tf && any_nonzero(b + tf, fe);", "sticky = false;"),
    "slow_leading_zeros": ("q -= static_cast<std::ptrdiff_t>(static_cast<std::size_t>(b - fb) + tf);", "q -= static_cast<std::ptrdiff_t>(tf);"),
    "slow_frac_digits": ("\t\t\ttake(fb, tf);\n\t\t\tq -= static_cast<std::ptrdiff_t>(tf);", "\t\t\ttake(fb, tf);"),
    # the syntax it accepts: it must decline what is not plain decimal text
    "slow_no_digits": ("\tif (ie == ib && fb == fe) { return 0; }\n\tstd::ptrdiff_t q = 0;", "\tstd::ptrdiff_t q = 0;"),
    "slow_exp_empty": ("\t\tif (++p == e) { return 0; }\n\t\tconst bool eneg", "\t\t++p;\n\t\tconst bool eneg"),
    "slow_exp_sign_empty": ("if ((eneg || *p == '+') && ++p == e) { return 0; }", "if (eneg || *p == '+') { ++p; }"),
    "slow_exp_junk": ("\t\t\tif (d > 9) { return 0; }\n\t\t\tif (q < 1000000000000000LL)", "\t\t\tif (d > 9) { break; }\n\t\t\tif (q < 1000000000000000LL)"),
    "slow_mant_junk": ("\t\tif ((static_cast<unsigned char>(*p) | 0x20) != 'e') { return 0; }\n\t\tif (++p == e) { return 0; }", "\t\tif (++p == e) { return 0; }"),
    "slow_exp_neg": ("\t\tif (eneg) { q = -q; }\n\t}\n\tif (!rounds_to_nearest()) { return 0; }", "\t}\n\tif (!rounds_to_nearest()) { return 0; }"),
    # the guards
    "slow_no_rounding_guard": ("\tif (!rounds_to_nearest()) { return 0; }\n", ""),
    "slow_no_locale_guard": ("\tif (dot && !decimal_point_is_dot()) { return 0; }\n\n\t// The significant digits", "\n\t// The significant digits"),
    # the screening of what is certainly infinity or zero
    "slow_no_screen_hi": ("if (lg - 1 >= tr::dec_hi || lg <= tr::dec_lo) { return 2; }", "if (lg <= tr::dec_lo) { return 2; }"),
    "slow_no_screen_lo": ("if (lg - 1 >= tr::dec_hi || lg <= tr::dec_lo) { return 2; }", "if (lg - 1 >= tr::dec_hi) { return 2; }"),
    # the comparison with the halfway point
    "slow_tie_even": ("m += m & 1;  // a tie: the even mantissa", "m += 1 - (m & 1);"),
    "slow_tie_up": ("m += m & 1;  // a tie: the even mantissa", "++m;"),
    "slow_tie_down": ("if (c < 0) {\n\t\t\t\tdone = true;\n\t\t\t} else if (c == 0) {", "if (c <= 0) {\n\t\t\t\tdone = true;\n\t\t\t} else if (c == 100) {"),
    "slow_loop_cap": ("for (int i = 0; i < 4 && !done; ++i) {", "for (int i = 0; i < 1 && !done; ++i) {"),
    "slow_cmp_shift": ("a.shl(q - mn);", "a.shl(q - mn + (q > 20 ? 1 : 0));"),
    "slow_cmp_t": ("b.mul(t);", "b.mul(t + (g > 0 ? 1 : 0));"),
    "slow_candidate": ("m0 = (hi >> (63 + u - nb)) >> 1;", "m0 = ((hi >> (63 + u - nb)) >> 1) + 1;"),
    "slow_candidate_exp": ("const int E = ((q * 217706) >> 16) + 63 + u - lz;  // the binary exponent of the leading bit", "const int E = ((q * 217706) >> 16) + 63 + u - lz + 1;  // the binary exponent of the leading bit"),
    "slow_emin": ("if (E >= 1 - tr::bias) {\n\t\tm0 =", "if (E >= -tr::bias) {\n\t\tm0 ="),
    "slow_final_renorm": ("\tif (m == (std::uint64_t(1) << (tr::mant + 1))) {\n\t\tm >>= 1;\n\t\t++eu;\n\t}\n\tconst std::ptrdiff_t be", "\tconst std::ptrdiff_t be"),
    # the screening of a subnormal or zero result without a division
    "slow_below_exp": ("if (E < -tr::bias) { return cand_below; }", "if (E < -tr::bias + 1) { return cand_below; }"),
    "slow_below_margin": ("return hn < t_hn - 256 ? cand_below : cand_unknown;", "return hn < t_hn + 256 ? cand_below : cand_unknown;"),
    "slow_below_normalise": ("const std::uint64_t hn = u != 0 ? hi : (hi << 1) | (lo >> 63);", "const std::uint64_t hn = hi;"),
    "slow_below_exact": ("if (static_cast<std::ptrdiff_t>(nd) * 100000 <= -q * 69897) { return 2; }", "if (true) { return 2; }"),
    "slow_below_exact_const": ("-q * 69897) { return 2; }", "-q * 79897) { return 2; }"),
    # the 128 bit comparison
    "slow_small_t": ("c = cmp_small(w, static_cast<int>(q), 2 * m + 1, eu - 1);", "c = cmp_small(w, static_cast<int>(q), 2 * m, eu - 1);"),
    "slow_small_nd": ("const bool small = nd <= 19 &&", "const bool small = nd <= 20 &&"),
    "slow_small_pow5": ("mul64(t, pow5_small.v[-q], b.hi, b.lo);", "mul64(t, pow5_small.v[-q + 1], b.hi, b.lo);"),
    "slow_small_shift": ("return cmp128(shl128(a, static_cast<unsigned>(s)), b);", "return cmp128(shl128(a, static_cast<unsigned>(s + 1)), b);"),
    "slow_small_side": ("a = U128{0, D};", "a = U128{0, D + 1};"),
    "slow_shl128_carry": ("return U128{(v.hi << s) | (v.lo >> (64 - s)), v.lo << s};", "return U128{(v.hi << s), v.lo << s};"),
    "slow_cmp128": ("if (a.lo != b.lo) { return a.lo < b.lo ? -1 : 1; }\n\treturn 0;\n}\ninline int cmp_small", "if (a.lo != b.lo) { return a.lo > b.lo ? -1 : 1; }\n\treturn 0;\n}\ninline int cmp_small"),
    # the division, for the subnormal range
    "slow_div_tie": ("if (c > 0 || (c == 0 && (m & 1) != 0)) { ++m; }", "if (c > 0) { ++m; }"),
    "slow_div_exact": ("const bool exact = N.zero();", "const bool exact = false;"),
    "slow_div_exp": ("const std::ptrdiff_t E = (Big::cmp(x, y) >= 0 ? r0 : r0 - 1) + q;", "const std::ptrdiff_t E = r0 + q;"),
    "slow_div_clamp": ("\t\tif (eu < tr::qsub) { eu = tr::qsub; }\n", ""),
    # what it returns, and what the API does with it
    "slow_exact_subnormal": ("return exact ? 0 : 2;", "return 2;"),
    "slow_subnormal_carry": ("if (m >= (std::uint64_t(1) << tr::mant)) { return 0; }", "if (false) { return 0; }"),
    "slow_zero_status": ("if (m == 0) { return 2; }", "if (m == 0) { return 0; }"),
    "slow_no_overflow": ("\tif (be >= tr::max_be) { return 2; }\n\tconst bits_t b =", "\tconst bits_t b ="),
    "slow_sign": ("out = std::bit_cast<R>(static_cast<bits_t>(b | (static_cast<bits_t>(neg) << top)));\n\treturn 1;", "out = std::bit_cast<R>(b);\n\treturn 1;"),
    "slow_zero_sign": ("out = std::bit_cast<R>(static_cast<bits_t>(static_cast<bits_t>(neg) << top));", "out = std::bit_cast<R>(static_cast<bits_t>(0));"),
    "slow_errno": ("*errno_save = status == 2 ? ERANGE : 0;", "*errno_save = 0;"),
    "slow_idx_on_range": ("\t} else if (status == 2) {\n\t\tout = 0;\n\t}", "\t} else if (status == 2) {\n\t\tout = 0;\n\t\tif (idx != nullptr) { *idx = s.size(); }\n\t}"),
    "slow_decline_idx": ("\tif (status == 1) {\n\t\tif (idx != nullptr) { *idx = s.size(); }\n\t} else if", "\tif (status == 1) {\n\t} else if"),
    "slow_decline_zero": ("\t} else if (status == 2) {\n\t\tout = 0;\n\t}", "\t} else if (status == 2) {\n\t}"),
    "slow_no_throw": ("if (status == 2) { throw std::out_of_range(std::string(name) + \": Out of range value: \" + std::string(str)); }", "if (status == 2) { return fast; }"),
    # the big integers
    "slow_pow5_chunk": ("while (k >= 27) { mul(pow5_small.v[27]); k -= 27; }", "while (k >= 27) { mul(pow5_small.v[27]); k -= 26; }"),
    "slow_shl_high": ("l[top] = l[n - 1] >> (64 - b);", "l[top] = 0;"),
    "slow_cmp_order": ("if (a.l[i] != b.l[i]) { return a.l[i] < b.l[i] ? -1 : 1; }", "if (a.l[i] != b.l[i]) { return a.l[i] < b.l[i] ? 1 : -1; }"),
    "slow_from_digits": ("\t\t\tmul(pow10_small.v[k]);\n\t\t\tadd(chunk);", "\t\t\tmul(pow10_small.v[k]);"),
    # the short path (at most 19 digits: no digit array)
    "slow_short_digits10": ("return static_cast<std::size_t>(t + 1 - (w < pow10_small.v[t] ? 1 : 0));", "return static_cast<std::size_t>(t + 1);"),
    "slow_short_q": ("\t\tnd = digits10(wall);\n\t\tq -= static_cast<std::ptrdiff_t>(fe - fb);", "\t\tnd = digits10(wall);"),
    "slow_short_limit": ("if (nall <= 19) {", "if (nall <= 20) {"),
    "slow_short_digits": ("if (have_dg) { X.from_digits(dg, nd); } else { X.set(w); }", "X.from_digits(dg, nd);"),
    # ---- the integer range and the first byte route (the API, not the harness: verify.cc does not call them) ----
    # an integer out of range: the status, the value, idx, and that strto* is not called for it (test/slow_test.cc)
    "int_range_value": ("if (__builtin_mul_overflow(acc, ub, &acc) || __builtin_add_overflow(acc, static_cast<U>(d), &acc)) { return 2; }",
                        "if (__builtin_mul_overflow(acc, ub, &acc) || __builtin_add_overflow(acc, static_cast<U>(d), &acc)) { out = 1; return 1; }"),
    "int_range_add": ("if (__builtin_mul_overflow(acc, ub, &acc) || __builtin_add_overflow(acc, static_cast<U>(d), &acc)) { return 2; }",
                      "if (__builtin_mul_overflow(acc, ub, &acc)) { return 2; } acc += d;"),
    "int_range_limit": ("if (neg ? acc > lim + 1 : acc > lim) { return 2; }", "if (neg ? acc > lim + 2 : acc > lim) { return 2; }"),
    "int_range_idx": ("\t\t\t\t\tout = static_cast<Out>(0);\n\t\t\t\t\treturn 2;", "\t\t\t\t\tout = static_cast<Out>(0);\n\t\t\t\t\tif (idx) { *idx = str.size(); }\n\t\t\t\t\treturn 2;"),
    "int_range_zero": ("\t\t\t\t\tout = static_cast<Out>(0);\n\t\t\t\t\treturn 2;", "\t\t\t\t\tout = static_cast<Out>(1);\n\t\t\t\t\treturn 2;"),
    "int_range_declined": ("\t\t\t\t\tif (status == 0) { return 0; }\n\t\t\t\t\tout = static_cast<Out>(0);\n\t\t\t\t\treturn 2;", "\t\t\t\t\treturn 0;"),
    # the first byte of a text that is not decimal: routed to strto*; a decimal text must not be
    "slow_start_no_dot": ("return c - '0' <= 9 || c == '.';", "return c - '0' <= 9;"),
    "slow_start_no_sign": ("\tif (c == '-') {\n\t\tif (n == 1) { return false; }\n\t\tc = static_cast<unsigned char>(p[1]);\n\t}\n", ""),
    "slow_start_sign_dot": ("c = static_cast<unsigned char>(p[1]);\n\t}\n\treturn c - '0' <= 9 || c == '.';", "c = static_cast<unsigned char>(p[1]);\n\t\treturn c - '0' <= 9;\n\t}\n\treturn c - '0' <= 9 || c == '.';"),
    "slow_start_digit_top": ("return c - '0' <= 9 || c == '.';", "return c - '0' < 9 || c == '.';"),
    "slow_start_always": ("return c - '0' <= 9 || c == '.';", "return true;"),
    # an integer of at most 19 digits: one conversion of a 64 bit integer (fp_test)
    "int_tie_truncate": ("b = std::bit_cast<bits_t>(static_cast<double>(w));", "b = std::bit_cast<bits_t>(static_cast<double>(w & ~1ULL));"),
    "int_tie_exponent": ("} else if (q == 0) {", "} else if (q <= 0) {"),
    "int_tie_declined": ("} else if (q == 0) {", "} else if (false) {"),
}


# The tests every mutant is built against: name -> (source, extra flags).
TESTS = {
    "fp_test": ("fp_test.cc", []),
    "rounding_test": ("rounding_test.cc", []),
    "locale_mock_test": ("locale_test.cc", ["-DLOCALE_TEST_MOCK=1"]),
    "slow_test": ("slow_test.cc", ["-DSTRICT_STOX_COUNT_LIBC=1"]),
}
# Mutants that one named test has to kill by itself. The mutants of the exact path and of the route of a text (slow_*) and of the
# integer range (int_range_*) are killed by slow_test unless named here.
REQUIRED_KILLER = {
    "no_rounding_guard": "rounding_test",
    "rounding_guard_neg_only": "rounding_test",
    "no_locale": "locale_mock_test",
    "locale_first_byte_only": "locale_mock_test",
    "locale_always_checked": "locale_mock_test",
    "slow_no_locale_guard": "locale_mock_test",
}
for _name in MUTANTS:
    if _name.startswith("slow_") or _name.startswith("int_range_"):
        REQUIRED_KILLER.setdefault(_name, "slow_test")
# A mutant that never returns (a loop without an exit) is as dead as one that fails.
RUN_TIMEOUT = 300


work = os.environ.get("MUT_DIR") or tempfile.mkdtemp(prefix="stox-mut-")
os.makedirs(work, exist_ok=True)
survived = []
for name, (old, new) in MUTANTS.items():
    if not selected(name):
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
        try:
            t = subprocess.run([exe], capture_output=True, text=True, cwd=d, timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired:
            killers.append(tname)
            first = "(timed out)"
            break
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
