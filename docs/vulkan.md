# Vulkan renderer

The Vulkan renderer (by OpenAI Codex, originally the `vulcan` branch) translates GX2 to Vulkan; on a
Mac it runs on MoltenVK. The shader translator and texture addressing reuse the vendored Cemu
components under MPL-2.0; Cemu's emulator runtime is not needed.

## One app, two renderers (macOS)

The default macOS build (`WWHD_RENDERER=BOTH`) contains the Metal renderer and the Vulkan renderer.
`runtime/src/gfx/renderer.h` is the interface: each renderer fills a table of entry points
(`gfx/metal_backend.mm` for Metal, namespace `gfx`; `gfx/vulkan/backend_table.cpp` for Vulkan,
namespace `gfxvk`), and the GX2 layer, save states, menus and input call `render::*`, which
forward to the renderer chosen once at start-up:

1. `--renderer=metal|vulkan` (or `--renderer metal|vulkan`) on the command line,
2. `WWHD_RENDERER_RUNTIME=metal|vulkan` (tests),
3. the saved choice from **Graphics > Renderer** (`renderer` in
   `~/Library/Application Support/wwhd/display.plist`), which takes effect at the next start; the
   menu offers "Restart Now" (the game relaunches itself with the same arguments),
4. Metal.

If Vulkan cannot start, the game falls back to Metal and shows why (log, a sheet on the TV window,
the window title says "Metal (Vulkan unavailable)"): no Vulkan loader or glslang installed (both are
weak imports, so the app still starts without them), no driver (MoltenVK), no device with dynamic
rendering. `WWHD_VK_FORCE_INIT_FAIL=1` forces this path for testing.

Both renderers use the same AppKit host (`gfx/display.mm`, `menu.mm`, `input.mm`,
`controls_ui.mm`, `mods/mouse.mm`): TV and GamePad windows, full screen, GamePad screen modes
(separate window, picture-in-picture, automatic), scaling filters, aspect ratio, the Controls window,
the Gameplay mods including the mouse camera, save states, 60 fps / true 60. Vulkan presents into
the windows' views through a `CAMetalLayer` and `VK_EXT_metal_surface`; `display_plan()` gives it the
same layout as the Metal composition (picture rectangle, GamePad overlay with frame and opacity,
filter), and `gfx/vulkan/present.cpp` draws it. The TV window title starts with the renderer in use.

| Feature (macOS app) | Metal | Vulkan |
| --- | --- | --- |
| TV / GamePad windows, full screen, display modes, PiP, automatic PiP | yes | yes |
| Scaling smooth / sharp / integer, FXAA | yes | yes |
| Internal resolution, AO modes, full-size AO depth, 16x AF | yes | yes |
| Aspect ratio (16:10, 21:9, 32:9, match window) | yes | yes |
| 60 fps interpolation, true 60 | yes | yes |
| Save states (menu, Shift+F1..F5 / F1..F5) | yes | yes (a state loads with either renderer) |
| Controls window, keyboard, game controllers, mouse camera, mods | yes | yes |
| Climb mod stamina wheel | yes | yes (ported shader; not yet seen in a test run) |
| Frame dumps `WWHD_DUMP_FRAMES`, `WWHD_DUMP_PRESENT` | yes | yes |
| Capture frame (P) | pictures + draw log | pictures only (no draw log) |
| Shader head start (`--warm-shaders`) | yes | no; previously seen variants warm automatically at boot |

Other builds: `-DWWHD_RENDERER=METAL` (Metal only, no Vulkan dependencies) and
`-DWWHD_RENDERER=VULKAN` (Vulkan only with the portable SDL3 host: SDL windows, input and audio;
the default on Windows/Linux). Test runs: `WWHD_HIDDEN_WINDOWS=1` keeps the windows off screen
(frame and present dumps still work), `WWHD_EXIT_AT_FRAME=n` quits in order at frame n,
`WWHD_TEST_RENDERER_SWITCH=frame:metal|vulkan` does what "Restart Now" does, `WWHD_LOG_TITLE=1`
logs the window title.

## Build

Use Clang/clang++ with CMake, Vulkan 1.3 headers and loader, glslang and zlib (and SDL3 and, on
Windows/Linux, LZ4 for the SDL host). A GPU/driver with dynamic rendering is required: Vulkan 1.3,
or Vulkan 1.1 / 1.2 with `VK_KHR_dynamic_rendering`. Dependencies must match the target architecture.

