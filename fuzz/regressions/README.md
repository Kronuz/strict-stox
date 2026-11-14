# Floating-point platform crash reproducers

The same 209-byte minimized input aborts Apple's libc `strtod` and both strict-stox `stod`
entry forms in `divideMPByMP`, `strtofp.c:1390`, on x86_64 macOS 15.8.1 (24H32).
The libc-only executable contains no strict-stox code. This establishes an external libc
assertion, rather than attributing the abort to a wrapper or sanitizer error.

| Artifact | Bytes | SHA-256 |
|---|---:|---|
| apple-strtofp-original.txt | 694 | 4f287364996e7452a478c34d8c77e7ba9b34b87b6c30b50e0e124943a884d648 |
| apple-strtofp-minimal.txt | 209 | 1c427b3bc8efe231014b1cabd4e8e6bf78025d18bc3175aeae0edea14d3d04f2 |

The original artifact comes from parser fuzzing at `strict-stox:21c104b`. The independent
replay programs are `strict-stox:21212a1`. The same assertion site on a different input is
recorded in [the verification report](../../docs/perf/verification.md#defects-of-other-libraries-the-harness-found).
The product header is unchanged by this regression evidence.

## macOS replay results

Both artifacts produce this matrix. The C++ program calls nothrow entry points, with and
without a prefix index. Each conversion runs in a separate process so one abort cannot
hide another conversion's result.

| Conversion | libc-only | strict-stox | strict-stox with prefix index |
|---|---|---|---|
| float | Pass | Pass | Pass |
| double | SIGABRT at strtofp.c:1390 | SIGABRT at strtofp.c:1390 | SIGABRT at strtofp.c:1390 |
| long double | Pass | Pass | Pass |

[The complete replay log](replay-macos.txt) records 12 passing tests and 6 subprocess aborts
across the two artifacts. CTest exits 8. These are crash reproducers: the aborts remain real
failures, with no `WILL_FAIL`, disabled test, or platform skip. The fuzzer does not filter
these inputs or suppress the assertion. The artifacts are separate from the default seeds
so a routine seed smoke does not claim that this platform defect is fixed.

## Reproduction

Run from the repository root:

```sh
quiet check
quiet light strtofp-build 5 'Build independent replay programs' -- sh -c \
  'cmake -S fuzz/regressions -B .scratch/strtofp-proof -DCMAKE_BUILD_TYPE=Release && cmake --build .scratch/strtofp-proof -j4'
quiet light strtofp-replay 5 'Replay both platform artifacts' -- \
  ctest --test-dir .scratch/strtofp-proof --output-on-failure
quiet light strtofp-libc-minimal 5 'Replay minimized input through libc only' -- \
  .scratch/strtofp-proof/libc_strtofp_repro strtod fuzz/regressions/apple-strtofp-minimal.txt
quiet light strtofp-wrapper-minimal 5 'Replay same minimized input through wrapper' -- \
  .scratch/strtofp-proof/strict_stox_strtofp_repro stod fuzz/regressions/apple-strtofp-minimal.txt
```

The C-only program uses NUL-terminated input and `strtof`, `strtod`, or `strtold`; it neither
includes the wrapper header nor links sanitizer libraries. The C++ program reads the same
file bytes into a bounded string view. Both use the same byte fixture, not reconstructed
numeric text.
