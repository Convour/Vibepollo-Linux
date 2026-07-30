# WebRTC browser streaming on Linux — scoping notes (2026-07-29, updated 2026-07-29)

**Status: milestones 1-2 done; milestone 3's premise turned out to be wrong (see §2b, no new
frame-delivery code needed); the RPATH gap milestone 4 was thought to be blocked on turned out not
to exist either (see §1b retest).** `libwebrtc` builds clean for Linux, the CMake wiring finds and
links it, `src/webrtc_stream.cpp` compiles with `SUNSHINE_ENABLE_WEBRTC=1` on GCC/Linux with zero
changes needed to that file, and a full `sunshine` binary linked against it actually runs and
loads `libwebrtc.so` at runtime out of the box. This started as a first-pass investigation into
what porting `/webrtc` browser streaming to Linux would actually take, done after discovering the
running dev service returns `Error: WebRTC: support is disabled at build time`
(`src/webrtc_stream.cpp`) whenever a browser client posts an SDP offer, because
`SUNSHINE_ENABLE_WEBRTC` is Windows-only and off by default (`cmake/prep/options.cmake:20`). What
remains is an actual browser smoke test (milestone 4) — see "Rough milestone plan".

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

### Trial build results (2026-07-29) — it builds

Ran the full recipe outside CMake in `~/vibeshine-webrtc-linux-trial` (not committed, outside the
repo): submodules initialized, `.gclient` written for
`https://github.com/webrtc-sdk/webrtc.git@m125_release` with `target_os=['linux']`, `gclient sync
--jobs 16` (clean, ~24G synced, ~5 min on this connection — much faster than the "1-3 hours"
estimate above suggested, since that estimate was for the ninja build, not sync), wrapper copied to
`src/libwebrtc`, `BUILD.gn` patched per the wrapper's own README diff. **Result: `libwebrtc.so`
(137M, valid ELF, `ldd` reports no missing deps) built successfully**, exporting
`lwrtc_video_source_push_nv12` and the rest of the C API — confirmed via `nm -D`.

Two fixes were needed versus the wrapper README's documented Linux recipe verbatim, neither of
which touches upstream webrtc source:

1. **Drop `use_custom_libcxx=false` from the `gn gen` args.** The README's Linux recipe sets this
   explicitly, but `build/config/c++/c++.gni` shows `use_custom_libcxx` already defaults to `true`
   on Linux — Chromium/WebRTC's own Linux builds always use their bundled libc++, never the
   sysroot's system libstdc++. Building with the README's literal args (`use_custom_libcxx=false`)
   fails at 655/3877 objects in `modules/congestion_controller/goog_cc/loss_based_bwe_v2.cc:552`
   (`config.emplace()`) with `error: no matching member function for call to 'emplace'` — a known
   class of GCC ≤10 `is_constructible_v` bug for aggregates that contain members of a type with an
   explicitly deleted default constructor (`TimeDelta`/`DataRate`, see
   `api/units/time_delta.h:56`) but a default member initializer (`= TimeDelta::Zero()`). The
   Debian bullseye sysroot's bundled libstdc++ 10 headers evaluate this incorrectly; Chromium's own
   libc++ does not have the bug. Removing the override (falling back to the Linux default of
   `true`) fixed it — confirmed by rebuilding just that one object file before re-running the full
   build.
2. **Trim 3 unused NAL-type constants from the wrapper's own
   `libwebrtc/src/passthrough_video_encoder.cc`.** `kH264NalTypeIdr`, `kHevcNalTypeIdrWRadl`,
   `kHevcNalTypeIdrNLp` are defined but never referenced anywhere in that file, and the build's
   `-Werror -Wunused-const-variable` turns that into a hard failure at 2289/3944 objects (progress
   at that point was already through 100% of upstream webrtc's own source — this was the wrapper's
   own code). This is dead code, not a platform issue; deleting the 3 lines was sufficient. Applied
   only to the trial checkout's copy, not the pinned submodule — needs a real patch (see milestone
   plan below) before this is a repeatable build.

