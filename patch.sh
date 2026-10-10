#!/bin/bash
#
# Build and install mcpelauncher from source, with this repository's patches
# applied to the relevant submodules.
#
# It clones mcpelauncher-manifest at a pinned commit, applies every patch in
# ./patches to the submodule it belongs to, then configures, builds and
# installs the launcher into ~/.local (override with PREFIX).
#
# Environment overrides:
#   PREFIX      install prefix (default: $HOME/.local)
#   SRC_DIR     checkout directory (default: <script dir>/mcpelauncher-manifest)
#   BUILD_DIR   build directory (default: <SRC_DIR>/build)
#   CC / CXX    compilers (default: clang / clang++)
#   CMAKE_ARGS  extra arguments appended to the cmake configure command
#   JOBS        parallel build jobs (default: number of CPUs)
#
set -euo pipefail

MANIFEST_REPO="https://github.com/minecraft-linux/mcpelauncher-manifest"
MANIFEST_COMMIT="91220f0f7ccfd460090d14056ab1bf12e82869b1"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
PATCH_DIR="$SCRIPT_DIR/patches"

PREFIX="${PREFIX:-$HOME/.local}"
SRC_DIR="${SRC_DIR:-$SCRIPT_DIR/mcpelauncher-manifest}"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build}"

: "${CC:=clang}"
: "${CXX:=clang++}"
if ! command -v "$CC" >/dev/null 2>&1 || ! command -v "$CXX" >/dev/null 2>&1; then
    echo "warning: '$CC'/'$CXX' not found, falling back to CMake's default compilers" >&2
    unset CC CXX
fi

if [ -z "${JOBS:-}" ]; then
    JOBS="$(nproc 2>/dev/null || echo 4)"
fi

if [ ! -d "$PATCH_DIR" ]; then
    echo "error: patch directory not found: $PATCH_DIR" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Clone / update the manifest and check out the pinned commit
# ---------------------------------------------------------------------------
if [ -e "$SRC_DIR" ] && [ ! -e "$SRC_DIR/.git" ]; then
    echo "==> Removing non-git directory $SRC_DIR"
    rm -rf "$SRC_DIR"
fi

if [ -e "$SRC_DIR/.git" ]; then
    echo "==> Updating existing checkout in $SRC_DIR"
    git -C "$SRC_DIR" fetch --all --tags
else
    echo "==> Cloning $MANIFEST_REPO"
    git clone "$MANIFEST_REPO" "$SRC_DIR"
fi

echo "==> Checking out mcpelauncher-manifest $MANIFEST_COMMIT"
git -C "$SRC_DIR" checkout --force "$MANIFEST_COMMIT"

echo "==> Updating submodules (this can take a while)"
git -C "$SRC_DIR" submodule sync --recursive
git -C "$SRC_DIR" submodule update --init --recursive --force

# ---------------------------------------------------------------------------
# 2. Apply the patches to whichever submodule they belong to
# ---------------------------------------------------------------------------
mapfile -t SUBMODULES < <(git -C "$SRC_DIR" submodule status --recursive | awk '{print $2}')

shopt -s nullglob
PATCHES=("$PATCH_DIR"/*.patch)
shopt -u nullglob
if [ "${#PATCHES[@]}" -eq 0 ]; then
    echo "error: no patches found in $PATCH_DIR" >&2
    exit 1
fi

for patch in "${PATCHES[@]}"; do
    name="$(basename "$patch")"
    applied=false
    for sub in "${SUBMODULES[@]}"; do
        if git -C "$SRC_DIR/$sub" apply --check "$patch" >/dev/null 2>&1; then
            echo "==> Applying $name to $sub"
            git -C "$SRC_DIR/$sub" apply "$patch"
            applied=true
            break
        fi
    done
    if [ "$applied" = false ]; then
        echo "error: $name does not apply to any submodule" >&2
        echo "       (is the manifest pinned at the right commit?)" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# 3. Configure, build and install
# ---------------------------------------------------------------------------
echo "==> Configuring (prefix: $PREFIX)"
# shellcheck disable=SC2086
cmake -S "$SRC_DIR" -B "$BUILD_DIR" \
    -Wno-dev \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DGAMEWINDOW_SYSTEM=SDL3 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5  \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DSDL_PIPEWIRE=off \
    ${CMAKE_ARGS:-}

echo "==> Building with $JOBS job(s)"
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "==> Installing into $PREFIX"
cmake --install "$BUILD_DIR"

echo "==> Done. Launcher installed at $PREFIX/bin/mcpelauncher-client"
