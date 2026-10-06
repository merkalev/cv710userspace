#ifndef LGX2USERSPACE_FRAMEBUILDER_H
#define LGX2USERSPACE_FRAMEBUILDER_H

#include <cstdint>
#include <memory>

namespace utils {
    class FrameBuilder {
    public:
        FrameBuilder();
        ~FrameBuilder();
        void buildVideo(uint8_t *block, uint32_t len);
        void buildAudio(uint8_t *block, uint32_t len);
        uint32_t *completeVideoFrame();
        uint32_t *completeAudioFrame();
        uint32_t *videoFrameData();
        void clearVideo();
        void clearAudio();

        uint32_t videoFrameSize();
        uint32_t audioFrameSize();

        const static uint32_t VIDEO_FRAME_START_MARKER = 0xC0FFFF00;
        const static uint32_t VIDEO_FRAME_END_MARKER   = 0xC1FFFF00;
        const static uint32_t AUDIO_FRAME_START_MARKER = 0x58FFFF00;
        const static uint32_t AUDIO_FRAME_END_MARKER   = 0xAA5555AA;

    private:
        static constexpr uint32_t MAX_VIDEO_WORDS = 1920 * 1080;
        static constexpr uint32_t MAX_AUDIO_WORDS = 8192;

        uint32_t* _videoFrame{nullptr};
        uint32_t* _audioFrame{nullptr};

        uint32_t _videoOffset{0};
        uint32_t _audioOffset{0};
    };
}

#endif //LGX2USERSPACE_FRAMEBUILDER_H
