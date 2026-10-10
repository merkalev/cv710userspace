#ifndef LGX2USERSPACE_V4LFRAMEOUTPUT_H
#define LGX2USERSPACE_V4LFRAMEOUTPUT_H

#include <cstdint>
#include <string>
#include <lgxdevice.h>

namespace v4l {
    class V4LFrameOutput : public lgx2::VideoOutput {
    public:
        explicit V4LFrameOutput(const std::string &deviceName);

        void initialiseVideo(lgx2::VideoScale scale) override;

        void videoFrameAvailable(uint32_t *image) override;
        void videoFrameAvailable(uint32_t *image, uint32_t width, uint32_t height) override;

        void display() override;

        void shutdownVideo() override;

    private:
        // Negotiate the V4L2 output format for the given geometry. Returns true
        // when the device is (now) configured for that size. Safe to call
        // repeatedly; it is a no-op when the size is unchanged.
        bool negotiateFormat(uint32_t width, uint32_t height);

        int _v4l2fd{-1};

        // Buffer sized for the largest supported (1080p YUY2) frame; frames are
        // copied into it so a slow consumer can never alias the parser buffer.
        uint8_t *_frameBuffer{nullptr};
        size_t  _bufferCapacity{0};

        // Currently negotiated geometry / payload size.
        uint32_t _width{0};
        uint32_t _height{0};
        size_t   _frameBytes{0};

        // CV-17: only write a frame to the loopback device when a new one has
        // actually arrived, instead of on every main-loop iteration.
        bool     _newFrame{false};

        uint64_t _droppedMismatch{0};
    };
}


#endif //LGX2USERSPACE_V4LFRAMEOUTPUT_H
