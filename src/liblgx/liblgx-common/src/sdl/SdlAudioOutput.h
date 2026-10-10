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

        // CV-19: route captured audio to a chosen playback device instead of the
        // desktop default. Used to feed a virtual loopback sink (snd-aloop /
        // PipeWire) so third-party software can capture the audio.
        void setOutputDevice(const std::string &spec);
        void setLoopbackPreferred(bool preferred) { _loopbackPreferred = preferred; }
        void setLoopbackRequired(bool required) { _loopbackRequired = required; }
        bool isLoopbackRouted() const { return _loopbackRouted; }
        const std::string &resolvedDeviceName() const { return _resolvedName; }

        // Print the available playback devices (index, name, loopback marker).
        static void printAudioDevices();

    private:
        SDL_AudioDeviceID resolveDevice();

        SDL_AudioStream *_stream{nullptr};
        uint32_t _currentSampleRate{48000};
        std::string _deviceSpec;          // empty = default playback
        bool _loopbackPreferred{false};
        bool _loopbackRequired{false};
        bool _loopbackRouted{false};
        std::string _resolvedName;
    };
}


#endif //LGX2USERSPACE_SDLAUDIOOUTPUT_H
