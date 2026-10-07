# Vulkan draw CPU preparation

These changes reduce repeated CPU preparation. They do not change upload memory
types, command submission, or presentation. Switch frame time still needs to be
measured on hardware.

## Changes and invariants

- **Index and feedback-copy scratch:** render-thread-local vectors retain their
  capacity. Every draw clears the index vector, including native-index draws.
  Uploads copy the active index bytes before scratch reuse; Vulkan copies image
  region descriptions during command recording. Neither retains CPU pointers.
- **Direct index conversion and extent elision:** when every declared vertex
  binding fits imported MEM2 and vertex-window diagnostics are off, index bounds
  cannot affect any vertex snapshot. Skip the extent reduction. Specialized
  conversion writes every active uint32 directly into an arena slice after
  texture/pass preparation, with the same width/endian/restart/primitive reader
  as the vector path. The arena tracks dirty bytes and owns the slice until its
  submission fence completes; padding stays zero. Wrapped BE addresses, copied
  vertex bindings, diagnostics and disabled specialization retain CPU scratch.
  Full imported bindings are counted at their declared size when no prefix is
  computed; this counter does not represent an actual MEM2 copy.
- **Pipeline and descriptor keys:** lookups borrow serialized bytes through
  `string_view`; stored strings and borrowed views use the same hash and exact
  binary equality. Descriptor bytes fit fixed stack capacity. Pipeline overflow
  scratch retains capacity between calls. Cache misses still allocate persistent
  entries and create Vulkan objects where required.
- **Pipeline state generation:** small and bulk register writes advance a
  separate generation only when a serialized pipeline input changes. Context
  replacement advances it when relevant values differ; save-state loading
  always advances it. The shortcut also
  compares the register-file identity, fetch object and fragment hash, both
  shader keys, topology, and attachment formats. Standalone register arrays
  always take the original serialization path. Stencil reference, viewport,
  scissor and blend constants remain freshly processed as dynamic state.
- **Sampler slots:** each of the 18 slots in each stage has its own memo. Hits
  require the device, all three words, compare/integer mode, and effective
  anisotropy setting to match. AO-patched words participate in the comparison.
  The full cache uses four integer words instead of a temporary string key.
  Renderer initialization resets both the memos and sampler cache.
- **Program hash lookup:** desktop retains once-per-frame byte hashing;
  standalone calls always rehash. Switch framed calls use an atomic write epoch
  instead of the frame, with process-lifetime watches for every 4 KiB page holding
  a hashed VS/PS/fetch program (128 KiB bitmap). `switch_dc_store` publishes an
  epoch after cache cleaning when a flush intersects a watched page, including
  compact fetch headers. This works with the MEM2 copy fallback too. Nearby
  writes may conservatively invalidate, but unrelated pages do not. Shader-bit
  `GX2Invalidate` invalidates globally before texture-only early exits, including
  zero-size and all-range requests. Page watches survive address reuse and resets.
  A write during hashing leaves the memo stamped with the older epoch, forcing
  the next lookup to rehash; guest code must publish shader writes through its
  DC flush/store or shader invalidation, as it does for the real GPU.
  Separate VS/PS/fetch memos compare program address/size and validation stamp;
  translation also checks register identity/generation, primitive, and the VS
  fetch key/object. PS does not depend on fetch. Warm Switch fetch hits read no
  guest header bytes. Existing save-state memo resets clear all hash/memo entries
  while retaining compiled variants.
- **Register application:** small Vulkan packets use masks classified once for
  the selected renderer/options. The masks are identical to the prior predicates,
  including conservative unknown registers; each batch bumps each generation at
  most once. Shadow writes still happen even when the live value matches. ALU
  constant-only packets have no shader/pipeline inputs and copy directly to live
  and shadow registers (statistics still count changed batches when enabled).
  Register packets bypass the large execution switch and borrow queue/list
  payloads synchronously. Projection packets copy only when aspect adjustment
  modifies the matrix. Full context restores apply the saved register values and
  advance generations only for changed inputs. See [Vulkan state reuse](vulkan-state-reuse.md)
  for the outer draw caches and their complete dependencies.
- **Raw fetch divisors:** the raw fetch parser reads both instance step-rate
  registers. Those values now participate in its key and last-fetch comparison,
  so a changed rate cannot reuse metadata parsed with an earlier divisor.