Working `gn gen` args for Linux (x64, debug):
```
target_os="linux" target_cpu="x64" is_debug=true rtc_include_tests=false rtc_use_h264=true \
ffmpeg_branding="Chrome" is_component_build=false use_rtti=true rtc_enable_protobuf=false
```
(same as the README's, minus `use_custom_libcxx=false`.) Not yet tried: a release build
(`is_debug=false`), 32-bit/arm targets, or size/perf comparison against the Windows DLL — this
trial only proves the debug x64 shared object links and exports the expected symbols.

## 1b. CMake wiring and a real compile test (2026-07-29)

`scripts/build_linux_webrtc.sh` now exists — a from-scratch bash port of
`build_mingw_webrtc.ps1` baking in both fixes from §1 (drops `use_custom_libcxx=false`; sed-trims
the 3 dead constants in a fresh copy of the wrapper, hard-failing if the patterns stop matching so
this doesn't silently regress into a confusing `-Werror` failure later). **This script itself has
not been run end-to-end** — only its individual steps were verified by hand during the trial
build in §1 and the CMake test below, which reused those trial artifacts rather than re-running a
multi-hour sync/build. `cmake/dependencies/linux.cmake` now has the same
`if(SUNSHINE_ENABLE_WEBRTC) include(webrtc.cmake) endif()` block `windows.cmake` already had, and
`webrtc.cmake`'s default `WEBRTC_ROOT` resolution grew a Linux-parity fallback
(`~/.cache/vibeshine/deps`, XDG-aware) matching the Windows `%LOCALAPPDATA%` default.

**Found and fixed a real, pre-existing bug in `webrtc.cmake` while verifying this** (affects the
Windows path too, not just this Linux addition): `WEBRTC_LIBRARY`/`WEBRTC_INCLUDE_DIR` were
pre-declared as empty-default `CACHE` variables *before* the `find_path()`/`find_library()` calls
that were supposed to populate them. Confirmed via a minimal repro
(`set(FOO "" CACHE PATH "x")` followed by `find_path(FOO NAMES stdio.h PATHS /usr/include)`
leaves `FOO` empty on CMake 4.4.1) that CMake treats any pre-existing cache entry for a find-target
variable as already-resolved — even an empty one — and skips the real search. This made the
`WEBRTC_ROOT` auto-discovery path silently resolve to nothing on every configure, regardless of
platform. Fix: stopped pre-declaring those two cache entries; `find_path`/`find_library` create
them (with a real search) the first time either is genuinely undefined. Verified both directions
still work: auto-discovery with no `-D` flags at all, and explicit
`-DWEBRTC_INCLUDE_DIR=... -DWEBRTC_LIBRARY=...` overrides still short-circuit the whole block as
before.

**Verified the fix against the real project, not just an isolated harness**: staged the §1 trial's
`libwebrtc.so` + wrapper headers at the Linux default cache location, then ran a full
`cmake -B ... -DSUNSHINE_ENABLE_WEBRTC=ON` configure of this repo (unrelated blocker hit and
worked around: `glad.cmake`'s pip install fails under Arch's PEP 668 externally-managed-Python
restriction — pre-existing, orthogonal to WebRTC, has its own escape hatch already in the codebase,
`-DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=<venv with jinja2>`). Configure succeeded;
`CMakeCache.txt` shows `WEBRTC_LIBRARY` resolved to the real `.so` path. Then went one step
further and actually built `CMakeFiles/sunshine.dir/src/webrtc_stream.cpp.o` with
`SUNSHINE_ENABLE_WEBRTC=1` defined — **compiled with zero errors**. This is real evidence, not
just a read of the source: no `#ifdef` branch in that ~5,940-line file had ever been compiled by
anything other than MSVC before this.

**Previously-documented "RPATH gap" was wrong — retest 2026-07-29**: this doc used to claim
`libwebrtc.so` "has no runtime story on Linux yet" and needs an explicit `$ORIGIN` RPATH wired
into the `sunshine` CMake target. That was reasoning from a Windows DLL-loading mental model, not
a tested claim. Actually did the full link (`cmake -B build-webrtc-linktest ... -DSUNSHINE_ENABLE_WEBRTC=ON`
+ `ninja -C build-webrtc-linktest sunshine`, a separate build dir, not the real dev `build/`) and
checked the result:

```
$ readelf -d sunshine-1.18.3-beta.6 | grep -i runpath
 0x000000000000001d (RUNPATH)  Library runpath: [/home/klebby/.cache/vibeshine/deps/libwebrtc/out/lib:]
$ ldd sunshine-1.18.3-beta.6 | grep -i webrtc
        libwebrtc.so => /home/klebby/.cache/vibeshine/deps/libwebrtc/out/lib/libwebrtc.so (0x...)
```

No "not found" entries, and `./sunshine --version` actually ran and started logging config values
before being cut off. **CMake's default build-tree behavior already embeds an absolute RUNPATH to
`WEBRTC_ROOT/lib`** whenever a library is linked via a full path from `find_library()` (the
default unless `CMAKE_SKIP_BUILD_RPATH` is set, which this project doesn't set for Linux/UNIX —
only the `APPLE` branch of `cmake/prep/init.cmake` touches RPATH settings at all). Nothing needed
to change in `cmake/targets/linux.cmake` for the dev/build-tree case milestone 4 actually needs.

The one real caveat, not a milestone-4 blocker: this RUNPATH is an *absolute path on the build
machine* (`$HOME/.cache/vibeshine/...`), not a portable one. That's fine for local dev builds (the
scenario every milestone here runs under) but would need an actual `$ORIGIN`-relative RPATH (or a
proper install step) before a *packaged/distributed* Linux binary could carry `libwebrtc.so`
across machines. Deferred — packaging is out of scope until there's a working feature to package.

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

## 2b. Bucket A was wrong: the NV12 push path is dead code on every platform (2026-07-29)

§2's Bucket A analysis assumed `try_push_nv12_frame`/`try_push_d3d11_frame` (the raw-NV12
`lwrtc_video_source_push_nv12` path) are the live video-delivery mechanism and that Linux needs an
`#ifdef __linux__` branch alongside them. That assumption came from reading the source, not from
tracing the call graph — it's wrong. Verified by grep across all of `src/`:

