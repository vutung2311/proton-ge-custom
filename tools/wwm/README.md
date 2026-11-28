# Where Winds Meet: bpftrace generators

The game's A/B harness (`wwm_ab_run.sh`, `wwm_matrix.sh`, the recorders and reports) moved to the
`local_diagnostic` repository, `wwm/harness/`, since it measures the game rather than Wine. Its
last version here is 10c6def6. Its profiled runs still use `proton_profiler.py` and the generators
below (it finds this checkout through `PROTON_GE_DIR`, default `~/Git/proton-ge-custom`).

| Tool | Purpose |
| :--- | :--- |
| `gen_nt_census.sh <runner-dir> [out.bt]` | Nt* syscall census script for `proton_profiler.py` from a runner's `ntdll.so`. |
| `gen_iocp_purity.sh <runner-dir> [out.bt]`, `iocp_purity_report.py` | Completion-port census script and its report. |

Measurements taken before the move are in `telemetry/`.
