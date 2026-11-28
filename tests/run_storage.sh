#!/usr/bin/env bash
# Build tests/test_storage.c and run it under one or more Proton runners, each in its own throwaway
# prefix with D: mapped to the game disk as in the game prefix (never the game prefix itself: a bare
# `wine` there triggers a prefix update).
#
#   tests/run_storage.sh GE-Proton10-34 GE-Proton11-custom
#   TARGETS='\\.\C: D:\' tests/run_storage.sh GE-Proton11-custom
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNERS="$HOME/.local/share/lutris/runners/wine"
GAME_DISK="${GAME_DISK:-/media/gamedisk/}"
[ "$#" -gt 0 ] || { echo "usage: $0 <runner-name>..." >&2; exit 2; }
[ -d "$GAME_DISK" ] || { echo "error: game disk $GAME_DISK not mounted" >&2; exit 1; }

x86_64-w64-mingw32-gcc -O2 -Wall "$SCRIPT_DIR/test_storage.c" -o "$SCRIPT_DIR/test_storage.exe"

run_proton() {
    STEAM_COMPAT_DATA_PATH="$pfx" \
    STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
    PROTON_USE_NTSYNC=1 PROTON_NO_NTSYNC=0 WINENTSYNC=1 WINEFSYNC=0 PROTON_USE_FSYNC=0 \
        "$RUNNERS/$runner/proton" run "$@" >/dev/null 2>&1
}

for runner in "$@"; do
    [ -x "$RUNNERS/$runner/proton" ] || { echo "error: no proton script in $RUNNERS/$runner" >&2; exit 1; }
    pfx="${XDG_CACHE_HOME:-$HOME/.cache}/storage_pfx_$runner"
    mkdir -p "$pfx"
    if [ ! -d "$pfx/pfx/dosdevices" ]; then
        # first use: let Proton create the prefix, then map D: like the game prefix
        run_proton "$SCRIPT_DIR/test_storage.exe" --log 'C:\test_storage.log'
    fi
    ln -sfn "$GAME_DISK" "$pfx/pfx/dosdevices/d:"
    rm -f "$pfx/pfx/drive_c/test_storage.log"
    echo "=== $runner"
    # shellcheck disable=SC2086
    run_proton "$SCRIPT_DIR/test_storage.exe" --log 'C:\test_storage.log' ${TARGETS:-}
    tr -d '\r' < "$pfx/pfx/drive_c/test_storage.log"
    WINEPREFIX="$pfx/pfx" "$RUNNERS/$runner/files/bin/wineserver" -w
done
