#include <cstring>
#include "FrameBuilder.h"

namespace utils {

    inline uint32_t min(uint32_t a, uint32_t b) {
        return (a < b) ? a : b;
    }

    FrameBuilder::FrameBuilder() : _videoFrame{new uint32_t[MAX_VIDEO_WORDS]}, _audioFrame{new uint32_t[MAX_AUDIO_WORDS]},
                                   _videoOffset{0}, _audioOffset{0} {
    }

    FrameBuilder::~FrameBuilder() {
        delete[] _videoFrame;
        delete[] _audioFrame;
        _videoFrame = nullptr;
        _audioFrame = nullptr;
    }

    void FrameBuilder::buildVideo(uint8_t *block, uint32_t len) {
        if (!block || len == 0 || _videoOffset >= MAX_VIDEO_WORDS) return;
        uint32_t space = MAX_VIDEO_WORDS - _videoOffset;
        uint32_t blocksCopied = min(len, space);

        memcpy(_videoFrame + _videoOffset, block, blocksCopied * 4);
        _videoOffset += blocksCopied;
    }

    void FrameBuilder::buildAudio(uint8_t *block, uint32_t len) {
        if (!block || len == 0 || _audioOffset >= MAX_AUDIO_WORDS) return;
        uint32_t blocksUntilCompleteFrame = MAX_AUDIO_WORDS - _audioOffset;
        uint32_t blocksCopied = min(len, blocksUntilCompleteFrame);

        memcpy(_audioFrame + _audioOffset, block, blocksCopied * 4);
        _audioOffset += blocksCopied;
    }

    uint32_t *FrameBuilder::completeVideoFrame() {
        _videoOffset = 0;
        return _videoFrame;
    }

    void FrameBuilder::clearVideo() {
        _videoOffset = 0;
    }

    void FrameBuilder::clearAudio() {
        _audioOffset = 0;
    }

    uint32_t *FrameBuilder::videoFrameData() {
        return _videoFrame;
    }

    uint32_t *FrameBuilder::completeAudioFrame() {
        _audioOffset = 0;
        return _audioFrame;
    }

    uint32_t FrameBuilder::videoFrameSize() {
        return _videoOffset;
    }

    uint32_t FrameBuilder::audioFrameSize() {
        return _audioOffset;
    }

}