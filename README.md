# Vibepollo-Linux

## What is this fork?

Vibepollo-Linux is a Linux-focused fork of [Nonary's Vibepollo](https://github.com/Nonary/Vibepollo).
Upstream Vibepollo's active development is almost entirely Windows-oriented (Playnite integration,
native virtual display, RTSS/NVCP frame limiting) — the Linux build path exists in the CMake/CI
config but had never actually been build-tested end-to-end, and did not compile out of the box.

The goal here is the inverse focus: get and keep Vibepollo actually working on Linux, with the same
kind of streaming quality and feature set Windows users get from upstream, starting from a real
Arch/CachyOS + NVIDIA + Wayland desktop. See **`docs/linux/AGENTS.md`** and **`docs/linux/LEARNINGS.md`**
for the concrete build fixes and machine setup steps discovered so far, kept up to date as this
fork progresses. This is a personal fork, not a request for upstream to change focus.

In the spirit of Vibepollo being written with the help of AI, this Linux port has been entirely
implemented using **Claude Sonnet 5**, with **Claude Opus 5** used for advising on trickier tasks.
I have not written or read most of the changes myself.

### Closing the Windows-only feature gap on Linux

Several of Vibepollo's headline features (below) are currently Windows-only, built against
Win32/driver-level APIs with no Linux equivalent in the codebase. Rather than treating that as a
permanent limitation, this fork is mapping each one to a Linux-native mechanism that achieves the
same end-user outcome, even where the underlying implementation has to be entirely different.
Status below reflects live verification against real hardware (Arch/CachyOS + NVIDIA + KDE
Plasma/Wayland, `docs/linux/LEARNINGS.md` §24-25), not just code that compiles:

* **RTSS & NVIDIA Control Panel frame limiting → MangoHud + NVIDIA env vars.** ✅ **Confirmed
  working end-to-end.** Windows drives RTSS via DLL-hook injection and NVCP via NVAPI
  driver-settings ordinals. Linux capture already paces to the stream's target FPS, so the gap was
  capping the game's own render loop and toggling vsync/prerender-limit — done via MangoHud's
  config-driven FPS limiter plus `__GL_SYNC_TO_VBLANK`/`__GL_MaxFramesAllowed` set on the launched
  process's environment. Live-verified by dumping a launched process's real environment
  (`MANGOHUD_CONFIG=fps_limit=60000`, `__GL_SYNC_TO_VBLANK=0`) and the Web UI's frame-limiting tab
  is gated to hide the RTSS-only fields that have no Linux backend. Caveats that still apply:
  MangoHud's cap only takes effect if the launched app's own command already runs under MangoHud
  (no forced `LD_PRELOAD` injection yet); NVIDIA parity is inherently weaker on Linux (no
  NVAPI-equivalent system-wide toggle); there's no `/api/rtss/status`-equivalent endpoint.
* **Playnite Integration → Steam library sync (Lutris planned next).** ✅ **Shipped and
  live-verified end-to-end**, including the Web UI. Playnite itself has no Linux port, so this
  isn't a port — it's a new integration targeting Linux-native launchers. Steam library discovery
  (`GET /api/steam/status`) parses local `.vdf`/`.acf` files with no companion process and was
  confirmed against a real Steam install (141 games discovered correctly). Launch/quit tracking
  originally reused `steam://rungameid/<appid>`, but that hands the real game off to Steam's
  already-running client as a process Sunshine never owns — quitting in-game didn't end the
  stream, and ending the stream didn't kill the game. Fixed via a companion launcher binary
  (`tools/steam_launcher/`, mirroring the Playnite integration's own proxy-process pattern) that
  Sunshine tracks directly and that matches the real game process(es) via `SteamAppId` in
  `/proc/<pid>/environ`; both directions are now confirmed live against a real Moonlight client
  and a Proton title (`docs/linux/LEARNINGS.md` §25). A Web UI tab (enable/auto-sync toggles,
  manual "Sync now", status card) ships alongside the backend. Lutris is next; the portable JSON
  protocol/reconciliation logic from the Playnite implementation is being generalized so Lutris
  (and eventually a real Playnite path, if one ever exists on Linux) can reuse the same core.
* **Native Virtualized Display → KDE `kscreen-doctor` + EDID-injected output.** 🟡 **Partially
  verified — apply path confirmed, revert path still untested live.** Windows uses a bundled
  kernel-mode driver to create a synthetic monitor. On Linux (KDE Plasma/Wayland target) the
  equivalent is an EDID-injected dummy output (one-time kernel/initramfs setup, documented in
  `docs/linux/LEARNINGS.md` §11/§16/§17) that Sunshine enables/disables/mode-switches at session
  start/stop via `kscreen-doctor`, replacing the hand-rolled `global_prep_cmd` scripts validated
  there with native code. Non-KDE compositors degrade gracefully rather than failing the stream.
  The apply-side hook (`launch_app_commands()`) fires correctly and no-ops safely when unconfigured;
  no session in this repo's history has yet exercised the revert path end-to-end (`terminate()` →
  `revert_session_display`) against a real EDID-injected output with a long-running process, so
  that half is implemented but not yet proven on hardware.
