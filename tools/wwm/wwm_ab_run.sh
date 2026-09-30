#!/usr/bin/env bash
# One A/B profiling run of the Where Winds Meet teleport load.
#
#   tools/wwm/wwm_ab_run.sh p10   # GE-Proton10-34   (Lutris entry 17)
#   tools/wwm/wwm_ab_run.sh p11   # GE-Proton11-7    (Lutris entry 18; NOTE: the Lutris copy of 11-7 has a
#                                 hand-replaced ntdll.so/wineserver, see its *.orig files. Symlink Steam's
#                                 untouched copy into runners/wine first for a real stock measurement.)
#   tools/wwm/wwm_ab_run.sh p11c  # GE-Proton11-custom (Lutris entry 18)
#   NOPROFILE=1 tools/wwm/wwm_ab_run.sh p11c   # timed run only: no profiler, no probes
#   PROTONLOG=1 ...                          # also write a Proton log (+seh,+loaddll) to telemetry/
#   PROTONLOG_DEBUG=+microsecs,+iocp ...     # WINEDEBUG channels for that log instead of the default
#   NOLL=1 ...                               # low-latency layer off for this run (as in Lutris entry 11)
#   tools/wwm/wwm_ab_run.sh p11ce11            # GE-Proton11-custom via Lutris entry 11 (layer already off)
#   tools/wwm/wwm_ab_run.sh p11cgfx10          # custom Wine 11 + GE-Proton10-34's DXVK/vkd3d-proton DLLs (entry 11)
#   QPROFILE=1 ...                           # also record vkd3d-proton's queue timeline (VKD3D_QUEUE_PROFILE)
#   BT=tools/wwm/x.bt ...                      # use this bpftrace script instead of the Nt* census
#   EXTRA_ENV="K=V;K2=V2" ...                # extra environment variables for this run only
#   AUTO=1 ...                               # hands-free: posts Log In, then Resume, from inside Wine
#                                            # (tools/wwm/wwm_click.exe: never takes focus, no prompt),
#                                            # marks the load start from the game log and quits
#                                            # AUTO_LOAD_SECS (default 100) later. The load end is
#                                            # taken from the upload trace (QPROFILE=1 is implied).
#   AUTO_LOGIN_SECS=240 ...                  # how long AUTO keeps clicking Log In while the game is busy
#   ALLOW_BUSY=1 ...                         # run even if the machine is busy before launch (BUSY_MAX=25 %)
#   DISPLAY_DRIVER=x11 ...                   # Wine's X11 driver instead of Wayland (the default)
#
# 1. Points the Lutris entry at the runner under test and sets Wine's display driver for the run
#    (Wayland by default, DISPLAY_DRIVER=x11 for X11); the entry is restored on exit.
# 2. Starts proton_profiler.py via pkexec with the matching Nt* census script,
#    or reuses an already-waiting profiler if it uses the same script.
# 3. Launches the game, records your "teleport started" / "world visible"
#    keypresses as wall-clock markers, then quits the game.
# 4. Waits for the profiler to flush and lists the files it produced.
set -euo pipefail

TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$TOOLS/../.." && pwd)"
LUTRIS_GAMES="$HOME/.local/share/lutris/games"
RUNNERS="$HOME/.local/share/lutris/runners/wine"
TELEMETRY="$PROJ/telemetry"

case "${1:-}" in
    p10) game_id=17; yml_glob="where-winds-meet-geproton-10-34-*.yml"; runner="GE-Proton10-34" ;;
    p11) game_id=18; yml_glob="where-winds-meet-geproton-11-*.yml";    runner="GE-Proton11-7-x86_64" ;;
    p11c) game_id=18; yml_glob="where-winds-meet-geproton-11-*.yml";   runner="GE-Proton11-custom" ;;
    p11ce11) game_id=11; yml_glob="where-winds-meet-1[0-9]*.yml";      runner="GE-Proton11-custom" ;;
    # Differential: custom Wine 11 with GE-Proton10-34's dxvk/ and vkd3d-proton/ directories.
    p11cgfx10) game_id=11; yml_glob="where-winds-meet-1[0-9]*.yml";    runner="GE-Proton11-custom-gfx10" ;;
    # Any test runner (e.g. an instrumented copy) through Lutris entry 11.
    p11cx) game_id=11; yml_glob="where-winds-meet-1[0-9]*.yml";        runner="${TEST_RUNNER:?p11cx needs TEST_RUNNER=<runner directory name>}" ;;
    *)   echo "usage: $0 p10|p11|p11c|p11ce11|p11cgfx10|p11cx" >&2; exit 2 ;;
esac
label="$1"
bt="$(realpath -m "${BT:-$TOOLS/nt_census_${runner}.bt}")"
if [ "${NOPROFILE:-0}" != "1" ] && [ ! -f "$bt" ]; then
    echo "error: no bpftrace script $bt (generate it with tools/wwm/gen_nt_census.sh or pass BT=)" >&2
    exit 1
fi
stamp="$(date +%Y%m%d_%H%M%S)"
log="$TELEMETRY/abrun_${label}_${stamp}.log"
markers="$TELEMETRY/abrun_${label}_${stamp}.markers"

die() { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

# A running Lutris caches its list of Proton versions at startup; a runner created after that is
# unknown to it and the game silently starts under Lutris's default Proton instead, which also
# rewrites the prefix's version stamp. Refuse before touching anything.
[ -d "$RUNNERS/$runner" ] || die "no runner $RUNNERS/$runner"
if lutris_pid="$(pgrep -o -f '^python3 /usr/bin/lutris')"; then
    runner_born="$(stat -L -c %W "$RUNNERS/$runner")"
    [ "$runner_born" -gt 0 ] || runner_born="$(stat -L -c %Z "$RUNNERS/$runner")"
    lutris_started=$(( $(date +%s) - $(ps -o etimes= -p "$lutris_pid") ))
    if [ "$runner_born" -ge "$lutris_started" ]; then
        die "Lutris (pid $lutris_pid) started before $runner existed and does not know it; restart Lutris first"
    fi
fi

if [ "${AUTO:-0}" = "1" ]; then
    QPROFILE=1
    # The in-Wine input and window helpers are built here when missing or older than their source.
    command -v x86_64-w64-mingw32-gcc >/dev/null || die "AUTO=1 needs x86_64-w64-mingw32-gcc to build $TOOLS/wwm_click.exe"
    for exe in wwm_click wwm_windows; do
        if [ ! -f "$TOOLS/$exe.exe" ] || [ "$TOOLS/$exe.c" -nt "$TOOLS/$exe.exe" ]; then
            x86_64-w64-mingw32-gcc -O2 -Wall -o "$TOOLS/$exe.exe" "$TOOLS/$exe.c" -luser32
        fi
    done
    command -v tesseract >/dev/null || die "AUTO=1 needs tesseract (screen checks)"
    python3 -c 'import Xlib, PIL' || die "AUTO=1 needs python-xlib and python-pillow (screen checks)"
fi
follower_pid=""
recorder_pid=""
cpu_pid=""
run_invalid=""

# ---------------------------------------------------------------- preconditions
[ -d "$RUNNERS/$runner" ] || die "runner not installed: $RUNNERS/$runner"
if pgrep -x wwm.exe >/dev/null; then
    die "wwm.exe is already running; quit it first"
fi
# A leftover tracer from an earlier run once held ~125 GB of RAM + swap and made
# the next measurement worthless. Refuse to measure on a machine in that state.
if leftover="$(pgrep -a bpftrace)"; then
    printf '%s\n' "$leftover" >&2
    die "bpftrace is still running from an earlier run; stop it with:  pkexec kill -KILL $(pgrep -d ' ' bpftrace)"
fi
mem_avail_mb=$(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))
mem_psi="$(awk '/^some/ {sub("avg10=", "", $2); print $2}' /proc/pressure/memory)"
if [ "$mem_avail_mb" -lt 16384 ] || awk -v p="$mem_psi" 'BEGIN { exit !(p > 1.0) }'; then
    die "system memory is not in a state to measure: ${mem_avail_mb} MB available, memory PSI avg10=${mem_psi}%"
fi
info "memory OK: ${mem_avail_mb} MB available, memory PSI avg10=${mem_psi}%"

shopt -s nullglob
ymls=( $LUTRIS_GAMES/$yml_glob )
shopt -u nullglob
[ "${#ymls[@]}" -eq 1 ] || die "expected exactly one Lutris config matching $yml_glob, found ${#ymls[@]}"
yml="${ymls[0]}"
# A run killed before it restored the config (e.g. its terminal tab closed) leaves its temporary
# lines behind; a stale VKD3D_QUEUE_PROFILE would silently send this run's trace to the old file.
if grep -qE '^    (VKD3D_QUEUE_PROFILE|WINE_NO_SYSTEM_BIOS_DATE|WINE_FEATURESET|WINE_HIDE_CPU_FEATURES):' "$yml"; then
    grep -nE '^    (VKD3D_QUEUE_PROFILE|WINE_NO_SYSTEM_BIOS_DATE|WINE_FEATURESET|WINE_HIDE_CPU_FEATURES):' "$yml" >&2
    die "$(basename "$yml") still holds temporary lines from an interrupted run (above); remove them first"
fi

if [ ! -f "$bt" ]; then
    info "generating census script for $runner"
    "$TOOLS/gen_nt_census.sh" "$RUNNERS/$runner" "$bt"
fi
mkdir -p "$TELEMETRY"

