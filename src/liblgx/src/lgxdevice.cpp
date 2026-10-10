#include "lgxdevice.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace lgx2 {

    namespace {
        // The CV710 C0 sequence counter runs 1..0xFF and wraps 0xFF -> 0x01: on
        // real hardware it never emits 0x00 (verified by logging every C0 for a
        // full cycle). Treat that wrap as continuous so a healthy stream is not
        // reported as a dropped frame once per 255-frame cycle. A genuine loss
        // across the wrap (e.g. 0xFF -> 0x02) is still detected.
        inline uint8_t nextFrameSeq(uint8_t seq) {
            return seq == 0xFF ? 0x01 : static_cast<uint8_t>(seq + 1);
        }
    }

    Device::Device(Stream *stream, VideoOutput *videoOutput, AudioOutput *audioOutput, Logger *logger, ErrorSink *errorSink)
            : _stream{stream}, _videoOutput{videoOutput}, _audioOutput{audioOutput}, _logger{logger}, _errorSink{errorSink} {
        _interlacedBuffer.resize(1920 * 1080 / 2, 0x80108010u);
        _onFrameData = [&](uint8_t *frameData, uint32_t byteLength) {
            onFrameData(frameData, byteLength);
        };
    }

    bool Device::isDeviceAvailable(DeviceType device) {
        return _stream->deviceAvailable(device);
    }

    void Device::initialise(lgx2::DeviceType deviceType, lgx2::VideoScale videoScale) {
        _errorSink->catchErrors([&]() {
            if (!isDeviceAvailable(deviceType)) {
                throw std::runtime_error("Target device is not available to use - is it plugged in?");
            }

            _stream->streamSetupCommands(deviceType);

            _videoOutput->initialiseVideo(videoScale);
            _audioOutput->initialiseAudio();

            _stream->queryVideoSignalStatus();
            _stream->queueFrameRead(&_onFrameData);
        });
    }

    void Device::run() {
        auto now = std::chrono::steady_clock::now();
        if (_lastValidVideoTime != std::chrono::steady_clock::time_point{} &&
            (now - _lastValidVideoTime) > std::chrono::milliseconds(250)) {
            if (!_audioMuted) {
                _audioMuted = true;
                _consecutiveValidFrames = 0;
                _audioOutput->clearAudio();
            }
        }

        // Refresh metrics from the cached signal status. CV-03: the periodic
        // ADV7604 I2C polling runs on the stream's own control thread (see
        // UsbStream::controlLoop), so a stalled control endpoint can no longer
        // block video processing or SDL presentation here. This is only a cached
        // snapshot copy, so publish it often enough that the standby->live gate
        // (CV-18b) and the HUD react quickly.
        if (_lastSignalCheck == std::chrono::steady_clock::time_point{} ||
            (now - _lastSignalCheck) > std::chrono::milliseconds(200)) {
            _lastSignalCheck = now;
            VideoSignalInfo sig = _stream->getVideoSignalInfo();
            _audioOutput->setAudioSampleRate(sig.audioSampleRate);
            _videoOutput->updateMetrics({sig, _measuredFps, _validFrames, _droppedFrames});
        }

        _logger->logTimeStart("streamUpdate");
        _stream->update();
        _logger->logTimeEnd("streamUpdate", "Stream update");

        _logger->logTimeStart("videoDisplay");
        _videoOutput->display();
        _logger->logTimeEnd("videoDisplay", "Video display update");

        _logger->logTimeStart("audioOutput");
        _audioOutput->render();
        _logger->logTimeEnd("audioOutput", "Audio output render");
    }

    Device::VideoMode Device::detectVideoMode(uint32_t frameWords, bool hwInterlaced) {
        // 1. Adaptive path: use the geometry the ADV7604 measured from the actual TMDS
        //    signal. Works for any resolution (1600x900, 1280x1024, 1366x768, ...)
        //    without needing a table entry. 1 word = 2 YUY2 pixels.
        VideoSignalInfo sig = _stream->getVideoSignalInfo();
        // The FPGA's own C0 field flag (b2 & 0x80) is the authoritative scan-mode
        // source; only fall back to the receiver's STDI interlace bit if the
        // flag is unavailable. Relying on the receiver bit alone misclassified
        // 1080i fields (518,400 words) as a 1920x540 progressive frame.
        bool interlaced = hwInterlaced || sig.interlaced;
        if (!interlaced) {
            // Some firmware leaves both scan-mode flags clear. The per-field
            // word counts of the standard interlaced modes are distinct from
            // every progressive mode, so use them as a tie-breaker (see the
            // fallback table below): 1080i field ~518,400, 576i ~103,680,
            // 480i ~86,400 words.
            if ((frameWords >= 510000 && frameWords <= 524000) ||
                (frameWords >= 98000 && frameWords <= 110000) ||
                (frameWords >= 80000 && frameWords <= 92000)) {
                interlaced = true;
            }
        }
        if (sig.locked && sig.activeWidth > 0) {
            uint32_t w = sig.activeWidth;
            if (interlaced) {
                // Derive the field height from the observed word count so we do
                // not depend on whether the receiver reports the field height
                // (540) or the full frame height (1080) at HDMI 0x09.
                uint32_t fieldH = (frameWords * 2) / w;
                if (fieldH >= 100 && fieldH <= 700) {
                    return {w, fieldH * 2, w * fieldH, "measured-i", true, true};
                }
                // Otherwise trust the receiver-reported geometry.
                if (sig.activeHeight > 0) {
                    uint32_t frameH = (sig.activeHeight <= 700) ? sig.activeHeight * 2 : sig.activeHeight;
                    uint32_t expected = (w * (frameH / 2)) / 2;
                    uint32_t atol = expected / 50 + 64;
                    if (frameWords + atol >= expected && frameWords <= expected + atol) {
                        return {w, frameH, w * (frameH / 2), "measured-i", true, true};
                    }
                }
            } else if (sig.activeHeight > 0) {
                uint32_t h = sig.activeHeight;
                uint32_t expected = (w * h) / 2;
                uint32_t tol = expected / 50 + 64; // ~2%
                if (frameWords + tol >= expected && frameWords <= expected + tol) {
                    return {w, h, expected, "measured-p", true, false};
                }
            }
        }

        // 2. Fallback table (used before the first ADV7604 status read, or if the
        //    measurement disagrees with what the FPGA is actually sending).
        if (frameWords >= 1000000 && frameWords <= 1050000) return {1920, 1080, 1036800, "1080p", true, false};
        if (frameWords >= 510000 && frameWords <= 524000)   return {1920, 1080, 1036800, "1080i", true, true};
        if (frameWords >= 440000 && frameWords <= 480000)   return {1280, 720, 460800, "720p", true, false};
        if (frameWords >= 526000 && frameWords <= 545000)   return {1280, 720, 460800, "720p", true, false};
        if (frameWords >= 195000 && frameWords <= 220000)   return {720, 576, 207360, "576p", true, false};
        if (frameWords >= 98000 && frameWords <= 110000)    return {720, 576, 207360, "576i", true, true};
        if (frameWords >= 160000 && frameWords <= 185000)   return {720, 480, 172800, "480p", true, false};
        if (frameWords >= 80000 && frameWords <= 92000)     return {720, 480, 172800, "480i", true, true};
        return {0, 0, 0, "unknown", false, false};
    }

    void Device::onFrameData(uint8_t *data, uint32_t byteLength) {
        // The transport delivers whole 32-bit words (byteLength is always a
        // multiple of 4) and every buffer handed to us is 4-byte aligned, so a
        // reinterpret_cast to uint32_t* is safe. All framing is little-endian,
        // matching the FX3 FPGA stream (see PROTOCOL.md).
        assert(data != nullptr || byteLength == 0);
        assert(reinterpret_cast<std::uintptr_t>(data) % alignof(uint32_t) == 0);

        uint32_t count = byteLength / 4;
        auto *d = reinterpret_cast<uint32_t *>(data);

        // CV-01: prepend words held back from the previous transfer so markers
        // that straddle a USB transfer boundary (C0, C1 trailer, audio header)
        // are still parsed instead of being discarded.
        std::vector<uint32_t> carried;
        if (!_pendingWords.empty()) {
            carried = std::move(_pendingWords);
            _pendingWords.clear();
            if (count != 0) {
                carried.insert(carried.end(), d, d + count);
            }
            d = carried.data();
            count = static_cast<uint32_t>(carried.size());
        }

        uint32_t i = 0;
        if (++_transferCount <= 5 || (_transferCount % 60) == 0) {
            printf("[Debug] onFrameData transfer #%u (%u bytes)\n", _transferCount, byteLength);
            fflush(stdout);
        }

        // 1. Drain audio continuation if transfer boundary straddled audio payload
        if (_inAudio) {
            uint32_t avail = count - i;
            uint32_t take = std::min(_remainingAudioWords, avail);
            _frameBuilder.buildAudio(reinterpret_cast<uint8_t *>(d + i), take);
            i += take;
            _remainingAudioWords -= take;
            if (_remainingAudioWords == 0) {
                _inAudio = false;
                uint32_t collectedBytes = _frameBuilder.audioFrameSize() * 4;
                if (!_audioMuted && _consecutiveValidFrames >= 2) {
                    produceAudioData(reinterpret_cast<uint8_t *>(_frameBuilder.completeAudioFrame()), collectedBytes);
                } else {
                    _frameBuilder.clearAudio();
                }
                _inAudioPadding = true;
            } else {
                return;
            }
        }

        // 2. Initial lock hunt: discard any data before the first valid C0 header
        if (!_streamLocked) {
            _audioMuted = true;
            _consecutiveValidFrames = 0;
            _frameBuilder.clearAudio();
            _frameBuilder.clearVideo();
            _inVideo = false;
            while (i + 1 < count) {
                if (d[i] == utils::FrameBuilder::VIDEO_FRAME_START_MARKER) {
                    uint32_t meta = d[i + 1];
                    uint8_t b0 = meta & 0xFF;
                    uint8_t b1 = (meta >> 8) & 0xFF;
                    uint8_t b2 = (meta >> 16) & 0xFF;
                    uint8_t b3 = (meta >> 24) & 0xFF;
                    uint8_t chk = (b0 + b1 + b2 - 0x40) & 0xFF;
                    if (++_c0HuntCount <= 10) {
                        printf("[Debug] C0 candidate #%u at word %u: meta=0x%08x [b3=%02x, b2=%02x, b1=%02x, b0=%02x, chk=%02x, b1==1:%d, chk==b3:%d]\n",
                               _c0HuntCount, i, meta, b3, b2, b1, b0, chk, (b1 == 0x01), (chk == b3));
                        fflush(stdout);
                    }
                    if (b1 == 0x01 && (chk == b3)) {
                        _streamLocked = true;
                        // Start the visible drop count at stream lock: the resync
                        // events while hunting for the first C0 are one-time
                        // startup noise, not ongoing transport loss.
                        _droppedFrames = 0;
                        _receiverMismatchDrops = 0;
                        _lastSeq = b0;
                        _frameBuilder.clearVideo();
                        _frameBuilder.clearAudio();
                        _inAudioPadding = false;
                        _inVideo = true;
                        _fieldsWoven = 0;
                        printf("[Debug] STREAM LOCKED to C0 at word %u, seq=0x%02x\n", i, b0);
                        fflush(stdout);
                        i += 2;
                        break;
                    }
                }
                i++;
            }
            if (!_streamLocked) {
                // CV-01: the hunt ran off the end of the transfer - hold the final
                // word back, it may be the first half of a C0 header.
                if (i < count) {
                    holdWordsForNextTransfer(d, count, i);
                }
                return;
            }
        }

        // 3. Main parser loop
        while (i < count) {
            // Skip audio padding (0xAA5555AA) up to the next marker
            if (_inAudioPadding) {
                while (i < count && d[i] == utils::FrameBuilder::AUDIO_FRAME_END_MARKER) i++;
                if (i < count) {
                    _inAudioPadding = false;
                } else {
                    break;
                }
            }

            // Check for C0 (VIDEO_FRAME_START_MARKER)
            if (d[i] == utils::FrameBuilder::VIDEO_FRAME_START_MARKER) {
                if (i + 1 < count) {
                    uint32_t meta = d[i + 1];
                    uint8_t b0 = meta & 0xFF;
                    uint8_t b1 = (meta >> 8) & 0xFF;
                    uint8_t b2 = (meta >> 16) & 0xFF;
                    uint8_t b3 = (meta >> 24) & 0xFF;
                    if (b1 == 0x01 && (((b0 + b1 + b2 - 0x40) & 0xFF) == b3)) {
                        // Genuine C0 header marks the start of a new video frame / field.
                        _currentFieldFlags = b2;
                        if (_lastSeq >= 0 && b0 != nextFrameSeq(static_cast<uint8_t>(_lastSeq))) {
                            // Transport gap (CPU lag / dropped USB transfers).
                            // Keep the weave parity state: clearing it here would
                            // prevent interlaced frames from ever completing when
                            // the FPGA sequence counter legitimately skips (e.g.
                            // per-frame sequencing in 1080i). A mix of a stale and
                            // a fresh field self-heals on the next field.
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
                        } else if (_inVideo && _frameBuilder.videoFrameSize() > 0) {
                            // Previous frame did not reach C1 before new C0
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
                            _fieldsWoven = 0;
                        }

                        _lastSeq = b0;
                        _frameBuilder.clearVideo();
                        _frameBuilder.clearAudio();
                        _inAudio = false;
                        _inAudioPadding = false;
                        _inVideo = true;
                        i += 2;
                        continue;
                    }
                } else {
                    // CV-01: C0 straddles the end of the transfer - hold the marker
                    // back so the next transfer can pair it with its metadata word
                    holdWordsForNextTransfer(d, count, i);
                    return;
                }

                // A C0 word whose metadata failed validation (b1 != 0x01 or bad
                // checksum) means this frame-start boundary cannot be trusted -
                // typically a corrupt read during a mode switch / clock recovery.
                // Absorbing the marker words as pixels would shift every remaining
                // scanline by 2 words (4 px) and wrap the row stride, so the frame
                // would still pass the word-count mode check and reach the display
                // "misaligned". Discard the partial frame and resynchronise on the
                // next genuine C0 instead.
                if (_inVideo || _frameBuilder.videoFrameSize() > 0) {
                    _frameBuilder.clearVideo();
                    _inVideo = false;
                    _consecutiveValidFrames = 0;
                    _droppedFrames++;
                    _fieldsWoven = 0;
                }
                i += 2;
                continue;
            }

            // Check for C1 (VIDEO_FRAME_END_MARKER)
            if (d[i] == utils::FrameBuilder::VIDEO_FRAME_END_MARKER) {
                if (i + 2 < count) {
                    uint32_t t1 = d[i + 1];
                    uint8_t tb0 = t1 & 0xFF;
                    uint8_t tb1 = (t1 >> 8) & 0xFF;
                    uint8_t tb2 = (t1 >> 16) & 0xFF;
                    uint8_t tb3 = (t1 >> 24) & 0xFF;

                    uint32_t t2 = d[i + 2];
                    uint8_t b4 = t2 & 0xFF;
                    uint8_t expectedChk = (tb0 + tb1 + tb2 + tb3 - 0x3F) & 0xFF;

                    // Strict C1 trailer validation:
                    // 1. tb1 must be 0x02
                    // 2. Hardware FPGA checksum formula MUST match b4
                    // 3. Sequence counter must match current frame sequence
                    // 4. Must have accumulated at least minimal valid field/frame size (>= 70,000 words)
                    bool trailerValid = (tb1 == 0x02 && expectedChk == b4 &&
                                         (_lastSeq < 0 || tb0 == static_cast<uint8_t>(_lastSeq)));

                    // b2 bit 4 is the FPGA "HDMI clock / PLL recovering" flag
                    // (0x14 vs the locked 0x04, see PROTOCOL.md). While the clock
                    // re-locks - e.g. when the HDMI cable is wiggled - the frame
                    // is partial/adjusting, so presenting it tears and combs (the
                    // "freaky" picture). Drop it and resynchronise on the next C0.
                    if (trailerValid && (tb2 & 0x10) != 0) {
                        _frameBuilder.clearVideo();
                        _inVideo = false;
                        _consecutiveValidFrames = 0;
                        _fieldsWoven = 0;
                        if (++_syncDropCount <= 3 || (_syncDropCount % 300) == 0) {
                            printf("[Video] Dropped frame while HDMI clock recovering (b2=0x%02x, count=%u)\n",
                                   tb2, _syncDropCount);
                            fflush(stdout);
                        }
                        i += 2;
                        if (i < count && (d[i] < 0x10000 || (d[i] & 0xFF000000) == 0x00000000)) i++;
                        continue;
                    }

                    bool genuineC1 = false;
                    if (trailerValid && _frameBuilder.videoFrameSize() >= 40000) {
                        genuineC1 = true;
                    }

                    if (genuineC1) {
                        // Genuine C1 trailer: active video frame or field is complete
                        uint32_t frameWords = _frameBuilder.videoFrameSize();
                        VideoMode mode = detectVideoMode(frameWords, (_currentFieldFlags & 0x80) != 0);

                        // Receiver cross-check (resolution-switch desync guard):
                        // the ADV7604 reports the real input geometry. While the
                        // source re-locks, the FPGA can emit a transitional frame
                        // with a valid marker pair and an in-range word count for
                        // the WRONG width - e.g. a 1920-wide 480-line remnant and
                        // a 1280-wide 720-line frame both total 460800 words.
                        // Presenting it with the locked stride wraps the right
                        // side of every line ("duplicated / misaligned lines" for
                        // a few frames). If a locked receiver contradicts the
                        // detected width, drop the frame and let the parser and
                        // status catch up.
                        bool receiverMismatch = false;
                        if (mode.valid) {
                            lgx2::VideoSignalInfo sig = _stream->getVideoSignalInfo();
                            if (sig.locked && sig.activeWidth > 0 && sig.activeWidth != mode.width) {
                                receiverMismatch = true;
                            }
                        }

                        if (mode.valid && !receiverMismatch) {
                            // Check mode stability hysteresis: require 4 consecutive matching frames
                            if (mode.width == _activeWidth && mode.height == _activeHeight) {
                                _pendingCount = 0;
                            } else {
                                if (mode.width == _pendingWidth && mode.height == _pendingHeight) {
                                    _pendingCount++;
                                    if (_pendingCount >= 4) {
                                        printf("[Video] Mode switch locked: %ux%u (%s)\n", mode.width, mode.height, mode.name);
                                        fflush(stdout);
                                        _activeWidth = _pendingWidth;
                                        _activeHeight = _pendingHeight;
                                        _pendingCount = 0;
                                        _fieldsWoven = 0; // CV-04: drop half-woven old-mode fields
                                    }
                                } else {
                                    _pendingWidth = mode.width;
                                    _pendingHeight = mode.height;
                                    _pendingCount = 1;
                                }
                            }

                            // Only deliver frame if it matches the locked active mode
                            if (mode.width == _activeWidth && mode.height == _activeHeight) {
                                if (mode.interlaced) {
                                    if (_interlacedBuffer.size() < mode.targetWords) {
                                        _interlacedBuffer.resize(mode.targetWords, 0x80108010u);
                                    }
                                    uint32_t strideWords = mode.width / 2;
                                    uint32_t fieldLines = mode.height / 2;
                                    // CV-23: a field whose word count does not match
                                    // the locked geometry is corrupt - weaving it with
                                    // the other (good) field would blend shifted rows.
                                    // Drop the field and wait for a clean pair instead
                                    // of presenting a combed / skewed picture.
                                    if (frameWords != strideWords * fieldLines) {
                                        noteCorruptFrame(frameWords, mode.name, "invalid field size");
                                        _fieldsWoven = 0;
                                    } else {
                                        // CV-04: determine the field parity. Use the
                                        // device-supplied field index (C0 metadata b2
                                        // bit 0, see PROTOCOL.md) only once it has been
                                        // observed to actually alternate; otherwise some
                                        // firmware holds it constant and weaving would
                                        // never complete (1080i black screen). Fall back
                                        // to a local toggle in that case.
                                        bool deviceOdd = (_currentFieldFlags & 0x01) != 0;
                                        int8_t deviceParity = deviceOdd ? 1 : 0;
                                        if (_lastDeviceParity >= 0 && deviceParity != _lastDeviceParity &&
                                            _deviceParityChanges < 0xFF) {
                                            _deviceParityChanges++;
                                            if (_deviceParityChanges >= 2) {
                                                _deviceParityReliable = true;
                                            }
                                        }
                                        _lastDeviceParity = deviceParity;

                                        bool oddField;
                                        if (_deviceParityReliable) {
                                            oddField = deviceOdd;
                                        } else {
                                            _weaveToggle = !_weaveToggle;
                                            oddField = _weaveToggle;
                                        }
                                        // If the chosen parity is already woven for this
                                        // frame, the opposite field was dropped or the
                                        // device flag is stuck. Use the free slot so the
                                        // weave still completes instead of waiting forever
                                        // (which shows as a black/never-updating 1080i
                                        // picture).
                                        if (_fieldsWoven & (oddField ? 0x02 : 0x01)) {
                                            oddField = !oddField;
                                        }
                                        uint32_t *srcField = _frameBuilder.videoFrameData();
                                        for (uint32_t y = 0; y < fieldLines && (y * strideWords) < frameWords; y++) {
                                            uint32_t dstLine = oddField ? (y * 2 + 1) : (y * 2);
                                            memcpy(_interlacedBuffer.data() + dstLine * strideWords,
                                                   srcField + y * strideWords,
                                                   strideWords * sizeof(uint32_t));
                                        }
                                        _fieldsWoven |= oddField ? 0x02 : 0x01;
                                        // Only present a complete woven frame once both fields
                                        // have been written; a half-written weave would combine
                                        // newly captured lines with stale ones.
                                        if (_fieldsWoven == 0x03) {
                                            _fieldsWoven = 0;
                                            if (_validFrames <= 5 || (_validFrames % 120) == 0) {
                                                printf("[Video] Woven interlaced frame: %ux%u b2=0x%02x hwInt=%d parityReliable=%d fieldWords=%u\n",
                                                       _activeWidth, _activeHeight, _currentFieldFlags,
                                                       (_currentFieldFlags & 0x80) != 0, _deviceParityReliable, frameWords);
                                                fflush(stdout);
                                            }
                                            produceVideoData(mode.targetWords, _activeWidth, _activeHeight,
                                                             reinterpret_cast<uint8_t *>(_interlacedBuffer.data()));
                                            _validFrames++;
                                            _skewRun = 0;
                                        }
                                    }
                                } else {
                                    // CV-23: a frame whose word count differs from the
                                    // locked geometry is corrupt by construction - a
                                    // single word lost or duplicated mid-frame shifts
                                    // every following scanline by 1 word (2 px), so
                                    // padding / truncating and presenting it would put
                                    // a permanent "skewed lines" seam into the picture.
                                    // Drop instead: a skipped frame is invisible next
                                    // to a misaligned one, and the next genuine C0
                                    // re-anchors the raster.
                                    if (frameWords != mode.targetWords) {
                                        noteCorruptFrame(frameWords, mode.name, "locked-mode size mismatch");
                                    } else {
                                        produceVideoData(mode.targetWords, _activeWidth, _activeHeight,
                                                         reinterpret_cast<uint8_t *>(_frameBuilder.videoFrameData()));
                                        _validFrames++;
                                        _skewRun = 0;
                                    }
                                }
                            } else {
                                // Not the locked mode: discard any half-woven field state
                                _fieldsWoven = 0;
                            }
                        } else if (receiverMismatch) {
                            // Transitional frame whose detected width contradicts
                            // the locked receiver: drop it without touching the
                            // mode hysteresis, so a wrong-width frame can never be
                            // locked or presented.
                            if (++_receiverMismatchDrops <= 3 || (_receiverMismatchDrops % 300) == 0) {
                                printf("[Video] Dropped transitional frame: %u words -> %ux%u (%s) contradicts receiver width\n",
                                       frameWords, mode.width, mode.height, mode.name);
                                fflush(stdout);
                            }
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
                            _fieldsWoven = 0;
                        } else if (frameWords > 0) {
                            if (frameWords != _lastLoggedFrameWords || (++_sizeLogCount % 60) == 0) {
                                printf("[Video] Unmatched frameWords: %u (expected 1080p: ~1036800, 720p: ~460800, 1080i: ~518400)\n", frameWords);
                                fflush(stdout);
                                _lastLoggedFrameWords = frameWords;
                            }
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
                        }
                        _frameBuilder.clearVideo();
                        _inVideo = false;

                        // Skip C1 marker + trailer words:
                        i += 2;
                        if (i < count && (d[i] < 0x10000 || (d[i] & 0xFF000000) == 0x00000000)) i++;
                        continue;
                    }
                } else if (i + 2 >= count) {
                    // CV-01: C1 straddles the transfer boundary - hold the marker and
                    // any available trailer words back for the next transfer
                    holdWordsForNextTransfer(d, count, i);
                    return;
                }

                // Lone C1: a frame-end marker whose trailer failed validation (bad
                // checksum, tb1 != 0x02, sequence mismatch, or frame size < 40000
                // words). The marker still marks the frame boundary with high
                // confidence - absorbing it as pixels would shift the remaining
                // scanlines by 2 words and present a wrapped, misaligned frame.
                // End the frame here and discard the partial result; the parser
                // resumes cleanly at the next genuine C0.
                if (_inVideo || _frameBuilder.videoFrameSize() > 0) {
                    _frameBuilder.clearVideo();
                    _inVideo = false;
                    _consecutiveValidFrames = 0;
                    _droppedFrames++;
                    _fieldsWoven = 0;
                }
                i++;
                continue;
            }

            // Check for 58 (AUDIO_FRAME_START_MARKER)
            if (d[i] == utils::FrameBuilder::AUDIO_FRAME_START_MARKER) {
                if (i + 1 < count) {
                    uint32_t rawLen = d[i + 1];
                    uint32_t audioBytes = (((rawLen >> 16) & 0xFF) << 8) | ((rawLen >> 24) & 0xFF);
                    if (audioBytes >= 512 && audioBytes <= 8192) {
                        uint32_t audioWords = audioBytes / 4;
                        i += 2; // skip marker + len word
                        uint32_t avail = count - i;
                        uint32_t take = std::min(audioWords, avail);
                        _frameBuilder.buildAudio(reinterpret_cast<uint8_t *>(d + i), take);
                        i += take;
                        _remainingAudioWords = audioWords - take;
                        if (_remainingAudioWords == 0) {
                            uint32_t collectedBytes = _frameBuilder.audioFrameSize() * 4;
                            if (!_audioMuted && _consecutiveValidFrames >= 2) {
                                produceAudioData(reinterpret_cast<uint8_t *>(_frameBuilder.completeAudioFrame()), collectedBytes);
                            } else {
                                _frameBuilder.clearAudio();
                            }
                            _inAudioPadding = true;
                        } else {
                            _inAudio = true;
                        }
                        continue;
                    }
                } else {
                    // CV-01: audio header straddles the end of the transfer - hold the
                    // marker back so its length word is not lost
                    holdWordsForNextTransfer(d, count, i);
                    return;
                }

                if (_inVideo && _frameBuilder.videoFrameSize() < CV710_MAX_FRAME_WORDS) {
                    _frameBuilder.buildVideo(reinterpret_cast<uint8_t *>(d + i), 1);
                }
                i++;
                continue;
            }

            // Slice up to the next marker
            uint32_t start = i;
            while (i < count &&
                   d[i] != utils::FrameBuilder::VIDEO_FRAME_START_MARKER &&
                   d[i] != utils::FrameBuilder::VIDEO_FRAME_END_MARKER &&
                   d[i] != utils::FrameBuilder::AUDIO_FRAME_START_MARKER) {
                i++;
            }

            // ONLY accumulate into video if we are in active video
            if (_inVideo) {
                uint32_t curWords = _frameBuilder.videoFrameSize();
                if (curWords < CV710_MAX_FRAME_WORDS) {
                    uint32_t slice = i - start;
                    if (curWords + slice > CV710_MAX_FRAME_WORDS) {
                        slice = CV710_MAX_FRAME_WORDS - curWords;
                    }
                    if (slice > 0) {
                        _frameBuilder.buildVideo(reinterpret_cast<uint8_t *>(d + start), slice);
                    }
                }
            }
        }
    }

    void Device::produceVideoData(uint32_t frameSize, uint32_t width, uint32_t height, uint8_t *data) {
        _videoOutput->videoFrameAvailable((uint32_t *) data, width, height);

        auto now = std::chrono::steady_clock::now();
        _lastValidVideoTime = now;
        _consecutiveValidFrames++;
        if (_consecutiveValidFrames >= 2) {
            _audioMuted = false;
        }

        if (_validFrames <= 5 || (_validFrames % 60) == 0) {
            printf("[Debug] produceVideoData: valid=%u, drops=%u, %ux%u (%u words)\n",
                   _validFrames, _droppedFrames, width, height, frameSize);
            fflush(stdout);
        }

        _videoFrameCount++;
        if (frameSize < _minVideoFrameSize) _minVideoFrameSize = frameSize;
        if (frameSize > _maxVideoFrameSize) _maxVideoFrameSize = frameSize;

        if (_fpsTimestamp == std::chrono::steady_clock::time_point{}) {
            _fpsTimestamp = now;
        } else if (now - _fpsTimestamp >= std::chrono::seconds(1)) {
            auto elapsed = std::chrono::duration<double>(now - _fpsTimestamp).count();
            _measuredFps = static_cast<float>(static_cast<double>(_videoFrameCount) / elapsed);
            printf("Frames: %" PRIu64 " (%.1f fps)  drops: %u  valid: %u  size: %u uint32s\n",
                   _videoFrameCount,
                   _measuredFps,
                   _droppedFrames, _validFrames,
                   frameSize);
            fflush(stdout);
            _videoOutput->updateMetrics({_stream->getVideoSignalInfo(), _measuredFps, _validFrames, _droppedFrames});
            _fpsTimestamp = now;
            _videoFrameCount = 0;
            _minVideoFrameSize = UINT32_MAX;
            _maxVideoFrameSize = 0;
        }
    }

    void Device::produceAudioData(uint8_t *data, uint32_t byteLength) {
        _audioOutput->audioFrameAvailable((uint32_t *) data, byteLength);
    }

    void Device::reassertStream() {
        _autoResyncCount++;
        if (_stream) {
            // CV-23a: software replug = toggle the FPGA stream-enable bit off/on
            // (the registers shutdownStream/queueFrameRead already use) rather than
            // the FX3 DMA reset (0x14). The 0x14 command is only safe at stream
            // start: issued mid-stream it kills the EP1 control path - ADV7604 I2C
            // status reads fail and bulk streaming never resumes, leaving the app
            // stuck on the standby BMP. The FPGA toggle re-arms frame generation
            // with no risk to the control path.
            _stream->resetStreamPipeline();
        }

        // Re-anchor every parser state on the next genuine C0. Frames buffered
        // before the reset are stale garbage; the lock hunt below discards
        // everything until a fresh, valid C0 header arrives.
        _streamLocked = false;
        _inVideo = false;
        _inAudio = false;
        _inAudioPadding = false;
        _remainingAudioWords = 0;
        _pendingWords.clear();
        _frameBuilder.clearVideo();
        _frameBuilder.clearAudio();
        _fieldsWoven = 0;
        _deviceParityReliable = false;
        _lastDeviceParity = -1;
        _deviceParityChanges = 0;
        _weaveToggle = false;
        _lastSeq = -1;
        _consecutiveValidFrames = 0;
        _audioMuted = true;
        _skewRun = 0;
        _c0HuntCount = 0;
        printf("[Device] Capture pipeline re-asserted (software replug #%u) - waiting for the next C0 lock\n",
               _autoResyncCount);
        fflush(stdout);
    }

    void Device::noteCorruptFrame(uint32_t frameWords, const char *modeName, const char *reason) {
        _skewRun++;
        _droppedFrames++;
        _consecutiveValidFrames = 0;
        if (_skewLogCount == 0 || (_skewLogCount % 60) == 0) {
            printf("[Video] Dropped corrupt frame: %u words, %s (%s), streak=%u\n",
                   frameWords, modeName, reason, _skewRun);
            fflush(stdout);
        }
        _skewLogCount++;

        // A persistent run of un-presentable frames while the receiver reports a
        // *locked* signal means the capture pipeline itself is stuck (FPGA/FX3
        // FIFO pointers or the HDMI receiver) - the state that previously only a
        // physical replug could clear. Fire the software replug automatically.
        lgx2::VideoSignalInfo sig{};
        if (_stream) {
            sig = _stream->getVideoSignalInfo();
        }
        if (_skewRun >= kCorruptFramesBeforeAutoResync && sig.locked && sig.activeWidth > 0 && _streamLocked) {
            printf("[Video] Capture pipeline corrupt for %u consecutive frames while receiver is locked - "
                   "automatic re-sync (software replug)\n", _skewRun);
            fflush(stdout);
            reassertStream();
        }
    }

    void Device::holdWordsForNextTransfer(const uint32_t *words, uint32_t count, uint32_t start) {
        if (start >= count) return;
        const uint32_t remain = count - start;
        if (_pendingWords.size() + remain > MAX_PENDING_WORDS) {
            // Defensive: the straddle conditions hold back at most three words,
            // so this should be unreachable. Drop rather than grow unbounded.
            printf("[Parser] Unexpected carry-over of %zu words - discarding\n", _pendingWords.size() + remain);
            fflush(stdout);
            _pendingWords.clear();
            return;
        }
        _pendingWords.insert(_pendingWords.end(), words + start, words + count);
    }

    void Device::shutdown() {
        _videoOutput->shutdownVideo();
        _audioOutput->shutdownAudio();
        _stream->shutdownStream();
    }
}