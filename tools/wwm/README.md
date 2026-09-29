# Where Winds Meet A/B harness

Hands-free teleport-load and in-world measurements of Where Winds Meet under a given runner,
used for the numbers in `docs/research_notes.md` (§34-§37). Nothing here activates or raises a
window or moves the pointer: input is posted from inside Wine (`wwm_click.c`) and the screen is
read from the window's own contents (`wwm_grab.py`).

| Tool | Purpose |
| :--- | :--- |
| `wwm_ab_run.sh <mode>` | One run: points the Lutris entry at the runner (restored on exit), launches the game, logs in, teleports, quits. `AUTO=1` hands-free, `NOPROFILE=1` no bpftrace, `PROTONLOG=1` Proton log (`PROTONLOG_DEBUG` picks the channels), `EXTRA_ENV`, `BT`. See the header for all modes and switches. |
| `wwm_screen.py` | Screen recorder and classifier (loading bar, world HUD, dialogs); `scan` derives `world_visible`; `locate` finds a button by its text, so the harness clicks Resume and Continue where they are at any resolution, aspect ratio or UI scale (fixed 2560x1440 fractions are the logged second attempt). |
| `wwm_grab.py` | Read-only XComposite capture of the game window. |
| `wwm_cpu.py` | Per-second CPU and context switches of the game and its wineserver; `report` compares runs. |
| `frametimes.py` | In-world frame-time statistics from vkd3d-proton's queue trace. |
| `vkd3d_qprof.py` | Upload/queue timeline of a run from the same trace. |
| `wwm_gamelog_follow.py` | Follows the game log for the load-start marker. |
| `gen_nt_census.sh`, `gen_iocp_purity.sh`, `iocp_purity_report.py` | bpftrace script generators for the profiler and the completion-port census. |
| `wwm_click.c`, `wwm_windows.c` | In-Wine input and window listing; `wwm_ab_run.sh` builds the `.exe` files when needed. |

Runs write to `telemetry/` (`abrun_<mode>_<stamp>.*`, `vkd3d_queue_*.json`).
