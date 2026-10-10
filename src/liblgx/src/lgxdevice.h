#ifndef LGX2USERSPACE_LGXDEVICE_H
#define LGX2USERSPACE_LGXDEVICE_H

#include <functional>
#include <cstdint>
#include <string>
#include <chrono>
#include <vector>
#include "FrameBuilder.h"

namespace lgx2 {

    enum class DeviceType {
        CV710
    };

    class ErrorSink {
    public:
        virtual ~ErrorSink() = default;
        virtual void catchErrors(const std::function<void()> &run) = 0;
    };

    class Logger {
    public:
        virtual ~Logger() = default;
        virtual void logTimeStart(const std::string &name) = 0;

        virtual void logTimeEnd(const std::string &name, const std::string &message) = 0;

        virtual void summarise() = 0;
    };

    enum class VideoInputSource {
        HDMI,
        Component
    };

    struct VideoSignalInfo {
        bool valid{false};       // true once at least one status snapshot has been published
        bool locked{false};      // ADV7604 TMDS/STDI/SSPD lock (real HDMI signal present)
        bool cableDetected{false};
        bool interlaced{false};
        uint16_t activeWidth{0};
        uint16_t activeHeight{0};
        uint16_t totalLines{0};
        uint8_t vidStd{0};
        uint32_t audioSampleRate{48000};
        bool audioLocked{false};
        float measuredFps{60.0f};
        // Raw ADV7604 HDMI-map 0x53 colorspace code (see Linux adv7604.c):
        // 0 RGB limited, 1 RGB full, 2 YCbCr601 limited, 3 YCbCr709 limited,
        // 6 YCbCr601 full, 7 YCbCr709 full, others xvYCC/sYCC/opRGB.
        uint8_t aviColorspace{1};
    };

    struct DisplayMetrics {
        VideoSignalInfo signalInfo{};
        float liveFps{0.0f};
        uint32_t validFrames{0};
        uint32_t droppedFrames{0};
    };

    class Stream {
    public:
        virtual ~Stream() = default;
        virtual bool deviceAvailable(DeviceType deviceType) = 0;

        virtual void streamSetupCommands(DeviceType deviceType) = 0;

        virtual void queueFrameRead(std::function<void(uint8_t *frameData, uint32_t byteLength)> *onData) = 0;

        virtual void update() = 0;

        virtual void shutdownStream() = 0;

        virtual void setVideoInput(VideoInputSource source) { (void)source; }
        // CV-15/fast-start: replay the trimmed bootstrap (I2C status reads and
        // poll-loop duplicates removed) instead of the full capture.
        virtual void setFastBootstrap(bool fast) { (void)fast; }
        virtual void queryVideoSignalStatus() {}
        virtual void setVideoStandard(uint8_t std) { (void)std; }
        virtual VideoSignalInfo getVideoSignalInfo() const { return {}; }
        // CV-23: software replug - reset the FX3 stream DMA (clears its internal
        // FIFO pointers) and re-assert the FPGA streaming bit without
        // re-enumerating the USB device.
        virtual void resetStreamPipeline() {}
    };

    enum class VideoScale {
        Full,
        Half,
        Quarter
    };

    class VideoOutput {
    public:
        virtual ~VideoOutput() = default;

        virtual void initialiseVideo(VideoScale scale) = 0;

        virtual void videoFrameAvailable(uint32_t *image) = 0;

        virtual void videoFrameAvailable(uint32_t *image, uint32_t width, uint32_t height) {
            (void)width;
            (void)height;
            videoFrameAvailable(image);
        }

        virtual void display() = 0;

        virtual void shutdownVideo() = 0;

        virtual void setStatus(const std::string &text) { (void)text; }
        virtual void updateMetrics(const DisplayMetrics &metrics) { (void)metrics; }

    private:
    };

    class AudioOutput {
    public:
        virtual ~AudioOutput() = default;
        virtual void initialiseAudio() = 0;

        virtual void audioFrameAvailable(uint32_t *audio, uint32_t byteLength) = 0;

        virtual void render() = 0;

        virtual void clearAudio() {}

        virtual void shutdownAudio() = 0;
        virtual void setAudioSampleRate(uint32_t sampleRate) { (void)sampleRate; }

    private:
    };

    class Device {
    public:
        Device(Stream *stream, VideoOutput *videoOutput, AudioOutput *audioOutput, Logger *logger, ErrorSink *errorSink);

        bool isDeviceAvailable(DeviceType deviceType);

        void initialise(DeviceType deviceType, VideoScale videoScale);

        void run();

        void shutdown();
        void setVideoInput(VideoInputSource source) {
            if (_stream) {
                _stream->setVideoInput(source);
            }
        }
        void queryVideoSignalStatus() {
            if (_stream) {
                _stream->queryVideoSignalStatus();
            }
        }
        void setVideoStandard(uint8_t std) {
            if (_stream) {
                _stream->setVideoStandard(std);
            }
        }
        // CV-23: a software replug - reset the FX3 stream DMA (clears its
        // internal FIFO pointers), re-assert the FPGA streaming bit, and
        // re-anchor the parser on the next genuine C0. Clears a persistently
        // skewed / misaligned picture without unplugging the cable or
        // restarting the app. Mapped to the R key and fired automatically when
        // the stream stays corrupt while the receiver reports a locked signal.
        void reassertStream();

    private:
        Stream *_stream;
        VideoOutput *_videoOutput;
        AudioOutput *_audioOutput;
        Logger *_logger;
        ErrorSink *_errorSink;