- `lwrtc_video_source_create` (the thing that would produce the `lwrtc_video_source_t*` these
  functions push into) is **never called anywhere**. `session.video_source` is declared
  (`webrtc_stream.cpp:1722`) but never assigned.
- `try_push_nv12_frame` and `try_push_d3d11_frame` are **never called anywhere** — only defined
  (`webrtc_stream.cpp:3655`, `:4236`; the latter is already marked `[[maybe_unused]]` in the
  source itself, which in hindsight was the tell).
- `submit_video_frame()` (`webrtc_stream.cpp:5509`), the only thing that pushes into
  `session.raw_video_frames`, has **zero callers** anywhere in the codebase, and
  `raw_video_frames` is never drained/popped anywhere either (only pushed).

This whole raw-NV12/`video_source` path — on Windows and macOS too, not just Linux — is unfinished
scaffolding for a design (hand libwebrtc raw frames and let it run its own internal encoder) that
was superseded before it was wired up. It is **not part of the current architecture** and is left
in place (not deleted — it's shared Windows/macOS-adjacent code and out of this port's scope; see
below).

**What's actually live**: `webrtc_stream::submit_video_packet(video::packet_raw_t&)`
(`webrtc_stream.cpp:5406`), fed from `src/video.cpp`'s three encoder-result call sites
(`encode_avcodec:2675`, `encode_nvenc:2710`, `deliver_amf_frames:2748` — none of these three call
sites are platform-gated). This is the exact same already-encoded byte stream the classic RTSP
path consumes, pushed into a `session.video_frames` ring buffer of `EncodedVideoFrame` and later
handed to libwebrtc as pre-encoded H264/HEVC via `lwrtc_encoded_video_source_push_shared`
(`webrtc_stream.cpp:4758`). Nothing in `submit_video_packet` or the `EncodedVideoFrame` struct is
platform-specific — codec/keyframe/timestamp handling is all generic (`packet.is_idr()`,
`packet.frame_index()`, raw byte payload). Since Linux's VAAPI/software encode already flows
through `encode_avcodec` and Linux NVENC through `encode_nvenc` — the same functions RTSP already
depends on — **this feed already reaches WebRTC sessions on Linux with no new code**.

Traced the other half of the question too: whether Linux control flow actually reaches the point
that starts the capture/encode threads and flips the gate `submit_video_packet` checks
(`webrtc_capture.active`, `webrtc_stream.cpp:5411`). Read `start_webrtc_capture()`
(`webrtc_stream.cpp:2936-3165`) end to end — the `#ifdef _WIN32` blocks inside it without a
matching `#else` (lines 3034, 3074, 3147) are all *additive* Windows-only extras (output-override
lease bookkeeping, virtual-display prep, display-helper coordination — Bucket B, already tracked
separately), not gates that block reaching `webrtc_capture.active.store(true, ...)` or the
`video_thread`/`audio_thread` spawn a few lines later at `:3160-3165`. The one non-Windows
early-return in that function (`:3132-3134`, no virtual-display retry-and-reprobe on failure) only
fires if `video::probe_encoders()` itself fails — the same failure mode RTSP already has on a
headless/no-display Linux box, not a WebRTC-specific gap. `channel_data` gating also checked: the
WebRTC capture thread calls `video::capture(mail, video_config, nullptr)` (`:3163`, `channel_data
= nullptr`), so `submit_video_packet`'s `packet.channel_data != nullptr` early-return
(`:5411`) correctly *admits* WebRTC-originated packets and rejects RTSP-originated ones (which
carry a real per-client `channel_data`) — this is the intended routing mechanism, working exactly
as designed, not a bug or a gap.

