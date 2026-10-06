# cv710userspace

A Linux userspace driver and capture application for the **AVerMedia ExtremeCap U3 (CV710)** (USB ID `07ca:0710`), forked from [ChrisAJS/lgx2userspace](https://github.com/ChrisAJS/lgx2userspace).

It captures uncompressed 1080p60 raw video and PCM audio directly over USB 3.0 bulk endpoints using `libusb`, with output to an SDL3 preview window or a `v4l2loopback` virtual webcam (for OBS Studio, Discord, Zoom, etc.).

---

## Features

- **Hardware Framing**: Protocol-level FPGA frame validation (`0x40` checksum and `0xC1` trailer verification) for rock-solid 60 fps sync without timing drift.
- **Color Correction**: Co-sited linear chroma reconstruction eliminates red color bleed. Real-time toggle between BT.709 limited/full range, BT.601, UYVY, and raw YUY2.
- **Audio Watchdog**: Automatic muting during signal loss or mode changes to eliminate static and buzzing.
- **V4L2 Loopback**: Feeds virtual video devices (`/dev/videoN`) for OBS Studio and browser conferencing.

---

## Requirements

- Linux with a USB 3.0 controller
- CMake 3.18+, C++17 compiler
- `libusb-1.0`, `SDL3`, `v4l2loopback` (optional, for virtual camera)

### Install Dependencies

**Ubuntu / Debian (24.04+)**:
```bash
sudo apt install cmake g++ libusb-1.0-0-dev libsdl3-dev v4l2loopback-dkms
```

**Arch Linux**:
```bash
sudo pacman -S cmake gcc libusb sdl3 v4l2loopback-dkms
```

**Fedora**:
```bash
sudo dnf install cmake gcc-c++ libusb1-devel SDL3-devel v4l2loopback
```

---

## Build & Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Udev Rules (Non-root USB Access)

```bash
sudo cp 99-avermedia-cv710.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```
After copying rules, unplug and reconnect the capture card.

---

## Usage

### Quick Start (SDL Preview)
```bash
./build/src/cli/cv710userspace
```

### Stream to OBS / Virtual Camera
```bash
# 1. Create a loopback device
sudo modprobe v4l2loopback video_nr=99 exclusive_caps=1 card_label="CV710"

# 2. Start capturing to the device
./build/src/cli/cv710userspace -d /dev/video99
```

### Options & Shortcuts

| Key / Option | Action |
|---|---|
| `C` | Cycle colorspace profiles (BT.709, BT.709 Full, BT.601, UYVY, YUY2) |
| `F` | Toggle Fullscreen |
| `Esc` | Quit application |
| `-c COLOR` | Set initial colorspace (`bt709`, `bt709full`, `bt601`, `bt601full`, `uyvy`, `yuy2`) |
| `-S SCALE` | Downscale output (`1` = 1080p, `2` = 540p, `4` = 270p) |
| `-d DEVICE` | Output to V4L2 loopback device (e.g. `/dev/video99`) |
| `-g` | Video only (disable audio) |
| `-s` | Audio only (disable video preview) |

---

## Documentation

- [PROTOCOL.md](PROTOCOL.md): Wire protocol specification and packet framing details.
- [ISSUES.md](ISSUES.md): Known hardware quirks and reverse engineering notes.
- [devlog/01-cv710-hardware-framing.md](devlog/01-cv710-hardware-framing.md): Detailed reverse engineering devlog.

---

## Credits & License

Forked from [lgx2userspace](https://github.com/ChrisAJS/lgx2userspace) by Chris Sawczuk (ChrisAJS). Released under the [MIT License](LICENSE.md).