# ------------------------------------------------------- runner swap + restore
backup=""
restore_config() {
    if [ -n "$backup" ] && [ -f "$backup" ]; then
        mv -f "$backup" "$yml"
        info "restored $(basename "$yml")"
    fi
}
stop_follower() {
    if [ -n "$follower_pid" ] && kill -0 "$follower_pid" 2>/dev/null; then
        kill "$follower_pid"
        wait "$follower_pid" 2>/dev/null || [ $? -eq 143 ]
    fi
    follower_pid=""
}
stop_recorder() {
    if [ -n "$recorder_pid" ] && kill -0 "$recorder_pid" 2>/dev/null; then
        kill "$recorder_pid"
        wait "$recorder_pid" 2>/dev/null || [ $? -eq 143 ]
    fi
    recorder_pid=""
}
stop_cpu() {
    if [ -n "$cpu_pid" ] && kill -0 "$cpu_pid" 2>/dev/null; then
        kill "$cpu_pid"
        wait "$cpu_pid" 2>/dev/null || [ $? -eq 143 ]
    fi
    cpu_pid=""
}
# Per-thread state, kernel wait channel and name of the game and wineserver, from /proc only
# (no ptrace: nothing attaches to the game).
snapshot_threads() {
    local out="$1" pid t st wc nm
    {
        echo "# $(date +%s.%N) tid state wchan name"
        for pid in $(pgrep -x wwm.exe) $(pgrep -x wineserver); do
            echo "## pid $pid"
            for t in "/proc/$pid/task/"*; do
                # A thread can exit between listing and reading: skip it rather than fail.
                if st="$(awk '{ print $3 }' "$t/stat" 2>/dev/null)" && wc="$(cat "$t/wchan" 2>/dev/null)" \
                        && nm="$(cat "$t/comm" 2>/dev/null)"; then
                    echo "${t##*/} $st $wc $nm"
                fi
            done
        done
    } >"$out"
    info "thread snapshot: $out"
}

on_exit() {
    stop_follower
    stop_recorder
    stop_cpu
    # An AUTO run interrupted after the load started (closed terminal tab, Ctrl-C): record the
    # quit time the analysis anchors on and quit the game, so the run stays usable.
    if [ "${AUTO:-0}" = "1" ] && [ -f "${markers:-}" ] && grep -q '^teleport_start ' "$markers" \
            && ! grep -q '^quit_at ' "$markers"; then
        echo "quit_at $(date +%s.%N)" >>"$markers"
        if pgrep -x wwm.exe >/dev/null; then pkill -x wwm.exe; fi
    elif [ "${AUTO:-0}" = "1" ] && [ -n "${game_launched:-}" ] && pgrep -x wwm.exe >/dev/null; then
        # A hands-free run that stopped before the load (login not registered, a check failed):
        # record where the game's threads wait (a stalled login is otherwise undiagnosable), then
        # close the game it launched instead of leaving it at the login screen.
        snapshot_threads "${markers%.markers}.threads.txt"
        pkill -x wwm.exe
    fi
    restore_config
}
trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT TERM

backup_config_once() {
    if [ -z "$backup" ]; then
        backup="$yml.bak-abrun-$stamp"
        cp -p "$yml" "$backup"
    fi
}

# Timings are only comparable on an otherwise idle machine: a background build using most cores
# slows the game to a few FPS and even makes the login dialog miss clicks. Measure overall CPU
# use for 3 s before touching anything; ALLOW_BUSY=1 skips the check.
if [ "${ALLOW_BUSY:-0}" != "1" ]; then
    read -r _ u1 n1 s1 i1 w1 q1 sq1 st1 _ </proc/stat
    sleep 3
    read -r _ u2 n2 s2 i2 w2 q2 sq2 st2 _ </proc/stat
    busy_total=$(( (u2 + n2 + s2 + i2 + w2 + q2 + sq2 + st2) - (u1 + n1 + s1 + i1 + w1 + q1 + sq1 + st1) ))
    busy_pct=$(( 100 * (busy_total - (i2 - i1) - (w2 - w1)) / busy_total ))
    if [ "$busy_pct" -gt "${BUSY_MAX:-25}" ]; then
        top -b -n1 -o %CPU | sed -n '7,12p' >&2
        die "the machine is ${busy_pct}% busy before launch (limit ${BUSY_MAX:-25}%); the run would not be comparable. Top processes above; ALLOW_BUSY=1 to run anyway"
    fi
    info "CPU ${busy_pct}% busy before launch"
fi

current="$(sed -n 's/^  version: //p' "$yml")"
[ -n "$current" ] || die "no 'version:' line in $yml"
if [ "$current" != "$runner" ]; then
    backup_config_once
    sed -i "s/^  version: .*/  version: $runner/" "$yml"
    info "Lutris entry $game_id: $current -> $runner (temporary)"
else
    info "Lutris entry $game_id already uses $runner"
fi

# Display driver for this run, whatever the entry says for normal play (restored on exit like the
# runner swap): Wayland by default, as the game is played; DISPLAY_DRIVER=x11 for Wine's X11 driver.
# Screen checks read the game window either way (wwm_grab.py): through XWayland's pixmap on X11,
# through KWin's ScreenShot2 interface on Wayland (tools/wwm/setup_wayland_capture.sh, once).
# On the X11 driver, logins intermittently stall with plasmashell and wineserver saturated
# (seen 2026-09-29/30), so X11 runs are for comparisons with older X11 measurements.
display_driver="${DISPLAY_DRIVER:-wayland}"
case "$display_driver" in
    wayland) want_wayland=1 ;;
    x11) want_wayland=0 ;;
    *) die "DISPLAY_DRIVER must be wayland or x11, not '$display_driver'" ;;
