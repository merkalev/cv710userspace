#include <getopt.h>
#include <iostream>
#include "OptionParser.h"

lgx2::VideoOutput *app::OptionParser::videoOutput() {
    return _videoOutput;
}

lgx2::AudioOutput *app::OptionParser::audioOutput() {
    return _audioOutput;
}

lgx2::Logger *app::OptionParser::logger() {
    return _logger;
}

lgx2::Stream *app::OptionParser::stream() {
    return _stream;
}

lgx2::DeviceType app::OptionParser::deviceType() {
    return _deviceType;
}

lgx2::VideoScale app::OptionParser::scale() {
    return _scale;
}

bool app::OptionParser::process(int argc, char **argv) {
    for(;;)
    {
        // CV-15/fast-start: long aliases so the trimmed bootstrap can be
        // selected explicitly. Default is the full capture.
        static struct option longOptions[] = {
            {"fast-start",     no_argument, 0, 'b'},
            {"full-bootstrap", no_argument, 0, 'B'},
            {0, 0, 0, 0}
        };
        switch(getopt_long(argc, argv, "vVd:hsgfS:c:i:bB", longOptions, nullptr))
        {
#ifndef __MINGW32__
#ifndef __APPLE__
            case 'd':
                std::cout << "Attempting to output to V4L2Loopback device: " << optarg << std::endl;
                _videoOutput = new v4l::V4LFrameOutput(optarg);
                continue;
#endif
#endif
            case 'f':
                std::cout << "Using fake USB stream - reading data from 'dump.bin'" << std::endl;
                _stream = new FakeUsbStream();
                continue;
            case 'v':
                std::cout <<"Logging diagnostics information at end of execution" << std::endl;
                _logger = new ChronoLogger(true);
                continue;
            case 'V':
                std::cout <<"Logging diagnostics information - with output during execution " << std::endl;
                _logger = new ChronoLogger(false);
                continue;
            case 's':
                _videoOutput = new NullVideoOutput();
                continue;
            case 'b':
                _fastBootstrap = true;
                std::cout << "Using fast bootstrap (I2C status polls and poll-loop "
                             "duplicates removed; same final register state)" << std::endl;
                continue;
            case 'B':
                _fastBootstrap = false;
                std::cout << "Using full bootstrap (complete capture replay)" << std::endl;
                continue;
            case 'g':
                _audioOutput = new NullAudioOutput();
                continue;
            case 'c':
                _colorspace = optarg;
                std::cout << "Setting initial colorspace to: " << _colorspace << std::endl;
                continue;
            case 'i': {
                std::string inp = optarg;
                if (inp == "component" || inp == "ypbpr") {
                    std::cout << "Notice: Component (YPbPr) input is unsupported. Using HDMI." << std::endl;
                } else {
                    std::cout << "Setting initial video input to: HDMI" << std::endl;
                }
                _inputSource = lgx2::VideoInputSource::HDMI;
                continue;
            }
            case 'S':
                std::cout << "Setting output scaling to: 1/" << optarg << std::endl;
                if (std::string("2") == optarg) {
                    _scale = lgx2::VideoScale::Half;
                } else if (std::string("4") == optarg) {
                    _scale = lgx2::VideoScale::Quarter;
                }
                continue;
            case 'h':
            default :
                std::cout << argv[0] <<
                    " usage:\n"
                    "\t-h\tPrint this usage message\n"
                    "\t-v\tPrint diagnostics information summary at end of execution, useful when submitting bugs\n"
                    "\t-V\tPrint diagnostic information during execution\n"
#ifndef __MINGW32__
#ifndef __APPLE__
                    "\t-d V4L2LoopbackDevice\tSpecify the V4L2Loopback device to output video to (e.g. /dev/video99)\n"
#endif
#endif
                    "\t-s Output only sound\n"
                    "\t-g Output video only\n"
                    "\t-f Use a fake USB stream containing unprocessed frames from a dump.bin file\n"
                    "\t-S SCALE\tSpecify the output scaling (1, 2, 4)\n"
                    "\t-c COLORSPACE\tSpecify initial colorspace (auto, bt709, bt709full, bt601, bt601full, uyvy, yuy2)\n"
                    "\t-i INPUT\tSpecify initial video input source (hdmi)\n"
                    "\t-b, --fast-start\tUse the trimmed bootstrap (default; same final register state)\n"
                    "\t-B, --full-bootstrap\tForce the full capture replay (slow: ~7100 transfers)\n";
                return false;
            case -1:
                break;
        }

        break;
    }

    return true;
}
