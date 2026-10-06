# Project Roadmap and TODO List

## Priority: Sub-1080p Resolution Support (Immediate Focus)
- [ ] **ADV7604 STDI (Standard Identification) Register Polling**:
  - Implement I2C queries to ADV7604 IO map (`0x20`) registers `0x88` - `0x8E` to read incoming line count and frame rate.
  - Dynamically switch `VID_STD` (e.g., `0x13` for 720p60, `0x0A` for 480p60) when input format changes from 1080p.
- [ ] **Multi-Resolution EDID Generation**:
  - Expand the 256-byte EDID table in `UsbStream.cpp` (`0x6C` RAM) to advertise standard CEA-861 timings for 720p, 576p, and 480p so source devices do not force 1080p output.
- [ ] **Frame Size Calibration for Sub-1080p**:
  - Verify exact word count outputs for 720p (`460,800` words), 576p (`207,360` words), and 480p (`172,800` words) under locked STDI timing.

---

## Performance and Pipeline Enhancements (Feasible Implementations)
- [ ] **GPU-Accelerated Zero-Copy Shaders**:
  - Upload raw YUY2 macropixels directly as an `RG88` or YUV texture to the GPU.
  - Perform Rec.709/Rec.601 color matrix conversion inside an SDL3 / OpenGL / Vulkan fragment shader, dropping CPU usage to near 0%.
- [ ] **SIMD AVX2 and NEON Intrinsics**:
  - Implement vectorized YUY2 to RGBA conversion using `_mm256_*` (x86_64) and NEON (ARM64) for headless V4L2 streaming.
- [ ] **Dynamic V4L2 Loopback Format Re-negotiation**:
  - Tear down and renegotiate `VIDIOC_S_FMT` dynamically on the fly when the physical HDMI resolution changes, allowing OBS and Discord to automatically follow source resolution changes.
- [ ] **Virtual Audio Loopback Output**:
  - Expose captured 48 kHz stereo audio to an ALSA loopback device (`snd-aloop`) or PipeWire virtual source, allowing capture in third-party software without requiring SDL desktop audio playback.
- [ ] **Aspect Ratio Preservation for SD Sources**:
  - Add configurable aspect ratio handling (`16:9` vs `4:3` pillarboxing) for 480p and 576p video.
- [ ] **HDCP Detection and Status Notification**:
  - Query ADV7604 HDMI map `0x68` register `0x05` bit 7 to detect encrypted input and display an OSD warning instead of a silent black screen.

---

## Hardware and Architectural Limitations (Not Implementable)
- [x] **Component (YPbPr) Analog Video**: Officially classified as unsupported. Requires proprietary analog breakout cable, ADV7604 analog front-end ADC calibration, sync slicer clock recovery, and TLV320AIC3101 analog audio routing.
- [x] **Hardware Video Compression (H.264 / HEVC)**: Not possible. The CV710 hardware contains no onboard hardware encoder ASIC.
- [x] **4K (2160p) / 1440p / High Refresh Rates (120Hz+)**: Not possible. The ADV7604 digitizer has a hard 170 MHz pixel clock ceiling.

---

## Completed Milestones
- [x] Locked 1080p60 capture at sustained 60.0 fps with zero queue drops (`dropped=0`, `qdepth=1`).
- [x] Multi-threaded CPU conversion across all available cores (`std::thread::hardware_concurrency()`).
- [x] Resolved audio stuttering and pops by eliminating destructive playback buffer clears on video frame drops.
- [x] Eliminated 720p false-classification oscillation loop and texture allocation crash by applying narrow tolerance bands and 5-frame hysteresis.
- [x] Expanded USB queue depth to 128 transfers (~256 MB buffer) and pipeline depth to 16.
- [x] Renovated HUD OSD with translucent card, native 1:1 fonts, color-coded presets, and smooth alpha fade.
- [x] Clean CMake build system with portable asset discovery and zero hardcoded paths.