The executable imports no Vulkan functions (`VK_NO_PROTOTYPES`): `gfx/vulkan/loader.h` loads them
at run time through the loader's `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` (SDL host:
`SDL_Vulkan_LoadLibrary`; macOS app: the weakly linked Homebrew loader). On Windows and Linux the
Vulkan loader is not linked at all, so a missing loader or an older one (Vulkan 1.2 drivers on
Windows lack `vkCmdBeginRendering`) no longer stops the program before it starts. Without a usable
GPU, the SDL host shows a message box naming the GPU, its Vulkan and driver versions and what is
missing, and exits.

On macOS:

```sh
brew install cmake vulkan-headers vulkan-loader molten-vk glslang   # sdl3 too for WWHD_RENDERER=VULKAN
cmake -S . -B build/cmake -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++   # BOTH
cmake --build build/cmake --parallel
# Vulkan only, SDL3 host:
cmake -S . -B build/vulkan -DWWHD_RENDERER=VULKAN -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
```

Generate `build/gen` from your own game as described in the main README first.
Set `GEN_DIR` to use another generated-code directory. For Windows, use a Clang
CMake toolchain with the required dependencies; generated guest code requires
Clang's `musttail` support. Cross-compiling runtime files alone does not establish
that the executable links or runs on Windows/Linux.

## Check the renderer

```sh
./build/vulkan/wwhd --renderer-smoke
```

This test does not read game assets. It checks GPU texture uploads, mip/layer
readback, clears, blits, depth/stencil, a generated triangle and presentation.
It also queues ten asynchronous submissions, wraps the four-slot ring, and checks
immutable upload payloads and deferred-buffer retirement by GPU readback.
To enable Khronos validation, install the Vulkan validation layers and set
`WWHD_VK_VALIDATION=1`. On Homebrew, set `VK_LAYER_PATH` to
`/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d`.
Test aids for older drivers: `WWHD_VK_FORCE_API=1.2` (or `1.1`) makes the renderer treat every GPU as
if it reported that Vulkan version, so a Vulkan 1.3 GPU takes the `VK_KHR_dynamic_rendering` path;
`WWHD_VK_HIDE_EXTENSIONS=VK_KHR_dynamic_rendering` (comma-separated names) hides device extensions,
which together with `WWHD_VK_FORCE_API=1.2` shows the "missing features" error.
If the loader does not discover MoltenVK, set `VK_DRIVER_FILES` to its installed
`MoltenVK_icd.json` (Homebrew places it below `$(brew --prefix molten-vk)/etc/vulkan/icd.d`).

## Current scope

This backend is under development. It records real Vulkan draw calls and translates
Latte shaders to SPIR-V. Guest GX2Flush queues work using four fenced submission
slots; DrawDone, readbacks and final presentation still wait for completion. Each
slot retains its command/descriptor pools and upload arena until its fence completes,
so memory use is higher than the original single-slot implementation.
Geometry shaders, transform feedback, multisampled guest surfaces and some format
conversions are rejected explicitly. Vulkan now provides live Graphics controls, including a
macOS menu; other Metal-specific menus remain unported. SDL mouse capture and camera
input are supported. MoltenVK devices without sampler LOD bias use zero bias,
matching the existing Metal backend; native Vulkan devices retain the bias. Shader warmup/cache behavior differs from Metal.

Vulkan keeps a persistent device pipeline cache under the host configuration
directory's `shadercache` folder. `WWHD_VK_PIPELINE_CACHE=/path/cache.bin` selects
an isolated cache file; `WWHD_VK_PIPELINE_CACHE=0` disables persistence. Setting
`WWHD_SHADER_CACHE=0` also disables persistence unless an explicit Vulkan cache
path is supplied. Caches are matched to the device and pipeline-cache UUID;
incompatible, truncated, or checksum-invalid files fall back to an empty cache. New data is saved
atomically after 120 frames without new pipelines. First-time shader translation
and pipeline creation can still cause stalls.

