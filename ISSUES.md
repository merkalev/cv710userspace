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

### Status
- **Intentionally out of scope.** No HDCP interrogation, no register polling for encryption status, and no HDCP OSD notice is to be implemented. Unencrypted HDMI only.

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












---

## 9. 60 Hz Detected as 50 Hz (RESOLVED)

### Symptoms Previously Observed
- The HUD reported 50 Hz for sources that were actually 60 Hz (notably 720p60, whose
  pixel clock of 74.25 MHz is identical to 720p50).

### Root Cause
- The refresh rate was inferred from HDMI map `0x05` bit 4, which is the **VSYNC polarity**
  bit, not a 50 Hz indicator. When VSYNC was positive the code forced `50.0f`.

### Resolution
- The refresh rate is now computed from the pixel clock (HDMI `0x06` + fractional bits in
  `0x3B`, adjusted for deep colour and pixel repetition) divided by the horizontal and
  vertical totals read from the porch registers (masks per `drivers/media/i2c/adv7604.c`).
  The raw value is snapped to the nearest standard rate (23.98/24/25/29.97/30/50/59.94/60/
  100/119.88/120). Because the horizontal total differs between 720p50 and 720p60, the two
  are now distinguished correctly. Interlaced sources report the conventional field rate.

---

## 10. No Input Shows 720x576 / No Splash / No Signal Message (RESOLVED)

### Symptoms Previously Observed
- With no HDMI source connected, the preview showed a 720x576 frame instead of the
  "no signal" splash, and no "[ NO SIGNAL ]" indicator appeared.

### Root Cause
- The ADV7604 free-runs a default video pattern (typically 720x576) when TMDS is not locked.
  The parser accepted those frames as real video (`_hasSignal = true`), which suppressed the
  splash; the geometry was additionally misread (see audit F-03), so the lock state was
  unreliable.

### Resolution
- `VideoSignalInfo::valid` marks the first successful status snapshot; the splash is now shown
  whenever the receiver is not TMDS-locked (`valid && !locked`) in addition to the existing
  frame-arrival watchdog. The status/lock registers were corrected (IO `0x6A & 0xE0` for lock,
  IO `0x12 & 0x10` for interlace, `0x0F` high-byte masks), and the HUD status badge now uses
  the lock state. The procedural fallback card is shown when the BMP asset cannot be loaded.

---

## 11. Interlaced Video Still Rendered Half-Height (FIXED — pending hardware check)

### Symptoms Previously Observed
- 1080i (and by extension 576i/480i) was still not woven after the first parity fix: the field
  was shown as a squashed half-height picture instead of a full frame.

### Root Cause
- `detectVideoMode` decided scan mode from the ADV7604 STDI interlace bit (IO `0x12 & 0x10`)
  alone. When that bit reads `0` for an interlaced field, the adaptive geometry path matches
  `1920 x 540` (the field) as a valid **progressive** mode, so the weave branch was never
  reached and the interlaced field was displayed directly.

### Resolution
- The FPGA's own `C0` field flag (`b2 & 0x80`, documented in `PROTOCOL.md`) is now the primary
  scan-mode source, ORed with the receiver bit. When both flags are clear, the per-field
  word-count ranges (1080i ~518,400; 576i ~103,680; 480i ~86,400 words) are used as a
  tie-breaker, since they are disjoint from every progressive mode's word count.
- The field height is derived from the observed word count (`fieldH = frameWords * 2 / width`),
  so it no longer matters whether the receiver reports the field height (540) or the full frame
  height (1080).
- Weave parity still honours the `b2 & 0x01` field index once it is seen to alternate, and falls
  back to a local toggle otherwise. The opposite parity slot is used if one field is dropped, so
  the weave can no longer stall on a black frame. A `Woven interlaced frame` diagnostic prints
  the geometry and flags periodically.

---

## 12. Audio Detuned / "Depressing" After Repeated HDMI Reconnects (FIXED)

