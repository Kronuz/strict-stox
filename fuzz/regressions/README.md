# Floating-point platform crash reproducers

`apple-strtofp-original.txt` is the exact 694-byte artifact from parser
fuzzing at strict-stox:21c104b on macOS 15.8.1 (24H32), x86_64. The fuzzer
aborts in Apple's `divideMPByMP`, `strtofp.c:1390`. The same assertion site
with a different input is recorded in
[the verification report](../../docs/perf/verification.md#defects-of-other-libraries-the-harness-found).

The standalone C program calls only libc. The C++ program calls the
strict-stox nothrow entry points, with and without a prefix index. Both
read the exact same file. Each conversion runs in its own process so a
libc abort cannot hide the other cases.

```sh
quiet check
quiet light strtofp-build 5 "Build platform reproducers" -- sh -c \
  'cmake -S fuzz/regressions -B .scratch/strtofp-proof -DCMAKE_BUILD_TYPE=Release && cmake --build .scratch/strtofp-proof --parallel 4'
quiet light strtofp-replay 5 "Replay platform regressions" -- \
  ctest --test-dir .scratch/strtofp-proof --output-on-failure
```

The regression project registers every libc and wrapper conversion in
CTest. These are crash reproducers: a subprocess abort is a failure, with
no `WILL_FAIL`, disabled test, or platform skip. They are deliberately
separate from the default seed corpus; the fuzz target does not filter
this input or suppress its abort. The replay results and minimized
artifact are pending the host's scheduled measurement window.
