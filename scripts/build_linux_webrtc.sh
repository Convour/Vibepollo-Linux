#!/usr/bin/env bash
# Builds the Nonary/libwebrtc C API wrapper for Linux, analogous to
# scripts/build_mingw_webrtc.ps1 but without any MSVC toolchain plumbing.
#
# See docs/linux/webrtc-linux-port-plan.md for the trial build this recipe
# came from, including why the two non-obvious steps below (dropping
# use_custom_libcxx=false, trimming 3 unused constants) are needed.
#
# Env var overrides (all optional):
#   VIBESHINE_DEPS_DIR   Shared cache root. Default: $XDG_CACHE_HOME/vibeshine/deps
#                         or ~/.cache/vibeshine/deps -- matches the default
#                         cmake/dependencies/webrtc.cmake looks for on Linux.
#   WEBRTC_BUILD_DIR      Working checkout dir. Default: $VIBESHINE_DEPS_DIR/libwebrtc/src
#   WEBRTC_OUT_DIR        Install dir (include/ + lib/). Default: $VIBESHINE_DEPS_DIR/libwebrtc/out
#   WEBRTC_CONFIGURATION  Debug or Release. Default: Debug (the only configuration
#                         actually build-tested so far -- see port plan doc).
#   WEBRTC_ARCH           x64, x86, arm, arm64. Default: x64.
#   WEBRTC_BRANCH         webrtc-sdk/webrtc branch. Default: m125_release.
#   WEBRTC_REPO_URL       webrtc-sdk/webrtc repo. Default: https://github.com/webrtc-sdk/webrtc.git
#   WEBRTC_GCLIENT_JOBS   gclient sync parallelism. Default: 16.
#   WEBRTC_DEPOT_TOOLS_DIR  Where to clone depot_tools. Default: $WEBRTC_BUILD_DIR/depot_tools
#                         (a fresh clone, not this repo's third-party/depot_tools submodule --
#                         same reasoning as the Windows script: keep this multi-hour build's
#                         state out of the repo's own submodule checkouts).
#   STAGE                 all | sync | build. Default: all.
set -euo pipefail