Vulkan also persists compiled SPIR-V in the host configuration directory's
`shadercache/vulkan-shaders/spirv.bin`. `WWHD_VK_SHADER_CACHE=/directory` selects
an isolated directory; `WWHD_VK_SHADER_CACHE=0` disables disk storage. An explicit
directory overrides `WWHD_SHADER_CACHE=0`. Canonical GLSL and stage deduplication
remains active in memory when disk storage is disabled. Fresh decompiler metadata
is kept for every guest variant; identical final shader programs share compilation.
Previously recorded variants are translated before the game thread starts, with
their SPIR-V reused from disk. `WWHD_VK_SHADER_WARMUP=0` disables this boot replay
on both Switch and desktop. See [shader warm-up](vulkan-shader-warmup.md) for input
coverage, limits, migration and CPU regressions.

The versioned cache checks the compiler recipe, exact shader source, checksum,
record bounds and SPIR-V structure. Incompatible or corrupt files become misses.
After 120 quiet frames, the render thread copies a bounded immutable snapshot
and schedules one background save. The worker serializes, checksums, and atomically
replaces the file; it does not access Vulkan objects or the live shader cache.
Completion is polled on the render thread. Shaders discovered during a save remain
dirty for a later snapshot, and failed writes remain eligible for retry. Orderly
window close joins any worker and saves the latest records before exiting. Forced
termination can leave the most recent additions unsaved. Files and live canonical
entries (including input recipes) are bounded to 128 MiB. New programs compile
once when encountered; replay covers previously visited variants within its boot
budgets. Unvisited scenes and device pipelines are still prepared on demand.

An isolated Outset startup through frame480 originally compiled 2,135 variants
in 2.44 seconds. Deduplication compiled 412 distinct programs in 0.59 seconds;
closing at frame500 saved 415 programs in a 9.8 MB cache. A fresh-process reload
hit every variant with zero SPIR-V compilation and about 17.5 ms cache-load time.
This reduces compilation hitches; sustained frame rate has a separate CPU/GPU
budget.

Set `WWHD_VK_STATS=1` to report frame rate, frame-interval percentiles, upload
traffic, shader compilation, and GPU/presentation waits every 120 frames.
The SDL window title displays live presented FPS and the selected 60 FPS mode.
Stats also report descriptor allocation/reuse, SPIR-V compilation and disk hits,
decompiler time, and Vulkan pipeline-creation time. Cache snapshot-copy time is
reported separately from background serialization and disk-write time.
On POSIX hosts, stats also report render-thread CPU milliseconds per frame using
`CLOCK_THREAD_CPUTIME_ID`, sampled once per reporting window. This excludes blocked
waits and OS descheduling, and excludes CPU work on other threads. Discard startup
reports when comparing warmed scenes. Uniform preparation, reuse hits/bytes,
descriptor bind opportunities, actual bind calls, and skipped sets are reported
alongside upload traffic; snapshot counts describe requests, including reuse hits.
Benchmark with validation disabled; the validation layer adds substantial CPU
overhead when gameplay submits thousands of draws each frame.

On macOS, direct Metal resource bindings are the default MoltenVK path. Set
`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1` to restore argument buffers if needed
for larger resource limits or another device. A same-scene M3 Max comparison
measured about 30 FPS with argument buffers and 30–40 FPS with direct bindings;
these are historical measurements from before the later CPU fixes. The setting is documented in
[MoltenVK configuration parameters](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Configuration_Parameters.md).

## Opt-in CPU experiments

The following paths are off by default on desktop. Each requires its environment value
to be exactly `1`; other values leave that path disabled. Switch enables the surface
image pool and feedback image retention by default (explicit `0` disables either).
They can be combined,
but timing comparisons should use the same binary, guarded saved scene, graphics
settings, warmed caches, and a quiet host, with validation disabled.

For timing with detailed per-draw instrumentation disabled, unset
`WWHD_VK_STATS` and set `WWHD_VK_CPU_ONLY_STATS=1`. This reports render-thread
CPU time, FPS, frame pacing, and cumulative shader/pipeline cache counters every
120 frames. It leaves comparison clocks, detailed preparation counters, and
wait timers disabled. CPU time excludes sleeping and GPU waits.

