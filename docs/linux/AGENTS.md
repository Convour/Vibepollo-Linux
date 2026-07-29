# Vibeshine Linux Agent Guide

This file is automatically loaded by AI coding assistants (OpenCode, Claude Code, etc.) when working in this repository. It contains authoritative knowledge about building, configuring, and troubleshooting Vibeshine on Linux — specifically Arch Linux / CachyOS with NVIDIA GPU and Wayland.

For full details on every topic, see **`LEARNINGS.md`**. This file is the quick-reference distillation.

---

## Repository Context

- **What**: Vibeshine — a Sunshine fork by Nonary with CUDA/NVENC, virtual display, WebRTC, and Playnite integration
- **Branch**: `vibe` (main development), `fix/linux-build-boost-1.89` (Linux build fixes)
- **Binary**: Installed at `~/.local/bin/sunshine` (symlink → versioned binary)
- **Build dir**: `~/vibeshine-build/build/`
- **Config**: `~/.config/sunshine/sunshine.conf`

---

## 1. Building on Linux (Arch / CachyOS)

> **Note:** Section 1 below reflects a from-scratch build verified on a *different* physical
> machine than the "Reference Machine" in §10 (RTX 3070, driver 610, kernel 7.1.5, no CUDA
> preinstalled, GCC 16 as system compiler). Package names and workarounds here supersede the old
> Boost-1.89 patch table further down — Boost is FetchContent'd by CMake (1.89.0 pinned) and no
> longer needs manual patching or a system package.

### Required packages
Note there is no root `package.json` — `npm`/`nodejs` are only needed for the CMake-driven
frontend build step, not for manual `npm` usage.
```bash
sudo pacman -S cmake ninja gcc cuda nvidia-utils libva libdrm \
    avahi miniupnpc openssl opus libpulse pipewire libevdev \
    libcap libnotify nodejs npm python-jinja python-setuptools
```
Do **not** install a system `boost` package — CMake's `Boost_Sunshine.cmake` FetchContents
1.89.0 automatically when no matching system package is found, and that's the working path.

### Submodules: init, then verify pinned commits
```bash
git submodule update --init --recursive
```
On at least one Arch/CachyOS setup, `git submodule update --init --recursive` (especially when
combined with a path-limited pathspec) left several submodules checked out at their upstream
branch tip instead of the commit actually pinned by the superproject — including a **nested**
submodule two levels down (`third-party/build-deps/third-party/FFmpeg/x265_git`). This silently
compiles the wrong third-party code. Always verify after init:
```bash
git submodule status   # any leading +/- means a mismatch, not a clean pinned checkout
git submodule update --force   # re-checks-out the recorded commit; objects are already fetched
```

### glad's Python deps (jinja2 + pkg_resources) vs. Arch's PEP 668 lock
CMake's `cmake/dependencies/glad.cmake` pip-installs `jinja2`/`setuptools<81` at configure time if
they're not importable, and glad's generator (`glad/plugin.py`) genuinely imports `pkg_resources`
at generation time (not just an overcautious check). Arch's system Python blocks `pip install`
outright (PEP 668 "externally-managed-environment"), and Arch's current `python-setuptools` (83.x)
has already dropped `pkg_resources`, so even installing the pacman package doesn't satisfy the
check. Fix: build a throwaway venv with the older setuptools pin, and use the CMake escape hatch
built for exactly this (Flatpak/Homebrew sandboxed builds):
```bash
python3 -m venv ~/.cache/vibepollo-glad-venv
~/.cache/vibepollo-glad-venv/bin/pip install --quiet --upgrade \
    -r third-party/glad/requirements.txt "setuptools<81"
```
Then pass `-DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=~/.cache/vibepollo-glad-venv/bin/python`
to every `cmake -B build` invocation below.

