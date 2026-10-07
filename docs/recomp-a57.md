# A57 recompiler measurements

This change provides a measured candidate for the stock-clock main-thread
budget. It does **not** establish a 30% frame-time improvement: the Switch must
be measured. The largest static improvements are in paired-single routines;
the general sample's instruction reduction is much smaller, although its
memory-instruction reduction is useful.

## Method

`tools/recomp/measure_a57.py` extracts the 32 profiled hot functions and an
evenly spaced deterministic sample across generated function bodies. One
sample overlapped the hot set, leaving 127 broad functions. `_abi` bodies are
selected where present; normal entry wrappers are excluded. There are 5,980
PPC instructions in the hot cohort and 5,071 in the broad cohort, including
unreachable padding in the emitted bodies. The same manifest is reused.

Compiler: MSYS2 Clang 22.1.8, `--target=aarch64-linux-gnu -std=gnu11 -O3
-march=armv8-a+crc+crypto -mtune=cortex-a57 -ffp-contract=off
-fno-strict-aliasing -D__SWITCH__`. No AArch64 GCC was found. Minimal target libc
declarations and Clang's target integer headers avoid needing a target sysroot.
Builtins remain enabled; `-ffreestanding` would incorrectly turn bit-cast
`memcpy` into library calls.

Assembly totals count actual instructions, including host spills, both sides
of branches and cold paths. An `ldp`/`stp` counts as one instruction. Cpu and
guest memory counts below are **optimized LLVM IR operations**, before pairing,
spilling and lowering; they are not bytes, words or executed accesses. Root
pointer provenance separates Cpu, guest window, base global and other globals.
The extracted corpus is compiled together, so its inlining opportunities also
differ from the full game build. These metrics compare static expansion, not
cycles, dynamic instruction counts or profile-weighted frame time.

## Results

| Metric | Hot before | Hot after | Broad before | Broad after |
|---|---:|---:|---:|---:|
| A57 instructions | 41,407 | 35,212 | 25,675 | 25,156 |
| Instructions / PPC instruction | 6.924 | 5.888 | 5.063 | 4.961 |
| Assembly load instructions | 9,310 | 8,845 | 6,857 | 6,435 |
| Assembly store instructions | 9,153 | 8,247 | 6,659 | 5,566 |
| Assembly branches, including calls/returns | 3,470 | 2,049 | 2,043 | 2,049 |
| Assembly endian swaps | 2,311 | 2,321 | 1,550 | 1,624 |
| IR Cpu loads | 6,634 | 6,479 | 4,734 | 4,337 |
| IR Cpu stores | 9,579 | 8,692 | 7,174 | 6,153 |
| IR guest loads | 1,994 | 1,775 | 1,144 | 1,199 |
| IR guest stores | 902 | 972 | 814 | 821 |

Hot instructions fall **15.0%**; broad instructions fall **2.0%**. Broad assembly
loads fall **6.2%** and stores **16.4%**. The two shared PSQ helpers occupy 281
instructions in both builds, separately from the function totals. Adding them
once to the hot totals gives 41,688 -> 35,493 (-14.9%). Production duplicates
static helpers across translation units as needed.

There are 51 static PSQ helper calls before and 120 after. Afterward those calls
are behind integer-type GQR tests. They must not be treated as increased
executed calls on float GQR paths, or as free work on integer GQR paths. Likewise,
the change in static guest-access counts reflects cold-path outlining and
compiler decisions, not changing guest memory semantics.

Some representative functions, in profile order:

| Function | PPC | A57 before | A57 after | Change |
|---|---:|---:|---:|---:|
| `f_02759E20` | 171 | 554 | 539 | -2.7% |
| `f_02017D3C_abi` | 18 | 114 | 114 | 0.0% |
| `f_028E9824_abi` | 21 | 1,005 | 283 | -71.8% |
| `f_0246C08C` | 336 | 2,636 | 2,466 | -6.4% |
| `f_028F4C44` | 29 | 243 | 208 | -14.4% |
| `f_0200B380` | 82 | 441 | 456 | +3.4% |
| `f_02010FFC` | 621 | 2,800 | 2,504 | -10.6% |
| `f_028E99C8_abi` | 57 | 1,764 | 737 | -58.2% |
| `f_0246C5EC` | 75 | 339 | 332 | -2.1% |
| `f_025170DC` | 300 | 2,424 | 2,402 | -0.9% |
| `f_028E9994_abi` | 13 | 587 | 246 | -58.1% |
| `f_028F4CB8` | 90 | 480 | 426 | -11.3% |

These reductions include outlining cold quantization. A -71.8% static reduction
in a matrix function is not a -71.8% execution-time prediction. Individual
regressions also exist; the JSON reports every selected function.

## Changes and correctness

1. CR initialization/reload groups complete four-byte fields into one native
   word load. Functions with actual guest/host CPU observations also group
   complete-field stores. Small leaves retain compiler-chosen byte/vector
   stores, which avoids a measured regression in the second hottest function.
   Byte locals, arbitrary byte values, partial writes, ABI register classes and
   the Cpu layout are unchanged. `memcpy` avoids alias/alignment violations;
   endian conversion preserves byte order on big-endian hosts too.
