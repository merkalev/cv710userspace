# cv710userspace

## Massive Credit and Attribution
This project is based on and extends the outstanding userspace capture driver originally designed and developed by **ChrisAJS** (https://github.com/ChrisAJS/lgx2userspace). Massive thanks and credit to ChrisAJS for reverse-engineering the USB stream format and laying the groundwork for Linux userspace capture on AVerMedia hardware.

---

## Overview

**cv710userspace** is a high-performance userspace driver and capture application dedicated exclusively to the **AVerMedia ExtremeCap U3 (CV710 / C877)** (USB ID `07ca:0710`) on Linux (and cross-platform).

The application captures uncompressed raw video and audio directly over SuperSpeed USB bulk endpoints using `libusb` and presents them via:
- An interactive standalone SDL3 preview window.
- A virtual V4L2 loopback webcam device (`/dev/videoN`) ready for OBS Studio, Discord, Zoom, or Chromium.

---

## Key Features

1. **Hardware Protocol Framing**:
   - Eliminates legacy manual timing nudges and drift.
   - Hardware FPGA checksum validation (`((b0 + b1 + b2 - 0x40) & 0xFF) == b3`).
   - Hardware C1 trailer validation (`((b0 + b1 + b2 + b3 - 0x3F) & 0xFF) == b4`).
   - Frame continuity tracking and single-frame transport gap resynchronization under CPU lag.
   - Stable 60 fps active capture with zero memory leaks.

2. **Real-Time Colorspace Conversion & Linear Chroma Reconstruction**:
   - Solves chromatic distortion (neon pink/green or amber tinting) when capturing from Apple MacBooks, iPads, or PC GPUs outputting RGB Full-Range over HDMI.
   - Eliminates 1-pixel rightward red bleeding via co-sited linear horizontal chroma reconstruction ($C_{odd} = (C_0 + C_1)/2$).
   - Supports 6 selectable colorspace profiles:
     - `bt709` (BT.709 Limited Range: Standard 1080p HDTV Rec.709 Studio levels [16-235]) - Default
     - `bt709full` (BT.709 Full Range: PC / Mac HDMI Full Range levels [0-255])
     - `bt601` (BT.601 Limited Range: SDTV / Legacy consoles)
     - `bt601full` (BT.601 Full Range: PC SD levels)
     - `uyvy` (UYVY Swap: Inverts luma/chroma for devices sending UYVY)
     - `yuy2` (Direct YUY2: Pass-through raw hardware stream to GPU shaders)
   - Cycle modes at runtime using the `C` key, with visual On-Screen Display (OSD) feedback.

3. **Audio Watchdog Muting (No Buzzing)**:
   - HDMI audio clock regeneration (ACR) loses PLL lock during resolution switches, refresh rate changes, or cable disconnects.
   - The driver gates audio production on active video sync. Audio is automatically muted and buffers are flushed during signal loss or sequence gaps, preventing buzzing or static through speakers.
   - Audio is smoothly restored once 2 consecutive valid video frames arrive cleanly.

4. **Official "No Signal" Splash Screen**:
   - Integrates authentic 640x480 AVerMedia splash bitmaps extracted directly from the vendor driver (`aver_custom_no_signal.bmp`).
   - Centered on a pure black canvas (`0, 0, 0, 255`) without bloat text or footer distractions.

5. **Deep USB Queue Pipeline**:
   - Drains transfers continuously without per-frame memory allocation using reusable buffer pools.
   - Prevents USB transfer drops even under compositor or window manager stutter.

---

## Building

### Prerequisites
- CMake 3.18 or newer
- C++17 compatible compiler
- `libusb-1.0`
- `SDL3`
- `v4l2loopback` (for virtual camera output)

### Package Installation

#### Ubuntu / Debian (24.04+)
```bash
sudo apt install cmake libusb-1.0-0-dev libsdl3-dev v4l2loopback-dkms v4l2loopback-utils
```

#### Arch Linux / Manjaro
```bash
sudo pacman -S cmake libusb sdl3 v4l2loopback-dkms v4l2loopback-utils
```

#### Fedora
```bash
sudo dnf install cmake libusb1-devel SDL3-devel v4l2loopback
```

### Compilation

```bash
# Configure and build Release target
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

For debugging with AddressSanitizer:
```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
cmake --build build-asan -j$(nproc)
```

---

## Udev Rules Setup

Linux requires non-root user permissions to access the USB capture endpoints:

```bash
sudo cp 99-avermedia-cv710.rules /etc/udev/rules.d/99-avermedia-cv710.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

After updating rules, unplug and reconnect the capture card.

---

## Usage

### Basic Execution (SDL Preview Window)
```bash
./build/src/cli/cv710userspace
```

The device will automatically be detected and stream at 1080p60.

### Selecting Colorspace
```bash
# Start directly in Mac/PC RGB Full-Range mode:
./build/src/cli/cv710userspace -c bt709full

# Start in direct hardware YUY2 mode:
./build/src/cli/cv710userspace -c yuy2
```

### Interactive Window Controls
- `C`: Cycle colorspace profiles in real time (shows OSD notification).
- `F`: Toggle Fullscreen mode.
- `G`: Exit Fullscreen (return to windowed mode).
- `Esc` or window close: Quit application.

### Output Scaling (Lower CPU Usage)
```bash
./build/src/cli/cv710userspace -S 2   # 1/2 scale (960x540)
./build/src/cli/cv710userspace -S 4   # 1/4 scale (480x270)
```

### Streaming to V4L2 Loopback (OBS Studio, Discord, Webcams)

1. Load the `v4l2loopback` kernel module:
   ```bash
   sudo modprobe v4l2loopback video_nr=99 exclusive_caps=1 card_label="CV710"
   ```

2. Direct video output to `/dev/video99`:
   ```bash
   ./build/src/cli/cv710userspace -d /dev/video99
   ```

3. Open OBS Studio, add a Video Capture Device (V4L2), and select `CV710 (/dev/video99)`.

---

## Command Line Reference

```
cv710userspace usage:
  -h            Print usage message
  -c COLORSPACE Specify initial colorspace (bt709, bt709full, bt601, bt601full, uyvy, yuy2)
  -S SCALE      Specify output scaling (1 = Full 1080p, 2 = Half 540p, 4 = Quarter 270p)
  -d DEVICE     Specify V4L2 Loopback device node (e.g. /dev/video99)
  -v            Print diagnostic timing summary at exit
  -V            Print real-time diagnostic timing information during execution
  -s            Output audio only (no video display)
  -g            Output video only (no audio playback)
  -f            Use fake USB stream from dump.bin file
```

---

## Technical Documentation
For in-depth reverse engineering analysis, FPGA checksum formulas, packet framing diagrams, and hardware register details, see:
- [PROTOCOL.md](PROTOCOL.md): Wire protocol specification and packet framing details.
- [ISSUES.md](ISSUES.md): Hardware quirks, interlaced weaving roadmap, and driver reverse-engineering notes.
- [devlog/01-cv710-hardware-framing.md](devlog/01-cv710-hardware-framing.md): Detailed reverse-engineering journey and disassembly analysis.

---

## Attributions and Thanks
- **ChrisAJS** (https://github.com/ChrisAJS/lgx2userspace): Original creator and architect of `lgx2userspace`.
- **libusb project** (https://libusb.info/): SuperSpeed bulk USB communication.
- **SDL project** (https://libsdl.org/): High performance audio and video display.
- **V4L2Loopback** (https://github.com/umlaeute/v4l2loopback): Virtual webcam loopback driver.