### Build commands — pass 1, no CUDA (validates the core build first)
```bash
cmake -B build -G Ninja -S . \
  -DCMAKE_INSTALL_PREFIX=~/.local \
  -DSUNSHINE_ENABLE_CUDA=OFF -DCUDA_FAIL_ON_MISSING=OFF \
  -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=~/.cache/vibepollo-glad-venv/bin/python
ninja -C build
```
If CMake configure fails on a submodule's `cmake_minimum_required(VERSION <3.5)` (CMake ≥4 made
this a hard error), add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

Six source-level bugs blocked this on a fresh clone as of `48b88477` — all fixed on `master`
(commit `55a98064`, "fix(linux): fix build/runtime bugs blocking a Linux build"): a stale glad
source-path list in `cmake/compile_definitions/linux.cmake`, two functions accidentally trapped
inside `#ifdef _WIN32` in `nvhttp.cpp`/`video.cpp` despite being called from cross-platform code,
a GCC-16 `-Wchanges-meaning` hard error from an nvenc struct field shadowing its own enum type
name, a missing `vulkan` enumerator on `platf::mem_type_e`, a typo'd member reference in
`host_stats.cpp`, and a `string_view`→`const char*` mismatch in `publish.cpp`. If you're building
from a commit before that fix, cherry-pick it first.

### Build commands — pass 2, enable CUDA
Arch's `cuda` package pulls in a dedicated `gcc15` as `nvcc`'s host compiler (via
`$NVCC_CCBIN` in `/etc/profile.d/cuda.sh`) because `nvcc` caps the GCC versions it officially
supports, and Arch's own system `gcc` (16.x as of this writing) is newer than any CUDA 13.3
supports. **Don't use that gcc15 detour** — it works for compiling `.cu` files, but the final
*link* step then picks up `-L .../gcc/.../15.x` ahead of the system compiler's own lib dir, and
with `-static-libstdc++` that resolves to gcc15's *older* static libstdc++, which is missing a
libstdc++ symbol version (`GLIBCXX_3.4.35`) that the rest of the binary (compiled by the system's
newer default `g++`) requires — an `undefined reference` at link time, not a compile error.

The clean fix is `nvcc --allow-unsupported-compiler`, which lets `nvcc` accept the system's
default `g++` directly, so there's no compiler split and no ABI mismatch:
```bash
export PATH=/opt/cuda/bin:$PATH
cmake -B build -G Ninja -S . \
  -DCMAKE_INSTALL_PREFIX=~/.local \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++ \
  -DCMAKE_CUDA_FLAGS="--allow-unsupported-compiler" \
  -DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=~/.cache/vibepollo-glad-venv/bin/python
ninja -C build
```
If you already configured once with the gcc15 host compiler, delete `build/CMakeCache.txt` before
switching — CMake caches CUDA ABI/implicit-link-dir detection and won't recompute it just because
`CMAKE_CUDA_HOST_COMPILER` changed.

### Install + capabilities
```bash
cmake --install build
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine   # required for `capture = kms`
```
`cmake --install` also tries to install a udev rule, a systemd user unit, and a modules-load.d
entry to `/usr/lib/...` — these need root and will fail silently-ish (a `CMake Error` at the very
end, after everything else installed fine) if run as a normal user. Install them manually:
```bash
sudo install -Dm644 src_assets/linux/misc/60-sunshine.rules /usr/lib/udev/rules.d/60-sunshine.rules
sudo install -Dm644 build/app-dev.lizardbyte.app.Sunshine.service \
    /usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service
sudo install -Dm644 src_assets/linux/misc/60-sunshine.conf /usr/lib/modules-load.d/60-sunshine.conf
sudo udevadm control --reload && sudo udevadm trigger
sudo modprobe uhid
```

