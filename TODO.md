# Project Roadmap and TODO List

## Priority: Sub-1080p Resolution Support (Immediate Focus)
- [ ] **ADV7604 STDI (Standard Identification) Register Polling**:
  - Implement I2C queries to ADV7604 IO map (`0x20`) registers `0x88` - `0x8E` to read incoming line count and frame rate.
  - Dynamically switch `VID_STD` (e.g., `0x13` for 720p60, `0x0A` for 480p60) when input format changes from 1080p.
- [ ] **Multi-Resolution EDID Generation** — **Won't do (by design)**:
  - The captured bootstrap/EDID bytes in `UsbStream.cpp` (`0x6C` RAM) must remain exactly as captured. Advertising extra CEA-861 timings would change device behaviour and is out of scope. Sub-1080p support, if needed, is handled purely at the display/V4L2 layer from the receiver-reported geometry.
- [ ] **Frame Size Calibration for Sub-1080p**:
  - Verify exact word count outputs for 720p (`460,800` words), 576p (`207,360` words), and 480p (`172,800` words) under locked STDI timing. Requires a sub-1080p source on the bench.

---

## Performance and Pipeline Enhancements (Feasible Implementations)
- [~] **GPU-Accelerated Zero-Copy Shaders** — **shipped as an opt-in mode**:
  - The `Direct_YUY2` colorspace mode already uploads raw YUY2 macropixels as an `SDL_PIXELFORMAT_YUY2` texture and lets SDL/the GPU do the YUV→RGB conversion (near-zero CPU). Select it via `-c yuy2` / `--colorspace yuy2`. It is not the default because the GPU path uses standard chroma reconstruction and cannot replicate the CV-09 co-sited reconstruction used by the CPU path.
- [x] **SIMD AVX2 Intrinsics**: `convertYuy2ToRgba` now has a runtime-dispatched AVX2 row converter (8 macropixels/iteration, `_mm256_*`) that is bit-for-bit identical to the scalar path (including CV-09). NEON for ARM64 remains open.

- [x] **Dynamic V4L2 Loopback Format Re-negotiation**: `V4LFrameOutput` now renegotiates `VIDIOC_S_FMT` when the incoming geometry changes and only writes on new frames (CV-17). A consumer holding a fixed format is detected and mismatched frames are dropped instead of corrupting the stream.
- [x] **Virtual Audio Loopback Output**: `--audio-device <name|index>` and `--audio-loopback` route captured audio to any SDL playback device, including an `snd-aloop`/PipeWire virtual sink, so it can be captured without desktop playback (CV-19). `--list-audio-devices` enumerates options. V4L2 output auto-prefers a loopback sink when one is present.
- [ ] **Aspect Ratio Preservation for SD Sources**:
  - Add configurable aspect ratio handling (`16:9` vs `4:3` pillarboxing) for 480p and 576p video.
- [ ] **HDCP Detection and Status Notification** — **Won't do (by design)**:
  - Intentionally out of scope. The device is used on unencrypted HDMI only, and no HDCP interrogation/notice is to be implemented.

---

## Hardware and Architectural Limitations (Not Implementable)
- [x] **Component (YPbPr) Analog Video**: Officially classified as unsupported. Requires proprietary analog breakout cable, ADV7604 analog front-end ADC calibration, sync slicer clock recovery, and TLV320AIC3101 analog audio routing.
- [x] **Hardware Video Compression (H.264 / HEVC)**: Not possible. The CV710 hardware contains no onboard hardware encoder ASIC.
- [x] **4K (2160p) / 1440p / High Refresh Rates (120Hz+)**: Not possible. The ADV7604 digitizer has a hard 170 MHz pixel clock ceiling.

---

## Completed Milestones
- [x] Locked 1080p60 capture at sustained 60.0 fps with zero queue drops (`dropped=0`, `qdepth=1`).
- [x] Multi-threaded CPU conversion across all available cores (`std::thread::hardware_concurrency()`).
- [x] Removed the main-loop busy-spin: the main thread now blocks on a condition variable in `UsbStream::update()` and is woken by the USB completion callback, instead of pegging a full CPU core (CV-17).
- [x] Removed the startup "blue flash": the standby screen is held until the receiver reports a valid, locked signal with known geometry for a short settle period, then the first frame cross-fades in (CV-18/18b).
- [x] Resolved audio stuttering and pops by eliminating destructive playback buffer clears on video frame drops.
- [x] Eliminated 720p false-classification oscillation loop and texture allocation crash by applying narrow tolerance bands and 5-frame hysteresis.
- [x] Expanded USB queue depth to 128 transfers (~256 MB buffer) and pipeline depth to 16.
- [x] Renovated HUD OSD into a rounded "glass" card (accent top-line + dot, coloured status pill, aligned `LABEL value` rows) with hotkey hint.
- [x] Split the transient mode/resolution notification out of the diagnostic card into a small rounded bottom-centre toast with its own fade.
- [x] AVX2-accelerated YUY2→RGBA conversion (`_mm256_*`, runtime dispatch, bit-exact vs scalar incl. CV-09).
- [x] Clean CMake build system with portable asset discovery and zero hardcoded paths.
