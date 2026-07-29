# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

Vibepollo is an AI-enhanced fork of Apollo (itself a fork of Sunshine) — a host-side C++ daemon
that captures the display and audio, injects input, and streams to Moonlight-compatible clients
or a built-in WebRTC browser client (`/webrtc`). The frontend is a Vue 3 + TypeScript SPA served
by the daemon. Per the project README, ~99% of the code is AI-generated under human-defined
architecture: cosmetic/stylistic debt is accepted deliberately, but architectural coherence
(API design, threading model, modularity) is treated as the thing that actually matters — hold
new code to that same bar rather than "fixing" style inconsistencies as a side effect of
unrelated changes.

**`architecture.md`** (repo root) is the authoritative deep-dive on runtime architecture (thread
model, mailbox coordination) and a full code-level walkthrough of the WebRTC streaming pipeline
(signaling, ICE, media bridging, data-channel input). Read it before working on streaming,
capture, or WebRTC code instead of re-deriving that flow from scratch.

## Build & Development Commands

### Clone
Submodules are required (~19, including googletest, libwebrtc, libdisplaydevice — see
`.gitmodules`):
```bash
git clone --recurse-submodules <url>
```

### C++ build
```bash
cmake -B build -G Ninja -S .
ninja -C build
```
This also builds the web UI: `cmake/targets/common.cmake` wires an `npm ci` + `npm run build`
step into the CMake target graph, so a full `ninja -C build` builds both halves. To iterate on
the UI alone, use `npm run dev` inside `src_assets/common/assets/web/` instead of rebuilding via
CMake each time.

Key options (`cmake/prep/options.cmake`):
- `SUNSHINE_ENABLE_WEBRTC` — Windows-only, OFF by default; needs `third-party/libwebrtc` +
  `third-party/depot_tools`, built via `scripts/build_mingw_webrtc.ps1`.
- Linux capture backends — `SUNSHINE_ENABLE_CUDA/DRM/VAAPI/VULKAN/WAYLAND/X11/KWIN/PORTAL`
  (default ON).
- `BUILD_WERROR`, `BUILD_DOCS`, `SUNSHINE_ENABLE_TRAY`.

### C++ unit tests are disabled by repository policy
`BUILD_TESTS` is force-set `OFF` in `cmake/prep/options.cmake` (`FORCE`, cannot be overridden via
`-D`). `docs/contributing.md` states explicitly: *"Unit tests are disabled by repository policy.
Do not add, compile, or run them... validate production changes by compiling the affected
production target and performing relevant runtime checks."* The `tests/` C++ CMake project is a
standalone gtest setup that is never wired into the main build — treat it as inert legacy, not a
place to add coverage. Do not propose adding or running C++ unit tests for this codebase.

### C++ formatting
```bash
python ./scripts/update_clang_format.py   # formats src/, tests/, tools/ in place per .clang-format
```
Lint deps (`clang-format`, `flake8`, pinned in `pyproject.toml`) install via
`pip install ".[lint]"`. Not enforced in CI — run manually when touching C++.

### Frontend (Vue/TS)
There is no root `package.json` — everything runs from `src_assets/common/assets/web/`:
```bash
cd src_assets/common/assets/web
npm ci
npm run dev          # vite build --mode debug --watch
npm run build        # production build
npm run lint         # eslint --max-warnings 0
npm run format        # prettier --write
npm run typecheck     # vue-tsc --noEmit
```
Frontend unit tests use Vitest, but the test files live at the repo root under
`tests/frontend/*.test.ts` (not colocated with the web app). Run/filter a single file from the
web dir:
```bash
npx vitest run ../../../../tests/frontend/navbar.test.ts
```

### CI reality check
CI (`ci-windows.yml`, `ci-archlinux.yml`, `ci-macos.yml`, `ci-freebsd.yml`) runs
`cmake -B build -G Ninja ... -DBUILD_TESTS=OFF` + `cmake --build build`; `ci-bundle.yml` runs
`npm ci --ignore-scripts`, `npm run l10n:audit`, `npm run build` for the frontend. Lint, format,
and vitest are **not** run in CI — they're manual/local-only checks. `docs/building.md` and
`docs/contributing.md` retain some upstream-inherited text (e.g. clone URLs pointing at the
upstream repo); where they conflict with the actual workflow YAML, the workflows are ground
truth for this fork.

## Architecture

- Single daemon process (`src/main.cpp`) starts long-lived service threads coordinated via a
  shared mailbox (`safe::mail_raw_t`): `nvhttp` (GameStream-compatible control plane),
  `confighttp` (Web UI + REST API + WebRTC signaling), `rtsp_stream` (classic Moonlight
  media/control plane). Full detail in `architecture.md`.
- Classic RTSP/GameStream streaming and WebRTC streaming are mutually exclusive at runtime
  (`rtsp_sessions_active` flag) — only one streaming mode runs at a time.
- `src/config.cpp`/`config.h` — the central `config::` struct, loaded from a `.conf` file;
  some settings are deferred and only take effect after the current stream ends.
- `src/http_auth.cpp` — session-cookie auth (`__Host-apollo_session`) and scoped bearer API
  tokens (per-path/method scoping). `authenticate()` in `src/confighttp.cpp` gates nearly every
  REST handler.
- Frontend (`src_assets/common/assets/web/`): Vue 3 + TS SPA, Naive UI component library, Pinia
  stores (`stores/`), typed API clients (`services/`), route table in `router.ts`, one file per
  settings tab under `configs/tabs/`.
- Config reference: `docs/configuration.md`. REST API reference: `docs/api.md`.

## Vibepollo-specific feature map

These are additions on top of upstream Sunshine/Apollo; upstream docs won't mention them.

**Playnite integration** (syncs Playnite's game library into Sunshine's app list):
`src/platform/windows/playnite_integration.cpp` (lifecycle owner), `playnite_ipc.cpp` /
`playnite_protocol.cpp` (named-pipe IPC to a Playnite-side plugin), `playnite_sync.cpp`
(reconciles Playnite library into `apps.json`), `src/config_playnite.cpp` (config),
`src/confighttp_playnite.cpp` (REST endpoints), `plugins/playnite/SunshinePlaynite/` (the
Playnite-side PowerShell plugin), `tools/playnite_launcher/` (standalone launch/cleanup helper
binary), `configs/tabs/Playnite.vue` (UI).

**Virtual display / display automation** (Windows only): `src/display_device.cpp`
(cross-platform abstraction that picks a backend), `src/platform/windows/display_helper_integration.cpp`
+ `display_helper_coordinator.cpp` + `display_helper_watchdog.cpp` (out-of-process display
helper: apply/revert/retry, supervised), `virtual_display_sunshine.cpp` (bundled native driver,
default), `virtual_display_sudovda.cpp` (SudoVDA rollback backend), `virtual_display_legacy.cpp`
(pre-SudoVDA compat path), `virtual_display_cleanup.cpp` (crash recovery of the physical display
if Sunshine dies with a virtual display active).

**RTSS & NVIDIA Control Panel frame limiting** (Windows only):
`src/platform/windows/rtss_integration.cpp` (RTSS profile/hook management via
`RTSSHooks(64).dll`), `frame_limiter_nvcp.cpp` (NVCP low-latency/vsync/frame-limit toggles),
`src/framegen_policy.h` (arbitrates which limiter applies), `src/confighttp_rtss.cpp` (REST
endpoints).

**Session history / host stats**: `src/session_history*.cpp` (recorded per-session stats),
`src/host_stats.cpp` (live CPU/GPU/mem sampling) — both feed the web UI's Stats view.
