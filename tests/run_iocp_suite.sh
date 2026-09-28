#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

RUNNER_PATH="${RUNNER_PATH:-$HOME/.local/share/lutris/runners/wine/GE-Proton11-custom}"
PFX_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/iocp_test_pfx"

export PATH="$RUNNER_PATH/files/bin:$PATH"
export WINESERVER="${WINESERVER:-$RUNNER_PATH/files/bin/wineserver}"
export WINELOADER="${WINELOADER:-$RUNNER_PATH/files/bin/wine}"

echo "============================================================"
echo "   Building and Running NTDLL In-Process IOCP Test Suite"
echo "============================================================"

# Compile centralized test binary with MinGW
x86_64-w64-mingw32-gcc -O2 -Wall "$SCRIPT_DIR/test_iocp_suite.c" -o "$SCRIPT_DIR/test_iocp_suite.exe"

# Execute under Proton runner
cleanup() {
    local pfx="$PFX_DIR/pfx"
    local ws_bin="$RUNNER_PATH/files/bin/wineserver"
    if [ -d "$pfx" ] && [ -x "$ws_bin" ]; then
        if WINEPREFIX="$pfx" "$ws_bin" -k 2>/dev/null; then
            WINEPREFIX="$pfx" "$ws_bin" -w 2>/dev/null
        fi
    fi
}
trap cleanup EXIT

mkdir -p "$PFX_DIR"
rm -f "$PFX_DIR/pfx/drive_c/test_iocp_suite.log"

PROT_EXIT=0
if ! STEAM_COMPAT_DATA_PATH="$PFX_DIR" \
     STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam" \
     "$RUNNER_PATH/proton" run "$SCRIPT_DIR/test_iocp_suite.exe" "$@"; then
    PROT_EXIT=$?
fi

if [ -f "$PFX_DIR/pfx/drive_c/test_iocp_suite.log" ]; then
    cat "$PFX_DIR/pfx/drive_c/test_iocp_suite.log"
fi

exit $PROT_EXIT
