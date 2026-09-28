#!/usr/bin/env bash
# ==============================================================================
# Build & Deploy Script for GE-Proton 11 Custom
#
# Produces GE-Proton 11 plus this repository's custom patches:
#   * Wine is patched by GE's own patches/protonprep-valve-staging.sh (wine-staging,
#     GE hotfixes/game patches, and its CUSTOM RUNNER PATCHES section) inside a
#     disposable shadow tree, then compiled for x86_64 in the SteamRT SDK container.
#   * vkd3d-proton, low_latency_layer, lsteamclient, vrclient, the steam/umu helpers
#     and protonfixes are built/patched from shadow copies of their committed sources.
#   * The result is overlaid onto a stock GE-Proton 11 runner used as a template, then
#     checked: wineserver protocol parity and the storage-query regression test.
#
# Every submodule is built at the commit the main checkout's HEAD pins for it (nested
# submodules included), extracted into build/ with git archive; a missing commit is
# fetched into the submodule's object store. Checkouts are never modified: no git
# reset/clean/checkout/submodule update is run on them, and a checkout that differs from
# the pinned commit only produces a warning.
# ==============================================================================
set -euo pipefail

if [[ "${1:-}" =~ ^(-h|--help)$ ]]; then
    echo "Usage: [CLEAN_BUILD=0|1] ./build_runner.sh"
    echo ""
    echo "Options / Environment Variables:"
    echo "  CLEAN_BUILD=1 (default)  Wipe ${0%/*}/build, re-extract and re-patch all sources, reconfigure and compile."
    echo "  CLEAN_BUILD=0            Reuse the existing patched shadow tree and compile incrementally. Refuses to"
    echo "                           run if any source commit or patch changed since that tree was prepared."
    echo "  PREBUILT_BINARIES_SRC    Stock runner used as the template; must be the unmodified release this tree is (GE-Proton11-7)."
    echo "  RUNNER_DST               Target output path (default: ~/.local/share/lutris/runners/wine/GE-Proton11-custom)."
    exit 0
fi

PROTON_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_ROOT="${PROTON_SRC}/build"
BUILD_DIR="${BUILD_ROOT}/wine64"
SHADOW="${BUILD_ROOT}/proton-tree"   # mirrors the checkout layout protonprep expects
SRC_WINE="${SHADOW}/wine"
SRC_LOW_LATENCY="${BUILD_ROOT}/src-low_latency_layer"
SRC_VKD3D="${BUILD_ROOT}/src-vkd3d-proton"
SRC_FFMPEG="${BUILD_ROOT}/src-ffmpeg"
SRC_VULKAN_HEADERS="${BUILD_ROOT}/src-Vulkan-Headers"
SRC_VULKAN_UTILITY="${BUILD_ROOT}/src-Vulkan-Utility-Libraries"
SRC_VOSK="${BUILD_ROOT}/src-vosk-api"
BUILD_VKD3D="${BUILD_ROOT}/obj-vkd3d-x86_64"
STAGING_DIR="${BUILD_ROOT}/staging"
BUILD_FFMPEG="${BUILD_ROOT}/obj-ffmpeg-x86_64"
FFMPEG_PREFIX="${BUILD_ROOT}/ffmpeg-install"
INPUTS_STAMP="${BUILD_ROOT}/.inputs.sha256"
PROTONPREP_LOG="${BUILD_ROOT}/protonprep.log"
PROTONPREP="${PROTON_SRC}/patches/protonprep-valve-staging.sh"

# protonprep reverts this Valve commit (it removed the generated configure files that
# wine-staging regenerates) with `git revert`; the shadow tree gets the same revert.
WINE_CONFIGURE_REVERT="e813ca5771658b00875924ab88d525322e50d39f"

# protonprep steps that already fail on GE's own tree. GE's build does not stop on
# patch failures, so a released GE-Proton can ship with such a step partially applied;
# list exactly those (and their rejects) so the result matches GE. Anything else is fatal.
# Empty on GE-Proton11-7 + master (74177a03): the earlier ws2_32 connect-address failure
# applies cleanly there, and GE's 0062-winedmo-open-progressive-http-mp4-streams hunk,
# whose context missed a blank line, is fixed in our copy of the patch instead.
KNOWN_UPSTREAM_FAILED_STEPS=()
KNOWN_UPSTREAM_REJECTS=()

# GE patches that cannot build at this master commit and are not applied here. Each entry
# is a patch file basename; apply_patch_dir skips it and says so.
#   0010-lsteamclient-maintain-wayland-overlay-controller-focus.patch: GE commit 8c3c59f7
#   (2026-09-20) adds `#include "steam_overlay_focus.h"` and `overlay_focus::sync()` to
#   lsteamclient/unixlib.cpp but never committed that header, so lsteamclient fails to
#   compile with it. Wayland-only overlay controller focus; not needed on X11.
SKIP_UPSTREAM_PATCHES=(
    "0010-lsteamclient-maintain-wayland-overlay-controller-focus.patch"
)

# The template must be the stock GE-Proton release this source tree is (GE-Proton11-7):
# it supplies everything not built here, notably the whole 32-bit Wine, whose wineserver
# protocol must equal ours. A different release makes every 32-bit process fail with
# "wine client error: version mismatch" (11-6 speaks protocol 935, 11-7 938).
TEMPLATE_RELEASE="GE-Proton11-7"

