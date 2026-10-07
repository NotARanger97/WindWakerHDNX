#!/usr/bin/env bash
# Build the Switch homebrew (.nro) from your own copy of the game, in Docker (docs/switch.md).
#
#   tools/switch/build.sh --nvk DIR (--rpx FILE [--region us|eu] | --gen DIR) [--jobs N]
#   tools/switch/build.sh --nvk DIR --sdk [--jobs N]
#
#   --rpx FILE    the game's code/cking.rpx from your own dump; it is recompiled to C in
#                 build/gen-<region> (once; minutes), which is never committed or distributed
#   --region      us (default) or eu: the EU executable is recompiled without the USA-address hooks
#                 (60 fps modes and mods stay unavailable there; the game itself is complete)
#   --gen DIR     reuse an existing recompiler output instead of --rpx (a directory under the repo)
#   --nvk DIR     directory with libnvk_local.o, the Mesa NVK Vulkan driver for Horizon (docs/switch.md)
#   --jobs N      parallel compile jobs (default: all cores; the game code needs ~1 GB per job)
#   --sdk         build the Switch SDK for the PC builder instead (no game needed): sdk-switch/
#                 (tools/switch/builder.py makes the .nro from a dump with it, without Docker)
#
# Output: build/switch-<region>/wwhd.nro. The Docker image (tools/switch/Dockerfile) is built on first use.
set -euo pipefail
cd "$(dirname "$0")/../.."
REPO="$(pwd)"
RPX="" REGION="us" GEN="" NVK="" JOBS="" SDK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rpx) RPX="$2"; shift 2 ;;
    --region) REGION="$2"; shift 2 ;;
    --gen) GEN="$2"; shift 2 ;;
    --nvk) NVK="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --sdk) SDK=1; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
[ -n "$NVK" ] && [ -f "$NVK/libnvk_local.o" ] || { echo "--nvk DIR must contain libnvk_local.o (docs/switch.md)" >&2; exit 2; }
[ "$REGION" = us ] || [ "$REGION" = eu ] || { echo "--region must be us or eu" >&2; exit 2; }
IMAGE=wwhd-switch-build
export MSYS_NO_PATHCONV=1  # Git Bash on Windows: keep container paths as they are
if [ -n "$SDK" ]; then
  docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f tools/switch/Dockerfile tools/switch
  docker run --rm -v "$REPO:/src" -v "$(cd "$NVK" && pwd):/nvk:ro" -e JOBS="$JOBS" "$IMAGE" bash -c '
    set -euo pipefail
    cd /src
    cmake -S . -B build/switch-sdk -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake       -DSWITCH_MESA_SDK_ROOT=/nvk -DVULKAN_HEADERS=/opt/vulkan-headers/include -DWWHD_SWITCH_SDK=ON
    cmake --build build/switch-sdk ${JOBS:+--parallel $JOBS}
    rm -rf sdk-switch && cp -r build/switch-sdk/sdk sdk-switch
    echo "=== built sdk-switch/"
  '
  exit 0
fi
if [ -z "$GEN" ]; then
  [ -n "$RPX" ] && [ -f "$RPX" ] || { echo "--rpx FILE (code/cking.rpx of your dump) or --gen DIR is required" >&2; exit 2; }
  GEN="build/gen-$REGION"
fi
case "$GEN" in /*) echo "--gen must be a directory inside the repository (relative path)" >&2; exit 2 ;; esac
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" -f tools/switch/Dockerfile tools/switch
MOUNTS=(-v "$REPO:/src" -v "$(cd "$NVK" && pwd):/nvk:ro")
[ -n "$RPX" ] && MOUNTS+=(-v "$(cd "$(dirname "$RPX")" && pwd)/$(basename "$RPX"):/game/cking.rpx:ro")
docker run --rm "${MOUNTS[@]}" -e REGION="$REGION" -e GEN="$GEN" -e JOBS="$JOBS" -e HAVE_RPX="${RPX:+1}" "$IMAGE" bash -c '
  set -euo pipefail
  cd /src
  if [ -n "$HAVE_RPX" ] && [ ! -f "$GEN/table.c" ]; then
    echo "=== recompiling cking.rpx ($REGION) into $GEN"
    if [ "$REGION" = eu ]; then
      : > /tmp/no-hooks.txt
      WWHD_HOOKS=/tmp/no-hooks.txt python3 tools/recomp/recomp.py /game/cking.rpx "$GEN"
      python3 tools/recomp/region_compat.py "$GEN"
    else
      python3 tools/recomp/recomp.py /game/cking.rpx "$GEN"
    fi
  fi
  [ -f "$GEN/table.c" ] || { echo "no recompiler output in $GEN" >&2; exit 1; }
  OUT=build/switch-$REGION
  cmake -S . -B "$OUT" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
    -DSWITCH_MESA_SDK_ROOT=/nvk -DVULKAN_HEADERS=/opt/vulkan-headers/include -DGEN_DIR="/src/$GEN"
  cmake --build "$OUT" ${JOBS:+--parallel $JOBS}
  echo "=== built $OUT/wwhd.nro"
'
