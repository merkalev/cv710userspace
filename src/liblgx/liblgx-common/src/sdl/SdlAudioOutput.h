#ifndef LGX2USERSPACE_SDLAUDIOOUTPUT_H
#define LGX2USERSPACE_SDLAUDIOOUTPUT_H

#include "lgxdevice.h"
#include <SDL3/SDL.h>

namespace sdl {
    class SdlAudioOutput : public lgx2::AudioOutput {
    public:
        SdlAudioOutput();
        ~SdlAudioOutput() override;

        void initialiseAudio() override;

        void audioFrameAvailable(uint32_t *audio, uint32_t byteLength) override;

        void render() override;

        void clearAudio() override;

        void shutdownAudio() override;
        void setAudioSampleRate(uint32_t sampleRate) override;
    private:
        SDL_AudioStream *_stream{nullptr};
        uint32_t _currentSampleRate{48000};
    };
}


#endif //LGX2USERSPACE_SDLAUDIOOUTPUT_H
