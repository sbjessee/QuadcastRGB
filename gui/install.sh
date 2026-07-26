#!/bin/bash
# Installs the quadcastrgb GUI/tray app for the current user (XDG dirs).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="${HOME}/.local/bin"
APPDIR="${HOME}/.local/share/applications"
AUTOSTARTDIR="${HOME}/.config/autostart"
BINPATH="${BINDIR}/quadcastrgb-gui"

command -v quadcastrgb >/dev/null || {
    echo "quadcastrgb CLI not found on PATH. Install/build it first (see main README)." >&2
    exit 1
}
python3 -c "import PySide6" 2>/dev/null || {
    echo "PySide6 not found. Install it, e.g.: sudo pacman -S pyside6" >&2
    exit 1
}

mkdir -p "$BINDIR" "$APPDIR" "$AUTOSTARTDIR"

install -m 755 "${SCRIPT_DIR}/quadcastrgb_gui.py" "$BINPATH"

sed "s|__BINPATH__|${BINPATH}|" "${SCRIPT_DIR}/quadcastrgb-gui.desktop.in" \
    > "${APPDIR}/quadcastrgb-gui.desktop"
cp "${APPDIR}/quadcastrgb-gui.desktop" "${AUTOSTARTDIR}/quadcastrgb-gui.desktop"

echo "Installed:"
echo "  ${BINPATH}"
echo "  ${APPDIR}/quadcastrgb-gui.desktop        (app menu entry)"
echo "  ${AUTOSTARTDIR}/quadcastrgb-gui.desktop  (autostart on login)"
echo
echo "Launch now with: quadcastrgb-gui"
