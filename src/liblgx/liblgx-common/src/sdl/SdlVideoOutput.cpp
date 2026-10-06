#include "SdlVideoOutput.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>

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

    const char *SdlVideoOutput::colorspaceTitle(ColorspaceMode mode) {
        switch (mode) {
            case ColorspaceMode::BT709_Limited: return "BT.709 Limited Range (Default)";
            case ColorspaceMode::BT709_Full:    return "BT.709 Full Range (PC / Mac)";
            case ColorspaceMode::BT601_Limited: return "BT.601 Limited Range (SDTV)";
            case ColorspaceMode::BT601_Full:    return "BT.601 Full Range (PC SD)";
            case ColorspaceMode::UYVY_Swap:     return "UYVY Chroma Inversion Fix";
            case ColorspaceMode::Direct_YUY2:   return "Direct Hardware YUY2 Passthrough";
            default:                            return "Default Colorspace";
        }
    }

    const char *SdlVideoOutput::colorspaceSubtitle(ColorspaceMode mode) {
        switch (mode) {
            case ColorspaceMode::BT709_Limited: return "Studio [16-235] | Co-sited Chroma";
            case ColorspaceMode::BT709_Full:    return "Full [0-255] | Co-sited Chroma";
            case ColorspaceMode::BT601_Limited: return "Rec.601 studio [16-235] | Co-sited Chroma";
            case ColorspaceMode::BT601_Full:    return "Rec.601 full [0-255] | Co-sited Chroma";
            case ColorspaceMode::UYVY_Swap:     return "Inverted U/Y byte order correction";
            case ColorspaceMode::Direct_YUY2:   return "Raw hardware frame pass-through to GPU";
            default:                            return "";
        }
    }

    const char *SdlVideoOutput::colorspaceShortName(ColorspaceMode mode) {
        switch (mode) {
            case ColorspaceMode::BT709_Limited: return "BT.709";
            case ColorspaceMode::BT709_Full:    return "BT.709 Full";
            case ColorspaceMode::BT601_Limited: return "BT.601";
            case ColorspaceMode::BT601_Full:    return "BT.601 Full";
            case ColorspaceMode::UYVY_Swap:     return "UYVY";
            case ColorspaceMode::Direct_YUY2:   return "YUY2";
            default:                            return "Default";
        }
    }

    void SdlVideoOutput::updateWindowTitle() {
        if (!_window) return;
        char title[128];
        if (_targetScale == lgx2::VideoScale::Full) {
            snprintf(title, sizeof(title), "cv710userspace - %dx%d [%s]",
                     _srcWidth, _srcHeight, colorspaceShortName(_colorspaceMode));
        } else {
            int div = (_targetScale == lgx2::VideoScale::Half) ? 2 : 4;
            snprintf(title, sizeof(title), "cv710userspace - %dx%d (1/%d) [%s]",
                     _srcWidth, _srcHeight, div, colorspaceShortName(_colorspaceMode));
        }
        SDL_SetWindowTitle(_window, title);
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
        _srcWidth = 1920;
        _srcHeight = 1080;
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
        _rgbaCapacity = 1920 * 1080;
        _rgbaBuffer = new uint32_t[_rgbaCapacity];

        updateTextureFormat();
        loadSplashBitmaps();
        updateWindowTitle();

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
        updateWindowTitle();
    }

    void SdlVideoOutput::setColorspace(const std::string &name) {
        setColorspace(parseColorspace(name));
    }

    void SdlVideoOutput::convertYuy2ToRgba(const uint32_t *src, uint32_t *dst, int srcWidth, int dstWidth, int dstHeight, int step) {
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

        const int pairsPerDstRow = dstWidth / 2;
        const int srcStrideWords = srcWidth / 2;

        auto processRows = [&](int y_start, int y_end) {
            for (int y = y_start; y < y_end; y++) {
                const uint32_t *srcRow = src + (y * step) * srcStrideWords;
                uint32_t *dstRow = dst + y * dstWidth;

                for (int x = 0; x < pairsPerDstRow; x++) {
                    uint32_t word = srcRow[x * step];
                    uint8_t y0, u, y1, v;

                    if (!swapChroma) {
                        y0 = word & 0xFF;
                        u  = (word >> 8) & 0xFF;
                        y1 = (word >> 16) & 0xFF;
                        v  = (word >> 24) & 0xFF;
                    } else {
                        u  = word & 0xFF;
                        y0 = (word >> 8) & 0xFF;
                        v  = (word >> 16) & 0xFF;
                        y1 = (word >> 24) & 0xFF;
                    }

                    int u_val = static_cast<int>(u) - 128;
                    int v_val = static_cast<int>(v) - 128;

                    int r_off = cRV * v_val;
                    int g_off = -(cGU * u_val + cGV * v_val);
                    int b_off = cBU * u_val;

                    int y0_scaled = cY * (static_cast<int>(y0) - y_off) + 32768;
                    int y1_scaled = cY * (static_cast<int>(y1) - y_off) + 32768;

                    uint8_t r0 = clamp8((y0_scaled + r_off) >> 16);
                    uint8_t g0 = clamp8((y0_scaled + g_off) >> 16);
                    uint8_t b0 = clamp8((y0_scaled + b_off) >> 16);

                    uint8_t r1 = clamp8((y1_scaled + r_off) >> 16);
                    uint8_t g1 = clamp8((y1_scaled + g_off) >> 16);
                    uint8_t b1 = clamp8((y1_scaled + b_off) >> 16);

                    // SDL_PIXELFORMAT_RGBA32 in memory (little-endian): R, G, B, A
                    dstRow[x * 2]     = 0xFF000000u | (static_cast<uint32_t>(b0) << 16) | (static_cast<uint32_t>(g0) << 8) | r0;
                    dstRow[x * 2 + 1] = 0xFF000000u | (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(g1) << 8) | r1;
                }
            }
        };

        const int numThreads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        if (numThreads > 1 && dstHeight >= numThreads * 4) {
            std::vector<std::thread> workers;
            workers.reserve(numThreads - 1);
            int rowsPerThread = dstHeight / numThreads;
            for (int t = 1; t < numThreads; t++) {
                int y_start = t * rowsPerThread;
                int y_end = (t == numThreads - 1) ? dstHeight : (t + 1) * rowsPerThread;
                workers.emplace_back(processRows, y_start, y_end);
            }
            processRows(0, rowsPerThread);
            for (auto &w : workers) {
                w.join();
            }
        } else {
            processRows(0, dstHeight);
        }
    }

    void SdlVideoOutput::videoFrameAvailable(uint32_t *image) {
        videoFrameAvailable(image, 1920, 1080);
    }

    void SdlVideoOutput::videoFrameAvailable(uint32_t *image, uint32_t width, uint32_t height) {
        if (!image || !_renderer) return;
        _lastFrameTime = std::chrono::steady_clock::now();
        _hasSignal = true;

        int targetW = static_cast<int>(width);
        int targetH = static_cast<int>(height);
        int step = 1;
        if (_targetScale == lgx2::VideoScale::Half) {
            targetW /= 2;
            targetH /= 2;
            step = 2;
        } else if (_targetScale == lgx2::VideoScale::Quarter) {
            targetW /= 4;
            targetH /= 4;
            step = 4;
        }

        if (width != static_cast<uint32_t>(_srcWidth) || height != static_cast<uint32_t>(_srcHeight) ||
            targetW != _texWidth || targetH != _texHeight || !_texture) {
            printf("[Video] Input resolution detected: %ux%u -> output texture %dx%d\n", width, height, targetW, targetH);
            fflush(stdout);
            _srcWidth = static_cast<int>(width);
            _srcHeight = static_cast<int>(height);
            _texWidth = targetW;
            _texHeight = targetH;
            updateTextureFormat();
            updateWindowTitle();
            _showOsd = true;
            _osdTimestamp = _lastFrameTime;
        }

        if (static_cast<size_t>(_texWidth) * _texHeight > _rgbaCapacity) {
            delete[] _rgbaBuffer;
            _rgbaCapacity = static_cast<size_t>(_texWidth) * _texHeight;
            _rgbaBuffer = new uint32_t[_rgbaCapacity];
        }

        if (_colorspaceMode == ColorspaceMode::Direct_YUY2) {
            if (step == 1) {
                if (!SDL_UpdateTexture(_texture, nullptr, image, width * 2)) {
                    fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
                }
                _newFrameAvailable = true;
                return;
            }

            int texW = _texWidth;
            int texH = _texHeight;
            int pairsPerRow = texW / 2;
            int srcStride = width / 2;

            std::vector<uint32_t> buf(pairsPerRow * texH);
            for (int y = 0; y < texH; y++) {
                for (int x = 0; x < pairsPerRow; x++) {
                    buf[y * pairsPerRow + x] = image[(y * step) * srcStride + x * step];
                }
            }
            if (!SDL_UpdateTexture(_texture, nullptr, buf.data(), texW * 2)) {
                fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
            }
            _newFrameAvailable = true;
            return;
        }

        // RGBA software conversion modes
        convertYuy2ToRgba(image, _rgbaBuffer, width, _texWidth, _texHeight, step);

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
                    updateWindowTitle();
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

            // Render sleek modern HUD badge if active
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - _osdTimestamp).count();
            if (_showOsd && elapsedMs < 2500) {
                uint8_t alpha = 255;
                if (elapsedMs > 1800) {
                    float fade = 1.0f - static_cast<float>(elapsedMs - 1800) / 700.0f;
                    if (fade < 0.0f) fade = 0.0f;
                    alpha = static_cast<uint8_t>(255.0f * fade);
                }

                int winW = 1920, winH = 1080;
                SDL_GetWindowSize(_window, &winW, &winH);

                float cardW = 390.0f;
                float cardH = 56.0f;
                float cardX = static_cast<float>(winW) - cardW - 24.0f;
                float cardY = 24.0f;

                SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

                // Main card background (deep translucent obsidian)
                SDL_FRect bgRect{cardX, cardY, cardW, cardH};
                SDL_SetRenderDrawColor(_renderer, 16, 20, 28, static_cast<uint8_t>(alpha * 225 / 255));
                SDL_RenderFillRect(_renderer, &bgRect);

                // Subtle card border
                SDL_SetRenderDrawColor(_renderer, 60, 75, 95, static_cast<uint8_t>(alpha * 180 / 255));
                SDL_RenderRect(_renderer, &bgRect);

                // Accent bar on the left edge
                SDL_FRect accentRect{cardX, cardY, 4.0f, cardH};
                uint8_t accR = 0, accG = 180, accB = 216; // BT709 Limited Cyan
                switch (_colorspaceMode) {
                    case ColorspaceMode::BT709_Full:    accR = 255; accG = 183; accB = 3;   break; // Amber
                    case ColorspaceMode::BT601_Limited: accR = 6;   accG = 214; accB = 160; break; // Emerald
                    case ColorspaceMode::BT601_Full:    accR = 138; accG = 201; accB = 38;  break; // Lime
                    case ColorspaceMode::UYVY_Swap:     accR = 157; accG = 78;  accB = 221; break; // Violet
                    case ColorspaceMode::Direct_YUY2:   accR = 58;  accG = 134; accB = 255; break; // Blue
                    default: break;
                }
                SDL_SetRenderDrawColor(_renderer, accR, accG, accB, alpha);
                SDL_RenderFillRect(_renderer, &accentRect);

                // Line 1: Mode title (rendered at 1.0f scale: crisp native font)
                SDL_SetRenderScale(_renderer, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(_renderer, 255, 255, 255, alpha);
                SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 12.0f, colorspaceTitle(_colorspaceMode));

                // Line 2: Details (source resolution + technical subtitle)
                char detailBuf[160];
                snprintf(detailBuf, sizeof(detailBuf), "%ux%u | %s",
                         _srcWidth, _srcHeight, colorspaceSubtitle(_colorspaceMode));
                SDL_SetRenderDrawColor(_renderer, 160, 185, 215, static_cast<uint8_t>(alpha * 210 / 255));
                SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 34.0f, detailBuf);
            } else if (_showOsd) {
                _showOsd = false;
            }

            SDL_RenderPresent(_renderer);
            _newFrameAvailable = false;
        }
    }

    void SdlVideoOutput::loadSplashBitmaps() {
        if (!_renderer) return;

        std::vector<std::string> searchPaths;
        searchPaths.emplace_back("assets/aver_custom_no_signal.bmp");
        searchPaths.emplace_back("../assets/aver_custom_no_signal.bmp");

        const char *basePath = SDL_GetBasePath();
        if (basePath) {
            std::string base(basePath);
            searchPaths.push_back(base + "assets/aver_custom_no_signal.bmp");
            searchPaths.push_back(base + "../assets/aver_custom_no_signal.bmp");
            searchPaths.push_back(base + "../../assets/aver_custom_no_signal.bmp");
            searchPaths.push_back(base + "../share/cv710userspace/assets/aver_custom_no_signal.bmp");
        }

        searchPaths.emplace_back("/usr/local/share/cv710userspace/assets/aver_custom_no_signal.bmp");
        searchPaths.emplace_back("/usr/share/cv710userspace/assets/aver_custom_no_signal.bmp");

        for (const auto &path : searchPaths) {
            SDL_Surface *surf = SDL_LoadBMP(path.c_str());
            if (surf) {
                _splashTexture = SDL_CreateTextureFromSurface(_renderer, surf);
                SDL_DestroySurface(surf);
                if (_splashTexture) {
                    printf("[Video] Loaded standby splash bitmap: %s\n", path.c_str());
                    fflush(stdout);
                    SDL_SetTextureScaleMode(_splashTexture, SDL_SCALEMODE_LINEAR);
                    break;
                }
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
