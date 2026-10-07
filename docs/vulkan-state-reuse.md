# Vulkan draw state reuse

`WWHD_VK_STATE_REUSE` defaults to enabled on Switch and opt-in on desktop.
Only `1` enables an explicit override; `0` runs the original resolution paths.
The shortcuts apply only to `gx2::regs()`. Other register arrays retain their
previous behavior. All stamps and resolved objects are owned by the render
thread; this adds no DC-flush work or per-page atomic counters.

Small packets classify each changed word with the existing shader/pipeline
masks and the added fetch/target/texture/sampler groups in `shader_key_dirty.h`.
Bulk packets use the same predicates. Identical writes do not advance stamps;
shadow stores are skipped only when a block equality token proves the shadow
already matches. Otherwise the shadow is repaired. Context switches apply the saved
registers through this path, including primitive type changed directly by draw.
ALU-only packets still copy directly. Save-state loads invalidate every group.
Unknown shader registers retain the existing conservative dirty mask.

## Dependencies of each shortcut

| Resolved object | Required unchanged dependencies |
| --- | --- |
| Fetch shader | Tracked register identity; fetch start/size and both instance step-rate registers; existing watched program-write epoch. Switch only: desktop still reads compact headers freshly. `WWHD_VK_FETCH_MEMO=0` also disables this outer fetch shortcut. |
| VS/PS pair | Tracked register identity; existing shader generation and masks; primitive low six bits (draw writes this directly); fetch object/key; program validation stamp. The stamp is the existing watched write epoch on Switch and frame on desktop. Resetting shader memos also clears the outer resolved state. |
| Attachments and slices | Target generation, PS object/output mask, frame, surface lifetime epoch, and GPU-written eligibility generation. Target registers include all eight color base/size/info/tile/frag/view groups, depth base/size/info/view/HTILE-size/depth-slice convention, color control/target mask, depth control, and both generic scissor words. Scissor participates in LatteMRT's active color mask. Frame changes force the full path because resolution/aspect/shadow scale is latched there. Cache the guest attachments before private AO substitution or extent filtering. |
| Sampled guest surface | Register identity, all seven resource words for this stage/unit, depth-compare mode, surface lifetime epoch, and the selected address's alias generation. Surface descriptors remain immutable. Surfaces sharing a normalized address share an alias stamp: adding a candidate, changing GPU-written eligibility, or changing write sequence at an ambiguous address invalidates selection. Uploads and CPU surface-copy fallback publish these changes too. Writes to unrelated addresses do not advance this alias stamp. |
| Sampled view | All seven resource words, actual surface pointer (after AO substitution), and surface lifetime epoch. Image retirement advances the epoch before clearing views; catalog insertion and save-state surface reset also advance it. Feedback views always use the original copy path. |
| Sampler | All three words for its hardware slot, device, compare/integer mode, effective anisotropy setting, and whether the AO filter patch applies. Effective anisotropy is checked freshly, including GPU-written/mipmap eligibility. Untracked calls invalidate the generation shortcut for that slot and retain exact word comparison. |
| Pipeline | Existing pipeline generation/masks, immutable VS/PS/fetch objects, final topology and attachment pointers, and surface lifetime epoch. Attachment pointers plus epoch imply the formats used in the key. AO substitutions/filtering happen first. A miss still enters the existing exact pipeline memo/key/map path. Pipeline reset clears this outer pointer before it can be reused. |
| Descriptor set/key | Exact resource identity after fresh preparation: layout, binding/type/order, UBO buffer/range and regular offset, sampled view/sampler/layout, and descriptor-pool submission generation. With a complete valid rank plan, compare each identity as it is prepared, avoiding a second descriptor walk as well as serialization/map lookup. The prepared count must equal the full plan count and the submission generation must remain unchanged throughout preparation. Partial/invalid plans and unequal identities use the existing exact comparison/key path. Dynamic UBO offsets remain fresh and participate in bind reuse, not descriptor identity; register generations alone cannot skip uniform preparation. |

Surface creation/destruction can conservatively invalidate unrelated resolved
surfaces/pipelines. Alias write-sequence invalidation is confined to the shared
guest address. Private AO and feedback copies clear the copied address-stamp
pointer because they do not belong to the guest alias catalog.

Guest-memory uniform packing/snapshots, upload validation (including the original
guest surface before AO redirection), texture-scale patches, feedback copies,
image transitions, render-pass checks, dynamic state and command/submission
binding guards still run. Register stamps cannot establish that guest-memory
contents or Vulkan command state are unchanged.

`preparationStatsEnabled` is one process-cached boolean, without a function call
or local-static guard on each use. The per-stage program-hash memos retain their
entries on forward frame/epoch changes: their exact validation stamps already
reject stale entries. Standalone hashing, rewinds and explicit reset still clear
them, preventing another stage's old stamp from becoming valid again.

Clear, surface-copy and scan-copy commands borrow the immutable queued structs
directly in Vulkan. Payload entry points consume the same big-endian structure
fields synchronously and retain no payload pointers. Vulkan records handles and
copies command arguments; guest texture/index/uniform snapshots retain their
existing ownership. Metal retains the guest-scratch dispatch path. Projection
commands still copy the matrix only when aspect adjustment changes it.

## Validation and hardware checks

Host Clang C++20 syntax checks cover the modified Vulkan and GX2 sources;
`__SWITCH__` host preprocessing checks are not a libnx/aarch64 build.
The standalone CPU regressions passed in desktop and Switch-default modes,
including explicit option overrides. A temporary harness using the production
register-application/context functions and surface write helper passed identical
and changed small/bulk writes, shadow repair, all texture/sampler slots, fetch
and target dependencies, masked stencil changes, full context restore, invalid
register bounds, group invalidation and shared/unrelated alias epochs. Both fused
small-register and bulk fallback modes passed. Run renderer smoke on hardware.
An isolated production descriptor-helper harness also passed dynamic/regular
UBO offset handling, range/binding/count/layout changes, view/sampler/image-layout
changes, preparation-order versus rank-order checks, and serialized key equality.

Compare Outset CPU profiles and visuals with `WWHD_VK_STATE_REUSE=0/1`.
With `WWHD_VK_STATS=1`, `[vulkan state reuse]` reports skipped fetch/shader-pair,
attachment, sampled-surface/view and pipeline resolutions per frame. Existing
pipeline/shader counters now count the lookups actually entered.

Exercise identical register rewrites, uniform-only changes, context switches,
save-state loads, primitive/topology changes, scissor-driven attachment disable,
MRT masks and array slices, raw-fetch divisor changes, same-address shader writes
published through DC flush/store or shader invalidation, same-address texture
aliases with alternating GPU writes/uploads/CPU copies, swizzles and depth
comparison, resolution/aspect changes, private AO/replay, anisotropy toggles,
feedback sampling, asynchronous submission/pool resets and `WWHD_MEM2_COPY=1`.
No Switch CPU timing improvement was measured for this change alone.

Further draw preparation changes and their validation/measurement boundaries
are documented in [vulkan-render-prepare.md](vulkan-render-prepare.md).
