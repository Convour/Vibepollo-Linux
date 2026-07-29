# Vibeshine Linux Build & Setup - Learnings

## Overview

This document captures all learnings from building and configuring Vibeshine (a Sunshine fork) on Arch Linux with Boost 1.89+, NVIDIA GPU, and Wayland.

---

## 1. Build Fixes for Linux (Boost 1.89+)

### Problem
Vibeshine/Sunshine fails to build on Linux with Boost 1.89+ due to API changes and Windows-only code not properly guarded.

### Files Modified

| File | Issue | Fix |
|------|-------|-----|
| `src/boost_process_shim.h` | Missing Boost.Process v2 headers | Added `#include <boost/process/v2/stdio.hpp>` and `<boost/process/v2/start_dir.hpp>` |
| `src/platform/linux/misc.cpp` | API rename in Boost 1.89 | Changed `v2::start_dir` → `v2::process_start_dir` |
| `src/process.cpp` | Windows-only code | Wrapped `display_helper_integration` in `#ifdef _WIN32` |
| `src/nvhttp.cpp` | Windows-only VDISPLAY code | Wrapped `VirtualDisplayDriverReady` in `#ifdef _WIN32` |
| `src/config.cpp` | Windows-only Playnite | Wrapped `apply_playnite()` in `#ifdef _WIN32` |
| `src/webrtc_stream.cpp` | WebRTC-only code | Wrapped in `#ifdef SUNSHINE_ENABLE_WEBRTC` |
| `third-party/Simple-Web-Server/CMakeLists.txt` | `boost_system` link error | Removed `boost_system` (header-only in Boost 1.89+) |

### Build Commands
```bash
# Install dependencies (Arch Linux)
sudo pacman -S cmake ninja gcc cuda nvidia-utils libva libvdpau \
    avahi miniupnpc openssl opus libpulse libpipewire libdrm \
    libevdev libcap libnotify libayatana-appindicator

# Clone and build
git clone https://github.com/Nonary/vibeshine.git
cd vibeshine
git checkout vibe
git checkout -b fix/linux-build-boost-1.89
# Apply patches...
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=~/.local -DSUNSHINE_ENABLE_CUDA=ON
cmake --build . --parallel
cmake --install .
```

---

## 2. Network Configuration

### Problem
Vibeshine not discoverable on other devices - no video streaming working.

### Root Causes
1. **UFW Firewall** was blocking required ports
2. **mDNS** blocked between wired/wireless clients on some routers
3. **UDP ports** not open for video streaming

### Required Ports

| Port | Protocol | Purpose |
|------|----------|---------|
| 47989 | TCP | GameStream pairing |
| 47990 | TCP | Web UI (HTTPS) |
| 47984 | TCP | RTSP |
| 48010 | TCP | Video/control |
| **47998** | **UDP** | **Video streaming (CRITICAL)** |
| **47999** | **UDP** | **Control channel (CRITICAL)** |
| **48000** | **UDP** | **Video streaming (CRITICAL)** |
| 5353 | UDP | mDNS/Bonjour discovery |

### UFW Firewall Rules
```bash
# TCP ports
sudo ufw allow 47989/tcp comment 'Sunshine GameStream'
sudo ufw allow 47990/tcp comment 'Sunshine Web UI'
sudo ufw allow 47984/tcp comment 'Sunshine RTSP'
sudo ufw allow 48010/tcp comment 'Sunshine Video'

# UDP ports (CRITICAL for video)
sudo ufw allow 47998/udp comment 'Sunshine Video 1'
sudo ufw allow 47999/udp comment 'Sunshine Control'
sudo ufw allow 48000/udp comment 'Sunshine Video 2'
sudo ufw allow 48002/udp comment 'Sunshine Video 3'
sudo ufw allow 5353/udp comment 'mDNS'

# Local network
sudo ufw allow from 192.168.0.0/16 comment 'Local Network'

sudo ufw reload
```

### Sunshine Configuration (`~/.config/sunshine/sunshine.conf`)
```ini
origin_web_ui_allowed = lan
upnp = on
```

---

## 3. KMS Capture Setup (Wayland)

### Problem
`Error: Failed to gain CAP_SYS_ADMIN` - KMS capture not working on Wayland.

### Solution
Set capabilities on the Sunshine binary:
```bash
sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))
```

Or for custom install:
```bash
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine
```

### Alternative: Use sunshine-kms service
The logs recommend using `sunshine-kms` service instead of regular `sunshine` service for KMS capture. Check Sunshine documentation for setup.

---

## 4. Video Scaling Errors

### Problem
```
Error: Couldn't scale frame: Invalid argument
Error: Could not convert image
```

### Causes
1. **Resolution mismatch**: Monitor is 2560x1440 but capture is 1920x1080
2. **DMA-BUF format issues**: XDG portal returning incompatible buffer formats
3. **Color space conversion errors**: NVENC expecting different format

### Potential Fixes
1. **Match resolutions**: Set monitor to 1920x1080 or configure Sunshine to capture at native resolution
2. **Try different capture method**: Switch between XDG Portal, KMS, or NvFBC
3. **Check NVIDIA driver**: Ensure latest drivers with proper DMA-BUF support
4. **Set encoder color space**: Configure encoder to match captured format

### In Web UI, check:
- Video > Encoder: `nvenc`
- Video > Capture: Try `KMS` or `XDG Portal`
- Video > Resolution: Match or lower than monitor

---

## 5. System Requirements

### User Groups
```bash
# Add user to required groups
sudo usermod -aG input $USER
sudo usermod -aG video $USER
sudo usermod -aG render $USER
```