# Steam's copy first: the Lutris copy of GE-Proton11-7 on this machine was modified by hand
# (a replaced ntdll.so, with the original kept as ntdll.so.orig).
find_default_prebuilt_binaries_src() {
    local dir
    for dir in "$HOME/.local/share/Steam/compatibilitytools.d/${TEMPLATE_RELEASE}-x86_64" \
               "$HOME/.local/share/Steam/compatibilitytools.d/${TEMPLATE_RELEASE}" \
               "${XDG_DATA_HOME:-$HOME/.local/share}/lutris/runners/wine/${TEMPLATE_RELEASE}-x86_64" \
               "${XDG_DATA_HOME:-$HOME/.local/share}/lutris/runners/wine/${TEMPLATE_RELEASE}"; do
        if [ -d "$dir" ]; then
            echo "$dir"
            return 0
        fi
    done
    echo "$HOME/.local/share/Steam/compatibilitytools.d/${TEMPLATE_RELEASE}-x86_64"
}

PREBUILT_BINARIES_SRC="${PREBUILT_BINARIES_SRC:-$(find_default_prebuilt_binaries_src)}"
RUNNER_DST="${RUNNER_DST:-${XDG_DATA_HOME:-$HOME/.local/share}/lutris/runners/wine/GE-Proton11-custom}"
STEAMRT_IMAGE="registry.gitlab.steamos.cloud/proton/steamrt4/sdk/x86_64:4.0.20260714.251823-0"
DOCKER_ENV=(-e CCACHE_DISABLE=1 -e CCACHE_DIR="${BUILD_ROOT}/tmp" -e XDG_CACHE_HOME="${BUILD_ROOT}/tmp" -e HOME="${BUILD_ROOT}/tmp" -e TMPDIR="${BUILD_ROOT}/tmp")

# ANSI Colors
BOLD='\033[1m'
G='\033[0;32m'
Y='\033[1;33m'
C='\033[0;36m'
R='\033[0;31m'
NC='\033[0m'

info()  { echo -e "${BOLD}${C}::${NC} $*"; }
ok()    { echo -e "${BOLD}${G}✓${NC} $*"; }
warn()  { echo -e "${BOLD}${Y}▲${NC} $*"; }
err()   { echo -e "${BOLD}${R}✗${NC} $*" >&2; }

# Apply one patch to a freshly extracted tree; any fuzz-free failure is fatal.
apply_patch_tree() {
    local target_dir="$1"
    local patch_file="$2"
    local patch_name
    patch_name="$(basename "$patch_file")"

    if patch -d "$target_dir" -Np1 --dry-run -s -f -i "$patch_file" >/dev/null; then
        patch -d "$target_dir" -Np1 -s -i "$patch_file"
        ok "Applied ${patch_name} to $(basename "$target_dir")."
        return 0
    fi

    err "Failed to apply patch ${patch_name} to $(basename "$target_dir")!"
    echo -e "\n${BOLD}${R}=== Patch Failure Diagnostic: ${patch_name} ===${NC}" >&2
    if ! patch -d "$target_dir" -Np1 --dry-run -i "$patch_file" >&2; then
        err "Dry-run failed: see diagnostic above."
    fi
    echo -e "${BOLD}${R}===================================================${NC}\n" >&2
    return 1
}

# The commit the main checkout's HEAD pins for submodule <path>.
pinned_commit() { git -C "${PROTON_SRC}" rev-parse "HEAD:$1"; }

# Make <sha> available in <repo> without touching its checkout.
ensure_commit() {
    local repo="$1" sha="$2"
    if ! git -C "$repo" cat-file -e "${sha}^{commit}" 2>/dev/null; then
        info "Fetching ${sha:0:12} into ${repo#"${PROTON_SRC}/"}..."
        git -C "$repo" fetch --depth=1 origin "$sha"
    fi
}

# Extract <sha> of <repo> into <dest>, then every nested submodule that commit pins and
# that is initialized in the checkout (uninitialized ones are skipped, as git archive does).
extract_tree() {
    local repo="$1" sha="$2" dest="$3" mode type sub path
    ensure_commit "$repo" "$sha"
    mkdir -p "$dest"
    git -C "$repo" archive "$sha" | tar -x -C "$dest"
    while read -r mode type sub path; do
        [ "$type" = "commit" ] || continue
        if [ -e "${repo}/${path}/.git" ]; then
            extract_tree "${repo}/${path}" "$sub" "${dest}/${path}"
        fi
    done < <(git -C "$repo" ls-tree -r "$sha")
}

# Extract submodule <path> at its pinned commit into <dest>.
extract_pinned() {
    local path="$1" dest="$2" sha head
    sha="$(pinned_commit "$path")"
    head="$(git -C "${PROTON_SRC}/${path}" rev-parse HEAD)"
    if [ "$head" != "$sha" ]; then
        warn "${path}/ is checked out at ${head:0:12}; building the commit HEAD pins, ${sha:0:12}."
    fi
    extract_tree "${PROTON_SRC}/${path}" "$sha" "$dest"
    ok "Extracted ${path} ${sha:0:12}."
}

