# cv710userspace: Source Code Audit and Issue Consolidation

**Review date:** 2026-10-09  
**Project:** [merkalev/cv710userspace](https://github.com/merkalev/cv710userspace)  
**Upstream foundation:** [ChrisAJS/lgx2userspace](https://github.com/ChrisAJS/lgx2userspace)  
**Review type:** Static, source-level review of publicly accessible `main` branch files, plus an assessment of the supplied eight-item review.  
**Validation limit:** No CV710 hardware was available. The repository could not be cloned or built in the review environment. Findings are based on inspected source paths and code logic, not runtime reproduction. GitHub `main` links can change after this review.

## Executive Summary

The driver is substantial and contains useful defenses against malformed video frames, USB disconnects, and signal loss. The most significant remaining risks are **stream-boundary parsing, latency under backpressure, synchronous hardware status polling, field ordering for interlaced input, and partial failure handling**. The pre-existing eight-item review also contains several incorrect or overstated conclusions. In particular, a C-style pointer cast is not by itself proof of misalignment, and two frame-size constants described as different are currently identical.

Classification used here:

- **Confirmed from code:** A specific problematic path or inconsistency is visible in the inspected source. Hardware symptoms may still need reproduction.
- **Conditional risk:** A plausible failure mode depends on concurrency, data shapes, or external conditions that were not reproduced.
- **Documentation/cleanup:** A maintainability or documentation problem, not a demonstrated crash.

Severity is an engineering triage assessment, not a CVSS score.

## Prioritized Findings

| ID | Priority | Finding | Evidence category |
| --- | --- | --- | --- |
| CV-01 | High | Split C0/C1/audio headers are discarded at transfer boundaries | Confirmed from code |
| CV-02 | High | Large FIFO plus drain-until-empty can add substantial latency and starve display | Confirmed design risk |
| CV-03 | High | Synchronous I2C status reads run in the video/display thread | Confirmed from code |
| CV-04 | High | Interlaced field placement ignores supplied field-index flag | Confirmed from code |
| CV-05 | High | Non-timeout bootstrap probe errors can skip initialization | Confirmed from code |
| CV-06 | Medium | Setup exceptions can leave a USB device handle open | Confirmed from code |
| CV-07 | Medium | Shutdown may race a one-time transfer-vector expansion | Conditional concurrency risk |
| CV-08 | Medium | Transfer submission failure leaves a dead pipeline slot without recovery | Confirmed failure-handling gap |
| CV-09 | Medium | Claimed co-sited chroma interpolation is not implemented in conversion loop | Confirmed discrepancy |
| CV-10 | Medium | Word-count-only mode inference can accept truncated or oversized frames | Conditional visual correctness risk |
| CV-11 | Medium | USB descriptor version is mistaken for negotiated link speed | Confirmed logic error |
| CV-12 | Medium | USB completion handling performs synchronous stall recovery in event callback | Conditional responsiveness risk |
| CV-13 | Low | I2C read failures are ignored by signal-status reporting | Confirmed from code |
| CV-14 | Low | Static counters and duplicated checks cause scope/maintenance issues | Cleanup |
| CV-15 | Low | Raw pointer casts and C++17 preprocessor extension reduce portability | Conditional portability risk |
| CV-16 | Low | Signal handler writes a regular `bool` rather than `sig_atomic_t` | Portability/correctness |

---

## Detailed Findings

### CV-01: Incomplete markers are not preserved between USB transfers

**Priority:** High  
**Confidence:** High, confirmed by code path  
**Source:** [`lgxdevice.cpp`, lines 108-175 and 186-365](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L108-L175)

**Observation:** `Device::onFrameData()` processes one transfer buffer per invocation. If a C0 marker occurs at the final word, it executes `break` because the metadata word is unavailable. C1 requires two additional trailer words, and an audio-start marker requires one length word. Each branch has an analogous `break`. No pending-word state is retained for the following `onFrameData()` call. `_inAudio` handles split **audio payload**, but not a split **audio header**.

**Impact:** An otherwise valid header or trailer crossing a transfer boundary is lost. The next transfer's first word is parsed independently, potentially dropping a frame, corrupting its frame size, or disrupting audio/video synchronization.

**Fix:** Introduce a small carry buffer (up to two trailing `uint32_t` words, plus any byte-level remainder if the transport does not guarantee 4-byte alignment). Prepend pending words to the next transfer in a stateful parser. Add tests with every possible split position for C0, C1, and audio headers.

**Acceptance test:** Replaying the same captured byte stream with arbitrary USB chunk boundaries must produce identical frames, audio packets, and sequence counts.

### CV-02: USB queue can retain a large backlog of stale video

**Priority:** High for low-latency use  
**Confidence:** High for potential backlog, unmeasured for real-world occurrence  
**Sources:** [`UsbStream.h`, line 41](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.h#L40-L42), [`UsbStream.cpp`, lines 301-349](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L301-L349)

**Observation:** A FIFO accepts up to 128 completed USB transfers, each at most `0x1FC000` or 2,080,768 bytes. `update()` repeatedly pops and processes items until the queue is empty. At full transfer sizes this allows approximately **254 MiB of queued data**. At the nominal 1080p60 YUY2 payload rate, that corresponds to roughly **1.07 seconds** of video data, ignoring audio and protocol overhead.

**Impact:** In normal steady state a shallow queue can work well. Under sustained load, however, the application can process stale transfers in FIFO order. Because `update()` has no fixed per-loop work budget, SDL presentation and event handling may also be delayed while draining a continuously refilling queue.

**Fix:** Instrument oldest-transfer age, queue high-water mark, and capture-to-present latency. Use a small bounded real-time queue and an explicit overload policy. **Do not blindly drop individual transfers:** the stream contains variable-length frame/audio framing. Discard only at validated C0 boundaries with parser resynchronization and audio-state reset, or prioritize throughput using a distinct parser thread and a latest-complete-frame presentation queue.

**Acceptance test:** Under injected CPU load, the application remains responsive, bounded in memory, and returns quickly to live video after overload without delivering corrupted frames.

### CV-03: Hardware status polling can block the frame-processing thread

**Priority:** High for low-latency use  
**Confidence:** High  
**Sources:** [`lgxdevice.cpp`, lines 35-61](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L35-L61), [`UsbStream.cpp`, lines 213-280](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L213-L280)

**Observation:** `Device::run()` calls `queryVideoSignalStatus()` about every 1.5 seconds before calling `_stream->update()` and SDL `display()`. The query makes four synchronous `libusb_bulk_transfer()` I2C-read request/response pairs, with individual timeouts of 500 ms. An automatic `setVideoStandard()` may perform an additional synchronous 1,000 ms USB write.

**Impact:** A stalled control endpoint can delay video processing and presentation by seconds, causing queue growth and visible freezes. The normal successful path may be much quicker, but the worst-case structure is undesirable for a low-latency capture tool.

**Fix:** Perform status polling on a separate control worker or use a nonblocking state machine. Cache results for the render thread. Avoid racing control and streaming operations during shutdown, and measure transaction latency.

**Acceptance test:** Delayed or failed status reads must not stall the preview or event loop.

### CV-04: Interlaced field parity is inferred from a global toggle

**Priority:** High when using 1080i, 576i, or 480i  
**Confidence:** High  
**Source:** [`lgxdevice.cpp`, lines 186-197 and 274-290](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L274-L290)

**Observation:** C0 metadata byte `b2` is saved to `_currentFieldFlags`. The interlaced copy instead uses `static uint32_t interlacedFieldCount` and `interlacedFieldCount++ % 2`, ignoring the available field-index bit. It also emits a full-sized video buffer after each individual field.

**Impact:** If a field is dropped or capture restarts mid-cycle, top and bottom fields can swap. Presentation after a single field combines newly written lines with previous or initialized lines. This can produce temporal tearing or inconsistent fields. Regular interlacing combing is not itself a defect in weaving; incorrect parity and premature presentation are the issues here.

**Fix:** Use `(_currentFieldFlags & 0x01)` for field placement after verifying the device's actual top/bottom mapping. Track field pair completeness by frame sequence/field identity, reset on discontinuity, and emit complete woven frames or explicitly use bob deinterlacing.

**Acceptance test:** Start capture on either field, drop one field deliberately, and verify correct reconstruction after resynchronization.

### CV-05: Any probe result other than timeout is interpreted as an already streaming device

**Priority:** High  
**Confidence:** High  
**Source:** [`UsbStream.cpp`, lines 119-152](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L119-L152)

**Observation:** After one synchronous bulk read, the bootstrap proceeds only when `res == LIBUSB_ERROR_TIMEOUT`; all other statuses, including negative error codes, take the path labeled `Device already streaming` and skip the configuration command sequence.

**Impact:** Disconnect, pipe, overflow, I/O, or other unexpected errors can masquerade as successful initialization. The subsequent reader may hang, produce garbage, or fail later with less useful diagnostics.

**Fix:** Explicitly distinguish `LIBUSB_SUCCESS` with appropriate data from expected timeout and all other errors. Reject or retry unexpected statuses. If the already-streaming fast path is supported, validate the stream with genuine C0/C1 framing before treating it as initialized.

### CV-06: Exceptions during setup leak the opened USB handle

**Priority:** Medium  
**Confidence:** High  
**Sources:** [`UsbStream.cpp`, lines 107-141](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L107-L141), [`UsbStream.cpp`, lines 357-389](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L357-L389)

**Observation:** `_dev` is opened before configuration/interface claims. Multiple later branches throw exceptions. The destructor only invokes `shutdownStream()` when `_readThread` is joinable and `_shuttingDown` is false. A setup failure before thread creation therefore has no visible path that closes `_dev`.

**Impact:** Device handles and potentially claimed interfaces may remain open when an initialization error is caught and the process continues. The process exiting will eventually release OS resources, but relying on that is not safe resource ownership.

**Fix:** Wrap the device handle in an RAII deleter, and separately track interface-claim state. Make cleanup safe at any intermediate initialization stage, including `libusb_init()` failure.

### CV-07: Shutdown can race the one-time probe-to-pipeline transition

**Priority:** Medium  
**Confidence:** Conditional, concurrency interleaving not reproduced  
**Source:** [`UsbStream.cpp`, lines 10-42, 289-300, 357-370 and 415-425](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L415-L425)

**Observation:** `queueAllFrameReads()` can append seven descriptors to `_transfers` from a libusb completion callback. `shutdownStream()` can iterate, cancel, and later free `_transfers` from the main thread without synchronizing this vector's mutation. `_shuttingDown` checks and the active-transfer counter do not by themselves protect vector iteration from an in-progress expansion.

**Impact:** An unlucky close during initial probe completion can cause a C++ data race, missed cancellation, or invalidated vector iterators. This is a different interleaving from the earlier resolved erase-during-callback bug.

**Fix:** Serialize transfer-vector construction and shutdown with a mutex or a single event-thread lifecycle state machine. Prefer allocating all descriptors before starting the event thread. Add stress tests that close immediately after starting capture.

### CV-08: Failed asynchronous resubmission is not escalated

**Priority:** Medium  
**Confidence:** High  
**Source:** [`UsbStream.cpp`, lines 391-400](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L391-L400)

**Observation:** `submitTransfer()` prints a failure when `libusb_submit_transfer()` is unsuccessful but does not retry, signal a fatal error, or restore a pipeline slot. This is also used from completion callbacks.

**Impact:** Repeated transient submission failures can progressively reduce the active pipeline to zero without cleanly notifying the application. The preview may show a no-signal state even though the USB device remains connected.

**Fix:** Classify error codes into retryable and terminal cases. Add bounded backoff, a pipeline-depth health check, and a single well-defined fatal error path if no transfers remain active.

### CV-09: Documented horizontal chroma interpolation does not match source

**Priority:** Medium  
**Confidence:** High  
**Sources:** [`SdlVideoOutput.cpp`, lines 204-245](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/sdl/SdlVideoOutput.cpp#L204-L245), [repository `ISSUES.md`](https://github.com/merkalev/cv710userspace/blob/main/ISSUES.md)

**Observation:** The existing issue documentation claims a co-sited reconstruction in which odd-pixel U/V values are interpolated between the current and next YUY2 macropixel. The checked `convertYuy2ToRgba()` implementation computes `u_val` and `v_val` once per pair and uses the **same** chroma-derived offsets for both `y0` and `y1`. No read of the next macropixel's U/V is visible in this conversion loop.

**Impact:** The code currently performs the conventional pairwise chroma replication. The advertised reconstruction quality is not implemented by this path.

**Fix:** Either implement documented interpolation with correct end-of-line handling and tests, or revise the documentation to accurately describe pairwise chroma replication. Do not claim that all interpolation is universally superior; compare against intended chroma siting and SDL hardware conversion.

### CV-10: Frame-size detection tolerances can hide malformed or padded content

**Priority:** Medium  
**Confidence:** Conditional; requires representative captures  
**Source:** [`lgxdevice.cpp`, lines 72-107 and 250-300](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L72-L107)

**Observation:** Format detection uses loose ranges for accumulated word counts. For example, an inferred 720p frame may be accepted in a 526,000 to 545,000 word range while the emitted texture dimensions assume 460,800 active words. 1080p frames are similarly accepted in a tolerance range around 1,036,800 words. Frames shorter than the inferred target are padded to black, and longer frames are not explicitly rejected before video output.

**Impact:** Partial frames or blanking-inclusive captures can be misclassified and rendered as valid pictures. Wrong per-line stride or prefix content can cause artifacts despite the data fitting within allocated memory.

**Fix:** Validate against a detected ADV7604 mode and confirmed raster layout, rather than only word-count heuristics. Track exact line dimensions, reject impossible sizes, and test mode changes against raw traces.

### CV-11: `bcdUSB` is not the active connection speed

**Priority:** Medium  
**Confidence:** High  
**Source:** [`UsbStream.cpp`, lines 80-95](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L80-L95)

**Observation:** Detection uses `libusb_device_descriptor::bcdUSB >= 0x300` to decide the CV710 is on USB 3.0. That field is the device's USB specification version, not the negotiated bus speed.

**Impact:** A USB 3.0-capable device attached through a USB 2.0 port or cable can pass this check even when 1080p60 raw throughput is impossible.

**Fix:** Use `libusb_get_device_speed(device)` and require `LIBUSB_SPEED_SUPER` or better, with an informative error message.

### CV-12: Clearing endpoint halt synchronously inside the libusb completion path

**Priority:** Medium  
**Confidence:** Conditional, depends on libusb backend behavior  
**Source:** [`UsbStream.cpp`, lines 44-69 and 401-405](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L44-L69)

**Observation:** When a transfer completes with `LIBUSB_TRANSFER_STALL`, the completion callback calls `clearHalt()`, which executes synchronous `libusb_clear_halt()` on the event-processing thread, then immediately attempts resubmission. Return codes from `clearHalt()` are ignored.

**Impact:** Recovery can block event dispatch or repeatedly resubmit to an unrecovered endpoint. Whether this creates a deadlock on a given platform was not established.

**Fix:** Schedule stall recovery outside the completion callback, inspect return codes, and prevent resubmission until recovery succeeds. Exercise a synthetic stalled endpoint where possible.

### CV-13: Signal-status queries treat failed I2C reads as valid zero values

**Priority:** Low  
**Confidence:** High  
**Source:** [`UsbStream.cpp`, lines 228-263](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L228-L263)

**Observation:** The caller ignores return values of all `sendI2cRead()` invocations. Locals initialized to zero are then printed or used as status fields.

**Impact:** Transient USB/I2C errors can look like absent HDMI lock or zero dimensions instead of being recognized as telemetry failures.

**Fix:** Validate returned byte counts, preserve last known good values, and report query failures separately from physical signal loss.

### CV-14: Diagnostic state and minor dead code

**Priority:** Low  
**Confidence:** High for code existence, not for an active data race  
**Sources:** [`lgxdevice.cpp`, lines 112-116 and 152-157](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L112-L116), [`UsbStream.cpp`, lines 120-129 and 152-180](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp#L120-L180)

**Observation:** Local `static` diagnostic counters persist across `Device` instances and sessions. The device-null check is duplicated immediately after a throw-on-null check, and the bootstrap `if` block has confusing indentation.

**Impact:** Primarily confusing diagnostics and maintenance. The inspected stream callback queues bytes, while the parser is called from `update()` on the main thread. Therefore, the `static` parser counters are **not established as an active data race** by current code alone.

**Fix:** Use per-instance counters where appropriate. Remove the duplicate null check and format the bootstrap block consistently. Use atomics only if a variable is genuinely accessed concurrently, not as a blanket response to `static` storage duration.

### CV-15: Pointer casts and C++ language-version portability

**Priority:** Low  
**Confidence:** Conditional  
**Sources:** [`lgxdevice.cpp`, lines 108-110 and 389-425](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp#L389-L425), [`main.cpp`, lines 41-47](https://github.com/merkalev/cv710userspace/blob/main/src/cli/main.cpp#L41-L47), [`CMakeLists.txt`](https://github.com/merkalev/cv710userspace/blob/main/CMakeLists.txt)

**Observation:** The program interprets incoming byte buffers through `uint32_t*` and uses C-style casts at output boundaries. Actual alignment depends on the allocation path; `FrameBuilder` returns `uint32_t*` storage, so the cast is not proof of a crash. Raw USB buffers and endianness do require attention on strict-alignment or big-endian targets. Separately, `main.cpp` uses `#elifdef __APPLE__` while the project declares C++17. `#elifdef` is standardized in C++23 and may be rejected or warned about by strict older toolchains.

**Fix:** Use explicit typed storage or `memcpy` where unaligned data is possible. Document little-endian assumptions. Use `#elif defined(__APPLE__)` for C++17 portability.

### CV-16: Signal handling uses non-signal-safe shared state

**Priority:** Low  
**Confidence:** High  
**Source:** [`main.cpp`, lines 8 and 56-65](https://github.com/merkalev/cv710userspace/blob/main/src/cli/main.cpp#L56-L65)

**Observation:** `SIGINT` and `SIGTERM` handlers assign a normal global `bool do_exit` read by the main loop.

**Impact:** Standard C/C++ signal handling does not guarantee ordinary `bool` access is appropriate for asynchronous signal communication.

**Fix:** Prefer a `volatile std::sig_atomic_t` flag and keep handlers minimal, or use a platform-specific signal/event integration.

---

## Reassessment of the Previous Eight Findings

The supplied review reported eight problems. Below is a direct comparison with the source examined for this audit.

| Original item | Verdict | Corrected assessment |
| --- | --- | --- |
| 1. Static variables in hot loop are critical races | **Overstated** | These counters are shared across instances, but `Device::onFrameData()` is invoked by `UsbStream::update()` on the main thread in the inspected flow. A data race is not demonstrated. Move to instance state for clarity. |
| 2. `CV710_1080P_FRAME_WORDS` instead of `CV710_MAX_FRAME_WORDS` causes sub-1080p overflow | **Incorrect as stated** | Both constants currently equal `1920 * 1080 / 2`. They are not dynamic resolution limits. The separate `FrameBuilder` allocation holds `1920 * 1080` words. The hardcoded line is inconsistent but does not establish the claimed underflow. See CV-10 for real size-handling concerns. |
| 3. C-style `uint8_t*` to `uint32_t*` casts are necessarily misaligned | **Unproven** | The `FrameBuilder` exposes correctly typed `uint32_t*` storage. A cast is a portability smell, not evidence of misalignment in this path. See CV-15. |
| 4. Interlaced parity static counter is a critical thread race | **Partially valid, wrong mechanism** | The more concrete bug is ignoring the field-index flag and emitting a combined frame per field. See CV-04. |
| 5. USB handle leak if setup throws | **Valid** | No cleanup of `_dev` is visible on the pre-read-thread failure path. See CV-06. |
| 6. Duplicate null check | **Valid, low severity** | The second `_dev == nullptr` check is unreachable after the first check throws. See CV-14. |
| 7. Bootstrap indentation | **Valid style issue only** | Braces determine scope, not indentation. See CV-14. |
| 8. Zero-byte transfer causes null pointer dereference in recorder | **Not supported** | `libusb_transfer` is a valid object in the callback, and the transfer buffer is configured from allocated storage. `buffer + 0` is not a null dereference when `buffer` is valid. Defensive checks can help, but the supplied failure claim is not demonstrated. |

**Important correction:** An unconditional `std::atomic` substitution does not fix incorrect interlaced field pairing, and replacing the hardcoded 1080p guard with `CV710_MAX_FRAME_WORDS` changes no value in the current code.

---

## Recommendations by Implementation Phase

### Phase 1: Correctness and resilience

1. Implement a streaming marker parser that is independent of USB transfer boundaries (CV-01).
2. Separate expected timeout, successful data, and unexpected USB errors during setup (CV-05).
3. Make USB ownership and shutdown order exception-safe and serialize pipeline creation (CV-06, CV-07).
4. Add explicit handling of failed transfers, failed resubmissions, and endpoint recovery (CV-08, CV-12).

### Phase 2: Low-latency operation

1. Move the periodic ADV7604 queries off the frame-processing thread (CV-03).
2. Instrument queue age, per-stage processing time, and actual capture-to-present latency (CV-02).
3. Choose a frame-aware overload strategy that never discards arbitrary video/audio chunks.
4. Benchmark direct YUY2 against RGBA conversion under CPU load. Thread creation per converted frame should also be profiled before optimization.

### Phase 3: Modes and presentation

1. Correct interlaced parity and field-pair completion (CV-04).
2. Validate active raster size against confirmed hardware mode (CV-10).
3. Reconcile chroma documentation with the real implementation (CV-09).
4. Improve USB speed detection, signal diagnostics, and portability (CV-11, CV-13 through CV-16).

## Suggested Regression Test Matrix

| Test | Procedure | Expected result |
| --- | --- | --- |
| Split marker | Replay the same stream with C0, C1, and audio headers split across chunk boundaries | Identical frames/audio regardless of transfer splitting |
| Mid-frame loss | Delete one raw transfer from a recording | Discard incomplete frame, recover at next valid C0, no invalid audio burst |
| USB saturation | Slow consumer while running 1080p60 | Bounded latency/memory and predictable resynchronization |
| Control timeout | Make ADV7604 status requests fail or delay | Video event loop remains responsive |
| Interlaced first field | Start replay with either parity first | Correct line ordering and complete output frames |
| Field loss | Drop one interlaced field | No permanent top/bottom inversion |
| Start/stop stress | Stop capture immediately after probe begins, repeat many times | No crashes, hangs, or leaked transfer descriptors |
| Initialization fault | Inject libusb timeout, stall, I/O, and no-device statuses | Status-specific diagnostics and clean recovery or failure |
| Non-1080 modes | Test 720p, 576p/i, 480p/i with raw traces | No accepted wrong-size frame or stride corruption |
| USB 2.0 port | Connect via a high-speed-only hub | Explicit speed rejection |
| Color edge | Use a high-contrast color bar/test pattern | Conversion output matches the documented reconstruction algorithm |

## Inheritance and Attribution

The repository states it is based on ChrisAJS's `lgx2userspace`. Some general architecture is inherited, but this audit **does not assign responsibility for each defect to upstream or the CV710 fork** without a complete, commit-pinned diff. One directly observable inherited issue is using `bcdUSB` in place of negotiated USB speed: the upstream `UsbStream.cpp` performs the same check. CV710-specific protocol framing and mode detection must be reviewed on their own merits.

## Source Index

- [CV710 README](https://github.com/merkalev/cv710userspace/blob/main/README.md)
- [CV710 known issues](https://github.com/merkalev/cv710userspace/blob/main/ISSUES.md)
- [CV710 roadmap](https://github.com/merkalev/cv710userspace/blob/main/TODO.md)
- [CV710 protocol](https://github.com/merkalev/cv710userspace/blob/main/PROTOCOL.md)
- [CV710 `lgxdevice.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.cpp)
- [CV710 `lgxdevice.h`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/lgxdevice.h)
- [CV710 `FrameBuilder.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/src/FrameBuilder.cpp)
- [CV710 `UsbStream.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.cpp)
- [CV710 `UsbStream.h`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/usb/UsbStream.h)
- [CV710 `SdlVideoOutput.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/sdl/SdlVideoOutput.cpp)
- [CV710 `SdlAudioOutput.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/liblgx/liblgx-common/src/sdl/SdlAudioOutput.cpp)
- [CV710 `main.cpp`](https://github.com/merkalev/cv710userspace/blob/main/src/cli/main.cpp)
- [Original `lgx2userspace` source](https://github.com/ChrisAJS/lgx2userspace)

**Review conclusion:** Address the transfer-boundary parser and the control-thread latency coupling before spending effort on cosmetic cleanup or speculative atomic conversions. Validate every fix against captured raw USB traces and live hardware.

## Resolution Status (2026-10-09, local working tree)

All code-level findings below have been addressed in the local source. Line references above describe the audited GitHub `main` snapshot and have drifted. Hardware validation (see the validation plan above) has **not** been performed.

| ID | Status | Fix summary |
|----|--------|-------------|
| CV-01 | Fixed | `Device::onFrameData` now keeps a bounded `_pendingWords` carry buffer; any parse that hits a marker straddling a transfer end (C0 header, C1 trailer, audio header, trailing lock-hunt word) holds the words back and they are prepended to the next transfer before parsing. |
| CV-02 | Fixed | Queue depth reduced 128 → 32 with a **drop-oldest** overload policy (resumes near-live after a burst; the gap is caught by the existing sequence check and resyncs on the next C0). `update()` processes at most **32 transfers or 10 ms** per call so SDL stays responsive while a burst fully recovers; queue age > 250 ms, high-water mark and drops are logged. (Budget raised from 16/4 ms after hardware testing showed the 1080p drain rate ~64 transfers/s exceeded the old budget and caused a persistent 250 ms+ backlog.) |
| CV-03 | Fixed | Periodic ADV7604 polling and automatic VID_STD reprogramming moved off `Device::run()` onto a dedicated `UsbStream::controlLoop` worker started in `queueFrameRead`. Every EP1 control transaction is serialised by `_controlMutex` (atomic request/response pairs); the result is published as an immutable snapshot under `_signalMutex`, so the render thread only ever takes a short lock. The worker is joined before teardown I2C traffic. `Device::run()` reads the cached snapshot only. |
| CV-04 | Fixed (reworked after 1080i black screen) | Field placement uses the device-supplied field index (`C0` metadata `b2 & 0x01`, per `PROTOCOL.md`) **only once it has been observed to alternate** (2 parity changes); some firmware holds it constant, which meant `_fieldsWoven` never reached `0x03` and no 1080i frame was ever emitted (black screen). Until then the code falls back to a local toggle. `_fieldsWoven` resets on incomplete frames and mode switches but is no longer force-reset on every C0 sequence gap (that also prevented weaving). |
| CV-05 | Fixed | The `res == LIBUSB_ERROR_TIMEOUT` probe is gone, and bootstrap command write/read failures throw with the failing command index instead of falling through. The warm-start fast path (`isDeviceBootstrapped()`) was subsequently **removed** — the full bootstrap now always runs so the EDID is re-uploaded and HPD re-driven on every launch (skipping it left the HDMI source with a stale mode list). |
| CV-06 | Fixed | `streamSetupCommands` wraps all post-open work in try/catch and calls the new `closeDevice()` (release interface + close handle) on any throw; `libusb_init` failure is detected and reported. `shutdownStream()` is idempotent and the destructor always runs it, so the handle/interface can no longer leak regardless of which stage failed. Also fixed a working-tree bug where the shutdown FIFO drain used `_frameBuffer` *after* it was deleted (null/2 MB-length buffer). |
| CV-07 | Fixed | `queueAllFrameReads()` moved private and only ever runs from `queueFrameRead()` before the read thread exists; the transfer vector is never mutated from a completion callback. |
| CV-08 | Fixed | `submitTransfer` counts consecutive failures and escalates via `signalError()` when a failure occurs while zero transfers remain active (pipeline dead), instead of only printing. `signalError()` now reports only the **first** fatal error, so a disconnect (all in-flight transfers completing with `NO_DEVICE` at once) can no longer produce a burst of identical fatal messages. |
| CV-09 | Fixed | `convertYuy2ToRgba` now performs the co-sited linear horizontal chroma interpolation documented in `ISSUES.md` (even pixel: chroma as-is; odd pixel: midpoint to the next macropixel's chroma, replicated at end of row), and the even/odd pixel color values are derived from their own luma (previously `y1` used the `y0`-scaled offsets). |
| CV-10 | Partially addressed | The measured-path (ADV7604 geometry) detection from the working tree remains the primary detector. Fallback word-count ranges are unchanged (hardware-calibrated, removing them risks regressions), but any frame whose size differs from the locked mode target is now logged with signed delta, so padding/truncation is visible instead of silent. Full tightening requires captured traces per the validation plan. |
| CV-11 | Fixed | Device availability now requires `libusb_get_device_speed(device) >= LIBUSB_SPEED_SUPER`; the actual negotiated speed is printed on rejection. `bcdUSB` is only reported as context. |
| CV-12 | Fixed | The completion callback no longer runs `clear_halt()`; it parks the stalled transfer in `_stalledTransfers`. `update()` performs `clear_halt()` on the main thread, inspects the return code, and only resubmits parked transfers after recovery succeeds — escalation to `signalError()` after 10 failed attempts. |
| CV-13 | Fixed | `queryVideoSignalStatus` now treats lock/interlace/geometry as **core** reads (keep last known good snapshot + throttled warning if any fail) and all other fields (timings, colorspace, audio, cable, VID_STD) as **optional** (a single failure degrades one field instead of discarding the snapshot). A changed snapshot must be seen **twice in a row** before publication, so flaky I2C reads can no longer flap the HUD/lock badge or restart the audio device every second. The published snapshot (not the raw read) drives logging. |
| CV-14 | Fixed | Function-local statics converted to per-instance members (`_transferCount`, `_c0HuntCount`, `_lastLogged*`, `noteTransferError()`, `_backlogWarnings`); duplicate device-null check in `streamSetupCommands` removed; dead `recordProbeAttempt`/`_probeTransfer`/`MAX_PROBE_ATTEMPTS`/`clearHalt()` and unused `_lastSubmitTime` removed; setup body re-indented inside its try block. |
| CV-15 | Fixed | `#elifdef __APPLE__` replaced with `#elif defined(__APPLE__)` in `main.cpp`; alignment assumptions documented with `assert`s in `Device::onFrameData` plus a little-endian framing note (buffer source alignment verified: `malloc`/`vector<uint8_t>` storage, transfer offsets are multiples of `0x1FC000`). |
| CV-16 | Fixed | `do_exit` is now `volatile std::sig_atomic_t`, initialized to 0, assigned 1 in both signal handlers and SDL quit paths. |

**Verification performed:** full CMake build with no new warnings; `framebuilder_test` passes. **Not performed:** any run against CV710 hardware or raw USB traces (see open items above and in CV-10).

---

## Follow-up Fixes (2026-10-09, after first hardware run)

The first hardware run after the CV-01…CV-16 pass surfaced two regressions and several pre-existing
ADV7604/OSD problems. These were fixed in the same working tree:

| ID | Severity | Finding | Fix |
|----|----------|---------|-----|
| F-01 | High | **1080i black screen.** The CV-04 interlaced path required `_fieldsWoven == 0x03`, but this board's `C0` metadata `b2` field-index bit does not alternate, so the bit mask never completed and `produceVideoData` never fired (hundreds of fields, valid frame counter stuck). The C0 sequence-gap branch also reset `_fieldsWoven` on every gap. | Treat `b2 & 0x01` as the field index only after it is observed to change twice (`_deviceParityReliable`); otherwise toggle locally. `_fieldsWoven` is no longer reset on C0 sequence gaps (still reset on incomplete frames and mode switches). Verified build + unit test; **awaiting hardware confirmation**. |
| F-02 | High | **Reappearing preview latency/backlog.** After CV-02 the drain budget (16 transfers / 4 ms) was below the 1080p production rate (~64+ transfers/s), so the queue slowly filled and the app logged `Backlog: oldest queued transfer is 250–267 ms old` and dropped transfers. | Raised `MAX_ITEMS_PER_UPDATE` 16 → 32 and the per-`update()` deadline 4 ms → 10 ms. |
| F-03 | High | **Wrong ADV7604 status register decode.** Width/height high byte masked with `0x1F` instead of `0x0F` (12-bit values) → width read as actual+4096, so `geometryValid` was always false and `detectVideoMode` never used the measured geometry. Interlaced flag was read from HDMI `0x0B` bit 5 instead of IO `0x12` bit 4. TMDS lock was read from HDMI `0x04` bit 1 (the audio-PLL bit) instead of IO `0x6A & 0xE0`, causing `Lock: YES/NO` flapping. Colorspace was `>> 5` instead of `& 0x0F`. Refresh rate used HDMI `0x05` bit 4 (VSYNC polarity) as a "50 Hz" flag → 60 Hz sources reported as 50 Hz. | Re-derived all of the above from `drivers/media/i2c/adv7604.c`: width/height `0x0FF … 0x0F` mask; interlaced from IO `0x12 & 0x10`; lock from IO `0x6A & 0xE0`; cable from IO `0x6F & 0x01`; colorspace from HDMI `0x53 & 0x0F`; audio lock from HDMI `0x04 & 0x01`; refresh rate computed from pixel clock (HDMI `0x06` + `0x3B` bits 5:4, deep-colour/pixel-repeat adjusted) and H/V totals from the porch registers, then snapped to the nearest standard rate (correctly separates 720p50 from 720p60, which share a pixel clock). |
| F-04 | Medium | **Status flapping / repeated audio re-config.** Flaky I2C reads produced alternating lock/geometry/audio-rate values every poll, restarting the SDL audio stream and spamming the log. | Added a confirm-change debounce: a snapshot differing from the published one must be observed twice before it is published, **except** a transition to `locked` (signal acquisition), which is published immediately so a mode switch / re-lock cannot leave the "no signal" splash on top of live video for two poll intervals. Logging now uses the published snapshot. The auto VID_STD reprogramming was removed (the receiver runs in HDMI auto-detect and the parser derives the mode from the actual frame word count; the extra I2C writes only added instability). |
| F-05 | Medium | **"No input shows 720x576, no splash."** With no HDMI source the ADV7604 free-runs a default pattern (typically 720x576), which the parser accepted as real video, suppressing the "no signal" splash. | Added `VideoSignalInfo::valid` and gated the splash/HUD on `valid && locked`: live video is only shown once the receiver reports TMDS lock; otherwise the splash (BMP or procedural fallback) is rendered. The HUD status badge now prefers the lock state. |
| F-06 | Low | **OSD colorspace labels/auto-selection** used the old `aviColorspace` 0/1/2 semantics with a bad register decode. | Centralised decode in `autoColorspaceForCode`/`aviIsRgb`/`autoColorspaceSourceLabel` (ADV7604 `0x53` nibble), used by `updateMetrics`, `videoFrameAvailable`, the `C` hotkey and the HUD; full-range YCbCr is now handled. |
| F-07 | Low | **Disconnect message storm** — every in-flight transfer completing with `LIBUSB_TRANSFER_NO_DEVICE` printed and re-signalled the same fatal error. | `signalError()` is now first-error-wins (`_errorSignaled`). |
| F-08 | Info | **Warm vs. cold start timing** was not measurable. | Added elapsed-ms instrumentation around the bootstrap (idle + DMA reset + full command list) phase, printed as `Bootstrapping complete (N commands, N ms)`. **Superseded:** the warm-start fast path was later removed entirely (see F-12) because skipping the full bootstrap left the HDMI source with a stale EDID/mode list. |

---

## Second Follow-up Fixes (2026-10-09, after second hardware run)

The second hardware run reported that 1080i still did not render, that audio became
detuned after repeated HDMI reconnects, and that wiggling the HDMI cable produced
tearing/combing that the official driver does not show.

| ID | Severity | Finding | Fix |
|----|----------|---------|-----|
| F-09 | High | **1080i still not recognised.** `detectVideoMode` decided scan mode from the ADV7604 STDI interlace bit (`IO 0x12 & 0x10`) only. When that bit reads 0 for an interlaced field, the adaptive path matches `activeWidth x fieldHeight` (1920x540) as a *progressive* mode, so the field is displayed as a half-height frame and the weave path is never taken. | `detectVideoMode(frameWords, hwInterlaced)` now ORs the FPGA's own `C0` field flag (`b2 & 0x80`, per `PROTOCOL.md`) with the receiver bit, and — when both flags are clear — uses the per-field word-count ranges (1080i ~518,400; 576i ~103,680; 480i ~86,400) as a tie-breaker, since those ranges are disjoint from every progressive mode. The interlaced field height is now derived from the observed word count (`fieldH = frameWords*2/width`) so the result does not depend on whether the receiver reports field or frame height. Added a one-off/periodic `Woven interlaced frame` log. |
| F-10 | High | **Audio detuned/"depressing" after many HDMI replugs.** HDMI `0x18` was decoded as an IEC 60958 sample-rate code, but per Linux `adv7604.c` its bit 0 is only "audio sample packet detected"; the low nibble is not a rate. The code latched random 32/44.1/48 kHz values, and `SDL_SetAudioStreamFormat` then resampled to the wrong pitch. `audioLocked` also ignored packet detect. | Sample rate is now taken from the received **Audio InfoFrame** on the INFOFRAME page (slave `0x3e`, head `0xE3`, payload `0x1C`; sample-rate = payload byte 1 bits 4:2, per CEA-861 / Linux `hdmi_audio_infoframe_unpack`). If the infoframe is absent/unreadable, the last known-good rate is kept and sanitised to 48 kHz — never invented. `audioLocked` now requires **both** HDMI `0x04` bit 0 (PLL) and HDMI `0x18` bit 0 (packet detect). |
| F-11 | Medium | **Wiggle/"freaky" tearing.** While the HDMI clock re-locks (cable wobble) the FPGA still emits a partial, adjusting frame; the parser padded and presented it. | The `C1` trailer `b2` bit 4 is the FPGA "PLL recovering" flag (`0x14` vs locked `0x04`, per `PROTOCOL.md`). Those frames are now dropped (buffer cleared, `_fieldsWoven` cleared, audio unmute counter reset) and the parser resynchronises on the next `C0`, so no partial frame is presented. |
| F-12 | Info | **"Is it really needed to rebootstrap every time?"** | Yes, for now: the full bootstrap is the only thing that re-uploads the 256-byte EDID and drives the receiver's HPD handshake, which is what refreshes the source's mode list without a physical replug. The F-10 audio fix removes the *audio* reason for needing it. The printed `Bootstrapping complete (N commands, N ms)` shows the cost; if it is unacceptable we can add an opt-in `--fast-start` that skips video-only parts while keeping EDID/HPD. |
| F-13 | Medium | **Startup is slow: ~7100 USB transfers replayed verbatim every launch** (4301 OUT + 2807 IN), most of it I2C status polling. The official driver is a resident kernel driver that inits once at attach and keeps state, which is why it looks instant. | Implemented the opt-in `--fast-start` (`-b`) per the F-12 plan, grounded in the capture rather than a guess: `tools/trim_bootstrap.py` derives `cv710_setup_commands_fast` by (1) removing every I2C status-read round-trip (`>02` + `<N`) — reads are passive and write values are verbatim capture bytes, so end state is unchanged; (2) run-length-collapsing the driver's poll loops (identical repeated pure-I2C-write blocks) — idempotent, so final register state is identical; (3) keeping all FPGA/FX3 vendor commands and their request/response pairs intact and in order. Result: **7108 → 871 transfers (≈8.2x)** with the same write sequence, same end state, full EDID upload and HPD handshake. `--fast-start` (`-b`) is the default; `--full-bootstrap` (`-B`) forces the original replay. Also documented that shutdown never tears down the ADV7604/EDID (no `libusb_reset_device`, `set_configuration` skipped when set), so receiver state already persists across runs — the fast path just re-establishes the source-visible handshake in ~1/8 the transfers. |
| F-14 | High | **`--fast-start` "felt the same speed" as the full replay.** Per-command instrumentation showed the 871-command loop totals only **~508 ms**, but the bootstrap wall time was **10 994 ms**. The gap was entirely `libusb_clear_halt()` on EP 0x83: **~5.2 s per call**, invoked three times per run (bootstrap prep + bootstrap post + shutdown drain). That fixed ~10.4 s/launch cost was identical in both paths, which is exactly why trimming the command list changed nothing perceptible. | Removed all three unconditional `libusb_clear_halt()` calls. They are redundant when the endpoint is not halted (the normal case) and this device takes ~5 s to answer a CLEAR_FEATURE(ENDPOINT_HALT). A genuine halt still surfaces as `LIBUSB_TRANSFER_STALL` during streaming and is recovered by the existing CV-12 path (`serviceStalledTransfers()`, main thread, bounded retries). Expected `Bootstrapping complete (871 commands, ~600 ms)`. Diagnostic instrumentation was removed after use. |

**Verification performed for the second follow-up fixes:** full CMake build with no warnings; `framebuilder_test` passes. **Awaiting hardware validation:** 1080i render (watch the `Woven interlaced frame` log), audio pitch after repeated replugs, and clean picture while wiggling the cable.