* **WebRTC browser streaming (`/webrtc`) — currently non-functional on Linux, scoping done, build
  not yet attempted.** 🔴 Not part of the original Windows-only feature map (the code is largely
  platform-agnostic), but discovered during this fork's audit to be effectively Windows-only in
  practice: `SUNSHINE_ENABLE_WEBRTC` defaults `OFF` and is documented "(Windows only)"
  (`cmake/prep/options.cmake:20`), so a Linux build currently returns `WebRTC: support is disabled
  at build time` for any browser client. A scoping pass (`docs/linux/webrtc-linux-port-plan.md`)
  found the gap smaller than expected — `src/webrtc_stream.cpp`'s signaling/session/SDP/data-channel
  logic already gates Windows-specifics behind `#ifdef _WIN32`, and the `libwebrtc` dependency's
  own `BUILD.gn` has real `is_linux` branches — but that Linux support is inherited from upstream
  and **not CI-validated in this fork's `libwebrtc` branch**; nobody has actually built it for
  Linux yet. First concrete milestone (not yet started): a manual trial build of `libwebrtc` for
  Linux to confirm it compiles before scoping the rest.

See `docs/linux/LEARNINGS.md` for the full verification notes behind each status above.

## What is Vibepollo?

Vibepollo is an AI‑enhanced version of Apollo, a popular remote streaming application. It intends to integrate all scripts from myself (Nonary) and more.



## Key Features

These are upstream Vibepollo's headline features as originally written (Windows-first). The
`Linux:` line under each one is this fork's status — see "Closing the Windows-only feature gap on
Linux" above for the four features actively being ported, and `docs/linux/LEARNINGS.md` for the
underlying verification.

* **Display Setting Automation**
  Vibepollo adds multiple safeguards to prevent dummy plugs or virtual displays from getting “stuck” when you return to your PC. It resolves common Windows 11 **24H2** display issues and restores your layout after hard crashes, shutdowns, or reboots. (The only scenario it can’t restore is during a user logout.) The workflow is simplified to a dropdown—just pick the display you want to stream.
  **Linux: 🟡 partial.** The `kscreen-doctor`-driven apply path is live; the crash/reboot-recovery
  and revert-on-session-end guarantees this bullet describes are Windows-specific behavior not yet
  reproduced on Linux (see the virtual-display entry above).

* **Windows Graphics Capture in Service Mode**
  Running Windows Graphics Capture (WGC) as a service improves performance and stability. It captures the full frame rate of frame‑generated titles, avoids crashes when VRAM is exceeded, and follows Microsoft’s recommended capture method going forward. Vibepollo auto‑switches capture methods on demand, so the login screen and UAC prompts are still captured even when using WGC.
  **Linux: N/A — different mechanism, not a gap.** WGC is a Windows API; Linux capture goes through
  KMS/DRM, VA-API, Vulkan, Wayland, or a portal backend instead (`SUNSHINE_ENABLE_DRM/VAAPI/
  VULKAN/WAYLAND/KWIN/PORTAL`, all on by default), confirmed working via NVENC probes on this
  fork's dev hardware. There's no login-screen/UAC-prompt equivalent to auto-switch for.

* **Native Virtualized Display**
  Vibeshine uses its bundled virtual display driver by default and keeps SudoVDA installed as a rollback option. It can capture output from any GPU, including those in hybrid laptops, ensuring the virtual screen connects to the correct GPU when needed. It also provides simple virtual display options, allowing users to choose between a physical or virtual display. On headless setups, it enables automatically to prevent 503 errors and false encoder detections, such as incorrect HEVC support reports.
  **Linux: 🟡 in progress** — see "Closing the Windows-only feature gap on Linux" above.

* **WebRTC Browser Streaming**
  Vibeshine can stream straight to your web browser from the `/webrtc` page, so you can play without installing a separate client. It is designed for fast response and smooth audio/video, while still letting you use the regular Moonlight-compatible streaming path if you prefer.
  **Linux: 🔴 not functional yet** — see "Closing the Windows-only feature gap on Linux" above.

* **Redesigned Frontend with Full Mobile Support**
  The new Web UI makes it easy to add games and change settings without restarting the program. It’s fully responsive, so you can manage your library and configuration from a phone or tablet.
  **Linux: ✅ works.** The frontend is platform-agnostic; served and used from this fork's Linux
  builds daily, including the Linux-specific tabs (Steam sync, frame limiting) added for this port.