        utils::FrameBuilder _frameBuilder;

        std::function<void(uint8_t *, uint32_t)> _onFrameData;

        // CV710 frame size constants (1 uint32 word = 2 YUY2 pixels)
        static constexpr uint32_t CV710_MAX_FRAME_WORDS   = 1920u * 1200u / 2u;
        static constexpr uint32_t CV710_1080P_FRAME_WORDS = 1920u * 1080u / 2u;
        static constexpr uint32_t CV710_720P_FRAME_WORDS  = 1280u * 720u  / 2u;
        static constexpr uint32_t CV710_576P_FRAME_WORDS  = 720u  * 576u  / 2u;
        static constexpr uint32_t CV710_480P_FRAME_WORDS  = 720u  * 480u  / 2u;
        static constexpr uint32_t MINIMUM_VIDEO_FRAME_WORDS = 80000u;

        struct VideoMode {
            uint32_t width;
            uint32_t height;
            uint32_t targetWords;
            const char *name;
            bool valid;
            bool interlaced;
        };

        VideoMode detectVideoMode(uint32_t frameWords, bool hwInterlaced);

        // CV710 protocol state
        bool _streamLocked{false};
        int _lastSeq{-1};
        uint32_t _droppedFrames{0};
        uint32_t _validFrames{0};

        // CV-01: words that could not be parsed because a marker straddled a USB
        // transfer boundary. Prepended to the next transfer so headers/trailers
        // are never dropped. Bounded: only C0 (1 word), C1 (3 words), the audio
        // header (2 words) and a trailing lock-hunt word (1) can be held back.
        static constexpr uint32_t MAX_PENDING_WORDS = 16;
        std::vector<uint32_t> _pendingWords;

        // CV-14: per-instance diagnostics (previously function-local statics,
        // which leaked state across Device instances/sessions)
        uint32_t _transferCount{0};
        uint32_t _c0HuntCount{0};
        uint32_t _sizeLogCount{0};
        uint32_t _lastLoggedFrameWords{0};
        // Frames dropped because the FPGA flagged the HDMI clock as still
        // recovering (C1 b2 bit 4). A persistent non-zero growth here means the
        // flag is stuck and the drop heuristic should be re-examined.
        uint32_t _syncDropCount{0};
        // Transitional frames dropped because their detected width contradicted a
        // locked ADV7604 receiver geometry (resolution-switch desync guard).
        uint32_t _receiverMismatchDrops{0};
        // CV-23: consecutive un-presentable frames (locked-mode size mismatch or
        // invalid field size) while the receiver reports a locked signal. After
        // kCorruptFramesBeforeAutoResync of them the pipeline is presumed stuck
        // and reassertStream() runs automatically (a software replug).
        uint32_t _skewRun{0};
        uint32_t _skewLogCount{0};
        uint32_t _autoResyncCount{0};
        static constexpr uint32_t kCorruptFramesBeforeAutoResync = 60;

        // Resolution stability hysteresis
        uint32_t _activeWidth{1920};
        uint32_t _activeHeight{1080};
        uint32_t _pendingWidth{0};
        uint32_t _pendingHeight{0};
        uint32_t _pendingCount{0};

        bool _inVideo{false};
        bool _inAudio{false};
        uint32_t _remainingAudioWords{0};
        bool _inAudioPadding{false};

        // Audio muting / video lock tracking
        std::chrono::steady_clock::time_point _lastValidVideoTime{};
        uint32_t _consecutiveValidFrames{0};
        bool _audioMuted{true};

        uint64_t _videoFrameCount{0};
        uint32_t _maxVideoFrameSize{0};
        uint32_t _minVideoFrameSize{UINT32_MAX};
        std::chrono::steady_clock::time_point _fpsTimestamp{};
        std::chrono::steady_clock::time_point _lastSignalCheck{};
        float _measuredFps{60.0f};

        uint8_t _currentFieldFlags{0};
        // CV-04: which field parities have been woven since the last complete
        // frame was emitted (bit 0 = field 0, bit 1 = field 1).
        uint8_t _fieldsWoven{0};
        // CV-04: field parity source. The C0 metadata field-index bit is only
        // trusted once it has been *observed to alternate*; some firmware
        // revisions hold b2 bit 0 constant, in which case we fall back to a
        // local toggle (the behaviour that worked before CV-04) so weaving can
        // still complete. A toggle can only mis-order a frame after a dropped
        // field; the device index, when valid, cannot.
        bool _deviceParityReliable{false};
        int8_t _lastDeviceParity{-1};
        uint8_t _deviceParityChanges{0};
        bool _weaveToggle{false};
        std::vector<uint32_t> _interlacedBuffer;

        void onFrameData(uint8_t *data, uint32_t byteLength);

        // CV-01: hold back words starting at `start` for the next transfer
        void holdWordsForNextTransfer(const uint32_t *words, uint32_t count, uint32_t start);

        // CV-23: drop a frame/field whose word count contradicts the locked
        // geometry (presenting it would put a row-stride "skewed lines" seam in
        // the picture), count it, and arms the automatic software replug once a
        // persistent corrupt streak builds up.
        void noteCorruptFrame(uint32_t frameWords, const char *modeName, const char *reason);

        void produceVideoData(uint32_t frameSize, uint32_t width, uint32_t height, uint8_t *data);

        void produceAudioData(uint8_t *data, uint32_t byteLength);
    };

}

#endif
