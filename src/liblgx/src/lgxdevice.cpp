#include "lgxdevice.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <stdexcept>

namespace lgx2 {

    Device::Device(Stream *stream, VideoOutput *videoOutput, AudioOutput *audioOutput, Logger *logger, ErrorSink *errorSink)
            : _stream{stream}, _videoOutput{videoOutput}, _audioOutput{audioOutput}, _logger{logger}, _errorSink{errorSink} {
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

    struct VideoMode {
        uint32_t width;
        uint32_t height;
        uint32_t targetWords;
        const char *name;
        bool valid;
    };

    static VideoMode detectVideoMode(uint32_t frameWords) {
        // 1080p: target 1,036,800 words (tolerance: 1,020,000 to 1,045,000)
        if (frameWords >= 1020000 && frameWords <= 1045000) {
            return {1920, 1080, 1036800, "1080p", true};
        }
        // 720p standard: target 460,800 words (tolerance: 440,000 to 480,000)
        if (frameWords >= 440000 && frameWords <= 480000) {
            return {1280, 720, 460800, "720p", true};
        }
        // 720p with blanking overhead (~520,000 to 540,000 words)
        if (frameWords >= 520000 && frameWords <= 540000) {
            return {1280, 720, 460800, "720p", true};
        }
        // 576p: target 207,360 words (tolerance: 195,000 to 220,000)
        if (frameWords >= 195000 && frameWords <= 220000) {
            return {720, 576, 207360, "576p", true};
        }
        // 480p: target 172,800 words (tolerance: 160,000 to 185,000)
        if (frameWords >= 160000 && frameWords <= 185000) {
            return {720, 480, 172800, "480p", true};
        }
        // Incomplete / corrupted frame
        return {0, 0, 0, "unknown", false};
    }

    void Device::onFrameData(uint8_t *data, uint32_t byteLength) {
        const uint32_t count = byteLength / 4;
        auto *d = reinterpret_cast<uint32_t *>(data);
        uint32_t i = 0;
        static uint32_t transferCount = 0;
        if (++transferCount <= 5 || (transferCount % 60) == 0) {
            printf("[Debug] onFrameData transfer #%u (%u bytes)\n", transferCount, byteLength);
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
                    static uint32_t c0Hunts = 0;
                    if (++c0Hunts <= 10) {
                        printf("[Debug] C0 candidate #%u at word %u: meta=0x%08x [b3=%02x, b2=%02x, b1=%02x, b0=%02x, chk=%02x, b1==1:%d, chk==b3:%d]\n",
                               c0Hunts, i, meta, b3, b2, b1, b0, chk, (b1 == 0x01), (chk == b3));
                        fflush(stdout);
                    }
                    if (b1 == 0x01 && (chk == b3)) {
                        _streamLocked = true;
                        _lastSeq = b0;
                        _frameBuilder.clearVideo();
                        _frameBuilder.clearAudio();
                        _inAudioPadding = false;
                        _inVideo = true;
                        printf("[Debug] STREAM LOCKED to C0 at word %u, seq=0x%02x\n", i, b0);
                        fflush(stdout);
                        i += 2;
                        break;
                    }
                }
                i++;
            }
            if (!_streamLocked) return;
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
                        // Genuine C0 header marks the start of a new video frame.
                        if (_lastSeq >= 0 && b0 != ((_lastSeq + 1) & 0xFF)) {
                            // Transport gap (CPU lag / dropped USB transfers)
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
                        } else if (_inVideo && _frameBuilder.videoFrameSize() > 0) {
                            // Previous frame did not reach C1 before new C0
                            _droppedFrames++;
                            _consecutiveValidFrames = 0;
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
                    // C0 straddles the end of transfer: stop here so next transfer validates it
                    break;
                }

                // If not genuine C0: only treat as pixel if we are currently in active video
                if (_inVideo && _frameBuilder.videoFrameSize() < CV710_MAX_FRAME_WORDS) {
                    _frameBuilder.buildVideo(reinterpret_cast<uint8_t *>(d + i), 1);
                }
                i++;
                continue;
            }