### Symptoms Previously Observed
- After several HDMI unplug/replug cycles, audio played at the wrong pitch (slow/"depressing"),
  and was only corrected by a full re-bootstrap or relaunching the app.

### Root Cause
- The code decoded HDMI map `0x18` (`audFreqByte`) as an IEC 60958 sample-rate code. Per
  `drivers/media/i2c/adv7604.c`, bit 0 of `0x18` is only the "audio sample packet detected"
  flag; the low nibble is **not** a rate. Flaky reads latched random 32/44.1/48 kHz values, and
  `SDL_SetAudioStreamFormat` then resampled the stream to the wrong pitch. `audioLocked` also
  checked only the PLL bit, not packet detect.

### Resolution
- The sample rate now comes from the received **Audio InfoFrame** on the INFOFRAME page
  (slave `0x3e`, head `0xE3`, payload `0x1C`); the rate is payload byte 1 bits 4:2 (CEA-861 /
  Linux `hdmi_audio_infoframe_unpack`). If the infoframe is absent or unreadable, the last
  known-good rate is kept (sanitised to 48 kHz) instead of inventing one.
- `audioLocked` now requires **both** HDMI `0x04` bit 0 (PLL locked) and HDMI `0x18` bit 0
  (sample packet detected).

---

## 13. Tearing / "Freaky" Picture While Wiggling the HDMI Cable (FIXED)

### Symptoms Previously Observed
- Bumping or wiggling the HDMI connector produced tearing/combing on this driver, while the
  official driver showed no noticeable glitches.

### Root Cause
- While the HDMI clock re-locks, the FPGA emits a partial, adjusting frame. The parser padded
  and presented it, so torn/partial scanlines appeared.

### Resolution
- The `C1` trailer `b2` bit 4 is the FPGA "HDMI clock / PLL recovering" flag (`0x14` while
  recovering vs `0x04` locked, per `PROTOCOL.md`). Frames carrying that flag are now dropped
  (video buffer and weave state cleared, audio unmute counter reset) and the parser
  resynchronises on the next `C0`, so no partial frame reaches the display.

---

## 14. Slow Startup / Full Bootstrap Replay on Every Launch (CV-15 — FIXED via --fast-start)

### Symptom Previously Observed
- Every launch replays the entire captured bootstrap: 4301 OUT + 2807 IN ≈ 7100 USB
  transfers. Most of that is I2C status polling and repeated register writes, so startup
  takes seconds while the official (resident kernel) driver appears instant.

### Why the full replay existed
- Skipping bootstrap on a "warm" start left the HDMI source with a stale/limited mode
  list and, after reconnects, drifting audio, until the cable was physically replugged.
  A full replay guarantees the 256-byte EDID is re-uploaded and the receiver's hot-plug
  handshake is re-run so the source re-reads all modes.

### Resolution (intent-based fast path, opt-in)
- `tools/trim_bootstrap.py` derives `cv710_setup_commands_fast` from the capture:
  1. **Drops every I2C status-read round-trip** (`>02...` request + `<N` response).
     Reads are passive and the subsequent write values are verbatim capture bytes,
     so final state cannot change.
  2. **Collapses the driver's poll loops** (runs of identical pure-I2C-write blocks,
     e.g. 63x writing 0x00 to a row of status registers) to one copy. Same value to
     the same register repeatedly is idempotent, so final register state is identical.
  3. **Keeps all FPGA/FX3 vendor commands** (0x03/0x05/0x06/0x07/0x08/0x09/0x0B/0x0E)
     and their request/response pairs untouched and in order.
- Result: 7108 → 871 transfers (≈8.2x), same write sequence, same end state, still
  uploads the full EDID and still re-runs the HPD handshake.
- `--fast-start` (`-b`) selects the trimmed bootstrap and is the **default**;
  `--full-bootstrap` (`-B`) forces the original capture replay (~4.6 s vs ~0.6 s).