esac
if [ "$want_wayland" = 1 ] && ! "$TOOLS/setup_wayland_capture.sh" --check; then
    die "screen checks on Wayland need the KWin capture helper: run tools/wwm/setup_wayland_capture.sh once"
fi
if ! grep -qE "^    PROTON_ENABLE_WAYLAND: ['\"]?${want_wayland}['\"]?$" "$yml"; then
    backup_config_once
    sed -i "/^    PROTON_ENABLE_WAYLAND: /d" "$yml"
    grep -q '^    WINEDEBUG: ' "$yml" || die "no WINEDEBUG entry under system.env in $yml"
    sed -i "/^    WINEDEBUG: /a\\    PROTON_ENABLE_WAYLAND: '${want_wayland}'" "$yml"
    info "display: $display_driver for this run (temporary config change)"
else
    info "display: $display_driver (the entry already uses it)"
fi
echo "display_driver $display_driver" >>"$markers"

# PROTONLOG=1: Proton log with exception/loader channels, written into telemetry/.
# The Lutris config sets WINEDEBUG=-all and Lutris runs as a persistent app, so the
# switch has to go into the game config (restored on exit like the runner swap).
protonlog_dir="$TELEMETRY/protonlog_${label}_${stamp}"
if [ "${PROTONLOG:-0}" = "1" ]; then
    grep -q '^    WINEDEBUG: ' "$yml" || die "no WINEDEBUG entry under system.env in $yml"
    backup_config_once
    mkdir -p "$protonlog_dir"
    sed -i -e "s|^    WINEDEBUG: .*|    WINEDEBUG: ${PROTONLOG_DEBUG:-+timestamp,+pid,+tid,+seh,+loaddll}|" \
           -e "/^    WINEDEBUG: /a\\    PROTON_LOG: '1'\\n    PROTON_LOG_DIR: $protonlog_dir" "$yml"
    # Lutris's "Output debugging info" (wine: show_debug, default -all) overwrites WINEDEBUG
    # unless it is "inherit".
    grep -q '^wine:$' "$yml" || die "no wine: section in $yml"
    if grep -q '^  show_debug: ' "$yml"; then
        sed -i "s|^  show_debug: .*|  show_debug: inherit|" "$yml"
    else
        sed -i "/^wine:$/a\  show_debug: inherit" "$yml"
    fi
    info "PROTONLOG=1: Proton log -> $protonlog_dir (temporary config change)"
fi

# NOLL=1: turn the KORTHOS low-latency layer, its VK_NV_low_latency2 mode and vkd3d-proton's
# forced low-latency calls off for this run (the baseline for layer on/off comparisons).
if [ "${NOLL:-0}" = "1" ]; then
    backup_config_once
    for var in LOW_LATENCY_LAYER LOW_LATENCY_LAYER_REFLEX DISABLE_LOW_LATENCY_LAYER VKD3D_FORCE_LOW_LATENCY; do
        sed -i "/^    ${var}: /d" "$yml"
    done
    grep -q '^    WINEDEBUG: ' "$yml" || die "no WINEDEBUG entry under system.env in $yml"
    sed -i "/^    WINEDEBUG: /a\\    LOW_LATENCY_LAYER: '0'\\n    LOW_LATENCY_LAYER_REFLEX: '0'\\n    DISABLE_LOW_LATENCY_LAYER: '1'\\n    VKD3D_FORCE_LOW_LATENCY: '0'" "$yml"
    info "NOLL=1: low-latency layer off for this run (temporary config change)"
fi