| Environment option | Scope and correctness constraints |
| --- | --- |
| `WWHD_VK_REUSE_UNIFORM_SNAPSHOTS` | Reuses immutable uniform slices after full fresh byte comparison within the same device/submission generation. Support uniforms are packed and texture/AO patches applied before lookup; allocation alignment and zero padding remain unchanged. |
| `WWHD_VK_REUSE_FEEDBACK_IMAGES` | Retains up to four compatible feedback-copy shapes per shader-stage/texture-unit slot, bounded to 128 MiB of retained image allocations. Every attachment alias still receives a fresh copy, with barriers preserving prior reads before overwrite. Overflow uses fence-retired temporaries. Default on Switch. |
| `WWHD_VK_SURFACE_IMAGE_POOL` | Pools released image/memory pairs only after fence retirement, keyed by exact Vulkan format, extent, mips, layers, usage, image type and creation flags. Bounded to four images per key, 32 images total and 64 MiB, with oldest releases evicted first. Default on Switch. See [surface reuse](vulkan-surface-reuse.md). |
| `WWHD_VK_SKIP_REDUNDANT_BINDS` | Omits only exact consecutive descriptor bindings, including layout, sets, dynamic offsets, command buffer, and submission generation. It can bind just the changed stage; resource preparation still runs. Pass changes reset the tracked state. |
| `WWHD_VK_DESCRIPTOR_RANKS` | Precomputes descriptor binding ranks and compacts active writes/offsets in binding order. Invalid or duplicate metadata falls back to the original sorting path. |
| `WWHD_VK_PIPELINE_LOOKASIDE` | Adds eight exact pipeline-key entries ahead of the existing map lookup. Full key comparison, device checks, and reset handling remain in place. |
| `WWHD_VK_SHADER_ADDRESS_MEMO` | Texture address words 2/3 no longer invalidate the last Vulkan shader lookup. Other register classification is unchanged; textures and attachment aliases are resolved afresh, and context/save-state loads invalidate shader memoization. |
| `WWHD_VK_FETCH_MEMO` | Reuses the last fetch-program lookup within one frame after fresh header/range validation, preserving the existing once-per-frame program-byte hashing contract and save-state reset. |
| `WWHD_VK_SPECIALIZE_INDICES` | Selects a typed endian/restart reader once per draw and directly fills expanded primitive indices. Native index eligibility and the final immutable index extent scan remain unchanged; wrapped big-endian guest ranges use the original reader. |
| `WWHD_VK_SHADER_STATE_MEMO` | Retains four exact gathered-state hash entries per shader stage. Every lookup freshly gathers all existing masked words and compares every active byte, count, and program hash seed. Misses use the identical hash mixer; save-state and sentinel calls clear the memo. |
| `WWHD_VK_SKIP_VERTEX_BINDS` | Omits only identical host vertex buffer/offset bindings after fresh snapshot preparation. Sixteen binding slots are guarded by command buffer, submission generation, and pass resets. Index bindings remain fresh. Combine with exact vertex snapshot reuse to make unchanged slice identities available. |
| `WWHD_VK_SAMPLER_MEMO` | Reuses immutable sampler handles after exact device, fresh sampler-word, compare/integer, and effective anisotropy matching. Texture preparation still runs before lookup. |
| `WWHD_VK_SPARSE_HASH_MEMO` | Only used where page write tracking is unavailable (texture changes are detected by `runtime/src/write_watch.h`; the sparse check is its fallback). Compares all freshly read, ordered sparse texture samples before reusing their hash. Full texture checks, invalidation, and uploads remain unchanged. Retains 64 entries by default; `WWHD_VK_SPARSE_HASH_ENTRIES=256` selects a bounded 16 MiB sample store. Oversized sample sets stream through the original mixer. |
| `WWHD_VK_SHADER_KEY_DIRTY` | Avoids shader-generation bumps only for explicitly classified register bits absent from the current Vulkan shader key. Program headers, fetch strides, unknown registers, and context/save-state invalidation remain conservative. |
| `WWHD_VK_VERTEX_HISTORY_REUSE` | Adds a second distinct vertex snapshot key per binding, requiring `WWHD_VK_REUSE_VERTEX_SNAPSHOTS=1`. Every candidate receives a fresh full payload comparison within the same device/submission generation; changed bytes allocate a fresh slice. |

The four additional paths together with the previous ten and vertex snapshot
reuse passed a 2,400-frame Khronos game run with a guarded wide-view save-state
restore, resolution transitions, FXAA, anisotropy, and high-resolution AO.
The run exited normally without validation errors, and its actual presentation
capture was inspected. In this scene the 256-entry sparse memo matched all
roughly 170 checks per frame after warming; secondary vertex reuse avoided about
2.6 MiB of additional copies per frame, and the register classifier avoided about
1,460 generation bumps per frame. These are work counts; performance and default
selection still require controlled comparisons with validation disabled.

