#ifndef LGX2USERSPACE_SDLVIDEOOUTPUT_H
#define LGX2USERSPACE_SDLVIDEOOUTPUT_H

#include "lgxdevice.h"
#include <SDL3/SDL.h>
#include <chrono>
#include <string>

namespace sdl {

    enum class ColorspaceMode {
        BT709_Limited = 0,  // Standard 1080p HDTV (Rec.709 Studio [16-235]) - DEFAULT
        BT709_Full    = 1,  // PC / Mac HDMI Full Range [0-255]
        BT601_Limited = 2,  // SDTV / Rec.601 Limited
        BT601_Full    = 3,  // SDTV / Rec.601 Full Range
        UYVY_Swap     = 4,  // Swapped Chroma / UYVY Mode
        Direct_YUY2   = 5,  // Pass-through raw YUY2 to GPU shader
        Count         = 6
    };

    class SdlVideoOutput : public lgx2::VideoOutput {
    public:
        SdlVideoOutput();
        ~SdlVideoOutput() override;

        void initialiseVideo(lgx2::VideoScale scale) override;

        void videoFrameAvailable(uint32_t *image) override;
        void videoFrameAvailable(uint32_t *image, uint32_t width, uint32_t height) override;

        void display() override;

        void shutdownVideo() override;

        void setStatus(const std::string &text) override;

        void setColorspace(ColorspaceMode mode);
        void setColorspace(const std::string &name);
        ColorspaceMode colorspace() const { return _colorspaceMode; }
        static const char *colorspaceName(ColorspaceMode mode);
        static const char *colorspaceTitle(ColorspaceMode mode);
        static const char *colorspaceSubtitle(ColorspaceMode mode);
        static const char *colorspaceShortName(ColorspaceMode mode);
        static ColorspaceMode parseColorspace(const std::string &name);

    private:
        void updateWindowTitle();
        void renderSplashScreen();
        void updateTextureFormat();
        void loadSplashBitmaps();
        void convertYuy2ToRgba(const uint32_t *src, uint32_t *dst, int srcWidth, int dstWidth, int dstHeight, int step);

        SDL_Window *_window{nullptr};
        SDL_Renderer *_renderer{nullptr};
        SDL_Texture *_texture{nullptr};
        SDL_Texture *_splashTexture{nullptr};
        lgx2::VideoScale _targetScale{lgx2::VideoScale::Full};

        ColorspaceMode _colorspaceMode{ColorspaceMode::BT709_Limited};
        uint32_t *_rgbaBuffer{nullptr};
        size_t _rgbaCapacity{0};
        int _srcWidth{1920};
        int _srcHeight{1080};
        int _texWidth{1920};
        int _texHeight{1080};

        std::chrono::steady_clock::time_point _lastFrameTime{};
        std::chrono::steady_clock::time_point _lastSplashRender{};
        bool _hasSignal{false};
        bool _newFrameAvailable{false};

        // OSD & key toggle state
        bool _cKeyPressed{false};
        std::chrono::steady_clock::time_point _osdTimestamp{};
        bool _showOsd{false};
    };
}

#endif //LGX2USERSPACE_SDLVIDEOOUTPUT_H