### Follow-up measurement (2026-10-10) — the real cost was clear_halt, not the command count
- Per-command instrumentation showed the 871-command loop totals **only ~508 ms**, yet
  the bootstrap wall time was **10 994 ms**. The entire gap was `libusb_clear_halt()`
  on EP 0x83: **~5.2 s per call**, called twice per bootstrap (prep + post) plus once
  at shutdown. That fixed ~10.4 s per launch was present in *both* the full and fast
  paths, which is why `--fast-start` "felt like the same speed".
- On this device a CLEAR_FEATURE(ENDPOINT_HALT) control request takes ~5 s to be
  answered, and it is pure overhead when the endpoint is not halted (the normal case).
- **Fix:** removed all three unconditional `libusb_clear_halt()` calls (bootstrap prep,
  bootstrap post, shutdown drain). Genuine halts still surface as
  `LIBUSB_TRANSFER_STALL` during streaming and are recovered by the existing CV-12
  path (`serviceStalledTransfers()`, on the main thread, bounded retries).
- Expected result: `Bootstrapping complete (871 commands, ~600 ms)`.

### State preservation (item 1)
- Shutdown clears only the FPGA stream-enable bit and resets FX3 stream DMA. It never
  touches the ADV7604 or its EDID RAM; there is no `libusb_reset_device` and
  `set_configuration` is skipped when already set, so receiver config + EDID persist
  across app restarts while the device stays powered. The fast path re-establishes the
  source-visible state in ~1/8 the transfers regardless.

### What to check on hardware
- `Bootstrapping device (fast EDID/HPD initialisation, 871 commands)...` then
  `Bootstrapping complete (871 commands, N ms)` with **N ≈ 0.5-1 s** (was ~11 s).
- Source resolution list still contains all modes (no replug needed).
- Picture, audio, 1080i weave, HDMI-wiggle behaviour identical to the full path.
- A genuinely halted endpoint (e.g. after a crash mid-stream) still recovers via the
  CV-12 stall path - the `[USB] Endpoint stalled; deferring halt recovery to the main
  loop` + `[USB] Endpoint recovered from stall; resubmitting N transfer(s)` logs.

---

## 15. Misaligned Lines for a Second After a Resolution Switch (TWO FIXES — pending re-validation)

### Symptom Sometimes Observed
- Switching resolution / refresh (e.g. 1080p -> 720p) shows a brief spell (~1 s) of
  horizontally shifted / wrapped "misaligned lines", which then clears by itself.
  Occurs occasionally, not on every switch.
- Re-validation after fix #1 showed the artifact as **video-only, ~3-4 frames,
  "skewed but straight", content duplicated mostly on the right side, lines not
  matching** - i.e. a row-stride mismatch, self-healing once the new mode locks.

### Root Cause — two independent mechanisms
1. **Marker absorption**: every `0xC0000000` / `0xC1000000` marker word is validated
   before ending/starting a frame, but the *failure* path treated the marker as pixel
   data and accumulated it into the video buffer, shifting every remaining scanline by
   2 words (4 px) and wrapping the row stride. (Fixed by marker handling below.)
2. **Wrong-width transitional frame**: while the source re-locks, the FPGA can emit a
   frame with a *valid* marker pair and an *in-range* word count for the wrong width.
   Example: a 1920-wide 480-line remnant and a 1280-wide 720-line frame both total
   460800 words, so the word-count ranges cannot tell them apart. Presenting the
   remnant with the locked 1280 stride shows the end of each 1920-wide line wrapping
   into the next line -> "duplicated right side / misaligned lines" for a few frames.
   Only the ADV7604-reported geometry can discriminate these.

### Resolution
- Fix #1 — `Device::onFrameData` no longer absorbs rejected markers:
  - A **non-genuine C0** (metadata fails `b1 == 0x01` or checksum) now discards the
    in-progress frame, clears `_inVideo`, and resynchronises on the next genuine C0.
  - A **lone C1** (trailer fails checksum / `tb1 == 0x02` / sequence, or frame size
    < 40000 words) likewise ends and discards the partial frame instead of treating
    the marker as pixels.
