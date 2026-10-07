# Vulkan surface image reuse

## Allocation paths

Guest surfaces already live in `R.surfaces`, an address multimap with no per-frame
eviction. Different formats/sizes at one address can coexist. Scan buffers and
private AO targets also retain their images until their format/size changes.
Resolution/aspect changes replace render images using a preserving resample.
Offscreen presentation captures allocate temporary images only when requested.

The recurring allocation path is attachment feedback: each alias used to create
a snapshot and defer its destruction by default. The opt-in feedback cache kept
only one shape per stage/unit, so alternating target shapes replaced it repeatedly.
Switch now retains four shapes per stage/unit within the existing 128 MiB budget.
Excess shapes and oversized copies still use temporary images. Desktop retention
remains opt-in with `WWHD_VK_REUSE_FEEDBACK_IMAGES=1`.

## Identity and contents

A CPU-owned guest surface matches its normalized base address, mip address, full
guest format, width/height, pitch, slices, mip count, dimension, tile mode,
swizzle and depth interpretation. Empty dimensions/counts normalize to one;
render-target descriptors request one mip and no separate mip address. Macro-tile swizzle
bits embedded in register addresses are separated from the real guest base,
as sampled texture descriptors already do. Depth registers now decode their
tile-mode field, matching descriptors obtained from GX2DepthBuffer.

The Vulkan usage mask is derived from format capabilities and always includes
sampling and both transfer directions, plus attachment usage where supported.
Rendering checks attachment usage and the requested resolution/aspect before
reuse. GPU-written render-target/texture aliases retain the existing selection
rules, independently of exact CPU descriptor identity: their current contents
exist on the GPU and must not be replaced by an upload of old guest bytes.

Released allocations have no guest identity. Their pool key is the exact Vulkan
format, physical three-dimensional extent, mip count, array layers, usage mask,
image type and creation flags (including cube compatibility). Samples, tiling
and sharing mode are fixed at one sample, optimal tiling and exclusive sharing.
Image views are destroyed after retirement and recreated for the new owner;
guest hashes, upload gates and GPU-written state are never stored in the pool.
Every acquired allocation starts with `UNDEFINED` tracked layout, discarding its
previous contents rather than treating them as valid data.

New guest textures/attachments keep their initial dirty upload path. Scan images
receive a resample before presentation; feedback snapshots copy every mip,
layer and aspect before sampling, even on a retained-image hit. Private AO images
are populated by the existing replay before use. Rescaled targets receive their
preserving resample. Reuse adds no path that samples an allocation's old contents
as valid guest data; unwritten regions retain the same undefined Vulkan semantics
as a freshly created image. Existing live GPU-written surfaces keep their contents
and normal barriers, rather than being discarded each frame.

## Retirement and bounds

Destroying a Surface transfers its handles and views to the active submission's
garbage list. Only `cleanup_submission`, after that submission's fence completes,
admits an allocation to the pool. That fence also covers earlier submissions on
the single graphics queue. CPU release alone never makes an image reusable.
Objects outside this surface allocator are destroyed normally.

The pool retains at most four images per allocation key, 32 images overall and
64 MiB. Oldest released entries are evicted to meet these limits; larger images
bypass the pool. Thus a stable working set within the retention limits warms
without further VkImage or VkDeviceMemory allocations; exceeding the budgets can
still cause churn. Shutdown drains submissions and frees the pool. The pool is
enabled by default only on Switch; desktop testing uses
`WWHD_VK_SURFACE_IMAGE_POOL=1`. Explicit `0` disables either reuse option.

## Verification

`WWHD_VK_STATS=1` adds `[vulkan surface images]` every 120 frames: actual image
creations/destructions and pool hits per frame, current pool count/bytes and guest
surface count. Logical release into a garbage list is not counted as destruction.
After warmup, look for zero creates/destroys in stable Outset Island gameplay.

The device smoke test (`--renderer-smoke`) with both reuse options set to `1`
checks pre-fence exclusion, reuse of both handles after retirement, fresh guest
upload pixels, live-image isolation, and allocation-free alternating format,
extent, mip, layer and dimensionality classes. It also exercises the production
feedback path with alternating shapes in both shader stages, verifies fresh
pixels after every source mutation, and checks zero warm creations/destructions.
Run with `WWHD_VK_ASYNC=1` to exercise Switch submission retirement on desktop,
and `WWHD_VK_VALIDATION=1` when Khronos validation layers are installed.

On Switch compare Outset Island render-thread CPU and the new counters after
warmup. Check AO, shadow/depth arrays, GPU-written texture aliases, TV/GamePad
scan copies, scene transitions, resolution/aspect changes and save-state reload.
Compare visuals with both options explicitly set to `0`. Hardware measurement
is still required; a desktop smoke test does not establish Switch performance.
