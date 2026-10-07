The recompiler caches each function's touched GPRs, CR bits, LR and CTR in C
locals by default. Regenerate the output to change this setting:

```sh
python tools/recomp/recomp.py game/code/cking.rpx build/gen
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-abi
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-locals
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-single-rounds
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-icache
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-sync-dataflow
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-dead-flags
python tools/recomp/recomp.py game/code/cking.rpx build/gen --no-leaf-inputs
```

`WWHD_RECOMP_LOCALS=0` also disables the cache. ABI-aware synchronization is
enabled by default; `WWHD_RECOMP_ABI=0` or `--no-abi` removes ABI/leaf contracts
while keeping locals. `WWHD_RECOMP_SYNC_DATAFLOW=0` or `--no-sync-dataflow`
restores function-wide dirty sets and reloads. `WWHD_RECOMP_ICACHE=0` or
`--no-icache` restores table dispatch at indirect calls. `--no-locals` bypasses
all three optimizations. The report records modes, eligible guest callees and
cached indirect call sites.
These are generation options on every platform, not runtime settings.

Round 3 adds backward flag liveness and upward-exposed leaf inputs. Flag stores
are removed only when every path overwrites them before a read or observation;
CR bits, XER CA/OV/SO and the FPSCR compare nibble are analyzed separately.
Exits, calls and site hooks observe all flags. Sticky/compound updates and
unrecognized helpers remain conservative. Leaf contracts now flush actual
incoming values needed by the leaf, rather than every field it touches. A
conditional output remains an input when its bypass path needs the old value;
all output and preemption contracts are retained. `--no-dead-flags` /
`WWHD_RECOMP_DEAD_FLAGS=0` disables flag pruning. `--no-leaf-inputs` /
`WWHD_RECOMP_LEAF_INPUTS=0` restores the round-2 touched-field contracts.
Using both switches reproduces round-2 output. See
[round 3 measurements](../../docs/recomp-round3.md).

Every touched local is initialized, including fields only conditionally written.
At an eligible ordinary call the GHS PowerPC EABI barrier writes back
function-written r0, r3-r12, CR0/CR1/CR5-CR7 bits, LR and CTR, plus
function-written r1/r2/r13. On return it reloads only touched volatile fields,
including write-only fields: a subsequent full flush must preserve callee
changes. r14-r31 and CR2-CR4 stay in locals; r1/r2/r13 are assumed preserved.
CFG dataflow narrows stores to fields potentially dirty since the last
synchronization and reloads to locals needed afterward. Joins union dirty/live
sets; conditional calls and jump-table default dispatch do not kill dirtiness
on their other edges. Return/tail/observer stores are reads for liveness, so a
conditionally written or write-only local is reloaded whenever a later flush
could use it. No runtime dirty bits or counters are added.

Guest callee eligibility is checked over reachable instructions and CFG edges,
including conditional branches, backedges and jump-table cases. The check is
deliberately stronger than "reads a non-volatile before writing it": **any**
reachable non-volatile access rejects the callee. Thus ordinary prologue stores,
`stmw`, `mfcr`, and unnamed restore helpers are excluded from the legacy `_abi`
classification. A second classification summarizes incoming cached fields needed
and fields written by proven leaves. Direct calls to these `_sync` bodies flush
the required dirty inputs and reload the modified live fields, including r1/r2/r13,
r14-r31 and CR2-CR4 outputs. There is no save/restore or EABI preservation
assumption for guest leaves. Unreachable padding does not affect the contract.

Preemption requires another restriction: eligible guests cannot reach a nested
guest entry, external guest tail transfer, fallthrough, unknown dispatch, site
hook, trap or unimplemented instruction. A later asynchronous preemption request
at a nested entry could otherwise require registers still cached in an ancestor.
Eligible guests have a normal `f_X` wrapper that runs `PPC_ENTER` before local
initialization and tail-calls `f_X_abi` or `f_X_sync`. A direct eligible call
performs the same trace/preemption check in the caller and enters the fast body
directly. When preemption is taken, **all dirty** locals are flushed before
`ppc_preempt`, and live/dirty locals are reloaded afterward. Other fields are
already coherent in Cpu. The fast entry performs no second
check, so there is no check-then-partial-flush race. Normal entry addresses and
the runtime dispatch table are unchanged.

Ordinary `imp_*` imports use the ABI barrier. Imports and guest symbols matching
`setjmp`, `longjmp`, `savegpr`/`restgpr`, `savefpr`/`restfpr`,
`OSSwitchFiber`, `OSSwitchStack`, `__OSSwitchStack`, and
`OSLoadContext`/`OSSaveContext`/`OSSetContext`/`OSGetContext` use full barriers.
The name check is case-insensitive and accepts prefixes/suffixes. OS imports
whose names contain Thread, Mutex, Semaphore, Cond, Event or Message also use
full barriers because scheduling/waits can expose parked CPU state. Unnamed
guest leaves with custom inputs/outputs use their actual access/write summaries;
named custom helpers remain excluded. This does not implement non-local control
flow itself; it only preserves
the existing synchronization at those calls. Newly added imports with custom
register contracts or whole-CPU observations must extend `custom_abi`.

