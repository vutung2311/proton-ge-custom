# Project Engineering Rules

## Robust, Bug-Free & Concurrency-Safe Logic
- **Invariant Verification & Zero Silly Assumptions**:
  - All written and proposed logic must be mathematically sound, robust, and verified against edge cases.
  - Never make ungrounded assumptions regarding API contracts, kernel state transitions, buffer boundaries, or execution order.
- **Race-Condition-Free & Deadlock-Free Concurrency**:
  - Analyze all concurrent execution paths before implementing synchronization primitives.
  - Maintain consistent lock acquisition hierarchies to eliminate deadlock potential.
  - Use exact memory ordering semantics (explicit `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` or full fences where appropriate) for lock-free data paths.
  - Strictly prevent unreset manual synchronization states and avoid tight non-blocking busy loops.
- **Handle & Resource Lifecycle Management**:
  - Account for handle invalidation, recycling, and teardown across every failure and exit branch.
  - Ensure zero resource leaks (file descriptors, synchronization objects, heap allocations).
- **Balance Between Simplicity & Robustness**:
  - Favor simple, idiomatic, and maintainable architectures over fragile or over-engineered hacks.
  - Keep logic concise and transparent while delivering production-grade reliability strictly aligned with specifications.

## Proactive Reporting & Communication Cadence
- **Prompt Reporting on Completion & State Changes**:
  - Never remain silent or require the user to ping for progress updates.
  - As soon as any background task, command execution, build, test, or investigation finishes, immediately return to report findings, results, and current operational status.
  - Proactively inform the user of next steps or blockers without waiting for follow-up prompts.

## Continuous Patch Synchronization
- **Mandatory Mirroring of Submodule & Source Edits**:
  - Whenever modifications or fixes are made to source trees/submodules (`wine/`, `dxvk/`, `vkd3d-proton/`, `protonfixes/`), immediately generate and sync the corresponding unified diff back into the relevant patch files under `patches/` (e.g. `patches/wine-hotfixes/pending/`, `patches/vkd3d-proton/`, `patches/protonfixes/`).
  - Keep `patches/protonprep-valve-staging.sh` aligned with any new or updated patch files so that clean checkouts and full upstream rebuilds are 100% reproducible.
  - Never leave source tree changes un-synced to `patches/`.

## Shell Scripting & Command Execution Standards
- **Strict Error Handling & Zero Failure Suppression**:
  - Never use `|| true` (or `|| :` / silent fallback masking) in bash commands, scripts, or build steps.
  - Scripts and command chains must fail fast on errors (`set -e` / `set -euo pipefail`) so that underlying failures are surfaced immediately rather than silently ignored.
  - When non-zero exit codes are expected under specific edge cases, handle them explicitly via structured conditionals/checks rather than blindly masking errors with `|| true`.

## Telemetry & Settle Time Grounding Standards
- **Zero Hallucinated Baselines & Rigorous Settle Metrics**:
  - Never use arbitrary game log callbacks (such as `on_finish_setup_acsdk`, which is just NetEase's Anti-Cheat SDK initialization) as a proxy for scene loading completion.
  - A teleport/scene load settle is ONLY confirmed when GPU utilization transitions to active 3D rendering (70%–100%, >150W), NVMe streaming bursts finish, and the 97% loading overlay is dismissed.
  - In Stock Upstream GE-Proton 11 (`wwm.exe_240163`), the game does NOT settle in 2.7s; it freezes for ~77 seconds in single-threaded Wineserver IPC (`anon_pipe_read`). Any claim that stock GE-Proton 11 settles in 2.0s–2.7s is a documented methodological fallacy and must never be cited in sub-agent debates or implementation plans.