The first six paths together passed a 2,400-frame full-game Khronos validation
run, including a guarded save-state load and resolution/FXAA/anisotropy transitions,
with normal exit and no validation errors. The boot-time unused-fragment-output
warning also occurs with these paths disabled. All seven paths also passed the
native build, four CTest checks, actual-device Khronos smoke checks, and a separate
1,800-frame game run with a guarded save-state restore and inspected capture.
The measured scene shows about 4,244 fetch-memo hits and 150 pipeline-lookaside hits
per frame; roughly 4,935 descriptor sets and 0.50 MiB of uniform uploads are avoided
per frame. These are work counts, not timing claims. Preliminary CPU comparisons
were taken under substantial external CPU load and do not establish a final speedup;
all seven paths remain opt-in pending controlled measurements.

The index specialization passed 3,594,240 UBSan differential vector/extent checks,
265 additional edge checks, an independent source review, and the native build.
Private conversion microbenchmarks support the reduced decoding/expansion cost;
they do not establish game FPS gains. Both newer paths together with the previous
seven and exact vertex snapshot reuse passed a 1,800-frame Khronos game run with
a guarded save-state restore, normal exit, no validation errors, and an inspected
actual swapchain capture. The state memo avoids hashing on roughly 70% of its
2,150 lookups per frame in that scene. Controlled timing remains required before
default selection.

All ten paths plus exact vertex snapshot reuse also passed a 2,400-frame Khronos
run with guarded save-state restore, 1→1.5→2→3→1 resolution transitions, FXAA,
anisotropy, and inspected actual swapchain capture. Vertex bind elision skips
roughly 4,124 calls per frame in the saved scene while retaining fresh byte checks.
State memo differential QA passed 100,000 cases with UBSan. ASan could not run:
a process sample showed a runtime initializer deadlock before `main`.

## Live graphics controls

In the macOS app the Graphics and Display menus and their keys work as with Metal. The SDL game window
also accepts the following plain-key shortcuts; repeated keydown events are ignored.
The shortcuts are scoped to the game window, respect host-input disabling, and do
not toggle settings while entering text. Menu checkmarks reflect pending settings;
resolution changes latch at the next render-frame boundary. Unsupported effect
controls remain disabled until the renderer reports support.

| Control | Behavior |
| --- | --- |
| `R` | Cycle internal resolution through 1×, 1.5×, 2×, and 3×: 1280×720, 1920×1080, 2560×1440, and 3840×2160. The menu also selects each preset directly. |
| `O` | Cycle AO mode: 0 original Wii U behavior, 1 centre-depth filtering fix, 2 centre fix plus corrected noise tiling. |
| `M` | Toggle full-size AO depth. A private 1.5× replay of the game's depth-downsample pass supplies the AO pass without changing guest surface metadata. |
| `N` | Toggle anisotropic filtering up to 16×, bounded by device support, for eligible mipmapped asset textures. |
| `8` | Toggle FXAA in final screen presentation. This is post-process antialiasing; it does not enable multisampled guest surfaces. |
| `6` | Toggle 60 FPS interpolation mode. |
| `7` | Toggle true 60 FPS mode. Selecting either mode does not guarantee its target frame rate. |
| Graphics menu: Scaling | Select smooth, sharp, or integer scaling for presentation. |

Startup overrides match Metal: `WWHD_RES_SCALE` selects the initial internal scale
(1–4); `WWHD_AO_MODE` selects AO mode 0–2. If `WWHD_AO_MODE` is absent, presence of
`WWHD_NO_AO_QUIRK` selects mode 0, otherwise mode 2 is the default. The explicit AO
mode takes precedence over the legacy flag, whose value is ignored.
`WWHD_AO_HIRES` defaults to on; `0` disables it. `WWHD_ANISO` defaults to off and a
nonzero value enables it. `WWHD_FXAA` defaults to off and its presence enables it,
including a value of `0`, matching Metal's existing behavior.
`WWHD_SCALE_FILTER=smooth|sharp|integer` selects the initial presentation filter;
smooth is the default. Live settings currently apply to this process.

