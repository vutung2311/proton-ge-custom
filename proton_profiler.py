#!/usr/bin/env python3
"""
Unified Proton / Wine Linux Game Performance Profiler & Launch Diagnostic Engine
================================================================================
A high-resolution (20 Hz) telemetry and diagnostic tool designed to analyze,
profile, and troubleshoot Windows / Proton game launches and runtime performance on Linux.

Features:
  - Real-time continuous JSON streaming (NDJSON) flushed to disk line-by-line
  - Process exit detection and automatic session file rotation
  - Strict binary process target discovery (filters out wrapper scripts)
  - Comprehensive Proton launch environment diagnostics (sysctl, prefix, DLL overrides, sync)
  - Memory mappings, loaded PE DLLs, Linux .so, and Vulkan/DXVK/VKD3D layers
  - Open descriptors (/dev/ntsync, DRM render nodes, IPC sockets, pipes)
  - Multi-source log watchers & crash/exception detectors (0xc0000005, missing DLLs, stubs)
  - Multi-vendor GPU telemetry (AMDGPU DRM sysfs, NVIDIA SMI, Intel DRM)
  - Per-thread kernel metrics (TID, state, CPU ticks, wchan, kstack, context switches)
  - Modular, maintainable function architecture
"""

import os
import sys
import time
import json
import glob
import shutil
import signal
import argparse
import subprocess
from datetime import datetime
from collections import defaultdict

# ------------------------------------------------------------------------------
# 1. Constants, Error Codes & Defaults
# ------------------------------------------------------------------------------
PROFILER_VERSION = "2.1.0"
DEFAULT_TELEMETRY_DIR = os.environ.get("PROTON_TELEMETRY_DIR") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "telemetry")

KNOWN_WRAPPER_INTERPRETERS = {
    "bash", "sh", "dash", "zsh", "python", "python3",
    "perl", "ruby", "node", "env"
}

KNOWN_PROTON_ENV_KEYS = [
    "WINEPREFIX", "WINEARCH", "WINEDEBUG", "WINEDLLOVERRIDES",
    "WINEFSYNC", "WINEESYNC", "WINE_NTSYNC", "PROTON_NTSYNC",
    "PROTON_USE_WINED3D", "PROTON_NO_ESYNC", "PROTON_NO_FSYNC",
    "PROTON_FORCE_LARGE_ADDRESS_AWARE", "PROTON_HIDE_NVIDIA_GPU",
    "PROTON_ENABLE_NVAPI", "PROTON_ENABLE_NGX_UPDATER",
    "PROTON_LOG", "PROTON_LOG_DIR", "PROTON_CRASH_REPORT",
    "STEAM_COMPAT_DATA_PATH", "STEAM_COMPAT_CLIENT_INSTALL_PATH",
    "SteamAppId", "SteamGameId",
    "DXVK_CONFIG_FILE", "DXVK_HUD", "DXVK_ASYNC", "DXVK_STATE_CACHE",
    "VKD3D_CONFIG", "VKD3D_FEATURE_LEVEL", "VKD3D_DEBUG", "VKD3D_SHADER_CACHE_PATH",
    "RADV_PERFTEST", "AMD_VULKAN_ICD", "VK_ICD_FILENAMES", "VK_DRIVER_FILES",
    "MESA_VK_DEVICE_SELECT", "DRI_PRIME", "__GL_SHADER_DISK_CACHE",
    "DISPLAY", "WAYLAND_DISPLAY", "GAMESCOPE_WAYLAND_DISPLAY",
    "PULSE_LATENCY_MSEC", "PIPEWIRE_LATENCY"
]

NT_STATUS_SIGNATURES = {
    "0xc0000005": "STATUS_ACCESS_VIOLATION (Segmentation Fault / Memory Access Violation)",
    "0xc0000135": "STATUS_DLL_NOT_FOUND (Missing required DLL / unresolved import)",
    "0xc00000fd": "STATUS_STACK_OVERFLOW (Call stack recursion limit exceeded)",
    "0xc000001d": "STATUS_ILLEGAL_INSTRUCTION (CPU instruction set mismatch e.g. AVX/SSE)",
    "0xc0000022": "STATUS_ACCESS_DENIED (Permission error or Anti-Cheat driver block)",
    "0xc0000374": "STATUS_HEAP_CORRUPTION (Heap allocation boundary overrun)",
    "0x80000003": "STATUS_BREAKPOINT (Debug breakpoint hit without attached debugger)"
}

# Global graceful shutdown flag
g_stop_requested = False

def _signal_handler(signum, frame):
    global g_stop_requested
    g_stop_requested = True


# ------------------------------------------------------------------------------
# 2. Permissions, Utilities & Privilege Elevation
# ------------------------------------------------------------------------------
def get_original_user_ids():
    """Retrieve original non-root UID/GID when running under pkexec or sudo."""
    uid = os.environ.get("SUDO_UID") or os.environ.get("PKEXEC_UID")
    gid = os.environ.get("SUDO_GID")
    try:
        return (int(uid), int(gid) if gid else -1) if uid else (None, None)
    except Exception:
        return (None, None)


def ensure_file_permissions(filepath):
    """Ensure output files are writable and owned by the calling non-root user."""
    try:
        if os.path.exists(filepath):
            os.chmod(filepath, 0o666)
            uid, gid = get_original_user_ids()
            if uid is not None:
                os.chown(filepath, uid, gid)
    except Exception:
        pass


def read_file_safe(path, limit=None):
    """Safely read text file content without raising unhandled exceptions.

    Note: no os.path.exists() pre-check. In the hot sampling path this function is
    called ~4x per thread per sample; the extra stat() doubled the syscall count for
    no benefit, since a vanished thread raises and is caught here anyway.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read(limit) if limit else f.read()
    except Exception:
        return None


def check_and_elevate_privileges(auth_method="auto"):
    """
    Ensure root privileges for reading /proc/<pid>/task/<tid>/stack, DRM sysfs, and fds.
    If unprivileged, automatically re-executes with pkexec or sudo preserving environment.
    """
    if os.geteuid() == 0:
        return True

    if auth_method == "none":
        print("[WARN] Running without root privileges. Kernel stack traces and certain DRM nodes will be restricted.", file=sys.stderr)
        return False

    tool = None
    if auth_method == "pkexec":
        tool = shutil.which("pkexec")
    elif auth_method == "sudo":
        tool = shutil.which("sudo")
    else:  # auto
        tool = shutil.which("pkexec") or shutil.which("sudo")

    if not tool:
        print("[WARN] Neither pkexec nor sudo found. Continuing with standard permissions.", file=sys.stderr)
        return False

    script_path = os.path.abspath(sys.argv[0])
    args = [tool, "python3", script_path] + sys.argv[1:]

    if tool.endswith("pkexec"):
        args = [tool, "env", f"PYTHONPATH={os.environ.get('PYTHONPATH', '')}", "python3", script_path] + sys.argv[1:]

    try:
        print(f"[AUTH] Elevating privileges via {os.path.basename(tool)}...")
        os.execvp(tool, args)
    except Exception as e:
        print(f"[WARN] Privilege elevation failed: {e}. Continuing with standard permissions.", file=sys.stderr)
        return False


# ------------------------------------------------------------------------------
# 3. Target Process Discovery & Disambiguation
# ------------------------------------------------------------------------------
def is_wrapper_process(pid, comm, cmdline_parts, target_name):
    """
    Check if this process is merely a wrapper script or interpreter passing target_name as an argument.
    """
    comm_l = comm.lower()
    t_name = target_name.lower()
    t_base = t_name[:-4] if t_name.endswith(".exe") else t_name

    # If comm matches target name or basename (accounting for Linux 15-char comm limit), it is the game process
    if comm_l in (t_name, t_base, t_name[:15], t_base[:15]):
        return False

    # Check executable binary
    try:
        exe_path = os.readlink(f"/proc/{pid}/exe")
        exe_base = os.path.basename(exe_path).lower()
        if exe_base in (t_name, t_base):
            return False
        # If the underlying binary is a known script interpreter and comm doesn't match target
        if exe_base in KNOWN_WRAPPER_INTERPRETERS:
            return True
    except Exception:
        pass

    # If comm is an interpreter / shell, it's a wrapper
    if comm_l in KNOWN_WRAPPER_INTERPRETERS:
        return True

    return False


def get_process_cmdline_parts(pid):
    """Read cmdline tokens from /proc/<pid>/cmdline."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            data = f.read()
        return [p.decode("utf-8", errors="replace") for p in data.split(b"\x00") if p]
    except Exception:
        return []


def match_process_by_name(pid, target_name):
    """
    Check if a PID matches the target process name.
    Strictly ensures the target matches the binary comm/exe, ignoring script wrappers.
    """
    try:
        comm = read_file_safe(f"/proc/{pid}/comm")
        if not comm:
            return False
        comm = comm.strip()
        cmdline_parts = get_process_cmdline_parts(pid)

        if is_wrapper_process(pid, comm, cmdline_parts, target_name):
            return False

        t_name = target_name.lower()
        t_base = t_name[:-4] if t_name.endswith(".exe") else t_name
        comm_l = comm.lower()

        # 1. Match comm exact, extension-less, or 15-char Linux comm truncation
        if comm_l in (t_name, t_base, t_name[:15], t_base[:15]):
            return True

        # 2. Match exe target basename
        try:
            exe_target = os.readlink(f"/proc/{pid}/exe")
            exe_base = os.path.basename(exe_target).lower()
            if exe_base in (t_name, t_base):
                return True
        except Exception:
            pass

        # 3. Match argv[0] of binary if not an interpreter
        if cmdline_parts:
            argv0 = cmdline_parts[0].replace("\\", "/")
            argv0_base = os.path.basename(argv0).lower()
            if argv0_base in (t_name, t_base):
                return True
            # For Wine: 'wine64-preloader /path/to/game.exe'
            if any(w in argv0_base for w in ["wine", "wine64", "preloader"]) and len(cmdline_parts) > 1:
                argv1 = cmdline_parts[1].replace("\\", "/")
                argv1_base = os.path.basename(argv1).lower()
                if argv1_base in (t_name, t_base):
                    return True

    except Exception:
        pass

    return False


def scan_for_proton_executables():
    """Auto-discover active Proton/Wine game binaries (.exe), ignoring system services."""
    candidates = []
    try:
        for p in os.listdir("/proc"):
            if not p.isdigit():
                continue
            pid = int(p)
            if pid == os.getpid():
                continue
            try:
                comm_path = f"/proc/{pid}/comm"
                if not os.path.exists(comm_path):
                    continue
                with open(comm_path, "r", encoding="utf-8", errors="replace") as f:
                    comm = f.read().strip()
                comm_l = comm.lower()

                ignored_services = {"wineserver", "services.exe", "explorer.exe", "svchost.exe", "rpcss.exe", "plugplay.exe"}
                if comm_l.endswith(".exe") and comm_l not in ignored_services:
                    cmdline_parts = get_process_cmdline_parts(pid)
                    if not is_wrapper_process(pid, comm, cmdline_parts, comm):
                        candidates.append((pid, comm))
            except Exception:
                pass
    except Exception:
        pass

    # Sort ascending by PID (first created process first)
    candidates.sort(key=lambda x: x[0])
    return candidates


def resolve_target_pid(target_name=None, target_pid=None):
    """
    Resolve the primary target PID:
      - If PID provided: returns it if valid
      - If process name provided: finds the first matching non-wrapper PID
      - If none provided: auto-discovers active Proton game binary
    """
    if target_pid is not None:
        if os.path.exists(f"/proc/{target_pid}"):
            return int(target_pid)
        return None

    if target_name:
        candidates = []
        try:
            for p in os.listdir("/proc"):
                if not p.isdigit() or int(p) == os.getpid():
                    continue
                pid = int(p)
                if match_process_by_name(pid, target_name):
                    candidates.append(pid)
        except Exception:
            pass

        if candidates:
            # Pick the first matching PID
            candidates.sort()
            return candidates[0]
        return None

    # Auto-discovery
    auto_candidates = scan_for_proton_executables()
    if auto_candidates:
        return auto_candidates[0][0]

    return None


def _read_environ_key(pid, key):
    """Extract a single environment variable from /proc/<pid>/environ."""
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            data = f.read()
        for item in data.split(b"\x00"):
            if not item:
                continue
            k, _, v = item.partition(b"=")
            if k.decode("utf-8", "replace") == key:
                return v.decode("utf-8", "replace")
    except Exception:
        pass
    return None


def get_wineserver_pid(target_pid=None):
    """Find the wineserver serving the target game process.

    The previous implementation returned the lowest-PID wineserver on the machine,
    which is wrong whenever more than one prefix is running (Lutris + Steam, a second
    game, a background Wine app): the ws_ticks/ws_wchan columns then describe an
    unrelated process. Match on WINEPREFIX first, then on the server socket directory,
    and only fall back to "any wineserver" when the target is unknown.
    """
    try:
        out = subprocess.check_output(["pgrep", "-x", "wineserver"], stderr=subprocess.DEVNULL).decode().strip()
        pids = [int(p) for p in out.split() if p.isdigit()]
    except Exception:
        return None
    if not pids:
        return None
    if len(pids) == 1 or not target_pid:
        return pids[0]

    want_prefix = _read_environ_key(target_pid, "WINEPREFIX")
    if want_prefix:
        for wp in pids:
            if _read_environ_key(wp, "WINEPREFIX") == want_prefix:
                return wp

    # Fall back: same wineserver socket directory (/tmp/.wine-<uid>/server-<dev>-<inode>)
    def server_dir(p):
        try:
            for fd in os.listdir(f"/proc/{p}/fd"):
                tgt = os.readlink(f"/proc/{p}/fd/{fd}")
                if "/server-" in tgt:
                    return tgt.rsplit("/", 1)[0]
        except Exception:
            pass
        return None

    want_dir = server_dir(target_pid)
    if want_dir:
        for wp in pids:
            if server_dir(wp) == want_dir:
                return wp

    return pids[0]


# ------------------------------------------------------------------------------
# 4. Host System, Kernel & Filesystem Diagnostics
# ------------------------------------------------------------------------------
def get_host_system_info():
    """Retrieve host kernel, CPU model, architecture, and memory summary."""
    info = {
        "kernel_release": "unknown",
        "cpu_model": "unknown",
        "cpu_cores_logical": os.cpu_count() or 1,
        "glibc_version": "unknown"
    }
    try:
        info["kernel_release"] = os.uname().release
    except Exception:
        pass

    cpuinfo = read_file_safe("/proc/cpuinfo")
    if cpuinfo:
        for line in cpuinfo.splitlines():
            if line.startswith("model name"):
                info["cpu_model"] = line.split(":", 1)[1].strip()
                break

    try:
        out = subprocess.check_output(["ldd", "--version"], stderr=subprocess.DEVNULL).decode()
        first_line = out.splitlines()[0]
        info["glibc_version"] = first_line.split()[-1]
    except Exception:
        pass

    return info


def get_sysctl_settings():
    """Read critical sysctls affecting Proton and multi-threaded games."""
    sysctls = {
        "vm.max_map_count": None,
        "fs.file-max": None,
        "kernel.split_lock_mitigate": None
    }
    for key, path in [
        ("vm.max_map_count", "/proc/sys/vm/max_map_count"),
        ("fs.file-max", "/proc/sys/fs/file-max"),
        ("kernel.split_lock_mitigate", "/proc/sys/kernel/split_lock_mitigate")
    ]:
        val = read_file_safe(path)
        if val:
            try:
                sysctls[key] = int(val.strip())
            except ValueError:
                sysctls[key] = val.strip()
    return sysctls


def get_system_memory_info():
    """Read host RAM, swap, and dirty cache metrics from /proc/meminfo."""
    mem = {"total_mb": 0, "free_mb": 0, "available_mb": 0, "swap_total_mb": 0, "swap_free_mb": 0, "dirty_mb": 0}
    content = read_file_safe("/proc/meminfo")
    if content:
        for line in content.splitlines():
            parts = line.split(":")
            if len(parts) == 2:
                k = parts[0].strip()
                v = parts[1].strip().split()[0]
                if k == "MemTotal": mem["total_mb"] = int(v) // 1024
                elif k == "MemFree": mem["free_mb"] = int(v) // 1024
                elif k == "MemAvailable": mem["available_mb"] = int(v) // 1024
                elif k == "SwapTotal": mem["swap_total_mb"] = int(v) // 1024
                elif k == "SwapFree": mem["swap_free_mb"] = int(v) // 1024
                elif k == "Dirty": mem["dirty_mb"] = int(v) // 1024
    return mem


def get_psi_pressure():
    """Read Linux Pressure Stall Information (PSI) for CPU, memory, and I/O."""
    psi = {"cpu_some_10": 0.0, "mem_some_10": 0.0, "io_some_10": 0.0}
    for ptype in ["cpu", "memory", "io"]:
        content = read_file_safe(f"/proc/pressure/{ptype}")
        if content:
            for line in content.splitlines():
                if line.startswith("some"):
                    for item in line.split():
                        if item.startswith("avg10="):
                            val = float(item.split("=")[1])
                            if ptype == "cpu": psi["cpu_some_10"] = val
                            elif ptype == "memory": psi["mem_some_10"] = val
                            elif ptype == "io": psi["io_some_10"] = val
    return psi


def get_filesystem_info(path):
    """Determine mount point, filesystem type, and disk space for a given path."""
    res = {"path": path, "mount_point": "unknown", "fs_type": "unknown", "free_gb": 0.0, "total_gb": 0.0, "is_ntfs": False}
    if not path or not os.path.exists(path):
        return res

    try:
        statv = os.statvfs(path)
        res["free_gb"] = round((statv.f_bavail * statv.f_frsize) / (1024**3), 2)
        res["total_gb"] = round((statv.f_blocks * statv.f_frsize) / (1024**3), 2)
    except Exception:
        pass

    try:
        target_dev = os.stat(path).st_dev
        with open("/proc/mounts", "r", encoding="utf-8") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 3:
                    mount_p = parts[1]
                    try:
                        if os.stat(mount_p).st_dev == target_dev:
                            res["mount_point"] = mount_p
                            res["fs_type"] = parts[2]
                            if any(k in parts[2].lower() for k in ["ntfs", "fuseblk"]):
                                res["is_ntfs"] = True
                    except Exception:
                        pass
    except Exception:
        pass

    return res


