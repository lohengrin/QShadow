#!/usr/bin/env bash
# Installs the QShadow desktop entry and icon for the current user so it shows
# up in the applications menu (GNOME/Ubuntu). Run after a successful build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/qshadow"

if [[ ! -x "$BIN" ]]; then
    echo "qshadow binary not found at $BIN" >&2
    echo "Build it first: cmake --build build" >&2
    exit 1
fi

APPDIR="$HOME/.local/share/applications"
ICONDIR="$HOME/.local/share/icons/hicolor/scalable/apps"

mkdir -p "$APPDIR" "$ICONDIR"

# The committed desktop file uses a generic Exec; the installed copy points at
# this checkout's binary.
sed "s|^Exec=qshadow$|Exec=$BIN|" "$ROOT/qshadow.desktop" > "$APPDIR/qshadow.desktop"
chmod 0644 "$APPDIR/qshadow.desktop"
cp "$ROOT/qshadow.svg" "$ICONDIR/qshadow.svg"

update-desktop-database "$APPDIR" 2>/dev/null || true
gtk-update-icon-cache "$HOME/.local/share/icons/hicolor" 2>/dev/null || true

echo "Installed: $APPDIR/qshadow.desktop"
echo "Icon:      $ICONDIR/qshadow.svg"