- Fix #2 — receiver geometry cross-check + fast status refresh:
  - A locked ADV7604 snapshot whose `activeWidth` contradicts the frame's detected
    mode width now drops the frame (logged as
    `Dropped transitional frame: N words -> WxH (...) contradicts receiver width`)
    *without* touching the mode hysteresis, so the wrong-width frame can never be
    locked or presented.
  - `UsbStream::controlLoop` now polls the status every ~100 ms instead of ~1 s, so
    the two-confirmation debounce publishes a geometry change in ~200 ms instead of
    ~2 s. Expected behaviour on a switch: the last clean frame holds for up to
    ~200 ms, then the new mode appears perfectly aligned - no skew.
- Both count as dropped frames and reset the audio-unmute counter, consistent with
  the other drop paths.

### What to check on hardware
- Switch resolution / refresh several times in a row (and from standby to signal).
  No misaligned/shifted frame should appear; expected worst case is a brief hold of
  the previous frame (~200 ms) while the receiver catches up, plus at most one logged
  transitional drop per switch. Audio must not go out of tune (unmute gate unchanged).
- If a mode ever stays black/frozen after a switch, the receiver width is being read
  wrong for that mode: the `Dropped transitional frame` log will show it repeating.

---

## 16. Startup Shows "No Signal" / a Blue Flash Before the Picture (CV-18 — FIXED)

### Symptom Sometimes Observed
- On launch the standby image appears, then there is a brief **blue flash**, then the
  real picture. Sometimes the standby screen wrongly says "NO SIGNAL" even when a
  source is already connected.

### Root Cause
1. The standby screen dismissed as soon as the *first* frame arrived, even before the
   ADV7604 status had been read. During HDMI negotiation the FPGA/receiver emits
   free-run / transitional frames (often a blue raster), which were therefore shown
   for a frame or two - the "blue flash".
2. The procedural fallback card used a saturated-blue palette, and the standby text
   said "[ NO SIGNAL ]" during the bootstrap window when the state was simply unknown.

### Resolution
- The standby screen is now held until the receiver reports a **valid, locked** signal
  with known geometry (`valid && locked && activeWidth > 0`) for a short settle period
  (~200 ms). Transitional/free-run frames are masked behind it.
- A 3 s safety valve shows the preview anyway if the status channel never validates, so
  the screen can never be permanently stuck. It deliberately **does not** fire when a
  valid status says "unlocked": the CV710 keeps emitting free-run frames after the source
  is switched off, and an earlier version of this valve kept showing that frozen/free-run
  picture instead of returning to the standby image.
- The first live frame is cross-faded in (CV-18b) instead of popping.
- The standby screen is just the AVerMedia bitmap; no textual status states are
  overlaid (the BMP is enough). The blue procedural fallback was removed entirely.
- `Device::run()` now publishes the cached signal snapshot every 200 ms instead of
  1000 ms so the gate and HUD react quickly (the snapshot copy is cheap; the actual I2C
  polling still runs on the control thread).

---

## 17. High CPU / Main-Loop Busy-Spin (CV-17 — FIXED)

### Symptom
- The process pegged a full CPU core even when idle, and V4L2 loopback output was
  flooded with frames.

### Root Cause
- The main loop (`while (!do_exit) { SDL_PollEvent; device.run(); }`) had no pacing.
  `UsbStream::update()` returned immediately when the queue was empty, so the loop spun
  as fast as the CPU allowed. `V4LFrameOutput::display()` wrote the full 4 MB frame on
  **every** iteration (no new-frame guard), so the loopback device received thousands of
  redundant writes per second.

### Resolution
- `UsbStream` now exposes a condition variable: the main thread blocks in `update()`
  (5 ms timeout) and is woken by the USB completion callback when a transfer is queued.
