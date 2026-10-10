#!/bin/bash
#
# Build and install the MCPE Launcher into ~/.local (override with PREFIX).
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
cd "$SCRIPT_DIR"

PREFIX="${PREFIX:-$HOME/.local}"
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

echo "==> Configuring meson (prefix: $PREFIX)"
meson setup "$BUILD_DIR" --prefix "$PREFIX"

echo "==> Building with $JOBS job(s)"
meson compile -C "$BUILD_DIR" -j "$JOBS"

echo "==> Installing into $PREFIX"
meson install -C "$BUILD_DIR"

ICON_CACHE="$PREFIX/share/icons/hicolor"
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -d "$ICON_CACHE" ]; then
    gtk-update-icon-cache -f -t "$ICON_CACHE" 2>/dev/null || true
fi
if command -v update-desktop-database >/dev/null 2>&1 && [ -d "$PREFIX/share/applications" ]; then
    update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi

echo "==> Done. Run $PREFIX/bin/mcpe-launcher (or 'mcpe-launcher' if $PREFIX/bin is on PATH)."