Runtime function hooks and instruction hooks remain conservative: their C
implementations can inspect/modify arbitrary CPU fields or call original code.
Indirect `bctrl`/`blrl` calls also retain full barriers because their dynamic
targets can be context helpers or runtime-registered host functions. Each site
uses a host-thread-local target/function cache; hits call the normal dispatch
entry directly, including its preemption check. Mapping replacements invalidate
caches with a global epoch. New addresses cannot invalidate a cached hit and
do not increment the epoch. Miss handling is shared in the runtime. Unknown
targets retain the same fatal diagnostic. Indirect tails remain table dispatch.
Returns, all `MUSTTAIL` transfers, default jump-table dispatch, fallthrough,
traps and unimplemented instructions flush every dirty field. Returning full
observers reload every live local. Internal gotos and jump-table cases retain
locals. See [round 2 measurements and correctness](../../docs/recomp-round2.md).

CR compare/record, copy, packing and masked-write helpers are expanded with the
same types and side effects as `runtime/include/ppc.h`. This also lets the C
optimizer eliminate CR assignments overwritten before an observation.
`ppc_stwcx` overwrites CR0 in `Cpu`, so its live bits are reloaded immediately.
FPRs, XER, FPSCR, GQRs and reservations stay in `Cpu`; their audited helpers
need no register-cache barrier. A new helper taking `c` must be audited in
`ppc2c.py`; otherwise generation fails rather than emitting unsynchronized code.

Complete CR fields are loaded/reloaded as words using alias-safe `memcpy`
helpers, while individual locals keep their original byte types. Functions
with guest calls, hooks or other CPU observations also pack complete-field
stores into words. Partial writes and stores in small leaves stay byte stores;
all four bytes, including non-boolean values, retain their original meaning.
Floating CR comparisons use `__builtin_isunordered` and ordered C relations;
their CR bits and FPSCR updates are unchanged.

The generator also tracks definitely promoted single-precision FPR lanes over
reachable CFG edges. Only `round25` on such a lane is removed: promotion from
float leaves at least 29 low significand bits zero, whereas `round25` clears
27 and adds bit 27. Arithmetic stays double with the same final float rounding.
Joins intersect facts; backedges participate in the fixed point; guest calls,
imports, dispatch, traps and hooks discard them. Copies/merges track each lane
separately; `lfd` and other double/bit writes invalidate their destination.
`--no-single-rounds` or `WWHD_RECOMP_SINGLE_ROUNDS=0` disables this pass without
changing register-local or ABI modes. The report records its effective mode.

On Switch, PSQ float loads/stores are always inlined and integer quantization
is outlined. Each access tests the current GQR type; no constant GQR assumption
is made. Types 0..3 retain float semantics and ignore scale; types 4..7 use the
original quantizer. Desktop keeps the generic inline path. Guest memory
accesses remain volatile, and indirect/custom/context observers still receive
coherent Cpu state. See [the A57 measurements](../../docs/recomp-a57.md) for the static
results, rejected experiments, limits and target validation.

Run the synthetic check without game data:

```sh
python tools/recomp/test_locals.py -v
WWHD_RECOMP_TEST_CC=clang python tools/recomp/test_locals.py -v
WWHD_RECOMP_TEST_CC=clang python tools/recomp/test_dispatch.py -v
```

With Clang in `PATH` (or a compiler path in `WWHD_RECOMP_TEST_CC`), the check
compiles generated C with real musttail attributes and compares direct access,
full cache barriers and ABI barriers at
`-O0`, `-O2` and `-O3`: full `Cpu` state, snapshots at observations and guest
memory. It covers extended opcodes, CR/XER/FP/reservation helpers, conditional
calls/returns/traps, backedges, jump tables, hooks and fallthrough. Added cases
check all ABI register classes, repeated calls with dirty non-volatiles,
write-only volatile changes, custom context calls, real guest entry wrappers,
nested guest calls, custom leaf outputs, dirty/live CFG joins, deterministic
CFG sweeps, and preemption requested during a fast callee. The dispatcher test
compiles the actual dispatch section of `core.cpp` and tests cache hits/misses,
replacement, address domains, recursion, host threads and unknown targets.
GCC used for
this check must support `musttail` (15+). Switch GCC 16 compilation and gameplay
profiling still need to be checked on the target: regenerate/build each mode,
compare behavior and main-thread frame time on Outset Island. Also test save/load
and thread scheduling while calls are active; inspect the report's eligible
callee count and generated barriers before interpreting performance differences.
