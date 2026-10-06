# Known Issues and Protocol Roadmap: AVerMedia ExtremeCap U3 (CV710)

This document tracks known limitations, hardware quirks, and reverse-engineering findings for the AVerMedia ExtremeCap U3 (CV710 / C877, USB ID 07ca:0710) userspace capture tool.

Credit: This work builds upon the pioneering research and userspace driver foundation created by ChrisAJS (https://github.com/ChrisAJS/lgx2userspace).

---

## 1. Resolution and Refresh Rate Limitations (< 1080p)

### Symptoms
- Feeds from consoles or PCs set to 720p, 576p, or 480p produce a black screen, garbled lines, or frame drops.
- Changing resolution on the fly does not automatically adapt the output window.

### Root Cause
1. Hardcoded ADV7604 and FPGA Bootstrap:
   - The startup command sequence in `commanddata_cv710.cpp` sends hardcoded register writes (via USB EP 0x01 vendor control commands) captured from a 1080p60 HDMI initialization.
   - The Analog Devices ADV7604 HDMI receiver and Cypress FX3 / Lattice ECP3 FPGA are statically configured for 1080p60 pixel clock, EDID timing, and PLL multipliers.
2. Fixed Frame Sizing:
   - Active video accumulation currently expects 1080p active frame size: 1,036,800 uint32 words (1920 x 1080 / 2).
   - Standard word counts for other modes:
     - 720p60: 1280 x 720 / 2 = 460,800 uint32 words.
     - 480p60: 720 x 480 / 2 = 172,800 uint32 words.
     - 576p50: 720 x 576 / 2 = 207,360 uint32 words.

### Solution and Next Steps
- Implement dynamic mode detection via ADV7604 register query (`CRXADV7604::ADIAPI_MwRxGetVideoFormat`).
- Extract or dump per-mode initialization tables (`commanddata_cv710_720p60.cpp`, `commanddata_cv710_480p60.cpp`) from driver USB traces.
- Update frame size validation to dynamically follow the detected mode.
- Note: Mode guard is explicitly disabled per design requirements.

---

## 2. Interlaced Video (1080i, 576i, 480i)

### Symptoms
- 1080i or 480i input renders as half-height scrambled frames, combed edges, or fails to lock.

### Root Cause
- The ADV7604 sends interlaced video in alternating half-height fields (Top Field and Bottom Field).
- In the 0xC0FFFF00 metadata word (`b0, b1, b2, b3`), byte `b2` indicates field structure:
  - `b2 & 0x80`: Dual field / Interlaced indicator.
  - `b2 & 0x01`: Field index (Field 0 vs Field 1).
- In `libC877Driver.dylib`, interlaced frames are handled by `VideoParseBuffer::copyDualFieldData`:
  ```cpp
  // Dual-field copy weaves top and bottom fields into alternating scanlines
  dest[y * 2 * stride + x]     = field0[y * stride + x];
  dest[(y * 2 + 1) * stride + x] = field1[y * stride + x];
  ```
- The current userspace parser assumes progressive video and appends all incoming chunks linearly.

### Solution and Next Steps
- Inspect `b2` in C0 metadata for the interlace flag.
- Route dual-field video through a field-weaving buffer (`copyDualFieldData`) or expose a Bob/Weave deinterlace filter before SDL/V4L2 presentation.

---

## 3. Colorspace and Range Incompatibilities (RESOLVED)

### Symptoms Previously Observed
- Connecting a MacBook, iPad, or PC graphics card produced neon pink/green or amber tinting with horizontal chromatic artifacts.
- Nintendo Switch (which defaults to limited-range YCbCr 4:2:2) displayed correct colors.

### Root Cause
- Apple computers and desktop GPUs default to RGB Full-Range (0-255) over HDMI.
- Passing raw bytes directly into an SDL texture configured as YUY2 caused RGB channel bytes to be misinterpreted as U/V chroma and Y luma.

### Resolution
- Implemented high-performance fixed-point integer colorspace conversion in `SdlVideoOutput`:
  - Mode 0: BT.709 Limited (HDTV Standard 1080p, Rec.709 Studio levels [16-235]) - Default
  - Mode 1: BT.709 Full (PC / Mac HDMI Full Range [0-255])
  - Mode 2: BT.601 Limited (SDTV Standard)
  - Mode 3: BT.601 Full (PC SD Full Range)
  - Mode 4: UYVY Swap (Fixes inverted chroma on devices outputting UYVY)
  - Mode 5: Direct YUY2 (Raw hardware GPU shader pass-through)
- Runtime interactive switching via 'C' key with on-screen display (OSD) notification.
- Command-line option `-c COLORSPACE` (`bt709`, `bt709full`, `bt601`, `bt601full`, `uyvy`, `yuy2`).

---

## 4. Audio Buzzing During Mode Switches and Clock Changes (RESOLVED)

### Symptoms Previously Observed
- When unplugging/replugging HDMI, switching resolution, or changing refresh rate (e.g. 60Hz to 50Hz), a loud buzzing / glitching noise was heard through speakers.

### Root Cause
- HDMI audio clock regeneration (ACR) depends on stable video clock timing (N and CTS packets).
- When HDMI lock is lost during a mode transition:
  - The ADV7604 audio PLL unlocks temporarily.
  - The Cypress FX3 FIFO streams floating bus noise or stale buffer loops into the 0x58FFFF00 audio packets.
  - The sample count fluctuates (60 fps: 800 samples = 3200 bytes; 50 fps: 960 samples = 3840 bytes; 59.94 fps: alternating 800/801 samples).
- Pushing these noise packets into SDL audio output without a video sync gate caused continuous audio buzzing.

### Resolution
- Added a hardware watchdog and video sync lock gate in `lgxdevice.cpp`:
  - Audio is muted whenever video lock is lost or when no valid frame has arrived for > 250 ms.
  - On sequence gaps (`b0 != ((lastSeq + 1) & 0xFF)`), audio buffers and queues are immediately flushed.
  - Audio is only unmuted after at least 2 consecutive valid frames arrive cleanly.
  - Audio packet byte lengths are validated dynamically (512 to 8192 bytes) and passed with exact byte count to `SDL_PutAudioStreamData`.
  - Added `clearAudio()` to flush pending SDL audio buffers upon signal loss.

---

## 5. Signal Loss and Preview Freeze (RESOLVED)

### Symptoms
- When HDMI input was disconnected or signal was lost, the preview window froze on the last received frame.

### Resolution
- Added a 500 ms signal loss watchdog in `SdlVideoOutput` that clears stale frame data and displays a clean standby screen on a black canvas.

---

## 6. HDCP Behavior

### Observation
- HDMI sources that typically refuse to capture due to HDCP handshakes (streaming boxes, consoles with HDCP enabled) stream freely on this userspace driver.

### Explanation
- The userspace bootstrap sequence for the ADV7604 HDMI receiver does not initiate downstream HDCP authentication or repeater negotiation over USB EP 0x01.
- The ADV7604 hardware digitizes the incoming HDMI stream and sends raw frames to the FX3 bulk endpoint.
- This behavior is intended and verified functional.

---

## 7. Address Boundary Crashes (SIGSEGV) at No Signal and Input Switches (RESOLVED)

### Symptoms Previously Observed
- Switching between RGB, YCbCr 4:2:2, or changing resolutions from the connected input machine caused an immediate crash with "SIGSEGV: Address boundary error".
- Running without an active HDMI input or disconnecting the HDMI cable ("No Signal") produced random address boundary crashes.

### Root Cause Analysis
1. Frame Word Accumulator Underflow in `lgxdevice.cpp`:
   - When the HDMI signal was disconnected or format negotiation occurred, the ADV7604 receiver lost TMDS lock and emitted bus noise or partial frames without genuine C1 trailers.
   - Slices continued accumulating words into `_frameBuilder` until reaching the 1080p limit (`1,036,800` words).
   - Stray markers or unvalidated non-header words added single words, pushing `_frameBuilder.videoFrameSize()` past `1,036,800` (e.g. `1,036,801`).
   - On the subsequent transfer slice, the calculation `slice = CV710_1080P_FRAME_WORDS - curWords` underflowed unsigned 32-bit arithmetic to `4,294,967,295` (`0xFFFFFFFF`).
   - `buildVideo()` was called with `slice = 0xFFFFFFFF`, causing `memcpy` to attempt reading 4 MB from the 2 MB USB transfer buffer. This triggered an immediate `SEGV_MAPERR` segfault inside libc `memcpy`.
2. Unsafe USB Transfer Lifecycle and Vector Mutation in `UsbStream.cpp`:
   - In `usbTransferComplete()`, non-completed transfer statuses (`LIBUSB_TRANSFER_TIMED_OUT`, `LIBUSB_TRANSFER_STALL`, `LIBUSB_TRANSFER_OVERFLOW`) were treated as fatal errors, triggering `signalError()` which threw `std::runtime_error` and initiated shutdown.
   - During shutdown, `submitTransfer()` called `_transfers.erase(std::find(...))` while `readLoop()` was concurrently iterating over `_transfers`, leading to iterator invalidation and use-after-free segfaults.
   - If a transfer was not found in `_transfers`, calling `_transfers.erase(end())` produced an immediate crash.
3. Unchecked Keyboard Pointer in `SdlVideoOutput.cpp`:
   - `SDL_GetKeyboardState(nullptr)` was dereferenced without checking for a null pointer, causing a segmentation fault if the keyboard subsystem was not yet ready.

### Resolution
- In `lgxdevice.cpp`:
  - Added strict upper-bound checks: video accumulation is completely halted once `_frameBuilder.videoFrameSize() >= CV710_1080P_FRAME_WORDS`.
  - Guarded the slice calculation so `CV710_1080P_FRAME_WORDS - curWords` can never underflow.
  - Guarded lone C0, C1, and audio marker accumulation with word-capacity limits.
- In `FrameBuilder.cpp`:
  - Added defensive guards to `buildVideo()` and `buildAudio()` to reject null blocks, zero lengths, or writes exceeding buffer capacity.
- In `UsbStream.cpp`:
  - Replaced vector mutation during completion callbacks with an atomic in-flight transfer counter (`_activeTransfers`).
  - Non-fatal transfer statuses (timeout, stall, overflow) are logged as warnings and safely resubmitted; endpoint stalls are recovered with `libusb_clear_halt`.
  - Only `LIBUSB_TRANSFER_NO_DEVICE` triggers a fatal disconnection signal.
  - Shutdown cancels all transfers, drains in-flight completions cleanly, and frees transfer descriptors only after the USB read thread has joined.
- In `SdlVideoOutput.cpp`:
  - Added null check `if (keyboardState != nullptr)` before indexing scancodes.
- In `main.cpp`:
  - Added `SDL_EVENT_WINDOW_CLOSE_REQUESTED` handling to ensure clean, graceful exits.

---

## 8. Chromatic Shift and Rightward Color Bleed (RESOLVED)

### Symptoms Previously Observed
- High-contrast edges and text exhibited horizontal color bleeding, resembling reduced chroma bandwidth.
- Red color appeared shifted to the right by some pixels.

### Root Cause
- In 4:2:2 YUY2 formats, two adjacent horizontal pixels share one chroma pair (Cb/Cr).
- In standard ITU-R BT.709 and BT.601 specifications, chroma is co-sited with the even luma pixel ($Y_0$, $x = 2k$).
- The odd pixel ($Y_1$, $x = 2k + 1$) sits geometrically halfway between chroma sample $k$ and chroma sample $k + 1$.
- Previously, the conversion applied the exact same chroma values to both the even and odd pixels without horizontal interpolation (box reconstruction). This caused the odd pixel to inherit the left pixel's chroma, extending color edges by one pixel to the right and producing visible red fringing.

### Resolution
- Implemented high-quality co-sited linear horizontal chroma reconstruction in `SdlVideoOutput::convertYuy2ToRgba`:
  - Even pixel ($2k$): uses co-sited chroma $(U_k, V_k)$.
  - Odd pixel ($2k + 1$): linearly interpolates chroma halfway between $(U_k, V_k)$ and $(U_{k+1}, V_{k+1})$:
    $U_{odd} = (U_k + U_{next} + 1) / 2$, $V_{odd} = (V_k + V_{next} + 1) / 2$.
- Eliminates the 1-pixel rightward chroma offset and provides clean, razor-sharp color transitions across all software conversion modes.