A full-game Khronos validation run passed internal-scale transitions
1× → 1.5× → 2× → 3× → 1× with FXAA, anisotropy, AO mode 2, and full-size AO depth
enabled. Four CTest host regressions and the GPU smoke test passed. The native
macOS Graphics menu was verified through its accessibility item list. Captures of
actual swap output with FXAA on and off differed at 31,446 pixels, verifying that
the toggle reaches presentation. These checks establish the tested controls and
rendering paths, not higher-resolution frame-rate acceptance. Higher-resolution
performance remains unverified; contemporaneous 1× and 2× runs both measured
roughly 30 FPS while other CPU workloads were active.

## Save-state shortcuts

In the macOS app, the Save States menu and keys work as with Metal. In the SDL game window, `F1` through `F5` load slots 1 through 5;
`Shift+F1` through `Shift+F5` save those slots. Repeated keydown events are ignored,
and these keys do not reach the game's button mapping. Host-input-disabled scripted
runs do not accept the shortcuts. State files remain in the configured state directory.

A load must pass the existing allocation, thread, and guest-stack guards. A state
that loads in the same process may fail during a new process's startup; matching
printed LR/SP values do not establish that deeper guest stack frames match. Loading
later can succeed if the worker threads reach compatible waits. Failed loads do not
bypass these checks or establish a valid benchmark starting point.

## Draw batching (all platforms)

Every platform defaults to submitting after 2,048 guest draws, with at most three
mid-frame submissions (measured on macOS and Windows; Linux uses the same defaults). Each submission retains its upload slices, descriptor
pool and deferred resources until its fence completes. The next draw reopens
attachments with `LOAD`, preserving draw order and contents. This uses the
existing asynchronous submission path.

Set `WWHD_VK_DRAW_BATCH=0` to disable mid-frame batching, or set an explicit
positive draw count to tune it. Empty or invalid values disable batching.
`WWHD_VK_DRAW_BATCH_CAP=1|2|3` sets the maximum mid-frame submissions (default three).
See `docs/performance.md` for the local Windows comparison.

## macOS defaults and tuning

Mac/MoltenVK gameplay and correctness tests are the current priority. The macOS
Vulkan defaults now select command-buffer prefill mode 3, asynchronous batches of
2,048 draws with a cap of three extra submissions per frame, and precise vsync
sleeping. Explicit environment overrides take precedence. Windows/Linux defaults
remain unchanged and further device testing there is deferred. Vertex snapshot
reuse remains an opt-in experiment on every platform.

| Option | Behavior and default |
| --- | --- |
| `WWHD_VSYNC_PRECISE` | macOS defaults to enabled: sleep to near the vsync deadline, then spin for the final portion (up to 2 ms). Set `0` to disable. Other platforms keep the previous off default. Precise sleeping increases CPU use. |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | macOS defaults to `3`; an explicit MoltenVK override, including `0`, is respected. Normal macOS render batches use an autorelease pool. Other platforms receive no application override. |
| `WWHD_VK_DRAW_BATCH` | Defaults to `2048` when unset, on every platform. Set `0` to disable. Positive decimal values up to 1,048,576 select the batch size; malformed or out-of-range values disable batching. |
| `WWHD_VK_DRAW_BATCH_CAP` | Accepts `1`, `2`, or `3`. When unset, defaults to `3`. An explicitly invalid value falls back to `2`. Has no effect with batching disabled. |
| `WWHD_VK_REUSE_VERTEX_SNAPSHOTS=1` | Reuse a bounded vertex snapshot only after exact guest-byte comparison within the same fenced submission generation. Off by default; comparison overhead can outweigh saved copies. |

Draw batching keeps the four-slot fence retirement contract. More submissions can
reduce the final GPU tail, but add attachment LOAD/STORE boundaries and descriptor
cache resets, and can wait when the ring wraps. DrawDone, capture/readback, and final
presentation still drain work. Use matching camera views and warm caches for
comparisons, and keep validation runs separate from timing runs.

On Apple M3 Max, warm static-view, camera-sweep, and character movement/swimming
benchmarks achieved 99.78%, 100%, and 99.45% coverage of nominal 59.94 Hz display
ticks. These percentages measure flip intervals divided by elapsed vsync ticks,
not the percentage of frames meeting the ready-time deadline. The static run
covered 1,799 flip intervals over 1,803 ticks; a capture inside its measurement
window can contribute to a long interval. The character run covered 1,799 intervals
over 1,809 ticks, with eight late-ready frames among 1,800 samples.