- `V4LFrameOutput` only writes when a new frame has actually arrived.
- Net effect: idle CPU drops to near zero and the loopback stream carries one frame per
  captured frame rather than a CPU-speed flood.

---

## 18. V4L2 Output Ignored Resolution and Audio Went to the Desktop (CV-19 — FIXED)

### V4L2 dynamic format
- `V4LFrameOutput` assumed a fixed 1920x1080 / 4 MB payload. It now renegotiates
  `VIDIOC_S_FMT` whenever the source geometry changes, writes the correct byte count,
  and drops (rather than corrupts) a frame when a consumer holds a fixed format.

### Virtual audio output
- V4L2 carries video only, so audio was played through the desktop default. Added
  `--audio-device <name|index>` and `--audio-loopback` (plus `--list-audio-devices`) to
  route the captured 48 kHz stereo stream to any SDL playback device, including an
  `snd-aloop` / PipeWire virtual sink, so third-party software can capture it.
- When `-d` (V4L2) is used, a loopback sink is auto-preferred if one is present, falling
  back to the desktop default otherwise.

---

## 19. OSD/HUD Renovation (CV-20)

### Previous look
- A single flat rectangle with the SDL 8x8 debug font, a bracketed `[ TMDS LOCK ]`
  badge, colon-prefixed rows (`Video:`, `Color:` …), and a dense hotkey line.
- Every transient event (resolution detect, auto/manual colorspace change, HUD toggle)
  popped up the *entire* diagnostic card for 2.5 s, which read as visual noise.

### Now
- The persistent diagnostic HUD (Tab/O) is a rounded "glass" card drawn at native
  resolution (no 2x debug-font scaling): accent glow line + dot, a filled coloured
  status pill (`LOCKED` / `NO SIGNAL` / `LIVE`), a divider, aligned dim `LABEL` +
  bright value rows, and a compact hotkey hint.
- Transient events now show a small rounded **toast** pill at the bottom-centre
  (accent dot + message) with its own fade, instead of the full card.
- The 8x8 SDL debug font was replaced by a modern antialiased TTF via the vendored
  public-domain `stb_truetype` (`SdlTextRenderer`): ASCII glyphs are packed into one
  2x-oversampled atlas texture and blitted with a tint + alpha. This is what makes
  the OSD read as modern text instead of blocky debug pixels, and lets the card be
  ~60% smaller in area.
- Rounded rectangles are still drawn from horizontal spans (`fillRoundRect()`), so
  they composite correctly with the renderer's alpha blending.
- Font selection: `CV710_FONT` env override, then `assets/fonts/ui.ttf`, then a
  system search (Inter, Noto Sans Medium/Regular, Fira Sans, Liberation, DejaVu).
  If none loads, the OSD gracefully falls back to `SDL_RenderDebugText`.

---

## 21. False-Positive Dropped Frame Every ~4 Seconds (CV-22 — FIXED)

### Symptom
- With the persistent HUD on, the `CAPTURE` row's `drop` counter crept up by one
  every ~4.25 s (e.g. "100 frames dropped") on a perfectly healthy 1080p60 stream,
  even though USB queue drops stayed at 0.

### Root Cause
- The parser expected the C0 header sequence counter to advance as
  `(last + 1) & 0xFF`, i.e. `0xFF` → `0x00`. Logging every genuine C0 on hardware
  showed the FPGA counter actually runs **1..0xFF and wraps `0xFF` → `0x01`; it never
  emits `0x00`.** So once per 255-frame cycle the wrap was misclassified as a
  transport gap and one bogus dropped frame was counted.
- The capture/parse code was byte-identical to the previous "flawless" build; this
  was a long-standing counter bug newly *visible* only because the HUD is now
  persistent.

### Resolution
- Added `nextFrameSeq()`: `0xFF` → `0x01`, otherwise `seq + 1`. A genuine loss that
  straddles the wrap (e.g. `0xFF` → `0x02`) is still detected as a gap.
