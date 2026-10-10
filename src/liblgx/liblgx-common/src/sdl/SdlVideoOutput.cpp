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

    namespace {
        // ADV7604 HDMI map 0x53 low nibble (Linux adv7604.c hdmi_color_space_txt):
        //   0 RGB limited, 1 RGB full, 2 YCbCr601 limited, 3 YCbCr709 limited,
        //   6 YCbCr601 full, 7 YCbCr709 full, others xvYCC/sYCC/opRGB/invalid.
        // The FPGA delivers YUY2, so this selects the inverse matrix + luma range.
        ColorspaceMode autoColorspaceForCode(uint8_t code, int srcHeight) {
            bool fullRange;
            switch (code & 0x0F) {
                case 0x1:            // RGB full range
                case 0x6:            // YCbCr 601 full range
                case 0x7:            // YCbCr 709 full range
                    fullRange = true;
                    break;
                default:             // RGB limited / YCbCr limited / other
                    fullRange = false;
                    break;
            }
            if (srcHeight <= 576) {
                return fullRange ? ColorspaceMode::BT601_Full : ColorspaceMode::BT601_Limited;
            }
            return fullRange ? ColorspaceMode::BT709_Full : ColorspaceMode::BT709_Limited;
        }

        bool aviIsRgb(uint8_t code) {
            code &= 0x0F;
            return code == 0x0 || code == 0x1;
        }

        const char *autoColorspaceSourceLabel(uint8_t code) {
            switch (code & 0x0F) {
                case 0x0: return "RGB limited";
                case 0x1: return "RGB full";
                case 0x2: return "YCbCr 4:2:2";
                case 0x3: return "YCbCr 4:2:2";
                case 0x6: return "YCbCr full";
                case 0x7: return "YCbCr full";
                default:  return "YCbCr";
            }
        }
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
        char title[160];
        const char *modeTag = _colorspaceUserOverride ? "" : " [Auto]";
        if (_targetScale == lgx2::VideoScale::Full) {
            snprintf(title, sizeof(title), "cv710userspace - %dx%d [%s%s]",
                     _srcWidth, _srcHeight, colorspaceShortName(_colorspaceMode), modeTag);
        } else {
            int div = (_targetScale == lgx2::VideoScale::Half) ? 2 : 4;
            snprintf(title, sizeof(title), "cv710userspace - %dx%d (1/%d) [%s%s]",
                     _srcWidth, _srcHeight, div, colorspaceShortName(_colorspaceMode), modeTag);
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
        _colorspaceUserOverride = true;
        if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
            updateTextureFormat();
        }
        updateWindowTitle();
    }

    void SdlVideoOutput::setColorspace(const std::string &name) {
        if (name == "auto") {
            _colorspaceUserOverride = false;
            ColorspaceMode autoMode = autoColorspaceForCode(_metrics.signalInfo.aviColorspace, _srcHeight);
            ColorspaceMode oldMode = _colorspaceMode;
            _colorspaceMode = autoMode;
            if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
                updateTextureFormat();
            }
            updateWindowTitle();
        } else {
            setColorspace(parseColorspace(name));
            _colorspaceUserOverride = true;
        }
    }

    void SdlVideoOutput::updateMetrics(const lgx2::DisplayMetrics &metrics) {
        _metrics = metrics;

        if (!_colorspaceUserOverride) {
            ColorspaceMode autoMode = autoColorspaceForCode(metrics.signalInfo.aviColorspace, _srcHeight);

            if (autoMode != _colorspaceMode) {
                ColorspaceMode oldMode = _colorspaceMode;
                _colorspaceMode = autoMode;
                if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
                    updateTextureFormat();
                }
                updateWindowTitle();
                _showOsd = true;
                _osdTimestamp = std::chrono::steady_clock::now();
                printf("[Video] Auto-selected colorspace from AVI InfoFrame: %s (%s)\n",
                       colorspaceName(_colorspaceMode),
                       autoColorspaceSourceLabel(metrics.signalInfo.aviColorspace));
                fflush(stdout);
            }
        }
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

                    // CV-09: co-sited chroma reconstruction as documented in ISSUES.md.
                    // Chroma sits on the even luma pixel (Y0); the odd pixel (Y1) uses
                    // chroma linearly interpolated halfway to the next macropixel's.
                    // At the end of a row there is no next macropixel, so the current
                    // chroma is replicated.
                    uint8_t uNext = u, vNext = v;
                    if (x + 1 < pairsPerDstRow) {
                        uint32_t nextWord = srcRow[(x + 1) * step];
                        if (!swapChroma) {
                            uNext = (nextWord >> 8) & 0xFF;
                            vNext = (nextWord >> 24) & 0xFF;
                        } else {
                            uNext = nextWord & 0xFF;
                            vNext = (nextWord >> 16) & 0xFF;
                        }
                    }

                    int u_val = static_cast<int>(u) - 128;
                    int v_val = static_cast<int>(v) - 128;
                    int u_valOdd = (static_cast<int>(u) + static_cast<int>(uNext) + 1) / 2 - 128;
                    int v_valOdd = (static_cast<int>(v) + static_cast<int>(vNext) + 1) / 2 - 128;

                    int r_off  = cRV * v_val;
                    int g_off  = -(cGU * u_val + cGV * v_val);
                    int b_off  = cBU * u_val;

                    int r_off1 = cRV * v_valOdd;
                    int g_off1 = -(cGU * u_valOdd + cGV * v_valOdd);
                    int b_off1 = cBU * u_valOdd;

                    int y0_scaled = cY * (static_cast<int>(y0) - y_off) + 32768;
                    int y1_scaled = cY * (static_cast<int>(y1) - y_off) + 32768;

                    uint8_t r0 = clamp8((y0_scaled + r_off) >> 16);
                    uint8_t g0 = clamp8((y0_scaled + g_off) >> 16);
                    uint8_t b0 = clamp8((y0_scaled + b_off) >> 16);

                    uint8_t r1 = clamp8((y1_scaled + r_off1) >> 16);
                    uint8_t g1 = clamp8((y1_scaled + g_off1) >> 16);
                    uint8_t b1 = clamp8((y1_scaled + b_off1) >> 16);

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
        if (_firstFrameTime == std::chrono::steady_clock::time_point{}) {
            _firstFrameTime = _lastFrameTime;
        }

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
            if (!_colorspaceUserOverride) {
                _colorspaceMode = autoColorspaceForCode(_metrics.signalInfo.aviColorspace, _srcHeight);
            }
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

            // Tab or O key: toggle persistent diagnostic HUD
            if (keyboardState[SDL_SCANCODE_TAB] || keyboardState[SDL_SCANCODE_O]) {
                if (!_hudTogglePressed) {
                    _hudTogglePressed = true;
                    _hudPersistent = !_hudPersistent;
                    _showOsd = true;
                    _osdTimestamp = now;
                    printf("[Video] Diagnostic HUD: %s\n", _hudPersistent ? "ON" : "OFF");
                    fflush(stdout);
                }
            } else {
                _hudTogglePressed = false;
            }

            // 'C' key: cycle colorspace mode (debounced)
            if (keyboardState[SDL_SCANCODE_C]) {
                if (!_cKeyPressed) {
                    _cKeyPressed = true;
                    ColorspaceMode oldMode = _colorspaceMode;

                    if (!_colorspaceUserOverride) {
                        // Switch from Auto to manual BT709 Limited
                        _colorspaceUserOverride = true;
                        _colorspaceMode = ColorspaceMode::BT709_Limited;
                    } else if (_colorspaceMode == ColorspaceMode::Direct_YUY2) {
                        // Cycled past all manual modes: return to Auto
                        _colorspaceUserOverride = false;
                        _colorspaceMode = autoColorspaceForCode(_metrics.signalInfo.aviColorspace, _srcHeight);
                    } else {
                        // Next manual mode
                        _colorspaceMode = static_cast<ColorspaceMode>(static_cast<int>(_colorspaceMode) + 1);
                        _colorspaceUserOverride = true;
                    }

                    if ((oldMode == ColorspaceMode::Direct_YUY2) != (_colorspaceMode == ColorspaceMode::Direct_YUY2)) {
                        updateTextureFormat();
                    }
                    updateWindowTitle();
                    _showOsd = true;
                    _osdTimestamp = now;

                    if (!_colorspaceUserOverride) {
                        printf("[Video] Switched colorspace to: Auto (HDMI InfoFrame: %s)\n", colorspaceName(_colorspaceMode));
                    } else {
                        printf("[Video] Switched colorspace to: %s (Manual Override)\n", colorspaceName(_colorspaceMode));
                    }
                    fflush(stdout);
                }
            } else {
                _cKeyPressed = false;
            }
        }

        uint8_t hudAlpha = 0;
        if (_hudPersistent) {
            hudAlpha = 255;
        } else if (_showOsd) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - _osdTimestamp).count();
            if (elapsedMs < 2500) {
                if (elapsedMs <= 1800) {
                    hudAlpha = 255;
                } else {
                    float fade = 1.0f - static_cast<float>(elapsedMs - 1800) / 700.0f;
                    if (fade < 0.0f) fade = 0.0f;
                    hudAlpha = static_cast<uint8_t>(255.0f * fade);
                }
            } else {
                _showOsd = false;
            }
        }

        // CV-18b: hold the standby screen until the receiver reports a *valid,
        // locked* signal with known geometry for a short settle period. Frames
        // can start flowing before the ADV7604 has finished HDMI negotiation and
        // those free-run / transitional frames render as a brief blue flash.
        // Masking them behind the standby image avoids showing garbage between
        // the splash and the real picture.
        const lgx2::VideoSignalInfo &si = _metrics.signalInfo;
        const bool signalReady = _hasSignal && si.valid && si.locked && si.activeWidth > 0;
        if (signalReady) {
            if (_signalReadySince == std::chrono::steady_clock::time_point{}) {
                _signalReadySince = now;
            }
        } else {
            _signalReadySince = std::chrono::steady_clock::time_point{};
        }
        const bool settled = signalReady &&
            (now - _signalReadySince) >= std::chrono::milliseconds(200);

        // Safety valve: if the status channel never validates but frames keep
        // arriving, never hide the preview permanently. It must NOT fire when the
        // receiver has a valid status that says "unlocked" - the CV710 keeps
        // emitting free-run frames after the source goes away, and those must be
        // masked by the standby screen (regression fixed: previously this valve
        // kept showing the frozen/free-run picture when the TV box was switched
        // off).
        bool forceShow = false;
        if (!settled && !si.valid && _hasSignal &&
            _firstFrameTime != std::chrono::steady_clock::time_point{} &&
            (now - _firstFrameTime) > std::chrono::seconds(3)) {
            forceShow = true;
        }

        const bool noRealSignal = !(settled || forceShow);

        if (noRealSignal) {
            if (now - _lastSplashRender >= std::chrono::milliseconds(33)) {
                _lastSplashRender = now;
                renderSplashScreen();
                if (hudAlpha > 0) {
                    renderDiagnosticHud(hudAlpha);
                }
                SDL_RenderPresent(_renderer);
            }
            _wasShowingSplash = true;
            return;
        }

        // CV-18: the first live frame after the standby screen is cross-faded in
        // rather than popping abruptly over it.
        if (_wasShowingSplash) {
            _wasShowingSplash = false;
            _videoFadeActive = true;
            _videoFadeStart = now;
        }

        bool shouldRender = _newFrameAvailable;
        if (!shouldRender && hudAlpha > 0 && (now - _lastHudRender >= std::chrono::milliseconds(33))) {
            shouldRender = true;
            _lastHudRender = now;
        }

        if (shouldRender) {
            uint8_t videoAlpha = 255;
            if (_videoFadeActive) {
                auto fadeMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - _videoFadeStart).count();
                if (fadeMs >= 220) {
                    _videoFadeActive = false;
                } else {
                    videoAlpha = static_cast<uint8_t>(255 * fadeMs / 220);
                }
            }

            SDL_BlendMode prevBlend = SDL_BLENDMODE_NONE;
            SDL_GetTextureBlendMode(_texture, &prevBlend);
            if (videoAlpha < 255) {
                SDL_SetTextureBlendMode(_texture, SDL_BLENDMODE_BLEND);
            }
            SDL_SetTextureAlphaMod(_texture, videoAlpha);
            SDL_RenderTexture(_renderer, _texture, nullptr, nullptr);
            if (prevBlend != SDL_BLENDMODE_BLEND) {
                SDL_SetTextureBlendMode(_texture, prevBlend);
            }

            if (hudAlpha > 0) {
                renderDiagnosticHud(hudAlpha);
            }

            SDL_RenderPresent(_renderer);
            _newFrameAvailable = false;
        }
    }

    void SdlVideoOutput::renderDiagnosticHud(uint8_t alpha) {
        if (!_renderer || alpha == 0) return;

        int winW = 1920, winH = 1080;
        SDL_GetWindowSize(_window, &winW, &winH);

        float cardW = 440.0f;
        float cardH = 118.0f;
        float cardX = static_cast<float>(winW) - cardW - 20.0f;
        if (cardX < 10.0f) cardX = 10.0f;
        float cardY = 20.0f;

        SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

        // Main card background (deep translucent obsidian)
        SDL_FRect bgRect{cardX, cardY, cardW, cardH};
        SDL_SetRenderDrawColor(_renderer, 14, 18, 25, static_cast<uint8_t>(alpha * 230 / 255));
        SDL_RenderFillRect(_renderer, &bgRect);

        // Subtle card border (slate grey)
        SDL_SetRenderDrawColor(_renderer, 55, 70, 90, static_cast<uint8_t>(alpha * 200 / 255));
        SDL_RenderRect(_renderer, &bgRect);

        // Accent bar on left edge reflecting colorspace mode
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

        // Subtle divider line under header
        SDL_FRect sepRect{cardX + 14.0f, cardY + 25.0f, cardW - 28.0f, 1.0f};
        SDL_SetRenderDrawColor(_renderer, 45, 58, 75, static_cast<uint8_t>(alpha * 180 / 255));
        SDL_RenderFillRect(_renderer, &sepRect);

        // Header Left: Device title
        SDL_SetRenderScale(_renderer, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(_renderer, 240, 245, 255, alpha);
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 11.0f, "AVerMedia CV710 (ExtremeCap U3)");

        // Header Right: Status badge. Prefer the ADV7604 lock state (authoritative
        // for "is there really an HDMI signal?") over mere frame arrival, which can
        // be the receiver's free-run pattern when nothing is connected.
        if (_metrics.signalInfo.valid && _metrics.signalInfo.locked) {
            SDL_SetRenderDrawColor(_renderer, 76, 214, 100, alpha);
            SDL_RenderDebugText(_renderer, cardX + cardW - 120.0f, cardY + 11.0f, "[ TMDS LOCK ]");
        } else if (_metrics.signalInfo.valid) {
            SDL_SetRenderDrawColor(_renderer, 255, 170, 50, alpha);
            SDL_RenderDebugText(_renderer, cardX + cardW - 120.0f, cardY + 11.0f, "[ NO SIGNAL ]");
        } else if (_hasSignal) {
            SDL_SetRenderDrawColor(_renderer, 0, 200, 255, alpha);
            SDL_RenderDebugText(_renderer, cardX + cardW - 120.0f, cardY + 11.0f, "[ STREAMING ]");
        } else {
            SDL_SetRenderDrawColor(_renderer, 255, 170, 50, alpha);
            SDL_RenderDebugText(_renderer, cardX + cardW - 120.0f, cardY + 11.0f, "[ NO SIGNAL ]");
        }

        // Line 1: Video mode (Resolution and hardware refresh rate)
        char videoBuf[128];
        float hwFps = _metrics.signalInfo.measuredFps > 0.0f ? _metrics.signalInfo.measuredFps : 60.0f;
        snprintf(videoBuf, sizeof(videoBuf), "Video: %ux%u%s @ %.2f Hz",
                 _srcWidth, _srcHeight,
                 _metrics.signalInfo.interlaced ? "i" : "p",
                 hwFps);
        SDL_SetRenderDrawColor(_renderer, 180, 215, 245, static_cast<uint8_t>(alpha * 220 / 255));
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 31.0f, videoBuf);

        // Line 2: Colorspace mode and source info
        char colorBuf[128];
        const char *csTag = _colorspaceUserOverride ? "Manual" :
            (aviIsRgb(_metrics.signalInfo.aviColorspace) ? "Auto: RGB" : "Auto: YCbCr");
        snprintf(colorBuf, sizeof(colorBuf), "Color: %s (%s)",
                 colorspaceShortName(_colorspaceMode), csTag);
        SDL_SetRenderDrawColor(_renderer, 210, 225, 240, static_cast<uint8_t>(alpha * 220 / 255));
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 47.0f, colorBuf);

        // Line 3: Audio status and sample rate
        char audioBuf[128];
        float audKhz = static_cast<float>(_metrics.signalInfo.audioSampleRate) / 1000.0f;
        snprintf(audioBuf, sizeof(audioBuf), "Audio: %.1f kHz Stereo PCM (%s)",
                 audKhz > 0.0f ? audKhz : 48.0f,
                 _metrics.signalInfo.audioLocked ? "Locked" : "Unlocked");
        if (_metrics.signalInfo.audioLocked) {
            SDL_SetRenderDrawColor(_renderer, 120, 220, 160, static_cast<uint8_t>(alpha * 220 / 255));
        } else {
            SDL_SetRenderDrawColor(_renderer, 220, 170, 110, static_cast<uint8_t>(alpha * 200 / 255));
        }
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 63.0f, audioBuf);

        // Line 4: Host capture performance (FPS, valid frames, drops)
        char perfBuf[128];
        snprintf(perfBuf, sizeof(perfBuf), "Capture: %.1f fps | Valid: %u | Drops: %u",
                 _metrics.liveFps, _metrics.validFrames, _metrics.droppedFrames);
        SDL_SetRenderDrawColor(_renderer, 235, 215, 165, static_cast<uint8_t>(alpha * 220 / 255));
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 79.0f, perfBuf);

        // Line 5: Hotkeys guide
        SDL_SetRenderDrawColor(_renderer, 115, 140, 170, static_cast<uint8_t>(alpha * 190 / 255));
        SDL_RenderDebugText(_renderer, cardX + 16.0f, cardY + 97.0f, "[Tab/O] HUD  [C] Color  [F/G] Fullscreen");
    }

    void SdlVideoOutput::loadSplashBitmaps() {
        if (!_renderer) return;

        const std::string file = "assets/aver_custom_no_signal.bmp";

        std::vector<std::string> dirs;
        dirs.emplace_back("");        // relative to the current working directory
        dirs.emplace_back("./");

        // CV-18: the executable normally lives in build/src/cli, so walking up a
        // few parents is what actually finds the in-tree assets/ folder. The
        // previous list only reached two levels and missed it when run from the
        // build directory (which is why the blue procedural fallback appeared).
        if (const char *basePath = SDL_GetBasePath()) {
            std::string base(basePath);
            dirs.push_back(base);
            for (int i = 1; i <= 4; ++i) {
                base += "../";
                dirs.push_back(base);
            }
        }

        dirs.emplace_back("/usr/local/share/cv710userspace/");
        dirs.emplace_back("/usr/share/cv710userspace/");

        for (const auto &dir : dirs) {
            std::string path = dir + file;
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

        // The AVerMedia standby bitmap is the whole standby screen; no textual
        // status states are overlaid. The same image is shown while initialising,
        // while unlocked and while waiting for the first frame.
        if (_splashTexture) {
            const float splashW = 640.0f;
            const float splashH = 480.0f;
            const float dstX = (static_cast<float>(winW) - splashW) / 2.0f;
            const float dstY = (static_cast<float>(winH) - splashH) / 2.0f;
            SDL_FRect dstRect{dstX, dstY, splashW, splashH};
            SDL_RenderTexture(_renderer, _splashTexture, nullptr, &dstRect);
        } else {
            // Minimal fallback when the asset is missing: device name on black,
            // no status text.
            SDL_SetRenderScale(_renderer, 2.0f, 2.0f);
            SDL_SetRenderDrawColor(_renderer, 200, 208, 220, 255);
            SDL_RenderDebugText(_renderer,
                                (static_cast<float>(winW) / 2.0f - 120.0f) / 2.0f,
                                (static_cast<float>(winH) / 2.0f) / 2.0f,
                                "AVerMedia ExtremeCap U3 (CV710)");
        }

        SDL_SetRenderScale(_renderer, 1.0f, 1.0f);
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