Compact fetch programs continue to use their encoded divisors.
- **Surface invalidation:** an interval index replaces scanning all cached
  surfaces. A cached surface is published after its first full guest-memory
  check, with the original base byte extent and separate address-library mip
  ranges. Until then it is already dirty with no completed check, so invalidation
  cannot change its state. Cached guest descriptors and their calculated sizes
  stay immutable; physical image rescaling does not alter them. Private images
  are excluded. Save-state reset clears the index, and subsequent full checks
  republish it. Queries retain the original strict overlap predicate, GPU-written
  and special-address exclusions, and pending-dirty suppression. Multiple
  intersecting ranges invalidate/count a surface only once.
- **Uniforms:** packing remains fresh on every draw. Register generations do
  not track guest writes to remapped uniform memory, and texture scales/AO
  adjustments are applied after packing. Existing scratch reuse remains intact.

## Reviewed Switch defaults

`WWHD_VK_SHADER_KEY_DIRTY`, `WWHD_VK_FETCH_MEMO`, `WWHD_VK_DESCRIPTOR_RANKS`,
`WWHD_VK_SAMPLER_MEMO`, `WWHD_VK_SKIP_REDUNDANT_BINDS`, and
`WWHD_VK_SPECIALIZE_INDICES` now default to enabled under `__SWITCH__`.
Explicit values retain their previous meaning: only `1` enables an option.
Desktop defaults remain opt-in.

The shader dirty masks retain all state-hash inputs; unknown registers stay
conservative. Fetch memoization checks range/step-rate registers and uses the
platform-specific byte validation described above. Descriptor ranks preserve increasing
binding order, with fallback for invalid/duplicate metadata. Redundant descriptor
binds are skipped only for equal command, submission generation, pipeline layout,
sets and dynamic offsets within a tracked pass. Specialized indices retain
width/endian/restart semantics and the original fallback for wrapped BE addresses.

## Validation

Standalone tests require only a C++20 compiler, and work without game data or a GPU:

```sh
clang++ -std=c++20 -O2 -pthread -Wall -Wextra -Iruntime/src runtime/tools/vulkan_draw_test.cpp -o vulkan_draw_test
./vulkan_draw_test
clang++ -std=c++20 -O2 -pthread -D__SWITCH__ -Iruntime/src runtime/tools/vulkan_draw_test.cpp -o vulkan_draw_switch_test
./vulkan_draw_switch_test
WWHD_VK_TEST_OPTION=0 ./vulkan_draw_switch_test
WWHD_VK_TEST_OPTION=1 ./vulkan_draw_switch_test
```

The CPU regressions compare vector and mapped-span conversion against the
original expansion for every primitive/index width/endian/restart combination,
including short/empty conversions, unaligned sources and output boundary guards.
They also check shader watches, address reuse, both sides of page boundaries,
zero/disjoint/full/wrapped flushes and concurrent watch/notification publication.

For this change, Clang C++20 syntax checks passed for `gx2_core.cpp`, `shaders.cpp`,
`surfaces.cpp` and `draw.cpp`, with and without `__SWITCH__` (host compiler only;
no libnx build). On hardware, repeat Outset Island CPU sampling and verify area
loads, same-address shader replacement followed by DC flush/store, shader-bit
invalidates, save-state reload, raw fetch divisor changes, strips/restart and
fan/quad expansion. Also compare `WWHD_MEM2_COPY=1`, vertex-window diagnostics
and `WWHD_VK_SPECIALIZE_INDICES=0` fallback paths. No Switch timing gain has been
measured here.

Tests compare indexed interval queries against a brute-force overlap oracle,
including nested/duplicate/mip-like ranges, boundaries, zero sizes, cache growth,
reset and 32-bit address overflow. Binary-key tests cover spill/reset, collisions
and allocation-free warm lookups. Index tests compare the original draw loops
against specialized conversion across every supported primitive, both widths,
both byte orders, restart markers, unaligned input and short/empty draws. Warm
index conversion is checked for heap allocations.

On Switch, compare Outset Island frame time, malloc/free, pipeline map lookups,
shader last hits and sampler memo hits with `WWHD_VK_STATS=1`. Run renderer smoke
for same-frame interior mip invalidation/readback. Check textured gameplay, AO,
shadow/depth passes, sampler changes, strips/fans/quads, and save-state reloads.
Repeat with the six switches explicitly set to `0` for visual comparison.