### Service Setup
```bash
# Create systemd override for custom binary
mkdir -p ~/.config/systemd/user/sunshine.service.d
cat > ~/.config/systemd/user/sunshine.service.d/override.conf << 'EOF'
[Service]
ExecStart=
ExecStart=/home/$USER/.local/bin/sunshine
EOF

# Enable and start
systemctl --user daemon-reload
systemctl --user enable sunshine
systemctl --user start sunshine
```

---

## 6. mDNS/Avahi Debugging

### Check Broadcasting
```bash
# Check if Sunshine is broadcasting
avahi-browse -r _nvstream._tcp -t

# Should show:
# hostname = [hostname.local]
# address = [192.168.x.x]
# port = [47989]
```

### Manual Connection
If mDNS doesn't work (router blocking), add host manually:
1. Open Moonlight client
2. Add host: `192.168.x.x`
3. Pair with PIN from Web UI

---

## 7. Debugging Commands

```bash
# Check service status
systemctl --user status sunshine

# View logs
journalctl --user -u sunshine -f

# Check open ports
ss -tulnp | grep sunshine

# Check mDNS
avahi-browse -a -t | grep nvstream

# Test Web UI
curl -sk https://localhost:47990 | head -5

# Check capabilities
getcap $(readlink -f ~/.local/bin/sunshine)

# Check groups
groups $USER | grep -E "input|video|render"
```

---

## 8. Files Created

| File | Purpose |
|------|---------|
| `~/setup-sunshine-ufw.sh` | UFW firewall setup script |
| `~/.config/sunshine/sunshine.conf` | Sunshine configuration |
| `~/.config/systemd/user/sunshine.service.d/override.conf` | Systemd service override |
| `~/0001-fix-Linux-build-compatibility-for-Boost-1.89.patch` | Build fix patch |

---

## 9. Key Learnings Summary

1. **Boost 1.89+ requires code changes** - Many API changes from older Boost versions
2. **Windows-only code needs `#ifdef _WIN32`** - Playnite, VDISPLAY, display_helper are Windows-only
3. **UDP ports are critical** - TCP alone won't work; video streams over UDP
4. **UFW blocks by default** - Must explicitly allow all Sunshine ports
5. **KMS capture needs CAP_SYS_ADMIN** - Set capabilities or use sunshine-kms service
6. **mDNS may be blocked by router** - Manual IP connection works around this
7. **Resolution mismatch causes scaling errors** - Match capture to display resolution

---

## 10. References