2. Ordered floating relations already return false for unordered operands.
   `__builtin_isunordered(a,b)` supplies the fourth CR bit, and the same four
   bits build FPSCR. Independent `isnan` checks and guards are unnecessary.
3. A must analysis over reachable CFG edges tracks each FPR lane that is
   definitely a promoted float. `round25` is the identity on these bit patterns,
   including zeros, subnormals, infinities and NaN payloads. Copies/merges and
   simultaneous paired writes use incoming facts; joins intersect, loops reach
   a fixed point, and calls/hooks invalidate facts. Unknown/double writes are
   not assumed single. Across the selected sources, `round25` sites fall from
   465 to 59. No double arithmetic is replaced with float arithmetic or FMA.
4. Switch PSQ wrappers inline float accesses and call the original quantizers
   for integer types. Tests use the actual current GQR on each access; they do
   not assume GQR0 remains zero. Guest accesses retain volatility and ordering.

## Audited alternatives

The corpus contains 76 indirect call sites. Their emitted GPR/LR/CTR traffic is
9..32 words stored and reloaded per site, median 26 each, before CR/FPR traffic.
The first hot function stores/reloads 21 words at each `bctrl`, seven of them
non-volatile GPRs. This is substantial, but simply treating dispatch as EABI
is unsafe: context/restore helpers and runtime hooks can change non-volatiles,
and nested preemption can observe cached ancestor state. Stale prologue save
values alone do not resolve the latter problem. Full synchronization remains;
the existing eligible guest count remains 9,645.

Non-volatile guest accesses with loop compiler barriers were tested and dropped.
The best loop-barrier variant saved only 0.7% hot instructions relative to its
preceding configuration and increased broad instructions 0.3%. Global barriers
also spilled FPR/Cpu state. The small result did not justify shared-memory and
polling risk. No per-page atomic increments were introduced.

Function-wide XER/FPSCR local caching was also dropped: relative to the preceding
configuration, instructions increased 2.0% hot and 2.7% broad. It increased
call synchronization and memory traffic. Unforced PSQ slow-path inlining
increased hot instructions about 4% versus baseline; explicit Switch outlining
was necessary. A packed-union CR local experiment did not improve the broad
sample and was dropped.

Endian swaps, address arithmetic, memory-base reloads and stack save/restore
were inspected in the assembly. Endian swaps are not the dominant retained
cost, and their counts do not improve overall. Guest 32-bit address wrapping,
architectural register values at observations, and `stmw`/`lmw` stack traffic
are retained; no unproved stack-store or register-initialization elimination
was added. The most general reduction is CR synchronization traffic.

## Reproduction and validation

The baseline is the previous generator's output (`build/gen`) with its `ppc.h` saved under
`build/a57-before/runtime/ppc.h`. `HOT` is a text file of hot guest function addresses (one per
line, e.g. from a `WWHD_PROFILE` run); `clang` is an AArch64-capable Clang.
An empty `WWHD_HOOKS` file recompiles the European executable without the USA-address hooks.

```sh
python tools/recomp/measure_a57.py build/gen build/a57-before --cc clang --hot HOT --include build/a57-before/runtime
WWHD_HOOKS=empty-hooks.txt python tools/recomp/recomp.py <dump>/code/cking.rpx build/gen-a57
python tools/recomp/measure_a57.py build/gen-a57 build/a57-after --cc clang --hot HOT --manifest build/a57-before/manifest.json
WWHD_RECOMP_TEST_CC=clang python tools/recomp/test_locals.py -v
```

Metrics, manifests, extracted C, optimized LLVM IR and assembly remain in
`build/a57-before` and `build/a57-after`; game-derived files are ignored.
Regeneration completes: 39,713 functions, 78 files, 9,645 eligible guest callees,
with the same 43 unhandled `op=0` instructions as baseline.

The 13 checks pass. At each of `-O0`, `-O2` and `-O3`, 1,655 synthetic function
pairs match over 64 states (full Cpu, observations and memory). Added cases
cover reaching-definition joins, backedges, calls/hooks, lane overlap, double
overwrites, partial CR fields and small leaves. Each executable also checks
100,000 promoted-float bit patterns and 8,192 combinations of GQR type, scale,
index and single/pair mode at unaligned guest addresses against the original
quantizer bodies. AArch64 syntax checks pass for all 78 generated units;
`ppc.h` passes desktop and Switch-macro C++20 checks. GCC 16 and the real Switch
build are not tested here.

Build `build/gen-a57` with the same Switch optimization flags, then compare
main-thread CPU milliseconds at stock handheld clocks in the same Outset scene.
Also test camera/matrix-heavy scenes, sailing/combat, NaN-sensitive collision
edges, save/load and thread scheduling. Integer-GQR-heavy code may pay extra
helper-call costs; measure it as well. `--no-single-rounds` provides a generator
ablation for the provenance pass. The stock-clock 31 ms budget remains a target
for the hardware measurement, not a verified outcome of this static metric.
