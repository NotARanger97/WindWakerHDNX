# Switch runtime hot paths

These changes are unmeasured on hardware. No clock, service, CPU placement or
guest scheduling priority changes are included.

## Command emission

Switch uses a fixed 4 MiB word ring instead of the two growing command vectors.
The main thread is the usual producer; a short atomic producer gate preserves
FIFO order when other guest threads or host save-state/shutdown code emit.
Publication normally batches 1,024 words. Flush, draw-done, swap and fence
commands publish immediately. When published work runs out, the consumer tries
the gate and publishes a pending short batch before parking. It never waits
for the gate, because a producer holding it may be waiting for ring space.

Commands never straddle the ring end: native-endian NOP packets skip padding.
The consumer releases storage only after executing a contiguous span. Payloads
remain owned snapshots; display-list calls retain their existing guest-address
semantics. Fence issuance follows enqueue order under a separate mutex, so a
later sync cannot acknowledge an earlier, still-unqueued fence. Two-word IDs
avoid 32-bit rollover; execution still accepts old one-word fence packets.
Save-state drains publish and wait through the same queue before replacing
memory. Desktop keeps its vector queue, and inline execution remains available.

Register batches copy their prefix and values directly to the command stream,
removing the TLS vector, its allocation and the intermediate copy. Clear/copy
HLE calls use bounded stack arrays instead of allocating vectors. Fetch shader
start/size is one adjacent register packet. Uniform conversion uses a 4 KiB
stack buffer instead of TLS and initializes only the requested words.

## Audio and synchronization

AX frame runs roughly 333 times/second (96 samples at 32 kHz), with a pacing
adjustment of up to 5%. Its service CPU is guest core 0, mapped to CPU core 0 in
the default Switch configuration. Callbacks acquire the guest core; voice processing
is guarded by the existing AX mutex and service save-state gate.

Switch AArch64 uses NEON for envelope multiply, bus mixing and 32-to-48 kHz
upsampling. Gain ramps retain sequential scalar additions, and multiplication
and addition stay separate with the existing `-ffp-contract=off`. ADPCM decode
and the stateful filters remain scalar. Diagnostic per-sample SFX energy work
now runs only with `WWHD_AX_STATS`. Host audio push/pull copies at most two
contiguous spans, preserving the release/acquire ownership and flush behavior.

Message queues allocate their ring storage at init/load, then preserve normal
FIFO and high-priority front insertion without deque allocation on send/receive.
The existing spin data lock and sleeper handshake are unchanged. Save-state
message order and encoding are unchanged.

Thread/current-core, interrupt, mutex/event and receive HLE paths use the existing
`Cpu::thread` where available. Generic waits consult TLS only when parking.
Uncontended core acquisition skips deque push/erase; core release notifies only
if a ready thread is registered under the scheduler mutex. A new acquirer after
unlock sees a free core itself. Standard mutex/event locking remains unchanged.
Round-2 dispatch caches remain unchanged; their native regression passes.

## Cache cleaning

`switch_dc_store` cleans only the intersection with GPU-importable MEM2. Other
guest regions are read by the CPU and copied into uploads. Cleaning for MEM2
stays conservative: no shader/texture-page heuristic can establish that a range
will never be used as a direct vertex/index buffer. Both page-watch notifications
retain the original guest range, including non-imported regions. Actual flush
size/frequency cannot be determined from the HLE wrappers alone.

## Validation and target measurements

`python tools/switch/test_runtime.py -v` with Clang++ on PATH (or
`WWHD_RUNTIME_TEST_CC` set) passes six host tests. They exercise ring ownership,
wrap/full/idle cases and multiple producers; desktop and Switch emission,
concurrent fences/rollover, display-list overflow and inline execution; message
FIFO/front insertion; DC clipping; and host audio wrapping, underruns, drops,
flushes and sequence rollover. Randomized NEON intrinsic-model checks compare
10,000 frames bit-for-bit with scalar math. Real AArch64 intrinsic headers also
pass syntax checking. The model does not execute NEON hardware.

Changed desktop translation units and host-compiled Switch GX2/threads branches
pass Clang C++20 syntax checks. `tools/recomp/test_dispatch.py -v` passes at
O0/O2/O3. There is no Switch SDK build, game run or save/load integration test.

Expect modest main-thread savings from command copies, allocations and TLS, and
reduced AX work on core 0; no percentage or millisecond gain is established.
Measure main/render/record CPU ms/frame, AX samples, malloc/TLS/queue/DC-clean
profile share, and pacing on the same dock and heavy views. Check boot, several
audio-heavy scenes, loops/stream transitions, display-list users, and save/load
while audio/workers are active. Check both imported MEM2 and `WWHD_MEM2_COPY`,
plus `WWHD_NO_RENDER_THREAD`. Queue publication timing, ring backpressure, the
4 MiB allocation and 4 KiB uniform stack scratch are the main target risks.
