#!/usr/bin/env bash
# Install a PNG icon for the cpu-throttle tray into the user's local icon theme.
# Usage: ./scripts/install-user-icon.sh /path/to/icon.png
set -euo pipefail
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 /path/to/icon.png" >&2
  exit 2
fi
SRC="$1"
if [ ! -f "$SRC" ]; then
  echo "Source file not found: $SRC" >&2
  exit 2
fi
DESTDIR="$HOME/.local/share/icons/hicolor/32x32/apps"
mkdir -p "$DESTDIR"
cp "$SRC" "$DESTDIR/cpu-throttle.png"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  ICONCACHE_DIR="$HOME/.local/share/icons/hicolor"
  gtk-update-icon-cache -t -f "$ICONCACHE_DIR" || true
fi
echo "Installed $SRC -> $DESTDIR/cpu-throttle.png"