- [Sunshine Documentation](https://docs.lizardbyte.dev/projects/sunshine/latest/)
- [Vibeshine GitHub](https://github.com/Nonary/vibeshine)
- [Moonlight Game Streaming Ports](https://portforward.com/moonlight-game-streaming/)
- [Sunshine Getting Started](https://docs.lizardbyte.dev/projects/sunshine/latest/md_docs_2getting__started.html)

---

## 11. Virtual Display Setup (EDID Method)

### Problem
Need to stream without physical monitor, or stream while monitor is off.

### Solution: EDID Virtual Display
Load a custom EDID file via kernel parameters to create a virtual display on an unused video output.

### Step 1: Find Available Output
```bash
for p in /sys/class/drm/*/status; do con=${p%/status}; echo -n "${con#*/card?-}: "; cat $p; done
# Look for "disconnected" outputs
```

### Step 2: Download EDID File
```bash
sudo mkdir -p /usr/lib/firmware/edid

# Option A: Use existing monitor's EDID
sudo cat /sys/class/drm/card1-HDMI-A-1/edid > /tmp/monitor.bin
sudo cp /tmp/monitor.bin /usr/lib/firmware/edid/virtual-display.bin

# Option B: Download 4K 120Hz HDR EDID (samsung-q800t-hdmi2.1)
curl -sL "https://git.linuxtv.org/v4l-utils.git/plain/utils/edid-decode/data/samsung-q800t-hdmi2.1" -o /tmp/samsung-q800t-hdmi2.1
sudo cp /tmp/samsung-q800t-hdmi2.1 /usr/lib/firmware/edid/
```

### Step 3: Configure Kernel Parameters

#### For GRUB:
```bash
sudo nano /etc/default/grub
# Add to GRUB_CMDLINE_LINUX:
drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e

sudo update-grub
```

#### For Limine (CachyOS):
```bash
sudo nano /boot/limine.conf
# Add to cmdline:
drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e
```

### Step 4: Add EDID to Initramfs
```bash
sudo nano /etc/mkinitcpio.conf
# Add to FILES:
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)

sudo mkinitcpio -P
```

### Step 5: Configure Sunshine
In Sunshine Web UI (https://localhost:47990):
1. Go to **Configuration > Audio/Video**
2. Set **Output Name** to the virtual display (e.g., `HDMI-A-2`)
3. Or edit `~/.config/sunshine/sunshine.conf`:
```ini
output_name = HDMI-A-2
```

### Step 6: Reboot
```bash
sudo reboot
```

### Verify Virtual Display
```bash
# Check if virtual display is detected
xrandr --query | grep connected
# or
cat /sys/class/drm/card*/status
```

### Alternative Methods

#### Headless Sway (Dynamic Resolution)
- Runs separate Wayland compositor for streaming
- Dynamic resolution matching to client
- See: https://github.com/daaaaan/sunshine-headless-sway

#### Dummy Display Script (evtest)
- Automated toggle with physical display
- See: https://github.com/TheRealHoobi/Sunshine-Dummy-Display-for-linux

---

## 12. Auto-Login Setup (SDDM)

### Disable Login Screen
```bash
sudo nano /etc/sddm.conf
```
Add:
```ini
[Autologin]
User=$USER
Session=plasma
```

---

## 13. Passwordless Sudo (Optional)

### Full Passwordless Sudo
```bash
echo "$USER ALL=(ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/$USER
```

### Specific Commands Only
```bash
echo "$USER ALL=(ALL) NOPASSWD: /usr/bin/ufw, /usr/bin/setcap, /usr/bin/mkinitcpio" | sudo tee /etc/sudoers.d/sunshine-commands
```

---

## 14. Complete Setup Summary

After all steps, you should have:

| Component | Status | Verification |
|-----------|--------|--------------|
| Vibeshine binary | Installed | `which sunshine` |
| UDP ports | Open | `sudo ufw status` |
| CAP_SYS_ADMIN | Set | `getcap $(which sunshine)` |
| User groups | Added | `groups $USER` |
| Virtual display | Configured | `xrandr --query` |
| Auto-login | Enabled | `/etc/sddm.conf` |
| Sunshine service | Running | `systemctl --user status sunshine` |

### Quick Test
1. Open Moonlight on client device
2. Connect to host (auto-discover or manual IP)
3. Enter PIN from https://localhost:47990
4. Stream!

---

## 15. Troubleshooting

### No video received
1. Check UDP ports: `sudo ufw status | grep udp`
2. Check Sunshine logs: `journalctl --user -u sunshine -f`
3. Verify virtual display: `xrandr --query`

### "Failed to gain CAP_SYS_ADMIN"
```bash
sudo setcap cap_sys_admin+p $(readlink -f $(which sunshine))
```

### Scaling errors
1. Match Sunshine resolution to virtual display
2. Try different capture method (KMS vs XDG Portal)
3. Check NVIDIA driver version

### Virtual display not detected
1. Verify EDID file in initramfs: `lsinitcpio /boot/initramfs-linux.img | grep edid`
2. Check kernel parameters: `cat /proc/cmdline`
3. Try different output port
4. **Critical**: If using Limine bootloader, kernel params may be wiped on kernel update — see §16

---

## 16. Boot Parameter Persistence (Limine Bootloader)

### Problem
On CachyOS/Arch with Limine bootloader, running `mkinitcpio -P` (e.g. after updating EDID in initramfs) triggers `limine-entry-tool`, which **overwrites** `limine.conf` and strips any custom kernel parameters like `drm.edid_firmware` and `video=HDMI-A-2:e`. The virtual display then silently disappears on next boot.

### Symptom
- `cat /proc/cmdline` is missing `drm.edid_firmware` after reboot
- HDMI-A-2 no longer appears in `kscreen-doctor -o`
- Sunshine logs show connector ID 133 (physical) instead of 140 (virtual)

### Fix: Persist params via `/etc/kernel/cmdline`
`limine-entry-tool` reads `/etc/kernel/cmdline` if it exists, using it as the base for all future `limine.conf` writes.
```bash
# Create this file with your full desired cmdline
sudo tee /etc/kernel/cmdline << 'EOF'
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<YOUR-UUID>
EOF
```
After this, `mkinitcpio -P` will no longer wipe your custom params.

### Also fix limine.conf immediately
If params were already wiped, restore them manually before the next reboot:
```bash
sudo grep -n 'cmdline:' /boot/limine.conf | head -5
# Edit lines for your main boot entries (not snapshot entries)
sudo nano /boot/limine.conf
# Add: drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e
```

---

## 17. Custom EDID Resolution (Patching Binary EDID)

### Problem
The stock Samsung Q800T EDID (used for virtual display) only advertises `2560x1440@120` as its highest non-4K mode. Streaming to a Lenovo Y700 tablet (native `2560x1600`) meant the virtual display never offered that resolution.

### Solution: Patch the EDID binary
The EDID DTD (Detailed Timing Descriptor) is an 18-byte structure. Replace the `2560x1440@120` DTD with a `2560x1600@120` CVT Reduced Blanking entry:

```python
#!/usr/bin/env python3
# Patch EDID: replace 2560x1440@120 with 2560x1600@120 (CVT-RB)
# CVT-RB modeline: 552.75 2560 2608 2640 2720 1600 1603 1609 1694 +hsync -vsync

old_dtd = bytes.fromhex('6fc200a0a0a0555030203500501d7400001a')  # 2560x1440@120
new_dtd = bytes.fromhex('ebd700a0a0405e6030203600501d7400001a')  # 2560x1600@120 RB

data = bytearray(open('/usr/lib/firmware/edid/samsung-q800t-hdmi2.1', 'rb').read())
i = 0
while i <= len(data) - 18:
    if data[i:i+18] == old_dtd:
        data[i:i+18] = new_dtd
    i += 1

# Recalculate checksums for each 128-byte block
for block in range(len(data) // 128):
    offset = block * 128
    s = sum(data[offset:offset+127]) % 256
    data[offset+127] = (256 - s) % 256

open('/usr/lib/firmware/edid/samsung-q800t-hdmi2.1', 'wb').write(data)
```

After patching, regenerate initramfs: `sudo mkinitcpio -P`

### Key numbers
| Field | Value |
|-------|-------|
| Pixel clock | 552.75 MHz (fits in EDID 1.3's 655 MHz max) |
| CVT modeline | `552.75 2560 2608 2640 2720 1600 1603 1609 1694 +hsync -vsync` |
| Old DTD hex | `6fc200a0a0a0555030203500501d7400001a` |
| New DTD hex | `ebd700a0a0405e6030203600501d7400001a` |

### Verify
```bash
edid-decode /usr/lib/firmware/edid/samsung-q800t-hdmi2.1 2>/dev/null | grep 'DTD 2'
# Expected: DTD 2:  2560x1600  119.96 Hz  16:10   203.217 kHz    552.750000 MHz
```

---

## 18. Virtual Display Switching: global_prep_cmd vs ExecStartPre

### Architecture
There are two distinct moments to switch displays:

| Moment | Mechanism | Use |
|--------|-----------|-----|
| Service start (boot) | `ExecStartPre` in systemd override | Enable virtual display at boot so Sunshine can detect it |
| Client connect/disconnect | `global_prep_cmd` in `sunshine.conf` | Switch to virtual on connect, restore physical on disconnect |

Both are needed. `ExecStartPre` ensures HDMI-A-2 is active when Sunshine starts and scans displays. `global_prep_cmd` handles the per-stream lifecycle.

### global_prep_cmd format
```ini
# sunshine.conf
global_prep_cmd = [{"do":"/path/to/switch-to-virtual.sh","undo":"/path/to/switch-to-physical.sh"}]
```
- `do` runs when a client **connects** (before stream starts)
- `undo` runs when the **last client disconnects** (after stream ends)
- If `do` exits non-zero, stream is aborted

### switch-to-virtual.sh
```bash
#!/bin/bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/1000
export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

kscreen-doctor "output.HDMI-A-2.enable" "output.HDMI-A-2.primary" "output.HDMI-A-1.disable" 2>&1

# Set correct resolution (mode ID varies — look it up dynamically)
sleep 1
MODE_ID=$(kscreen-doctor -o 2>/dev/null | grep -oP '\d+:2560x1600@120\.\d+' | head -1 | cut -d: -f1)
if [ -n "$MODE_ID" ]; then
    kscreen-doctor "output.HDMI-A-2.mode.$MODE_ID" 2>&1
fi
```

### switch-to-physical.sh
```bash
#!/bin/bash
export WAYLAND_DISPLAY=wayland-0
export XDG_RUNTIME_DIR=/run/user/1000
export DISPLAY=:0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus

kscreen-doctor "output.HDMI-A-1.enable" "output.HDMI-A-1.primary" "output.HDMI-A-2.disable" 2>&1
```

### systemd override
```ini
# ~/.config/systemd/user/sunshine.service.d/override.conf
[Service]
ExecStart=
ExecStartPre=/bin/sleep 5
ExecStartPre=/bin/bash -c 'export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus DISPLAY=:0 && kscreen-doctor output.HDMI-A-2.enable output.HDMI-A-2.primary output.HDMI-A-1.disable 2>/dev/null; sleep 2; MODE_ID=$(kscreen-doctor -o 2>/dev/null | grep -oP "\\d+:2560x1600@120\\.\\d+" | head -1 | cut -d: -f1); [ -n "$MODE_ID" ] && kscreen-doctor "output.HDMI-A-2.mode.$MODE_ID" 2>/dev/null; true'
ExecStart=/home/$USER/.local/bin/sunshine
ExecStopPost=/bin/bash -c 'export WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus DISPLAY=:0 && kscreen-doctor output.HDMI-A-1.enable output.HDMI-A-1.primary output.HDMI-A-2.disable 2>/dev/null'
```

---

## 19. Audio Crackling Fix: PipeWire Quantum Mismatch

### Root Cause
Vibeshine reads audio using PulseAudio's `pa_simple` API with `fragsize = frame_size * channels * sizeof(float)`.

- Sunshine's audio frame size = `packetDuration * sampleRate / 1000` = `5ms * 48000 / 1000` = **240 samples**
- PipeWire's default quantum was **1024 samples** (~21ms)
- `pa_simple_read()` blocks until `fragsize` bytes are available, but PipeWire delivers data in 1024-sample chunks
- This misalignment causes timing jitter and buffer underruns → **crackling artifacts**

### Fix: Set PipeWire quantum to 240
```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d
cat > ~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf << 'EOF'
context.properties = {
    default.clock.rate = 48000
    # Match Sunshine's 5ms audio frame (240 samples @ 48kHz)
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
EOF
systemctl --user restart pipewire pipewire-pulse
```

### Verify
```bash
pw-metadata -n settings 2>/dev/null | grep quantum
# Expected: clock.quantum = '240'
```

---

## 20. Audio Config: virtual_sink vs audio_sink

### Difference
| Setting | Behaviour |
|---------|-----------|
| `audio_sink = <name>` | Capture from an existing sink's monitor. Host audio is NOT muted. You manage the sink yourself. |
| `virtual_sink = <name>` | Sunshine creates the virtual null-sink, sets it as system default, captures from its monitor, and restores host audio on disconnect. |

### Recommendation: use `virtual_sink`
```ini
# ~/.config/sunshine/sunshine.conf
virtual_sink = sink-sunshine-stereo
```
Sunshine will create `sink-sunshine-stereo` (float32le, 2ch, 48kHz) via PulseAudio's `module-null-sink` and manage its lifecycle. No manual sink setup needed.

### How Vibeshine creates the sink (source reference)
In `src/platform/linux/audio.cpp`:
```cpp
// Format: PA_SAMPLE_FLOAT32, 48000 Hz, 2ch (stereo) or 6/8ch (surround)
pa_sample_spec ss {PA_SAMPLE_FLOAT32, sample_rate, (std::uint8_t) channels};
// fragsize matches Sunshine's frame size exactly
pa_buffer_attr pa_attr = { .fragsize = uint32_t(frame_size * channels * sizeof(float)) };
```
The virtual sink runs at `float32le` — PipeWire handles format conversion to your physical audio device automatically.

---

## 21. CUDA Build on Arch Linux

### Install CUDA
```bash
sudo pacman -S cuda
# CUDA installs to /opt/cuda
```

### Build with CUDA
```bash
cmake .. \
  -DCMAKE_INSTALL_PREFIX=~/.local \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCUDA_TOOLKIT_ROOT_DIR=/opt/cuda
cmake --build . --parallel
```

### Verify CUDA symbols in binary
```bash
nm ~/.local/bin/sunshine | grep -i cuda | head -5
# Should show CUDA symbols if compiled correctly
```

### Set capabilities after install
```bash
sudo setcap cap_sys_admin+p ~/.local/bin/sunshine
# Verify
getcap ~/.local/bin/sunshine
# Expected: /home/user/.local/bin/sunshine cap_sys_admin=p
```

---

## 22. Complete Working Configuration

### System Setup Summary (Arch Linux / CachyOS, NVIDIA, Wayland/KDE)

#### Hardware
- Host GPU: NVIDIA RTX 3080 Ti, Driver 590.48.01
- Physical monitor: Samsung LS27A600U (HDMI-A-1), 2560x1440@75Hz
- Virtual display: HDMI-A-2 (via EDID firmware), 2560x1600@120Hz
- Streaming target: Lenovo Y700 tablet, 2560x1600, 120Hz

#### `~/.config/sunshine/sunshine.conf`
```ini
# Network
origin_web_ui_allowed = lan
upnp = on

# Display - stream to virtual display
output_name = HDMI-A-2
adapter_name = /dev/dri/renderD128

# FPS
fps = [30, 60, 90, 120]

# Audio - Sunshine manages virtual sink lifecycle
virtual_sink = sink-sunshine-stereo

# Encoder
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

# Display switching on client connect/disconnect
global_prep_cmd = [{"do":"/home/$USER/.config/sunshine/scripts/switch-to-virtual.sh","undo":"/home/$USER/.config/sunshine/scripts/switch-to-physical.sh"}]
```

#### `/etc/kernel/cmdline` (persistent boot params)
```
quiet nowatchdog splash drm.edid_firmware=HDMI-A-2:edid/samsung-q800t-hdmi2.1 video=HDMI-A-2:e rw rootflags=subvol=/@ root=UUID=<UUID>
```

#### `~/.config/pipewire/pipewire.conf.d/99-sunshine-audio.conf`
```ini
context.properties = {
    default.clock.rate = 48000
    default.clock.quantum = 240
    default.clock.min-quantum = 240
    default.clock.max-quantum = 2048
}
```

#### `/etc/mkinitcpio.conf` (EDID in initramfs)
```
FILES=(/usr/lib/firmware/edid/samsung-q800t-hdmi2.1)
```

---

## 23. From-Scratch Build on a Second Machine (RTX 3070, GCC 16, CUDA 13.3, no prior setup)

Everything above (§1-22) came from a machine that already had a working build. This section
documents what it took to go from a completely fresh clone + fresh CachyOS install (no Boost, no
CUDA, no Vibepollo deps at all) to a running, streaming daemon. See §10/§11 in `AGENTS.md` for the
two machines' specs side by side — the physical-display machine here has **no** virtual display
configured; that's deliberately out of scope for this pass.

### Submodules can silently land on the wrong commit
`git submodule update --init --recursive`, especially when given a path-limited pathspec (e.g. to
skip the large Windows-only `third-party/libwebrtc`/`third-party/depot_tools`), left most
submodules — including a **nested** one two levels down,
`third-party/build-deps/third-party/FFmpeg/x265_git` — checked out at their upstream branch tip
instead of the commit actually pinned in the superproject's tree. `git submodule status` shows
this as a `+`/`-` prefix instead of a clean commit line. The objects for the correct commit are
already fetched during init, so the fix is cheap and doesn't need a re-clone:
```bash
git submodule status                                    # look for +/- prefixes
git submodule update --force -- <path> [<path> ...]      # re-checkout the pinned commit
# for a nested mismatch, cd into the parent submodule first and repeat there
```
Building against the wrong submodule commit doesn't fail loudly — it just silently compiles
different third-party code than what's actually pinned, which can produce subtle bugs. Always
verify `git submodule status` is clean before trusting a build.

### glad's Python dependency vs. Arch's PEP 668
See the updated §1 "glad's Python deps" section in `AGENTS.md` — short version: glad's own code
imports `pkg_resources` (not just jinja2), Arch's `python-setuptools` 83.x dropped it, and Arch
blocks `pip install` into the system Python outright. A disposable venv +
`-DGLAD_SKIP_PIP_INSTALL=ON -DPython_EXECUTABLE=<venv python>` is the sanctioned escape hatch —
this exact mechanism already existed in `cmake/dependencies/glad.cmake` for Flatpak/Homebrew
builds, it just wasn't documented for a plain Arch system-Python case.

### CUDA host-compiler version mismatch causes a *link*-time ABI error, not a compile error
Arch's `cuda` package (13.3.1) installs a dedicated `gcc15` and points `nvcc` at it via
`$NVCC_CCBIN` in `/etc/profile.d/cuda.sh`, because `nvcc` enforces a maximum supported host-GCC
version and Arch's system `gcc` (16.x) is newer than CUDA 13.3 officially supports. Using that
gcc15 detour compiles fine, but the CUDA CMake machinery then adds `-L
/usr/lib/gcc/x86_64-pc-linux-gnu/15.x` to the *link* command ahead of the system compiler's own
implicit library path. Combined with this project's `-static-libstdc++`, the linker resolves the
static libstdc++ from gcc15's directory — which is *older* than the libstdc++ every other object
file was compiled against (by the system's default, newer gcc16 `c++`) — and is missing a symbol
version those objects need:
```
/usr/bin/ld: .../main.cpp.o: undefined reference to symbol '_ZNSt8__detail13__notify_implEPKvbRKNS_16__wait_args_baseE@@GLIBCXX_3.4.35'
/usr/bin/ld: /usr/lib/libstdc++.so.6: error adding symbols: DSO missing from command line
```
The fix is to skip the gcc15 detour entirely and let `nvcc` accept the system's own `g++` via its
official escape hatch, `--allow-unsupported-compiler`:
```bash
export PATH=/opt/cuda/bin:$PATH
cmake -B build -G Ninja -S . \
  -DSUNSHINE_ENABLE_CUDA=ON \
  -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++ \
  -DCMAKE_CUDA_FLAGS="--allow-unsupported-compiler"
```
**Important:** if you already ran `cmake` once with the gcc15 host compiler, you must delete
`build/CMakeCache.txt` before switching — CMake's CUDA ABI/implicit-link-directory detection is
cached and does not get re-run just because `CMAKE_CUDA_HOST_COMPILER` changed on a later
`cmake -B build` invocation; the stale `-L .../15.x` will keep appearing on the link line.

### Six source bugs, fixed on `master` (commit `55a98064`)
None of these are Linux-specific hacks — they're genuine cross-platform correctness bugs that
apparently never got exercised because Linux was never build-tested:

1. **`cmake/compile_definitions/linux.cmake`** hardcoded glad source paths
   (`third-party/glad/src/egl.c` etc.) that don't exist — the newer generator-based glad CMake
   integration (`cmake/dependencies/glad.cmake`) writes generated sources out-of-tree under
   `${CMAKE_BINARY_DIR}/gladsources/...` and compiles them into a proper `glad` library target
   that's already linked via `SUNSHINE_EXTERNAL_LIBRARIES`. The hardcoded list was redundant and
   pointed at nonexistent files — removed.
2. **`src/nvhttp.cpp`**: `has_active_or_stopping_stream_session()` was defined inside an
   `#ifdef _WIN32` block (alongside genuinely Windows-only virtual-display cleanup code) but
   called from code outside that block too. It only checks `rtsp_stream`/`stream::session`/
   `webrtc_stream` state, none of which is Windows-specific — hoisted above the `#ifdef`.
3. **`src/video.cpp`**: same pattern — `encode_session_teardown_mutex` and
   `native_amf_lifecycle_gate` (from the platform-independent `src/amf/amf_lifecycle.h`) were
   declared inside an unrelated `#ifdef _WIN32` block, then used from AMF encoder-lifecycle code
   that runs on all platforms. Hoisted above the `#ifdef`.
4. **`src/nvenc/nvenc_config.h`**: the `nvenc_config` struct had a field literally named
   `split_encode_mode` of type `split_encode_mode` (the enum). Legal C++ (the member hides the
   type name from that point on), but GCC 16 promotes `-Wchanges-meaning` to a hard error for it.
   Renamed the field to `split_encode_mode_value` (two call sites in `config.cpp`/
   `nvenc_base.cpp` updated); the external config key (`nvenc_split_encode`) was unaffected since
   it's a separate string, not tied to the field name.
5. **`src/nvenc/nvenc_base.cpp`**: `saved_init_params.encodeGUID == NV_ENC_CODEC_HEVC_GUID` don't
   compile — this platform's `GUID` (from `third-party/nv-codec-headers`) has no `operator==`.
   The file already had an `equal_guids()` memcmp-based helper for exactly this; used it instead.
6. **`src/platform/common.h`**: `platf::mem_type_e` was missing a `vulkan` enumerator that
   `src/platform/linux/pipewire.cpp`, `kmsgrab.cpp`, and `vulkan_encode.cpp` already referenced —
   Vulkan capture support was half-wired. Added the enumerator.
7. **`src/platform/linux/host_stats.cpp`**: a typo'd member reference, `_shutdown` (never
   declared) instead of the actual declared member `nvmlShutdown`. Fixed the 4 references.
8. **`src/platform/linux/publish.cpp`**: `platf::SERVICE_TYPE` is a `std::string_view`; avahi's
   `entry_group_add_service()` wants `const char*`. Added `.data()` (safe here since it's always
   constructed from a string literal, so it's guaranteed null-terminated).

### `cmake --install` partially fails without root — install the rest manually
`cmake --install build` installs the binary/assets to the user prefix fine, but its last three
steps (`udev` rule, systemd user unit, `modules-load.d` entry) target `/usr/lib/...` and need
root — it fails with a `CMake Error: file INSTALL cannot copy file ... Permission denied` on the
first one, aborting before the other two. Finish them manually:
```bash
sudo install -Dm644 src_assets/linux/misc/60-sunshine.rules /usr/lib/udev/rules.d/60-sunshine.rules
sudo install -Dm644 build/app-dev.lizardbyte.app.Sunshine.service \
    /usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service
sudo install -Dm644 src_assets/linux/misc/60-sunshine.conf /usr/lib/modules-load.d/60-sunshine.conf
sudo udevadm control --reload && sudo udevadm trigger && sudo modprobe uhid
```

### The installed systemd unit's `ExecStart=sunshine` fails with `status=203/EXEC`
The vendored unit uses a bare command name, relying on `$PATH`. systemd's user-manager `PATH`
does not include `~/.local/bin`, so the service fails to even exec the binary, despite the binary
running fine from an interactive shell. Override with an absolute path (see §5 above for the full
override file) — this is not optional for a `~/.local`-prefixed install.

### `uaccess`-tagged devices don't need a fresh login for group membership
Adding a user to `input`/`render` via `usermod -aG` normally requires logging out and back in
before the new group membership is active in any existing session. But `/dev/uinput` and
`/dev/uhid` carry the `uaccess` udev tag (from `60-sunshine.rules`), which means `systemd-logind`
grants the *active seat session's* user a POSIX ACL directly on the device node — check with
`getfacl /dev/uinput`. If a `user:<name>:rw-` ACL entry is already present, input device access
works immediately without waiting for group membership to take effect; only things gated purely
by *group* ownership (with no `uaccess` tag) still need the fresh login.

### Missing tray icon after manually installing icons — `plasmashell` needs a restart, not `sunshine`
Because of the `cmake --install` partial-failure above, the SVG icon files
(`apollo-tray.svg`/`apollo-playing.svg`/`apollo-pausing.svg`/`apollo-locked.svg`/`apollo.svg`) may
never get copied into `~/.local/share/icons/hicolor/scalable/{apps,status}/`. Installing them
manually and restarting the `sunshine` service is not enough on KDE Plasma: the tray icon is
resolved by *Plasma's* own `KIconLoader`/`KIconEngine` (via the StatusNotifierItem DBus interface),
not GTK — `third-party/tray/src/tray_linux.c` only ever passes an icon *name* (e.g. `apollo-tray`)
to `app_indicator_set_icon_full()`, never a theme path, so resolution is entirely delegated to
whichever desktop shell owns the tray. If `plasmashell` was already running when the icon files
were added, its icon cache/directory watches predate the new `scalable/status/` directory and it
never notices the new files — restarting `sunshine` (or even `gtk-update-icon-cache`, which is
GTK-only and irrelevant here) has no effect. Fix: restart the shell itself so it rescans icon
theme directories:
```bash
systemctl --user restart plasma-plasmashell.service
```

### "Open Apollo" tray menu logs success but no browser window appears to open
This was originally suspected to be a KDE/KWin focus-stealing issue (an already-open browser
window receiving the tab but not being raised) — that theory was disproven by direct testing:
after a tray click, no tab appeared anywhere (checked every virtual desktop/activity, including
minimized windows). The real cause is a dangling-pointer bug that silently corrupts the
environment handed to every child process `run_command()` spawns, not just `xdg-open`.

`platf::open_url()` builds a `bp::environment` snapshot via `bp::this_process::env()` and passes
it to `platf::run_command()`, which calls `env.to_process_environment()`
(`src/boost_process_shim.h`). That method used to build a local `std::vector<std::string>
env_buffer` and return `v2::process_environment(env_buffer)` in one expression. Boost.Process v2's
`process_environment` constructor has two `build_env` overloads selected by whether the argument's
element type converts to `cstring_ref`; `std::string` does (via `.c_str()`), so the *non-owning*
overload runs — it collects raw `e.c_str()` pointers into `process_environment::env` and leaves
the copying member (`process_environment::env_buffer`) empty. Those pointers point into
`env_buffer`, a function-local vector destroyed the instant `to_process_environment()` returns —
so the returned `process_environment` is dangling before `run_command()` even uses it to `execve`
the child.

This was confirmed two ways: a PATH-shimmed `xdg-open` (intercepting the real binary, logging
argv/env/fds, then `exec`-ing through) showed the child process missing `HOME`, `PATH`,
`DBUS_SESSION_BUS_ADDRESS`, `WAYLAND_DISPLAY`, `XDG_RUNTIME_DIR`, `XDG_CURRENT_DESKTOP`, and
`XDG_SESSION_TYPE`, plus a garbled trailing environment entry (classic freed-memory garbage).
Separately, `pkexec cat /proc/<sunshine-pid>/environ` (needed because Sunshine's `setcap`
capabilities make it non-dumpable, so `/proc/<pid>/environ` isn't readable without root) proved
Sunshine's *own* environment has every one of those variables intact — the corruption happens
strictly in the copy-to-child path, not in the session or the systemd user service. Without
`DBUS_SESSION_BUS_ADDRESS`/`XDG_RUNTIME_DIR`, `xdg-open` has no session bus to hand the URL to a
browser through, which is why the process spawns cleanly, logs `Opened url [...]` with no error,
and nothing ever opens.

Fixed by splitting `to_process_environment()` into `to_env_strings()` (returns the owned
`std::vector<std::string>`/`vector<std::wstring>`) and constructing the `process_environment` at
each call site with the owning vector declared first, so it outlives the `process_environment`
built from it:
```cpp
auto env_strings = env.to_env_strings();       // declared first: owns the data
v2::process_environment env_init {env_strings}; // views into env_strings; destroyed first
```
This affects `src/platform/linux/misc.cpp` and `src/platform/macos/misc.mm` (both call
`to_process_environment()`); Windows's `run_command()` never called it, so it was unaffected. The
bug wasn't limited to `open_url()` — `run_command()` is the single spawn path for game launches
and prep/undo commands too (`src/process.cpp`, `src/stream.cpp`), so every child process Sunshine
spawned on Linux/macOS was receiving a corrupted environment, not just the browser-opening one.

Do not "fix" this by passing `std::vector<v2::environment::key_value_pair>` instead of
`vector<string>` to force the copying `build_env` overload — `key_value_pair` also exposes
`.c_str()`, so it likely converts to `cstring_ref` too and silently re-selects the dangling-pointer
path. Do not cache the buffer as a `mutable` member of `basic_environment` mutated from a `const`
method either — `process.cpp` passes long-lived `_env` members into `run_command()` from multiple
call sites, so a shared mutable buffer risks a data race across concurrent launches.

## 24. Full Feature-Set Audit (2026-07-29): Live Verification of Virtual Display, Frame Limiting, Steam Sync

A full pass verifying every entry in the "Vibepollo-specific feature map" (see root `CLAUDE.md`)
against a rebuilt, reinstalled, freshly-restarted service — not just source review. Full findings
doc: `verify-vibepollo-linux-feature-set-audit.md` was the stale draft; this session's complete
report lives outside the repo at the time of writing, but the load-bearing findings are captured
here for anyone building on this work later.

### `CAP_SYS_ADMIN` is lost on every reinstall — this is not a one-time setup step

`cmake --install` does not reapply `setcap`. Any reinstall (including CI-adjacent local rebuilds)
silently breaks KMS capture until `sudo setcap cap_sys_admin+p ~/.local/bin/sunshine-<version>` is
rerun. Confirmed via `getcap` before/after and a clean journal (no `CAP_SYS_ADMIN` errors, both
`h264_nvenc`/`hevc_nvenc` probe successfully) after reapplying. Treat this as a required
post-reinstall step every time, not a first-run-only fix — see "Install + capabilities" in
`AGENTS.md`.

### Frame limiting (`src/platform/linux/frame_limiter.cpp`) — confirmed working end-to-end

`frame_limiter_enable` defaults to `false` (not present in a stock `sunshine.conf`), so the
feature is inert unless explicitly turned on. With it enabled, launching an app that dumps its own
environment to a file showed the real injected values in the child process:
```
__GL_SYNC_TO_VBLANK=0
MANGOHUD_CONFIG=fps_limit=60000
```
This confirms the mechanism itself works, not just that the code compiles. Caveats that still
apply: MangoHud's FPS cap only takes effect if the launched app's own command already runs under
MangoHud (no `LD_PRELOAD` force-injection); the NVIDIA path only toggles vsync, there's no real
Linux equivalent of NVCP's FPS cap; there is still no `/api/rtss/status`-equivalent endpoint on
Linux.

**Resolved 2026-07-29**: `FrameLimiterStep.vue` was unconditionally Windows-shaped — it polled
the (Windows-only) `/api/rtss/status` on every page load, and offered the `rtss` provider option,
the RTSS install-path field, and the SyncLimiter mode selector regardless of platform. Added an
`isWindows` gate (`config.platform === 'windows'`, the same convention `AppEditModal.vue`/
`Playnite.vue` already use) around all of that, so Linux no longer fires a guaranteed-404 status
poll or shows RTSS-only fields with nothing behind them. The platform-agnostic core — enable,
provider (`auto`/`nvidia-control-panel`), FPS limit, disable-vsync, virtual-display capture mode —
stays visible on Linux, since it's the part confirmed working above. A short Linux-only notice now
explains the MangoHud/NVIDIA env-var mechanism in place of RTSS.

### Steam library sync (`src/platform/linux/steam_library.cpp`) — backend confirmed live, no frontend yet

`GET /api/steam/status` against a real Steam installation returned 141 correctly-discovered
installed games. `POST /api/steam/sync` (the actual `apps.json`-mutating endpoint) was
deliberately not exercised in a verification pass, since it writes real data. There is currently
**no web UI** for this feature at all — confirmed no `steamApi.ts`/`LibrarySync.vue` and no
router entry exist yet; the only way to trigger a sync today is a direct authenticated
`curl POST /api/steam/sync` call.

### Virtual display (`src/platform/linux/display_device.cpp`) — apply-side confirmed, revert-side still open

Launching any app correctly invokes the apply hook at `process.cpp`'s `launch_app_commands()`
(`Linux display device: no output_name configured; skipping virtual display automation.` when
unconfigured, which is the safe/correct no-op). No session in this repo's history yet has
exercised the **revert** path (`terminate()` → `revert_session_display`) — every test launch used
either a no-op app or one whose process exits immediately, so `terminate()`'s revert branch never
actually ran. Verifying revert needs a real long-running launched process, then a `close`/stop
call, watched for the `"reverted to pre-session display arrangement"` log line. A true test against
a real EDID virtual output (§11/§16-18 above) still requires a reboot with kernel params and
remains undone.

### A quick, low-risk way to observe env/config effects on a real launched process without a Moonlight client

`POST /api/apps/launch` (with a session cookie from `POST /api/auth/login`) will launch any app
in `apps.json` by `uuid` without a real streaming client — useful for exactly this kind of
verification. Two gotchas:
- It requires a `rikeyid` query-string param (e.g. `?rikeyid=0`) even for local/Web-UI-triggered
  launches — `nvhttp.cpp`'s `make_launch_session()` has an unconditional `get_arg(args, "rikeyid")`
  with no default late in the function, outside the guard that already skips key/IV setup for
  local launches. This is pre-existing upstream code (`git blame` → commit `41cc08d5`), not
  something the Linux port introduced, and it's currently dead/unused — no component in the
  current Vue SPA calls `stores/apps.ts`'s `launchApp()` (only the pre-migration, inactive
  `apps.html` does), and it isn't documented in `docs/api.md` either. Still worth a real fix, just
  not urgent.
- Launching the same `uuid` a second time while it's still "current" acts as a stop toggle
  (sends `SIGTERM`, tears the app down) rather than relaunching — call `POST /api/apps/close`
  first if you need a guaranteed-clean relaunch.
- Any `cmd` you use for this kind of manual test needs an explicit shell wrapper
  (`sh -c "... > /path"`) — commands run directly via `run_command()`, not through a shell, so
  redirection operators are passed as literal argv to the target binary and silently do nothing.

### `origin_web_ui_allowed = wan` combined with an all-interfaces bind is worth auditing on any box

`ss -lntp` showed the config UI bound to `0.0.0.0:47990` (not loopback-only) with
`origin_web_ui_allowed = wan` in `sunshine.conf` — the documented reference config in `AGENTS.md`
uses `lan`. `upnp = on` but the journal logged repeated `Couldn't discover any IPv4 UPNP devices`,
meaning automatic port-forwarding isn't active — this doesn't confirm the box is internet-exposed
(a manual router forward can't be ruled out from the daemon side), but the app-layer safeguard
that would normally restrict this to LAN-only is not what's actually configured. Worth checking
deliberately on any machine following this doc, not assuming `wan` was intentional.

**Resolved 2026-07-29**: confirmed unintentional on this box; set to `origin_web_ui_allowed = lan`
and restarted the service. Verified the fix didn't break loopback/authenticated access
(`GET /api/metadata` with a valid session cookie still returns `200`; a `401` with no cookie is
just the normal auth gate, not an origin block).
