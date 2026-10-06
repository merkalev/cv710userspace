# Devlog 01: Reverse Engineering the AVerMedia ExtremeCap U3 (CV710)

## Credits
Based on the foundational userspace capture driver by ChrisAJS (https://github.com/ChrisAJS/lgx2userspace). This devlog documents protocol analysis and stability improvements for the AVerMedia ExtremeCap U3 (CV710).

---

## 1. Introduction and Objectives

The AVerMedia ExtremeCap U3 (CV710 / C877, USB ID `07ca:0710`) is an uncompressed USB 3.0 capture device capable of 1080p60 YUY2 video capture and 48 kHz stereo audio. Unlike modern UVC capture dongles, the CV710 uses proprietary vendor-specific USB bulk streaming protocols with custom FPGA framing and zero onboard hardware compression.

While legacy community attempts often suffered from rolling line shearing, audio buzzing, and dropped signal timeouts, this devlog records the complete journey of reverse engineering the official vendor drivers and wire captures to achieve pixel-perfect video and clear audio on Linux.

---

## 2. Reverse Engineering the Vendor Drivers

To understand the exact framing protocol and internal hardware state machine, we extracted and disassembled both the official macOS driver (`libC877Driver.dylib`) from `RECentral.dmg` and the Windows 10 x64 kernel driver (`avmu3_x64.sys`) from `CV710_Win10Logo_Drv_x64_V3.0.64.118_Install.exe`.

### Key Classes in the Architecture
Both vendor drivers share a common C++ core architecture:
- `ParseAVerData`: Manages incoming USB bulk stream packets, hunts for ancillary headers, validates FPGA checksums, and coordinates state transitions.
- `VideoParseBuffer`: Accumulates raw pixel data, handles progressive and dual-field interlaced scanning, and manages output buffers.
- `AudioParseBuffer`: Assembles L-PCM audio samples and strips DMA alignment padding.
- `CRXADV7604`: Interfaces with the Analog Devices ADV7604 HDMI receiver over I2C to query signal locks and timing.
- `CLFE3`: Interacts with the Lattice ECP3 FPGA registers.

### The C0 Frame Start Header and FPGA Checksum
Disassembly of `ParseAVerData::updateVideoStartInfo` (macOS `0x0000674e`, Windows `0x14009ae89`) revealed the exact 2-word header opening every video frame:
```
Word 0: 0xC0FFFF00  (VIDEO_FRAME_START_MARKER)
Word 1: [b3][b2][b1][b0]  (32-bit metadata word)
```

The metadata byte layout:
- `b0`: Frame sequence counter (0x00 to 0xFF, incrementing by 1 per frame).
- `b1`: Fixed validation flag, must equal `0x01`.
- `b2`: Scanning and field mode flags:
  - `b2 & 0x80`: Interlaced / dual-field indicator.
  - `b2 & 0x01`: Field parity (0 = Even / Top, 1 = Odd / Bottom).
- `b3`: Hardware FPGA checksum byte.

The driver verifies that Word 1 is a valid FPGA header and not random image pixels matching `0xC0FFFF00` using the checksum formula:
```cpp
uint8_t calculated = (b0 + b1 + b2 - 0x40) & 0xFF;
if (b1 == 0x01 && calculated == b3) {
    // Verified genuine C0 video frame start
}
```

### The C1 Frame End Trailer and Checksum
Disassembly of `ParseAVerData::updateVideoEndInfo` (macOS `0x00006a30`) revealed that active video frames are terminated by a 3-word sequence:
```
Word 0: 0xC1FFFF00  (VIDEO_FRAME_END_MARKER)
Word 1: [b3][b2][b1][b0]  (trailer word 1)
Word 2: [b7][b6][b5][b4]  (trailer word 2)
```

Byte assignments:
- `b0`: Sequence number, verified to match the C0 sequence number (`b0 == c0_seq`).
- `b1`: Fixed flag byte, equals `0x02`.
- `b2`: Hardware sync status flag:
  - `0x04`: Full 1080p frame lock confirmed.
  - `0x14`: HDMI clock syncing or PLL recovering (partial frame).
- `b3`: Fixed flag byte, equals `0x38`.
- `b4`: Trailer validation checksum byte (lowest byte of trailer word 2).

The vendor checksum formula:
```cpp
uint8_t expectedChecksum = (b0 + b1 + b2 + b3 - 0x3F) & 0xFF;
if (expectedChecksum == b4 && b0 == c0_seq) {
    // Verified genuine C1 frame end
}
```

### Audio Packet Framing and Inter-Frame Gap
Disassembly of `ParseAVerData::updateAudioInfo` (macOS `0x00006aa2`) revealed that audio packets are never embedded inside active video scanlines. Audio packets appear strictly in the inter-frame gap between C1 and the following C0:
```
Word 0: 0x58FFFF00  (AUDIO_FRAME_START_MARKER)
Word 1: [rawLen]     (Big-endian byte count word)
Word 2..N: [PCM Data] (16-bit signed stereo L-PCM samples)
Word N+1..M: 0xAA5555AA (Padding to 32 KB DMA boundary)
```
The payload length in bytes is decoded via:
```cpp
uint32_t audioBytes = (((rawLen >> 16) & 0xFF) << 8) | ((rawLen >> 24) & 0xFF);
```
Standard payload sizes for 48 kHz stereo 16-bit audio:
- 60.00 fps: 800 samples = 3,200 bytes per frame.
- 50.00 fps: 960 samples = 3,840 bytes per frame.
- 59.94 fps: Alternating 800 and 801 samples (3,200 and 3,204 bytes).

---

## 3. Packet Capture Analysis (host_u3cold.pcapng)

Analyzing raw USB bus traffic recorded from live CV710 hardware (`host_u3cold.pcapng`) confirmed our driver disassembly:

1. **Cold-Boot Sync Locking**:
   - Frames 1 to 10 immediately following initialization had trailer status `b2 = 0x14` (`0x381402xx`).
   - Active line counts during this initial phase fluctuated (e.g. Frame 1 = 822 lines, Frame 2 = 1064 lines, Frame 4 = 878 lines).
   - This represents the ADV7604 HDMI receiver locking its PLL and clock recovery circuits.

2. **Locked Progressive 1080p Video**:
   - Starting at Frame 11, the trailer status switched to `b2 = 0x04` (`0x3804020b`).
   - Every locked frame contained EXACTLY `1,036,800` uint32 words (`1920 * 1080 / 2`), matching 1080.00 active lines.
   - Extracting and rendering Frame 11 produced a 100% pixel-perfect image with zero tearing, zero line displacement, and zero color distortion.

---

## 4. Root Cause Analysis and Problem Resolution

### Issue 1: "Single Frame Rendered Then No Signal"
- **Cause**:
  1. The stream parser previously enforced an exact equality check `if (frameWords == 1036800)` at the C1 marker. During initial HDMI sync locking, frames with slightly fewer lines (e.g. 1064 lines) were discarded.
  2. A premature mid-slice emission check previously triggered when the buffer reached 1,036,800 words inside a USB transfer slice, clearing the frame builder and setting `_inVideo = false` before C1. This caused subsequent words to be discarded, breaking phase for the next frame.
  3. Dropping frames repeatedly caused the 500 ms signal timeout to expire, reverting the display to "No Signal".
- **Fix**:
  - Removed premature mid-slice emissions. Video is accumulated continuously between C0 and C1.
  - Slices are clamped to `CV710_1080P_FRAME_WORDS` without setting `_inVideo = false`.
  - When C1 arrives, if `frameWords >= MINIMUM_VIDEO_FRAME_WORDS` (200,000 words), any missing lines are padded with legal YUY2 studio black (`0x80108010`). The exact 1,036,800-word buffer is emitted to the display.
  - Video streams smoothly at 60 fps without false timeouts.

### Issue 2: "Lines Aren't Aligned / Left to Right Shearing"
- **Cause**:
  1. Setting the default output format to `Direct_YUY2` relied on SDL3's OpenGL/Vulkan YUY2 texture shader. In current SDL3 Linux backends, streaming YUY2 textures exhibit a horizontal stride calculation bug where the 1920-wide raster is rendered as two duplicated 960-wide halves side by side with alternating line comb displacement.
  2. Idle/padding words leaking into the frame accumulator previously shifted scanlines by words (2 pixels per word), wrapping the 1920-pixel stride.
- **Fix**:
  - Changed default colorspace mode in `SdlVideoOutput` and `OptionParser` to `bt709` (`ColorspaceMode::BT709_Limited`).
  - Implemented high-performance software fixed-point Rec.709 conversion that converts YUY2 macropixels directly into 32-bit RGBA and uploads to `SDL_PIXELFORMAT_RGBA32`.
  - This eliminates SDL3 YUY2 shader bugs completely, providing 100% aligned lines out of the box.

### Issue 3: Standby Screen and Background Cleanup
- **Cause**:
  - The preview window previously cleared to dark slate blue and rendered extraneous debug text across the canvas.
- **Fix**:
  - Set background clear color to solid black (`0, 0, 0, 255`).
  - Removed extraneous debug text labels from the preview canvas.

### Issue 4: "Audio Buzzing During Mode Switches"
- **Cause**:
  - Disconnecting HDMI or changing refresh rate disrupts the ADV7604 audio clock regeneration (ACR) PLL, causing FIFO under-runs that output stale DC loops and floating noise.
- **Fix**:
  - Added a hardware watchdog and video sync lock gate in `lgxdevice.cpp`.
  - Audio is muted immediately upon signal loss (> 250 ms) or sequence discontinuity.
  - Audio unmuting requires at least 2 consecutive valid video frames.
  - Flushed pending SDL audio streams via `clearAudio()`.

### Issue 5: "Address Boundary Crashes (SIGSEGV) at No Signal and Mode Switches"
- **Cause**:
  - In `lgxdevice.cpp`, missing C1 trailers during signal disconnect or resolution switching allowed video accumulation to exceed 1,036,800 words. On subsequent transfer slices, `CV710_1080P_FRAME_WORDS - curWords` underflowed unsigned 32-bit math to `4,294,967,295`, passing a massive length to `buildVideo()` and causing `memcpy` to fault past the 2 MB transfer buffer.
  - In `UsbStream.cpp`, non-fatal transfer timeouts/stalls triggered fatal `signalError()` exceptions, causing shutdown while completion callbacks concurrently modified `_transfers` via `erase()`, producing iterator invalidation and use-after-free faults.
- **Fix**:
  - Enforced strict word limits in `lgxdevice.cpp` preventing accumulation beyond `CV710_1080P_FRAME_WORDS` and eliminating underflow.
  - Replaced vector mutation during completion callbacks with an atomic in-flight counter (`_activeTransfers`) and safe shutdown synchronization.
  - Non-fatal transfer statuses are retried; stalls are cleared with `libusb_clear_halt`.

### Issue 6: "Chromatic Shift and Rightward Color Bleed"
- **Cause**:
  - Nearest-neighbor chroma assignment applied the same Cb/Cr values to both even and odd pixels without horizontal filtering, shifting color edges 1 pixel to the right and causing color bleed on text.
- **Fix**:
  - Implemented co-sited linear horizontal chroma reconstruction in `SdlVideoOutput::convertYuy2ToRgba`: even pixels use co-sited chroma, while odd pixels interpolate halfway to the next chroma site.

---

## 5. Verification and Current Status

1. **Clean Frame Render**: Frame 11 extracted from `host_u3cold.pcapng` verified 100% clean 1920x1080 progressive output with zero line shearing.
2. **Build Cleanliness**: Compiles cleanly on GCC/Clang under Release, Debug, and AddressSanitizer (ASan) configurations.
3. **Hardware Focus**: Dedicated exclusively to the AVerMedia CV710.
