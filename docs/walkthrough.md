# Walkthrough: Where Winds Meet on GE-Proton11-custom

How to build the runner, configure the game and measure it, as of 2026-09-30. The evidence behind
each recommendation is in `docs/research_notes.md` (section numbers below).

## 1. Build the runner

```bash
./build_runner.sh
```

A clean build by default: it wipes `build/`, extracts every submodule at the commit this branch
pins, applies `patches/` (Wine through `patches/protonprep-valve-staging.sh`, vkd3d-proton from
`patches/vkd3d-proton`, the low-latency layer from `patches/vklayers`), compiles in the Steam Runtime
container and overlays the result onto the stock GE-Proton11-9 template as
`~/.local/share/lutris/runners/wine/GE-Proton11-custom`. It refuses to replace a runner that is in
use, and checks wineserver protocol parity, the storage TRIM answer (§36) and that the low-latency
layer's manifest points at the runner's own library (§39.2).

After a build that re-creates the runner directory, restart Lutris: a running Lutris caches its
runner list and would silently launch its default Proton instead (§38).

## 2. Game settings (Lutris entry 11)

| Setting | Recommendation | Why |
| :--- | :--- | :--- |
| `PROTON_ENABLE_WAYLAND` | `1` | Normal play and harness runs use Wine's Wayland driver; logins stall intermittently on X11 (§39.3). |
| `LOW_LATENCY_LAYER`, `LOW_LATENCY_LAYER_REFLEX`, `VKD3D_FORCE_LOW_LATENCY` | `0` | At the game's 100 fps cap the layer has nothing to trim and adds ~5 ms (§39.2). |
| `PROTON_PREFER_SDL` | `1` (unchanged) | Controller chords reach the game as the pad sends them either way (§39.5), so there is no reason to switch. |
| In-game frame cap | ~157 fps | With VRR on the 165 Hz panel and the GPU about half busy at 100 fps in the scenes measured, a higher cap shortens every per-frame stage. Keep it below the refresh rate. |

On KDE Plasma 6.7, set `plasmanotifyrc [DoNotDisturb] WhenFullscreen=false`, or every game launch
can freeze plasmashell (§39.3):

```bash
kwriteconfig6 --file plasmanotifyrc --group DoNotDisturb --key WhenFullscreen false
```

## 3. Measure with the harness

One-time setup for screen checks on Wayland (builds `tools/wwm/wwm_kwin_grab` and authorizes that
one executable for KWin's screenshot interface), plus tesseract's English model:

```bash
tools/wwm/setup_wayland_capture.sh
```

```bash
sudo pacman -S tesseract-data-eng
```

A hands-free run on entry 11 (logs in, resumes, measures the load, lets the game run 100 s from the load start, quits):

```bash
AUTO=1 NOPROFILE=1 tools/wwm/wwm_ab_run.sh p11ce11
```

- Close other heavy work first: the harness refuses to start above 25% CPU (`ALLOW_BUSY=1` overrides),
  because a background build slows the game to a few fps and invalidates the numbers.
- `NOLL=1` turns the low-latency layer and vkd3d-proton's forced mode off for a run; `EXTRA_ENV` adds
  variables for one run (`EXTRA_ENV="VKD3D_SWAPCHAIN_LATENCY_FRAMES=1"`). The entry is restored on exit.
- Alternate the variants (off, on, off, on) so drift in the game or its servers does not favour one.
- The first login of the day shows a reward popup that hides the HUD; that run gets no load time.

Results go to `telemetry/`. Compare runs with:

```bash
python3 tools/wwm/frametimes.py telemetry/abrun_<run> ...
```

```bash
python3 tools/wwm/latency.py telemetry/abrun_<run> ...
```

`frametimes.py` gives fps and frame-time percentiles; `latency.py` gives the time from `Present` to
the frame's completion and whether low-latency sleeps ran. Both measure from the screen-detected
world time; `--anchor teleport` measures from the load start instead.

## 4. The low-latency switches, for other games

| Variable | Effect |
| :--- | :--- |
| `LOW_LATENCY_LAYER=1` | Loads the layer. It offers `VK_AMD_anti_lag`; vkd3d-proton (D3D12) calls it without the presentation info the layer needs, so it does not pace D3D12 games (§39.1). |
| `LOW_LATENCY_LAYER_REFLEX=1` | The layer offers `VK_NV_low_latency2` instead, on any GPU. |
| `VKD3D_FORCE_LOW_LATENCY=1` | vkd3d-proton (D3D12) drives `VK_NV_low_latency2` itself for games that never call it. Needs the previous two. |
| `VKD3D_FORCE_LOW_LATENCY_FPS=<n>` | Frame cap applied in the layer's sleep, before a frame starts. With VRR, a few fps below the maximum refresh. |

The layer only helps when the GPU is the bottleneck and frames queue up behind it. Measure before
keeping it on (§39.2).

## 5. When a run fails

| Symptom | Likely cause |
| :--- | :--- |
| "machine is N% busy before launch" | Another program is using the CPU; wait or close it. |
| Log In never registers, game frozen or at a few fps | Machine load, or the X11 driver (§39.3). Check `telemetry/abrun_<run>.threads.txt`, written before the stuck game is closed. |
| "Lutris started before GE-Proton11-custom existed" | Restart Lutris after rebuilding the runner. |
| "LOW_LATENCY_LAYER=1 loaded ..., not the copy in ..." | Another manifest with the same layer name is installed on the host (§39.2). |
| "screen checks on Wayland need the KWin capture helper" | Run `tools/wwm/setup_wayland_capture.sh`. |
| Gray screen after a load, the game still running, GPU idle | A Wayland driver path kept its window data lock (seen 2026-10-21). Before closing the game, run `tools/wwm/wwm_wayland_lock_trace.py`: it names the code holding the lock. |
