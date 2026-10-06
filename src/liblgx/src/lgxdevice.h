#ifndef LGX2USERSPACE_LGXDEVICE_H
#define LGX2USERSPACE_LGXDEVICE_H

#include <functional>
#include <cstdint>
#include <string>
#include <chrono>
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

    class Stream {
    public:
        virtual ~Stream() = default;
        virtual bool deviceAvailable(DeviceType deviceType) = 0;

        virtual void streamSetupCommands(DeviceType deviceType) = 0;

        virtual void queueFrameRead(std::function<void(uint8_t *frameData, uint32_t byteLength)> *onData) = 0;

        virtual void update() = 0;

        virtual void shutdownStream() = 0;

        virtual void setVideoInput(VideoInputSource source) { (void)source; }
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

    private:
        Stream *_stream;
        VideoOutput *_videoOutput;
        AudioOutput *_audioOutput;
        Logger *_logger;
        ErrorSink *_errorSink;

        utils::FrameBuilder _frameBuilder;

        std::function<void(uint8_t *, uint32_t)> _onFrameData;

        // CV710 frame size constants (1 uint32 word = 2 YUY2 pixels)
        static constexpr uint32_t CV710_MAX_FRAME_WORDS   = 1920u * 1080u / 2u;
        static constexpr uint32_t CV710_1080P_FRAME_WORDS = 1920u * 1080u / 2u;
        static constexpr uint32_t CV710_720P_FRAME_WORDS  = 1280u * 720u  / 2u;
        static constexpr uint32_t CV710_576P_FRAME_WORDS  = 720u  * 576u  / 2u;
        static constexpr uint32_t CV710_480P_FRAME_WORDS  = 720u  * 480u  / 2u;
        static constexpr uint32_t MINIMUM_VIDEO_FRAME_WORDS = 80000u;

        // CV710 protocol state
        bool _streamLocked{false};
        int _lastSeq{-1};
        uint32_t _droppedFrames{0};
        uint32_t _validFrames{0};

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

        void onFrameData(uint8_t *data, uint32_t byteLength);

        void produceVideoData(uint32_t frameSize, uint32_t width, uint32_t height, uint8_t *data);

        void produceAudioData(uint8_t *data, uint32_t byteLength);
    };

}

#endif