log() {
  echo "[webrtc] $*"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CONFIGURATION="${WEBRTC_CONFIGURATION:-Debug}"
ARCH="${WEBRTC_ARCH:-x64}"
WEBRTC_BRANCH="${WEBRTC_BRANCH:-m125_release}"
WEBRTC_REPO_URL="${WEBRTC_REPO_URL:-https://github.com/webrtc-sdk/webrtc.git}"
GCLIENT_JOBS="${WEBRTC_GCLIENT_JOBS:-16}"
STAGE="${STAGE:-all}"

if [ -n "${VIBESHINE_DEPS_DIR:-}" ]; then
  DEPS_ROOT="$VIBESHINE_DEPS_DIR"
elif [ -n "${XDG_CACHE_HOME:-}" ]; then
  DEPS_ROOT="$XDG_CACHE_HOME/vibeshine/deps"
else
  DEPS_ROOT="$HOME/.cache/vibeshine/deps"
fi

BUILD_DIR="${WEBRTC_BUILD_DIR:-$DEPS_ROOT/libwebrtc/src}"
OUT_DIR="${WEBRTC_OUT_DIR:-$DEPS_ROOT/libwebrtc/out}"
DEPOT_TOOLS_DIR="${WEBRTC_DEPOT_TOOLS_DIR:-$BUILD_DIR/depot_tools}"

log "Using libwebrtc build dir: $BUILD_DIR"
log "Using libwebrtc out dir:   $OUT_DIR"

mkdir -p "$BUILD_DIR"

if [ ! -d "$DEPOT_TOOLS_DIR" ]; then
  log "Cloning depot_tools"
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT_TOOLS_DIR"
fi

export PATH="$DEPOT_TOOLS_DIR:$PATH"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_METRICS=0

log "Bootstrapping depot_tools"
bash "$DEPOT_TOOLS_DIR/ensure_bootstrap"

if [ "$STAGE" = "all" ] || [ "$STAGE" = "sync" ]; then
  if [ -d "$BUILD_DIR/src/build/.git" ]; then
    log "Cleaning src/build before sync"
    git -C "$BUILD_DIR/src/build" reset --hard >/dev/null
    git -C "$BUILD_DIR/src/build" clean -fdx >/dev/null
  fi

  log "Writing .gclient"
  cat >"$BUILD_DIR/.gclient" <<EOF
solutions = [
  {
    "name"        : 'src',
    "url"         : '${WEBRTC_REPO_URL}@${WEBRTC_BRANCH}',
    "deps_file"   : 'DEPS',
    "managed"     : False,
    "custom_deps" : {
    },
    "custom_vars": {},
  },
]
target_os  = ['linux']
EOF

  log "Syncing WebRTC sources (tens of GB; can take a while on first run)"
  (cd "$BUILD_DIR" && gclient sync --jobs "$GCLIENT_JOBS")

  if [ "$STAGE" = "sync" ]; then
    log "WebRTC source sync complete."
    exit 0
  fi
elif [ ! -f "$BUILD_DIR/src/BUILD.gn" ]; then
  echo "WebRTC sources not found under $BUILD_DIR. Run with STAGE=sync first." >&2
  exit 1
fi

LOCAL_LIBWEBRTC="$ROOT_DIR/third-party/libwebrtc"
DEST_LIBWEBRTC="$BUILD_DIR/src/libwebrtc"
if [ ! -d "$LOCAL_LIBWEBRTC" ] || [ -z "$(ls -A "$LOCAL_LIBWEBRTC" 2>/dev/null)" ]; then
  echo "Local libwebrtc not found or empty: $LOCAL_LIBWEBRTC" >&2
  echo "Run: git submodule update --init third-party/libwebrtc" >&2
  exit 1
fi

log "Copying libwebrtc wrapper into checkout"
rm -rf "$DEST_LIBWEBRTC"
cp -r "$LOCAL_LIBWEBRTC" "$DEST_LIBWEBRTC"

# The wrapper's own passthrough_video_encoder.cc defines 3 NAL-type constants
# that are never referenced anywhere in the file; this build's -Werror
# -Wunused-const-variable turns that into a hard failure. Confirmed dead code
# via grep across the file, not a platform-specific issue.
PASSTHROUGH_ENCODER="$DEST_LIBWEBRTC/src/passthrough_video_encoder.cc"
if [ -f "$PASSTHROUGH_ENCODER" ] && grep -q 'kH264NalTypeIdr = 5;' "$PASSTHROUGH_ENCODER"; then
  log "Trimming unused NAL-type constants from passthrough_video_encoder.cc"
  sed -i \
    -e '/^constexpr uint8_t kH264NalTypeIdr = 5;$/d' \
    -e '/^constexpr uint8_t kHevcNalTypeIdrWRadl = 19;$/d' \
    -e '/^constexpr uint8_t kHevcNalTypeIdrNLp = 20;$/d' \
    "$PASSTHROUGH_ENCODER"
  # The sed patterns above are exact-line matches; if the wrapper reformats
  # this file upstream, the patterns silently stop matching and the build
  # fails later at -Werror with a much more confusing error. Fail loudly here
  # instead.
  if grep -q 'kH264NalTypeIdr = 5;' "$PASSTHROUGH_ENCODER"; then
    echo "Failed to trim unused NAL-type constants from $PASSTHROUGH_ENCODER (sed pattern no longer matches -- did the wrapper source change?)" >&2
    exit 1
  fi
fi

BUILD_GN="$BUILD_DIR/src/BUILD.gn"
if ! grep -q '//libwebrtc' "$BUILD_GN"; then
  log "Patching BUILD.gn to include libwebrtc"
  sed -i 's#deps = \[ ":webrtc" \]#deps = [ ":webrtc", "//libwebrtc" ]#' "$BUILD_GN"
  if ! grep -q '//libwebrtc' "$BUILD_GN"; then
    echo "Failed to patch $BUILD_GN to include //libwebrtc (sed pattern no longer matches -- did upstream webrtc's BUILD.gn change?)" >&2
    exit 1
  fi
fi

IS_DEBUG="false"
if [ "$CONFIGURATION" = "Debug" ]; then
  IS_DEBUG="true"
fi

# NOTE: deliberately omits use_custom_libcxx=false, which the upstream
# wrapper README's documented Linux recipe includes. That override fights
# Chromium's own Linux default (use_custom_libcxx=true, see
# build/config/c++/c++.gni) and triggers a GCC<=10 libstdc++
# is_constructible_v bug on aggregates containing deleted-default-ctor
# members (webrtc's TimeDelta/DataRate unit types). Using Chromium's bundled
# libc++ -- the way its own Linux builds already work -- avoids the bug
# entirely. See docs/linux/webrtc-linux-port-plan.md for the full trial.
GN_ARGS="target_os=\"linux\" target_cpu=\"$ARCH\" is_debug=$IS_DEBUG rtc_include_tests=false rtc_use_h264=true ffmpeg_branding=\"Chrome\" is_component_build=false use_rtti=true rtc_enable_protobuf=false"

GN_OUT_SUBDIR="out/linux-$ARCH"
GN_OUT_DIR="$BUILD_DIR/src/$GN_OUT_SUBDIR"

log "Generating GN build files"
(cd "$BUILD_DIR/src" && gn gen "$GN_OUT_SUBDIR" --args="$GN_ARGS")

log "Building libwebrtc"
(cd "$BUILD_DIR/src" && ninja -C "$GN_OUT_SUBDIR" libwebrtc)

mkdir -p "$OUT_DIR/include" "$OUT_DIR/lib"

log "Copying headers"
cp -r "$DEST_LIBWEBRTC/include/." "$OUT_DIR/include/"

SO_PATH="$GN_OUT_DIR/libwebrtc.so"
if [ ! -f "$SO_PATH" ]; then
  echo "libwebrtc.so not found at $SO_PATH" >&2
  exit 1
fi

log "Copying shared library"
cp -f "$SO_PATH" "$OUT_DIR/lib/libwebrtc.so"
if [ -f "$GN_OUT_DIR/libwebrtc.so.TOC" ]; then
  cp -f "$GN_OUT_DIR/libwebrtc.so.TOC" "$OUT_DIR/lib/libwebrtc.so.TOC"
fi

# NOTE: this copy to the CMake binary dir is NOT what makes libwebrtc.so
# loadable at runtime, unlike the Windows script's DLL copy. On Linux,
# find_library() in cmake/dependencies/webrtc.cmake resolves WEBRTC_LIBRARY
# to the full path under $OUT_DIR/lib set below, and CMake's default
# build-tree RPATH behavior embeds that full path into the sunshine binary
# automatically (verified 2026-07-29 -- see docs/linux/webrtc-linux-port-plan.md
# §1b -- no $ORIGIN RPATH or extra CMake wiring needed for local dev builds).
# This copy is just a convenience so tooling that expects a .so next to the
# binary (e.g. packaging, later) has one; the actual dynamic-linker discovery
# happens via $OUT_DIR/lib, not this location.
CMAKE_BINARY_DIR="$ROOT_DIR/build"
if [ -d "$CMAKE_BINARY_DIR" ]; then
  log "Copying shared library to CMake build directory (convenience copy only -- see NOTE above)"
  cp -f "$SO_PATH" "$CMAKE_BINARY_DIR/libwebrtc.so"
fi

log "Done. Set WEBRTC_ROOT to $OUT_DIR (or leave unset -- cmake/dependencies/webrtc.cmake finds this same default location automatically)."
