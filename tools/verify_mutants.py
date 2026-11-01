#!/usr/bin/env python3
"""Does the independent harness (tools/verify.cc) catch the bugs tools/fp_mutants.py plants?

For each mutant of tools/fp_mutants.py: build verify.cc against the mutated header and run
hard, syntax (length 5), dmid (20k) and rand (150k per format, no cross oracles), stopping at the
first command that reports a mismatch. A mutant the harness does not kill is listed. The ones
that only make the reader decline more (the text then goes to libc, which is right), the locale
mutants (the harness never changes the locale) and the mutants of the route of a text and of the
integer range (slow_start_*, int_range_*: the harness checks values, and these change which function
decides) are expected to survive: test/fp_test.cc, test/locale_test.cc and test/slow_test.cc kill those.

usage: tools/verify_mutants.py TREE CXX WORKDIR [JOBS] [only-name ...]
"""
import concurrent.futures as cf
import os
import shutil
import subprocess
import sys

tree = os.path.abspath(sys.argv[1])
cxx = sys.argv[2]
work = os.path.abspath(sys.argv[3])
jobs = int(sys.argv[4]) if len(sys.argv) > 4 else 3
only = set(sys.argv[5:])
here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(tree, "strict_stox.hh"), encoding="utf-8").read()

code = open(os.path.join(tree, "tools", "fp_mutants.py"), encoding="utf-8").read()
code = code[: code.index("# The tests every mutant")]
code = code.replace("tree = os.path.abspath(sys.argv[1])", "tree = %r" % tree).replace('cxx = sys.argv[2] if len(sys.argv) > 2 else "g++"', "cxx = 'c++'").replace("only = sys.argv[3:]", "only = []")
ns = {}
exec(code, ns)
MUTANTS = ns["MUTANTS"]

COMMANDS = [("hard", ["hard", "-t", "2"]), ("syntax", ["syntax", "-n", "5", "-t", "2"]), ("dmid", ["dmid", "-n", "20000", "-t", "2"]), ("rand", ["rand", "-n", "150000", "-t", "2", "--no-cross"])]


def one(item):
    name, (old, new) = item
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    if old not in src:
        return name, "PATCH DID NOT APPLY", ""
    open(os.path.join(d, "strict_stox.hh"), "w", encoding="utf-8").write(src.replace(old, new, 1))
    exe = os.path.join(d, "verify")
    r = subprocess.run(["nice", "-n", "19", cxx, "-std=c++20", "-O1", "-Wno-ignored-attributes", "-I" + d, os.path.join(here, "verify.cc"), "-o", exe, "-pthread"], capture_output=True, text=True)
    if r.returncode != 0:
        return name, "BUILD FAILED", r.stderr[:200]
    for cname, args in COMMANDS:
        t = subprocess.run(["nice", "-n", "19", exe] + args, capture_output=True, text=True)
        if t.returncode != 0:
            first = ([l for l in t.stderr.splitlines() if l] or [""])[0][:100]
            shutil.rmtree(d, ignore_errors=True)
            return name, "KILLED by " + cname, first
    shutil.rmtree(d, ignore_errors=True)
    return name, "SURVIVED", ""


items = [(n, v) for n, v in MUTANTS.items() if not only or n in only]
survivors = []
with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
    for name, verdict, first in ex.map(one, items):
        print("%-26s %-18s %s" % (name, verdict, first))
        sys.stdout.flush()
        if not verdict.startswith("KILLED"):
            survivors.append(name)
print("survivors:", survivors if survivors else "none")
sys.exit(1 if survivors else 0)
