# Vulkan shader warm-up

After `render::init()` has created the Vulkan device, `main.cpp` loads the shader
disk cache and replays its variant recipes before starting the game thread.
`WWHD_VK_SHADER_WARMUP=0` disables replay while preserving normal disk SPIR-V reuse.
The smoke-test path bypasses boot replay; it initializes guest memory separately.
Replay creates CPU Shader/decompiler objects, not Vulkan modules or pipelines.

## Persisted inputs and key verification

`WWVKSC03` adds a list of translation recipes to each stage/GLSL/SPIR-V entry.
Several translation keys can share GLSL, so every successful variant receives
its own recipe even on a disk/SPIR-V reuse hit. Recipes contain:

- The translation key and exact program bytes (1 MiB maximum). Stage is inherited
  from the owning disk entry; program size is the byte-vector length.
- The `state_hash` words, in their original order: 105 fixed words plus five
  words per texture unit, or four fewer words when streamout is disabled. The
  persisted fixed array has zero trailing words; hashing uses only active words.
  Primitive type is the masked `VGT_PRIMITIVE_TYPE` word in this array.
- For vertex shaders, fetch bytes, the original FS size register, and both
  instance step-rate registers. Compact `WWFS` programs contain their entire
  attribute/divisor description in 16-byte records (up to 64); raw programs
  contain their clauses/instructions and resolve indexed divisors from the two
  step-rate registers. Neither parser reads vertex-buffer contents.
- Extra analyzer annotations: all seven texture resource words for the stage,
  eight color bases/sizes, both generic scissor words and 16 attribute-stride
  words. These preserve framebuffer-alias annotations and the raw fetch parser's
  optional Metal stride annotation in dual-renderer builds. Vulkan does not use
  framebuffer fetch, so these reads do not expand the established variant key.

Hashing and replay use one templated register gather/scatter routine. For masked
words, any value with the same masked bits is equivalent for Vulkan translation;
the scatter routine zeros the other bits. Full annotation words restore the
recorded texture fields afterward. Optional streamout strides retain their order.
Neither the hash algorithm nor its active-word sequence changes.

Capture hashes the owned program/fetch copies and verifies that they reproduce
the actual translation key; the fetch must also be the cached object belonging
to those bytes/divisors. Unsupported or mismatched captures are omitted.
The Cemu parsers do not enforce program-size bounds on clause reads. Capture
and replay therefore check reachable CF/subroutine addresses, TEX clauses,
ALU instructions and grouped literals, raw fetch CF clauses and legacy attribute
reads. Recipes requiring bytes outside the stored range are omitted or skipped;
ordinary gameplay translation remains unchanged. Decompiler options, renderer API
and the adapted `ActiveSettings`/vendor shim are fixed Vulkan inputs, not guest
memory dependencies.

Replay uses one 256-byte-aligned guest scratch allocation (1 MiB + 4 KiB), a
zeroed register array, and normal fetch parsing and `translate_impl`, the common
implementation of `translate`. Standalone validation (`frame == ~uint64_t{0}`)
forces immediate byte hashing even though scratch addresses are reused. The
computed translation key must equal the recorded key **before** decompilation
or insertion. Generated GLSL must exactly match the disk entry, and its normal
source/recipe/stage lookup must find cached SPIR-V. Replay never invokes glslang
on a mismatch and inserts only ready, verified Shader objects. Resource mapping,
uniform offsets and descriptor ranks come from the normal decompiler path.

Afterward, address-based program hashes, last-shader/fetch memos and draw-state
memos are reset. Content-keyed Shader and fetch objects remain. Real lookups
hash/watch the real program addresses before selecting a warmed object. Switch
DC stores still advance the existing watched-page epoch, and changed bytes select
a different key. No new per-page write counters or DC-flush work are introduced.
Fetch parsers retain attribute data, not pointers into scratch program bytes.
The compact parser's address-valued `LatteFetchShader::key` is unused by Vulkan;
the Vulkan cache uses the content-derived `fsKey` instead.

## Bounds and migration

The schema and compile fingerprint both change; SC02 and incompatible files are
discarded atomically. The first session after upgrading must rebuild its cache
through gameplay. The existing checksum, SPIR-V validation, atomic replacement
and immutable save-worker snapshot remain in use. New structural checks bound
program/fetch lengths, compact headers, recipe counts and duplicate variant keys.

The disk payload remains limited to 128 MiB, with at most 65,536 canonical entries
and 65,536 recipes total. Each recipe adds 1,448 bytes of fixed fields plus its
program/fetch bytes (18 texture units). Disk recipes can therefore fill the cache
earlier than SPIR-V alone. Recording runs only on a successful translation miss,
not on steady-state draw hits, and marks the existing quiet-frame save mechanism
dirty even when no new GLSL compilation was needed.

Replay checks a 30-second deadline between variants, including disk-load time,
and uses a conservative 128 MiB admission estimate for retained Shader data:
two source copies including spare string capacity (three times source length),
SPIR-V, eight bytes per program byte for metadata vectors,
four bytes per fetch byte, fixed object sizes and 4 KiB overhead per variant.
The estimate deliberately charges shared fetch data repeatedly. It is separate
from disk-cache storage, scratch and transient decompiler allocations. Variants
that exceed admission are skipped; variants left at the deadline remain cold.
The final log reports warmed, skipped and unvisited counts, milliseconds and
which budget applied. One in-progress translation can extend past the deadline.

At the measured Switch cost of about 0.27 ms per variant, expect about 0.27 seconds
per 1,000 variants, plus disk I/O, parsing and input hashing. A 382-variant burst
moves roughly 103 ms of translation work to boot. These are estimates; verify
the logged time on hardware. Pipeline creation remains on demand.

## CPU regression test

With C++20 and glslang development packages available:

```sh
cmake -S runtime/tools/shader-warmup-test -B build/shader-warmup-test -DCMAKE_BUILD_TYPE=Release
cmake --build build/shader-warmup-test --parallel
ctest --test-dir build/shader-warmup-test --output-on-failure
```

The standalone harness uses the actual Cemu decompiler, raw parser, GX2 compact
parser, cache serializer/decoder and replay path. Four tests cover warm-up enabled
and disabled on desktop and with Switch memo/watch branches enabled on the host.
They exercise masked state/streamout round-trips, pixel and both fetch formats,
instance divisors, point-size uniforms, several keys sharing GLSL, key/source
rejection without insertion, out-of-range clause/literal/fetch rejection,
malformed caches with valid checksums, byte
accounting, metadata/rank reproduction, relocated real lookups and notified
program changes. No Vulkan device or game assets are required. Tests remove
only `spirv.bin` in their disposable build-directory cache paths.

On Switch, visit several shader-heavy views, let the cache checkpoint or close
orderly, then restart and revisit the same views. Check warm-up counts/time and
that the formerly cold translation bursts disappear. Compare with
`WWHD_VK_SHADER_WARMUP=0`, check rendering/point-size/instancing, and repeat after
a save-state load. Also test a large cache to confirm budget logs and cold fallback.
The host Switch-branch test is not a devkitA64 build or a hardware timing test.