### systemd user service: ExecStart needs an absolute path
The installed unit ships `ExecStart=sunshine` (bare command name). systemd's user-manager `PATH`
does not include `~/.local/bin`, so this fails with `status=203/EXEC` even though the binary
installed and runs fine manually. Fix with an override:
```bash
mkdir -p ~/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d
cat > ~/.config/systemd/user/app-dev.lizardbyte.app.Sunshine.service.d/override.conf << 'EOF'
[Service]
ExecStart=
ExecStart=/home/$USER/.local/bin/sunshine
EOF
systemctl --user daemon-reload
systemctl --user enable --now app-dev.lizardbyte.app.Sunshine.service
```

### Boost 1.89+ (historical — already merged into `master`, no action needed)
The patches below were required on an older commit and are now part of `master` (verified via
`process_start_dir`, the `stdio.hpp`/`start_dir.hpp` includes, and the `_WIN32`/
`SUNSHINE_ENABLE_WEBRTC` guards already present in the files listed). Kept here only as a record
in case a very old branch/tag still needs them.

| File | Fix |
|------|-----|
| `src/boost_process_shim.h` | Add `#include <boost/process/v2/stdio.hpp>` and `<boost/process/v2/start_dir.hpp>` |
| `src/platform/linux/misc.cpp` | `v2::start_dir` → `v2::process_start_dir` |
| `src/process.cpp` | Wrap `display_helper_integration` in `#ifdef _WIN32` |
| `src/nvhttp.cpp` | Wrap `VirtualDisplayDriverReady` in `#ifdef _WIN32` |
| `src/config.cpp` | Wrap `apply_playnite()` in `#ifdef _WIN32` |
| `src/webrtc_stream.cpp` | Wrap WebRTC-only code in `#ifdef SUNSHINE_ENABLE_WEBRTC` |
| `third-party/Simple-Web-Server/CMakeLists.txt` | Remove `boost_system` (header-only in 1.89+) |

---

## 2. Virtual Display Setup

Vibeshine streams to a **virtual display** on HDMI-A-2 (a physically disconnected port) using a custom EDID loaded by the kernel at boot.

### How it works
1. A custom EDID binary is embedded in the initramfs
2. Kernel params `drm.edid_firmware=HDMI-A-2:edid/<file>` + `video=HDMI-A-2:e` force-enable HDMI-A-2 at boot
3. Sunshine is configured with `output_name = HDMI-A-2`
4. On client connect, `global_prep_cmd` switches to HDMI-A-2; on disconnect, restores HDMI-A-1

### EDID files
```
/usr/lib/firmware/edid/samsung-q800t-hdmi2.1   # patched: 2560x1600@120 as DTD2
/usr/lib/firmware/edid/y700-virtual.bin         # same patch applied
```
Both are patched from the original Samsung Q800T EDID to replace `2560x1440@120` → `2560x1600@120` (CVT-RB, 552.75 MHz).

### CRITICAL: Boot parameter persistence (Limine)
**`mkinitcpio -P` wipes custom kernel params from `limine.conf`.**
The fix is `/etc/kernel/cmdline` — this file is the persistent source for `limine-entry-tool`:
```
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<YOUR-UUID>
```
After any `mkinitcpio` run, verify: `sudo grep 'cmdline:' /boot/limine.conf | head -2`

### Display IDs (this machine)
- `HDMI-A-1` connector_id=133 → Physical Samsung LS27A600U, 2560x1440@75Hz
- `HDMI-A-2` connector_id=140 → Virtual display, 2560x1600@120Hz

### `/etc/mkinitcpio.conf`
```
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)
```

---

## 3. Display Switching on Stream Connect/Disconnect

### Architecture
```
Boot              → ExecStartPre: enable HDMI-A-2, set mode
Client connects   → global_prep_cmd "do": switch-to-virtual.sh
Client disconnects → global_prep_cmd "undo": switch-to-physical.sh
Service stops     → ExecStopPost: restore HDMI-A-1
```