            // Check for C1 (VIDEO_FRAME_END_MARKER)
            if (d[i] == utils::FrameBuilder::VIDEO_FRAME_END_MARKER) {
                if (i + 1 < count) {
                    uint32_t t1 = d[i + 1];
                    uint8_t tb0 = t1 & 0xFF;
                    uint8_t tb1 = (t1 >> 8) & 0xFF;
                    uint8_t tb2 = (t1 >> 16) & 0xFF;
                    uint8_t tb3 = (t1 >> 24) & 0xFF;

                    bool genuineC1 = false;
                    if (tb1 == 0x02) {
                        if (tb3 == 0x38 || tb3 == 0xD0) {
                            genuineC1 = true;
                        } else if (i + 2 < count) {
                            uint8_t b4 = d[i + 2] & 0xFF;
                            uint8_t expectedChk = (tb0 + tb1 + tb2 + tb3 - 0x3F) & 0xFF;
                            if (expectedChk == b4) {
                                genuineC1 = true;
                            }
                        }
                    }

                    if (genuineC1) {
                        // Genuine C1 trailer: active video frame is complete
                        uint32_t frameWords = _frameBuilder.videoFrameSize();
                        VideoMode mode = detectVideoMode(frameWords);
                        if (mode.valid) {
                            // Check mode stability hysteresis: require 5 consecutive matching frames
                            if (mode.width == _activeWidth && mode.height == _activeHeight) {
                                _pendingCount = 0;
                            } else {
                                if (mode.width == _pendingWidth && mode.height == _pendingHeight) {
                                    _pendingCount++;
                                    if (_pendingCount >= 5) {
                                        printf("[Video] Mode switch locked: %ux%u (%s)\n", mode.width, mode.height, mode.name);
                                        fflush(stdout);
                                        _activeWidth = _pendingWidth;
                                        _activeHeight = _pendingHeight;
                                        _pendingCount = 0;
                                    }
                                } else {
                                    _pendingWidth = mode.width;
                                    _pendingHeight = mode.height;
                                    _pendingCount = 1;
                                }
                            }

                            // Only deliver frame if it matches the locked active mode
                            if (mode.width == _activeWidth && mode.height == _activeHeight) {
                                if (frameWords < mode.targetWords) {
                                    uint32_t *vData = _frameBuilder.videoFrameData();
                                    std::fill(vData + frameWords, vData + mode.targetWords, 0x80108010u);
                                }
                                produceVideoData(mode.targetWords, _activeWidth, _activeHeight, reinterpret_cast<uint8_t *>(_frameBuilder.videoFrameData()));
                                _validFrames++;
                            }
                        } else if (frameWords > 0) {
                            static uint32_t lastPrintWords = 0;
                            static uint32_t lastPrintCount = 0;
                            if (frameWords != lastPrintWords || ++lastPrintCount % 60 == 0) {
                                printf("[Video] Unmatched frameWords: %u (expected 1080p: ~1036800, 720p: ~460800)\n", frameWords);
                                fflush(stdout);
                                lastPrintWords = frameWords;
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
                } else if (i + 1 >= count) {
                    // C1 straddles transfer boundary: break so next transfer handles it
                    break;
                }

                // Lone C1: only treat as video word if in active video
                if (_inVideo && _frameBuilder.videoFrameSize() < CV710_MAX_FRAME_WORDS) {
                    _frameBuilder.buildVideo(reinterpret_cast<uint8_t *>(d + i), 1);
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
                    // 58 straddles end of transfer
                    break;
                }

                if (_inVideo && _frameBuilder.videoFrameSize() < CV710_1080P_FRAME_WORDS) {
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
            printf("Frames: %" PRIu64 " (%.1f fps)  drops: %u  valid: %u  size: %u uint32s\n",
                   _videoFrameCount,
                   static_cast<double>(_videoFrameCount) / elapsed,
                   _droppedFrames, _validFrames,
                   frameSize);
            fflush(stdout);
            _fpsTimestamp = now;
            _videoFrameCount = 0;
            _minVideoFrameSize = UINT32_MAX;
            _maxVideoFrameSize = 0;
        }
    }

    void Device::produceAudioData(uint8_t *data, uint32_t byteLength) {
        _audioOutput->audioFrameAvailable((uint32_t *) data, byteLength);
    }

    void Device::shutdown() {
        _videoOutput->shutdownVideo();
        _audioOutput->shutdownAudio();
        _stream->shutdownStream();
    }
}