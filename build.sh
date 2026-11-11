#!/usr/bin/env bash
# Builds Proving Ground Recompiled on Linux.
#
#   ./build.sh                     Release build in out/build/release, staged in out/dist
#   ./build.sh RelWithDebInfo      Build with symbols and performance counters
#
# Steps: fetch ReXGlue v0.10.0 into thirdparty/rexglue-sdk and apply patches/rexglue-sdk.patch
# (first run only), build the rexglue code generator, generate code from assets/default.xex,
# then build the game. Needs Clang 20 or newer, CMake 3.25+, Ninja and the libraries listed in
# docs/BUILDING.md. CC/CXX choose the compiler; REXSDK_SOURCE an already patched SDK tree.
set -euo pipefail

CONFIG="${1:-Release}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_TAG="v0.10.0"
SDK_URL="https://github.com/rexglue/rexglue-sdk.git"
PATCH="$REPO/patches/rexglue-sdk.patch"
JOBS="${JOBS:-$(nproc)}"
LOGS="$REPO/out/logs"
mkdir -p "$LOGS"

step() {
    local name="$1"; shift
    echo "==> $name"
    if ! "$@" >"$LOGS/$name.log" 2>&1; then
        tail -n 40 "$LOGS/$name.log"
        echo "$name failed. Full log: $LOGS/$name.log" >&2
        exit 1
    fi
}

if [[ ! -f "$REPO/assets/default.xex" ]]; then
    echo "Missing assets/default.xex. Extract your Xbox 360 copy of the game into assets first." >&2
    exit 1
fi

# --- ReXGlue SDK source ---
if [[ -n "${REXSDK_SOURCE:-}" ]]; then
    SDK="$(cd "$REXSDK_SOURCE" && pwd)"
else
    SDK="$REPO/thirdparty/rexglue-sdk"
    if [[ ! -f "$SDK/CMakeLists.txt" ]]; then
        step sdk-clone git clone --branch "$SDK_TAG" --depth 1 "$SDK_URL" "$SDK"
    fi
    PATCH_HASH="$(sha256sum "$PATCH" | cut -d' ' -f1)"
    if [[ ! -f "$SDK/.pg-patch" ]]; then
        # Until the tree is patched, so a submodule fetch that failed part way is completed.
        step sdk-submodules git -C "$SDK" submodule update --init --recursive --depth 1 --jobs 8
        step sdk-patch git -C "$SDK" apply --whitespace=nowarn "$PATCH"
        echo "$PATCH_HASH" > "$SDK/.pg-patch"
    elif [[ "$(cat "$SDK/.pg-patch")" != "$PATCH_HASH" ]]; then
        echo "warning: patches/rexglue-sdk.patch changed since thirdparty/rexglue-sdk was patched;" \
             "delete thirdparty/rexglue-sdk to fetch and patch it again." >&2
    fi
fi

# --- Compiler ---
pick() { for c in "$@"; do command -v "$c" >/dev/null 2>&1 && { echo "$c"; return; }; done; }
CC="${CC:-$(pick clang-22 clang-21 clang-20 clang)}"
CXX="${CXX:-$(pick clang++-22 clang++-21 clang++-20 clang++)}"
if [[ -z "$CC" || -z "$CXX" ]]; then
    echo "Clang 20 or newer is required (set CC and CXX to choose one)." >&2
    exit 1
fi
echo "Compiler: $CXX"

BUILD="$REPO/out/build/$(echo "$CONFIG" | tr '[:upper:]' '[:lower:]')"
configure() {
    step "configure-$1" cmake -S "$REPO" -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DREXSDK_DIR="$SDK" -DPG_BUILD_GAME="$1" \
        -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_BUILD_TESTS=OFF \
        -DREXGLUE_ENABLE_FIDELITYFX=OFF \
        -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2
}

configure OFF
step build-codegen cmake --build "$BUILD" --target rexglue --parallel "$JOBS"
# The SDK writes its tools to its own out/<platform> folder, named with a per-configuration suffix.
case "$CONFIG" in RelWithDebInfo) TOOL=rexgluerd ;; Debug) TOOL=rexglued ;; *) TOOL=rexglue ;; esac
REXGLUE="$(find "$BUILD" "$SDK/out" -type f -name "$TOOL" -perm -u+x -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n 1 | cut -d' ' -f2-)"
[[ -n "$REXGLUE" ]] || { echo "The rexglue code generator was not built." >&2; exit 1; }
(cd "$REPO" && step codegen "$REXGLUE" codegen "$REPO/pg_config.toml")

configure ON
step build-game cmake --build "$BUILD" --target pg --parallel "$JOBS"
[[ -x "$BUILD/ProvingGround" ]] || { echo "Build finished but $BUILD/ProvingGround is missing." >&2; exit 1; }

DIST="$REPO/out/dist/ProvingGroundRecomp"
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/ProvingGround" "$DIST/"
find "$BUILD" -maxdepth 1 -name '*.so*' -exec cp -P {} "$DIST/" \;
# The runtime library stays in the SDK's out/<platform> folder; the game finds it through $ORIGIN.
RUNTIME_LIB="$(ldd "$BUILD/ProvingGround" | awk '/librexruntime/ {print $1; exit}')"
find "$SDK/out" -name "$RUNTIME_LIB" -exec cp -P {} "$DIST/" \; -quit
[[ -f "$DIST/$RUNTIME_LIB" ]] || { echo "Could not find $RUNTIME_LIB to stage." >&2; exit 1; }
cp "$REPO/LICENSE" "$REPO/THIRD_PARTY_NOTICES.md" "$DIST/"
cp "$REPO/docs/PLAYING.md" "$DIST/README.md"
echo "Ready to play: $DIST/ProvingGround"
