# Nintendo Switch port

A native Switch homebrew build of the recompilation: the game's PowerPC code is recompiled to
AArch64, the Wii U libraries it uses are reimplemented, and graphics run on Vulkan through Mesa's
NVK driver for Horizon. There is no emulator. You build it yourself from your own copy of the game;
the `.nro` contains code generated from that copy and must not be shared.

## Status

- **Performance**: 30 fps in handheld mode at the CPU/GPU/memory clocks 1785/460/1996 MHz (see
  Clocks). On the heaviest view tested (the hill above Outset's village: ~6,000 draws a frame) it
  holds 30 fps with the occasional single late frame. Stock handheld clocks (1020/384-460) are
  not enough for 30 fps in gameplay.
- **Tested**: the European release (v0), Outset Island and the sea around it, the first hours of the
  game. The USA release uses the same code with the full hook set but has not been tested on the
  Switch yet.
- **Known hitches**: the first time a new area's shaders appear they are translated and compiled.
  This happens in the background: objects using them appear a few frames late instead of the game
  stopping (`WWHD_VK_ASYNC_SHADERS=0` in `env.txt` compiles them in place instead). They are cached
  on the SD card and prepared at boot from then on. Loading new areas streams textures.
- **Not available on the Switch**: the GamePad screen (the Switch controller acts as a Wii U Pro
  Controller, so maps and menus show on the TV picture), keyboard/mouse features, the 60 fps modes.
  With the European executable the hook-based features (mods, aspect-ratio hooks) are off.

## Requirements

- A Switch running Atmosphère, with hbmenu started in **title takeover** mode (hold R while starting
  a game). Applet mode (the album) does not give homebrew enough memory: the game needs ~1.5 GB.
- Your own Wind Waker HD dump (USA or Europe), decrypted: `code/`, `content/`, `meta/`, or a Cemu
  `.wua` archive of it.
- To build: Docker (Linux, macOS or Windows with Git Bash) and several GB of disk space; the
  recompiler and the toolchain run inside the container.
- The NVK Vulkan driver object, `libnvk_local.o` (see below).

## Building

### On a PC, without Docker (the builder)

With the Switch SDK of a release (`sdk-switch/`, it contains no game code) and Python 3.8+ (Windows,
macOS or Linux):

```bash
python3 tools/switch/builder.py --game /path/to/your/dump --out wwhd.nro
```

- `--game`: the extracted game (the folder with `code/`, `content/`, `meta/`) or its `code/cking.rpx`.
  The region (USA or Europe) comes from `meta/meta.xml` (`--region us|eu` otherwise).
- It translates the game code to C (about 3 minutes), compiles it for the Switch with clang from
  zig 0.16.0 (downloaded once into the work folder, checksum verified; about 1-2 minutes) and links it
  with the SDK's prebuilt runtime using zig's lld. No devkitPro, no Docker. A Windows PC took 4 minutes
  in all.
- The game code compiled with clang runs as fast as the devkitA64 GCC build (game thread 27.3 ms vs
  27.9 ms on the hill replay).
- The SDK: `tools/switch/build.sh --nvk DIR --sdk` (Docker, devkitA64) builds it into `sdk-switch/`: the
  runtime prelinked with libnx and the C/C++ libraries into one object (`tools/switch/sdk_prelink.py`),
  libnx's linker script adjusted for lld, and the compile and link flags (`manifest.json`). The NRO
  packing is `tools/switch/nro.py` (byte-identical to devkitPro's elf2nro and nacptool).

### With Docker (developers)

```bash
tools/switch/build.sh --rpx /path/to/game/code/cking.rpx --region us --nvk /path/to/nvk
```

- `--rpx`: the game executable from your dump. It is recompiled to C (a few minutes) into
  `build/gen-<region>`; nothing generated from the game is ever committed.
- `--region eu` for the European executable: it is recompiled without the USA-address hooks.
- `--nvk`: a directory holding `libnvk_local.o`.
- The first build compiles ~170 MB of generated C at -O2 (tens of minutes; ~1 GB of RAM per job,
  `--jobs N` limits that). Later builds are incremental.
- Output: `build/switch-<region>/wwhd.nro`.

### The NVK driver object

`libnvk_local.o` is Mesa's NVK Vulkan driver (MIT license) built for Horizon by
[NaGaa95/mesa-switch](https://github.com/NaGaa95/mesa-switch) (tested: commit `7bb7b95`), merged
into one relocatable object whose Vulkan entry points are local (the runtime calls them directly; there is
no Vulkan loader on the Switch). It is built with that repository's Docker build
(`Docker.switch-nvk`, Rust target `aarch64-unknown-linux-gnu` for NAK/NIL) and localized with the
`localize_nvk.sh` script of [NaGaa95/Cemu-nx](https://github.com/NaGaa95/Cemu-nx)
(`dist/switch/`). A prebuilt `libnvk_local.o` attached to a release of this fork is built that way.

## Installing

Copy to the SD card:

| What | Where |
|---|---|
| `wwhd.nro` | `sdmc:/switch/wwhd/wwhd.nro` |
| the game | a Cemu `.wua` of it in `sdmc:/switch/wwhd/` or `sdmc:/switch/Cemu/games/` (its name must contain "Wind Waker"), or the extracted folders as `sdmc:/switch/wwhd/game/{code,content,meta}` |
| saves (optional) | `sdmc:/switch/wwhd/save/` (a Cemu `mlc01/usr/save/.../user` folder works) |

`env.txt` in `sdmc:/switch/wwhd/` can set options, one `KEY=VALUE` per line, for example
`WWHD_GAME=sdmc:/games/wwhd` (game location). The log is written to `sdmc:/switch/wwhd/log.txt`.

Start hbmenu in title takeover mode (hold R while starting any game) and pick "Wind Waker HD". The
first start translates the game's shaders as they appear; later starts prepare them at boot
(about a second).

## Clocks

The port needs the console's handheld clock limits raised for this title: 1785 MHz CPU and
1996 MHz memory; the GPU's 460 MHz is enough (921 measured no faster). With the official 1600 MHz
memory limit the heaviest views lose a frame now and then. Set them with a clock-profile
sysmodule (sys-clk, horizon-oc) for the title hbmenu takes over: the homebrew runs as that title.
The game never changes clocks itself.

## Controls

The Switch controller is a Wii U Pro Controller, buttons by position (A/B/X/Y, L/R, ZL/ZR, +/-,
sticks and stick clicks). `WWHD_PRO_CONTROLLER=0` in `env.txt` makes it act as the GamePad instead
(the GamePad's screen is not shown, so some menus are then unreachable). HD rumble is supported.

**L3 + R3** (both stick clicks together) shows or hides a performance counter in the bottom-left
corner: fps, frame time (average and worst) with a graph, and the CPU time per frame of the game and
render threads. The choice is remembered; `WWHD_PERF_OVERLAY=1` in `env.txt` turns it on at start.

## How it runs (overview)

- **Threads and cores** (the process gets CPU cores 0-2): the game's main thread on core 1; the
  game's other guest cores, the GX2 front end (display-list flattening, register shadowing and
  change detection) and the Vulkan command-recording thread on core 0; the render thread (draw
  preparation) on core 2.
- **Frame pacing**: the game waits for the frame before last (GX2DrawDone with one frame of lag),
  so it computes frame N+1 while the render thread prepares frame N; a flip that the render thread
  finishes a few milliseconds late costs those milliseconds, not a whole vsync. Likewise a frame the
  game itself finishes up to 4 ms after its vsync delays the emulated vsync clock instead of waiting
  for the next one (`WWHD_VSYNC_GRACE_MS`, 0 turns it off): heavy scenes stay near 30 fps instead
  of dropping to 20-25.
- **GPU**: the game draws its geometry twice into one depth buffer (a depth and normals pre-pass for
  the ambient occlusion, then the color pass, depth writes still on). Alpha-tested color draws (grass,
  leaves) whose mesh the pre-pass drew skip their redundant depth write, so the GPU tests depth before
  running their pixel shader instead of after: walking through tall grass went from 19 to 30 fps
  (`WWHD_VK_PREPASS_TWINS=0` turns it off).
- **Memory**: guest memory is mapped so the GPU reads vertex and index data in place.
- **Caches** (`sdmc:/switch/wwhd/config/shadercache/`): translated SPIR-V with the inputs to
  rebuild each shader variant (`vulkan-shaders/spirv.bin` plus an append-only journal), the
  pipelines seen (`pipelines.bin`) and NVK's pipeline cache. Boot prepares all of them. Variants
  whose translations are identical share one shader and its pipelines (tens of thousands of
  register combinations come down to about a thousand distinct shaders) and are stored as aliases.

Design notes and measurements: [runtime-switch-hotpaths.md](runtime-switch-hotpaths.md),
[vulkan-render-prepare.md](vulkan-render-prepare.md), [vulkan-record-thread.md](vulkan-record-thread.md),
[vulkan-shader-warmup.md](vulkan-shader-warmup.md), [recomp-a57.md](recomp-a57.md).

## Diagnostics (`env.txt`)

| Option | Effect |
|---|---|
| `WWHD_FPS_EVERY=60` | log fps, per-thread CPU time and core idle every 60 frames |
| `WWHD_SLOW_FRAMES=1` | one line per frame over 40 ms with what it spent its time on |
| `WWHD_FRAME_TRACE=slow` | a timeline of the game/render thread waits of each slow frame |
| `WWHD_PROFILE=1` | sampling profiler of all threads into `profile.log` (or a list of thread names) |
| `WWHD_VK_STATS=1` | Vulkan draw-preparation statistics |
| `WWHD_VK_PHASES=1` | CPU time of the render thread's draw phases every 120 frames |
| `WWHD_VK_ASYNC_SHADERS=0` | compile new shaders on the render thread (a long frame) instead of in the background |
| `WWHD_VSYNC_GRACE_MS=0` | no late-swap grace (hardware vsync timing) |
| `WWHD_VK_PREPASS_TWINS=0` | color draws keep their depth writes (see GPU above) |
| `WWHD_VK_GPU_PASS_TIMESTAMPS=1` | GPU time of the costliest render passes every 120 frames |
| `WWHD_VK_DRAW_TIMING=<frame>` | GPU time of every draw of that frame: costliest draws and shaders in the log, all draws in `drawlist.txt` |
| `WWHD_VK_SUBMIT_LOG=<n>` | CPU and GPU start/end times of n submissions (from frame `WWHD_VK_SUBMIT_LOG_FROM`) |

GPU timestamps on the Switch run about 1.63 times slower than the driver reports: multiply the
GPU figures of these logs by 1.63 for real time.