The camera sweep covered all 1,799 ticks with no late-ready frames among 1,800
samples over 30 seconds. Its 99th-percentile ready time was 13.558 ms and maximum
16.622 ms against the 16.683 ms deadline. It reached far views of roughly 6,500
draws per frame; this is not an exact reproduction of the earlier user view of
roughly 6,600 draws. Character checks are complete.

A separate default-only warm heavy-view run in true 60 FPS mode averaged roughly
6,230 draws per frame. Over 30 seconds it covered 1,799 flip intervals across
1,800 nominal display ticks (99.94% coverage, one missed tick). Its 99th-percentile
ready time was 15.488 ms and maximum 18.894 ms; every warmed 120-frame reporting
window measured approximately 59.4–60 FPS. This verifies the selected macOS defaults
for that tested scene, while retaining the occasional missed deadline.

These results apply to the benchmarked scenes and configuration, not every area
or device. First-use shader/pipeline warmup can still hitch. Selected 60 FPS mode
and the live FPS title alone do not establish performance acceptance.

Windows/Linux save states use a separate LZ4 format version; they do not interchange
with Apple's existing compressed save states. Windows uses conservative safe
parking at host wait points; arbitrary guest-entry save parking is not enabled.

## Validation checkpoints

The selected macOS defaults passed a fresh-process Khronos gameplay validation run:
a guarded slot-5 state restore succeeded, the game reached the frame-1,800 checkpoint
and exited normally, and the log contained zero VUID reports, warnings, or errors.
A preceding cold-cache run reached frame 2,280 before its bounded duration timeout;
it was a partial run rather than the completed validation checkpoint. That cold
run compiled 518 canonical programs in 766 ms. Two background cache saves consumed
38.3 ms on the worker, with 2.2 ms of render-thread snapshot copying; the fresh
warm-cache process compiled zero programs. These measurements separate first-use
warmup cost from steady gameplay.

Earlier macOS/MoltenVK checkpoints passed four CTest host regressions and the GPU smoke
test with Khronos validation enabled, including upload-arena fence reuse and
interior mip invalidation, asynchronous slot wrap, and dynamic-UBO binding order
and reuse after pool reset. Actual Outset Island gameplay and scripted keyboard
movement render correctly on an Apple M3 Max through MoltenVK. Earlier warm gameplay builds
ran approximately 29.4–30.0 FPS, with median frame intervals near 33.4 ms and
95th percentiles around 36–37 ms; occasional longer frames remain. Enabling
true 60 FPS mode alone does not guarantee sustained 60 FPS. A bounded
20-frame trace measured a median 17.53 ms from vsync wake to the next swap,
missing the 16.68 ms deadline with one vsync wait per frame in that earlier build.
At an earlier optimization checkpoint, descriptor/state deduplication, asynchronous
flush slots, word-based shader and texture hashing, and direct Mac bindings produced
a 60-second true-60 dock run that
measured 39.13 FPS across warmed frames 720–1920 (31.13–55.77 FPS in
120-frame intervals). The comparable argument-buffer run was about 30 FPS.
The subsequent historical checkpoint with dynamic uniforms and the consecutive
binding fast path measured 47.27 FPS (34.75–59.44 FPS per 120-frame interval), with zero
SPIR-V compilation after cache reload. Across the longer warm run it averaged
46.69 FPS. These scene-specific results do not establish smooth or sustained
60 FPS. Vertex position
invariance prevents multipass depth artifacts on Link. All 195 captured vertex
shader variants passed SPIR-V compilation with invariant position outputs.
Earlier validation runs reported unused fragment outputs during depth-only passes;
the final gameplay validation run above reported no warnings.
Broader gameplay and devices remain to be validated. On Linux (Ubuntu 24.04, Clang 18) the
executable builds without warnings and links against placeholder guest code
(`tools/recomp/stubgen.py`), the unit tests pass and `--renderer-smoke` passes on Mesa lavapipe with
the Khronos validation layer reporting nothing (`.github/workflows/linux.yml`); gameplay on Linux
needs your own recompiled game and remains to be tested. On Windows (x86_64, Clang/MinGW: llvm-mingw
or MSYS2 CLANG64) the executable builds and links, the unit
tests pass, and `--renderer-smoke` passes under Wine with lavapipe (`.github/workflows/windows.yml`
builds and runs the tests natively); a run on Windows hardware with a GPU and gameplay remain to be
tested.

