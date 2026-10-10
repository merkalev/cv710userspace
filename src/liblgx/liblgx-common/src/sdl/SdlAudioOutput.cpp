#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "SdlAudioOutput.h"

namespace sdl {

    namespace {
        std::string toLower(std::string s) {
            for (char &c: s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        // CV-19: a "loopback" device is a virtual sink whose capture side is what
        // OBS/Discord read from. Covers the ALSA snd-aloop module and the common
        // PipeWire/PulseAudio virtual sink naming.
        bool looksLikeLoopback(const std::string &name) {
            const std::string n = toLower(name);
            return n.find("loopback") != std::string::npos ||
                   n.find("snd-aloop") != std::string::npos ||
                   n.find("aloop") != std::string::npos;
        }

        bool allDigits(const std::string &s) {
            if (s.empty()) return false;
            for (char c: s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            return true;
        }

        // RAII for the array returned by SDL_GetAudioPlaybackDevices().
        struct DeviceList {
            SDL_AudioDeviceID *ids{nullptr};
            int count{0};
            DeviceList() { ids = SDL_GetAudioPlaybackDevices(&count); }
            ~DeviceList() { SDL_free(ids); }
            const char *name(int i) const {
                const char *n = SDL_GetAudioDeviceName(ids[i]);
                return n ? n : "(unnamed)";
            }
        };
    }

    SdlAudioOutput::SdlAudioOutput() {
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    void SdlAudioOutput::setOutputDevice(const std::string &spec) {
        _deviceSpec = spec;
    }

    void SdlAudioOutput::printAudioDevices() {
        SDL_Init(SDL_INIT_AUDIO);
        DeviceList list;
        printf("Audio playback devices:\n");
        printf("  [default] %s\n", SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK));
        for (int i = 0; i < list.count; ++i) {
            printf("  [%d] %s%s\n", i, list.name(i),
                   looksLikeLoopback(list.name(i)) ? "  <-- loopback (capturable)" : "");
        }
        fflush(stdout);
    }

    SDL_AudioDeviceID SdlAudioOutput::resolveDevice() {
        // Explicit --audio-device <index|substring> takes priority.
        if (!_deviceSpec.empty()) {
            DeviceList list;
            if (allDigits(_deviceSpec)) {
                int idx = std::stoi(_deviceSpec);
                if (idx < 0 || idx >= list.count) {
                    throw std::runtime_error("Audio device index " + _deviceSpec + " out of range (0-" +
                                             std::to_string(list.count - 1) + ")");
                }
                _resolvedName = list.name(idx);
                _loopbackRouted = looksLikeLoopback(_resolvedName);
                return list.ids[idx];
            }
            const std::string want = toLower(_deviceSpec);
            for (int i = 0; i < list.count; ++i) {
                if (toLower(list.name(i)).find(want) != std::string::npos) {
                    _resolvedName = list.name(i);
                    _loopbackRouted = looksLikeLoopback(_resolvedName);
                    return list.ids[i];
                }
            }
            throw std::runtime_error("No audio playback device matching '" + _deviceSpec +
                                     "'. Use --list-audio-devices to see the options.");
        }

        // --audio-loopback (or implicit preference in V4L2 mode): pick the first
        // virtual sink. Required mode fails loudly; preferred mode falls back to
        // the desktop default with a hint.
        if (_loopbackPreferred) {
            DeviceList list;
            for (int i = 0; i < list.count; ++i) {
                if (looksLikeLoopback(list.name(i))) {
                    _resolvedName = list.name(i);
                    _loopbackRouted = true;
                    return list.ids[i];
                }
            }
            if (_loopbackRequired) {
                throw std::runtime_error(
                        "No loopback audio device found. Load the ALSA loopback module "
                        "(sudo modprobe snd-aloop) or create a PipeWire virtual sink, then retry "
                        "--audio-loopback. Use --list-audio-devices to inspect what SDL sees.");
            }
            printf("[Audio] No loopback device present; using the desktop default output. "
                   "(Load snd-aloop or a PipeWire virtual sink to capture audio separately.)\n");
            fflush(stdout);
        }

        _resolvedName = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }

    void SdlAudioOutput::initialiseAudio() {
        SDL_AudioSpec spec{};
        spec.freq     = static_cast<int>(_currentSampleRate);
        spec.format   = SDL_AUDIO_S16LE;
        spec.channels = 2;

        const SDL_AudioDeviceID dev = resolveDevice();
        _stream = SDL_OpenAudioDeviceStream(dev, &spec, nullptr, nullptr);
        if (!_stream) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(_stream));

        printf("[Audio] Output device: %s%s\n", _resolvedName.empty() ? "(default)" : _resolvedName.c_str(),
               _loopbackRouted ? "  [loopback]" : "");
        fflush(stdout);
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
