# Switch Vulkan recording thread

`WWHD_VK_RECORD_THREAD` defaults to ON on Switch; `0` restores direct NVK
forwarding and the original fence-reset position. Desktop builds are unchanged.
`WWHD_VK_RECORD_CORE` defaults to `0` (valid cores: 0–2), and
`WWHD_VK_RECORD_PRIO` defaults to `0x2D` (decimal or hex, valid 0x20–58).
The renderer is normally on core 2 at 0x2C; guests run at 59. The helper names
itself **VK record** before `switch_set_helper_thread` registers it with the
sampling profiler (`WWHD_PROFILE=1` or a list including `VK record`).

## Storage and ordering

The renderer is the single producer; a process-lifetime helper is the single
consumer. Startup/renderer smoke can produce on the startup thread instead,
but concurrent producers are unsupported. A fixed 8 MiB CPU ring contains
16-byte-aligned call records, argument tuples, and recursively copied arrays.
Handles are values. Copies are complete before publication; consumer completion
releases storage only after NVK returns. Monotonic byte sequences include wrap
padding. A record larger than the ring aborts. The hot path allocates nothing.

Each call is published immediately, so replay overlaps frame preparation. An
atomic parked flag and an autoclearing libnx kernel event avoid futex/condition
variable waits and avoid producer syscalls unless the consumer is parking.
The consumer spins for 128 iterations before announcing parking and rechecking
publication. Sequentially consistent publication/parking operations prevent a
missed wake; an early event signal stays latched. Ring-full and drain waits use
a short timed sleep, allowing the recorder to run even on the producer's core.

`vk_record_calls.inc` is the only deferred-call/signature/copy-policy list.
The handwritten file expands it into public wrappers; `gen_vk_switch.py` emits
only `nvk_real_vk*` forwarders for those names. Generation fails on an unlisted
runtime `vkCmd*` call or proc-name string, or a mismatched Vulkan signature.
CMake tracks sources and the policy as configure dependencies. Proc lookups for
known deferred/synchronizing functions return wrappers too; unknown command
lookups abort. Adding a raw pointer without an ownership policy fails compilation.

All runtime command calls, begin/end/reset, submit/submit2, and present replay
in FIFO order. Their result-returning wrappers report `VK_SUCCESS` immediately.
A deferred non-success result is logged, remains sticky, and is reported at
the next drain. Synchronizing result-returning APIs return it before entering
NVK, avoiding a fence wait after a failed submit. Remaining records are discarded
after failure, including presents waiting on a failed submit's signal semaphore;
their tuples are trivially destructible. Void teardown still runs after
the drain. This deliberately fails closed on deferred errors, including present
out-of-date/suboptimal results; it does not reproduce same-call WSI recovery.
Disable recording when investigating WSI recovery behavior.

