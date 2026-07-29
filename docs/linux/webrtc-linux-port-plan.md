# WebRTC browser streaming on Linux — scoping notes (2026-07-29)

**Status: not started, not yet tracked in the README's "Closing the Windows-only feature gap"
list.** This is a first-pass investigation into what porting `/webrtc` browser streaming to Linux
would actually take, done after discovering the running dev service returns
`Error: WebRTC: support is disabled at build time` (`src/webrtc_stream.cpp`) whenever a browser
client posts an SDP offer, because `SUNSHINE_ENABLE_WEBRTC` is Windows-only and off by default
(`cmake/prep/options.cmake:20`). Nothing here has been built or run yet — it's a map of the
territory so the actual work can be scoped into milestones, matching how virtual display / RTSS /
Playnite got a documented mapping before implementation (see README "Closing the Windows-only
feature gap on Linux").

**Headline finding: this is smaller than it looks at first glance.** `src/webrtc_stream.cpp` is
~5,940 lines and greps for D3D11/DXGI/COM look alarming, but the file already gates every
Windows-specific piece behind `#ifdef _WIN32` (and has a real `#ifdef __APPLE__` branch alongside
it) — the core signaling/session/SDP/data-channel logic outside those blocks is already
platform-agnostic. The two genuinely new pieces of work are (1) getting `libwebrtc` to build for
Linux at all, and (2) writing one Linux-specific function to hand capture frames to it. Everything
else Windows-only that the file touches is *optional* polish this fork already tracks as separate,
independent Linux-porting efforts.

## 1. The `libwebrtc` dependency

`third-party/libwebrtc` (submodule, currently uninitialized on this machine — `git submodule
status` shows a leading `-`) points at `https://github.com/Nonary/libwebrtc`, branch
`feat/upgrade-to-m137` (`.gitmodules`). This is Nonary's fork of the third-party
`webrtc-sdk/libwebrtc` project — a thin C API wrapper (`libwebrtc.h`/`libwebrtc_c.h`) around
Google's WebRTC C++ internals, built for flutter-webrtc-style desktop consumption.

Shallow-cloned it to inspect (not committed anywhere, just local investigation):

- **`BUILD.gn` already has real `is_linux` branches** (lines 15, 19, 54, 166, 213, 270 in that
  repo) for source selection, desktop-capture deps, etc. — this isn't Windows-only code that needs
  a rewrite; Linux was a first-class target upstream.
- **The wrapper's own `README.md` claims Linux x86/x64 support** and documents a build recipe:
  `gclient` checkout of `webrtc-sdk/webrtc.git@m125_release` with `target_os = ['linux']`, then
  `gn gen` + `ninja` with the wrapper copied in as a `//libwebrtc` GN target — structurally the
  same recipe `scripts/build_mingw_webrtc.ps1` already automates for Windows (see that script,
  `$gnArgs` around line 466), just without the MSVC toolchain patching / Windows SDK plumbing.
- **Caveat, and the actual #1 risk**: that Linux support is inherited from the upstream
  `webrtc-sdk/libwebrtc` project this was forked from — it is **not CI-validated in Nonary's own
  fork**. `.github/workflows/` in the wrapper repo has exactly two jobs: `release.yml` (Windows,
  `runs-on: windows-2022` — this is what `scripts/build_mingw_webrtc.ps1` and this fork's pinned
  `libwebrtc v1.0.1 artifacts` commits correspond to) and `webrtc-builds.yml` (macOS/iOS
  xcframework only — its `ubuntu-latest` job just drafts the GitHub release, it doesn't build
  anything). **Nobody has actually built this exact wrapper, on this exact branch
  (`feat/upgrade-to-m137`), for Linux.** The BUILD.gn branches being present is evidence it's
  *plausible*, not proof it currently builds clean.
- `cmake/dependencies/webrtc.cmake` already has a working non-`WIN32` branch
  (`find_library(WEBRTC_LIBRARY NAMES webrtc libwebrtc ...)`, line ~149) — but it's dead code on
  Linux today because it's only ever `include()`-d from `cmake/dependencies/windows.cmake:38`
  inside `if(SUNSHINE_ENABLE_WEBRTC)`. `cmake/dependencies/linux.cmake` has no equivalent include.

**First concrete milestone, before anything else is worth planning**: do a manual trial build —
`gclient sync` with `target_os=['linux']` against the pinned webrtc branch, patch in the
`//libwebrtc` GN target the same way the PowerShell script does, `ninja libwebrtc`. This answers
the only question that actually gates the rest of the plan. Expect this to be slow (webrtc's own
source tree is tens of GB via `gclient sync`, ninja build is likely 1–3 hours on this machine) but
disk isn't a constraint (253G free on `/home` right now).

## 2. What in `webrtc_stream.cpp` is actually Windows-only

Audited all ~25 `#ifdef _WIN32` blocks in the file. They fall into two buckets:

**Bucket A — the one real gap: frame delivery.** The video path ends in
`lwrtc_video_source_push_nv12(source, y_plane, stride_y, uv_plane, stride_uv, width, height,
timestamp)` (see the macOS path, `try_push_nv12_frame`, line ~3655, and the Windows path via
`D3D11Nv12Converter`, line ~3703). On Windows this is a full D3D11 GPU shader pass (texture →
NV12 render targets) because the Windows capture path hands over a raw D3D11 texture. On macOS
it's a straight read of an already-NV12 `CVPixelBuffer`'s plane pointers/strides — no conversion
needed. **This fork's existing Linux capture pipeline already produces NV12** for the classic
RTSP/NVENC path — see `nv12_img_t` / `import_target()` / `create_target()` in
`src/platform/linux/graphics.cpp` (EGL/dma-buf-backed NV12 textures) and `src/platform/linux/
cuda.h`. The Linux implementation almost certainly looks more like the macOS one-liner (read
existing NV12 planes) than the Windows one (do a GPU conversion from scratch) — the format
conversion this fork needs already exists for RTSP streaming; WebRTC would be hooking into an
existing NV12 producer, not building a new one.
  - Checked: `graphics.cpp`'s NV12 path (`nv12_img_t`, `import_target`/`create_target`) is
    GPU-texture-only — no `glReadPixels`/PBO/host-copy path exists there today (grepped for
    readback patterns, none found). `lwrtc_video_source_push_nv12` takes host pointers (see the
    macOS call site), so step 3 needs a GPU→host readback added, not just a pointer hand-off —
    this makes it more than a one-liner, though still far smaller than the Windows D3D11 shader
    conversion path it's replacing. Whether that readback can be cheap (PBO-based async, matching
    how the software/CPU encoder fallback path presumably already reads frames back for encoders
    that need host memory) needs a look at that fallback path before estimating step 3's size.

