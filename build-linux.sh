#!/usr/bin/env bash
# Build the Linux desktop target for one or more architectures.
#
#   ./build-linux.sh                 # all three: x64 x86 arm64
#   ./build-linux.sh x64             # just one
#   ./build-linux.sh x64 arm64       # a subset
#   ./build-linux.sh --keep x64      # keep the build tree for incremental rebuilds
#   ./build-linux.sh --vr x64        # the SteamVR/OpenXR PCVR build instead
#
# Output lands in build/linux/<arch>/ as a self-contained folder: the avp_<arch>
# binary, the bundled .so's it needs (staged under their SONAMEs) and the assets.
#
# --vr builds the OpenXR PCVR target instead: avp_<arch>vr in build/linux/<arch>vr/,
# with libopenxr_loader.so staged alongside. It is a SEPARATE folder and a separate
# build tree from the flat build, so the two coexist and never share config.cfg or
# user_profiles. Only x64 has a bundled loader; see extern/OpenXR/lib/linux/.
# Build trees are kept out of the repo, in .build-linux/<arch>/, and are DELETED on
# success once the finished folder has been produced.
#
# A failed arch keeps its tree — CMakeCache.txt / CMakeFiles/CMakeError.log and the
# compile output are exactly what you need to diagnose it. Pass --keep to retain
# every tree, which is what you want while iterating: without it each run is a full
# rebuild from scratch, which for this codebase is minutes rather than seconds.
#
# Prerequisites on Debian/Ubuntu:
#   x64    sudo apt install build-essential cmake ninja-build libgl-dev
#   x86    sudo dpkg --add-architecture i386 && sudo apt update
#          sudo apt install gcc-multilib g++-multilib libgl-dev:i386
#   arm64  sudo dpkg --add-architecture arm64 && sudo apt update
#          sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu libgl-dev:arm64
#   --vr   additionally: sudo apt install libx11-dev   (the OpenXR GLX binding)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

keep_trees=0
vr=0
ARCHES=()
for a in "$@"; do
    case "$a" in
        --keep|-k) keep_trees=1 ;;
        --vr)      vr=1 ;;
        -*)        echo "unknown option '$a' (expected --keep or --vr)" >&2; exit 2 ;;
        *)         ARCHES+=("$a") ;;
    esac
done
[ ${#ARCHES[@]} -eq 0 ] && ARCHES=(x64 x86 arm64)

# The VR build differs from the flat one only by -DAVP_ENABLE_PCVR=ON; CMake derives
# the "vr" name and folder from that. It gets its own build TREE too, because a tree
# configured one way keeps AVP_ENABLE_PCVR in its CMakeCache and would otherwise
# silently rebuild the other target in place.
pcvr_flag=""
suffix=""
if [ "$vr" -eq 1 ]; then
    pcvr_flag="-DAVP_ENABLE_PCVR=ON"
    suffix="vr"
fi

ok=()
failed=()

# Delete one build tree. Deliberately narrow: it only ever removes
# $REPO/.build-linux/<arch>[vr], and the arch has already been through the case
# whitelist below (the "vr" is a literal this script appends), so there is no path
# here that a stray argument could widen.
drop_build_tree() {
    local tree="$REPO/.build-linux/$1"
    [ -d "$tree" ] || return 0
    rm -rf "$tree"
    echo "==> removed build tree .build-linux/$1"
}

for arch in "${ARCHES[@]}"; do
    case "$arch" in
        x64)   toolchain="" ;;
        x86)   toolchain="-DCMAKE_TOOLCHAIN_FILE=$REPO/source/cmake/toolchain-linux-x86.cmake" ;;
        arm64) toolchain="-DCMAKE_TOOLCHAIN_FILE=$REPO/source/cmake/toolchain-linux-arm64.cmake" ;;
        *)     echo "unknown arch '$arch' (expected x64, x86 or arm64)" >&2; exit 2 ;;
    esac

    # Flat and VR builds of the same arch are distinct trees and distinct outputs;
    # $suffix is empty for the flat build, so this is the old path unchanged.
    target="$arch$suffix"

    # One arch failing must not abort the others, so the whole attempt runs under
    # `if` (which suspends set -e) and the result is collected for the summary.
    if (
        echo "==> configuring $target"
        cmake -S "$REPO/source" -B "$REPO/.build-linux/$target" \
              -G Ninja \
              -DAVP_ENABLE_LINUX=ON \
              -DCMAKE_BUILD_TYPE=Release \
              $pcvr_flag \
              $toolchain
        echo "==> building $target"
        cmake --build "$REPO/.build-linux/$target" -j"$(nproc)"
    ); then
        echo "==> done: build/linux/$target/avp_$target"
        ok+=("$target")
        [ "$keep_trees" -eq 1 ] || drop_build_tree "$target"
    else
        echo "==> FAILED: $target (build tree kept at .build-linux/$target)" >&2
        failed+=("$target")
    fi
done

# Tidy the parent too, but only when it is empty: a failed arch still has its tree
# in there, and so does every arch when --keep is in play. rmdir refuses a non-empty
# directory, which is exactly the guard we want.
rmdir "$REPO/.build-linux" 2>/dev/null || true

echo
echo "built:  ${ok[*]:-none}"
echo "failed: ${failed[*]:-none}"
[ ${#failed[@]} -eq 0 ]