* **Playnite Integration**
  Deep integration with Playnite (a “launcher of launchers”) automatically syncs your recently played games with configurable expiration rules, per‑category sync, and exclusions. You can also add games manually from a Web UI dropdown; Vibepollo handles artwork, launching, and clean termination—emulators included. The goal is a seamless, GeForce Experience–style library experience—only better.
  **Linux: ✅ Steam sync shipped as the Linux equivalent** (Playnite itself has no Linux port) —
  see "Closing the Windows-only feature gap on Linux" above. Lutris planned next.

* **RTSS & NVIDIA Control Panel Integration**
  Vibepollo can manage RTSS to apply the correct frame limit and disable V‑Sync before streaming, significantly improving frame pacing and smoothness. The applied frame cap matches the client device’s requested FPS.
  **Linux: ✅ MangoHud/env-var equivalent confirmed working end-to-end** — see "Closing the
  Windows-only feature gap on Linux" above.

* **Frame‑Generated Capture Fixes**
  DLSS/FSR game-provided frame generation requires Vibepollo's virtual screen for reliable capture. The virtual display guarantees composed flip, allowing generated frames to be captured through WGC, and Vibepollo targets 4x virtual refresh for pacing.
  **Linux: 🔴 not yet addressed.** This depends on both the virtual display's still-unverified
  revert path and a WGC-equivalent composed-flip guarantee under KDE/Wayland; no Linux-specific
  work has started here.

* **Lossless Scaling & NVIDIA Smooth Motion**
  Vibepollo can automatically apply optimal Lossless Scaling settings to generate frames for any application. On RTX 40‑series and newer GPUs, you can optionally enable **NVIDIA Smooth Motion** for better performance and image quality (while Lossless Scaling remains more customizable).
  **Linux: 🔴 not planned.** Lossless Scaling is a Windows-only third-party app with no Linux
  build; no Linux-native equivalent is currently mapped or scheduled.

* **API Token Management**
  Access tokens can be tightly scoped—down to specific methods—so external scripts don’t need full administrative rights. This improves security while keeping automation flexible.
  **Linux: ✅ works.** Platform-agnostic; unaffected by this port.

* **Session‑Based Authentication**
  The sign‑in flow supports password managers and includes a “remember me” option to minimize prompts. The experience is security‑hardened without sacrificing convenience.
  **Linux: ✅ works.** Platform-agnostic; unaffected by this port.

* **Update Notifications**
  Built‑in notifications let you know when new features or bug fixes are available, making it easy to stay current.
  **Linux: ✅ works.** Platform-agnostic; unaffected by this port.

Due to the sheer pace and volume of changes I was producing, it became impractical to manage them within the original Sunshine repository. The review process simply couldn’t keep up with the rate of development, and large feature sets were piling up without a clear path to integration. To ensure the work remained organized, maintainable, and actively progressing, I established Vibepollo as a standalone fork.

At this point, Vibepollo differs substantially from upstream Sunshine. At that scale, asking upstream maintainers to accept large backports in one sweep is generally not sustainable, which is why Vibepollo continues as a standalone fork.

---

## Does Vibepollo aim to replace Sunshine or Apollo?

No. Vibepollo is intended as a **complementary fork**, not a replacement.


## Will Vibepollo’s features merge back into Sunshine or Apollo?

**Short answer: Unlikely to be backported upstream as large, sweeping merges.**

Vibepollo is largely AI‑generated. While it works well, it carries a kind of surface‑level technical debt that many upstream projects want resolved before taking big changes (styling consistency, thin/missing docs, and some over‑engineering). I see that debt as relatively unimportant today because modern AI tools can answer “why does this function exist?”, “what does this parameter do?”, or “how do these classes interact?” and will soon auto‑fix these issues—re‑style trees, write docstrings, and prune unused layers—without human effort.

So this “mess” is mostly cosmetic. It doesn’t break the code, create security risks, or block future maintenance. The only debt that truly matters is architectural: API design, threading models, modularity, and performance. Those are hard to fix even with AI tools, which is why I focus on them up front and guide the AI accordingly.

Because I define the architecture, I know how everything works. Whether the code looks polished or not doesn’t matter to me.

Bringing Vibepollo fully in line with upstream style and documentation would take a lot of engineering time for limited practical gain. For now, full backports into Sunshine or Apollo are unlikely. Over time, targeted refactors or added documentation may make **selective upstreaming** possible.

---

## Origin of the Name "Vibepollo"

The name arose as a playful suggestion from another developer who joked about the potential unmanageability of extensive AI‑generated code. Given that approximately **99% of Vibepollo’s code is AI‑generated**, the name seemed fitting.

---

## Why Use AI‑generated Code? Concerns About Technical Debt?