**Bucket B — optional polish, all already separately tracked in this fork.** Every other
`#ifdef _WIN32` block is layered on top of the core session, not part of it, and each maps
directly onto a Windows-only feature this fork is *already* porting independently per the README:
  - Virtual display prep/cleanup for WebRTC sessions (`prepare_virtual_display_for_webrtc_session`,
    `VDISPLAY::*`, output-override leases — lines 229, 2689, 2922, 3034, 3074, 3147) → this is the
    exact same gap as **Native Virtualized Display → `kscreen-doctor`**, already in progress.
  - RTSS/frame-limiter start/stop hooks around WebRTC sessions (lines 830, 2863, 5340) → the exact
    same gap as **RTSS/NVCP → MangoHud + NVIDIA env vars**, already in progress.
  - HDR request override via DXGI (`config::video.dd.hdr_request_override`, lines 2511, 2714) —
    Windows driver-level HDR toggle, no Linux equivalent planned yet anywhere in this fork; lowest
    priority, can just no-op on Linux like RTSP streaming already effectively does.
  - `platf::dxgi::current_display_adapter_name()` for session telemetry (line 5124) — cosmetic
    only (`session.state.stream_gpu_model`), trivial to wire to `nvidia-smi`/DRM later or leave
    blank.
  - `GetSystemMetrics(SM_CXVIRTUALSCREEN/...)` for a touch-input default resolution fallback
    (line 1005) — **already has a working non-Windows branch** (`#else` at line 1008: hardcoded
    1920x1080 default, "updated when actual capture dimensions are known"). Not blocking, just not
    great; low priority to improve.

None of Bucket B needs to be solved for a first working Linux WebRTC session — it can all
`#ifdef __linux__`-noop the same way it's currently just absent for non-Windows builds elsewhere,
and pick up real wiring later as the virtual-display and RTSS Linux ports land independently.

## 3. Rough milestone plan

1. **Prove the dependency builds.** Manual `gclient`/`gn`/`ninja` trial build of
   `Nonary/libwebrtc@feat/upgrade-to-m137` for Linux, outside CMake. Go/no-go gate for everything
   below — if this doesn't build clean, the whole plan needs to be rethought (patching the wrapper,
   or pinning an older/different webrtc-sdk/webrtc branch with better Linux support history).
2. **Wire the build.** `scripts/build_linux_webrtc.sh` (new, analogous to
   `build_mingw_webrtc.ps1` but simpler — no MSVC toolchain patching), plus an
   `if(SUNSHINE_ENABLE_WEBRTC) include(...webrtc.cmake) endif()` block added to
   `cmake/dependencies/linux.cmake` so the existing non-`WIN32` `find_library` branch in
   `webrtc.cmake` actually gets reached.
3. **Frame delivery.** New `#ifdef __linux__` branch in `webrtc_stream.cpp` mirroring
   `try_push_nv12_frame`, sourcing from the existing `graphics.cpp` NV12 pipeline. This is the
   only genuinely new C++ logic the port needs.
4. **End-to-end smoke test.** `SUNSHINE_ENABLE_WEBRTC=ON` build, real browser session against
   `/webrtc`, verify signaling/ICE/SDP negotiation (already cross-platform) actually gets frames
   on screen. Bucket B items stay `#ifdef _WIN32`-only / no-op on Linux at this stage.
5. **Opportunistic follow-up**, not blocking: once virtual-display and RTSS Linux ports land
   independently, revisit Bucket B call sites to wire them in for WebRTC sessions too.

Steps 1–2 are almost pure unknowns until tried; steps 3–4 are where estimate confidence is highest
given how contained the actual gap turned out to be. No code has been touched for this yet —
this file is the map, not the work.
