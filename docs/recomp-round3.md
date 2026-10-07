# Recompiler round 3: flag liveness and leaf inputs

EU output is regenerated in `build/gen-r3`, untracked. This is a small measured
improvement, not evidence that the game thread reaches 18 ms. Moving 22.3 to
18 ms requires approximately 19.3% less elapsed time; the static reductions
below are much smaller, and do not predict cache misses or frame time.

## Changes and correctness

- Backward CFG liveness removes CR and XER flag assignments overwritten on
  every path before a read, call, hook or exit. The FPSCR compare nibble at
  bits 12..15 is tracked separately from the rest of FPSCR: its masked update
  overwrites that nibble without reading its previous value. Joins union live
  sets and loops reach a fixed point. Every function exit observes all flags;
  every guest/runtime call conservatively observes them too. Conditional writes
  have no definite kills. Unknown Cpu helpers are observations. Sticky SO and
  compound/chained updates remain intact. Definitions take effect after their
  RHS; fcmp's new CR bits remain available to its FPSCR update. No FP arithmetic,
  guest memory operation, rounding rule or FMA contraction changes.
- Proven leaf contracts distinguish incoming values from fields merely touched.
  Backward liveness includes all possible outputs at exits, so a conditional
  write still needs its input on the bypass path. Prologue saves, mfcr,
  read/modify/write instructions and inherited ordinary-import arguments are
  inputs. Unconditional overwrite-only destinations need no pre-call store.
  Output reloads, fixed/nonvolatile register handling, eligibility checks and
  the caller's full cold preemption synchronization are retained. Dirty output
  values become authoritative in Cpu after the callee, as in round 2.

These are semantics-preserving generator passes on both platforms; there is no
new Switch-only runtime behavior. Guest loads/stores remain volatile, including
polling loops. Existing atomic reservations and host fences retain their ordering.
Stack saves/restores and all guest-memory contents remain observable.

## Static results

Same Clang 22.1.8 A57 flags and manifest as round 2: 32 hot functions, 127 broad
functions, 5,980 and 5,071 PPC instructions respectively. Assembly counts include
cold paths, both sides of branches and spills. Cpu/guest operations are optimized
LLVM IR operations, not bytes or executed accesses. This is not profile-weighted.

| Metric | Hot round 2 | Hot round 3 | Broad round 2 | Broad round 3 |
| --- | ---: | ---: | ---: | ---: |
| A57 instructions | 31,508 | 31,013 (-1.57%) | 23,681 | 23,501 (-0.76%) |
| Assembly loads | 6,830 | 6,758 | 5,523 | 5,504 |
| Assembly stores | 6,096 | 5,980 | 4,476 | 4,409 |
| IR Cpu loads | 4,166 | 4,068 (-2.35%) | 2,849 | 2,797 (-1.83%) |
| IR Cpu stores | 6,095 | 5,984 (-1.82%) | 4,361 | 4,307 (-1.24%) |
| IR guest loads | 1,775 | 1,775 | 1,201 | 1,201 |
| IR guest stores | 972 | 972 | 821 | 821 |
| Endian swaps | 2,329 | 2,326 | 1,632 | 1,633 |

The two PSQ helpers remain 281 instructions, separately from function totals.
Leaf-input pruning alone measures 31,197 hot and 23,526 broad instructions;
flag pruning supplies the remaining reduction. A second assembly comparison at
`-O2` (the Switch CMake default) gives hot 30,650 -> 30,142 (-1.66%) and broad
23,005 -> 22,796 (-0.91%). These are still Clang results, not devkitA64 GCC.

| Hot function | Before | After | Change |
| --- | ---: | ---: | ---: |
| `f_02759E20` | 573 | 572 | -0.17% |
| `f_02010FFC` | 2,423 | 2,279 | -5.94% |
| `f_0246C08C` | 1,816 | 1,789 | -1.49% |
| `f_025170DC` | 2,082 | 2,028 | -2.59% |
| `f_02008974` | 308 | 287 | -6.82% |
| `f_02575B70` | 7,203 | 7,035 | -2.33% |
| `f_024F349C` | 346 | 349 | +0.87% |

