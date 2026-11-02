# stoi on 1 to 9 digits: 752cf9f against dc24d9f (issue 2)

Outcome: not a regression on the i9. The 24% gap does not reproduce, and the 1 to 9 digit loop has no code of its own to
blame: `fast_int` is byte for byte what it was. What moves is the placement of the inlined loop, which shifts the row by
more than the claimed gap in either direction. Kept as a trade; no code change.

Intel Core i9-9980HK, macOS 15, Apple clang 17, `-O3 -std=c++20`, ns per number, best of 9 samples inside the program,
4 rounds interleaved under `quiet strict`. `bench/bench.cc` unchanged at each commit (the `new` column of the `stoi 1-9
digits` row), built without the `from_chars` and fast_float columns.

| Binary | Rounds (ns) | Median |
|---|---|---:|
| 752cf9f | 13.6, 13.8, 13.9, 14.4 | 13.85 |
| 752cf9f again (A/A) | 13.5, 13.6, 13.8, 13.7 | 13.65 |
| dc24d9f (head) | 13.3, 13.3, 13.3, 13.2 | 13.3 |

A/A spread 1.5%. Head against 752cf9f: 2.5% faster (13.3 against 13.65 to 13.85), not 24% slower.

Placement test: a one-row program (`strict_stoi` over 1,024 random 1 to 9 digit texts, two identical lambdas back to back),
6 rounds interleaved.

| Binary | First lambda (ns) | Identical second lambda (ns) |
|---|---:|---:|
| 752cf9f | 13.9 to 14.2 | 14.1 to 14.3 |
| 752cf9f, `-mbranches-within-32B-boundaries` | 12.9 to 13.9 | 13.1 to 14.0 |
| dc24d9f | 9.1 to 9.4 | 11.1 to 11.5 |
| dc24d9f, `-mbranches-within-32B-boundaries` | 9.1 to 9.5 | 11.1 to 11.4 |

The same source in the same binary differs by 20% between two call sites (9.2 against 11.3 ns on head), and 752cf9f is
slower than head in both. The JCC erratum flag changes nothing, so the cause is not the 32-byte boundary rule. The
10.5 ns of the issue is one layout of the larger benchmark program; the figure follows the surrounding code, not the loop.

Not measured here: the M4 `stod` rows (5 to 8%): this host is the i9. The notes of the issue already say any branch placed
in `fast_fp` costs those rows 2% to 8% on the M4, and the entry check is what keeps declined text at START's cost; that is
a kept trade.

What would reopen it: a layout-independent measurement (the same row in two different program contexts) that shows head
slower than 752cf9f by more than the 20% placement swing.