apply_patch_dir() {
    local target_dir="$1"
    local patch_dir="$2"
    local p
    if [ -d "$patch_dir" ]; then
        for p in "$patch_dir"/*.patch; do
            if [ -f "$p" ]; then
                if in_list "$(basename "$p")" "${SKIP_UPSTREAM_PATCHES[@]}"; then
                    warn "Skipping $(basename "$p") (see SKIP_UPSTREAM_PATCHES)"
                    continue
                fi
                apply_patch_tree "$target_dir" "$p"
            fi
        done
    fi
}

# Fingerprint of everything the shadow trees are built from: source commits, the
# committed helper trees, and every patch (including protonprep itself).
inputs_fingerprint() {
    local repo
    {
        for repo in "${PINNED_SOURCES[@]}"; do
            printf '%s %s\n' "$repo" "$(pinned_commit "$repo")"
        done
        git -C "${PROTON_SRC}" rev-parse HEAD:wineopenxr HEAD:steam_helper HEAD:umu_helper HEAD:lsteamclient HEAD:vrclient_x64
        (cd "${PROTON_SRC}" && find patches -type f -print0 | sort -z | xargs -0 sha256sum)
        sha256sum "${PROTON_SRC}/build_runner.sh"
    } | sha256sum | cut -d' ' -f1
}

# Submodules the build reads; each is extracted at its pinned commit.
PINNED_SOURCES=(wine wine-staging vkd3d-proton vklayers/low_latency_layer vklayers/Vulkan-Utility-Libraries
                Vulkan-Headers protonfixes ffmpeg vosk-api)

in_list() {
    local needle="$1"; shift
    local item
    for item in "$@"; do
        if [ "$item" = "$needle" ]; then return 0; fi
    done
    return 1
}

# protonprep keeps GE's semantics (it does not stop on a failed patch), so check its
# log and the tree afterwards: every failure must be a known upstream one.
verify_protonprep_result() {
    local failures step msg rej fatal=0
    failures=$(awk '/^WINE: /{step=$0}
                    /FAILED|Reversed \(or previously applied\)|can.t find file to patch|malformed patch|Only garbage|ERROR:/ {print step "\t" $0}' \
               "$PROTONPREP_LOG")
    while IFS=$'\t' read -r step msg; do
        [ -n "$step$msg" ] || continue
        if in_list "$step" "${KNOWN_UPSTREAM_FAILED_STEPS[@]}"; then
            warn "Known upstream GE patch failure (tolerated, as in GE's build): ${step} -> ${msg}"
        else
            err "Patch failure in: ${step} -> ${msg}"
            fatal=1
        fi
    done <<< "$failures"

    while IFS= read -r rej; do
        [ -n "$rej" ] || continue
        rej="${rej#"${SRC_WINE}/"}"
        if ! in_list "$rej" "${KNOWN_UPSTREAM_REJECTS[@]}"; then
            err "Unexpected reject file: ${rej}"
            fatal=1
        fi
    done < <(find "$SRC_WINE" -name '*.rej')

    if [ "$fatal" = "1" ]; then
        err "Wine patching did not match GE's known result; full log: ${PROTONPREP_LOG}"
        return 1
    fi
    ok "Wine patched by protonprep (log: ${PROTONPREP_LOG})."
}

echo -e "\n${BOLD}${C}=== GE-Proton 11 Custom Build & Deployment Pipeline ===${NC}\n"

# 1. Dependency & Environment Checks
info "Verifying Docker and build environment..."
if ! command -v docker &>/dev/null; then
    err "Docker is required but not found in PATH."
    exit 1
fi

if ! docker ps &>/dev/null; then
    err "Cannot connect to Docker daemon. Please ensure docker is running."
    exit 1
fi

for tool in git patch python3 perl autoreconf sha256sum; do
    if ! command -v "$tool" &>/dev/null; then
        err "Required host tool not found: ${tool}"
        exit 1
    fi
done
ok "Docker environment and host tools verified."

info "Verifying Proton source tree at ${PROTON_SRC}..."
for dir in "${PINNED_SOURCES[@]}" wineopenxr; do
    if [ ! -d "${PROTON_SRC}/${dir}" ]; then
        err "Source directory not found: ${PROTON_SRC}/${dir}"
        exit 1
    fi
done

if [ ! -d "${PREBUILT_BINARIES_SRC}" ] || [ ! -f "${PREBUILT_BINARIES_SRC}/version" ]; then
    err "Stock GE-Proton template not found at ${PREBUILT_BINARIES_SRC}."
    exit 1
fi
TEMPLATE_VERSION="$(cat "${PREBUILT_BINARIES_SRC}/version")"
info "Template runner: ${PREBUILT_BINARIES_SRC} (${TEMPLATE_VERSION})"
if [ "${TEMPLATE_VERSION#* }" != "${TEMPLATE_RELEASE}" ]; then
    err "Template is ${TEMPLATE_VERSION#* }, but this source tree is ${TEMPLATE_RELEASE}; its 32-bit Wine would not match the wineserver built here."
    exit 1
fi
modified_files="$(find "${PREBUILT_BINARIES_SRC}/files" \( -name '*.orig' -o -name '*.bak' -o -name '*.rej' \) -print)"
if [ -n "${modified_files}" ]; then
    printf '%s\n' "${modified_files}" >&2
    err "Template ${PREBUILT_BINARIES_SRC} has been modified by hand (see the files above); point PREBUILT_BINARIES_SRC at an unmodified ${TEMPLATE_RELEASE}."
    exit 1
fi

# Every pending Wine patch must be wired into protonprep, or GE's build silently
# ships without it (AGENTS.md: keep protonprep aligned with the patch files).
for p in "${PROTON_SRC}"/patches/wine-hotfixes/pending/*.patch; do
    if ! grep -qF "wine-hotfixes/pending/$(basename "$p")" "$PROTONPREP"; then
        err "Pending patch not applied by protonprep-valve-staging.sh: $(basename "$p")"
        exit 1
    fi
done

# Only pinned commits are built; say so when there is uncommitted work in a submodule.
for repo in "${PINNED_SOURCES[@]}"; do
    if [ -n "$(git -C "${PROTON_SRC}/${repo}" status --porcelain --ignore-submodules=all)" ]; then
        warn "Uncommitted changes in ${repo}/ are NOT part of this build (only its pinned commit is built)."
    fi
done
if [ -n "$(git -C "${PROTON_SRC}" status --porcelain -- wineopenxr steam_helper umu_helper lsteamclient vrclient_x64)" ]; then
    warn "Uncommitted changes in wineopenxr/steam_helper/umu_helper/lsteamclient/vrclient_x64 are NOT part of this build."
fi
ok "Source tree, template and patch wiring verified."

# 2. Build Directory Initialization
CLEAN_BUILD="${CLEAN_BUILD:-1}"
INPUTS="$(inputs_fingerprint)"
if [ "${CLEAN_BUILD}" = "1" ] && [ -d "${BUILD_ROOT}" ]; then
    info "CLEAN_BUILD=1: wiping ${BUILD_ROOT} for a fresh build..."
    if ! rm -rf "${BUILD_ROOT}" 2>/dev/null; then
        if ! (chmod -R u+rwX "${BUILD_ROOT}" 2>/dev/null && rm -rf "${BUILD_ROOT}" 2>/dev/null); then
            if ! docker run --rm -v "${PROTON_SRC}:${PROTON_SRC}:rw" "${STEAMRT_IMAGE}" rm -rf "${BUILD_ROOT}"; then
                err "Failed to clean build directory: ${BUILD_ROOT}"
                exit 1
            fi
        fi
    fi
fi

NEED_PREP=0
if [ ! -d "${SRC_WINE}" ]; then
    NEED_PREP=1
    if [ -e "${BUILD_DIR}/Makefile" ]; then
        err "${BUILD_DIR} was configured against a different source tree; rebuild with CLEAN_BUILD=1."
        exit 1
    fi
elif [ ! -f "${INPUTS_STAMP}" ] || [ "$(cat "${INPUTS_STAMP}")" != "${INPUTS}" ]; then
    err "Sources or patches changed since ${SHADOW} was prepared; rebuild with CLEAN_BUILD=1."
    exit 1
else
    info "CLEAN_BUILD=0: reusing the patched shadow tree (inputs unchanged)."
fi

info "Ensuring Wine 64-bit build directory at ${BUILD_DIR}..."
mkdir -p "${BUILD_DIR}/tools" "${BUILD_DIR}/include" "${BUILD_DIR}/dlls" "${BUILD_ROOT}/tmp"

# 3. Shadow Source Trees & Patching
if [ "${NEED_PREP}" = "1" ]; then
    info "Extracting pinned sources into ${BUILD_ROOT}..."
    extract_pinned wine "${SRC_WINE}"
    extract_pinned wine-staging "${SHADOW}/wine-staging"
    extract_pinned ffmpeg "${SRC_FFMPEG}"
    extract_pinned Vulkan-Headers "${SRC_VULKAN_HEADERS}"
    extract_pinned vklayers/Vulkan-Utility-Libraries "${SRC_VULKAN_UTILITY}"
    extract_pinned vosk-api "${SRC_VOSK}"

    # Pre-create all matching module directories in build tree
    (cd "${SRC_WINE}" && find dlls libs programs -type d | while read -r d; do
        mkdir -p "${BUILD_DIR}/${d}" "${BUILD_DIR}/${d}/x86_64-windows" "${BUILD_DIR}/${d}/x86_64-unix"
    done)

    # protonprep addresses its inputs as ../patches, ../wine-staging and ../wineopenxr
    # relative to the wine tree (wine-staging was extracted there above).
    ln -s "${PROTON_SRC}/patches" "${SHADOW}/patches"

    # Main-repo helper sources at their committed state (protonprep restores them with
    # git checkout before patching), patched exactly as protonprep's prep section does.
    info "Extracting and patching wineopenxr, steam/umu helpers, lsteamclient and vrclient..."
    git -C "${PROTON_SRC}" archive HEAD wineopenxr steam_helper umu_helper lsteamclient vrclient_x64 | tar -x -C "${SHADOW}"
    apply_patch_dir "${SHADOW}/wineopenxr" "${PROTON_SRC}/patches/wineopenxr"
    apply_patch_dir "${SHADOW}" "${PROTON_SRC}/patches/discordrpc/helpers"
    apply_patch_dir "${SHADOW}" "${PROTON_SRC}/patches/lsteamclient"

    extract_pinned protonfixes "${SHADOW}/protonfixes"
    apply_patch_dir "${SHADOW}/protonfixes" "${PROTON_SRC}/patches/protonfixes"

    info "Reverting Valve commit ${WINE_CONFIGURE_REVERT:0:8} (restores configure files for wine-staging)..."
    if ! git -C "${PROTON_SRC}/wine" cat-file -e "${WINE_CONFIGURE_REVERT}^{commit}" 2>/dev/null; then
        info "Fetching ${WINE_CONFIGURE_REVERT:0:8} into the shallow wine clone..."
        git -C "${PROTON_SRC}/wine" fetch --depth=2 origin "${WINE_CONFIGURE_REVERT}"
    fi
    git -C "${PROTON_SRC}/wine" diff "${WINE_CONFIGURE_REVERT}^" "${WINE_CONFIGURE_REVERT}" \
        | patch -d "${SRC_WINE}" -R -p1 -s
    ok "Reverted ${WINE_CONFIGURE_REVERT:0:8}."

    # wine-staging's gitapply.sh hands patches to `git apply`. Inside this checkout
    # (build/ lives in the repo) git would resolve patch paths from the repo root and
    # silently skip them all, so stop git's repository discovery at BUILD_ROOT.
    info "Running protonprep-valve-staging.sh (wine-staging + GE patches + custom patches)..."
    if ! (cd "${SHADOW}" && GIT_CEILING_DIRECTORIES="${BUILD_ROOT}" PROTONPREP_WINE_ONLY=1 bash "${PROTONPREP}") > "${PROTONPREP_LOG}" 2>&1; then
        err "protonprep-valve-staging.sh exited with an error; full log: ${PROTONPREP_LOG}"
        tail -n 20 "${PROTONPREP_LOG}" >&2
        exit 1
    fi
    verify_protonprep_result

    # low_latency_layer and vkd3d-proton (with its nested khronos/ and subprojects/
    # submodules) at their pinned commits, then their patches.
    extract_pinned vklayers/low_latency_layer "${SRC_LOW_LATENCY}"
    apply_patch_dir "${SRC_LOW_LATENCY}" "${PROTON_SRC}/patches/vklayers"

    extract_pinned vkd3d-proton "${SRC_VKD3D}"
    apply_patch_dir "${SRC_VKD3D}" "${PROTON_SRC}/patches/vkd3d-proton"

    # 4. Protocol, Specfile & Vulkan Code Generation
    info "Generating Wine protocol requests, syscall headers, and Vulkan thunks in shadow source..."
    docker run --rm \
        -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
        -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
        -w "${SRC_WINE}" \
        "${DOCKER_ENV[@]}" \
        -u "$(id -u):$(id -g)" \
        "${STEAMRT_IMAGE}" /bin/bash -c "set -e; ./tools/make_specfiles && ./tools/make_requests && cd dlls/winevulkan && ./make_vulkan -x vk.xml -X video.xml && cd ../.. && autoconf && autoheader"

    CURRENT_PROTO_VER=$(grep -oP '#define SERVER_PROTOCOL_VERSION \K[0-9]+' "${SRC_WINE}/include/wine/server_protocol.h")
    ok "Wineserver protocol headers, syscall tables, Vulkan thunks, and configure generated in shadow source (Protocol Version: ${CURRENT_PROTO_VER})."

    echo "${INPUTS}" > "${INPUTS_STAMP}"
fi

# 5. FFmpeg (x86_64) from the ffmpeg submodule, as GE's Makefile.in builds it.
# GE's video rework routes Media Foundation playback through winedmo, which needs
# FFmpeg; without it configure silently builds a winedmo that cannot decode anything.
# This build only provides headers/libraries to compile and link against: at runtime
# winedmo uses the FFmpeg that the stock template ships (same submodule version).
if [ ! -f "${FFMPEG_PREFIX}/lib/pkgconfig/libavformat.pc" ]; then
    info "Building FFmpeg (x86_64) from the ffmpeg submodule for winedmo..."
    mkdir -p "${BUILD_FFMPEG}"
    docker run --rm \
        -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
        -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
        -w "${BUILD_FFMPEG}" \
        "${DOCKER_ENV[@]}" \
        -u "$(id -u):$(id -g)" \
        "${STEAMRT_IMAGE}" /bin/bash -c "
            set -e
            '${SRC_FFMPEG}/configure' --prefix='${FFMPEG_PREFIX}' \
                --enable-shared --disable-static --disable-everything --disable-programs \
                --disable-doc --disable-inline-asm --disable-x86asm --arch=x86_64 --target-os=linux
            make -j\$(nproc)
            make install
        "
    ok "Built FFmpeg into ${FFMPEG_PREFIX}."
fi

# 6. Wine 64-bit Configure
if [ ! -f "${BUILD_DIR}/Makefile" ]; then
    info "Configuring Wine 64-bit inside SteamRT SDK container..."
    docker run --rm \
        -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
        -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
        -v "${PREBUILT_BINARIES_SRC}:${PREBUILT_BINARIES_SRC}:ro" \
        -w "${BUILD_DIR}" \
        "${DOCKER_ENV[@]}" \
        -e PKG_CONFIG_PATH="${FFMPEG_PREFIX}/lib/pkgconfig" \
        -u "$(id -u):$(id -g)" \
        "${STEAMRT_IMAGE}" /bin/bash -c "${SRC_WINE}/configure --enable-win64 --disable-tests --prefix=/usr CFLAGS='-O2 -march=nocona -mtune=core-avx2 -fno-omit-frame-pointer -I${SRC_VOSK}/src' LDFLAGS='-L${PREBUILT_BINARIES_SRC}/files/lib/x86_64-linux-gnu -Wl,-rpath-link,${PREBUILT_BINARIES_SRC}/files/lib/x86_64-linux-gnu -Wl,-rpath-link,${FFMPEG_PREFIX}/lib'"
    ok "Configured Wine 64-bit."
fi
if ! grep -qE '^FFMPEG_LIBS *= *.*-lavformat' "${BUILD_DIR}/Makefile"; then
    err "Wine was configured without FFmpeg; winedmo would not be able to decode video."
    exit 1
fi

# 7. Full-Tree Compilation
info "Compiling full 64-bit Wine tree (using $(nproc) cores)..."
docker run --rm \
    -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
    -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
    -v "${PREBUILT_BINARIES_SRC}:${PREBUILT_BINARIES_SRC}:ro" \
    -w "${BUILD_DIR}" \
    "${DOCKER_ENV[@]}" \
    -u "$(id -u):$(id -g)" \
    "${STEAMRT_IMAGE}" /bin/bash -c "make -j\$(nproc)"
ok "Full Wine 64-bit build completed successfully."

# 8. Staging Installation (Installs Wine tree & ntdll.so required by auxiliary modules)
info "Installing compiled distribution into staging directory (${STAGING_DIR})..."
rm -rf "${STAGING_DIR}"
mkdir -p "${STAGING_DIR}"

docker run --rm \
    -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
    -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
    -w "${BUILD_DIR}" \
    "${DOCKER_ENV[@]}" \
    -u "$(id -u):$(id -g)" \
    "${STEAMRT_IMAGE}" /bin/bash -c "make install DESTDIR='${STAGING_DIR}'"
ok "Installed core Wine tree to staging."

WINEDMO_SO="${STAGING_DIR}/usr/lib/wine/x86_64-unix/winedmo.so"
if ! readelf -d "${WINEDMO_SO}" | grep -q 'libavformat\.so'; then
    err "winedmo.so is not linked against FFmpeg; video playback would be broken."
    exit 1
fi
ok "winedmo.so links FFmpeg."

# 9. Auxiliary Module Compilation (from the patched shadow copies)
info "Compiling auxiliary modules from source (lsteamclient, vrclient_x64, steam_helper, umu_helper)..."
docker run --rm \
    -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
    -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
    -v "${PREBUILT_BINARIES_SRC}:${PREBUILT_BINARIES_SRC}:ro" \
    -w "${BUILD_DIR}" \
    "${DOCKER_ENV[@]}" \
    -u "$(id -u):$(id -g)" \
    "${STEAMRT_IMAGE}" /bin/bash -c "
        set -e
        for pkg in steam_helper umu_helper lsteamclient vrclient_x64; do
            src_dir=\"${SHADOW}/\${pkg}\"
            obj_dir=\"${BUILD_ROOT}/obj-\${pkg}-x86_64\"
            mkdir -p \"\${obj_dir}\"
            inc_flags=\"-I${BUILD_DIR}/include -I${SRC_WINE}/include\"
            if [ \"\${pkg}\" = \"vrclient_x64\" ]; then
                inc_flags=\"\${inc_flags} -I${SRC_VULKAN_HEADERS}/include\"
            fi
            sed -e \"1 i\\\\UNIX_LIBS = ${STAGING_DIR}/usr/lib/wine/x86_64-unix/ntdll.so\\\\n\" \
                -e \"/^all:\$/,\\\$c all:\" \
                -e \"/^SUBDIRS/,/[^\\\\\\\\]\$/c SUBDIRS = \${src_dir}\" \
                -e \"/^TOP_INSTALL_LIB/c TOP_INSTALL_LIB = dlls/src-\${pkg}\" \
                -e \"/^srcdir/a objdir = ${BUILD_DIR}\" \
                -e \"/^prefix/c prefix = ${STAGING_DIR}/usr\" \
                -e \"/^libdir/c libdir = ${STAGING_DIR}/usr/lib\" \
                -e \"/^toolsdir/c toolsdir = ${BUILD_DIR}\" \
                -e \"/^CFLAGS/c CFLAGS = \${inc_flags}\" \
                -e \"/^CPPFLAGS/c CPPFLAGS = \${inc_flags}\" \
                -e \"/^CXXFLAGS/c CXXFLAGS = \${inc_flags} -std=c++17\" \
                -e \"/^x86_64_CFLAGS/c x86_64_CFLAGS = \${inc_flags} -I${SRC_WINE}/include/msvcrt -mcmodel=small -march=nocona -mtune=core-avx2 -O2\" \
                -e \"/^x86_64_CXXFLAGS/c x86_64_CXXFLAGS = \${inc_flags} -mcmodel=small -march=nocona -mtune=core-avx2 -O2 -std=c++17\" \
                ${BUILD_DIR}/Makefile > \"\${obj_dir}/Makefile\"
            cd \"\${obj_dir}\"
            touch config.status
            ${BUILD_DIR}/tools/makedep
            make -j\$(nproc)
        done
    "
ok "Compiled all source auxiliary modules."

# 10. Vulkan Layers Compilation (low_latency_layer from source)
if [ -d "${SRC_VULKAN_UTILITY}" ]; then
    info "Compiling low_latency_layer from source with custom patches..."
    docker run --rm \
        -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
        -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
        "${DOCKER_ENV[@]}" \
        -u "$(id -u):$(id -g)" \
        "${STEAMRT_IMAGE}" /bin/bash -c "
            set -e
            cmake -B \"${BUILD_ROOT}/obj-vulkan-utility-libraries-x86_64\" \
                -S \"${SRC_VULKAN_UTILITY}\" \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_INSTALL_PREFIX=\"${BUILD_ROOT}/vulkan-utility-libraries-install\" \
                -DVULKAN_HEADERS_INSTALL_DIR=\"${SRC_VULKAN_HEADERS}\"
            cmake --build \"${BUILD_ROOT}/obj-vulkan-utility-libraries-x86_64\" -j\$(nproc) --target install

            cmake -B \"${BUILD_ROOT}/obj-low_latency_layer-x86_64\" \
                -S \"${SRC_LOW_LATENCY}\" \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_PREFIX_PATH=\"${BUILD_ROOT}/vulkan-utility-libraries-install;${SRC_VULKAN_HEADERS}\" \
                -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
                -DCMAKE_INSTALL_DATADIR=share/low_latency_layer
            cmake --build \"${BUILD_ROOT}/obj-low_latency_layer-x86_64\" -j\$(nproc)
        "
    ok "Compiled low_latency_layer from source."
fi

# 11. Compile vkd3d-proton 64-bit from source
info "Configuring and compiling vkd3d-proton 64-bit from source..."
mkdir -p "${BUILD_VKD3D}"
docker run --rm \
    -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
    -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
    "${DOCKER_ENV[@]}" \
    -w "${BUILD_VKD3D}" \
    -u "$(id -u):$(id -g)" \
    "${STEAMRT_IMAGE}" /bin/bash -c "
        set -e
        if [ ! -f build.ninja ]; then
            meson setup . '${SRC_VKD3D}' \
                --cross-file '${SRC_VKD3D}/build-win64.txt' \
                --buildtype release \
                --strip \
                -Db_ndebug=true \
                -Denable_extended_emulation=true
        fi
        ninja -j\$(nproc)
    "
ok "Compiled vkd3d-proton 64-bit from source."

# Stage auxiliary module binaries
cp -f "${BUILD_ROOT}/obj-steam_helper-x86_64/dlls/steam_helper/x86_64-windows/steam.exe" "${STAGING_DIR}/usr/lib/wine/x86_64-windows/"
cp -f "${BUILD_ROOT}/obj-umu_helper-x86_64/dlls/umu_helper/x86_64-windows/umu.exe" "${STAGING_DIR}/usr/lib/wine/x86_64-windows/"
cp -f "${BUILD_ROOT}/obj-lsteamclient-x86_64/dlls/lsteamclient/x86_64-windows/lsteamclient.dll" "${STAGING_DIR}/usr/lib/wine/x86_64-windows/"
cp -f "${BUILD_ROOT}/obj-lsteamclient-x86_64/dlls/lsteamclient/lsteamclient.so" "${STAGING_DIR}/usr/lib/wine/x86_64-unix/"
cp -f "${BUILD_ROOT}/obj-vrclient_x64-x86_64/dlls/vrclient_x64/x86_64-windows/vrclient_x64.dll" "${STAGING_DIR}/usr/lib/wine/x86_64-windows/"
cp -f "${BUILD_ROOT}/obj-vrclient_x64-x86_64/dlls/vrclient_x64/vrclient_x64.so" "${STAGING_DIR}/usr/lib/wine/x86_64-unix/vrclient_x64.dll.so"
ok "Staged compiled binaries."

# 12. Refuse to replace a runner that is in use
info "Checking for processes using the target runner..."
if [ -d "${RUNNER_DST}" ] && command -v fuser &>/dev/null; then
    for bin_path in "${RUNNER_DST}/files/bin/wineserver" "${RUNNER_DST}/files/lib/wine/x86_64-unix/wine-preloader"; do
        if [ -f "${bin_path}" ] && fuser "${bin_path}" &>/dev/null; then
            err "${bin_path} is in use; quit the game/wineserver before deploying. Build output is kept in ${STAGING_DIR}."
            exit 1
        fi
    done
fi
ok "Target runner is not in use."

# 13. Runner Packaging: stock GE-Proton template + overlay of everything built here
info "Assembling runner package in ${RUNNER_DST}..."
rm -rf "${RUNNER_DST}"
cp -a "${PREBUILT_BINARIES_SRC}" "${RUNNER_DST}"

cp -rf "${STAGING_DIR}/usr/bin/"* "${RUNNER_DST}/files/bin/"
cp -rf "${STAGING_DIR}/usr/lib/wine/x86_64-unix/"* "${RUNNER_DST}/files/lib/wine/x86_64-unix/"
cp -rf "${STAGING_DIR}/usr/lib/wine/x86_64-windows/"* "${RUNNER_DST}/files/lib/wine/x86_64-windows/"
cp -rf "${STAGING_DIR}/usr/share/wine/"* "${RUNNER_DST}/files/share/wine/"

if [ -f "${BUILD_ROOT}/obj-low_latency_layer-x86_64/libVkLayer_KORTHOS_LowLatency.so" ]; then
    mkdir -p "${RUNNER_DST}/files/lib/x86_64-linux-gnu"
    cp -f "${BUILD_ROOT}/obj-low_latency_layer-x86_64/libVkLayer_KORTHOS_LowLatency.so" "${RUNNER_DST}/files/lib/x86_64-linux-gnu/"
fi
if [ -f "${BUILD_ROOT}/obj-low_latency_layer-x86_64/low_latency_layer.json" ]; then
    mkdir -p "${RUNNER_DST}/files/share/low_latency_layer/implicit_layer.d"
    sed 's|"library_path": ".*libVkLayer_KORTHOS_LowLatency.so"|"library_path": "libVkLayer_KORTHOS_LowLatency.so"|g' \
        "${BUILD_ROOT}/obj-low_latency_layer-x86_64/low_latency_layer.json" > "${RUNNER_DST}/files/share/low_latency_layer/implicit_layer.d/low_latency_layer.json"
fi

# vkd3d-proton goes where stock GE keeps it; the proton script installs it into the
# prefix from there. Wine's own d3d12/d3d11/dxgi builtins stay untouched, as in GE.
info "Deploying compiled vkd3d-proton 64-bit binaries..."
mkdir -p "${RUNNER_DST}/files/lib/wine/vkd3d-proton/x86_64-windows"
cp -f "${BUILD_VKD3D}/libs/d3d12/d3d12.dll" "${RUNNER_DST}/files/lib/wine/vkd3d-proton/x86_64-windows/"
cp -f "${BUILD_VKD3D}/libs/d3d12core/d3d12core.dll" "${RUNNER_DST}/files/lib/wine/vkd3d-proton/x86_64-windows/"
ok "Deployed compiled vkd3d-proton (d3d12.dll, d3d12core.dll)."

# winedmo was linked against our FFmpeg build but runs against the FFmpeg the template
# ships; make sure every FFmpeg library it needs is actually in the runner.
while read -r lib; do
    if [ ! -e "${RUNNER_DST}/files/lib/x86_64-linux-gnu/${lib}" ]; then
        err "winedmo.so needs ${lib}, which the runner does not ship."
        exit 1
    fi
done < <(readelf -d "${WINEDMO_SO}" | sed -n 's/.*Shared library: \[\(lib\(av\|sw\)[^]]*\)\].*/\1/p')
ok "Runner ships the FFmpeg libraries winedmo.so needs."

# protonfixes: overlay the patched Python sources onto the template's installed
# protonfixes. Only regular .py files: the source tree also carries repo files and
# symlinks into nested submodules that are not checked out (umu-database.csv), which
# would replace the template's real files with dangling links.
(cd "${SHADOW}/protonfixes" && find . -type f -name '*.py' -print0) \
    | while IFS= read -r -d '' py; do
        install -D -m 644 "${SHADOW}/protonfixes/${py}" "${RUNNER_DST}/protonfixes/${py}"
    done
if [ -f "${PROTON_SRC}/user_settings.py" ]; then
    cp -f "${PROTON_SRC}/user_settings.py" "${RUNNER_DST}/"
fi

echo "${TEMPLATE_VERSION%% *} ${TEMPLATE_VERSION#* }-custom" > "${RUNNER_DST}/version"
if [ -f "${RUNNER_DST}/compatibilitytool.vdf" ]; then
    sed -i 's/GE-Proton11-[^"]*/GE-Proton11-custom/g' "${RUNNER_DST}/compatibilitytool.vdf"
fi
ok "Packaged full Wine distribution into ${RUNNER_DST}."

# 14. Automated Sandbox Smoke Test
info "Running automated smoke test on newly built runner..."
WINESERVER_BIN="${RUNNER_DST}/files/bin/wineserver"
WINELOADER_BIN="${RUNNER_DST}/files/bin/wine"

if [ ! -x "${WINESERVER_BIN}" ] || [ ! -x "${WINELOADER_BIN}" ]; then
    err "Required runner binaries missing in ${RUNNER_DST}."
    exit 1
fi

if ! WINESERVER_VER=$("${WINESERVER_BIN}" --version 2>&1); then
    err "Smoke test failed: wineserver execution failed: ${WINESERVER_VER}"
    exit 1
fi

if ! WINE_VER=$("${WINELOADER_BIN}" --version 2>&1); then
    err "Smoke test failed: wine loader execution failed: ${WINE_VER}"
    exit 1
fi

ok "Smoke test passed: ${WINESERVER_VER} / ${WINE_VER}"

# Protocol parity: initialising a prefix starts 64-bit and 32-bit Wine processes, so
# a template whose 32-bit Wine speaks another wineserver protocol shows up here as
# "version mismatch" instead of as a game that silently misbehaves.
info "Checking wineserver protocol parity (64-bit and 32-bit clients)..."
PARITY_PFX="${BUILD_ROOT}/parity-prefix"
rm -rf "${PARITY_PFX}"
mkdir -p "${PARITY_PFX}"
PARITY_LOG="${BUILD_ROOT}/parity.log"
parity_rc=0
WINEPREFIX="${PARITY_PFX}" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    timeout 120 "${WINELOADER_BIN}" cmd /c exit > "${PARITY_LOG}" 2>&1 || parity_rc=$?
# -w (wait for the prefix's wineserver to exit) returns 0 even when none is running.
WINEPREFIX="${PARITY_PFX}" "${WINESERVER_BIN}" -w
if grep -q "version mismatch" "${PARITY_LOG}"; then
    grep "version mismatch" "${PARITY_LOG}" >&2
    err "Mixed wineserver protocols in ${RUNNER_DST}; the template does not match this build."
    exit 1
fi
if [ "${parity_rc}" != "0" ]; then
    err "Wine failed to initialise a test prefix (exit ${parity_rc}); see ${PARITY_LOG}"
    exit 1
fi
ok "All Wine clients speak the wineserver protocol of this build."

# Regression check: the storage answers Where Winds Meet's streaming depends on
# (docs/research_notes.md §36). Without StorageDeviceTrimProperty its teleport load
# takes ~47 s instead of ~12 s.
info "Checking storage-query answers (StorageDeviceTrimProperty, tests/test_storage.c)..."
docker run --rm \
    -v "${PROTON_SRC}:${PROTON_SRC}:ro" \
    -v "${BUILD_ROOT}:${BUILD_ROOT}:rw" \
    "${DOCKER_ENV[@]}" \
    -u "$(id -u):$(id -g)" \
    "${STEAMRT_IMAGE}" x86_64-w64-mingw32-gcc -O2 -Wall -o "${BUILD_ROOT}/test_storage.exe" "${PROTON_SRC}/tests/test_storage.c"
STORAGE_LOG="${PARITY_PFX}/drive_c/test_storage.log"
storage_rc=0
WINEPREFIX="${PARITY_PFX}" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" \
    timeout 120 "${WINELOADER_BIN}" "${BUILD_ROOT}/test_storage.exe" --log 'C:\test_storage.log' '\\.\C:' \
    > "${BUILD_ROOT}/test_storage.wine.log" 2>&1 || storage_rc=$?
WINEPREFIX="${PARITY_PFX}" "${WINESERVER_BIN}" -w
if [ "${storage_rc}" != "0" ] || [ ! -f "${STORAGE_LOG}" ]; then
    err "test_storage.exe failed (exit ${storage_rc}); see ${BUILD_ROOT}/test_storage.wine.log"
    exit 1
fi
if ! grep -q 'TrimEnabled=1' "${STORAGE_LOG}"; then
    cat "${STORAGE_LOG}" >&2
    err "The runner does not report StorageDeviceTrimProperty (TrimEnabled); patch 0006-mountmgr-report-storage-trim-property is missing or broken."
    exit 1
fi
ok "mountmgr reports TrimEnabled=1 and IncursSeekPenalty=$(grep -oP 'IncursSeekPenalty=\K[0-9]' "${STORAGE_LOG}" | head -1)."
rm -rf "${PARITY_PFX}"

# 15. Link into Steam compatibilitytools.d
STEAM_COMPAT_DIR="${HOME}/.local/share/Steam/compatibilitytools.d"
mkdir -p "${STEAM_COMPAT_DIR}"
ln -sfn "${RUNNER_DST}" "${STEAM_COMPAT_DIR}/GE-Proton11-custom"
ok "Linked runner to Steam compatibility tools: ${STEAM_COMPAT_DIR}/GE-Proton11-custom"

echo -e "\n${BOLD}${G}=== Full-Tree Build & Deployment Completed Successfully! ===${NC}\n"
echo -e "Runner Directory: ${RUNNER_DST}"
echo -e "Template:         ${PREBUILT_BINARIES_SRC}"
echo -e "Version Tag:      $(cat "${RUNNER_DST}/version")\n"