Across the complete generated source, explicit CA assignments fall
19,188 -> 16,406 (-14.5%) and FPSCR compare-nibble assignments
10,021 -> 8,741 (-12.8%). These source counts include unreachable code and do
not count hidden helper accesses. Round25 sites remain 4,067 and to_single sites
40,539. The real RPX retains 39,713 functions, 78 code files, 9,645 ABI leaves,
296 additional sync leaves, 14,966 cached indirect sites and 43 unhandled op=0
instructions.

## Alternatives measured and rejected

- Plain memory with a read/write compiler barrier describing the entire guest
  window at labels, entry/exit and calls: 32,127 hot / 23,966 broad instructions,
  worse than the baseline, with more Cpu traffic. This requires synchronization
  boundaries to survive inlining and still needs a shared-memory correctness
  contract. It is not retained.
- Plain scalar accesses only outside conservative backward-edge intervals:
  31,481 / 23,659 instructions, less than 0.1% improvement. Polls stay volatile,
  but acyclic concurrent reads still need justification. The tiny result does
  not justify relaxing their semantics.
- Guarded nonwrapping lmw/stmw pointer walks with original wrapping fallbacks:
  32,064 / 24,255. Volatile aggregate variants are larger still. The static
  totals include fallback duplication; no executed-cycle claim follows.
  Guest stack stores cannot be deleted based on host-register liveness.
- Always-inline copies of proven leaves of up to eight PPC instructions:
  31,891 / 24,122. A three-instruction limit gives 31,468 / 23,695; its small hot
  saving does not cover the broad regression or justify extra generated bodies.
  Larger limits expand more cold paths and calls. No new inlining is retained.
- Proven promoted-float round-trip elimination is bit-exact in synthetic tests,
  but removes zero to_single sites in this RPX; it is not retained.

Clang already cancels swaps for raw load/store copies and byte-reversed
instructions, including an ldr/str copy in the first hot function. Swaps are not
the largest retained cost. Guest 32-bit effective-address wrapping prevents
unconditionally using native pointer-offset addressing. FP arithmetic stays
double with explicit float rounding and `-ffp-contract=off`. Existing jump-table
and compare/branch lowering remain unchanged.

## Validation and reproduction

Twenty register-cache checks pass, including 1,794 synthetic differential pairs
times 64 states at each of `-O0`, `-O2` and `-O3`. They compare every Cpu byte,
observation hash and guest-memory byte. New cases cover leaf input joins,
backedges, conditional outputs, import arguments, stwcx outputs, flag consumers,
record/OE forms, masked FPSCR updates, self-reads, early returns and site hooks.
Forty-nine repeated flag-producing instruction pairs supplement the CFG cases.
Existing promoted-float and quantizer sweeps remain green. The actual dispatcher
test also passes at all three optimization levels. All 78 code units plus table
and imports pass AArch64 Clang syntax checks with desktop and `__SWITCH__`
header paths; ppc.h passes both C++20 header checks. GCC 16/SDK linking and
gameplay have not been tested.

```sh
WWHD_HOOKS=empty-hooks.txt python tools/recomp/recomp.py <dump>/code/cking.rpx build/gen-r3
python tools/recomp/measure_a57.py build/gen-r3 build/a57-r3 --cc clang --hot HOT --manifest build/a57-r3-baseline/manifest.json
WWHD_RECOMP_TEST_CC=clang python tools/recomp/test_locals.py -v
WWHD_RECOMP_TEST_CC=clang python tools/recomp/test_dispatch.py -v
```

The original generator output and header are in `build/gen-r3-baseline` and
`build/r3-baseline/runtime`; measurements are in `build/a57-r3-baseline`,
`build/a57-r3-inputs` and `build/a57-r3`. Per-function reports, manifests, source,
IR, assembly and O2 comparisons remain untracked in build/.

`--no-dead-flags` and `--no-leaf-inputs` disable the two passes independently;
using both reproduces round 2. Environment equivalents are
`WWHD_RECOMP_DEAD_FLAGS=0` and `WWHD_RECOMP_LEAF_INPUTS=0`.
Build with `GEN_DIR=build/gen-r3` and compare main-thread ms/frame on the same
Outset dock and heavy views at the established clocks. Measure each ablation,
then test boot, collision/NaN edges, sailing/combat, save/load and scheduling.
The important remaining risks are target-compiler code shape and cache behavior;
these static gains do not establish an 18 ms budget.
