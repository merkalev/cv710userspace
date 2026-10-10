# Project Roadmap and TODO List

## Priority: Sub-1080p Resolution Support (Immediate Focus)
- [x] **Mode detection below 1080p without STDI polling**: The parser derives the
  incoming geometry from the measured frame word count (720p ~460,800 / 1080i
  ~518,400 / 576p ~207,360 / 480p ~172,800 / 576i / 480i), with 5-frame
  hysteresis, so sub-1080p display works without touching the receiver.
- [~] **ADV7604 STDI (Standard Identification) Telemetry** — **read-only, needs bench verification**:
  - The status query now reads the ADV7604 **CP map** STDI registers
    (`0x22`, `0xB1` blanking length / `0xB3` line-count-per-frame) exactly as
    the Linux `adv7604.c` driver does, and logs `STDI: N lines (bl M)` in the
    `[ADV7604]` status line. (The old devlog assumption of IO-map `0x88`–`0x8E`
    was wrong — the upstream driver reads STDI from the CP map.) Optional reads
    only: a failure degrades the log field, never the published snapshot.
  - Dynamically switching `VID_STD` (e.g. `0x13` for 720p60, `0x0A` for 480p60)
    is **Won't do (by design)** — see `ISSUES.md` §23: mid-stream `VID_STD`
    rewrites destabilised capture on the bench. Word-count detection is the
    supported path.
  - Confirm the `STDI:` line on the bench when a sub-1080p source is attached.
- [ ] **Multi-Resolution EDID Generation** — **Won't do (by design)**:
  - The captured bootstrap/EDID bytes in `UsbStream.cpp` (`0x6C` RAM) must remain exactly as captured. Advertising extra CEA-861 timings would change device behaviour and is out of scope. Sub-1080p support, if needed, is handled purely at the display/V4L2 layer from the receiver-reported geometry.
- [ ] **Frame Size Calibration for Sub-1080p**:
  - Verify exact word count outputs for 720p (`460,800` words), 576p (`207,360` words), and 480p (`172,800` words) under locked STDI timing. **Requires a sub-1080p source on the bench — cannot be completed from this machine.**

---

## Performance and Pipeline Enhancements (Feasible Implementations)
- [~] **GPU-Accelerated Zero-Copy Shaders** — **shipped as an opt-in mode**:
  - The `Direct_YUY2` colorspace mode already uploads raw YUY2 macropixels as an `SDL_PIXELFORMAT_YUY2` texture and lets SDL/the GPU do the YUV→RGB conversion (near-zero CPU). Select it via `-c yuy2` / `--colorspace yuy2`. It is not the default because the GPU path uses standard chroma reconstruction and cannot replicate the CV-09 co-sited reconstruction used by the CPU path.
- [x] **SIMD AVX2 Intrinsics**: `convertYuy2ToRgba` now has a runtime-dispatched AVX2 row converter (8 macropixels/iteration, `_mm256_*`) that is bit-for-bit identical to the scalar path (including CV-09). **NEON for ARM64 is now also implemented** (`convertRowNeon`, 4 macropixels/iteration) and both SIMD paths are protected by a one-shot runtime bit-exactness self-check against the scalar reference (falls back to scalar on any mismatch).

- [x] **Dynamic V4L2 Loopback Format Re-negotiation**: `V4LFrameOutput` now renegotiates `VIDIOC_S_FMT` when the incoming geometry changes and only writes on new frames (CV-17). A consumer holding a fixed format is detected and mismatched frames are dropped instead of corrupting the stream.
- [x] **Virtual Audio Loopback Output**: `--audio-device <name|index>` and `--audio-loopback` route captured audio to any SDL playback device, including an `snd-aloop`/PipeWire virtual sink, so it can be captured without desktop playback (CV-19). `--list-audio-devices` enumerates options. V4L2 output auto-prefers a loopback sink when one is present.
- [x] **Aspect Ratio Preservation for SD Sources**: `--aspect <stretch|auto|4:3|16:9>` controls how non-native-aspect sources are fitted into the window. `auto` pillarboxes/letterboxes 480p/576p as classic 4:3 and preserves the natural DAR for HD; `4:3`/`16:9` force a DAR; default `stretch` keeps the historical fill behaviour (CV-24).
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
- [x] Replaced the 8x8 SDL debug font with a modern antialiased TTF (vendored `stb_truetype`; auto-detects Inter / Noto Sans / Fira Sans, override via `CV710_FONT` or `assets/fonts/ui.ttf`). HUD/toast now render at native resolution and are ~60% smaller.
- [x] AVX2-accelerated YUY2→RGBA conversion (`_mm256_*`, runtime dispatch, bit-exact vs scalar incl. CV-09).
- [x] Fixed a false-positive dropped frame once per 255-frame cycle: the FPGA C0 sequence counter wraps `0xFF`→`0x01` and never emits `0x00`; the parser now treats that wrap as continuous (CV-22).
- [x] Clean CMake build system with portable asset discovery and zero hardcoded paths.
