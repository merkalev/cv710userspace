
#include <iostream>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <liblgx.h>
#include "OptionParser.h"
#include "../version.h"

bool do_exit = false;

int main(int argc, char **argv) {
    std::cout << "cv710userspace " << APP_VERSION << " ("<< GIT_BRANCH << "-" << GIT_REV << " - " << GIT_TAG << ")" << std::endl;

    app::OptionParser optionParser{};

    if (!optionParser.process(argc, argv)) {
        return 0;
    }

    lgx2::Logger *logger{optionParser.logger()};
    lgx2::VideoOutput *videoOutput{optionParser.videoOutput()};
    lgx2::AudioOutput *audioOutput{optionParser.audioOutput()};
    lgx2::Stream *stream{optionParser.stream()};

    if (stream == nullptr) {
        stream = new libusb::UsbStream{};
    }

    if (videoOutput == nullptr) {
        auto *sdlVideo = new sdl::SdlVideoOutput{};
        if (!optionParser.colorspace().empty()) {
            sdlVideo->setColorspace(optionParser.colorspace());
        }
        videoOutput = sdlVideo;
    }

    if (audioOutput == nullptr) {
        audioOutput = new sdl::SdlAudioOutput{};
    }

    if (logger == nullptr) {
        logger = new NOOPLogger{};
    }

#ifdef __MINGW32__
    lgx2::ErrorSink *errorSink = new error::WindowsErrorSink();
#elifdef __APPLE__
    lgx2::ErrorSink *errorSink = new error::MacOsErrorSink();
#else
    lgx2::ErrorSink *errorSink = new error::SimpleErrorSink();
#endif
    lgx2::Device device{stream, videoOutput, audioOutput, logger, errorSink};

    lgx2::DeviceType targetDevice = optionParser.deviceType();

    device.initialise(targetDevice, optionParser.scale());

    lgx2::VideoInputSource currentSource = optionParser.inputSource();
    device.setVideoInput(currentSource);

    signal(SIGTERM, [](int) {
        do_exit = true;
    });
    signal(SIGINT, [](int) {
        do_exit = true;
    });

    SDL_Event event;

    try {
        while (!do_exit) {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    do_exit = true;
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_P) {
                        device.queryVideoSignalStatus();
                    } else if (event.key.key == SDLK_7) {
                        device.setVideoStandard(0x13); // 720p60
                    } else if (event.key.key == SDLK_1) {
                        device.setVideoStandard(0x06); // 1080p60
                    } else if (event.key.key == SDLK_4) {
                        device.setVideoStandard(0x0A); // 480p60
                    }
                }
            }
            device.run();
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "Fatal: %s\n", e.what());
    }

    device.shutdown();

    logger->summarise();

    delete stream;
    delete videoOutput;
    delete audioOutput;
    delete logger;
    delete errorSink;

    return 0;
}
