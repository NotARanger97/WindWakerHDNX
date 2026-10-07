# Recompiler round 2: indirect dispatch and register synchronization

Measured on regenerated EU output in `build/gen-r2` (no USA-address hooks). No game-derived output is tracked. The baseline is the unchanged
round-1 generator output in `build/gen-r2-baseline`.

## Changes

- **14,966 indirect call sites** (`bctrl`, `blrl`, conditional forms) use a
  host-thread-local `{target, epoch, function}` cache. A hit calls the normal
  dispatch entry directly. A miss uses the checked runtime resolver and updates
  the cache. Indirect tails and default jump-table transfers keep table dispatch.
- CFG may-dirty tracking removes redundant stores after full/partial
  synchronization. Backward liveness then removes reloads whose values cannot
  reach a local read or a subsequent dirty flush. Returns and all full observers
  receive the same optimization. There are no runtime dirty bits.
- Reachable guest leaf access/write summaries narrow direct-call barriers.
  **296 additional leaves** have `_sync` bodies, reached by **1,601 ordinary
  direct call sites**; the 9,645 existing `_abi` leaves also use precise summaries.
  These contracts include nonvolatile and fixed-register outputs.
- The dispatch text table now uses relaxed atomic function pointers. Replacing
  a non-null mapping publishes an epoch change; adding a new mapping preserves
  other sites' cached hits. The cache epoch is loaded with acquire ordering.
  TLS prevents races between host threads; recursive calls can replace their
  site's cache because the active function pointer is copied before invocation.

## Correctness

An indirect target is **not assumed EABI-safe**. Vtables/function pointers can
reach ordinary compiler code, unnamed save/restore helpers, hooks, imports or
runtime-registered host functions. Every dirty cached field is written back
before these calls, making the entire Cpu coherent. Fields already coherent
need no store. All locals needed afterward are reloaded, including nonvolatile
outputs; other outputs remain authoritative in Cpu.

Saving/restoring stale nonvolatiles is insufficient justification for a blanket
shortcut: custom restore helpers may restore their enclosing frame, callers may
consume nonvolatile outputs, and nested preemption/context observers can see
ancestor state. No such shortcut is used. Cached pointers enter the normal
wrapper and retain `PPC_ENTER`.

Precise guest contracts cover only reachable leaves with no nested guest entry,
external guest tail/fallthrough, indirect dispatch, hook/site, named custom ABI,
trap or unimplemented instruction. Ordinary imports retain their existing EABI
contract and `custom_abi` exclusions. Callee prologue saves and `mfcr` are actual
inputs; all guest writes, including r1/r2/r13 and CR2-CR4, are actual outputs.
FPRs remain in Cpu throughout and need no register-cache synchronization.

The `f_028F6A70` enclosing-r31 restore is a `_sync` leaf. Its callers either
reload the restored r31 when a later local read needs it, or leave it in Cpu
without a redundant return/tail store. The old sine-selector overwrite cannot
occur. Synthetic tests exercise this pattern and consume every register class
after custom output calls.

Fast direct entries perform trace/preemption checks in the caller. The cold
preemption path flushes all dirty locals and reloads live/dirty locals. The leaf
body has no second entry check, preserving the existing solution to the
check-then-partial-flush race.

Dirty sets union at CFG joins and backedges. Conditional calls and jump-table
default observers cannot clear dirtiness on bypass edges. Local liveness counts
future dirty stores as reads, so write-only and conditional definitions cannot
overwrite a callee's output with a stale local. The analysis conservatively
treats an instruction containing `if`/`switch` as having no unconditional kills.

## Static A57 results

Clang 22.1.8, the unchanged `measure_a57.py` A57 compilation flags, 32 profiled
functions and the same 127-function broad sample selected by the baseline
manifest. Instruction totals include cold cache/preemption blocks and host
spills; Cpu operations are optimized LLVM IR loads/stores. These are static
counts, not execution counts or a frame-time estimate.

| Metric | Baseline | Round 2 | Change |
| --- | ---: | ---: | ---: |
| Hot instructions | 35,212 | 31,508 | -10.52% |
| Hot load instructions | 8,845 | 6,830 | -22.78% |
| Hot store instructions | 8,247 | 6,096 | -26.08% |
| Hot Cpu loads | 6,479 | 4,166 | -35.70% |
| Hot Cpu stores | 8,692 | 6,095 | -29.88% |
| Broad instructions | 25,156 | 23,681 | -5.86% |
| Broad Cpu loads | 4,337 | 2,849 | -34.31% |
| Broad Cpu stores | 6,153 | 4,361 | -29.12% |