# EXTRA_ENV="NAME=value;NAME2=value2": extra environment for this run only (e.g.
# EXTRA_ENV="WINE_HIDE_CPU_FEATURES=avx512f,erms"), inserted next to WINEDEBUG.
if [ -n "${EXTRA_ENV:-}" ]; then
    grep -q '^    WINEDEBUG: ' "$yml" || die "no WINEDEBUG entry under system.env in $yml"
    IFS=';' read -r -a extra_pairs <<< "$EXTRA_ENV"
    for pair in "${extra_pairs[@]}"; do
        name="${pair%%=*}"; value="${pair#*=}"
        [[ "$name" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || die "bad EXTRA_ENV entry: $pair"
        sed -i "/^    WINEDEBUG: /a\    $name: '$value'" "$yml"
        info "EXTRA_ENV: $name=$value (temporary config change)"
    done
fi

# QPROFILE=1: vkd3d-proton's own queue timeline (Chrome trace JSON): every submission,
# fence signal, blocking wait, present and PSO compile with timestamps.
qprofile_json="$TELEMETRY/vkd3d_queue_${label}_${stamp}.json"
if [ "${QPROFILE:-0}" = "1" ]; then
    grep -q '^    WINEDEBUG: ' "$yml" || die "no WINEDEBUG entry under system.env in $yml"
    backup_config_once
    sed -i "/^    WINEDEBUG: /a\\    VKD3D_QUEUE_PROFILE: $qprofile_json" "$yml"
    info "QPROFILE=1: vkd3d queue timeline -> $qprofile_json (temporary config change)"
fi

# --------------------------------------------------------------------- profiler
own_profiler_pid=""
if [ "${NOPROFILE:-0}" = "1" ]; then
    # Timed run only: no profiler and no uprobes patched into the game's memory.
    if existing="$(pgrep -af 'proton_profiler.py record')"; then
        printf '%s\n' "$existing" >&2
        die "NOPROFILE=1 but a profiler is waiting and would attach to the game; stop it with:  pkexec pkill -f 'proton_profiler.py record'"
    fi
    info "NOPROFILE=1: no profiler, no probes (timing from your keypresses only)"
elif existing="$(pgrep -af 'proton_profiler.py record')"; then
    if printf '%s\n' "$existing" | grep -qF "$(basename "$bt")"; then
        info "reusing the profiler that is already waiting with $(basename "$bt")"
    else
        printf '%s\n' "$existing" >&2
        die "a profiler with a different script is already running; stop it with:  pkexec pkill -f 'proton_profiler.py record'"
    fi
else
    info "starting profiler (enter your password in the pkexec dialog)"
    pkexec python3 -u "$PROJ/proton_profiler.py" record --name wwm.exe --wait --exit-on-detach \
        --output-dir "$TELEMETRY" --bpftrace-script "$bt" >"$log" 2>&1 &
    own_profiler_pid=$!
    for _ in $(seq 1 120); do
        if grep -q "Standing by" "$log"; then break; fi
        if [ ! -d "/proc/$own_profiler_pid" ]; then
            cat "$log" >&2
            die "profiler exited before it was ready (password dialog dismissed?)"
        fi
        sleep 1
    done
    grep -q "Standing by" "$log" || die "profiler did not become ready within 120 s; see $log"
    info "profiler waiting for wwm.exe (log: $log)"
fi

# ------------------------------------------------------------------------ game
start_ref="$TELEMETRY/.abrun_${label}_${stamp}.start"
touch "$start_ref"
gamelog="$TELEMETRY/abrun_${label}_${stamp}.gamelog"
if [ "${AUTO:-0}" = "1" ]; then
    python3 -u "$TOOLS/wwm_gamelog_follow.py" > "$gamelog" &
    follower_pid=$!
fi
info "launching Lutris entry $game_id ($runner)"
lutris "lutris:rungameid/$game_id" >"$TELEMETRY/abrun_${label}_${stamp}.lutris.log" 2>&1 &

for _ in $(seq 1 300); do
    if pgrep -x wwm.exe >/dev/null; then break; fi
    sleep 1
done
pgrep -x wwm.exe >/dev/null || die "wwm.exe did not start within 300 s"
echo "game_started $(date +%s.%N)" >"$markers"
echo "mem_available_mb_start $mem_avail_mb" >>"$markers"
echo "mem_psi_avg10_start $mem_psi" >>"$markers"
game_exe="$(readlink "/proc/$(pgrep -xo wwm.exe)/exe")"
case "$game_exe" in
    "$RUNNERS/$runner/"*) info "wwm.exe is running under $runner"; game_launched=1 ;;
    *)  pkill -x wwm.exe
        die "wwm.exe started under the wrong runner ($game_exe); game stopped" ;;
esac
# AUTO=1 screen record: the game window's own contents (read-only; no focus change) once a
# second, OCR'd for dialogs and the world HUD. Low priority, one core for OCR.
framedir="$TELEMETRY/abrun_${label}_${stamp}.frames"
# CPU time and context switches of the game and of its wineserver, once a second from /proc.
cputsv="$TELEMETRY/abrun_${label}_${stamp}.cpu.tsv"
nice -n 10 python3 -u "$TOOLS/wwm_cpu.py" record "$cputsv" --pid "$(pgrep -xo wwm.exe)" >"$cputsv.log" 2>&1 &
cpu_pid=$!
if [ "${AUTO:-0}" = "1" ]; then
    nice -n 10 python3 -u "$TOOLS/wwm_screen.py" record "$framedir" >"$framedir.log" 2>&1 &
    recorder_pid=$!
fi

if [ -n "$own_profiler_pid" ] && grep -q "BPFTRACE ERROR" "$log"; then
    grep "BPFTRACE ERROR" "$log" >&2
    die "bpftrace failed to attach; the run would have no census data"
fi

# Wait for Enter, but give up (return 1) if the game exits first.
wait_enter() {
    printf '%s' "$1"
    while :; do
        if read -r -t 1 _; then return 0; fi
        if ! pgrep -x wwm.exe >/dev/null; then
            echo
            return 1
        fi
    done
}

# Wait until a game-log line matching $1 (extended regex) appears after line $2, for at most
# $3 seconds; print its line number. Fails if the game exits or the time runs out.
wait_log() {
    local pattern="$1" after="$2" limit="$3" n
    for _ in $(seq 1 $((limit * 10))); do
        n="$(awk -v a="$after" -v p="$pattern" 'NR > a && $0 ~ p { print NR; exit }' "$gamelog")"
        if [ -n "$n" ]; then echo "$n"; return 0; fi
        pgrep -x wwm.exe >/dev/null || return 1
        sleep 0.1
    done
    return 1
}
log_time() { sed -n "${1}p" "$gamelog" | cut -d' ' -f1; }