**Net effect on the milestone plan**: milestone 3 as originally scoped ("new `#ifdef __linux__`
branch mirroring `try_push_nv12_frame`") doesn't exist as a task — there's no NV12 branch to
write. This is a scoping correction, not a completed implementation: nothing has actually been
observed carrying a frame to a browser yet. That proof is milestone 4's job (real capture threads
spun up, real encoder probed, real packets flowing under a live browser session) — see §1b for why
that no longer needs an RPATH fix first either.

**Explicitly not doing**: deleting the dead `try_push_nv12_frame`/`try_push_d3d11_frame`/
`video_source`/`raw_video_frames` scaffolding. It's shared Windows/macOS-path code, unrelated to
what this fork's Linux port needs to touch, and removing it would just create upstream merge
conflicts for no benefit to this effort.

## 3. Rough milestone plan

1. ~~**Prove the dependency builds.**~~ **Done 2026-07-29** — see §1. `libwebrtc.so` builds clean
   for Linux x64 debug with two small fixes (a `gn` arg removal, a 3-line dead-code trim in the
   wrapper's own source). No upstream webrtc patching needed.
2. ~~**Wire the build.**~~ **Done 2026-07-29** — see §1b. `scripts/build_linux_webrtc.sh` written
   (not yet run end-to-end as a whole script), `linux.cmake`/`webrtc.cmake` wired and verified
   against a real project configure + a real compile of `webrtc_stream.cpp` with
   `SUNSHINE_ENABLE_WEBRTC=1`. Found and fixed a pre-existing `webrtc.cmake` caching bug along the
   way that affected the Windows path too. Originally thought runtime linking (`$ORIGIN` RPATH)
   was still unresolved here — retested in step 4's work and that turned out to be wrong too, see
   §1b's 2026-07-29 retest.
3. ~~**Frame delivery.**~~ **Turned out to be a no-op — done 2026-07-29** — see §2b. The originally
   assumed `#ifdef __linux__` NV12 branch doesn't need to exist: the raw-NV12 push path
   (`try_push_nv12_frame`/`video_source`) is dead code on every platform, and the path that's
   actually live (`submit_video_packet`, fed from the existing encoded-packet pipeline shared with
   RTSP) is already fully platform-agnostic. Verified by call-graph trace, not just a source read.
4. **End-to-end smoke test — build/link/runtime-load prerequisites done 2026-07-29, browser
   session itself not yet run.** Linked a full `sunshine` binary against the trial `libwebrtc.so`
   in an isolated build dir with `SUNSHINE_ENABLE_WEBRTC=ON`: it built clean, and — reversing the
   §1b "RPATH gap" claim — `readelf -d`/`ldd` show CMake's default build-tree RPATH behavior
   already embeds a working `RUNPATH` to `libwebrtc.so` with no CMake changes needed, and
   `./sunshine --version` actually ran and started logging config. No CMake/build work remains
   blocking this milestone. What's left is purely the runtime claim: real capture threads spin up,
   an encoder probes successfully, `webrtc_capture.active` flips, packets flow, and a real browser
   at `/webrtc` actually negotiates SDP/ICE and renders video. Bucket B items stay
   `#ifdef _WIN32`-only / no-op on Linux at this stage.
5. **Opportunistic follow-up**, not blocking: once virtual-display and RTSS Linux ports land
   independently, revisit Bucket B call sites to wire them in for WebRTC sessions too.

Steps 1–2 turned out to be the two biggest *build-side* unknowns and are resolved with working
recipes; step 3 turned out not to need any code at all; and step 4's build/link/RPATH
prerequisites turned out to already work with zero CMake changes, each of these only discoverable
by actually running the commands rather than reasoning from `#ifdef` blocks or a Windows mental
model. The only thing left before any of this is real is an actual browser session against a
running Linux build.
