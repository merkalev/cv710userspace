#include "SdlVideoOutput.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace sdl {

    static inline uint8_t clamp8(int val) {
        if (val < 0) return 0;
        if (val > 255) return 255;
        return static_cast<uint8_t>(val);
    }

    const char *SdlVideoOutput::colorspaceName(ColorspaceMode mode) {
        switch (mode) {
            case ColorspaceMode::BT709_Limited: return "BT.709 Limited (HDTV Standard)";
            case ColorspaceMode::BT709_Full:    return "BT.709 Full (PC/Mac Full Range)";
            case ColorspaceMode::BT601_Limited: return "BT.601 Limited (SDTV Standard)";
            case ColorspaceMode::BT601_Full:    return "BT.601 Full (PC SD Full Range)";
            case ColorspaceMode::UYVY_Swap:     return "UYVY Swap (Chroma Inversion Fix)";
            case ColorspaceMode::Direct_YUY2:   return "Direct YUY2 (Hardware GPU Shader)";
            default:                            return "Unknown";
        }
    }

    ColorspaceMode SdlVideoOutput::parseColorspace(const std::string &name) {
        if (name == "709full" || name == "bt709full" || name == "full") {
            return ColorspaceMode::BT709_Full;
        } else if (name == "601" || name == "bt601") {
            return ColorspaceMode::BT601_Limited;
        } else if (name == "601full" || name == "bt601full") {
            return ColorspaceMode::BT601_Full;
        } else if (name == "uyvy" || name == "swap") {
            return ColorspaceMode::UYVY_Swap;
        } else if (name == "yuy2" || name == "direct") {
            return ColorspaceMode::Direct_YUY2;
        }
        return ColorspaceMode::BT709_Limited;
    }

    SdlVideoOutput::SdlVideoOutput() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(SDL_GetError());
        }
    }

    SdlVideoOutput::~SdlVideoOutput() {
        shutdownVideo();
    }

    void SdlVideoOutput::initialiseVideo(lgx2::VideoScale scale) {
        _targetScale = scale;
        _texWidth = 1920;
        _texHeight = 1080;

        if (scale == lgx2::VideoScale::Half) {
            _texWidth = 1920 / 2;
            _texHeight = 1080 / 2;
        } else if (scale == lgx2::VideoScale::Quarter) {
            _texWidth = 1920 / 4;
            _texHeight = 1080 / 4;
        }

        _window = SDL_CreateWindow("AVerMedia CV710 / ExtremeCap U3", _texWidth, _texHeight, SDL_WINDOW_RESIZABLE);
        if (!_window) {
            throw std::runtime_error(SDL_GetError());
        }

        _renderer = SDL_CreateRenderer(_window, nullptr);
        if (!_renderer) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetRenderVSync(_renderer, 0);

        delete[] _rgbaBuffer;
        _rgbaBuffer = new uint32_t[1920 * 1080];

        updateTextureFormat();
        loadSplashBitmaps();

        SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
        SDL_RenderClear(_renderer);
        SDL_RenderPresent(_renderer);
    }

    void SdlVideoOutput::updateTextureFormat() {
        if (!_renderer) return;
        if (_texture) {
            SDL_DestroyTexture(_texture);
            _texture = nullptr;
        }
        SDL_PixelFormat format = (_colorspaceMode == ColorspaceMode::Direct_YUY2)
                                 ? SDL_PIXELFORMAT_YUY2
                                 : SDL_PIXELFORMAT_RGBA32;
        _texture = SDL_CreateTexture(
                _renderer,
                format,
                SDL_TEXTUREACCESS_STREAMING,
                _texWidth,
                _texHeight);
        if (!_texture) {
            fprintf(stderr, "Failed to create SDL texture: %s\n", SDL_GetError());
        }
    }

    void SdlVideoOutput::setColorspace(ColorspaceMode mode) {
        ColorspaceMode oldMode = _colorspaceMode;
        _colorspaceMode = mode;
        if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
            updateTextureFormat();
        }
    }

    void SdlVideoOutput::setColorspace(const std::string &name) {
        setColorspace(parseColorspace(name));
    }

    void SdlVideoOutput::convertYuy2ToRgba(const uint32_t *src, uint32_t *dst, int width, int height, int step) {
        // Fixed-point 16-bit coefficients (scaled by 65536)
        int cY = 76309;
        int cRV = 117489;
        int cGU = 13975;
        int cGV = 34925;
        int cBU = 138438;
        int y_off = 16;
        bool swapChroma = false;

        switch (_colorspaceMode) {
            case ColorspaceMode::BT709_Full:
                cY = 65536;
                cRV = 103206;
                cGU = 12276;
                cGV = 30679;
                cBU = 121608;
                y_off = 0;
                break;
            case ColorspaceMode::BT601_Limited:
                cY = 76309;
                cRV = 104597;
                cGU = 25675;
                cGV = 53279;
                cBU = 132201;
                y_off = 16;
                break;
            case ColorspaceMode::BT601_Full:
                cY = 65536;
                cRV = 91881;
                cGU = 22554;
                cGV = 46802;
                cBU = 116130;
                y_off = 0;
                break;
            case ColorspaceMode::UYVY_Swap:
                swapChroma = true;
                break;
            case ColorspaceMode::BT709_Limited:
            default:
                break;
        }

        const int pairsPerDstRow = width / 2;
        const int srcStrideWords = 960; // 1920 / 2

        for (int y = 0; y < height; y++) {
            const uint32_t *srcRow = src + (y * step) * srcStrideWords;
            uint32_t *dstRow = dst + y * width;

            for (int x = 0; x < pairsPerDstRow; x++) {
                uint32_t word = srcRow[x * step];
                uint8_t y0, u, y1, v;

                if (!swapChroma) {
                    // Standard YUY2: [Y0, U, Y1, V]
                    y0 = word & 0xFF;
                    u  = (word >> 8) & 0xFF;
                    y1 = (word >> 16) & 0xFF;
                    v  = (word >> 24) & 0xFF;
                } else {
                    // UYVY layout: [U, Y0, V, Y1]
                    u  = word & 0xFF;
                    y0 = (word >> 8) & 0xFF;
                    v  = (word >> 16) & 0xFF;
                    y1 = (word >> 24) & 0xFF;
                }

                // Co-sited chroma reconstruction:
                // Pixel 0 (even) is co-sited with (u0, v0).
                // Pixel 1 (odd) is centered halfway between pair x and pair x+1.
                uint8_t u_next = u;
                uint8_t v_next = v;
                if (x + 1 < pairsPerDstRow) {
                    uint32_t nextWord = srcRow[(x + 1) * step];
                    if (!swapChroma) {
                        u_next = (nextWord >> 8) & 0xFF;
                        v_next = (nextWord >> 24) & 0xFF;
                    } else {
                        u_next = nextWord & 0xFF;
                        v_next = (nextWord >> 16) & 0xFF;
                    }
                }

                int u0_val = static_cast<int>(u) - 128;
                int v0_val = static_cast<int>(v) - 128;
                int u1_val = ((static_cast<int>(u) + static_cast<int>(u_next) + 1) >> 1) - 128;
                int v1_val = ((static_cast<int>(v) + static_cast<int>(v_next) + 1) >> 1) - 128;

                int r0_off = cRV * v0_val;
                int g0_off = -(cGU * u0_val + cGV * v0_val);
                int b0_off = cBU * u0_val;

                int r1_off = cRV * v1_val;
                int g1_off = -(cGU * u1_val + cGV * v1_val);
                int b1_off = cBU * u1_val;

                int y0_scaled = cY * (static_cast<int>(y0) - y_off) + 32768;
                int y1_scaled = cY * (static_cast<int>(y1) - y_off) + 32768;

                uint8_t r0 = clamp8((y0_scaled + r0_off) >> 16);
                uint8_t g0 = clamp8((y0_scaled + g0_off) >> 16);
                uint8_t b0 = clamp8((y0_scaled + b0_off) >> 16);

                uint8_t r1 = clamp8((y1_scaled + r1_off) >> 16);
                uint8_t g1 = clamp8((y1_scaled + g1_off) >> 16);
                uint8_t b1 = clamp8((y1_scaled + b1_off) >> 16);

                // SDL_PIXELFORMAT_RGBA32 in memory (little-endian): R, G, B, A
                dstRow[x * 2]     = 0xFF000000u | (static_cast<uint32_t>(b0) << 16) | (static_cast<uint32_t>(g0) << 8) | r0;
                dstRow[x * 2 + 1] = 0xFF000000u | (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(g1) << 8) | r1;
            }
        }
    }

    void SdlVideoOutput::videoFrameAvailable(uint32_t *image) {
        if (!image || !_texture) return;
        _lastFrameTime = std::chrono::steady_clock::now();
        _hasSignal = true;

        if (_colorspaceMode == ColorspaceMode::Direct_YUY2) {
            if (_targetScale == lgx2::VideoScale::Full) {
                if (!SDL_UpdateTexture(_texture, nullptr, image, 1920 * 2)) {
                    fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
                }
                _newFrameAvailable = true;
                return;
            }

            int texW = _texWidth;
            int texH = _texHeight;
            int pairsPerRow = texW / 2;
            int step = (_targetScale == lgx2::VideoScale::Half) ? 2 : 4;

            std::vector<uint32_t> buf(pairsPerRow * texH);
            for (int y = 0; y < texH; y++) {
                for (int x = 0; x < pairsPerRow; x++) {
                    buf[y * pairsPerRow + x] = image[(y * step) * 960 + x * step];
                }
            }
            if (!SDL_UpdateTexture(_texture, nullptr, buf.data(), texW * 2)) {
                fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
            }
            _newFrameAvailable = true;
            return;
        }

        // RGBA software conversion modes
        int step = 1;
        if (_targetScale == lgx2::VideoScale::Half) step = 2;
        else if (_targetScale == lgx2::VideoScale::Quarter) step = 4;

        convertYuy2ToRgba(image, _rgbaBuffer, _texWidth, _texHeight, step);

        if (!SDL_UpdateTexture(_texture, nullptr, _rgbaBuffer, _texWidth * 4)) {
            fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
        }
        _newFrameAvailable = true;
    }

    void SdlVideoOutput::display() {
        auto now = std::chrono::steady_clock::now();

        // Signal loss detection (> 500ms without a new frame)
        if (_hasSignal && (now - _lastFrameTime > std::chrono::milliseconds(500))) {
            _hasSignal = false;
        }

        const bool *keyboardState = SDL_GetKeyboardState(nullptr);
        if (keyboardState != nullptr) {
            if (keyboardState[SDL_SCANCODE_F]) {
                SDL_SetWindowFullscreen(_window, true);
            } else if (keyboardState[SDL_SCANCODE_G]) {
                SDL_SetWindowFullscreen(_window, false);
            }

            // 'C' key: cycle colorspace mode (debounced)
            if (keyboardState[SDL_SCANCODE_C]) {
                if (!_cKeyPressed) {
                    _cKeyPressed = true;
                    ColorspaceMode oldMode = _colorspaceMode;
                    _colorspaceMode = static_cast<ColorspaceMode>(
                        (static_cast<int>(_colorspaceMode) + 1) % static_cast<int>(ColorspaceMode::Count));
                    if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
                        updateTextureFormat();
                    }
                    _showOsd = true;
                    _osdTimestamp = now;
                    printf("[Video] Switched colorspace to: %s\n", colorspaceName(_colorspaceMode));
                    fflush(stdout);
                }
            } else {
                _cKeyPressed = false;
            }
        }

        if (!_hasSignal) {
            if (now - _lastSplashRender >= std::chrono::milliseconds(33)) {
                _lastSplashRender = now;
                renderSplashScreen();
            }
            return;
        }

        if (_newFrameAvailable || _showOsd) {
            SDL_RenderTexture(_renderer, _texture, nullptr, nullptr);

            // Render OSD banner if active
            if (_showOsd && (now - _osdTimestamp < std::chrono::milliseconds(2500))) {
                int winW = 1920, winH = 1080;
                SDL_GetWindowSize(_window, &winW, &winH);

                float barW = 520.0f;
                float barH = 46.0f;
                float barX = 24.0f;
                float barY = static_cast<float>(winH) - barH - 24.0f;

                SDL_FRect bgRect{barX, barY, barW, barH};
                SDL_SetRenderDrawColor(_renderer, 15, 20, 28, 220);
                SDL_RenderFillRect(_renderer, &bgRect);

                SDL_SetRenderDrawColor(_renderer, 33, 150, 243, 255);
                SDL_RenderRect(_renderer, &bgRect);

                SDL_SetRenderScale(_renderer, 1.5f, 1.5f);
                SDL_SetRenderDrawColor(_renderer, 255, 255, 255, 255);
                char buf[128];
                snprintf(buf, sizeof(buf), "Colorspace: %s", colorspaceName(_colorspaceMode));
                SDL_RenderDebugText(_renderer, (barX + 16.0f) / 1.5f, (barY + 14.0f) / 1.5f, buf);
                SDL_SetRenderScale(_renderer, 1.0f, 1.0f);
            } else if (_showOsd) {
                _showOsd = false;
            }

            SDL_RenderPresent(_renderer);
            _newFrameAvailable = false;
        }
    }

    void SdlVideoOutput::loadSplashBitmaps() {
        if (!_renderer) return;
        const char *paths[] = {
            "assets/aver_custom_no_signal.bmp",
            "../assets/aver_custom_no_signal.bmp",
            "/usr/local/share/cv710userspace/assets/aver_custom_no_signal.bmp",
            "/usr/share/cv710userspace/assets/aver_custom_no_signal.bmp"
        };
        for (const char *path : paths) {
            SDL_Surface *surf = SDL_LoadBMP(path);
            if (surf) {
                _splashTexture = SDL_CreateTextureFromSurface(_renderer, surf);
                SDL_DestroySurface(surf);
                if (_splashTexture) {
                    printf("[Video] Loaded official driver splash bitmap: %s\n", path);
                    fflush(stdout);
                }
                break;
            }
        }
    }

    void SdlVideoOutput::renderSplashScreen() {
        int winW = 1920, winH = 1080;
        SDL_GetWindowSize(_window, &winW, &winH);

        SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
        SDL_RenderClear(_renderer);

        if (_splashTexture) {
            // Render the official 640x480 AVerMedia splash image centered on black background
            const int splashW = 640;
            const int splashH = 480;
            float dstX = (static_cast<float>(winW) - splashW) / 2.0f;
            float dstY = (static_cast<float>(winH) - splashH) / 2.0f;
            SDL_FRect dstRect{dstX, dstY, static_cast<float>(splashW), static_cast<float>(splashH)};
            SDL_RenderTexture(_renderer, _splashTexture, nullptr, &dstRect);
        } else {
            // Procedural fallback card on black background
            float cardW = 720.0f;
            float cardH = 380.0f;
            float cardX = (static_cast<float>(winW) - cardW) / 2.0f;
            float cardY = (static_cast<float>(winH) - cardH) / 2.0f;

            SDL_FRect cardRect{cardX, cardY, cardW, cardH};
            SDL_SetRenderDrawColor(_renderer, 24, 32, 47, 255);
            SDL_RenderFillRect(_renderer, &cardRect);

            SDL_SetRenderDrawColor(_renderer, 25, 118, 210, 255);
            SDL_RenderRect(_renderer, &cardRect);

            SDL_FRect barRect{cardX, cardY, cardW, 6.0f};
            SDL_SetRenderDrawColor(_renderer, 33, 150, 243, 255);
            SDL_RenderFillRect(_renderer, &barRect);

            SDL_SetRenderScale(_renderer, 2.0f, 2.0f);
            SDL_SetRenderDrawColor(_renderer, 220, 230, 242, 255);
            SDL_RenderDebugText(_renderer, (cardX + 40.0f) / 2.0f, (cardY + 35.0f) / 2.0f, "AVerMedia ExtremeCap U3 (CV710)");

            SDL_SetRenderScale(_renderer, 3.0f, 3.0f);
            SDL_SetRenderDrawColor(_renderer, 255, 179, 0, 255);
            SDL_RenderDebugText(_renderer, (cardX + 40.0f) / 3.0f, (cardY + 95.0f) / 3.0f, "[ NO SIGNAL ]");

            SDL_SetRenderScale(_renderer, 1.5f, 1.5f);
            SDL_SetRenderDrawColor(_renderer, 160, 174, 192, 255);
            SDL_RenderDebugText(_renderer, (cardX + 40.0f) / 1.5f, (cardY + 175.0f) / 1.5f, "Waiting for 1080p HDMI video input...");
            SDL_RenderDebugText(_renderer, (cardX + 40.0f) / 1.5f, (cardY + 210.0f) / 1.5f, "Audio output muted until video sync locks.");
        }

        SDL_SetRenderScale(_renderer, 1.0f, 1.0f);
        SDL_RenderPresent(_renderer);
    }

    void SdlVideoOutput::shutdownVideo() {
        delete[] _rgbaBuffer;
        _rgbaBuffer = nullptr;

        if (_splashTexture) {
            SDL_DestroyTexture(_splashTexture);
            _splashTexture = nullptr;
        }
        if (_texture) {
            SDL_DestroyTexture(_texture);
            _texture = nullptr;
        }
        if (_renderer) {
            SDL_DestroyRenderer(_renderer);
            _renderer = nullptr;
        }
        if (_window) {
            SDL_DestroyWindow(_window);
            _window = nullptr;
        }
    }

    void SdlVideoOutput::setStatus(const std::string &text) {
        if (_window) SDL_SetWindowTitle(_window, ("AVerMedia CV710 | " + text).c_str());
    }
}