AI significantly accelerates development by offloading much of the routine implementation work. Instead of spending hours writing boilerplate, wiring dependencies, or handling repetitive edge cases, I can focus on high‑level architecture, long‑term design decisions, and system direction. This shift doesn’t just speed things up—it fundamentally changes the role of the engineer, pushing us toward oversight, orchestration, and design rather than rote code production.

What stands out most is that AI code works on the first try around 90% of the time. That reliability, combined with instant generation, makes it dramatically more efficient to accept its form of debt than to painstakingly write everything from scratch. In other words, I’m trading minor, manageable debt for massive development velocity—and that trade is almost always worth it.

I’m not overly concerned about technical debt in this workflow, because the debt that truly matters stems from bad architecture and poor design choices, not from the code itself. As long as I guide the AI with clear structure and intent, the generated code ends up being maintainable. Problems like inconsistent naming, redundant code, or unused helpers are minor forms of debt—easily identified, cleaned up, or ignored. By contrast, deep architectural flaws, poor layering, or mismatched abstractions create lasting problems.

In fact, compared to many traditional enterprise codebases I’ve maintained, AI‑assisted code often comes out cleaner and easier to manage. Legacy systems are usually burdened with years of ad‑hoc patches, inconsistent styles, and various bad practices due to knowledge level of contributor. AI‑generated code doesn’t necessarily carry fewer design flaws than human code, but it does avoid accumulating those scars—especially when paired with an intentional architectural vision, and it is less likely to do seriously bad practices that you typically find in enterprise codebases.

Broadly speaking, AI‑assisted development represents the future of software engineering. Just as compilers and IDEs once transformed programming, AI is now transforming how we design, implement, and maintain systems. Instead of fearing it, I view it as a force multiplier that complements professional judgment. Vibepollo is an example of what happens when you embrace that shift: rapid iteration, a massive expansion of features, and code that remains maintainable because the architecture is intentionally guided.

---

## The Original “AI-Only” Goal (And Why It Changed)

One of the original goals of Vibepollo was to prove a specific point: that an experienced developer could maintain a complex project using almost entirely AI‑generated code, as long as they provided the architecture and kept the system coherent.

That idea hasn’t aged particularly well, not because it was wrong, but because the models scaled far faster than most projections. The result is that the “skill gap” in prompting and guiding the AI matters less than it did even a few months prior. You still need engineering judgment and architecture, but it’s now dramatically easier to get high‑quality, end‑to‑end results without the same level of careful orchestration. So the original “prove it’s possible” goal is basically moot: it’s not a niche workflow anymore, it’s simply where the tools have gone.

---

## AI Models Used by Vibepollo

Vibepollo has always been built with **Codex** as the primary workflow, and in practice that has meant mostly the **GPT‑5 family** (today: **GPT‑5.3‑Codex**). I use it with the same principles as before: start from architecture, sanity‑check assumptions, and do the hard reasoning up front so the implementation lands cleanly.

With **GPT‑5.3‑Codex**, there’s no real need to juggle a “fast but less capable” model anymore. In the past I’d reach for speed‑first models (like Sonnet, or smaller GPT “mini” variants) for quick turnaround, but **GPT‑5.3‑Codex** covers both: it’s about as fast as those options while also being strong enough to handle the hard engineering work in one pass.

Claude was used more heavily earlier on. Older Claude models had a tendency to go off on their own path, even when the architectural plan was clear. That behavior has mostly been fixed in newer Claude releases, but GPT still ended up being the more useful engineering tool for me because it will challenge you and not simply agree with whatever you ask for.

In general, GPT has felt more intelligent for the way I build and maintain this codebase. I may occasionally ask **Claude Opus 4.5** for a second opinion if GPT can’t resolve something cleanly end‑to‑end, but this is increasingly rare.

---

## Sponsors

<p align="center">
  <a href="https://signpath.io?utm_source=foundation&amp;utm_medium=github&amp;utm_campaign=vibepollo">
    <img src="docs/images/signpath.svg" alt="SignPath" width="420">
  </a>
</p>

Thank you to [SignPath.io](https://signpath.io?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo)
and the [SignPath Foundation](https://signpath.org?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo)
for sponsoring Vibepollo's Windows code signing.

### Code signing policy

Official Vibepollo Windows releases use free code signing provided by
[SignPath.io](https://signpath.io?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo), and a
certificate by the [SignPath Foundation](https://signpath.org?utm_source=foundation&utm_medium=github&utm_campaign=vibepollo).

* **Committer and reviewer:** [Nonary](https://github.com/Nonary)
* **Approver:** [Nonary](https://github.com/Nonary)
* **Privacy:** Vibepollo transfers information to networked systems only for functionality requested by the user or
  operator; it does not transmit user or runtime data to SignPath. Separately, SignPath's GitHub integration receives
  the build artifacts, signing-request details, and GitHub-provided build-origin metadata needed to sign official
  releases.
