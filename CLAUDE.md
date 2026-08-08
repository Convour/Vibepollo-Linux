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

**This fork's specific purpose (Vibepollo-Linux):** upstream Vibepollo's active development is
almost entirely Windows-oriented — the Linux build path existed in CMake/CI config but had never
been build-tested end-to-end and did not compile out of the box. This fork's focus is the inverse:
get and keep Vibepollo genuinely working on Linux (developed/validated against Arch/CachyOS +
NVIDIA + KDE Plasma/Wayland), and close the gap on the Windows-only feature set listed under
"Vibepollo-specific feature map" below by mapping each one to a Linux-native mechanism rather than
treating it as a permanent platform limitation. See the README's "Closing the Windows-only feature
gap on Linux" section for the current mapping, and `docs/linux/AGENTS.md` /
`docs/linux/LEARNINGS.md` for build fixes and machine setup discovered along the way. This is a
personal fork tracking a different goal than upstream, not a request for upstream to change focus.

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

These are additions on top of upstream Sunshine/Apollo; upstream docs won't mention them. Three of
these are currently Windows-only; each has a portable core already, and this fork is actively
mapping them to Linux-native equivalents (see README "Closing the Windows-only feature gap on
Linux" and the plan tracked for this work) rather than leaving them permanently unavailable here.

**Playnite integration** (syncs Playnite's game library into Sunshine's app list) — **Windows
only; Linux equivalent in progress, targeting Steam first (no Linux port of Playnite exists, so
this is a new integration, not a port):**
`src/platform/windows/playnite_integration.cpp` (lifecycle owner), `playnite_ipc.cpp` /
`playnite_protocol.cpp` (named-pipe IPC to a Playnite-side plugin), `playnite_sync.cpp`
(reconciles Playnite library into `apps.json`), `src/config_playnite.cpp` (config),
`src/confighttp_playnite.cpp` (REST endpoints), `plugins/playnite/SunshinePlaynite/` (the
Playnite-side PowerShell plugin), `tools/playnite_launcher/` (standalone launch/cleanup helper
binary), `configs/tabs/Playnite.vue` (UI). The JSON protocol schema and `apps.json` reconciliation
logic in `playnite_protocol.cpp`/`playnite_sync.cpp` are platform-agnostic and are being
generalized into `src/library_sync/` so a Linux Steam-library backend (local `.vdf`/`.acf`
parsing, no companion process) can reuse them; Lutris is the planned second target.

**Virtual display / display automation** — **Windows only; Linux equivalent in progress,
formalizing a hand-validated `kscreen-doctor`/EDID workflow (see `docs/linux/LEARNINGS.md`
§11/§16-18) instead of reproducing the Windows driver model:** `src/display_device.cpp`
(cross-platform config-parsing abstraction; non-Windows platforms currently get explicit no-ops,
per the comment at the top of the file), `src/platform/windows/display_helper_integration.cpp`
+ `display_helper_coordinator.cpp` + `display_helper_watchdog.cpp` (out-of-process display
helper: apply/revert/retry, supervised), `virtual_display_sunshine.cpp` (bundled native driver,
default), `virtual_display_sudovda.cpp` (SudoVDA rollback backend), `virtual_display_legacy.cpp`
(pre-SudoVDA compat path), `virtual_display_cleanup.cpp` (crash recovery of the physical display
if Sunshine dies with a virtual display active). The Linux path (planned: `src/platform/linux/
display_device.cpp`) targets KDE Plasma/Wayland via `kscreen-doctor`, driving an EDID-injected
dummy output rather than a kernel-mode driver; the EDID/bootloader setup itself stays a documented
manual step, not something the daemon automates.

**RTSS & NVIDIA Control Panel frame limiting** — **Windows only; Linux equivalent in progress via
MangoHud + NVIDIA env vars (Linux capture already paces to the stream's target FPS, so the actual
gap is game-render-loop capping and vsync/prerender-limit toggles, not stream cadence):**
`src/platform/windows/rtss_integration.cpp` (RTSS profile/hook management via
`RTSSHooks(64).dll`), `frame_limiter_nvcp.cpp` (NVCP low-latency/vsync/frame-limit toggles),
`src/framegen_policy.h` (platform-agnostic policy computation, arbitrates which limiter applies —
reused as-is on Linux), `src/confighttp_rtss.cpp` (REST endpoints; JSON schema is generic enough
to serve a Linux backend without changes).

**Session history / host stats**: `src/session_history*.cpp` (recorded per-session stats),
`src/host_stats.cpp` (live CPU/GPU/mem sampling) — both feed the web UI's Stats view.

## This workspace

This repo lives in `~/Projects/`, alongside ~13 other independent personal
projects — see [`~/Projects/CLAUDE.md`](../CLAUDE.md) for the full index.
Related repos:

- [`~/Projects/game-streaming-wolf-sunshine-setup`](../game-streaming-wolf-sunshine-setup/CLAUDE.md)
  and [`~/Projects/GameStreaming-Restore`](../GameStreaming-Restore/CLAUDE.md)
  — the pre-existing native-Sunshine/Wolf dual setup on this same machine and
  its recovery history. This fork is the actively-developed longer-term
  replacement path for the Sunshine half of that setup, not a from-scratch
  reinvention — check those repos before re-solving something (port
  allocation, DualSense controller override) they already solved.
- [`~/Projects/Moonlight-TCL`](../Moonlight-TCL/CLAUDE.md) — client-side
  research for the same GameStream target (concluded infeasible on that
  specific TV; recommends a companion-device client instead).
- [`~/Projects/MyCachyOS`](../MyCachyOS/CLAUDE.md) — this machine's
  hardware/software spec archive (Arch/CachyOS + NVIDIA + KDE Plasma/Wayland,
  the exact target this fork's Linux build path is validated against).