- Zero the visible drop counters at stream lock so the HUD reports drops *since
  lock* rather than one-time startup resync noise.
- Documented the counter range/wrap in `PROTOCOL.md`.

### Verification
- Hardware: `drop` stays at 0 across 13+ wrap cycles (was +1 every cycle); a 60 s
  headless run showed `drops: 0` throughout.

---

## 20. YUY2→RGBA CPU Conversion Cost (CV-21)

### Change
- Added a runtime-dispatched AVX2 row converter (`convertRowAvx2`, `_mm256_*`,
  8 macropixels per iteration) used for full-scale (`step == 1`) non-swapped chroma.
- It is **bit-for-bit identical** to the scalar path, including the CV-09 co-sited
  chroma reconstruction (the odd luma pixel uses the average of the current and the
  next macropixel's chroma).
- Guarded by `__builtin_cpu_supports("avx2")`; all other cases (downscale, UYVY swap,
  non-x86) fall back to the existing scalar loop. The existing multi-threaded row
  split is retained on top.

### Verification
- A standalone harness compared AVX2 against the exact production scalar for all four
  BT.709/601 limited/full matrices, across row widths both divisible by 8 (vector-only)
  and not (exercising the scalar tail): **bit-exact**. Single-row throughput improved
  ~1.6x in isolation.




---

## 22. Skewed / Misaligned Lines — Sometimes Persistent Until a Replug (CV-23 — FIXED)

### Symptom
- Occasionally the picture develops "skewed / misaligned lines" (rows shifted by
  a few pixels, content duplicated at the right edge). Usually brief around a
  resolution switch, but **sometimes it sticks and only a physical replug of the
  capture device clears it**.

### Root Cause
1. **Wrong-size frames were padded and presented.** A single word lost or
   duplicated mid-frame (USB hiccup, corrupt read) shifts every following
   scanline by 1 word (2 px). The frame total stays within the ±2 % mode band,
   the markers stay valid, and the old code padded/truncated it and presented it
   — the row-stride "skewed lines" seam. It self-heals only when the *next*
   frame happens to be clean, so while the transport keeps hiccuping the picture
   keeps looking skewed.
2. **Nothing ever re-locked a stuck pipeline.** `_streamLocked` was set once and
   never cleared, so once the FPGA/FX3 FIFO alignment or the parser state got
   stuck (the only-replug-clears case), no code path could recover.

### Resolution (CV-23)
- **Exact-size gate at presentation.** A frame/field whose word count differs
  from the locked geometry is now dropped (counted, rate-limited log) instead of
  padded/truncated and presented. A skipped frame is invisible next to a skewed
  one, and the next genuine C0 re-anchors the raster. Applies to progressive
  frames (`frameWords == mode.targetWords`) and interlaced fields
  (`frameWords == strideWords * fieldLines`).
- **Automatic software replug.** After 60 consecutive un-presentable frames
  while the ADV7604 reports a *locked* geometry (≈1 s), the parser runs
  `reassertStream()`: it resets the FX3 stream DMA (clears the FPGA FIFO
  pointers), re-asserts the FPGA streaming bit (the exact sequence used when
  streaming starts), and re-enters the C0 lock hunt — a replug without touching
  the cable.
- **Manual re-sync key.** `R` runs the same software replug on demand.
- `Device::reassertStream()` also resets weave/parity audio mute state so the
  next lock starts clean; the ADV7604 register config and EDID are deliberately
  left untouched (CV-15 state-preserving design).

### Verification
- New `device_parser_test` (Catch2) feeds synthetic 1080p frames through
  `Device::onFrameData`:
  - exact-size frame presented, off-by-one frame dropped (never presented),
    next clean frame presented again;
  - 60 consecutive corrupt frames with a locked receiver fire the software
    replug exactly once, the streak resets, and a clean frame presents
    afterwards. 83 assertions across 2 cases, all passing.