# ------------------------------------------------------------------------------
# 5. Multi-Vendor GPU Telemetry Collectors
# ------------------------------------------------------------------------------
def get_amdgpu_metrics():
    """Collect AMDGPU DRM sysfs telemetry (VRAM, GTT, load, power, temps)."""
    cards = glob.glob("/sys/class/drm/card[0-9]/device")
    metrics_list = []
    for card in cards:
        m = {
            "vendor": "AMD (amdgpu)",
            "busy_pct": 0, "mem_busy_pct": 0,
            "vram_used_mb": 0, "vram_total_mb": 0,
            "gtt_used_mb": 0, "gtt_total_mb": 0,
            "vis_vram_used_mb": 0, "vis_vram_total_mb": 0,
            "power_w": 0.0, "temp_c": 0.0, "temp_hotspot_c": 0.0, "fan_rpm": 0
        }
        for attr, key in [
            ("gpu_busy_percent", "busy_pct"),
            ("mem_busy_percent", "mem_busy_pct"),
        ]:
            p = os.path.join(card, attr)
            val = read_file_safe(p)
            if val:
                try: m[key] = int(val.strip())
                except ValueError: pass

        for attr, key in [
            ("mem_info_vram_used", "vram_used_mb"),
            ("mem_info_vram_total", "vram_total_mb"),
            ("mem_info_gtt_used", "gtt_used_mb"),
            ("mem_info_gtt_total", "gtt_total_mb"),
            ("mem_info_vis_vram_used", "vis_vram_used_mb"),
            ("mem_info_vis_vram_total", "vis_vram_total_mb")
        ]:
            p = os.path.join(card, attr)
            val = read_file_safe(p)
            if val:
                try: m[key] = int(val.strip()) // (1024 * 1024)
                except ValueError: pass

        # Hwmon sensors
        for hwd in glob.glob(os.path.join(card, "hwmon/hwmon*")):
            p_val = read_file_safe(os.path.join(hwd, "power1_average"))
            if p_val:
                try: m["power_w"] = max(m["power_w"], int(p_val.strip()) / 1e6)
                except ValueError: pass

            t_val = read_file_safe(os.path.join(hwd, "temp1_input"))
            if t_val:
                try: m["temp_c"] = max(m["temp_c"], int(t_val.strip()) / 1e3)
                except ValueError: pass

            thot_val = read_file_safe(os.path.join(hwd, "temp2_input"))
            if thot_val:
                try: m["temp_hotspot_c"] = max(m["temp_hotspot_c"], int(thot_val.strip()) / 1e3)
                except ValueError: pass

            fan_val = read_file_safe(os.path.join(hwd, "fan1_input"))
            if fan_val:
                try: m["fan_rpm"] = max(m["fan_rpm"], int(fan_val.strip()))
                except ValueError: pass

        metrics_list.append(m)

    if metrics_list:
        # Prioritize card with highest VRAM capacity / busiest
        metrics_list.sort(key=lambda x: (x["vram_total_mb"], x["busy_pct"]), reverse=True)
        return metrics_list[0]
    return None


def get_nvidia_metrics():
    """Collect NVIDIA GPU metrics via nvidia-smi query."""
    if not shutil.which("nvidia-smi"):
        return None
    try:
        out = subprocess.check_output([
            "nvidia-smi", "--query-gpu=utilization.gpu,memory.used,memory.total,power.draw,temperature.gpu",
            "--format=csv,noheader,nounits"
        ], stderr=subprocess.DEVNULL, timeout=0.15).decode().strip()
        if out:
            parts = [p.strip() for p in out.splitlines()[0].split(",")]
            return {
                "vendor": "NVIDIA",
                "busy_pct": int(float(parts[0])),
                "mem_busy_pct": 0,
                "vram_used_mb": int(float(parts[1])),
                "vram_total_mb": int(float(parts[2])),
                "gtt_used_mb": 0, "gtt_total_mb": 0,
                "vis_vram_used_mb": 0, "vis_vram_total_mb": 0,
                "power_w": float(parts[3]),
                "temp_c": float(parts[4]),
                "temp_hotspot_c": 0.0,
                "fan_rpm": 0
            }
    except Exception:
        pass
    return None


def get_intel_metrics():
    """Collect Intel i915 / Xe GPU metrics from sysfs."""
    cards = glob.glob("/sys/class/drm/card[0-9]/gt/gt[0-9]*")
    if cards:
        return {
            "vendor": "Intel (i915/xe)",
            "busy_pct": 0, "mem_busy_pct": 0,
            "vram_used_mb": 0, "vram_total_mb": 0,
            "gtt_used_mb": 0, "gtt_total_mb": 0,
            "vis_vram_used_mb": 0, "vis_vram_total_mb": 0,
            "power_w": 0.0, "temp_c": 0.0, "temp_hotspot_c": 0.0, "fan_rpm": 0
        }
    return None


def get_gpu_metrics():
    """Query and return active primary GPU metrics across AMD, NVIDIA, or Intel."""
    return get_amdgpu_metrics() or get_nvidia_metrics() or get_intel_metrics() or {
        "vendor": "unknown",
        "busy_pct": 0, "mem_busy_pct": 0,
        "vram_used_mb": 0, "vram_total_mb": 0,
        "gtt_used_mb": 0, "gtt_total_mb": 0,
        "vis_vram_used_mb": 0, "vis_vram_total_mb": 0,
        "power_w": 0.0, "temp_c": 0.0, "temp_hotspot_c": 0.0, "fan_rpm": 0
    }


