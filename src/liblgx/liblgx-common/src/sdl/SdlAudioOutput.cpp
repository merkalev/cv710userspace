#include <stdexcept>
#include "SdlAudioOutput.h"

namespace sdl {

    SdlAudioOutput::SdlAudioOutput() {
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    void SdlAudioOutput::initialiseAudio() {
        SDL_AudioSpec spec{};
        spec.freq     = static_cast<int>(_currentSampleRate);
        spec.format   = SDL_AUDIO_S16LE;
        spec.channels = 2;

        _stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!_stream) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(_stream));
    }

    void SdlAudioOutput::setAudioSampleRate(uint32_t sampleRate) {
        if (sampleRate == 0 || sampleRate == _currentSampleRate) return;
        _currentSampleRate = sampleRate;
        if (_stream) {
            SDL_AudioSpec srcSpec{};
            srcSpec.format = SDL_AUDIO_S16LE;
            srcSpec.channels = 2;
            srcSpec.freq = static_cast<int>(sampleRate);
            SDL_SetAudioStreamFormat(_stream, &srcSpec, nullptr);
            printf("[Audio] Output sample rate dynamically switched to: %u Hz\n", sampleRate);
            fflush(stdout);
        }
    }

    void SdlAudioOutput::audioFrameAvailable(uint32_t *audio, uint32_t byteLength) {
        if (byteLength == 0 || audio == nullptr || !_stream) return;

        // Prevent audio latency and desync build-up during lag spikes:
        // 48 kHz stereo 16-bit = 192 bytes/ms. 60 ms = 11,520 bytes.
        int queued = SDL_GetAudioStreamQueued(_stream);
        if (queued > 12000) {
            SDL_ClearAudioStream(_stream);
        }

        SDL_PutAudioStreamData(_stream, audio, static_cast<int>(byteLength));
    }

    void SdlAudioOutput::render() {
    }

    void SdlAudioOutput::clearAudio() {
        if (_stream) {
            SDL_ClearAudioStream(_stream);
        }
    }

    SdlAudioOutput::~SdlAudioOutput() {
        shutdownAudio();
    }

    void SdlAudioOutput::shutdownAudio() {
        if (_stream) {
            SDL_PauseAudioDevice(SDL_GetAudioStreamDevice(_stream));
            SDL_DestroyAudioStream(_stream);
            _stream = nullptr;
        }
    }
}
