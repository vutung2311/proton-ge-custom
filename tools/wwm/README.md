# Where Winds Meet A/B harness

Hands-free teleport-load and in-world measurements of Where Winds Meet under a given runner,
used for the numbers in `docs/research_notes.md` (§34-§37). Nothing here activates or raises a
window or moves the pointer: input is posted from inside Wine (`wwm_click.c`) and the screen is
read from the window's own contents (`wwm_grab.py`).

| Tool | Purpose |
| :--- | :--- |
| `wwm_ab_run.sh <mode>` | One run: points the Lutris entry at the runner (restored on exit), launches the game, logs in, teleports, quits. `AUTO=1` hands-free, `NOPROFILE=1` no bpftrace, `PROTONLOG=1` Proton log (`PROTONLOG_DEBUG` picks the channels), `EXTRA_ENV`, `BT`. See the header for all modes and switches. |
| `wwm_screen.py` | Screen recorder and classifier (loading bar, world HUD, dialogs); `scan` derives `world_visible`; `locate` finds a button by its text, so the harness clicks Resume and Continue where they are at any resolution, aspect ratio or UI scale (fixed 2560x1440 fractions are the logged second attempt). |
| `wwm_grab.py` | Read-only capture of the game window's own contents: XComposite on Wine's X11 driver, KWin's ScreenShot2 (through `wwm_kwin_grab`) on its Wayland driver. |
| `wwm_kwin_grab.c`, `setup_wayland_capture.sh` | The KWin capture helper and its one-time setup: builds it and authorizes that one executable for KWin's screenshot interface (`--check`, `--remove`). |
| `wwm_cpu.py` | Per-second CPU and context switches of the game and its wineserver; `report` compares runs. |
| `frametimes.py` | In-world frame-time statistics from vkd3d-proton's queue trace. |
| `latency.py` | In-world queue latency (Present call to GPU completion of that frame) and low-latency sleeps from vkd3d-proton's queue trace. |
| `vkd3d_qprof.py` | Upload/queue timeline of a run from the same trace. |
| `wwm_gamelog_follow.py` | Follows the game log for the load-start marker. |
| `gen_nt_census.sh`, `gen_iocp_purity.sh`, `iocp_purity_report.py` | bpftrace script generators for the profiler and the completion-port census. |
| `wwm_click.c`, `wwm_windows.c` | In-Wine input and window listing; `wwm_ab_run.sh` builds the `.exe` files when needed. |
| `wwm_wayland_lock_trace.py [pid]` | Who holds the Wayland driver's window data lock in a running or hung game, read from `/proc/<pid>/mem` without stopping it; lists the lock events since the outermost acquisition still in effect, with the calling functions. Needs Wine patch 0011. |

Every run launches the game on Wine's Wayland driver, as it is played, whatever the Lutris entry
says (restored on exit); `DISPLAY_DRIVER=x11` uses the X11 driver instead. The harness checks the
loaded driver and stops a run that is not on the requested one. Screen checks work on both; on
Wayland they need `tools/wwm/setup_wayland_capture.sh` run once (the harness checks and says so).
On the X11 driver, logins intermittently stall with plasmashell and wineserver saturated, so X11 is
for comparisons with older X11 measurements.

Before launching, the harness refuses to start when the machine is more than 25% busy
(`ALLOW_BUSY=1`, `BUSY_MAX`): a background build slows the game to a few FPS and makes the login
dialog miss clicks, so such a run would not be comparable.

Runs write to `telemetry/` (`abrun_<mode>_<stamp>.*`, `vkd3d_queue_*.json`).