echo
# Input for AUTO=1 is posted from inside Wine (tools/wwm/wwm_click.exe, same runner and prefix as
# the game): it never activates a window, moves the pointer or goes through the desktop, so the
# user's focus is untouched and KDE shows no input-permission prompt.
wine_click() {
    WINEPREFIX="$HOME/.wine" WINEDEBUG=-all "$RUNNERS/$runner/files/bin/wine" "$TOOLS/wwm_click.exe" "$@" >&2
}
login_dialog_up() {
    WINEPREFIX="$HOME/.wine" WINEDEBUG=-all "$RUNNERS/$runner/files/bin/wine" "$TOOLS/wwm_windows.exe" 2>/dev/null \
        | grep -q MPAY_SWITCH_ACCOUNT
}
# "Log In" in the NetEase dialog. The game ignores it while it is busy behind the dialog:
# compiling and saving its pipeline cache after a runner change stops it for well over a minute.
# So click every ~10 s until the game logs the login, the dialog closes (a late click was taken:
# give the login time to show up, never click again), or AUTO_LOGIN_SECS (default 240) pass.
# Prints the game log line number of the login.
login_until() {
    local deadline n attempt=0
    deadline=$(( $(date +%s) + ${AUTO_LOGIN_SECS:-240} ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        attempt=$((attempt + 1))
        if ! wine_click --class MPAY_SWITCH_ACCOUNT --post 0.5000 0.6660; then
            info "AUTO: posting Log In failed (attempt $attempt)" >&2
        fi
        if n="$(wait_log 'on_redis_get_account_back' 0 10)"; then echo "$n"; return 0; fi
        pgrep -x wwm.exe >/dev/null || return 1
        if ! login_dialog_up; then
            if n="$(wait_log 'on_redis_get_account_back' 0 30)"; then echo "$n"; return 0; fi
            info "AUTO: the login dialog closed but the game logged no login" >&2
            return 1
        fi
        info "AUTO: Log In not registered yet (attempt $attempt)" >&2
    done
    return 1
}
# Latest state the screen recorder saw (world, loading, already_online, disconnect, ...).
screen_state() { if [ -f "$framedir/states.tsv" ]; then tail -n 1 "$framedir/states.tsv" | cut -f2; fi; }
# Did the recorder see state $1 at or after epoch $2?
screen_seen() { [ -f "$framedir/states.tsv" ] && awk -F'\t' -v s="$1" -v t="$2" '$1 >= t && $2 == s { f = 1 } END { exit !f }' "$framedir/states.tsv"; }
# Mark the run unusable (kicked to the login screen, disconnected, ...); it still finishes and
# keeps its data, but exits with status 3 so a chained pair stops.
invalidate() {
    if [ -z "$run_invalid" ]; then
        run_invalid="$1"
        echo "run_invalid $(date +%s.%N) $1" >>"$markers"
        echo "warning: RUN INVALID: $1" >&2
    fi
}
# Post a click (args before the pattern) until the game logs $pattern after line $after.
click_until() {
    local after="$1" pattern="$2" what="$3" n attempt
    shift 3
    for attempt in 1 2 3; do
        wine_click "$@"
        if n="$(wait_log "$pattern" "$after" 10)"; then echo "$n"; return 0; fi
        info "AUTO: $what not registered (attempt $attempt)" >&2
    done
    return 1
}

# Click a labelled button in the game window until the game logs $pattern after line $after.
# Attempts 1 and 3 click where the label is on screen (wwm_screen.py locate: holds at any
# resolution, aspect ratio or UI scale); attempt 2, and any attempt whose label is not on screen,
# clicks the fixed fraction measured at 2560x1440. The position that registered goes into the
# markers as "<key>_click <fx> <fy> <source>".
click_label_until() {
    local after="$1" pattern="$2" what="$3" key="$4" label="$5" pick="$6" fx="$7" fy="$8"
    local n attempt pos x y conf how src
    for attempt in 1 2 3; do
        if [ "$attempt" != 2 ] && pos="$(python3 "$TOOLS/wwm_screen.py" locate "$label" --pick "$pick")"; then
            read -r x y conf how <<<"$pos"
            src="screen:ocr-conf-$conf"
        else
            x="$fx"; y="$fy"; src="fixed-2560x1440"
        fi
        info "AUTO: $what at ($x, $y) [$src]" >&2
        wine_click --fake-active --post "$x" "$y"
        if n="$(wait_log "$pattern" "$after" 10)"; then
            echo "${key}_click $x $y $src" >>"$markers"
            echo "$n"
            return 0
        fi
        info "AUTO: $what not registered (attempt $attempt)" >&2
    done
    return 1
}

# LOW_LATENCY_LAYER=1 must load the layer shipped in the runner under test. Other manifests with
# the same layer name and switch can exist (/usr/share, ~/.local/share/vulkan), and the runner's
# manifest names the library without a path, so check what the game actually mapped. Recorded as
# "low_latency_layer <path>|none"; a wrong or duplicate copy stops the run. Called while the game
# renders the world, so its Vulkan device exists.
check_low_latency_layer() {
    local pid p env want maps libs n rdir
    pid="$(pgrep -xo wwm.exe)" || { info "low-latency layer check skipped: the game is not running"; return 0; }
    maps="$(cat "/proc/$pid/maps")"
    # Wine sets the Windows environment with setenv after exec, so the game's /proc environ
    # lacks it: take the switch from the nearest ancestor that has it (umu/pressure-vessel or
    # Proton). Read into variables first: grep -q closing a pipe early would fail under pipefail.
    want=0
    p="$pid"
    while [ "$p" -gt 1 ]; do
        env="$(tr '\0' '\n' <"/proc/$p/environ")"
        if grep -q '^LOW_LATENCY_LAYER=' <<<"$env"; then
            if grep -qx 'LOW_LATENCY_LAYER=1' <<<"$env" && ! grep -qx 'DISABLE_LOW_LATENCY_LAYER=1' <<<"$env"; then
                want=1
            fi
            break
        fi
        p="$(awk '{ print $4 }' "/proc/$p/stat")"
    done
    libs="$(awk '$6 ~ /libVkLayer_KORTHOS_LowLatency\.so$/ { print $6 }' <<<"$maps" | sort -u)"
    n=0
    if [ -n "$libs" ]; then n="$(wc -l <<<"$libs")"; fi
    echo "low_latency_layer $(tr '\n' ' ' <<<"${libs:-none}")" >>"$markers"
    rdir="$(realpath "$RUNNERS/$runner")"
    if [ "$want" = 1 ]; then
        [ "$n" = 1 ] || { pkill -x wwm.exe; die "LOW_LATENCY_LAYER=1 but $n copies of the layer are loaded: ${libs:-none} (game stopped)"; }
        case "$libs" in
            "$rdir/"*) info "low-latency layer loaded from the runner: $libs" ;;
            *) pkill -x wwm.exe; die "LOW_LATENCY_LAYER=1 loaded $libs, not the copy in $rdir (game stopped)" ;;
        esac
    elif [ "$n" != 0 ]; then
        pkill -x wwm.exe; die "the low-latency layer is loaded without LOW_LATENCY_LAYER=1: $libs (game stopped)"
    fi
}

