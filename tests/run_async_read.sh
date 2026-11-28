#!/usr/bin/env bash
# Build tests/test_async_read.c and run it under one or more Proton runners, each in its
# own throwaway prefix, against a file on the game volume and one on the home filesystem.
#
#   tests/run_async_read.sh GE-Proton10-34 GE-Proton11-custom
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNERS="$HOME/.local/share/lutris/runners/wine"
GAME_FILE="${GAME_FILE:-$(find /media/gamedisk/Games/wwm/wwm_standard -type f -size +200M -print -quit)}"
HOME_FILE="${HOME_FILE:-$HOME/.cache/test_async_read_home.bin}"
COUNT="${COUNT:-5000}"   # the skip/noskip checks use min(COUNT, 2000); timers add ~40 s per runner and file
[ "$#" -gt 0 ] || { echo "usage: $0 <runner-name>..." >&2; exit 2; }
[ -f "$GAME_FILE" ] || { echo "error: no game file found for GAME_FILE" >&2; exit 1; }
[ -f "$HOME_FILE" ] || head -c 400M /dev/urandom > "$HOME_FILE"

x86_64-w64-mingw32-gcc -O2 -Wall "$SCRIPT_DIR/test_async_read.c" -o "$SCRIPT_DIR/test_async_read.exe"
# warm the page cache so the numbers measure Wine, not the disk
cat "$GAME_FILE" "$HOME_FILE" > /dev/null

for runner in "$@"; do
    [ -x "$RUNNERS/$runner/proton" ] || { echo "error: no proton script in $RUNNERS/$runner" >&2; exit 1; }
    pfx="${XDG_CACHE_HOME:-$HOME/.cache}/async_read_pfx_$runner"
    mkdir -p "$pfx"
    for f in "$GAME_FILE" "$HOME_FILE"; do
        rm -f "$pfx/pfx/drive_c/test_async_read.log"
        echo "=== $runner  $(df --output=fstype "$f" | tail -1)  $f"
        STEAM_COMPAT_DATA_PATH="$pfx" \
        STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
        PROTON_USE_NTSYNC=1 PROTON_NO_NTSYNC=0 WINENTSYNC=1 WINEFSYNC=0 PROTON_USE_FSYNC=0 \
            "$RUNNERS/$runner/proton" run "$SCRIPT_DIR/test_async_read.exe" "Z:$f" "$COUNT" >/dev/null 2>&1
        cat "$pfx/pfx/drive_c/test_async_read.log"
    done
    WINEPREFIX="$pfx/pfx" "$RUNNERS/$runner/files/bin/wineserver" -w
done