Largest hot instruction reductions: `02575B70` 9,297 → 7,203 (-22.5%),
`0246C08C` 2,466 → 1,816 (-26.4%), `027F1FA8` 3,388 → 2,964 (-12.5%),
`025170DC` 2,402 → 2,082 (-13.3%). Some small functions increase because the
cache compare/miss path costs more static instructions than a single external
dispatch call; that baseline count excludes the dispatcher/lookup body.

Across the 14,966 emitted indirect calls, explicit synchronization stores fall
from 295,009 to 134,597, and reloads from 297,351 to 90,769: an average removal
of **10.72 stores + 13.80 loads per site**. Each complete CR-word operation is
counted once. These are generated-source sync operations, including unreachable
sites, before compiler pairing/vectorization; cache accesses are not included.

A synthetic one-indirect-call path with dirty r14-r31 followed by an ordinary
import measures 51 → 30 A57 instructions for synchronization alone, with
18 fewer load/store instructions. Including the cache produces 54 static
instructions (cold path included); its hit path contains 45, six fewer than
the baseline caller, and additionally bypasses runtime dispatch/lookup. This
illustrates why there is no single instruction delta for every indirect call.

The selected real functions with cache expansion removed, retaining identical
sync code, measure 30,713 hot instructions (-12.78%) and 22,772 broad
instructions (-9.48%). This isolates the static cache cost; compare cache on/off
on hardware to establish the value of avoiding table misses.

Artifacts: `build/a57-r2-baseline`, `build/a57-r2`, `build/a57-r2-no-icache`,
`build/a57-micro-{before,dataflow,after}`, `build/indirect-sync-counts.json`.
The hot set is a list of the game thread's hottest functions from a profile.

## Validation and target testing

- 18 register-cache tests; 1,734 synthetic differential pairs × 64 states at
  `-O0/-O2/-O3`, comparing every Cpu byte, observation hash and guest-memory byte.
  Coverage includes custom register outputs, fixed/SP outputs, CR packing,
  reservations, joins, loops, conditional observers, jump tables, hooks,
  fallthrough, preemption, and 64 deterministic CFG sweeps.
- Actual dispatcher/cache test at `-O0/-O2/-O3`: hit/miss lookup counts, all
  address domains, replacement, new callback registration, recursive calls,
  concurrent host threads and unchanged unknown-target diagnostics.
- All 78 code files plus table/imports pass desktop and `__SWITCH__` header-path
  Clang syntax checks. `core.cpp` passes native C++20 syntax checking.
- The real output retains 39,713 functions, 78 code files and the same 43
  unhandled `op=0` instructions as the baseline.

Regenerate from the repository root (an empty `WWHD_HOOKS` file recompiles the European executable
without the USA-address hooks; `HOT` is a file of hot guest function addresses, one per line):

```sh
WWHD_HOOKS=empty-hooks.txt python tools/recomp/recomp.py <dump>/code/cking.rpx build/gen-r2
python tools/recomp/measure_a57.py build/gen-r2 build/a57-r2 --cc clang \
  --hot HOT --manifest build/a57-r2-baseline/manifest.json
```

Build with `GEN_DIR` pointing at `build/gen-r2`. Test boot/title and the sine/cosine
initialization first, then Outset gameplay, heavy views, transitions, save/load
and thread scheduling. Measure main-thread CPU time and compare a regeneration
with `--no-icache`. `--no-sync-dataflow` isolates CFG pruning; `--no-abi` disables
leaf/import contracts. Combining these switches restores the old full-cache
barrier mode. `--no-locals` restores direct Cpu code and disables call caches.

Switch GCC 16/SDK linking and gameplay cannot be validated here. Cache benefit
depends on target stability and the actual TLS implementation; a polymorphic
site can repeatedly miss. Cache storage is 16 bytes/site on AArch64, about
234 KiB per host thread before linker dead stripping. Cold code size and TLS
costs need target measurement. Existing ordinary-import EABI assumptions remain;
new imports with arbitrary register/context access must extend `custom_abi`.
