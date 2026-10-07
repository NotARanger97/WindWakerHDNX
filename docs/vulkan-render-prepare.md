# Render preparation reductions (2026-10-05)

This is an unmeasured Switch candidate for the 25.5 -> 19 ms render budget.
The changes target draw-count-dependent work, including the 32.1 ms village
view. No Switch build or gameplay measurement has been performed.

| Change | Expected dock saving, ms/frame (estimate) | Correctness boundary |
| --- | ---: | --- |
| Content-keyed shader pairs and alternating fetch ranges | 1.5–2.5 | Existing validated program hashes and fetch identity; exact state interning; original translation on misses. |
| Per-shader support upload reuse | 0.5–1.5 | Fresh packing and final AO/texture patches; exact CPU byte comparison; same device/submission; immutable slice. |
| Final image descriptor memo and removal of redundant checks | 0.3–0.8 | Actual view, surface epoch/device, sampler slot/generation, compare/integer/patch/effective anisotropy; feedback bypasses memo. |
| Dense descriptor ranks and flat descriptor cache | 0.5–1.1 | Complete binding order/type implied by layout, exact resource identities; incomplete plans retain binding/type words. |
| Compact register masks and proven-equal shadow skips | 0.2–0.5 | Same classification for every register; shadow equality tokens, including repair when equality is unknown. |

These are hypotheses from the measured phase budget, not measured gains, and
are not guaranteed additive. A roughly 3–6 ms saving would be useful; reaching
19 ms requires the upper end. CPU scratch/copies on uniform misses, additional
lookup work on shader misses, cache capacity churn and shared-L2 pressure can
erase gains. Use hardware results to keep or disable individual memos.

## Shader state and pair identity

`drawStateGenerations.translation` tracks the exact union of the existing VS/PS
`translation_state` inputs, including partial masks and all optional streamout
strides. Program/fetch switches leave it unchanged. Draw's direct primitive
store is checked separately. Unknown shader registers still advance the old
conservative shader generation and force pair resolution. Context restore
uses the same write path; save-state invalidation advances the new stamp too.
Untracked arrays gather fresh state on every pair lookup.

The VS gather includes the common words and all three stages' sampler words.
Append only PS texture words to form the pair state. Its count plus ordered
`(index, nonzero value)` words is an exact serialization. An open-addressed
table compares these words before assigning an ID. The pair key is five words:
state ID, VS program hash, PS program hash, fetch key and fetch pointer. Shader
objects supply the original immutable pipeline/binding metadata. Translation
keys, disk recipes and shader content validation remain unchanged.

At 2,048 states both tables clear before IDs can repeat; the pair table also
clears at 8,192 pairs. Shader-memo resets clear both tables and the gathered
state stamp. Successful pairs alone are inserted, and insertion/hit reuse
requires the captured program validation stamp to remain current. New shader
variants still use the original translator and warm-up machinery.

The Switch fetch range memo has 64 slots, checking address, size register,
both step rates and the same program validation epoch as the original memo.
Standalone calls clear it. Desktop keeps fresh compact-header reads.

## Uploads, textures and descriptor keys

Support bytes are freshly packed into ordinary CPU scratch and patched before
looking in a 256-slot shader memo. Exact byte equality reuses an upload only in
the same submission and device. Collisions merely evict; mapped upload memory
is never read by this memo, including uncached fallback allocations. Changed
bytes take the original snapshot path, including alignment, minimum size,
padding and flush bookkeeping. Renderer state resets invalidate the memo.

Texture upload validation remains on the original guest surface, and an AO
replacement receives its own check. The duplicated original-surface check is
removed. The caller skips only the same read-only layout transition that
`transition_image` already skips. View selection and feedback copying still
run under their existing guards. The final descriptor memo verifies freshly
resolved view/epoch/device and effective sampler inputs before returning the
immutable `VkDescriptorImageInfo`. Texture scales remain fresh.

For a complete rank plan, writes already occupy every sorted binding slot;
only precomputed uniform ranks need gathering into dynamic-offset order. A
compact descriptor key includes layout and count. Layout fixes all binding
numbers/types for complete plans; incomplete/disabled ranks explicitly encode
binding/type per write and use a separate key tag. All keys include UBO buffer
and range, regular offsets, view/sampler/layout; dynamic offsets remain outside
descriptor identity and fresh for binding. Hash collisions compare exact words.
Pool/submission/surface invalidation and deferred descriptor creation retain
their existing lifetime rules. Warm table lookups allocate nothing; new keys
grow contiguous metadata/key arenas, rather than allocating string/map nodes.

## Validation and hardware measurements

MSYS2 Clang 22 C++20 syntax checks passed for `draw.cpp`, `shaders.cpp`,
`backend.cpp` and `gx2_core.cpp` in desktop and `__SWITCH__` modes, plus GX2
without Vulkan. Host preprocessing is not a devkitA64/libnx build.

```sh
clang++ -std=c++20 -O2 -Wall -Wextra -Iruntime/src runtime/tools/vulkan_prepare_test.cpp -o prepare_test
./prepare_test --bench
python tools/switch/test_render_prepare.py clang++
```

The first test checks table growth/probing/clear/prefixes and allocation-free
warm finds, shader identity collisions, size/device/submission/reset changes,
and reuse with an intentionally unreadable mapped pointer. A synthetic host
benchmark of 600 18-word keys measured 21 ns/find for the flat table versus
27 ns for string/unordered_map; this does not predict A57 timings.

The second extracts production functions and tests them with CPU stubs in
desktop/Switch and fused-small/bulk modes. It checks all 65,536 registers and
every bit against both production shader gathers, with streamout on/off;
checks sparse versus dense masks; exercises shadow repair, equal tokens,
cross-block/bulk writes, group stamps and invalid bounds; compares cached
pairs to the original translation keys through both capacity limits; tests
fetch epoch/range/size/divisor/standalone behavior; tests descriptor identities,
complete/partial keys, image memo dependencies and uniform rank ordering.
The production Switch register mask table uses **159,252 bytes**, versus the
old **1,048,576-byte** table. These tests do not exercise a GPU or libnx ABI.

New options default to 1 on Switch and opt-in on desktop:
`WWHD_VK_SHADER_PAIR_MEMO`, `WWHD_VK_SUPPORT_UPLOAD_REUSE`, and
`WWHD_VK_IMAGE_DESCRIPTOR_MEMO`. Only explicit `1` enables an override.
Fetch memo uses the existing `WWHD_VK_FETCH_MEMO`. Descriptor ranks use the
existing `WWHD_VK_DESCRIPTOR_RANKS`; table/mask representation changes are
unconditional where used. Compare to the previous commit for a full baseline.

Measure stationary dock and stationary village views with `WWHD_VK_PHASES=1`
and `WWHD_VK_STATS=1`, then repeat timing without stats. Check `sh-translate`,
`sh-fetch`, `pack`, `support`, `textures`, `ranks`, `desc-hit`, descriptor binds
and render ms/frame. New logs report pair hits, state gathers skipped, resident
state/pair counts and table MiB, alternating fetch hits, and support upload
hits/avoided bytes. Disable each new option separately to identify regressions.
Compare captures/renderer smoke and gameplay through shadow/scene passes,
AO modes, sampler/anisotropy changes, feedback, area loads, save states and
asynchronous submission/persistent-pool turnover.