For an isolated capture, set `WWHD_CAPTURE_PATH` to the PNG output path and
`WWHD_CAPTURE` to the renderer frame number (default120). Keep these artifacts
private; they contain game imagery.

## Optional performance diagnostics

These switches require the exact value `1` and remain disabled by default.

- `WWHD_VK_GPU_TIMESTAMPS`: measures submission timestamp intervals using two queries per fenced submission slot. Results are collected only after the existing completion fence, without query waits. The interval sum is not GPU busy time or the frame critical path; unsupported or zero-only timestamp results cannot establish GPU cost.
- `WWHD_VK_UNCAPPED`: bypasses guest wall-clock flip eligibility for throughput diagnostics, while retaining FIFO GPU completion ordering. Frame-driven simulation accelerates; timebase/audio clocks remain real-time. This is not normal gameplay FPS.
- `WWHD_VK_READY_FLIP_WAIT`: experimental waiting for an already-eligible pending flip to complete instead of sleeping another full guest tick. It preserves the existing minimum swap interval and GPU completion guards. Literal wait-for-vblank behavior changes for an eligible pending flip; keep opt-in until matched gameplay benchmarks and correctness checks establish suitability.
- `WWHD_VK_READY_FLIP_PARK`: experimental eligible-flip completion wait after the save-state freeze gate and before guest-core reacquisition. The optional host callback runs while the thread is marked running and its guest core remains released; it keeps the existing flip eligibility and FIFO GPU guards. This avoids a second core release/acquire roundtrip. Default sleep behavior and Metal callers remain unchanged.
- `WWHD_VK_NARROW_BARRIERS`: experimental layout-specific source dependency scopes. Destination scopes, image ranges, render-pass breaks and same-layout write barriers remain unchanged. Shader-read classification assumes the current vertex/fragment consumers; general and unknown layouts retain conservative scopes.
- `WWHD_VK_FEEDBACK_STATS`: counts full feedback copies and identical source versions within one draw across both shader stages. It reports physical texels rather than estimated bandwidth and does not skip copies.

The saved uphill 3x scene passed 2400 frames with both experimental switches, guarded checkpoint restore and Vulkan synchronization validation on Apple M3 Max. This establishes a tested path, not a universal synchronization proof or FPS improvement. Duplicate feedback counters were zero for that scene, so duplicate-copy elimination was rejected there. Comparisons must use the same executable, accepted checkpoint, resolution and graphics settings; a rejected state load invalidates the scene benchmark.

The PARK experiment separately passed the native build, four CTest regressions, extracted-function UBSan ordering/guard tests, and 2400-frame synchronization validation with an accepted uphill checkpoint. Its gameplay performance still requires matched non-validation comparisons.

`WWHD_VK_GPU_PASS_TIMESTAMPS=1` adds bounded render-pass and feedback-copy scopes: at most 256 pairs per submission, recorded outside dynamic rendering and collected after the existing fence. Top-eight format/extent buckets report stage intervals, draw counts, unavailable/zero results and capacity drops. This instrumentation affects GPU scheduling: a matched uphill 3x calibration measured about 45 FPS with scopes versus 55 FPS without them. Use it to investigate work, and disable it for performance acceptance; interval sums are not exclusive GPU busy time.

`WWHD_VK_VERTEX_WINDOW_STATS=1` measures a conservative unused-prefix opportunity without changing uploads. Its separate index scan adds CPU overhead, so its FPS is not a gameplay benchmark. The accepted uphill checkpoint showed approximately 27.4 MiB of copied-prefix requests with 8.0 MiB never referenced by the draw.

The strict opt-in `WWHD_VK_VERTEX_COPY_WINDOW=1` instead fuses minimum and maximum index reduction, retains the original physical allocation and draw addressing, and copies/compares only the proven fetch window. Unsupported bounds/rates/layouts keep the original full-prefix path. A separate cache includes reservation, window offset/length, device and submission generation; fresh guest data is always compared. Device smoke tests passed full/window pixel equivalence with poisoned leading bytes, signed base vertex, restart, vertex-index checks, mutations and submission-ring retirement. Actual saved-scene validation and matched FPS acceptance are still required before adoption.

Small register updates (at most 16 words) use the fused Vulkan path by default. Set `WWHD_VK_FUSE_SMALL_REGS=0` to disable it. Matched scene tests suggest roughly 2% less renderer CPU time; FPS benefit remains modest and subject to run drift.