echo
if [ "${AUTO:-0}" = "1" ]; then
    info "AUTO: waiting for the NetEase login dialog"
    for _ in $(seq 1 150); do
        if login_dialog_up; then break; fi
        pgrep -x wwm.exe >/dev/null || die "the game exited before the login dialog appeared"
        sleep 2
    done
    login_dialog_up || die "no login dialog (MPAY_SWITCH_ACCOUNT) within 300 s"
    # A window exists now, so the display driver is loaded: it must be the one asked for.
    game_maps="/proc/$(pgrep -xo wwm.exe)/maps"
    if [ "$want_wayland" = 1 ]; then
        driver_ok=0; if grep -q 'winewayland' "$game_maps"; then driver_ok=1; fi
    else
        driver_ok=1; if grep -q 'winewayland' "$game_maps" || ! grep -q 'winex11' "$game_maps"; then driver_ok=0; fi
    fi
    if [ "$driver_ok" != 1 ]; then
        pkill -x wwm.exe
        die "the game is not on Wine's $display_driver driver (game stopped)"
    fi
    sleep "${AUTO_CLICK_DELAY:-2}"
    # "Log In" in the dialog (login_until): posted to the dialog window, which handles it without focus. The
    # dialog is a separate fixed-size window (360x380), so its fraction does not depend on the
    # game's resolution.
    ln="$(login_until)" || die "the game did not register the Log In click within ${AUTO_LOGIN_SECS:-240} s (see $gamelog)"
    echo "login_clicked $(log_time "$ln")" >>"$markers"
    sleep "${AUTO_CLICK_DELAY:-4}"
    # "Resume" (where "Start" was): the game ignores input while it thinks it is inactive, so post
    # WM_ACTIVATEAPP/WM_ACTIVATE/WM_SETFOCUS to its window first (desktop focus is not touched).
    cl="$(click_label_until "$ln" 'on_click_game_start' 'Resume' resume Resume best 0.8648 0.4063)" || \
        die "the game did not register the Resume click (see $gamelog)"
    echo "resume_clicked $(log_time "$cl")" >>"$markers"
    # Load start: on_become_player follows the Resume click within a second and is logged
    # reliably (on_teleport_in, logged in the same second, is sometimes missing).
    # "Account already online. Continue login?" (the previous session was not logged out, e.g. the
    # game of the previous run was killed): the log stops at "try_to_relay_other" instead of
    # reaching on_become_player. Click its "Space Continue" hint (Space itself is read as raw input
    # and cannot be posted).
    resume_epoch="$(date +%s)"
    if ! tp="$(wait_log 'on_become_player' "$((cl - 1))" 8)"; then
        if wait_log 'try_to_relay_other' "$((cl - 1))" 1 >/dev/null || screen_seen already_online "$resume_epoch"; then
            info "AUTO: 'account already online' dialog; clicking Continue"
            # "Continue" also appears in the message above the hint: take the lowest occurrence.
            tp="$(click_label_until "$cl" 'on_become_player' 'Continue' continue Continue lowest 0.5320 0.5850)" || \
                die "the game did not register the Continue click (see $gamelog)"
        else
            tp="$(wait_log 'on_become_player' "$((cl - 1))" 52)" || die "no on_become_player within 60 s of Resume (see $gamelog)"
        fi
    fi
    echo "teleport_start $(log_time "$tp")" >>"$markers"
    echo "teleport_source gamelog:on_become_player" >>"$markers"
    info "AUTO: load started; letting it run ${AUTO_LOAD_SECS:-100} s"
    tp_epoch="$(log_time "$tp")"
    for _ in $(seq 1 "${AUTO_LOAD_SECS:-100}"); do
        if ! pgrep -x wwm.exe >/dev/null; then
            invalidate "the game exited during the load window"
            break
        fi
        # Back at the login screen (server kick, relay failure): the log shows the login window
        # loading again, or the NetEase dialog reappears.
        if awk -v a="$tp" 'NR > a && /Start-Load-LoginWindow|enter LoginWindow/ { f = 1 } END { exit !f }' "$gamelog"; then
            invalidate "the game went back to the login window (game log)"
            break
        fi
        if screen_seen disconnect "$tp_epoch"; then
            invalidate "disconnect dialog on screen (see $framedir)"
            break
        fi
        sleep 1
    done
    check_low_latency_layer
    echo "quit_at $(date +%s.%N)" >>"$markers"