def get_cpu_frequencies_and_temperature():
    """Extract CPU scaling governor, per-core frequencies, and package temperature."""
    res = {"governor": "unknown", "avg_freq_mhz": 0.0, "max_freq_mhz": 0.0, "cpu_temp_c": 0.0}
    freq_files = glob.glob("/sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq")
    if freq_files:
        freqs = []
        for ff in freq_files:
            val = read_file_safe(ff)
            if val:
                try: freqs.append(int(val.strip()) / 1000.0)
                except ValueError: pass
        if freqs:
            res["avg_freq_mhz"] = round(sum(freqs) / len(freqs), 1)
            res["max_freq_mhz"] = round(max(freqs), 1)

    gov = read_file_safe("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    if gov:
        res["governor"] = gov.strip()

    # CPU Temp
    for hwd in glob.glob("/sys/class/hwmon/hwmon*"):
        name = read_file_safe(os.path.join(hwd, "name"))
        if name and name.strip() in ["k10temp", "coretemp", "zenpower"]:
            t_val = read_file_safe(os.path.join(hwd, "temp1_input"))
            if t_val:
                try: res["cpu_temp_c"] = max(res["cpu_temp_c"], int(t_val.strip()) / 1000.0)
                except ValueError: pass

    return res


# ------------------------------------------------------------------------------
# 6. Wine/Proton Process Tree & Environment Inspector
# ------------------------------------------------------------------------------
def get_process_tree(root_pid):
    """Retrieve full process tree (PIDs, comm, PPID, RSS) under a root process."""
    tree = []
    try:
        pids_to_check = [root_pid]
        visited = set()
        while pids_to_check:
            curr_pid = pids_to_check.pop(0)
            if curr_pid in visited:
                continue
            visited.add(curr_pid)

            comm = read_file_safe(f"/proc/{curr_pid}/comm")
            if not comm:
                continue

            entry = {
                "pid": curr_pid,
                "comm": comm.strip(),
                "cmdline": " ".join(get_process_cmdline_parts(curr_pid))[:256]
            }
            tree.append(entry)

            # Find children
            children_str = read_file_safe(f"/proc/{curr_pid}/task/{curr_pid}/children")
            if children_str:
                for c_pid_str in children_str.split():
                    try: pids_to_check.append(int(c_pid_str))
                    except ValueError: pass
    except Exception:
        pass
    return tree


def get_process_environ(pid):
    """Extract known Proton / Wine / Graphics environment variables from /proc/<pid>/environ."""
    env = {}
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            raw_entries = f.read().split(b"\x00")
        for entry in raw_entries:
            if not entry or b"=" not in entry:
                continue
            k, v = entry.split(b"=", 1)
            key_str = k.decode("utf-8", errors="replace")
            val_str = v.decode("utf-8", errors="replace")
            if key_str in KNOWN_PROTON_ENV_KEYS or key_str.startswith("WINE") or key_str.startswith("PROTON") or key_str.startswith("DXVK"):
                env[key_str] = val_str
    except Exception:
        pass
    return env


def detect_sync_mechanism(pid):
    """Detect if ntsync, fsync, esync, or wineserver sync is active."""
    mechanisms = []
    # Check open file descriptors for /dev/ntsync
    try:
        fd_dir = f"/proc/{pid}/fd"
        if os.path.exists(fd_dir):
            for link in os.listdir(fd_dir):
                try:
                    target = os.readlink(os.path.join(fd_dir, link))
                    if "ntsync" in target:
                        mechanisms.append("ntsync (in-kernel)")
                        break
                except Exception:
                    pass
    except Exception:
        pass

    # Check environment
    env = get_process_environ(pid)
    if env.get("WINEFSYNC") == "1" or env.get("PROTON_NO_FSYNC") != "1":
        if "ntsync (in-kernel)" not in mechanisms and env.get("WINEFSYNC") == "1":
            mechanisms.append("fsync (futex2)")
    if env.get("WINEESYNC") == "1":
        mechanisms.append("esync (eventfd)")

    if not mechanisms:
        mechanisms.append("wineserver (IPC fallback)")

    return ", ".join(mechanisms)


# ------------------------------------------------------------------------------
# 7. Mapped Binaries, Layers & Descriptors
# ------------------------------------------------------------------------------
def get_loaded_libraries(pid):
    """Categorize mapped PE DLLs, Linux .so, and Vulkan/DXVK/VKD3D layers from /proc/<pid>/maps."""
    pe_dlls = set()
    linux_so = set()
    vulkan_layers = set()

    maps_content = read_file_safe(f"/proc/{pid}/maps")
    if maps_content:
        for line in maps_content.splitlines():
            parts = line.split()
            if len(parts) >= 6:
                path = parts[5]
                base = os.path.basename(path).lower()
                if base.endswith(".dll"):
                    pe_dlls.add(base)
                elif ".so" in base:
                    linux_so.add(base)
                    if any(k in base for k in ["dxvk", "vkd3d", "radv", "amdgpu", "nvidia", "mangohud", "vkbasalt", "gamescope", "low_latency"]):
                        vulkan_layers.add(base)

    return {
        "pe_dlls_count": len(pe_dlls),
        "pe_dlls_sample": sorted(list(pe_dlls))[:15],
        "vulkan_layers": sorted(list(vulkan_layers)),
        "linux_so_sample": sorted(list(linux_so))[:10]
    }


def get_open_descriptors_summary(pid):
    """Inspect /proc/<pid>/fd to summarize device nodes, sockets, and pipes."""
    summary = {
        "ntsync_fd": False,
        "drm_render_nodes": [],
        "pipes_count": 0,
        "sockets_count": 0,
        "total_fds": 0
    }
    fd_dir = f"/proc/{pid}/fd"
    if os.path.exists(fd_dir):
        try:
            entries = os.listdir(fd_dir)
            summary["total_fds"] = len(entries)
            for link in entries:
                try:
                    target = os.readlink(os.path.join(fd_dir, link))
                    if "ntsync" in target:
                        summary["ntsync_fd"] = True
                    elif "renderD" in target or "card" in target or "nvidia" in target:
                        if target not in summary["drm_render_nodes"]:
                            summary["drm_render_nodes"].append(target)
                    elif target.startswith("pipe:"):
                        summary["pipes_count"] += 1
                    elif target.startswith("socket:"):
                        summary["sockets_count"] += 1
                except Exception:
                    pass
        except Exception:
            pass
    return summary


# ------------------------------------------------------------------------------
# 8. Granular Thread & CPU Telemetry
# ------------------------------------------------------------------------------
def read_thread_stat(pid, tid, collect_kstack=False, t_base=None):
    """Read granular per-thread statistics from /proc/<pid>/task/<tid>/.

    Fields captured per thread:
      ticks/utime/stime  CPU time (USER_HZ)
      minflt/majflt      page faults
      wchan              kernel wait channel (symbol, or "0" when on-CPU/runnable)
      vol_cs/invol_cs    context switch counters
      sched_ns           cumulative time ON cpu, nanoseconds     (schedstat[0])
      runq_ns            cumulative time RUNNABLE BUT NOT RUNNING (schedstat[1])
      sched_cnt          number of times scheduled onto a cpu     (schedstat[2])
      rd_off_ms          offset of this thread's read from the start of the scan

    runq_ns is the important one: delta(runq_ns) / delta(sched_cnt) is the average
    wake-to-run latency for that thread over the sample window. That is the quantity
    people usually try to infer from context-switch rates, and it is measured here
    directly rather than estimated.

    kstack is OFF by default. Reading /proc/<tid>/stack forces a kernel stack unwind
    under task_lock for every thread on every sample; at ~200 threads and 20 Hz that
    is ~4000 unwinds/sec, it dominates scan cost, and empirically it duplicates wchan
    in >99% of samples. Enable with --kstack only when you actually need the frame.
    """
    try:
        content = read_file_safe(f"/proc/{pid}/task/{tid}/stat")
        if not content:
            return None

        rparen = content.rfind(')')
        comm = content[content.find('(')+1:rparen]
        fields = content[rparen+2:].split()

        # /proc/<tid>/stat, 1-based field N lands at fields[N-3] after the comm split.
        state = fields[0]        # field 3
        minflt = int(fields[7])  # field 10
        majflt = int(fields[9])  # field 12
        utime = int(fields[11])  # field 14
        stime = int(fields[12])  # field 15

        wchan_str = read_file_safe(f"/proc/{pid}/task/{tid}/wchan") or "0"
        wchan_str = wchan_str.strip()

        # Scheduler accounting: "<sum_exec_runtime> <run_delay> <pcount>", all cumulative.
        sched_ns, runq_ns, sched_cnt = 0, 0, 0
        sched_content = read_file_safe(f"/proc/{pid}/task/{tid}/schedstat")
        if sched_content:
            sp = sched_content.split()
            if len(sp) >= 3:
                try:
                    sched_ns, runq_ns, sched_cnt = int(sp[0]), int(sp[1]), int(sp[2])
                except ValueError:
                    pass

        # Kernel stack top frame (opt-in: expensive, see docstring)
        kernel_stack_top = ""
        if collect_kstack:
            stack_content = read_file_safe(f"/proc/{pid}/task/{tid}/stack")
            if stack_content:
                for line in stack_content.splitlines():
                    if line.startswith("[<"):
                        parts = line.strip().split("]")
                        if len(parts) > 1:
                            func_name = parts[1].strip().split("+")[0]
                            if func_name and not func_name.startswith("0x") and func_name != "0":
                                kernel_stack_top = func_name
                                break

        vol_cs, invol_cs = 0, 0
        status_content = read_file_safe(f"/proc/{pid}/task/{tid}/status")
        if status_content:
            for line in status_content.splitlines():
                if line.startswith("voluntary_ctxt_switches:"):
                    vol_cs = int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches:"):
                    invol_cs = int(line.split()[1])
                    break

        rec = {
            "tid": tid,
            "comm": comm,
            "state": state,
            "ticks": utime + stime,
            "utime": utime,
            "stime": stime,
            "minflt": minflt,
            "majflt": majflt,
            "wchan": wchan_str,
            "kstack": kernel_stack_top,
            "vol_cs": vol_cs,
            "invol_cs": invol_cs,
            "sched_ns": sched_ns,
            "runq_ns": runq_ns,
            "sched_cnt": sched_cnt
        }
        if t_base is not None:
            # Threads are not read simultaneously. Record when each one was actually
            # sampled so per-thread rates can be corrected for scan position.
            rec["rd_off_ms"] = round((time.perf_counter() - t_base) * 1000.0, 3)
        return rec
    except Exception:
        return None


def get_memory_maps(pid):
    """Snapshot executable mappings from /proc/<pid>/maps.

    Wine maps its builtin PE DLLs (ntdll.dll and friends) high in the address space,
    and bpftrace's ustack/usym cannot name anything inside them -- those frames come
    back as bare addresses like 0x6fffef7514e7. Recording the mappings WHILE THE
    PROCESS IS ALIVE is what makes those addresses resolvable afterwards: subtract
    the mapping base and you have file+offset, which objdump/winedump can name.

    Must be captured during the run; /proc/<pid>/maps is gone the moment it exits.
    """
    out = []
    content = read_file_safe(f"/proc/{pid}/maps")
    if not content:
        return out
    for line in content.splitlines():
        parts = line.split(None, 5)
        if len(parts) < 5:
            continue
        addrs, perms = parts[0], parts[1]
        if "x" not in perms:
            continue
        path = parts[5].strip() if len(parts) > 5 else ""
        if not path or path.startswith("["):
            continue
        try:
            lo, hi = (int(x, 16) for x in addrs.split("-"))
        except Exception:
            continue
        out.append({"lo": lo, "hi": hi, "off": parts[2], "path": path})
    return out


_SYMCACHE = {}


def _load_symbols(path):
    """Lazily pull the symbol table of an on-disk ELF so addresses can be named."""
    if path in _SYMCACHE:
        return _SYMCACHE[path]
    syms = []
    try:
        if os.path.exists(path):
            out = subprocess.check_output(["nm", "-C", "--defined-only", "-n", path],
                                          stderr=subprocess.DEVNULL, timeout=20).decode()
            for line in out.splitlines():
                parts = line.split(None, 2)
                if len(parts) == 3 and parts[1] in "tTwW":
                    try:
                        syms.append((int(parts[0], 16), parts[2]))
                    except ValueError:
                        pass
            syms.sort()
    except Exception:
        syms = []
    _SYMCACHE[path] = syms
    return syms


def resolve_addr(addr, maps):
    """Attribute a raw instruction pointer to '<file>+0x<offset>', naming the
    function when the file is an ELF with symbols.

    The mapping's FILE OFFSET matters: a runtime address is
        file_offset = (addr - mapping.lo) + mapping.off
    Dropping that term silently shifts every address by the size of the preceding
    segments and lands it in the wrong function -- it reported a hot spin loop in
    NtRemoveIoCompletionEx as being in sock_ioctl, which is a wrong answer that
    looks entirely plausible.
    """
    try:
        a = int(addr)
    except Exception:
        return str(addr)
    for mp in maps:
        if mp["lo"] <= a < mp["hi"]:
            try:
                off = int(mp["off"], 16)
            except Exception:
                off = 0
            file_off = a - mp["lo"] + off
            base = os.path.basename(mp["path"])
            syms = _load_symbols(mp["path"])
            if syms:
                i = 0
                lo_i, hi_i = 0, len(syms) - 1
                while lo_i <= hi_i:
                    mid = (lo_i + hi_i) // 2
                    if syms[mid][0] <= file_off:
                        i, lo_i = mid, mid + 1
                    else:
                        hi_i = mid - 1
                if syms[i][0] <= file_off:
                    return f"{base}:{syms[i][1]}+0x{file_off - syms[i][0]:x}"
            return f"{base}+0x{file_off:x}"
    return f"0x{a:x}"


def read_process_memory_and_io(pid):
    """Read process RAM footprint and storage I/O metrics."""
    res = {
        "read_bytes": 0, "write_bytes": 0, "rchar": 0, "wchar": 0, "syscr": 0, "syscw": 0,
        "rss_mb": 0, "vsize_mb": 0, "swap_mb": 0
    }
    io_content = read_file_safe(f"/proc/{pid}/io")
    if io_content:
        for line in io_content.splitlines():
            parts = line.split(":")
            if len(parts) == 2:
                k = parts[0].strip()
                try:
                    v = int(parts[1].strip())
                    if k in res: res[k] = v
                except ValueError:
                    pass

    status_content = read_file_safe(f"/proc/{pid}/status")
    if status_content:
        for line in status_content.splitlines():
            if line.startswith("VmRSS:"):
                res["rss_mb"] = int(line.split()[1]) // 1024
            elif line.startswith("VmSize:"):
                res["vsize_mb"] = int(line.split()[1]) // 1024
            elif line.startswith("VmSwap:"):
                res["swap_mb"] = int(line.split()[1]) // 1024

    return res


def collect_thread_snapshots(pid, collect_kstack=False):
    """Collect statistics for all active threads in a process.

    Returns (threads, scan_ms). Threads are read in ascending tid order rather than
    in whatever order the directory happens to yield, so a given thread's position
    within the scan is stable across samples and the read skew is systematic instead
    of random.
    """
    threads = []
    t_base = time.perf_counter()
    try:
        tids = sorted(int(e) for e in os.listdir(f"/proc/{pid}/task") if e.isdigit())
    except Exception:
        return threads, 0.0
    for tid in tids:
        st = read_thread_stat(pid, tid, collect_kstack=collect_kstack, t_base=t_base)
        if st:
            threads.append(st)
    return threads, (time.perf_counter() - t_base) * 1000.0


def collect_wineserver_metrics(ws_pid):
    """Read wineserver CPU ticks, state, and wait channel."""
    if not ws_pid or not os.path.exists(f"/proc/{ws_pid}"):
        return {"ticks": 0, "wchan": "", "active": False}
    ticks = 0
    wchan = ""
    stat_content = read_file_safe(f"/proc/{ws_pid}/stat")
    if stat_content:
        rparen = stat_content.rfind(')')
        if rparen != -1:
            fields = stat_content[rparen+2:].split()
            ticks = int(fields[11]) + int(fields[12])
    wchan_val = read_file_safe(f"/proc/{ws_pid}/wchan")
    if wchan_val:
        wchan = wchan_val.strip()
    return {"ticks": ticks, "wchan": wchan, "active": True}


# ------------------------------------------------------------------------------
# 9. Multi-Source Log & Crash Monitoring
# ------------------------------------------------------------------------------
def decode_caesar(data) -> str:
    """Decrypt WWM Caesar-cipher log lines (shift printable ASCII 32-126 by -1).

    Operates on BYTES and decodes as UTF-8 afterwards. The previous version decoded
    latin1 first and shifted codepoints, which corrupted every multi-byte UTF-8
    sequence (the CJK server/area names came out as mojibake in the 'raw' field).
    Shifting at the byte level leaves bytes >= 0x80 untouched, so UTF-8 sequences
    survive intact.
    """
    if isinstance(data, str):
        data = data.encode("latin1", "replace")
    out = bytearray(len(data))
    for i, b in enumerate(data):
        out[i] = b - 1 if 32 <= b <= 126 else b
    return out.decode("utf-8", "replace")


# "2026-09-14 19:05:43 ->> ..."
_LOG_TS_RE = None


def parse_log_timestamp(line):
    """Extract the wall-clock timestamp the game itself wrote into a decoded log line.

    Returns a POSIX timestamp (local time) or None. Event times must come from the
    line, not from when the profiler happened to poll: the game buffers its log, so
    observation time lags the event by anything from one sample period to seconds,
    and every 'teleport at t=X' alignment inherits that error.
    """
    global _LOG_TS_RE
    if _LOG_TS_RE is None:
        import re as _re
        _LOG_TS_RE = _re.compile(r"(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})")
    m = _LOG_TS_RE.search(line)
    if not m:
        return None
    try:
        return time.mktime((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                            int(m.group(4)), int(m.group(5)), int(m.group(6)),
                            0, 0, -1))
    except Exception:
        return None


class GameLogMonitor:
    """Real-time event monitor for game engine logs and stage transitions.

    Emits two timestamps per event:
      log_ts    the time the GAME wrote in the line  -- use this to align phases
      ts        the time the profiler observed it    -- kept for provenance
    'lag_s' is the difference, so a run where the game's log buffering is unusually
    slow is visible rather than silently shifting every phase boundary.
    """
    DEFAULT_LOG_PATHS = [
        "/media/gamedisk/Games/wwm/wwm_standard/LocalData/game_account.log",
        os.path.expanduser("~/steam-game.log"),
    ]

    def __init__(self, log_path=None):
        self.log_path = log_path
        self.file_pos = 0
        self.clock_offset = None
        if not self.log_path:
            for candidate in self.DEFAULT_LOG_PATHS:
                if os.path.exists(candidate):
                    self.log_path = candidate
                    break

        if self.log_path and os.path.exists(self.log_path):
            try: self.file_pos = os.path.getsize(self.log_path)
            except Exception: pass

    def _calibrate_offset(self, raw_log_ts, now_t):
        """Correct the log line's timezone, per line.

        This game's log is not written in a single timezone: the client's own lines use
        local time while the lines echoed from the server use the server's (UTC+8), so a
        session-wide offset calibrated from the first event mis-corrects every line of
        the other kind -- which is exactly what happened, leaving the teleport events
        with no usable timestamp at all.

        Real UTC offsets are whole multiples of 15 minutes and genuine logging lag is
        seconds, so the two cannot be confused: snap each line's apparent difference to
        the nearest quarter hour and keep the remainder as the lag.
        """
        quarter = 900.0
        diff = now_t - raw_log_ts
        offset = round(diff / quarter) * quarter
        if offset != self.clock_offset:
            self.clock_offset = offset
            if offset:
                print(f"[LOG] Log line clock is {offset/3600.0:+.2f}h from this host; correcting.")
                sys.stdout.flush()
        return raw_log_ts + offset

    def _classify(self, dec, now_t):
        raw_ts = parse_log_timestamp(dec)
        base = {"ts": now_t}
        if raw_ts is not None:
            log_ts = self._calibrate_offset(raw_ts, now_t)
            lag = now_t - log_ts
            # A corrected timestamp that still implies an implausible lag means the line
            # carried someone else's timestamp; fall back to observation time and say so.
            if -2.0 <= lag <= 120.0:
                base["log_ts"] = log_ts
                base["lag_s"] = round(lag, 3)
            else:
                base["log_ts_suspect"] = round(log_ts, 3)
                base["lag_s"] = None
        if "on_teleport_in" in dec:
            import re
            m = re.search(r"spaceno:\s*(\d+)", dec)
            return {**base, "type": "teleport_start",
                    "spaceno": m.group(1) if m else "unknown", "raw": dec}
        if "on_finish_setup_acsdk" in dec:
            return {**base, "type": "acsdk_ready", "raw": dec}
        if "enter GameState" in dec or "callback_create_lobby" in dec:
            return {**base, "type": "teleport_finish", "raw": dec}
        if "Start-Load-LoginWindow" in dec or "enter InitState" in dec:
            # NOT a failure. The client writes these when it builds the login window,
            # which happens at normal startup and again on a normal return to title.
            # Recorded as a neutral phase marker; do not read it as a disconnect.
            return {**base, "type": "login_window", "raw": dec}
        return None

    def poll_events(self):
        events = []
        if not self.log_path:
            return events
        try:
            curr_size = os.path.getsize(self.log_path)
        except Exception:
            return events
        try:
            if curr_size < self.file_pos:
                # Log rotated or truncated. Re-read from the start, but mark the
                # events as backfill so historical teleports are not mistaken for
                # things that just happened.
                self.file_pos = 0
                backfill = True
            else:
                backfill = False
            if curr_size > self.file_pos:
                with open(self.log_path, "rb") as f:
                    f.seek(self.file_pos)
                    raw = f.read(curr_size - self.file_pos)
                    self.file_pos = f.tell()
                now_t = time.time()
                for rline in raw.split(b"\n"):
                    if not rline:
                        continue
                    dec = decode_caesar(rline).strip()
                    ev = self._classify(dec, now_t)
                    if ev:
                        if backfill:
                            ev["backfill"] = True
                        events.append(ev)
        except Exception:
            pass
        return events


class CrashMonitor:
    """Detects crash signatures, missing DLLs, and NT status codes."""

    # wchan / kstack symbols that mean the process is dying, not waiting
    DYING_WCHANS = ("vfs_coredump", "do_coredump", "coredump_wait", "do_exit", "exit_mm")

    @staticmethod
    def classify_sample(threads):
        """Classify one thread snapshot as healthy / dying / stalled.

        A run that ends in a core dump looks nothing like a clean exit in the sampled
        data: every thread moves to do_exit (state I) in a single sample while the
        main thread sits in vfs_coredump for as long as the dump takes. Relying on a
        dmesg grep alone misses this entirely -- and misses it at exactly the moment
        you most want to know, because a multi-GB core dump can still be running when
        the exit record is written.
        """
        if not threads:
            return "unknown"
        dying = sum(1 for t in threads
                    if (t.get("wchan") or "") in CrashMonitor.DYING_WCHANS
                    or (t.get("kstack") or "") in CrashMonitor.DYING_WCHANS)
        if dying >= max(2, len(threads) // 2):
            return "dying"
        return "healthy"

    @staticmethod
    def inspect_exit_status(pid, proc_name, sampled_evidence=None):
        """Analyze crash indicators, combining sampled evidence with the kernel log.

        sampled_evidence is the dict accumulated by the record loop (see
        SessionManager.note_sample_health); it carries what the profiler actually
        observed, which is authoritative over dmesg heuristics.
        """
        analysis = {
            "crashed": False,
            "signature": None,
            "description": None,
            "recommendation": None,
            "evidence": None
        }

        ev = sampled_evidence or {}
        if ev.get("coredump_samples"):
            analysis["crashed"] = True
            analysis["signature"] = "CORE DUMP"
            analysis["description"] = (
                f"All threads entered exit/coredump at t={ev.get('death_t', -1):.2f}s "
                f"({ev.get('coredump_samples')} samples in vfs_coredump/do_exit, "
                f"~{ev.get('coredump_seconds', 0):.1f}s of dumping). "
                f"The process was healthy in the preceding sample."
            )
            analysis["recommendation"] = (
                "Inspect the core with coredumpctl/gdb. Trailing samples after the "
                "death point are dump time, not game time -- exclude them from any "
                "phase duration."
            )
            analysis["evidence"] = ev
        elif ev.get("stall_samples"):
            analysis["signature"] = "STALL"
            analysis["description"] = (
                f"{ev.get('stall_samples')} consecutive samples "
                f"(~{ev.get('stall_seconds', 0):.1f}s) with zero CPU ticks and zero "
                f"context switches across all {ev.get('stall_threads', 0)} threads, "
                f"starting at t={ev.get('stall_t', -1):.2f}s."
            )
            analysis["recommendation"] = (
                "Whole-process quiescence with live threads is a lost wakeup or a "
                "deadlock, not slowness. Capture the futex state at that point."
            )
            analysis["evidence"] = ev

        # Kernel log corroboration (may lag a long core dump, so never the sole source)
        try:
            out = subprocess.check_output(["dmesg", "-T"], stderr=subprocess.DEVNULL).decode()
            for line in reversed(out.splitlines()[-200:]):
                low = line.lower()
                if proc_name.lower()[:15] in low and any(
                        s in low for s in ("segfault", "general protection", "trap ", "traps:")):
                    analysis["crashed"] = True
                    if not analysis["signature"] or analysis["signature"] == "CORE DUMP":
                        analysis["signature"] = "SIGSEGV / Page Fault"
                    analysis["description"] = (analysis["description"] or "") + " | dmesg: " + line.strip()
                    analysis["recommendation"] = (
                        "Inspect memory mappings, DXVK/VKD3D version, or address space "
                        "limits (vm.max_map_count)."
                    )
                    break
        except Exception:
            pass
        return analysis


# ------------------------------------------------------------------------------
# 10. Real-Time JSON Stream & Session Manager
# ------------------------------------------------------------------------------
class JsonStreamWriter:
    """Low-latency JSON Lines (NDJSON) streaming engine with atomic line flushing."""
    def __init__(self, filepath):
        self.filepath = filepath
        os.makedirs(os.path.dirname(os.path.abspath(filepath)), exist_ok=True)
        self.fp = open(filepath, "w", encoding="utf-8", buffering=1)
        ensure_file_permissions(self.filepath)

    def write_record(self, record_dict):
        """Write a dictionary as a JSON line and flush immediately to disk."""
        try:
            line = json.dumps(record_dict, ensure_ascii=False)
            self.fp.write(line + "\n")
            self.fp.flush()
        except Exception as e:
            print(f"[STREAM ERROR] Failed to write record: {e}", file=sys.stderr)

    def close(self):
        """Flush and close stream cleanly."""
        try:
            if self.fp and not self.fp.closed:
                self.fp.flush()
                self.fp.close()
                ensure_file_permissions(self.filepath)
        except Exception:
            pass


# ------------------------------------------------------------------------------
# 10b. Linux eBPF / Bpftrace Kernel Telemetry Engine
# ------------------------------------------------------------------------------
class BpftraceEngine:
    """
    Manages dynamic Linux eBPF instrumentation via bpftrace for Proton / Wine games.
    Captures microsecond-level kernel synchronization latency, futex wait distributions,
    scheduler off-CPU times, and runqueue wake-to-schedule delays.
    """
    # --------------------------------------------------------------------------
    # Probe design notes (please read before editing a preset)
    #
    #  * Everything is keyed by TID, never by comm. Every thread in a Wine game
    #    shares one comm ("wwm.exe", "game.exe", ...), so keying by comm collapses
    #    the coordinator, the job workers, the asset workers and the render thread
    #    into a single bucket and destroys exactly the distinction you are after.
    #    Resolve tid -> role afterwards from the sampled thread table.
    #
    #  * Futex waits are split by RETURN VALUE, not just by duration. "How long did
    #    it wait" cannot distinguish a wait that was woken from one that timed out,
    #    and those imply completely different bugs:
    #        ret == 0     woken by FUTEX_WAKE          -> real handoff
    #        ret == -110  ETIMEDOUT                    -> nobody woke it: lost wakeup
    #                                                     or a poll loop
    #        ret == -11   EAGAIN                       -> value changed before sleep
    #        ret == -4    EINTR                        -> signal
    #
    #  * WAIT and WAKE are never mixed in one histogram. A FUTEX_WAKE call is ~1us;
    #    a FUTEX_WAIT can be seconds. Averaging them is meaningless.
    #
    #  * Plain "$x = ..." assignment only. Typed "let $x: uint64 = ..." requires a
    #    recent bpftrace and fails the whole script to a parse error on older ones.
    #
    #  * Every preset flushes on an interval. Maps that are only printed at exit are
    #    lost if the run ends in a crash, a core dump or a SIGKILL -- which is the
    #    run you most wanted the data from.
    # --------------------------------------------------------------------------

    _PROLOGUE = (
        "BEGIN {\n"
        "    printf(\"#profiler-bpftrace pid={pid} begin_ns=%llu\\n\", nsecs);\n"
        "}\n"
    )

    PRESETS = {
        # FUTEX_WAIT = 0, FUTEX_WAIT_BITSET = 9; op & 127 strips PRIVATE (128) and
        # CLOCK_REALTIME (256) so both waiting ops normalise onto 0 / 9.
        "futex": (
            "tracepoint:syscalls:sys_enter_futex /pid == {pid}/ {\n"
            "    $op = args.op & 127;\n"
            "    @futex_ops[tid, $op] = count();\n"
            "    if ($op == 0 || $op == 9) {\n"
            "        @futex_start[tid] = nsecs;\n"
            "    }\n"
            "}\n"
            "tracepoint:syscalls:sys_exit_futex /@futex_start[tid]/ {\n"
            "    $lat_us = (nsecs - @futex_start[tid]) / 1000;\n"
            "    @futex_wait_us[tid] = hist($lat_us);\n"
            "    @futex_wait_stats_us[tid] = stats($lat_us);\n"
            "    @futex_ret[tid, args.ret] = count();\n"
            "    if (args.ret == 0) {\n"
            "        @futex_woken_us[tid] = hist($lat_us);\n"
            "        @futex_woken_stats_us[tid] = stats($lat_us);\n"
            "    } else if (args.ret == -110) {\n"
            "        @futex_timeout_us[tid] = hist($lat_us);\n"
            "        @futex_timeout_stats_us[tid] = stats($lat_us);\n"
            "    } else {\n"
            "        @futex_eagain_us[tid] = hist($lat_us);\n"
            "        @futex_eagain_stats_us[tid] = stats($lat_us);\n"
            "    }\n"
            "    delete(@futex_start[tid]);\n"
            "}\n"
            "tracepoint:syscalls:sys_enter_futex_waitv /pid == {pid}/ {\n"
            "    @futexv_start[tid] = nsecs;\n"
            "}\n"
            "tracepoint:syscalls:sys_exit_futex_waitv /@futexv_start[tid]/ {\n"
            "    $lat_us = (nsecs - @futexv_start[tid]) / 1000;\n"
            "    @futexv_wait_us[tid] = hist($lat_us);\n"
            "    @futexv_ret[tid, args.ret] = count();\n"
            "    delete(@futexv_start[tid]);\n"
            "}\n"
        ),
        # Runqueue latency is the wake-to-run delay: how long after being made
        # runnable does the thread actually get a CPU. Off-CPU is the complement:
        # how long it was not runnable at all.
        "sched": (
            "tracepoint:sched:sched_wakeup /args.pid != 0/ {\n"
            "    if (@is_target[args.pid]) {\n"
            "        @runq_start[args.pid] = nsecs;\n"
            "        @wakeups[args.pid] = count();\n"
            "    }\n"
            "}\n"
            "tracepoint:sched:sched_wakeup_new /args.pid != 0/ {\n"
            "    if (@is_target[args.pid]) {\n"
            "        @runq_start[args.pid] = nsecs;\n"
            "    }\n"
            "}\n"
            "tracepoint:sched:sched_switch {\n"
            "    if (pid == {pid}) {\n"
            "        @is_target[tid] = 1;\n"
            "        if (args.prev_state == 0) {\n"
            "            @preempted[tid] = count();\n"
            "        } else {\n"
            "            @offcpu_start[tid] = nsecs;\n"
            "            @blocked[tid] = count();\n"
            "        }\n"
            "    }\n"
            "    if (@offcpu_start[args.next_pid]) {\n"
            "        $off_us = (nsecs - @offcpu_start[args.next_pid]) / 1000;\n"
            "        @offcpu_us[args.next_pid] = hist($off_us);\n"
            "        @offcpu_stats_us[args.next_pid] = stats($off_us);\n"
            "        delete(@offcpu_start[args.next_pid]);\n"
            "    }\n"
            "    if (@runq_start[args.next_pid]) {\n"
            "        $rq_us = (nsecs - @runq_start[args.next_pid]) / 1000;\n"
            "        @runq_us[args.next_pid] = hist($rq_us);\n"
            "        @runq_stats_us[args.next_pid] = stats($rq_us);\n"
            "        delete(@runq_start[args.next_pid]);\n"
            "    }\n"
            "}\n"
        ),
        # Syscall census only -- no per-syscall latency map. Timing every syscall of
        # a 200-thread game perturbs the workload more than it measures it. Use the
        # counts to find what to instrument, then write a targeted script.
        "syscalls": (
            "tracepoint:raw_syscalls:sys_enter /pid == {pid}/ {\n"
            "    @sys_counts[args.id] = count();\n"
            "}\n"
        ),
        # Where is the CPU actually going? Samples the USER stack of every thread at
        # 99 Hz. This is the preset to reach for once you know a thread is burning CPU
        # but not which function is doing it -- the futex/sched presets can prove a
        # thread is on-cpu, but not what it is executing there.
        # ntdll.so carries symbols; PE-side frames (ntdll.dll, the game) resolve only
        # to addresses, which is still enough to separate "inside the unix sync layer"
        # from "inside game code".
        "profile": (
            "profile:hz:99 /pid == {pid}/ {\n"
            "    @cpu_samples[tid] = count();\n"
            "    @leaf[tid, reg(\"ip\")] = count();\n"
            "}\n"
        ),
        # Full stacks. Far higher cardinality -- a 10s title-screen sample already
        # produces ~600 distinct stacks, and most hot frames in a Wine process are
        # PE addresses that ustack cannot name anyway. Use "profile" first; reach
        # for this only when you need the caller chain of an address you already
        # identified.
        "profile-stacks": (
            "profile:hz:99 /pid == {pid}/ {\n"
            "    @cpu_samples[tid] = count();\n"
            "    @ustack[tid, ustack(8)] = count();\n"
            "}\n"
        ),
        # ntsync only: filter on the /dev/ntsync fd recorded at startup, otherwise
        # this counts DRM/amdgpu ioctls (which dominate in any Vulkan title) and
        # labels them as synchronisation.
        "ntsync": (
            "tracepoint:syscalls:sys_enter_ioctl /pid == {pid} && {ntsync_filter}/ {\n"
            "    @ioctl_start[tid] = nsecs;\n"
            "    @ntsync_cmds[tid, args.cmd & 0xff] = count();\n"
            "}\n"
            "tracepoint:syscalls:sys_exit_ioctl /@ioctl_start[tid]/ {\n"
            "    $lat_us = (nsecs - @ioctl_start[tid]) / 1000;\n"
            "    @ntsync_latency_us[tid] = hist($lat_us);\n"
            "    @ntsync_stats_us[tid] = stats($lat_us);\n"
            "    delete(@ioctl_start[tid]);\n"
            "}\n"
        ),
    }
    # Derived composite presets
    PRESETS["sync"] = PRESETS["futex"] + "\n" + PRESETS["sched"]
    PRESETS["all"] = PRESETS["futex"] + "\n" + PRESETS["sched"] + "\n" + PRESETS["ntsync"]
    PRESETS["profile-stacks"] = PRESETS["profile-stacks"] + "\n" + PRESETS["syscalls"] + "\n" + PRESETS["sched"]

    # Periodic flush so a crashed / SIGKILLed run still yields data.
    _FLUSH_MAPS = (
        "@futex_wait_us", "@futex_woken_us", "@futex_timeout_us", "@futex_eagain_us",
        "@futex_wait_stats_us", "@futex_woken_stats_us", "@futex_timeout_stats_us",
        "@futex_eagain_stats_us",
        "@futex_ret", "@futex_ops", "@futexv_wait_us", "@futexv_ret",
        "@offcpu_us", "@offcpu_stats_us", "@runq_us", "@runq_stats_us",
        "@wakeups", "@preempted", "@blocked",
        "@ntsync_latency_us", "@ntsync_stats_us", "@ntsync_cmds", "@sys_counts",
        "@cpu_samples", "@leaf", "@ustack",
        # @ustack is flushed on the interval ON PURPOSE, despite the output size:
        # bpftrace symbolizes user frames from /proc/<pid>/maps at PRINT time, so a
        # stack map printed only at exit -- after the target has died -- resolves to
        # nothing but addresses. Printing while the process lives is what makes the
        # frames readable. The parser merges the repeats.
    )

    def __init__(self, preset="sync", script_file=None, eval_str=None, flush_secs=10):
        self.preset = preset or "sync"
        self.script_file = script_file
        self.eval_str = eval_str
        self.flush_secs = max(0, int(flush_secs or 0))
        self.process = None
        self.pid = None
        self.output_path = None
        self.stderr_tail = []
        self._err_thread = None
        self.failed_reason = None

    @staticmethod
    def find_ntsync_fd(pid):
        """Locate the process's /dev/ntsync descriptor, so ioctl probes can filter
        on it instead of counting every DRM ioctl in the process."""
        try:
            for fd in os.listdir(f"/proc/{pid}/fd"):
                try:
                    if os.readlink(f"/proc/{pid}/fd/{fd}").endswith("/ntsync"):
                        return int(fd)
                except Exception:
                    continue
        except Exception:
            pass
        return None

    @staticmethod
    def is_available():
        """Check if bpftrace binary is present in system PATH."""
        return shutil.which("bpftrace") is not None

    def _substitute(self, text, pid):
        """Fill in the target PID and the ntsync ioctl filter."""
        fd = self.find_ntsync_fd(pid)
        if fd is not None:
            ntsync_filter = f"args.fd == {fd}"
        else:
            # Fall back to the ioctl type byte: ntsync uses magic 'N' (0x4e).
            ntsync_filter = "((args.cmd >> 8) & 0xff) == 0x4e"
        return (text.replace("{pid}", str(pid))
                    .replace("{ntsync_filter}", ntsync_filter))

    def _flush_clause(self, body_text):
        """Periodic print+clear so a run that ends badly still produces data, plus an
        END clause that drops the per-tid scratch maps (bpftrace prints every map at
        exit; without this the JSON is mostly in-flight timestamps).

        Only maps that the selected preset actually populates are referenced --
        print()/clear() on a map the script never assigns is a compile error, which
        would take the whole script down.
        """
        scratch = ("@futex_start", "@futexv_start",
                   "@offcpu_start", "@runq_start", "@ioctl_start", "@is_target")
        present = [m for m in self._FLUSH_MAPS if f"{m}[" in body_text or f"{m} =" in body_text]
        out = ""
        if self.flush_secs and present:
            # Stamp every flush with the monotonic clock. Without this the maps are
            # whole-run aggregates and cannot be restricted to, say, the settle phase --
            # which makes them incomparable between runs whose phases differ in length.
            body = '    printf("#flush %llu\\n", nsecs);\n'
            body += "".join(f"    print({m});\n    clear({m});\n" for m in present)
            out += f"interval:s:{self.flush_secs} {{\n{body}}}\n"
        present_scratch = [m for m in scratch if f"{m}[" in body_text]
        if present_scratch:
            out += "END {\n" + "".join(f"    clear({m});\n" for m in present_scratch) + "}\n"
        return out

    def _build_script(self, pid):
        """Construct the bpftrace program with target PID substituted."""
        if self.script_file:
            if not os.path.exists(self.script_file):
                print(f"[BPFTRACE ERROR] Script file not found: {self.script_file}", file=sys.stderr)
                return None
            try:
                with open(self.script_file, "r", encoding="utf-8") as f:
                    content = f.read()
                return self._substitute(content, pid)
            except Exception as e:
                print(f"[BPFTRACE ERROR] Failed to read script file {self.script_file}: {e}", file=sys.stderr)
                return None
        elif self.eval_str:
            return self._substitute(self.eval_str, pid)
        else:
            # Never silently substitute a different preset: a typo used to fall back to
            # "sync" and produce a plausible-looking capture of the wrong thing.
            if self.preset not in self.PRESETS:
                print(f"[BPFTRACE ERROR] Unknown preset '{self.preset}'. "
                      f"Available: {', '.join(sorted(self.PRESETS))}", file=sys.stderr)
                return None
            tpl = self.PRESETS[self.preset]
            return self._substitute(self._PROLOGUE + tpl + self._flush_clause(tpl), pid)

    def start(self, pid, output_path):
        """Start background bpftrace instrumentation attached to target PID."""
        if not self.is_available():
            print("[WARN] bpftrace binary not found in PATH. eBPF kernel tracing disabled.", file=sys.stderr)
            return False

        self.pid = pid
        self.output_path = output_path
        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)

        script = self._build_script(pid)
        if not script:
            return False

        # Per-tid maps over a long run will blow past the 4096-key default and drop
        # keys silently; raise it explicitly rather than losing threads.
        env = dict(os.environ)
        env.setdefault("BPFTRACE_MAP_KEYS_MAX", "65536")
        env.setdefault("BPFTRACE_MAX_MAP_KEYS", "65536")  # older releases
        # Keep resolved user symbols even after the target exits (name differs by
        # release; setting both is harmless on the one that ignores it).
        env.setdefault("BPFTRACE_CACHE_USER_SYMBOLS", "1")
        env.setdefault("BPFTRACE_PERF_RB_PAGES", "128")

        self.script_text = script
        cmd = ["bpftrace", "-f", "json", "-o", output_path, "-e", script]
        try:
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True,
                env=env
            )
        except Exception as e:
            self.failed_reason = str(e)
            print(f"[BPFTRACE ERROR] Failed to start bpftrace: {e}", file=sys.stderr)
            self.process = None
            return False

        # Drain stderr on a thread. Previously it was piped and never read: bpftrace's
        # own error message was discarded even on the failure path, and a verbose
        # attach could fill the 64 KiB pipe buffer and block bpftrace mid-run.
        import threading
        def _drain(fp, sink):
            try:
                for line in fp:
                    sink.append(line.rstrip())
                    if len(sink) > 200:
                        del sink[:100]
            except Exception:
                pass
        self._err_thread = threading.Thread(
            target=_drain, args=(self.process.stderr, self.stderr_tail), daemon=True)
        self._err_thread.start()

        # bpftrace fails fast on a parse error or a missing tracepoint. Give it a
        # moment and confirm it is actually alive before claiming probes are attached.
        time.sleep(0.6)
        rc = self.process.poll()
        if rc is not None:
            self.failed_reason = "\n".join(self.stderr_tail[-12:]) or f"exit code {rc}"
            print(f"[BPFTRACE ERROR] bpftrace exited immediately (rc={rc}).", file=sys.stderr)
            for line in self.stderr_tail[-12:]:
                print(f"    | {line}", file=sys.stderr)
            self.process = None
            return False

        print(f"[BPFTRACE START] Attached eBPF probes ({self.preset}) to PID {pid} -> {output_path}")
        sys.stdout.flush()
        return True

    def poll(self):
        """Check if bpftrace process is still running."""
        return self.process.poll() if self.process else -1

    # Override with PROFILER_BPFTRACE_MAX_MB. A healthy census run stays well under 1 GB.
    MEMORY_LIMIT_MB = int(os.environ.get("PROFILER_BPFTRACE_MAX_MB", "4096"))

    def _tracer_memory_mb(self):
        """Resident + swapped-out memory of the bpftrace process, in MB (None if unreadable)."""
        status = read_file_safe(f"/proc/{self.process.pid}/status")
        if not status:
            return None
        kb = 0
        for line in status.splitlines():
            if line.startswith(("VmRSS:", "VmSwap:")):
                kb += int(line.split()[1])
        return kb // 1024

    def check_alive(self):
        """Report (and latch) unexpected bpftrace death, or kill it if it runs away on
        memory, so a long recording does not silently produce an empty eBPF file."""
        if not self.process:
            return False
        rc = self.process.poll()
        if rc is None:
            # Memory watchdog: a runaway bpftrace once grew to ~75 GB (RAM + swap) and
            # thrashed the whole machine, silently invalidating the run it was
            # measuring. Kill it well before it can do that.
            mem_mb = self._tracer_memory_mb()
            if mem_mb is None or mem_mb <= self.MEMORY_LIMIT_MB:
                return True
            self.failed_reason = (f"bpftrace used {mem_mb} MB (RSS + swap), over the "
                                  f"{self.MEMORY_LIMIT_MB} MB watchdog limit; killed")
            print(f"[BPFTRACE ERROR] {self.failed_reason}. eBPF data stops here.",
                  file=sys.stderr)
            pid = self.process.pid
            self.process.kill()
            try:
                self.process.wait(timeout=10.0)
            except subprocess.TimeoutExpired:
                print(f"[BPFTRACE ERROR] bpftrace (PID {pid}) did not exit 10 s after SIGKILL; "
                      f"kill it manually before the next run.", file=sys.stderr)
            self.process = None
            return False
        self.failed_reason = "\n".join(self.stderr_tail[-12:]) or f"exit code {rc}"
        print(f"[BPFTRACE ERROR] bpftrace died mid-run (rc={rc}). eBPF data stops here.",
              file=sys.stderr)
        for line in self.stderr_tail[-12:]:
            print(f"    | {line}", file=sys.stderr)
        self.process = None
        return False

    def stop(self):
        """Cleanly terminate bpftrace, detach probes, and parse output maps."""
        if not self.process:
            # Still try to parse: an interval flush may have written data before death.
            if self.output_path and os.path.exists(self.output_path):
                ensure_file_permissions(self.output_path)
                return self.parse_output(self.output_path)
            return None
        # SIGINT lets bpftrace run END and detach cleanly. If it ignores SIGINT and
        # SIGTERM (seen when it was stuck paging in swap), SIGKILL it: a tracer left
        # behind keeps its probes attached and its memory, and poisons the next run.
        pid = self.process.pid
        stopped = False
        for sig, grace in ((signal.SIGINT, 10.0), (signal.SIGTERM, 5.0), (signal.SIGKILL, 10.0)):
            if self.process.poll() is not None:
                stopped = True
                break
            try:
                self.process.send_signal(sig)
            except ProcessLookupError:
                stopped = True
                break
            try:
                self.process.wait(timeout=grace)
                stopped = True
                break
            except subprocess.TimeoutExpired:
                print(f"[BPFTRACE WARN] bpftrace (PID {pid}) still running {grace:.0f} s after "
                      f"{signal.Signals(sig).name}", file=sys.stderr)
        self.process = None
        if not stopped:
            print(f"[BPFTRACE ERROR] bpftrace (PID {pid}) did not exit even after SIGKILL; "
                  f"kill it manually before the next run.", file=sys.stderr)

        if self._err_thread:
            self._err_thread.join(timeout=1.0)
            self._err_thread = None

        if self.output_path:
            ensure_file_permissions(self.output_path)
            summary = self.parse_output(self.output_path)
            if summary is not None:
                summary["stderr_tail"] = self.stderr_tail[-12:]
            if stopped:
                print(f"[BPFTRACE STOP] eBPF probes detached. Summary extracted from {self.output_path}")
            else:
                print(f"[BPFTRACE STOP] Partial summary extracted from {self.output_path} "
                      f"(bpftrace PID {pid} is still alive)")
            if summary is not None and not summary.get("attached_probes"):
                print("[BPFTRACE WARN] No attached-probe record found -- verify the JSON "
                      "shape matches this bpftrace version.", file=sys.stderr)
            sys.stdout.flush()
            return summary
        return None

    @classmethod
    def parse_output(cls, filepath):
        """Parse NDJSON output generated by bpftrace -f json.

        Routing is by MAP NAME, not by the record's "type" string: the type strings
        differ between bpftrace releases, and the old parser silently produced an
        empty summary whenever they did not match -- indistinguishable from "the
        trace captured nothing".

        Maps are MERGED across records because the script prints on an interval, so
        the same map appears many times over a run; histograms sum bucket-wise and
        stats sum count/total with the average recomputed.
        """
        if not os.path.exists(filepath):
            return None

        summary = {
            "attached_probes": 0,
            "futex": {"stats": {}, "woken_stats": {}, "timeout_stats": {}, "eagain_stats": {},
                      "hist": {}, "woken_hist": {}, "timeout_hist": {}, "eagain_hist": {},
                      "ret": {}, "ops": {}},
            "sched": {"offcpu_stats": {}, "offcpu_hist": {}, "runq_stats": {},
                      "runq_hist": {}, "wakeups": {}, "preempted": {}, "blocked": {}},
            "ntsync": {"stats": {}, "hist": {}, "cmds": {}},
            "syscalls": {"counts": {}},
            "profile": {"samples": {}, "stacks": {}, "leaf": {}},
            "raw_maps": {},
            "flushes": 0,
        }

        ROUTE = {
            "@futex_wait_stats_us":    ("futex", "stats"),
            "@futex_woken_stats_us":   ("futex", "woken_stats"),
            "@futex_timeout_stats_us": ("futex", "timeout_stats"),
            "@futex_eagain_stats_us":  ("futex", "eagain_stats"),
            "@futex_wait_us":          ("futex", "hist"),
            "@futex_woken_us":         ("futex", "woken_hist"),
            "@futex_timeout_us":       ("futex", "timeout_hist"),
            "@futex_eagain_us":        ("futex", "eagain_hist"),
            "@futex_ret":              ("futex", "ret"),
            "@futex_ops":              ("futex", "ops"),
            "@futexv_wait_us":         ("futex", "hist"),
            "@futexv_ret":             ("futex", "ret"),
            "@offcpu_stats_us":        ("sched", "offcpu_stats"),
            "@offcpu_us":              ("sched", "offcpu_hist"),
            "@runq_stats_us":          ("sched", "runq_stats"),
            "@runq_us":                ("sched", "runq_hist"),
            "@wakeups":                ("sched", "wakeups"),
            "@preempted":              ("sched", "preempted"),
            "@blocked":                ("sched", "blocked"),
            "@ntsync_stats_us":        ("ntsync", "stats"),
            "@ntsync_latency_us":      ("ntsync", "hist"),
            "@ntsync_cmds":            ("ntsync", "cmds"),
            "@sys_counts":             ("syscalls", "counts"),
            "@cpu_samples":            ("profile", "samples"),
            "@ustack":                 ("profile", "stacks"),
            "@leaf":                   ("profile", "leaf"),
        }

        def merge_hist(dst, key, buckets):
            cur = dst.setdefault(key, {})
            if not isinstance(buckets, list):
                return
            for b in buckets:
                if not isinstance(b, dict):
                    continue
                label = f"{b.get('min', b.get('lo', 0))}-{b.get('max', b.get('hi', 0))}"
                cur[label] = cur.get(label, 0) + int(b.get("count", 0) or 0)

        def merge_stats(dst, key, st):
            if not isinstance(st, dict):
                return
            cur = dst.setdefault(key, {"count": 0, "total": 0, "min": None, "max": None})
            cur["count"] += int(st.get("count", 0) or 0)
            cur["total"] += int(st.get("total", st.get("sum", 0)) or 0)
            for f, fn in (("min", min), ("max", max)):
                v = st.get(f)
                if v is not None:
                    cur[f] = v if cur[f] is None else fn(cur[f], v)
            cur["average"] = (cur["total"] / cur["count"]) if cur["count"] else 0

        def merge_scalar(dst, key, val):
            try:
                dst[key] = dst.get(key, 0) + int(val)
            except Exception:
                dst[key] = val

        def absorb(bucket, payload):
            """payload is {key: value}; value may be a hist list, a stats dict,
            a scalar, or a nested dict for multi-key maps."""
            if not isinstance(payload, dict):
                return
            for k, v in payload.items():
                if isinstance(v, list):
                    merge_hist(bucket, str(k), v)
                elif isinstance(v, dict):
                    if any(f in v for f in ("count", "average", "total", "sum")):
                        merge_stats(bucket, str(k), v)
                    else:
                        # multi-key map: flatten "outer,inner"
                        for k2, v2 in v.items():
                            if isinstance(v2, list):
                                merge_hist(bucket, f"{k},{k2}", v2)
                            elif isinstance(v2, dict):
                                merge_stats(bucket, f"{k},{k2}", v2)
                            else:
                                merge_scalar(bucket, f"{k},{k2}", v2)
                else:
                    merge_scalar(bucket, str(k), v)

        try:
            with open(filepath, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line or not line.startswith("{"):
                        continue
                    try:
                        obj = json.loads(line)
                    except Exception:
                        continue

                    t = obj.get("type")
                    data = obj.get("data", obj)

                    if t == "attached_probes":
                        n = 0
                        if isinstance(data, dict):
                            n = data.get("probes", data.get("count", 0))
                        summary["attached_probes"] = n or obj.get("count", 0) or 0
                        continue
                    if t in ("printf", "time", "cat", "join"):
                        continue
                    if not isinstance(data, dict):
                        continue

                    for map_name, payload in data.items():
                        if not isinstance(map_name, str) or not map_name.startswith("@"):
                            continue
                        route = ROUTE.get(map_name)
                        if route:
                            absorb(summary[route[0]][route[1]], payload)
                        else:
                            summary["raw_maps"].setdefault(map_name, {})
                            absorb(summary["raw_maps"][map_name], payload)
                    summary["flushes"] += 1
        except Exception as e:
            print(f"[BPFTRACE PARSE ERROR] Failed to parse {filepath}: {e}", file=sys.stderr)
            return None

        return summary

    @staticmethod
    def _fmt_us(v):
        if not v:
            return "n/a"
        return f"{v:.1f} us" if v < 1000 else (f"{v/1000.0:.2f} ms" if v < 1e6 else f"{v/1e6:.2f} s")

    @classmethod
    def format_summary(cls, summary, thread_roles=None):
        """Format parsed bpftrace metrics into human-readable diagnostic tables.

        thread_roles optionally maps "<tid>" -> label so the tables read as
        "coordinator" / "job-worker-3" instead of bare tids.
        """
        if not summary:
            return ""
        roles = thread_roles or {}

        def label(key):
            tid = str(key).split(",")[0]
            return f"{tid} {roles[tid]}" if tid in roles else str(key)

        out = []
        def p(text=""): out.append(text)

        p("==========================================================================================")
        p("   EBPF KERNEL SYNCHRONIZATION & SCHEDULER TELEMETRY (bpftrace)")
        p("==========================================================================================")
        p(f"  probes attached: {summary.get('attached_probes', 0)}   map flushes parsed: {summary.get('flushes', 0)}")
        for line in (summary.get("stderr_tail") or [])[-3:]:
            p(f"  bpftrace: {line}")

        futex = summary.get("futex", {})
        f_stats = futex.get("stats", {})
        woken = futex.get("woken_stats", {})
        timed = futex.get("timeout_stats", {})

        eag = futex.get("eagain_stats", {})

        if f_stats:
            p("")
            p("--- FUTEX WAIT OUTCOMES PER THREAD ---")
            p("  WOKEN   (ret 0)     slept, then someone signalled it. AVG WOKEN is how long")
            p("                      it slept; subtract RUNQ below to get the part that was")
            p("                      the producer being slow rather than the scheduler.")
            p("  IMMED   (ret EAGAIN) the futex value had already changed when the syscall ran,")
            p("                      so it never slept. A wasted syscall, not a lost wakeup.")
            p("  TIMEOUT (ret ETIMEDOUT) nobody ever signalled it. This is the lost-wakeup /")
            p("                      poll-loop signal, and only this one.")
            p("  Sorted by wait count -- idle threads that sat in one multi-second wait are")
            p("  uninteresting no matter how much total blocked time they accumulate.")
            p("")
            p(f"  {'THREAD':22s} | {'WAITS':>9s} | {'WOKEN':>9s} | {'IMMED':>9s} | {'TIMEOUT':>8s} | {'TMO%':>5s} | {'AVG WOKEN':>10s} | {'AVG IMMED':>10s}")
            p("  " + "-"*22 + "-+-----------+-----------+-----------+----------+-------+------------+-----------")
            def waits_of(key):
                return (woken.get(key, {}).get("count", 0)
                        + timed.get(key, {}).get("count", 0)
                        + eag.get(key, {}).get("count", 0))
            rows = [k for k in f_stats if waits_of(k) >= 50] or list(f_stats)
            for key in sorted(rows, key=lambda k: -waits_of(k))[:20]:
                w, to, ea = woken.get(key, {}), timed.get(key, {}), eag.get(key, {})
                wc, tc, ec = w.get("count", 0), to.get("count", 0), ea.get("count", 0)
                tot = wc + tc + ec
                p(f"  {label(key)[:22]:22s} | {tot:9d} | {wc:9d} | {ec:9d} | {tc:8d} | "
                  f"{(100.0*tc/tot if tot else 0):5.1f} | {cls._fmt_us(w.get('average')):>10s} | "
                  f"{cls._fmt_us(ea.get('average')):>10s}")

        f_ret = futex.get("ret", {})
        if f_ret:
            errno_names = {"0": "woken", "-110": "ETIMEDOUT (never signalled)",
                           "-11": "EAGAIN (value moved, no sleep)", "-4": "EINTR",
                           "-22": "EINVAL", "-512": "ERESTARTSYS", "-516": "ERESTART_RESTARTBLOCK"}
            agg = {}
            for k, v in f_ret.items():
                ret = str(k).split(",")[-1]
                agg[ret] = agg.get(ret, 0) + (v if isinstance(v, int) else 0)
            total = sum(agg.values()) or 1
            p("")
            p("--- FUTEX WAIT RETURN CODES (all threads) ---")
            for ret, cnt in sorted(agg.items(), key=lambda x: -x[1])[:8]:
                p(f"  - {errno_names.get(ret, 'ret='+ret):12s}: {cnt:9d} ({100.0*cnt/total:5.1f}%)")

        sched = summary.get("sched", {})
        runq = sched.get("runq_stats", {})
        off = sched.get("offcpu_stats", {})
        if runq or off:
            p("")
            p("--- WAKE-TO-RUN (RUNQUEUE) AND OFF-CPU LATENCY (per thread) ---")
            p("  runq = time between being made runnable and actually getting a CPU.")
            p("  This is the number people usually estimate from context-switch rates.")
            p("")
            p(f"  {'THREAD':22s} | {'WAKEUPS':>9s} | {'AVG RUNQ':>10s} | {'TOT RUNQ':>10s} | {'AVG OFFCPU':>11s} | {'TOTAL OFFCPU':>12s}")
            p("  " + "-"*22 + "-+-----------+------------+------------+-------------+-------------")
            keys = sorted(set(list(runq.keys()) + list(off.keys())),
                          key=lambda k: -(runq.get(k, {}).get("count", 0)))
            for key in keys[:20]:
                r = runq.get(key, {})
                o = off.get(key, {})
                p(f"  {label(key)[:22]:22s} | {r.get('count', 0):9d} | "
                  f"{cls._fmt_us(r.get('average')):>10s} | {cls._fmt_us(r.get('total')):>10s} | "
                  f"{cls._fmt_us(o.get('average')):>11s} | {cls._fmt_us(o.get('total')):>12s}")

        f_ops = futex.get("ops", {})
        if f_ops:
            op_names = {0: "FUTEX_WAIT", 1: "FUTEX_WAKE", 2: "FUTEX_FD", 3: "FUTEX_REQUEUE",
                        4: "FUTEX_CMP_REQUEUE", 5: "FUTEX_WAKE_OP", 6: "FUTEX_LOCK_PI",
                        7: "FUTEX_UNLOCK_PI", 8: "FUTEX_TRYLOCK_PI", 9: "FUTEX_WAIT_BITSET",
                        10: "FUTEX_WAKE_BITSET", 11: "FUTEX_WAIT_REQUEUE_PI"}
            agg = {}
            for k, v in f_ops.items():
                op = str(k).split(",")[-1]
                try:
                    op = int(op)
                except ValueError:
                    continue
                agg[op] = agg.get(op, 0) + (v if isinstance(v, int) else 0)
            total = sum(agg.values()) or 1
            p("")
            p("--- FUTEX OPERATION MIX ---")
            for op, cnt in sorted(agg.items(), key=lambda x: -x[1])[:8]:
                p(f"  - {op_names.get(op, f'OP_{op}'):22s}: {cnt:9d} ({100.0*cnt/total:5.1f}%)")

        ntsync = summary.get("ntsync", {})
        nt_stats = ntsync.get("stats", {})
        if nt_stats:
            p("")
            p("--- NTSYNC IOCTL LATENCY (per thread) ---")
            p(f"  {'THREAD':22s} | {'CALLS':>8s} | {'AVG':>10s} | {'TOTAL':>12s}")
            p("  " + "-"*22 + "-+----------+------------+-------------")
            for key, st in sorted(nt_stats.items(), key=lambda x: -x[1].get("total", 0))[:15]:
                p(f"  {label(key)[:22]:22s} | {st.get('count', 0):8d} | "
                  f"{cls._fmt_us(st.get('average')):>10s} | {cls._fmt_us(st.get('total')):>12s}")

        prof = summary.get("profile", {})
        psamp = prof.get("samples", {})
        leaf = prof.get("leaf", {})
        maps = summary.get("_memory_maps") or []
        if psamp and leaf:
            total = sum(v for v in psamp.values() if isinstance(v, int)) or 1
            p("")
            p("--- CPU PROFILE: HOTTEST INSTRUCTION ADDRESSES (99 Hz sampling) ---")
            p("  Wine's builtin PE modules cannot be symbolized by the kernel unwinder,")
            p("  so addresses are attributed to <module>+<offset> using the process's")
            p("  own memory map, captured while it was running. Feed an offset to")
            p("  objdump/winedump on that module to name the function:")
            p("      objdump -d --start-address=0x<off> --stop-address=0x<off+40> <module>")
            hot = sorted(((v, k) for k, v in psamp.items() if isinstance(v, int)), reverse=True)[:6]
            for cnt, tid in hot:
                p("")
                p(f"  {label(tid)}  --  {cnt} samples ({100.0*cnt/total:.1f}% of on-cpu time)")
                mine = [(c, str(k).split(",", 1)[1]) for k, c in leaf.items()
                        if isinstance(c, int) and str(k).split(",")[0] == str(tid) and "," in str(k)]
                mine.sort(reverse=True)
                sub = sum(c for c, _ in mine) or 1
                # Group adjacent addresses: a hot loop shows up as a tight cluster of
                # ips within a few dozen bytes, which is far more informative than any
                # single address in it.
                buckets = {}
                for c, addr in mine:
                    try:
                        a = int(addr)
                    except Exception:
                        continue
                    key = a & ~0x3F
                    e = buckets.setdefault(key, [0, a, a])
                    e[0] += c
                    e[1] = min(e[1], a)
                    e[2] = max(e[2], a)
                for key, (c, lo, hi) in sorted(buckets.items(), key=lambda kv: -kv[1][0])[:6]:
                    span = f"+0x{hi-lo:x}" if hi > lo else ""
                    p(f"      {100.0*c/sub:5.1f}%  {resolve_addr(lo, maps)}{span}  ({c} samples)")

        if psamp and prof.get("stacks"):
            total = sum(v for v in psamp.values() if isinstance(v, int)) or 1
            p("")
            p("--- CPU PROFILE: WHERE ON-CPU TIME IS SPENT (99 Hz user-stack sampling) ---")
            p("  Each sample is one thread caught executing. The stack is the top 8 user")
            p("  frames; symbols/modules are resolved via process memory maps.")
            hot = sorted(((v, k) for k, v in psamp.items() if isinstance(v, int)), reverse=True)[:6]
            stacks = prof.get("stacks", {})
            for cnt, tid in hot:
                p("")
                p(f"  {label(tid)}  --  {cnt} samples ({100.0*cnt/total:.1f}% of all on-cpu time)")
                mine = [(c, k) for k, c in stacks.items()
                        if isinstance(c, int) and str(k).split(",")[0] == str(tid)]
                mine.sort(reverse=True)
                sub = sum(c for c, _ in mine) or 1
                for c, k in mine[:6]:
                    frames_raw = str(k).split(",", 1)[1] if "," in str(k) else str(k)
                    resolved_frames = []
                    for f in frames_raw.strip().splitlines():
                        f_clean = f.strip()
                        if not f_clean:
                            continue
                        cand = f_clean[2:] if (f_clean.startswith("0x") or f_clean.startswith("0X")) else f_clean
                        if maps and cand and len(cand) >= 6 and all(ch in "0123456789abcdefABCDEF" for ch in cand):
                            try:
                                resolved_frames.append(resolve_addr(int(cand, 16), maps))
                            except Exception:
                                resolved_frames.append(f_clean)
                        else:
                            resolved_frames.append(f_clean)
                    frames_str = " <- ".join(resolved_frames)
                    p(f"      {100.0*c/sub:5.1f}%  {frames_str[:220]}")

        sys_counts = summary.get("syscalls", {}).get("counts", {})
        if sys_counts:
            # Common x86_64 Linux syscall names for diagnostic readability
            X86_SYSCALL_MAP = {
                0: "read", 1: "write", 2: "open", 3: "close", 4: "stat", 5: "fstat", 7: "poll",
                8: "lseek", 9: "mmap", 10: "mprotect", 11: "munmap", 12: "brk", 13: "rt_sigaction",
                14: "rt_sigprocmask", 16: "ioctl", 20: "writev", 23: "select", 24: "sched_yield",
                28: "madvise", 35: "nanosleep", 41: "socket", 42: "connect", 44: "sendto",
                45: "recvfrom", 47: "recvmsg", 56: "clone", 60: "exit", 61: "wait4", 72: "fcntl",
                186: "gettid", 202: "futex", 228: "clock_gettime", 230: "clock_nanosleep",
                232: "epoll_wait", 233: "epoll_ctl", 270: "pselect6", 271: "ppoll", 281: "epoll_pwait",
                284: "eventfd", 290: "eventfd2", 291: "epoll_create1", 293: "pipe2", 449: "futex_waitv"
            }
            p("")
            p("--- TOP SYSCALLS BY COUNT ---")
            total = sum(v for v in sys_counts.values() if isinstance(v, int)) or 1
            for sid, cnt in sorted(sys_counts.items(), key=lambda x: -(x[1] if isinstance(x[1], int) else 0))[:12]:
                sname = X86_SYSCALL_MAP.get(int(sid), "") if str(sid).isdigit() else ""
                label_str = f"#{sid} ({sname})" if sname else f"#{sid}"
                p(f"  - syscall {label_str:<24s}: {cnt:9d} ({100.0*cnt/total:5.1f}%)")

        return "\n".join(out)




class SessionManager:
    """Manages telemetry session file lifecycle, process exit detection, and rotation."""
    def __init__(self, output_dir=DEFAULT_TELEMETRY_DIR, bpftrace_opts=None):
        self.output_dir = output_dir
        os.makedirs(self.output_dir, exist_ok=True)
        self.active_writer = None
        self.active_session_path = None
        self.active_pid = None
        self.active_proc_name = None
        self.start_time = None
        self.sample_count = 0
        self.bpftrace_opts = bpftrace_opts or {}
        self.bpftrace_engine = None
        self.active_bpftrace_path = None
        self.health = {}
        self._last_state = "healthy"
        self._quiet_run = 0
        self._quiet_started = None
        self.thread_roles = {}

    def start_session(self, pid, proc_name):
        """Initialize a new recording session and JSON stream file."""
        self.close_session(reason="rotation")
        self.active_pid = pid
        self.active_proc_name = proc_name
        self.start_time = time.time()
        self.sample_count = 0
        self.health = {}
        self._last_state = "healthy"
        self._quiet_run = 0
        self._quiet_started = None
        self.thread_roles = {}

        ts_str = datetime.now().strftime("%Y%m%d_%H%M%S")
        safe_name = "".join(c if c.isalnum() or c in "._-" else "_" for c in proc_name)
        filename = f"{safe_name}_{pid}_{ts_str}.jsonl"
        self.active_session_path = os.path.join(self.output_dir, filename)

        self.active_writer = JsonStreamWriter(self.active_session_path)

        # Update 'latest.jsonl' symlink in working directory
        latest_link = os.path.join(self.output_dir, "latest.jsonl")
        try:
            if os.path.lexists(latest_link):
                os.remove(latest_link)
            os.symlink(os.path.basename(self.active_session_path), latest_link)
        except Exception:
            pass

        # Start dynamic eBPF kernel tracing if enabled
        if self.bpftrace_opts.get("enabled"):
            preset = self.bpftrace_opts.get("preset", "sync")
            script_file = self.bpftrace_opts.get("script")
            eval_str = self.bpftrace_opts.get("eval")
            custom_out = self.bpftrace_opts.get("output")
            self.active_bpftrace_path = custom_out or (os.path.splitext(self.active_session_path)[0] + "_bpftrace.json")
            self.bpftrace_engine = BpftraceEngine(
                preset=preset, script_file=script_file, eval_str=eval_str,
                flush_secs=self.bpftrace_opts.get("flush_secs", 10))
            if not self.bpftrace_engine.start(pid, self.active_bpftrace_path):
                # Do not pretend eBPF is running when it is not.
                print("[BPFTRACE] Continuing with /proc sampling only.", file=sys.stderr)
                self.bpftrace_engine = None
                self.active_bpftrace_path = None

        # Write stream header
        self.active_writer.write_record({
            "event": "stream_start",
            "profiler_version": PROFILER_VERSION,
            "session_start_ts": self.start_time,
            "pid": pid,
            "proc_name": proc_name,
            "host": get_host_system_info(),
            "sysctl": get_sysctl_settings(),
            "bpftrace_active": self.bpftrace_engine is not None,
            "bpftrace_preset": self.bpftrace_opts.get("preset") if self.bpftrace_opts.get("enabled") else None
        })

        # Write initial launch diagnostics
        cwd = None
        exe_target = None
        try: cwd = os.readlink(f"/proc/{pid}/cwd")
        except Exception: pass
        try: exe_target = os.readlink(f"/proc/{pid}/exe")
        except Exception: pass

        self.active_writer.write_record({
            "event": "launch_diagnostics",
            "pid": pid,
            "proc_name": proc_name,
            "exe": exe_target,
            "cwd": cwd,
            "cmdline": get_process_cmdline_parts(pid),
            "environ": get_process_environ(pid),
            "sync_mode": detect_sync_mechanism(pid),
            "libraries": get_loaded_libraries(pid),
            "descriptors": get_open_descriptors_summary(pid),
            "filesystem": get_filesystem_info(cwd or os.getcwd()),
            "process_tree": get_process_tree(pid),
            "memory_maps": get_memory_maps(pid)
        })

        print(f"\n[SESSION START] Logging to: {self.active_session_path}")
        print(f"  • Symlink active: {latest_link} -> {os.path.basename(self.active_session_path)}")
        if self.bpftrace_engine:
            print(f"  • eBPF tracing active: {self.active_bpftrace_path}")
        sys.stdout.flush()

    def infer_thread_roles(self, first_threads, last_threads):
        """Label threads by observed behaviour so the eBPF tables are readable.

        Every thread in a Wine game shares one comm, so a bare tid is meaningless when
        you come back to a trace a week later. Classify from cumulative counters:
          - a tight cluster of threads with near-identical switch counts is a pool
          - the highest-switch thread outside any pool is the coordinator
          - high page-fault threads are the asset/decompression pool
        Heuristic and clearly labelled as such; it never affects recorded numbers.
        """
        roles = {}
        if not last_threads:
            return roles
        # first_threads is a tid -> first-seen-record map, NOT the first sample:
        # the interesting threads are created seconds after the process starts, so
        # anchoring on sample[0] leaves every one of them without a baseline.
        if isinstance(first_threads, dict):
            base = first_threads
        else:
            base = {t["tid"]: t for t in (first_threads or [])}

        def delta(t, field):
            p = base.get(t["tid"])
            return t.get(field, 0) - (p.get(field, 0) if p else 0)

        # Only threads that actually did work. A process winding down has dozens of
        # near-idle threads whose switch counts match each other perfectly, and ranking
        # clusters purely by size elects that crowd as "the worker pool".
        cand = [t for t in last_threads if delta(t, "ticks") > 0 and delta(t, "vol_cs") > 100]
        if not cand:
            return roles
        cand.sort(key=lambda t: -delta(t, "ticks"))

        # Largest cluster of threads within 5% of each other on BOTH cpu time and switch
        # count, scored by size x cpu so an active pool beats a larger idle one.
        best, best_score = [], 0.0
        for ref_t in cand:
            r_tk, r_cs = delta(ref_t, "ticks"), delta(ref_t, "vol_cs")
            if r_tk <= 0 or r_cs <= 0:
                continue
            grp = [t for t in cand
                   if abs(delta(t, "ticks") - r_tk) <= 0.05 * r_tk
                   and abs(delta(t, "vol_cs") - r_cs) <= 0.05 * r_cs]
            score = len(grp) * r_tk
            if len(grp) >= 3 and score > best_score:
                best, best_score = grp, score
        pool = {t["tid"] for t in best}

        for n, t in enumerate(sorted(best, key=lambda t: t["tid"])):
            roles[str(t["tid"])] = f"job-worker-{n}"

        for t in cand:
            if t["tid"] not in pool:
                roles[str(t["tid"])] = "coordinator?"
                break

        flt = sorted(last_threads, key=lambda t: -delta(t, "minflt"))
        for n, t in enumerate(flt[:8]):
            if delta(t, "minflt") > 50000 and str(t["tid"]) not in roles:
                roles[str(t["tid"])] = f"asset-worker-{n}"

        for t in last_threads:
            key = str(t["tid"])
            if key not in roles and t.get("comm") and t["comm"] not in ("wwm.exe",):
                roles[key] = t["comm"][:18]
        return roles

    def note_sample_health(self, sample, prev_threads):
        """Watch each sample for the two failure modes that a rate-averaged trace hides.

        1. Death. All threads move to do_exit / vfs_coredump at once. The samples after
           that point are core-dump time, not game time, and must not be folded into a
           phase duration.
        2. Whole-process quiescence. Every thread alive, zero CPU ticks and zero context
           switches across the entire process for multiple consecutive samples. That is
           a lost wakeup or a deadlock -- categorically different from "slow", and
           invisible in any metric that averages over a window.
        """
        threads = sample.get("threads") or []
        t_rel = sample.get("ts", 0) - (self.start_time or sample.get("ts", 0))
        state = CrashMonitor.classify_sample(threads)

        if state == "dying":
            h = self.health
            if not h.get("coredump_samples"):
                h["death_t"] = t_rel
                h["threads_at_death"] = len(threads)
                if self.active_writer:
                    self.active_writer.write_record({
                        "event": "process_dying", "ts": sample.get("ts"), "t_rel": round(t_rel, 3),
                        "threads": len(threads),
                        "note": "all threads entered exit/coredump; subsequent samples are dump time"
                    })
                print(f"\n[CRASH] Target entered exit/core-dump at t={t_rel:.2f}s "
                      f"({len(threads)} threads). Samples after this point are dump time.")
                sys.stdout.flush()
            h["coredump_samples"] = h.get("coredump_samples", 0) + 1
            h["coredump_seconds"] = round(t_rel - h.get("death_t", t_rel), 2)
            self._last_state = state
            return

        # Quiescence check -- needs a previous snapshot to diff against
        if prev_threads and threads and len(threads) > 4:
            prev = {t["tid"]: t for t in prev_threads}
            moved = False
            for t in threads:
                p = prev.get(t["tid"])
                if not p:
                    continue
                if (t["ticks"] != p["ticks"] or t["vol_cs"] != p["vol_cs"]
                        or t["invol_cs"] != p["invol_cs"]):
                    moved = True
                    break
            if not moved:
                if self._quiet_run == 0:
                    self._quiet_started = t_rel
                self._quiet_run += 1
                if self._quiet_run == 5:
                    print(f"\n[STALL] Whole process quiescent since t={self._quiet_started:.2f}s "
                          f"({len(threads)} threads, zero CPU and zero context switches).")
                    sys.stdout.flush()
                    if self.active_writer:
                        self.active_writer.write_record({
                            "event": "stall_detected", "ts": sample.get("ts"),
                            "t_rel": round(self._quiet_started, 3), "threads": len(threads),
                            "note": "no CPU ticks and no context switches on any thread"
                        })
            else:
                if self._quiet_run >= 5:
                    h = self.health
                    dur = t_rel - (self._quiet_started or t_rel)
                    if dur > h.get("stall_seconds", 0):
                        h["stall_samples"] = self._quiet_run
                        h["stall_seconds"] = round(dur, 2)
                        h["stall_t"] = round(self._quiet_started or 0, 3)
                        h["stall_threads"] = len(threads)
                self._quiet_run = 0
                self._quiet_started = None
        self._last_state = state

    def finalize_health(self, last_t_rel):
        """Fold an in-progress stall into the health record at session end."""
        if self._quiet_run >= 5:
            h = self.health
            dur = last_t_rel - (self._quiet_started or last_t_rel)
            if dur > h.get("stall_seconds", 0):
                h["stall_samples"] = self._quiet_run
                h["stall_seconds"] = round(dur, 2)
                h["stall_t"] = round(self._quiet_started or 0, 3)
                h["stall_threads"] = h.get("stall_threads", 0)

    def bpftrace_healthcheck(self):
        """Confirm bpftrace is still alive. Without this a parse error at second one
        yields a full-length recording with a silently empty eBPF file."""
        if self.bpftrace_engine and not self.bpftrace_engine.check_alive():
            reason = self.bpftrace_engine.failed_reason
            if self.active_writer:
                self.active_writer.write_record({
                    "event": "bpftrace_died", "ts": time.time(), "reason": reason
                })
            self.bpftrace_engine = None

    def write_sample(self, sample_dict):
        """Append a telemetry sample record to active stream."""
        if self.active_writer:
            record = {"event": "sample", "index": self.sample_count, **sample_dict}
            self.active_writer.write_record(record)
            self.sample_count += 1

    def close_session(self, reason="exit"):
        """Cleanly finalize active stream with an exit summary event."""
        bpftrace_summary = None
        if self.bpftrace_engine:
            bpftrace_summary = self.bpftrace_engine.stop()
            self.bpftrace_engine = None

        if self.active_writer:
            duration = time.time() - (self.start_time or time.time())
            self.finalize_health(duration)
            crash_info = CrashMonitor.inspect_exit_status(
                self.active_pid, self.active_proc_name, sampled_evidence=self.health
            ) if self.active_pid else {}

            # Durations that exclude core-dump time, so a crashed run does not report
            # a plausible-looking session length that is mostly dumping.
            effective = duration
            if self.health.get("coredump_seconds"):
                effective = max(0.0, self.health.get("death_t", duration))

            if bpftrace_summary:
                self.active_writer.write_record({
                    "event": "bpftrace_summary",
                    "pid": self.active_pid,
                    "proc_name": self.active_proc_name,
                    "output_file": self.active_bpftrace_path,
                    "summary": bpftrace_summary
                })

            self.active_writer.write_record({
                "event": "process_exit",
                "pid": self.active_pid,
                "proc_name": self.active_proc_name,
                "reason": reason,
                "session_duration_s": round(duration, 2),
                "effective_duration_s": round(effective, 2),
                "total_samples": self.sample_count,
                "health": self.health,
                "thread_roles": self.thread_roles,
                "crash_analysis": crash_info,
                "bpftrace_summary": bpftrace_summary
            })
            if crash_info.get("signature"):
                print(f"[{crash_info['signature']}] {crash_info.get('description')}")
            self.active_writer.close()
            print(f"[SESSION CLOSED] Finalized {self.active_session_path} ({self.sample_count} samples, {duration:.1f}s)")
            if self.active_bpftrace_path and os.path.exists(self.active_bpftrace_path):
                print(f"  • eBPF kernel latency summary saved to: {self.active_bpftrace_path}")
            sys.stdout.flush()

        self.active_writer = None
        self.active_session_path = None
        self.active_pid = None
        self.active_proc_name = None
        self.active_bpftrace_path = None


class JsonStreamReader:
    """Loads and parses NDJSON streaming data or legacy JSON arrays."""
    @staticmethod
    def load_samples(filepath):
        """Read all sample events from a JSON stream file."""
        if not os.path.exists(filepath):
            return []
        samples = []
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                header = f.read(1024).strip()
                f.seek(0)
                if header.startswith("["):
                    # Legacy JSON array
                    return json.load(f)
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            obj = json.loads(line)
                            if obj.get("event") == "sample" or "ts" in obj:
                                samples.append(obj)
                        except Exception:
                            pass
        except Exception as e:
            print(f"[READER ERROR] Failed to load {filepath}: {e}", file=sys.stderr)
        return samples

    @staticmethod
    def load_bpftrace_summary(filepath):
        """Extract bpftrace summary from session stream or companion JSON file."""
        base, _ = os.path.splitext(filepath)
        companion = f"{base}_bpftrace.json"
        if os.path.exists(companion):
            try:
                res = BpftraceEngine.parse_output(companion)
                if res:
                    return res
            except Exception:
                pass

        if not os.path.exists(filepath):
            return None

        try:
            with open(filepath, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                        if obj.get("event") == "bpftrace_summary" and obj.get("summary"):
                            return obj["summary"]
                        if obj.get("event") == "process_exit" and obj.get("bpftrace_summary"):
                            return obj["bpftrace_summary"]
                    except Exception:
                        pass
        except Exception:
            pass
        return None


# ------------------------------------------------------------------------------
# 11. Root-Cause Diagnostic & Recommendation Engine
# ------------------------------------------------------------------------------
def diagnose_proton_launch_environment(pid, proc_name):
    """Run comprehensive launch checks and output actionable recommendations."""
    findings = []
    recommendations = []

    # 1. Check max_map_count
    sysctls = get_sysctl_settings()
    map_count = sysctls.get("vm.max_map_count")
    if map_count is not None and map_count < 1048576:
        findings.append(f"vm.max_map_count is {map_count} (recommended >= 1048576, ideal 2147483642)")
        recommendations.append("Increase map count: pkexec sysctl -w vm.max_map_count=2147483642")

    # 2. Check filesystem compatibility
    cwd = None
    try: cwd = os.readlink(f"/proc/{pid}/cwd")
    except Exception: pass
    fs_info = get_filesystem_info(cwd or os.getcwd())
    if fs_info.get("is_ntfs"):
        findings.append(f"Game prefix or working directory is on an NTFS filesystem ({fs_info.get('fs_type')})")
        recommendations.append("NTFS partitions often break Proton symlinks and wine prefix creation. Mount with uid/gid and exec options or move prefix to ext4/btrfs.")

    # 3. Check synchronization
    sync_mode = detect_sync_mechanism(pid)
    if "wineserver (IPC fallback)" in sync_mode:
        findings.append("Running on fallback Wineserver IPC synchronization (high latency overhead).")
        recommendations.append("Ensure ntsync module is loaded (/dev/ntsync) or enable fsync via WINEFSYNC=1 / PROTON_NTSYNC=1.")

    # 4. Check loaded Vulkan layers
    libs = get_loaded_libraries(pid)
    vulkan_layers = libs.get("vulkan_layers", [])
    if not vulkan_layers:
        findings.append("No Vulkan/DXVK translation layers detected in process maps.")
        recommendations.append("Verify DXVK/VKD3D installation and check Vulkan ICD drivers (VK_ICD_FILENAMES).")

    return {
        "sync_mode": sync_mode,
        "vulkan_layers": vulkan_layers,
        "findings": findings,
        "recommendations": recommendations
    }


# ------------------------------------------------------------------------------
# 12. Analysis & Reporting Functions
# ------------------------------------------------------------------------------
def generate_telemetry_report(samples, bpftrace_data=None, thread_roles=None, events=None):
    """Analyze recorded telemetry samples and generate a structured diagnostic report."""
    if len(samples) < 2:
        return "Insufficient samples (< 2) to compute deltas."

    out = []
    def p(text=""): out.append(text)

    p("==========================================================================================")
    p(f"   PROTON / LINUX FULL-SPECTRUM GAME PERFORMANCE REPORT ({len(samples)} samples)")
    p("==========================================================================================")
    p(f"Target Process : {samples[-1].get('proc_name', 'game.exe')} (PID {samples[-1].get('pid', 'unknown')})")
    p(f"Sync Mechanism : {samples[-1].get('sync_mode', 'unknown')}")
    p(f"Recorded Range : {time.strftime('%H:%M:%S', time.localtime(samples[0]['ts']))} -> {time.strftime('%H:%M:%S', time.localtime(samples[-1]['ts']))} (Duration: {samples[-1]['ts'] - samples[0]['ts']:.1f}s)")
    p()

    # Time budget aggregation
    wchan_categories = {
        "Compute (Running / On-CPU)": 0,
        "ntsync / Kernel Fast Sync": 0,
        "Futex / User Critical Section Wait": 0,
        "Wineserver IPC Wait (Pipe/Epoll/Select)": 0,
        "Storage / Disk I/O Wait (State D / VFS)": 0,
        "GPU Driver Wait / Fences (amdgpu/nv)": 0,
        "Timer / Sleep / Polling / Spin": 0,
        "Memory Alloc / Page Faults": 0,
        "Other / Idle / Unclassified": 0
    }
    thread_compute = defaultdict(int)
    thread_comm = {}
    kstack_hist = defaultdict(int)

    for k in range(1, len(samples)):
        prev_t = {t["tid"]: t for t in samples[k-1].get("threads", [])}
        for t in samples[k].get("threads", []):
            tid = t["tid"]
            thread_comm[tid] = t["comm"]
            if tid in prev_t:
                thread_compute[tid] += max(0, t["ticks"] - prev_t[tid]["ticks"])

            w = t["wchan"].lower()
            if t.get("kstack"): kstack_hist[t["kstack"]] += 1

            if t["state"] == "R" or w == "0":
                wchan_categories["Compute (Running / On-CPU)"] += 1
            elif "ntsync" in w or ("ioctl" in w and "amdgpu" not in w and "nvidia" not in w):
                wchan_categories["ntsync / Kernel Fast Sync"] += 1
            elif "futex" in w:
                wchan_categories["Futex / User Critical Section Wait"] += 1
            elif any(x in w for x in ["pipe", "select", "epoll", "unix_stream"]):
                wchan_categories["Wineserver IPC Wait (Pipe/Epoll/Select)"] += 1
            elif "io" in w or t["state"] == "D" or any(x in w for x in ["blk", "ext4", "btrfs", "xfs"]):
                wchan_categories["Storage / Disk I/O Wait (State D / VFS)"] += 1
            elif any(x in w for x in ["amdgpu", "nvidia", "dma_fence", "ttm", "vblank", "drm"]):
                wchan_categories["GPU Driver Wait / Fences (amdgpu/nv)"] += 1
            elif any(x in w for x in ["sleep", "timer", "schedule_timeout", "nanosleep"]):
                wchan_categories["Timer / Sleep / Polling / Spin"] += 1
            elif any(x in w for x in ["alloc", "fault", "compact"]):
                wchan_categories["Memory Alloc / Page Faults"] += 1
            else:
                wchan_categories["Other / Idle / Unclassified"] += 1

    total_time_samples = sum(wchan_categories.values())
    p("--- TIME BUDGET BREAKDOWN (Where Worker Threads Spent Time) ---")
    for cat, count in wchan_categories.items():
        pct = (count / max(1, total_time_samples)) * 100.0
        bar = "█" * int(pct / 4)
        p(f"  {cat:44s}: {pct:5.1f}%  | {bar}")

    if kstack_hist:
        p("\n--- TOP KERNEL BLOCKING STACK FRAMES ---")
        for kframe, cnt in sorted(kstack_hist.items(), key=lambda x: x[1], reverse=True)[:6]:
            p(f"  • {kframe:40s}: {cnt:5d} samples")

    duration = max(0.01, samples[-1]["ts"] - samples[0]["ts"])

    # Per-thread scheduler accounting from /proc schedstat. runq_ns is cumulative time
    # the thread was runnable but not running; dividing the delta by the delta in
    # schedule count gives the average wake-to-run latency directly, instead of
    # inferring it from context-switch rates over a window 100x longer than the event.
    # First/last observation per tid, not first/last SAMPLE: threads are created long
    # after the process starts, so anchoring on sample[0] silently drops every thread
    # that mattered.
    first_t, last_t = {}, {}
    death_idx = None
    stall_from = None
    stall_len = 0
    for k, s in enumerate(samples):
        ths = s.get("threads", [])
        for t in ths:
            first_t.setdefault(t["tid"], t)
            last_t[t["tid"]] = t
        if death_idx is None and CrashMonitor.classify_sample(ths) == "dying":
            death_idx = k
        if k > 0 and ths and len(ths) > 4:
            prev = {t["tid"]: t for t in samples[k-1].get("threads", [])}
            moved = any((t["ticks"], t["vol_cs"]) != (prev[t["tid"]]["ticks"], prev[t["tid"]]["vol_cs"])
                        for t in ths if t["tid"] in prev)
            if not moved and death_idx is None:
                if stall_len == 0:
                    stall_from = k
                stall_len += 1
            elif stall_len:
                stall_len = 0

    if death_idx is not None:
        t_rel = samples[death_idx]["ts"] - samples[0]["ts"]
        dump_s = samples[-1]["ts"] - samples[death_idx]["ts"]
        p(f"  !! PROCESS DIED at t={t_rel:.2f}s: all threads in exit/core-dump for the")
        p(f"     remaining {dump_s:.1f}s. Exclude those samples from any phase duration.")
        p()
    if stall_len >= 5 and stall_from is not None:
        t_rel = samples[stall_from]["ts"] - samples[0]["ts"]
        p(f"  !! STALL: {stall_len} consecutive samples from t={t_rel:.2f}s with zero CPU")
        p(f"     and zero context switches on every thread (lost wakeup or deadlock).")
        p()

    # ---- Settle time: the metric the whole exercise is actually about ----------
    # A teleport is followed by a burst of settling threads that are decommissioned
    # once the world is built, so the end of the phase is the collapse of the thread
    # pool back toward its steady-state size. That boundary is unambiguous and needs
    # no interpretation, unlike context-switch rates or "wave" counts.
    if events:
        tstart = samples[0]["ts"]
        teleports = [e for e in events if e.get("type") == "teleport_start"]
        if teleports:
            p("--- LOAD / SETTLE PHASES ---")
            for tp in teleports:
                t_tel = tp["ts"] - tstart
                ia = next((i for i, s in enumerate(samples) if s["ts"] - tstart >= t_tel), None)
                if ia is None:
                    continue
                peak, end_i = 0, None
                for i in range(ia, len(samples)):
                    peak = max(peak, len(samples[i].get("threads", [])))
                    if len(samples[i].get("threads", [])) <= peak - 12 and all(
                            len(samples[j].get("threads", [])) <= peak - 8
                            for j in range(i, min(i + 10, len(samples)))):
                        end_i = i
                        break
                if end_i is None:
                    p(f"  space {tp.get('spaceno', '?'):>6}: teleport at {t_tel:6.1f}s -- "
                      f"settle did not complete before the trace ended")
                    continue
                t_end = samples[end_i]["ts"] - tstart
                cpu = 0
                io = 0.0
                for k in range(ia + 1, end_i + 1):
                    prev = {x["tid"]: x for x in samples[k-1].get("threads", [])}
                    for x in samples[k].get("threads", []):
                        q = prev.get(x["tid"])
                        if q and x["ticks"] > q["ticks"]:
                            cpu += x["ticks"] - q["ticks"]
                    io += (samples[k].get("io_read_rate_mb") or 0) * (samples[k].get("dt") or 0)
                dur = max(0.01, t_end - t_tel)
                p(f"  space {tp.get('spaceno', '?'):>6}: teleport {t_tel:6.1f}s -> settled {t_end:6.1f}s"
                  f"  =  {dur:5.1f}s   |  {cpu/100:6.1f} CPU-s ({cpu/100/dur:4.2f} cores)"
                  f"  |  {io/1024:5.2f} GB read  |  peak {peak} threads")
            p()

    have_sched = any("runq_ns" in t for t in last_t.values())

    if have_sched:
        rows = []
        for tid, lt in last_t.items():
            ft = first_t.get(tid)
            if not ft or "runq_ns" not in lt or "runq_ns" not in ft:
                continue
            d_runq = lt["runq_ns"] - ft["runq_ns"]
            d_cnt = lt["sched_cnt"] - ft["sched_cnt"]
            d_on = lt["sched_ns"] - ft["sched_ns"]
            d_vcs = lt["vol_cs"] - ft["vol_cs"]
            if d_cnt <= 0 or d_runq < 0:
                continue
            rows.append((tid, d_cnt, d_runq / d_cnt / 1000.0, d_runq / 1e9,
                         d_on / 1e9, d_vcs / duration))
        if rows:
            p("\n--- SCHEDULER: WAKE-TO-RUN LATENCY PER THREAD (from /proc schedstat) ---")
            p("  runq = runnable but waiting for a CPU. Measured, not inferred.")
            p(f"  {'TID':>8} {'COMM':<16} {'SCHEDS':>9} {'AVG RUNQ':>10} {'TOT RUNQ':>9} {'ON-CPU':>9} {'VCS/s':>8}")
            p("  " + "-" * 74)
            for tid, cnt, avg_us, tot_s, on_s, vcs in sorted(rows, key=lambda r: -r[3])[:15]:
                p(f"  {tid:>8} {thread_comm.get(tid, '?')[:16]:<16} {cnt:>9d} "
                  f"{avg_us:>9.1f}u {tot_s:>8.2f}s {on_s:>8.2f}s {vcs:>8.0f}")
            tot_runq = sum(r[3] for r in rows)
            tot_on = sum(r[4] for r in rows)
            p(f"  TOTAL: {tot_on:.1f}s on-cpu, {tot_runq:.1f}s waiting for a cpu "
              f"({100.0 * tot_runq / max(0.001, tot_on + tot_runq):.1f}% of ready time)")
    else:
        p("\n  (no schedstat in this trace -- recorded by an older profiler build)")

    p("\n--- TOP 10 BUSIEST THREADS ---")
    for tid, ticks in sorted(thread_compute.items(), key=lambda x: x[1], reverse=True)[:10]:
        c_sec = ticks / 100.0
        t_util = (c_sec / duration) * 100.0
        ft, lt = first_t.get(tid), last_t.get(tid)
        extra = ""
        if ft and lt:
            d_vcs = lt["vol_cs"] - ft["vol_cs"]
            if d_vcs > 0:
                # CPU time per voluntary switch: high values with low throughput are
                # the signature of user-space spinning rather than useful work.
                extra = f" | {d_vcs/duration:6.0f} vcs/s | {c_sec*1e6/d_vcs:7.1f} us/switch"
        p(f"  TID {tid:7d} | {thread_comm.get(tid, 'unknown'):18s} | CPU: {c_sec:5.2f}s ({t_util:4.0f}% core){extra}")

    # Sampling fidelity: a trace taken at an effective rate well below the requested
    # one silently biases every derived rate, so state it rather than hide it.
    dts = [s.get("dt") for s in samples[1:] if s.get("dt")]
    if dts:
        dts_sorted = sorted(dts)
        med = dts_sorted[len(dts_sorted) // 2]
        scans = [s.get("scan_ms") for s in samples if s.get("scan_ms")]
        p(f"\n--- SAMPLING FIDELITY ---")
        p(f"  effective rate: {1.0/med:.1f} Hz (median period {med*1000:.1f} ms)")
        if scans:
            scans_sorted = sorted(scans)
            p(f"  per-sample scan cost: median {scans_sorted[len(scans_sorted)//2]:.1f} ms, "
              f"max {scans_sorted[-1]:.1f} ms")

    if bpftrace_data:
        bp_rep = BpftraceEngine.format_summary(bpftrace_data, thread_roles=thread_roles)
        if bp_rep:
            p("\n" + bp_rep)

    return "\n".join(out)


# ------------------------------------------------------------------------------
# 13. Execution Modes
# ------------------------------------------------------------------------------
def mode_record(target_name=None, target_pid=None, sample_hz=20, output_dir=DEFAULT_TELEMETRY_DIR,
                duration=None, max_samples=None, exit_on_detach=False, bpftrace_opts=None,
                collect_kstack=False, game_log=None):
    """
    Continuous background sampling engine with real-time JSON streaming (NDJSON)
    and automatic session rotation on process exit.
    """
    global g_stop_requested
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    interval = 1.0 / max(1, sample_hz)
    overrun_warned = False
    session_mgr = SessionManager(output_dir, bpftrace_opts=bpftrace_opts)

    target_desc = f"Name='{target_name}'" if target_name else (f"PID={target_pid}" if target_pid else "Auto-detect Proton games")
    print(f"Proton Profiler: Initialized for target ({target_desc}) @ {sample_hz} Hz [NDJSON Streaming]")
    sys.stdout.flush()

    waiting_logged = False
    total_recorded_across_sessions = 0
    start_time = time.time()

    try:
        while not g_stop_requested:
            pid = resolve_target_pid(target_name, target_pid)
            if not pid:
                if not waiting_logged:
                    print(f"[WAITING] Standing by for target '{target_name or 'Proton game'}' to launch...")
                    sys.stdout.flush()
                    waiting_logged = True
                time.sleep(0.2)
                continue

            waiting_logged = False
            proc_name = target_name or "game.exe"
            comm = read_file_safe(f"/proc/{pid}/comm")
            if comm: proc_name = comm.strip()

            ws_pid = get_wineserver_pid(pid)
            sync_mode = detect_sync_mechanism(pid)

            # Start fresh session for attached PID
            session_mgr.start_session(pid, proc_name)

            last_proc = read_process_memory_and_io(pid)
            last_ts = time.time()
            log_monitor = GameLogMonitor(game_log)
            prev_threads = None
            first_seen = {}   # tid -> first record ever seen for that thread
            last_seen = {}    # tid -> last record ever seen (NOT the final sample:
                              # at exit most threads are already gone)
            # Absolute deadline schedule. The old loop did sleep(interval) and THEN the
            # work, so the real period was interval + scan time: a requested 20 Hz came
            # out at ~17 Hz and drifted with thread count, biasing every derived rate.
            next_deadline = time.perf_counter()

            # Active sampling loop
            while not g_stop_requested:
                next_deadline += interval
                slack = next_deadline - time.perf_counter()
                if slack > 0:
                    time.sleep(slack)
                else:
                    # Scan is slower than the requested period; resync instead of
                    # accumulating an ever-growing backlog, and say so once.
                    next_deadline = time.perf_counter()
                    if not overrun_warned:
                        print(f"[WARN] Sampling cannot keep up with {sample_hz} Hz "
                              f"(scan alone exceeds {interval*1000:.0f} ms). "
                              f"Lower --rate or keep --kstack off.", file=sys.stderr)
                        overrun_warned = True

                if not os.path.exists(f"/proc/{pid}"):
                    print(f"\n[DETACHED] Process '{proc_name}' (PID {pid}) exited.")
                    session_mgr.thread_roles = session_mgr.infer_thread_roles(first_seen, list(last_seen.values()))
                    session_mgr.close_session(reason="process_exit")
                    if exit_on_detach or target_pid is not None:
                        g_stop_requested = True
                    break

                # Order matters: read the counters first, then stamp the sample with the
                # time the scan FINISHED. Previously ts was taken before an ~8 ms scan,
                # so every counter delta was attributed to a window that ended before
                # the counters were actually read.
                t_scan0 = time.time()
                threads_data, scan_ms = collect_thread_snapshots(pid, collect_kstack=collect_kstack)
                curr_proc = read_process_memory_and_io(pid)
                ws_data = collect_wineserver_metrics(ws_pid)
                gpu = get_gpu_metrics()
                cpu_sys = get_cpu_frequencies_and_temperature()
                psi = get_psi_pressure()
                now = time.time()
                dt = max(0.001, now - last_ts)

                # Poll Game Log events (timestamps come from the log lines themselves)
                for ev in log_monitor.poll_events():
                    session_mgr.active_writer.write_record({"event": "game_log_event", **ev})

                sample = {
                    "ts": now,
                    "dt": dt,
                    "scan_ms": round(scan_ms, 3),
                    "scan_start_ts": t_scan0,
                    "pid": pid,
                    "proc_name": proc_name,
                    "sync_mode": sync_mode,
                    "io_read_rate_mb": max(0.0, curr_proc["read_bytes"] - last_proc["read_bytes"]) / (dt * 1e6),
                    "io_char_read_mb": max(0.0, curr_proc["rchar"] - last_proc["rchar"]) / (dt * 1e6),
                    "io_write_rate_mb": max(0.0, curr_proc["write_bytes"] - last_proc["write_bytes"]) / (dt * 1e6),
                    "io_read_iops": max(0, curr_proc["syscr"] - last_proc["syscr"]) / dt,
                    "rss_mb": curr_proc["rss_mb"],
                    "swap_mb": curr_proc["swap_mb"],
                    "ws_ticks": ws_data["ticks"],
                    "ws_wchan": ws_data["wchan"],
                    "gpu": gpu,
                    "cpu_sys": cpu_sys,
                    "psi": psi,
                    "threads": threads_data
                }

                session_mgr.write_sample(sample)
                # Mappings change as the game loads modules; re-snapshot a few times
                # early so a late-loaded DLL is still resolvable.
                if session_mgr.sample_count % 200 == 0:
                    session_mgr.active_writer.write_record({
                        "event": "memory_maps", "ts": now, "pid": pid,
                        "maps": get_memory_maps(pid)})
                session_mgr.note_sample_health(sample, prev_threads)
                if session_mgr.sample_count % 40 == 0:
                    session_mgr.bpftrace_healthcheck()
                for _t in threads_data:
                    first_seen.setdefault(_t["tid"], _t)
                    last_seen[_t["tid"]] = _t
                prev_threads = threads_data
                total_recorded_across_sessions += 1
                last_proc = curr_proc
                last_ts = now

                if duration and (now - start_time) >= duration:
                    print(f"\n[LIMIT REACHED] Recording duration target ({duration:.1f}s) completed.")
                    g_stop_requested = True
                    break
                if max_samples and total_recorded_across_sessions >= max_samples:
                    print(f"\n[LIMIT REACHED] Recording sample count limit ({max_samples}) reached.")
                    g_stop_requested = True
                    break

    finally:
        try:
            session_mgr.thread_roles = session_mgr.infer_thread_roles(first_seen, list(last_seen.values()))
        except Exception:
            pass
        session_mgr.close_session(reason="profiler_shutdown")


def mode_diagnose(target_name=None, target_pid=None):
    """Run instant Proton launch diagnostics on host environment and target process."""
    print("==========================================================================================")
    print("   PROTON GAME LAUNCH ENVIRONMENT DIAGNOSTICS")
    print("==========================================================================================")
    host = get_host_system_info()
    sysctls = get_sysctl_settings()
    mem = get_system_memory_info()
    gpu = get_gpu_metrics()

    print(f"Host System  : Kernel {host['kernel_release']} | glibc {host['glibc_version']} | CPU: {host['cpu_model']} ({host['cpu_cores_logical']} cores)")
    print(f"Host Memory  : Total: {mem['total_mb']} MB | Available: {mem['available_mb']} MB | Swap: {mem['swap_total_mb']} MB")
    print(f"Primary GPU  : {gpu['vendor']} | VRAM: {gpu['vram_used_mb']}/{gpu['vram_total_mb']} MB | Temp: {gpu['temp_c']}°C")
    print(f"Sysctls      : vm.max_map_count = {sysctls.get('vm.max_map_count')} | split_lock_mitigate = {sysctls.get('kernel.split_lock_mitigate')}")
    print("------------------------------------------------------------------------------------------")

    pid = resolve_target_pid(target_name, target_pid)
    if not pid:
        print(f"Target process '{target_name or 'game.exe'}' is not currently running.")
        return

    comm = read_file_safe(f"/proc/{pid}/comm") or str(pid)
    diag = diagnose_proton_launch_environment(pid, comm.strip())
    print(f"Target PID   : {pid} ({comm.strip()})")
    print(f"Sync Active  : {diag['sync_mode']}")
    print(f"Vulkan Layers: {', '.join(diag['vulkan_layers']) if diag['vulkan_layers'] else 'None detected'}")

    if diag["findings"]:
        print("\n[DIAGNOSTIC FINDINGS]")
        for f in diag["findings"]: print(f"  • {f}")
    if diag["recommendations"]:
        print("\n[ACTIONABLE RECOMMENDATIONS]")
        for r in diag["recommendations"]: print(f"  • {r}")
    if not diag["findings"]:
        print("\n[STATUS OK] Launch environment parameters meet standard Proton requirements.")


def mode_top(target_name=None, target_pid=None):
    """Real-time live terminal dashboard."""
    print(f"Proton Profiler: Attaching live monitor to {target_name or (f'PID {target_pid}' if target_pid else 'active game')}...")
    waiting_logged = False
    while not g_stop_requested:
        pid = resolve_target_pid(target_name, target_pid)
        if not pid:
            if not waiting_logged:
                print(f"[WAITING] Standing by for target '{target_name or 'Proton game'}'...")
                waiting_logged = True
            time.sleep(0.5)
            continue

        waiting_logged = False
        sync_mode = detect_sync_mechanism(pid)
        last_proc = read_process_memory_and_io(pid)
        last_ts = time.time()
        last_threads = {t["tid"]: t["ticks"] for t in collect_thread_snapshots(pid)[0]}

        try:
            while not g_stop_requested:
                time.sleep(0.5)
                now = time.time()
                dt = max(0.001, now - last_ts)
                if not os.path.exists(f"/proc/{pid}"):
                    break

                curr_proc = read_process_memory_and_io(pid)
                gpu = get_gpu_metrics()
                cpu_sys = get_cpu_frequencies_and_temperature()
                curr_threads = collect_thread_snapshots(pid)[0]

                # Clear & Render Dashboard
                print("\033[H\033[J", end="")
                print(f"================== PROTON LIVE GAME PROFILER (PID {pid} | {sync_mode}) ==================")
                disk_mb_s = max(0.0, curr_proc["read_bytes"] - last_proc["read_bytes"]) / (dt * 1e6)
                print(f"GPU Load : {gpu['busy_pct']:3d}% | VRAM: {gpu['vram_used_mb']:5d}/{gpu['vram_total_mb']} MB | Temp: {gpu['temp_c']:.0f}°C | Power: {gpu['power_w']:.1f}W")
                print(f"CPU Load : Gov={cpu_sys['governor']} AvgFreq={cpu_sys['avg_freq_mhz']:.0f}MHz Temp={cpu_sys['cpu_temp_c']:.0f}°C | RSS={curr_proc['rss_mb']} MB | Disk: {disk_mb_s:5.1f} MB/s")
                print(f"------------------------------------------------------------------------------------------")

                thread_deltas = []
                for t in curr_threads:
                    tid = t["tid"]
                    prev_ticks = last_threads.get(tid, t["ticks"])
                    cpu_pct = (max(0, t["ticks"] - prev_ticks) / (dt * 100.0)) * 100.0
                    thread_deltas.append((cpu_pct, t))

                thread_deltas.sort(key=lambda x: x[0], reverse=True)
                print(f"{'TID':7s} | {'THREAD NAME':18s} | {'CPU%':6s} | {'STATE':5s} | {'VOL_CS':7s} | {'WAIT CHANNEL':22s} | {'KERNEL STACK'}")
                print(f"--------+--------------------+--------+-------+---------+------------------------+--------------------")
                for cpu_pct, t in thread_deltas[:12]:
                    print(f"{t['tid']:7d} | {t['comm'][:18]:18s} | {cpu_pct:5.1f}% | {t['state']:5s} | {t['vol_cs']:7d} | {t['wchan'][:22]:22s} | {t.get('kstack', '')[:20]}")

                last_proc = curr_proc
                last_ts = now
                last_threads = {t["tid"]: t["ticks"] for t in curr_threads}

        except KeyboardInterrupt:
            break


def mode_inspect(target_name=None, target_pid=None):
    """Instant 1-second process snapshot."""
    pid = resolve_target_pid(target_name, target_pid)
    if not pid:
        print(f"No active process found for '{target_name or (f'PID {target_pid}' if target_pid else 'any Proton game')}'.")
        return

    print(f"Inspecting PID {pid} for 1.0 second...")
    t1_proc = read_process_memory_and_io(pid)
    t1_threads = {t["tid"]: t for t in collect_thread_snapshots(pid)[0]}
    time.sleep(1.0)
    t2_proc = read_process_memory_and_io(pid)
    t2_threads = {t["tid"]: t for t in collect_thread_snapshots(pid)[0]}

    gpu = get_gpu_metrics()
    sync_mode = detect_sync_mechanism(pid)
    libs = get_loaded_libraries(pid)

    print(f"\n=== INSTANT SNAPSHOT: PID {pid} ({sync_mode}) ===")
    print(f"• Vulkan Layers : {', '.join(libs['vulkan_layers']) if libs['vulkan_layers'] else 'None'}")
    print(f"• GPU Metrics   : Busy={gpu['busy_pct']}% | VRAM={gpu['vram_used_mb']} MB | Temp={gpu['temp_c']:.0f}°C")
    print(f"• Memory & Disk : RSS={t2_proc['rss_mb']} MB | Disk Read={max(0, t2_proc['read_bytes'] - t1_proc['read_bytes'])/1e6:.2f} MB/s")
    print(f"\nTop Active Threads:")

    deltas = []
    for tid, t2 in t2_threads.items():
        if tid in t1_threads:
            deltas.append((t2["ticks"] - t1_threads[tid]["ticks"], t2))
    deltas.sort(key=lambda x: x[0], reverse=True)
    for d_ticks, t in deltas[:8]:
        print(f"  TID {t['tid']:7d} | {t['comm']:16s} | CPU: {d_ticks:3d}% | State: {t['state']} | Wchan: {t['wchan']:15s} | Stack: {t.get('kstack', '')}")


def mode_bpftrace(target_name=None, target_pid=None, preset="sync", script_file=None,
                  eval_str=None, output_file=None, duration=None, output_dir=DEFAULT_TELEMETRY_DIR,
                  flush_secs=10):
    """
    Dedicated interactive eBPF kernel tracing mode using bpftrace.
    Attaches directly to the target game process and captures low-level kernel latency.
    """
    global g_stop_requested
    signal.signal(signal.SIGINT, _signal_handler)
    signal.signal(signal.SIGTERM, _signal_handler)

    if not BpftraceEngine.is_available():
        print("[ERROR] bpftrace binary not found in PATH. Please install bpftrace (e.g. pacman -S bpftrace).", file=sys.stderr)
        return

    target_desc = f"Name='{target_name}'" if target_name else (f"PID={target_pid}" if target_pid else "Auto-detect Proton games")
    print(f"Proton Profiler: Attaching eBPF trace ({preset}) to target ({target_desc})...")
    sys.stdout.flush()

    waiting_logged = False
    pid = None
    while not g_stop_requested and not pid:
        pid = resolve_target_pid(target_name, target_pid)
        if not pid:
            if not waiting_logged:
                print(f"[WAITING] Standing by for target '{target_name or 'Proton game'}' to launch...")
                sys.stdout.flush()
                waiting_logged = True
            time.sleep(0.3)

    if not pid:
        return

    proc_name = target_name or "game.exe"
    comm = read_file_safe(f"/proc/{pid}/comm")
    if comm: proc_name = comm.strip()

    ts_str = datetime.now().strftime("%Y%m%d_%H%M%S")
    safe_name = "".join(c if c.isalnum() or c in "._-" else "_" for c in proc_name)
    if not output_file:
        os.makedirs(output_dir, exist_ok=True)
        output_file = os.path.join(output_dir, f"{safe_name}_{pid}_{ts_str}_bpftrace.json")

    engine = BpftraceEngine(preset=preset, script_file=script_file, eval_str=eval_str,
                            flush_secs=flush_secs)
    started = engine.start(pid, output_file)
    if not started:
        print(f"[ERROR] Failed to start bpftrace engine.", file=sys.stderr)
        return

    print(f"[BPFTRACE RUNNING] Press Ctrl+C to detach probes and display summary...")
    start_time = time.time()
    try:
        while not g_stop_requested:
            time.sleep(0.5)
            now = time.time()
            if not os.path.exists(f"/proc/{pid}"):
                print(f"\n[DETACHED] Process '{proc_name}' (PID {pid}) exited.")
                break
            if duration and (now - start_time) >= duration:
                print(f"\n[LIMIT REACHED] Tracing duration target ({duration:.1f}s) reached.")
                break
            if engine.poll() is not None:
                print(f"\n[BPFTRACE EXIT] bpftrace process exited unexpectedly.")
                break
    finally:
        summary = engine.stop()
        if summary:
            print("\n" + BpftraceEngine.format_summary(summary))


def summarize_trace(path):
    """Extract the one-line verdict for a trace: runner, settle time, CPU, handoffs.

    Streams the file in two passes and never holds more than two thread snapshots.
    Traces here reach multiple GB; keeping every sample in memory to find two indices
    gets the process OOM-killed, which is how the first version of this died.
    """
    t0 = tel = None
    runner, label = "?", None
    marks = []          # (ts, nthreads, io_rate, dt) -- small, one tuple per sample
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line.startswith("{"):
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                e = d.get("event")
                if e == "sample":
                    marks.append((d.get("ts", 0), len(d.get("threads") or ()),
                                  d.get("io_read_rate_mb") or 0, d.get("dt") or 0))
                elif e == "stream_start":
                    t0 = d.get("session_start_ts")
                elif e == "launch_diagnostics":
                    exe = d.get("exe") or ""
                    if "/wine/" in exe:
                        runner = exe.split("/wine/")[1].split("/")[0]
                    env = d.get("environ") or {}
                    label = ",".join(k for k in ("PROTON_NO_NTSYNC", "PROTON_NO_ESYNC",
                                                 "PROTON_NO_FSYNC", "PROTON_USE_WINED3D",
                                                 "WINE_DISABLE_NTSYNC")
                                     if env.get(k) not in (None, "0"))
                elif e == "game_log_event" and d.get("type") == "teleport_start" and tel is None:
                    tel = d.get("ts", 0) - (t0 or 0)
    except Exception:
        return None
    base = {"path": path, "runner": runner, "label": label, "settle": None}
    if not marks or tel is None or t0 is None:
        return base

    ia = next((i for i, m in enumerate(marks) if m[0] - t0 >= tel), None)
    if ia is None:
        return base
    peak, ib = 0, None
    for i in range(ia, len(marks)):
        peak = max(peak, marks[i][1])
        if marks[i][1] <= peak - 12 and all(marks[j][1] <= peak - 8
                                            for j in range(i, min(i + 10, len(marks)))):
            ib = i
            break
    if ib is None:
        return base

    # Second pass: pull the thread tables for exactly those two samples.
    want = {ia, ib}
    snaps = {}
    idx = 0
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if '"sample"' not in line:
                    continue
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                if d.get("event") != "sample":
                    continue
                if idx in want:
                    snaps[idx] = {t["tid"]: t for t in (d.get("threads") or ())}
                    if len(snaps) == len(want):
                        break
                idx += 1
    except Exception:
        return base
    if ia not in snaps or ib not in snaps:
        return base

    dur = marks[ib][0] - marks[ia][0]
    a, b = snaps[ia], snaps[ib]
    cpu = 0
    rows = []
    for tid, t in b.items():
        prev = a.get(tid)
        if not prev:
            continue
        d_tk = t["ticks"] - prev["ticks"]
        if d_tk > 0:
            cpu += d_tk
        rows.append((d_tk, t["vol_cs"] - prev["vol_cs"]))
    io = sum(m[2] * m[3] for m in marks[ia + 1:ib + 1])
    rows.sort(reverse=True)
    top = rows[1:13]
    best = []
    for tk, cs in top:
        grp = [r for r in top if tk and abs(r[0] - tk) <= 0.05 * tk]
        if len(grp) > len(best):
            best = grp
    hand = (sum(r[1] for r in best) / len(best) / dur) if best and dur else 0
    base.update({"settle": dur, "cpu": cpu / 100.0, "cores": cpu / 100.0 / max(dur, 0.01),
                 "io_gb": io / 1024.0, "handoffs": hand})
    return base


def mode_compare(output_dir=DEFAULT_TELEMETRY_DIR, data_files=None):
    """Tabulate settle time across traces -- the harness for A/B and bisect work."""
    files = data_files or sorted(glob.glob(os.path.join(output_dir, "*.jsonl")))
    files = [f for f in dict.fromkeys(files)
             if not os.path.basename(f).startswith("latest")]
    if not files:
        print("No traces found in " + str(output_dir))
        return
    print("=" * 104)
    print("   SETTLE-TIME COMPARISON  (teleport -> settling-thread-pool collapse)")
    print("=" * 104)
    hdr = "  {:<34} {:<20} {:>8} {:>8} {:>6} {:>8} {:>10}  flags"
    print(hdr.format("trace", "runner", "settle", "CPU-s", "cores", "GB read", "handoff/s"))
    print("  " + "-" * 100)
    out = []
    for f in files:
        r = summarize_trace(f)
        if r:
            out.append(r)
    for r in sorted(out, key=lambda r: (r["runner"], r["settle"] or 1e9)):
        name = os.path.basename(r["path"])[:34]
        if r.get("settle") is None:
            print("  {:<34} {:<20} {:>8}".format(name, r["runner"][:20], "no settle"))
            continue
        print("  {:<34} {:<20} {:7.1f}s {:8.1f} {:6.2f} {:8.2f} {:10.0f}  {}".format(
            name, r["runner"][:20], r["settle"], r["cpu"], r["cores"],
            r["io_gb"], r["handoffs"], r.get("label") or ""))
    byrunner = {}
    for r in out:
        if r.get("settle"):
            key = r["runner"] + ((" +" + r["label"]) if r.get("label") else "")
            byrunner.setdefault(key, []).append(r["settle"])
    print("")
    print("  median settle by configuration:")
    for k, v in sorted(byrunner.items()):
        v.sort()
        print("    {:<30} {:6.1f}s   (n={}, range {:.1f}-{:.1f}s)".format(
            k, v[len(v) // 2], len(v), v[0], v[-1]))


def mode_report(data_file=None):
    """Generate diagnostic report from recorded JSON stream file."""
    if not data_file:
        data_file = os.path.join(DEFAULT_TELEMETRY_DIR, "latest.jsonl")
    if not os.path.exists(data_file):
        print(f"Telemetry data file not found at: {data_file}")
        return
    samples = JsonStreamReader.load_samples(data_file)
    bpftrace_summary = JsonStreamReader.load_bpftrace_summary(data_file)

    # Surface the session verdict before the statistics: a run that ended in a core
    # dump or sat deadlocked needs to be read differently from a slow-but-healthy one.
    roles, verdict, log_events, mem_maps = {}, None, [], []
    try:
        with open(data_file, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if ('"process_exit"' not in line and '"stall_detected"' not in line
                        and '"game_log_event"' not in line
                        and '"launch_diagnostics"' not in line and '"memory_maps"' not in line):
                    continue
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                if obj.get("event") == "game_log_event":
                    log_events.append(obj)
                if obj.get("event") in ("launch_diagnostics", "memory_maps"):
                    mm = obj.get("memory_maps") or obj.get("maps") or []
                    if mm:
                        mem_maps.extend(mm)
                if obj.get("event") == "process_exit":
                    roles = obj.get("thread_roles") or {}
                    verdict = obj.get("crash_analysis") or {}
                    verdict["_duration"] = obj.get("session_duration_s")
                    verdict["_effective"] = obj.get("effective_duration_s")
    except Exception:
        pass

    if verdict and (verdict.get("signature") or verdict.get("crashed")):
        print("=" * 90)
        print(f"   SESSION VERDICT: {verdict.get('signature') or 'CRASH'}")
        print("=" * 90)
        print(f"  {verdict.get('description')}")
        if verdict.get("recommendation"):
            print(f"  -> {verdict['recommendation']}")
        if verdict.get("_effective") and verdict.get("_duration") \
                and abs(verdict["_effective"] - verdict["_duration"]) > 1.0:
            print(f"  Session was {verdict['_duration']}s wall clock but only "
                  f"{verdict['_effective']}s of running game.")
        print()

    if not samples and not bpftrace_summary:
        print(f"No valid telemetry samples or eBPF data found in {data_file}.")
        return
    if bpftrace_summary is not None and mem_maps:
        bpftrace_summary["_memory_maps"] = mem_maps
    if samples:
        print(generate_telemetry_report(samples, bpftrace_data=bpftrace_summary, thread_roles=roles, events=log_events))
    elif bpftrace_summary:
        print(BpftraceEngine.format_summary(bpftrace_summary, thread_roles=roles))


# ------------------------------------------------------------------------------
# 14. CLI Parser & Entrypoint
# ------------------------------------------------------------------------------
def parse_args():
    parser = argparse.ArgumentParser(description="Proton / Wine Linux Game Performance Profiler & Launch Diagnostic Engine")
    parser.add_argument("mode", choices=["record", "report", "compare", "top", "live", "inspect", "diagnose", "bpftrace"], nargs="?", default="record",
                        help="Operation mode: record (streaming telemetry), report (analytics), top/live (dashboard), inspect (snapshot), diagnose (environment checks), bpftrace (eBPF kernel tracing)")
    parser.add_argument("--name", type=str, default=None, help="Target process binary name (e.g. wwm.exe, Cyberpunk2077.exe)")
    parser.add_argument("--pid", "--process-id", type=int, default=None, dest="pid", help="Target process ID directly as is")
    parser.add_argument("--wait", action="store_true", default=False, help="Wait for process to appear before profiling (cannot be used with --pid)")
    parser.add_argument("--rate", type=int, default=20, help="Sampling rate in Hz (default: 20 Hz)")
    parser.add_argument("--output-dir", type=str, default=DEFAULT_TELEMETRY_DIR, help="Directory to store streaming telemetry files (default: ./telemetry)")
    parser.add_argument("--data-file", type=str, default=None, help="Specific JSON stream file to read for 'report' mode")
    parser.add_argument("--duration", type=float, default=None, help="Record for specified duration in seconds then exit")
    parser.add_argument("--samples", type=int, default=None, help="Record for specified number of samples then exit")
    parser.add_argument("--exit-on-detach", action="store_true", default=False, help="Exit when target process terminates instead of waiting for next launch")
    parser.add_argument("--auth", choices=["auto", "pkexec", "sudo", "none"], default="auto", help="Privilege elevation method (default: auto)")
    parser.add_argument("--kstack", action="store_true", default=False,
                        help="Also read /proc/<tid>/stack for every thread. Expensive: it forces a "
                             "kernel stack unwind per thread per sample and duplicates wchan in >99%% "
                             "of samples. Off by default.")
    parser.add_argument("--game-log", type=str, default=None,
                        help="Path to the game's own log, used to timestamp phase events from the "
                             "line contents rather than from poll time")

    # Dynamic Linux eBPF / bpftrace Options
    parser.add_argument("--bpftrace", nargs="?", const="sync", default=None, metavar="PRESET",
                        help="Enable dynamic eBPF tracing with bpftrace. Preset: sync (default), futex, sched, ntsync, profile (99 Hz user-stack CPU profile), syscalls, all")
    parser.add_argument("--bpftrace-preset", choices=["sync", "futex", "sched", "syscalls", "ntsync", "profile", "profile-stacks", "all"], default="sync",
                        help="eBPF tracing preset (default: sync)")
    parser.add_argument("--bpftrace-script", type=str, default=None, help="Path to custom bpftrace script (.bt) to attach")
    parser.add_argument("--bpftrace-eval", type=str, default=None, help="Inline bpftrace script expression to evaluate")
    parser.add_argument("--bpftrace-output", type=str, default=None, help="Explicit destination file for bpftrace output")
    parser.add_argument("--bpftrace-flush", type=int, default=10, metavar="SECS",
                        help="Print and clear eBPF maps every N seconds so a crashed or killed run "
                             "still yields data (0 disables; default: 10)")

    args = parser.parse_args()

    # Validate mutual exclusivity of --wait and --pid
    if args.wait and args.pid is not None:
        parser.error("The '--wait' option cannot be requested together with '--pid' / '--process-id'.")

    return args


def main():
    args = parse_args()

    # Elevate privileges if running active sampling/monitoring or bpftrace
    if args.mode in ["record", "top", "live", "inspect", "diagnose", "bpftrace"]:
        check_and_elevate_privileges(args.auth)

    preset_choice = args.bpftrace if (args.bpftrace and args.bpftrace != "sync") else args.bpftrace_preset
    bpftrace_opts = {
        "enabled": (args.bpftrace is not None) or (args.mode == "bpftrace") or bool(args.bpftrace_script) or bool(args.bpftrace_eval),
        "preset": preset_choice,
        "script": args.bpftrace_script,
        "eval": args.bpftrace_eval,
        "output": args.bpftrace_output,
        "flush_secs": args.bpftrace_flush
    }

    if args.mode == "record":
        try:
            mode_record(
                target_name=args.name,
                target_pid=args.pid,
                sample_hz=args.rate,
                output_dir=args.output_dir,
                duration=args.duration,
                max_samples=args.samples,
                exit_on_detach=args.exit_on_detach,
                bpftrace_opts=bpftrace_opts,
                collect_kstack=args.kstack,
                game_log=args.game_log
            )
        except KeyboardInterrupt:
            print("\n[STOPPED] Telemetry recording finished.")
    elif args.mode == "bpftrace":
        mode_bpftrace(
            target_name=args.name,
            target_pid=args.pid,
            preset=preset_choice,
            script_file=args.bpftrace_script,
            eval_str=args.bpftrace_eval,
            output_file=args.bpftrace_output,
            duration=args.duration,
            output_dir=args.output_dir,
            flush_secs=args.bpftrace_flush
        )
    elif args.mode == "compare":
        mode_compare(output_dir=args.output_dir,
                     data_files=[args.data_file] if args.data_file else None)
    elif args.mode == "diagnose":
        mode_diagnose(target_name=args.name, target_pid=args.pid)
    elif args.mode == "report":
        mode_report(data_file=args.data_file)
    elif args.mode in ["top", "live"]:
        mode_top(target_name=args.name, target_pid=args.pid)
    elif args.mode == "inspect":
        mode_inspect(target_name=args.name, target_pid=args.pid)


if __name__ == "__main__":
    main()