### `sunshine.conf` entry
```ini
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

### Scripts location
```
~/.config/sunshine/scripts/switch-to-virtual.sh
~/.config/sunshine/scripts/switch-to-physical.sh
```

The scripts use `kscreen-doctor` and dynamically look up mode IDs by resolution string (not hardcoded index, since mode IDs can shift between boots).

### Systemd service override
```
~/.config/systemd/user/sunshine.service.d/override.conf
```

---

## 4. Audio Configuration

### PipeWire quantum — MUST match Sunshine's frame size
Sunshine reads audio in 5ms frames = **240 samples at 48kHz**.
Default PipeWire quantum (1024 samples) causes buffer mismatch → crackling.

```bash
# ~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf
context.properties = {
    default.clock.rate = 48000
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
```
Verify: `pw-metadata -n settings 2>/dev/null | grep quantum`

### Use `virtual_sink` not `audio_sink`
```ini
# sunshine.conf — Sunshine creates and manages the sink lifecycle
virtual_sink = sink-sunshine-stereo
```
`virtual_sink` → Sunshine creates the null-sink, sets it as default, captures from it, restores on disconnect.
`audio_sink` → Sunshine captures from an existing sink but doesn't manage it.

### Audio format
The virtual sink is created as `float32le 2ch 48000Hz` (hardcoded in `src/platform/linux/audio.cpp`).
PipeWire handles conversion to your physical device format automatically.

---

## 5. Sunshine Configuration Reference

```ini
# ~/.config/sunshine/sunshine.conf (working config)

# Network
origin_web_ui_allowed = lan
upnp = on

# Display
output_name = HDMI-A-2          # Virtual display connector name
adapter_name = /dev/dri/renderD128

# FPS
fps = [30, 60, 90, 120]

# Audio
virtual_sink = sink-sunshine-stereo

# Encoder (NVENC + KMS for lowest latency)
encoder = nvenc
capture = kms
nvenc_preset = 1
nvenc_twopass = disabled
nvenc_latency_over_power = enabled
hevc_mode = 2

# Streaming
minimum_fps_target = 30
fec_percentage = 40
max_bitrate = 80000

# Display switching
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

---

## 6. Firewall (UFW)

```bash
sudo ufw allow 47984/tcp comment 'Sunshine RTSP'
sudo ufw allow 47989/tcp comment 'Sunshine GameStream'
sudo ufw allow 47990/tcp comment 'Sunshine Web UI'
sudo ufw allow 48010/tcp comment 'Sunshine Video'
sudo ufw allow 47998/udp comment 'Sunshine Video 1'   # CRITICAL
sudo ufw allow 47999/udp comment 'Sunshine Control'   # CRITICAL
sudo ufw allow 48000/udp comment 'Sunshine Video 2'   # CRITICAL
sudo ufw allow 48002/udp comment 'Sunshine Video 3'
sudo ufw allow 5353/udp  comment 'mDNS'
sudo ufw allow from 192.168.0.0/16 comment 'Local Network'
sudo ufw reload
```

UDP 47998/48000 are the most critical — without them, "no video received" error appears on client.

---

## 7. Common Diagnostics

```bash
# Service status and recent logs
systemctl --user status sunshine
journalctl --user -u sunshine -f

# Check which display Sunshine detected
journalctl --user -u sunshine | grep -E "connector|Monitor|HDMI"

# Check virtual display is present
kscreen-doctor -o | grep -E "Output:|enabled|2560"

# Check kernel loaded EDID
cat /proc/cmdline | grep edid
dmesg | grep -i "edid\|HDMI-A-2"

# PipeWire quantum
pw-metadata -n settings 2>/dev/null | grep quantum

# Audio sinks
pactl list sinks short

# Capabilities
getcap ~/.local/bin/sunshine

# mDNS discovery
avahi-browse -r _nvstream._tcp -t
```

---

## 8. Known Issues & Workarounds

| Issue | Cause | Fix |
|-------|-------|-----|
| Virtual display gone after kernel update | `mkinitcpio` wipes limine.conf params | Create `/etc/kernel/cmdline` with full cmdline |
| Sunshine uses HDMI-A-1 instead of HDMI-A-2 | HDMI-A-2 not initialized at boot | Check `/proc/cmdline` for `drm.edid_firmware` |
| Audio crackling on tablet | PipeWire quantum mismatch with Sunshine's 5ms frames | Set quantum=240 in pipewire conf |
| "no video received" on client | UDP ports blocked | Open 47998, 47999, 48000 UDP in UFW |
| KMS capture "Failed to gain CAP_SYS_ADMIN" | Binary missing capability | `sudo setcap cap_sys_admin+p ~/.local/bin/sunshine` |
| Resolution still 2560x1440 after EDID patch | EDID not in initramfs or boot params missing | `sudo mkinitcpio -P`, verify `/proc/cmdline` |

---

## 9. Files That Matter

| File | Purpose |
|------|---------|
| `~/.config/sunshine/sunshine.conf` | Main Sunshine config |
| `~/.config/systemd/user/sunshine.service.d/override.conf` | Systemd service customization |
| `~/.config/sunshine/scripts/switch-to-virtual.sh` | Enable virtual display on connect |
| `~/.config/sunshine/scripts/switch-to-physical.sh` | Restore physical display on disconnect |
| `~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf` | PipeWire quantum tuning |
| `/etc/kernel/cmdline` | Persistent kernel boot params (Limine) |
| `/usr/lib/firmware/edid/samsung-q800t-hdmi2.1` | Patched EDID (2560x1600@120 as DTD2) |
| `/etc/mkinitcpio.conf` | Must include EDID in `FILES=` |
| `~/vibeshine-build/LEARNINGS.md` | Full detailed learnings log |

---

## 10. System Info (Reference Machine — virtual display / EDID setup, §2-3)

This machine's virtual-display setup (custom EDID on HDMI-A-2, Lenovo Y700 tablet target) is
**not** the same physical machine as the build verified in §1 below it — connector names, EDID
files, and `kscreen-doctor` scripts here won't apply as-is to a different box. Re-derive connector
names from `/sys/class/drm/*/status` before reusing any of §2/§3 elsewhere.

| Component | Value |
|-----------|-------|
| OS | CachyOS (Arch Linux) |
| Kernel | linux-cachyos 6.19.5 |
| GPU | NVIDIA RTX 3080 Ti |
| Driver | 590.48.01 |
| CUDA | 13.1 (at /opt/cuda) |
| Display server | Wayland (KDE Plasma) |
| Bootloader | Limine |
| Init system | systemd |
| Audio | PipeWire 1.4.10 (PulseAudio compat) |
| Physical monitor | HDMI-A-1, Samsung LS27A600U, 2560x1440@75Hz |
| Virtual display | HDMI-A-2, 2560x1600@120Hz (EDID firmware) |
| Streaming target | Lenovo Y700 tablet, 2560x1600, 120Hz |

## 11. System Info (Build/run verified, §1 above)

Physical-display-only setup (no virtual display) — the build and NVENC/KMS fixes in §1 were
verified end-to-end on this machine.

| Component | Value |
|-----------|-------|
| OS | CachyOS (Arch Linux) |
| Kernel | 7.1.5-1-cachyos |
| CPU | AMD Ryzen 7 5800X |
| GPU | NVIDIA GeForce RTX 3070 (Ampere — no AV1 NVENC) |
| Driver | 610.43.03 |
| CUDA | 13.3.1 (`pacman -S cuda`, at /opt/cuda) |
| System compiler | GCC 16.1.1 (nvcc host compiler via `--allow-unsupported-compiler`, see §1) |
| Display server | Wayland (KDE Plasma 6.7.3, `kwin_wayland`) |
| Monitor | ASUS VG32VQ1B, 2560x1440@164.55Hz, DisplayPort (`DP-1`) |
| Capture | `kms` (KMS/DRM), `cap_sys_admin` via setcap |
| Encoder | `nvenc` — H.264/HEVC confirmed working, AV1 unsupported by this GPU |