elif wait_enter "Log in. Press Enter at the moment you START the teleport... "; then
    echo "teleport_start $(date +%s.%N)" >>"$markers"
    if wait_enter "Press Enter as soon as the loading screen is gone and the world renders... "; then
        echo "world_visible $(date +%s.%N)" >>"$markers"
        awk '/teleport_start/{a=$2} /world_visible/{b=$2} END{printf "==> load took %.1f s (by your keypresses)\n", b-a}' "$markers"
    fi
fi

# ------------------------------------------------------------------------ quit
stop_follower
stop_recorder
stop_cpu
if pgrep -x wwm.exe >/dev/null; then
    info "quitting the game"
    pkill -x wwm.exe
    for _ in $(seq 1 30); do
        if ! pgrep -x wwm.exe >/dev/null; then break; fi
        sleep 1
    done
    if pgrep -x wwm.exe >/dev/null; then
        info "wwm.exe still alive after 30 s, sending SIGKILL"
        pkill -KILL -x wwm.exe
    fi
else
    echo "game_exited_early $(date +%s.%N)" >>"$markers"
    info "game already exited before both keypresses; the load is still bracketed by the loading-screen threads"
fi

if [ "${NOPROFILE:-0}" != "1" ]; then
    info "waiting for the profiler to flush"
    if [ -n "$own_profiler_pid" ]; then
        wait "$own_profiler_pid"
    else
        while pgrep -f 'proton_profiler.py record' >/dev/null; do sleep 1; done
    fi
fi

if leftover="$(pgrep -a bpftrace)"; then
    printf '%s\n' "$leftover" >&2
    echo "warning: bpftrace outlived the profiler; stop it before the next run with:  pkexec kill -KILL $(pgrep -d ' ' bpftrace)" >&2
fi
echo "mem_available_mb_end $(( $(awk '/^MemAvailable:/ {print $2}' /proc/meminfo) / 1024 ))" >>"$markers"
echo "mem_psi_avg10_end $(awk '/^some/ {sub("avg10=", "", $2); print $2}' /proc/pressure/memory)" >>"$markers"

if [ "${AUTO:-0}" = "1" ] && [ -f "$framedir/states.tsv" ] && grep -q '^teleport_start ' "$markers"; then
    scan="$(python3 "$TOOLS/wwm_screen.py" scan "$framedir" "$(awk '$1 == "teleport_start" { print $2 }' "$markers")")"
    printf '%s\n' "$scan" | sed 's/^/    screen: /'
    wv="$(printf '%s\n' "$scan" | awk '$1 == "world_visible" && $2 != "none" { print $2 }')"
    if [ -n "$wv" ]; then
        echo "world_visible $wv" >>"$markers"
        echo "world_visible_source screen:loading-bar-gone+hud" >>"$markers"
        awk '/^teleport_start /{a=$2} /^world_visible /{b=$2} END{printf "==> world on screen (loading bar gone, HUD confirmed) %.1f s after the load started\n", b-a}' "$markers"
    else
        info "the world HUD never appeared on screen during the load window"
    fi
    if printf '%s\n' "$scan" | grep -q '^dialog disconnect '; then invalidate "disconnect dialog on screen (see $framedir)"; fi
fi

echo
info "files from this run:"
find "$TELEMETRY" -maxdepth 1 -newer "$start_ref" -type f -name 'wwm.exe_*' -printf '    %p\n'
echo "    $markers"
if [ -d "$framedir" ]; then echo "    $framedir/ ($(find "$framedir" -name '*.jpg' | wc -l) frames)"; fi
if [ -f "$cputsv" ]; then echo "    $cputsv"; fi
rm -f "$start_ref"
if [ -n "$run_invalid" ]; then
    echo "error: RUN INVALID: $run_invalid" >&2
    exit 3
fi
