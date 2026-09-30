#!/usr/bin/env bash
# Screen checks for the game on Wine's Wayland driver (KDE Plasma): build tools/wwm/wwm_kwin_grab
# and authorize it, and only it, to capture windows through KWin's ScreenShot2 D-Bus interface.
# KWin grants that interface to executables named in a .desktop file with
# X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2 (as it does for Spectacle).
#
#   tools/wwm/setup_wayland_capture.sh           # build if stale, install the .desktop file
#   tools/wwm/setup_wayland_capture.sh --check   # change nothing; exit 1 if setup is needed
#   tools/wwm/setup_wayland_capture.sh --remove  # remove the authorization
set -euo pipefail

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
helper="$TOOLS/wwm_kwin_grab"
src="$TOOLS/wwm_kwin_grab.c"
desktop="${XDG_DATA_HOME:-$HOME/.local/share}/applications/wwm-kwin-grab.desktop"
want="$(sed "s|@WWM_KWIN_GRAB@|$helper|" "$TOOLS/wwm-kwin-grab.desktop.in")"

stale() { [ ! -x "$helper" ] || [ "$src" -nt "$helper" ]; }
authorized() { [ -f "$desktop" ] && [ "$(cat "$desktop")" = "$want" ]; }

case "${1:-}" in
    --check)
        if stale; then echo "wwm_kwin_grab is missing or older than its source" >&2; exit 1; fi
        if ! authorized; then echo "wwm_kwin_grab is not authorized ($desktop)" >&2; exit 1; fi
        exit 0 ;;
    --remove)
        if [ -f "$desktop" ]; then rm -- "$desktop"; kbuildsycoca6 >/dev/null 2>&1; fi
        echo "removed $desktop"
        exit 0 ;;
    "") ;;
    *) echo "usage: $0 [--check|--remove]" >&2; exit 2 ;;
esac

if stale; then
    cc -O2 -Wall -Wextra -o "$helper" "$src" $(pkg-config --cflags --libs gio-unix-2.0)
    echo "built $helper"
fi
if ! authorized; then
    mkdir -p "$(dirname "$desktop")"
    printf '%s\n' "$want" >"$desktop"
    # KWin looks the caller up through KService, so refresh its cache now rather than on its own.
    kbuildsycoca6 >/dev/null 2>&1
    echo "authorized $helper ($desktop)"
fi
echo "Wayland window capture ready"
