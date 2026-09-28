#!/usr/bin/env bash
# Build tests/test_sysid.c (system identity as seen by VM/emulator detection) and run it under runners.
# each in its own throwaway prefix so the game prefix is never touched.
#
#   tests/run_waitable_timer.sh GE-Proton10-34 GE-Proton11-custom
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNNERS="$HOME/.local/share/lutris/runners/wine"
[ "$#" -gt 0 ] || { echo "usage: $0 <runner-name>..." >&2; exit 2; }

x86_64-w64-mingw32-gcc -O2 -Wall "$SCRIPT_DIR/test_sysid.c" -o "$SCRIPT_DIR/test_sysid.exe"

for runner in "$@"; do
    [ -x "$RUNNERS/$runner/proton" ] || { echo "error: no proton script in $RUNNERS/$runner" >&2; exit 1; }
    pfx="${XDG_CACHE_HOME:-$HOME/.cache}/waitable_timer_pfx_$runner"
    mkdir -p "$pfx"
    rm -f "$pfx/pfx/drive_c/test_sysid.log"

    echo "=== $runner"
    STEAM_COMPAT_DATA_PATH="$pfx" \
    STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
    PROTON_USE_NTSYNC=1 PROTON_NO_NTSYNC=0 WINENTSYNC=1 WINEFSYNC=0 PROTON_USE_FSYNC=0 \
        "$RUNNERS/$runner/proton" run "$SCRIPT_DIR/test_sysid.exe" >/dev/null
    cat "$pfx/pfx/drive_c/test_sysid.log"

    WINEPREFIX="$pfx/pfx" "$RUNNERS/$runner/files/bin/wineserver" -w
done