No deferred runtime structure currently supplies `pNext`, including rendering
attachments, barriers, begin inheritance, and submit structures. Every such
chain is checked and a non-null chain aborts rather than copying unknown structs.
Rendering attachments, submit arrays (including submit2's nested structs),
present wait/swapchain/index arrays, dynamic offsets, regions, viewport/scissor,
clear values and push-constant bytes are copied. Present `pResults` is always
null in this renderer; non-null asynchronous output is rejected explicitly.

## Host-call decisions

A drain waits for **host replay**, not GPU completion. The renderer's existing
fence/idle rules still protect objects and upload bytes from the GPU.

| Calls | Thread and boundary | Reason |
| --- | --- | --- |
| All listed `vkCmd*`, begin/end/reset, queue submit/submit2/present | Consumer, FIFO | One owner records and accesses the queue; arguments are owned. |
| Any other direct queue, command buffer/pool, fence, semaphore, swapchain, query-pool handle argument (including output handles) | Producer, drain first | Includes acquire, fence status/wait/reset, query reads, command allocation/free/reset, swapchain queries, and associated creation/destruction. No concurrent host access to these objects. |
| Device/queue idle; wait/signal semaphores and counter query | Producer, drain first | Handles can be nested in info structs; explicitly classified. |
| All destroy/free calls; unmap memory; reset descriptor pool | Producer, drain first | Includes images, buffers, memory, views, samplers, pipelines/layouts, descriptor pools/sets, and device/instance teardown. Protects even non-retirement teardown paths. GPU completion is still the caller's responsibility. |
| Allocate descriptor sets | Producer, no drain | Pool allocation metadata is producer-owned; replay only reads already published sets and immutable pool memory/address metadata. It does not traverse the allocation list. |
| Update descriptor sets | Producer, no drain | `bind_stage` allocates on a content miss and writes before binding; cache hits never update. Present allocates each set before writing/binding; smoke does likewise. Sets already bound in a submission are immutable. |
| Create pipelines/images/buffers/views/samplers/layouts; allocate/map/bind memory | Producer, no drain | Newly created/different objects, driver shared allocation paths have locks/atomic publication (audit below). |
| Flush mapped memory ranges | Producer, no drain | `submit()` calls `flush_uploads()` before enqueueing submit. Upload blocks and expanded dirty atoms belong exclusively to that submission until its GPU fence retires. |

`retire_submission` checks/waits for the fence (both APIs drain), collects queries,
then `cleanup_submission` recycles uploads/images and resets the command and
descriptor pools. Fence reset moves before command-buffer begin only when the
recorder is enabled: the active slot is already retired, so this is legal and
avoids draining the current batch immediately after end. Acquire drains before
entering WSI; present is queued after submit. The producer cannot enqueue during
its synchronous call, so queue/command-buffer/swapchain host access cannot overlap.

GPU readback review: capture and smoke use `flush_readback()`; signatures and
present capture use `wait_submission()` before dereferencing mapped output.
These retire a submission with a fence wait/status check. Absent serials have
already been retired. No unfenced GPU-written CPU read was found in these paths.

## NVK audit and remaining risks

Reviewed the unmodified NVK sources of mesa-switch (`src/`):

- `nvk_cmd_buffer.c`'s `nvk_bind_descriptor_sets` reads
  `set->dynamic_buffers[j]` during recording. `nvk_descriptor_set.c` writes those
  fields during update, so the immutable-set rule above is essential. Set storage
  is aligned to at least the non-coherent atom (`min_set_align_B`), avoiding
  fresh-set cache cleans sharing atoms with another set.
- `nvk_cmd_pool.c` has unprotected command-memory/free lists. Recording owns them;
  producer allocate/free/reset/destroy calls drain first. Descriptor-pool lists
  and heaps are producer-owned and are not traversed by binding.
- `nvk_heap.c`, `nvk_descriptor_table.c`, `nvk_upload_queue.c`, and SLM allocation
  in `nvk_device.c` lock shared allocators. Arena slots are fixed, append-only,
  initialized before atomic `mem_count` publication (`nvk_mem_arena.h/.c`), so
  unlocked readers of already allocated memory tolerate growth. Descriptor-table
  base VA is stable; queue allocation-count reads take the arena lock.
- `nvkmd.c` protects the device memory list and mapping state. Horizon memory/BO
  cache/identity/VA bookkeeping uses locks; memory and device references and heap
  usage are atomic. `nvkmd_switch_dev.c` creates separate graphics/copy contexts;
  the upload mutex serializes the shared upload context. Horizon channels and
  device submission ordering are locked. Switch sync state uses a mutex.
- Horizon cache helpers operate on retained memory identities and explicit ranges;
  optional timing counters are locked. ZBC uses locked writers and atomic sequence
  snapshots. Mesa meta and pipeline caches have their own locks. WSI's poisoned
  window registry is locked; runtime acquire/present accesses are drained/FIFO.

No additional unprotected shared allocator state was identified on these paths.
This is source inspection, not a proof of every NVK/libnx path or a hardware
concurrency test. Driver lock contention, concurrent first-use meta/shader
creation, cache coherency across cores, and kernel-event/scheduler behavior need
Switch testing. Do not update a previously bound descriptor set without adding
an ordering boundary. No guest DC-flush bookkeeping is added.

## Validation

With `WWHD_VK_STATS` set, every 120 renderer frames logs deferred calls/frame,
ring high-water bytes, full stalls, and drain-wait count/total milliseconds.
Counters/tick queries/high-water sampling are skipped otherwise. Existing phase
timers now measure enqueue costs; profile **VK record** to measure NVK replay.

Host test (C++20, simulated libnx/NVK; no GPU or Switch ABI validation):
`python tools/switch/test_vk_record_thread.py <Vulkan-Headers/include> <clang++>`.
It checks owned snapshots, FIFO, wrap/full/idle wakeups, synchronization, sticky
errors, disable mode, proc lookup routing, and unknown command/pNext rejection.

On Switch run `--renderer-smoke` with recording enabled and disabled; compare
captures/signatures, dynamic UBOs, pool reuse, asynchronous submissions, and
repeated presentation. Then compare Outset at ~4,000 draws/frame using the same
core map, stats and sampling: target render CPU below ~31 ms/frame, with no
20-fps FIFO quantization. Exercise scene changes, capture/readbacks, dock/resize,
teardown, and pipeline/texture allocation churn. The Switch toolchain/libnx ABI
and performance have not been built or tested here.
