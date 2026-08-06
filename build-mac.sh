#!/usr/bin/env bash
# Build the macOS desktop target for one or more architectures.
#
#   ./build-mac.sh                   # x64 (the only arch with bundled libs)
#   ./build-mac.sh x64               # same, explicit
#   ./build-mac.sh x64 arm64         # a subset / both
#   ./build-mac.sh --keep x64        # keep the build tree for incremental rebuilds
#
# Output lands in build/mac/<arch>/ as a self-contained folder: the avp_<arch>
# binary, the bundled .dylibs it needs (staged under their install names) and the
# assets. Build trees are kept out of the repo, in .build-mac/<arch>/, and are
# DELETED on success once the finished folder has been produced.
#
# A failed arch keeps its tree — CMakeCache.txt / CMakeFiles/CMakeError.log and the
# compile output are exactly what you need to diagnose it. Pass --keep to retain
# every tree, which is what you want while iterating: without it each run is a full
# rebuild from scratch.
#
# Prerequisites:
#   Xcode Command Line Tools   xcode-select --install     (clang, otool)
#   CMake 3.22+                brew install cmake
#   Ninja (optional)           brew install ninja         (falls back to make)
#
# SDL3, OpenAL and FFmpeg are bundled for x64 under source/extern/<dep>/lib/mac/x64,
# so nothing else is needed for that arch. arm64 has no bundled libs yet and falls
# back to system/Homebrew copies of all three — see the note the script prints.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Cross-building a Mach-O binary needs the macOS SDK and codesign, so this is
# Darwin-only. Fail loudly rather than emitting a broken configure on WSL.
if [ "$(uname -s)" != "Darwin" ]; then
    echo "build-mac.sh must run on macOS (uname -s says '$(uname -s)')." >&2
    exit 2
fi

if ! command -v otool >/dev/null 2>&1; then
    echo "otool not found — install the Xcode Command Line Tools:" >&2
    echo "    xcode-select --install" >&2
    echo "Without it CMake cannot read each dylib's install name and the staged" >&2
    echo "folder will be missing libavcodec.62.dylib etc." >&2
    exit 2
fi

# Ninja if it's there, otherwise Unix Makefiles — one less mandatory Homebrew
# package than the Linux script needs.
#
# Both are named explicitly, and both are SINGLE-config on purpose. Leaving the
# generator unset would honour a $CMAKE_GENERATOR of "Xcode", which is
# multi-config and appends the config name to RUNTIME_OUTPUT_DIRECTORY — the
# binary would land in build/mac/<arch>/Release/ instead of build/mac/<arch>/,
# splitting it from the dylibs and assets the post-build steps stage.
if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
else
    generator=(-G "Unix Makefiles")
fi

jobs="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"

keep_trees=0
ARCHES=()
for a in "$@"; do
    case "$a" in
        --keep|-k) keep_trees=1 ;;
        -*)        echo "unknown option '$a' (expected --keep)" >&2; exit 2 ;;
        *)         ARCHES+=("$a") ;;
    esac
done
[ ${#ARCHES[@]} -eq 0 ] && ARCHES=(x64)

ok=()
failed=()

# Delete one arch's build tree. Deliberately narrow: it only ever removes
# $REPO/.build-mac/<arch>, and $arch has already been through the case whitelist
# below, so there is no path here that a stray argument could widen.
drop_build_tree() {
    local tree="$REPO/.build-mac/$1"
    [ -d "$tree" ] || return 0
    rm -rf "$tree"
    echo "==> removed build tree .build-mac/$1"
}

for arch in "${ARCHES[@]}"; do
    case "$arch" in
        x64)   osx_arch="x86_64" ;;
        arm64) osx_arch="arm64"
               echo "note: no bundled libs for mac/arm64 — SDL3, OpenAL and FFmpeg will"
               echo "      come from the system (brew install sdl3 openal-soft ffmpeg)."
               echo "      Build x64 instead for the self-contained bundled path." ;;
        *)     echo "unknown arch '$arch' (expected x64 or arm64)" >&2; exit 2 ;;
    esac

    # One arch failing must not abort the others, so the whole attempt runs under
    # `if` (which suspends set -e) and the result is collected for the summary.
    if (
        echo "==> configuring $arch"
        cmake -S "$REPO/source" -B "$REPO/.build-mac/$arch" \
              "${generator[@]}" \
              -DAVP_ENABLE_MAC=ON \
              -DCMAKE_BUILD_TYPE=Release \
              -DCMAKE_OSX_ARCHITECTURES="$osx_arch"
        echo "==> building $arch"
        cmake --build "$REPO/.build-mac/$arch" -j"$jobs"
    ); then
        echo "==> done: build/mac/$arch/avp_$arch"
        ok+=("$arch")
        [ "$keep_trees" -eq 1 ] || drop_build_tree "$arch"
    else
        echo "==> FAILED: $arch (build tree kept at .build-mac/$arch)" >&2
        failed+=("$arch")
    fi
done

# Tidy the parent too, but only when it is empty: a failed arch still has its tree
# in there, and so does every arch when --keep is in play. rmdir refuses a non-empty
# directory, which is exactly the guard we want.
rmdir "$REPO/.build-mac" 2>/dev/null || true

echo
echo "built:  ${ok[*]:-none}"
echo "failed: ${failed[*]:-none}"
[ ${#failed[@]} -eq 0 ]
