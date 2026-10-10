#ifndef LGX2USERSPACE_OPTIONPARSER_H
#define LGX2USERSPACE_OPTIONPARSER_H

#include <liblgx.h>

namespace app {
    class OptionParser {
    public:
        bool process(int argc, char **argv);

        lgx2::VideoOutput *videoOutput();
        lgx2::AudioOutput *audioOutput();
        lgx2::Logger *logger();
        lgx2::DeviceType deviceType();
        lgx2::Stream *stream();

        lgx2::VideoScale scale();
        const std::string &colorspace() const { return _colorspace; }
        lgx2::VideoInputSource inputSource() const { return _inputSource; }
        bool fastBootstrap() const { return _fastBootstrap; }

    private:
        lgx2::VideoOutput *_videoOutput{nullptr};
        lgx2::AudioOutput *_audioOutput{nullptr};
        lgx2::Logger *_logger{nullptr};
        lgx2::DeviceType _deviceType{lgx2::DeviceType::CV710};
        lgx2::Stream *_stream{nullptr};
        lgx2::VideoScale _scale{lgx2::VideoScale::Full};
        std::string _colorspace{""};
        lgx2::VideoInputSource _inputSource{lgx2::VideoInputSource::HDMI};
        bool _fastBootstrap{true};
    };
}


#endif //LGX2USERSPACE_OPTIONPARSER_H
