#include "SdlVideoOutput.h"
#include "SdlTextRenderer.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define CV710_HAVE_X86_SIMD 1
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#define CV710_HAVE_ARM_SIMD 1
#endif

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

    const char *SdlVideoOutput::aspectName(AspectMode mode) {
        switch (mode) {
            case AspectMode::Stretch: return "stretch";
            case AspectMode::Auto:    return "auto";
            case AspectMode::R4x3:    return "4:3";
            case AspectMode::R16x9:   return "16:9";
        }
        return "stretch";
    }

    AspectMode SdlVideoOutput::parseAspect(const std::string &name) {
        if (name == "auto") {
            return AspectMode::Auto;
        } else if (name == "4:3" || name == "43" || name == "4x3" || name == "classic") {
            return AspectMode::R4x3;
        } else if (name == "16:9" || name == "169" || name == "16x9" || name == "widescreen") {
            return AspectMode::R16x9;
        }
        return AspectMode::Stretch;
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
        loadFont();
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
                _toastText = std::string("Color: ") + colorspaceShortName(_colorspaceMode) + " (Auto)";
                printf("[Video] Auto-selected colorspace from AVI InfoFrame: %s (%s)\n",
                       colorspaceName(_colorspaceMode),
                       autoColorspaceSourceLabel(metrics.signalInfo.aviColorspace));
                fflush(stdout);
            }
        }
    }

    namespace {
#if defined(CV710_HAVE_X86_SIMD)
        // AVX2 YUY2 -> RGBA32 row converter for step == 1, non-swapped chroma.
        // Bit-for-bit identical to the scalar path in convertYuy2ToRgba, including
        // the CV-09 co-sited chroma reconstruction. 8 macropixels per iteration.
        __attribute__((target("avx2")))
        void convertRowAvx2(const uint32_t *srcRow, uint32_t *dstRow, int pairs,
                            int cY, int cRV, int cGU, int cGV, int cBU, int y_off) {
            const __m256i maskFF  = _mm256_set1_epi32(0xFF);
            const __m256i bias128 = _mm256_set1_epi32(128);
            const __m256i one     = _mm256_set1_epi32(1);
            const __m256i round   = _mm256_set1_epi32(32768);
            const __m256i zero    = _mm256_setzero_si256();
            const __m256i c255    = _mm256_set1_epi32(255);
            const __m256i alpha   = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
            const __m256i cy  = _mm256_set1_epi32(cY);
            const __m256i crv = _mm256_set1_epi32(cRV);
            const __m256i cgu = _mm256_set1_epi32(cGU);
            const __m256i cgv = _mm256_set1_epi32(cGV);
            const __m256i cbu = _mm256_set1_epi32(cBU);
            const __m256i yoff = _mm256_set1_epi32(y_off);
            const __m256i nxt = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 7);

            int x = 0;
            for (; x + 8 <= pairs; x += 8) {
                __m256i word = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(srcRow + x));
                __m256i y0 = _mm256_and_si256(word, maskFF);
                __m256i u  = _mm256_and_si256(_mm256_srli_epi32(word, 8), maskFF);
                __m256i y1 = _mm256_and_si256(_mm256_srli_epi32(word, 16), maskFF);
                __m256i v  = _mm256_srli_epi32(word, 24);

                __m256i uN = _mm256_permutevar8x32_epi32(u, nxt);
                __m256i vN = _mm256_permutevar8x32_epi32(v, nxt);
                // Lane 7's neighbour lives outside this vector: patch it from the
                // following word, or leave it self-referential at the row end.
                if (x + 8 < pairs) {
                    uint32_t nw = srcRow[x + 8];
                    uN = _mm256_insert_epi32(uN, static_cast<int>((nw >> 8) & 0xFF), 7);
                    vN = _mm256_insert_epi32(vN, static_cast<int>((nw >> 24) & 0xFF), 7);
                }

                __m256i uv = _mm256_sub_epi32(u, bias128);
                __m256i vv = _mm256_sub_epi32(v, bias128);
                __m256i uOdd = _mm256_sub_epi32(_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(u, uN), one), 1), bias128);
                __m256i vOdd = _mm256_sub_epi32(_mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(v, vN), one), 1), bias128);

                __m256i rOff  = _mm256_mullo_epi32(crv, vv);
                __m256i gOff  = _mm256_sub_epi32(zero, _mm256_add_epi32(_mm256_mullo_epi32(cgu, uv), _mm256_mullo_epi32(cgv, vv)));
                __m256i bOff  = _mm256_mullo_epi32(cbu, uv);
                __m256i rOff1 = _mm256_mullo_epi32(crv, vOdd);
                __m256i gOff1 = _mm256_sub_epi32(zero, _mm256_add_epi32(_mm256_mullo_epi32(cgu, uOdd), _mm256_mullo_epi32(cgv, vOdd)));
                __m256i bOff1 = _mm256_mullo_epi32(cbu, uOdd);

                __m256i y0s = _mm256_add_epi32(_mm256_mullo_epi32(cy, _mm256_sub_epi32(y0, yoff)), round);
                __m256i y1s = _mm256_add_epi32(_mm256_mullo_epi32(cy, _mm256_sub_epi32(y1, yoff)), round);

                __m256i r0 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y0s, rOff), 16), zero), c255);
                __m256i g0 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y0s, gOff), 16), zero), c255);
                __m256i b0 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y0s, bOff), 16), zero), c255);
                __m256i r1 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y1s, rOff1), 16), zero), c255);
                __m256i g1 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y1s, gOff1), 16), zero), c255);
                __m256i b1 = _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(_mm256_add_epi32(y1s, bOff1), 16), zero), c255);

                __m256i ev = _mm256_or_si256(alpha,
                             _mm256_or_si256(_mm256_slli_epi32(b0, 16),
                             _mm256_or_si256(_mm256_slli_epi32(g0, 8), r0)));
                __m256i od = _mm256_or_si256(alpha,
                             _mm256_or_si256(_mm256_slli_epi32(b1, 16),
                             _mm256_or_si256(_mm256_slli_epi32(g1, 8), r1)));

                __m256i lo = _mm256_unpacklo_epi32(ev, od);
                __m256i hi = _mm256_unpackhi_epi32(ev, od);
                __m256i o0 = _mm256_permute2x128_si256(lo, hi, 0x20);
                __m256i o1 = _mm256_permute2x128_si256(lo, hi, 0x31);
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(dstRow + x * 2), o0);
                _mm256_storeu_si256(reinterpret_cast<__m256i *>(dstRow + x * 2 + 8), o1);
            }

            // Scalar tail: replicate the co-sited neighbour lookup for the final
            // macropixel(s) not covered by a full vector.
            for (; x < pairs; x++) {
                uint32_t word = srcRow[x];
                uint8_t y0 = word & 0xFF;
                uint8_t u  = (word >> 8) & 0xFF;
                uint8_t y1 = (word >> 16) & 0xFF;
                uint8_t v  = (word >> 24) & 0xFF;
                uint8_t uNext = u, vNext = v;
                if (x + 1 < pairs) {
                    uint32_t nw = srcRow[x + 1];
                    uNext = (nw >> 8) & 0xFF;
                    vNext = (nw >> 24) & 0xFF;
                }
                int u_val = static_cast<int>(u) - 128;
                int v_val = static_cast<int>(v) - 128;
                int u_valOdd = (static_cast<int>(u) + static_cast<int>(uNext) + 1) / 2 - 128;
                int v_valOdd = (static_cast<int>(v) + static_cast<int>(vNext) + 1) / 2 - 128;
                int r_off = cRV * v_val, g_off = -(cGU * u_val + cGV * v_val), b_off = cBU * u_val;
                int r_off1 = cRV * v_valOdd, g_off1 = -(cGU * u_valOdd + cGV * v_valOdd), b_off1 = cBU * u_valOdd;
                int y0s = cY * (static_cast<int>(y0) - y_off) + 32768;
                int y1s = cY * (static_cast<int>(y1) - y_off) + 32768;
                uint8_t r0 = clamp8((y0s + r_off) >> 16), g0 = clamp8((y0s + g_off) >> 16), b0 = clamp8((y0s + b_off) >> 16);
                uint8_t r1 = clamp8((y1s + r_off1) >> 16), g1 = clamp8((y1s + g_off1) >> 16), b1 = clamp8((y1s + b_off1) >> 16);
                dstRow[x * 2]     = 0xFF000000u | (static_cast<uint32_t>(b0) << 16) | (static_cast<uint32_t>(g0) << 8) | r0;
                dstRow[x * 2 + 1] = 0xFF000000u | (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(g1) << 8) | r1;
            }
        }
#endif

        bool avx2Available() {
#if defined(CV710_HAVE_X86_SIMD)
            return __builtin_cpu_supports("avx2");
#else
            return false;
#endif
        }
#if defined(CV710_HAVE_ARM_SIMD)
        // NEON (armv8 Advanced SIMD) YUY2 -> RGBA32 row converter. Same
        // arithmetic as the scalar path, bit-for-bit incl. the CV-09 co-sited
        // chroma reconstruction; 4 macropixels per iteration. armv8-A mandates
        // Advanced SIMD, so no runtime feature check is needed on aarch64.
        // Exactness is verified once at runtime by simdSelfCheckPass() below.
        void convertRowNeon(const uint32_t *srcRow, uint32_t *dstRow, int pairs,
                            int cY, int cRV, int cGU, int cGV, int cBU, int y_off) {
            const uint32x4_t maskFF  = vdupq_n_u32(0xFFu);
            const uint32x4_t bias128 = vdupq_n_u32(128u);
            const int32x4_t  round   = vdupq_n_s32(32768);
            const int32x4_t  zero    = vdupq_n_s32(0);
            const int32x4_t  c255    = vdupq_n_s32(255);
            const uint32x4_t alpha   = vdupq_n_u32(0xFF000000u);
            const int32x4_t  cy  = vdupq_n_s32(cY);
            const int32x4_t  crv = vdupq_n_s32(cRV);
            const int32x4_t  cgu = vdupq_n_s32(cGU);
            const int32x4_t  cgv = vdupq_n_s32(cGV);
            const int32x4_t  cbu = vdupq_n_s32(cBU);
            const int32x4_t  yoff = vdupq_n_s32(y_off);

            int x = 0;
            for (; x + 4 <= pairs; x += 4) {
                const uint32x4_t word = vld1q_u32(srcRow + x);
                // YUYV macropixel in a 32-bit word (LE bytes): [Y1 | V | Y0 | U]
                const uint32x4_t y0 = vandq_u32(word, maskFF);
                const uint32x4_t u  = vandq_u32(vshrq_n_u32(word, 8), maskFF);
                const uint32x4_t y1 = vandq_u32(vshrq_n_u32(word, 16), maskFF);
                const uint32x4_t v  = vshrq_n_u32(word, 24);  // top byte already 0..255

                // CV-09: the odd (Y1) pixel uses chroma interpolated halfway to
                // the NEXT macropixel's chroma; the row's final macropixel
                // replicates its own. Lane 3 (last macropixel of the vector)
                // reads its neighbour from srcRow[x+4] when it exists.
                uint32x4_t uN = vextq_u32(u, u, 1);              // {u1,u2,u3,u0}
                uint32x4_t vN = vextq_u32(v, v, 1);
                const uint32_t uLast = vgetq_lane_u32(u, 3);
                const uint32_t vLast = vgetq_lane_u32(v, 3);
                uN = vsetq_lane_u32(uLast, uN, 3);               // {u1,u2,u3,u3}
                vN = vsetq_lane_u32(vLast, vN, 3);
                if (x + 4 < pairs) {
                    const uint32_t nw = srcRow[x + 4];
                    uN = vsetq_lane_u32((nw >> 8) & 0xFFu, uN, 3);
                    vN = vsetq_lane_u32((nw >> 24) & 0xFFu, vN, 3);
                }

                const uint32x4_t uOdd = vrhaddq_u32(u, uN);      // (u+uN+1)>>1
                const uint32x4_t vOdd = vrhaddq_u32(v, vN);

                const int32x4_t uu  = vreinterpretq_s32_u32(vsubq_u32(u, bias128));
                const int32x4_t vv  = vreinterpretq_s32_u32(vsubq_u32(v, bias128));
                const int32x4_t uuO = vreinterpretq_s32_u32(vsubq_u32(uOdd, bias128));
                const int32x4_t vvO = vreinterpretq_s32_u32(vsubq_u32(vOdd, bias128));

                const int32x4_t rOff  = vmulq_n_s32(vv, cRV);
                const int32x4_t gOff  = vsubq_s32(zero, vaddq_s32(vmulq_n_s32(uu, cGU), vmulq_n_s32(vv, cGV)));
                const int32x4_t bOff  = vmulq_n_s32(uu, cBU);
                const int32x4_t rOff1 = vmulq_n_s32(vvO, cRV);
                const int32x4_t gOff1 = vsubq_s32(zero, vaddq_s32(vmulq_n_s32(uuO, cGU), vmulq_n_s32(vvO, cGV)));
                const int32x4_t bOff1 = vmulq_n_s32(uuO, cBU);

                const int32x4_t y0s = vaddq_s32(vmulq_n_s32(vsubq_s32(vreinterpretq_s32_u32(y0), yoff), cY), round);
                const int32x4_t y1s = vaddq_s32(vmulq_n_s32(vsubq_s32(vreinterpretq_s32_u32(y1), yoff), cY), round);

                const int32x4_t r0 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y0s, rOff), 16), c255), zero);
                const int32x4_t g0 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y0s, gOff), 16), c255), zero);
                const int32x4_t b0 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y0s, bOff), 16), c255), zero);
                const int32x4_t r1 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y1s, rOff1), 16), c255), zero);
                const int32x4_t g1 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y1s, gOff1), 16), c255), zero);
                const int32x4_t b1 = vmaxq_s32(vminq_s32(vshrq_n_s32(vaddq_s32(y1s, bOff1), 16), c255), zero);

                uint32x4_t ev = vreinterpretq_u32_s32(r0);
                uint32x4_t od = vreinterpretq_u32_s32(r1);
                ev = vorrq_u32(ev, vshlq_n_u32(vreinterpretq_u32_s32(g0), 8));
                ev = vorrq_u32(ev, vshlq_n_u32(vreinterpretq_u32_s32(b0), 16));
                od = vorrq_u32(od, vshlq_n_u32(vreinterpretq_u32_s32(g1), 8));
                od = vorrq_u32(od, vshlq_n_u32(vreinterpretq_u32_s32(b1), 16));
                ev = vorrq_u32(ev, alpha);
                od = vorrq_u32(od, alpha);

                const uint32x4x2_t zip = vzipq_u32(ev, od);
                vst1q_u32(dstRow + x * 2, zip.val[0]);
                vst1q_u32(dstRow + x * 2 + 4, zip.val[1]);
            }

            // Scalar tail: mirrors the production scalar loop exactly; see the
            // identical tail in convertRowAvx2.
            for (; x < pairs; x++) {
                uint32_t word = srcRow[x];
                uint8_t y0 = word & 0xFF;
                uint8_t u  = (word >> 8) & 0xFF;
                uint8_t y1 = (word >> 16) & 0xFF;
                uint8_t v  = (word >> 24) & 0xFF;
                uint8_t uNext = u, vNext = v;
                if (x + 1 < pairs) {
                    uint32_t nw = srcRow[x + 1];
                    uNext = (nw >> 8) & 0xFF;
                    vNext = (nw >> 24) & 0xFF;
                }
                int u_val = static_cast<int>(u) - 128;
                int v_val = static_cast<int>(v) - 128;
                int u_valOdd = (static_cast<int>(u) + static_cast<int>(uNext) + 1) / 2 - 128;
                int v_valOdd = (static_cast<int>(v) + static_cast<int>(vNext) + 1) / 2 - 128;
                int r_off = cRV * v_val, g_off = -(cGU * u_val + cGV * v_val), b_off = cBU * u_val;
                int r_off1 = cRV * v_valOdd, g_off1 = -(cGU * u_valOdd + cGV * v_valOdd), b_off1 = cBU * u_valOdd;
                int y0s = cY * (static_cast<int>(y0) - y_off) + 32768;
                int y1s = cY * (static_cast<int>(y1) - y_off) + 32768;
                uint8_t r0 = clamp8((y0s + r_off) >> 16), g0 = clamp8((y0s + g_off) >> 16), b0 = clamp8((y0s + b_off) >> 16);
                uint8_t r1 = clamp8((y1s + r_off1) >> 16), g1 = clamp8((y1s + g_off1) >> 16), b1 = clamp8((y1s + b_off1) >> 16);
                dstRow[x * 2]     = 0xFF000000u | (static_cast<uint32_t>(b0) << 16) | (static_cast<uint32_t>(g0) << 8) | r0;
                dstRow[x * 2 + 1] = 0xFF000000u | (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(g1) << 8) | r1;
            }
        }
#endif

        // Scalar row reference, byte-identical to the production loop for
        // step == 1 / non-swapped chroma. Used only by the SIMD self-check.
        void convertRowScalarRef(const uint32_t *srcRow, uint32_t *dstRow, int pairs,
                                 int cY, int cRV, int cGU, int cGV, int cBU, int y_off) {
            for (int x = 0; x < pairs; x++) {
                uint32_t word = srcRow[x];
                uint8_t y0 = word & 0xFF;
                uint8_t u  = (word >> 8) & 0xFF;
                uint8_t y1 = (word >> 16) & 0xFF;
                uint8_t v  = (word >> 24) & 0xFF;
                uint8_t uNext = u, vNext = v;
                if (x + 1 < pairs) {
                    uint32_t nw = srcRow[x + 1];
                    uNext = (nw >> 8) & 0xFF;
                    vNext = (nw >> 24) & 0xFF;
                }
                int u_val = static_cast<int>(u) - 128;
                int v_val = static_cast<int>(v) - 128;
                int u_valOdd = (static_cast<int>(u) + static_cast<int>(uNext) + 1) / 2 - 128;
                int v_valOdd = (static_cast<int>(v) + static_cast<int>(vNext) + 1) / 2 - 128;
                int r_off = cRV * v_val, g_off = -(cGU * u_val + cGV * v_val), b_off = cBU * u_val;
                int r_off1 = cRV * v_valOdd, g_off1 = -(cGU * u_valOdd + cGV * v_valOdd), b_off1 = cBU * u_valOdd;
                int y0s = cY * (static_cast<int>(y0) - y_off) + 32768;
                int y1s = cY * (static_cast<int>(y1) - y_off) + 32768;
                uint8_t r0 = clamp8((y0s + r_off) >> 16), g0 = clamp8((y0s + g_off) >> 16), b0 = clamp8((y0s + b_off) >> 16);
                uint8_t r1 = clamp8((y1s + r_off1) >> 16), g1 = clamp8((y1s + g_off1) >> 16), b1 = clamp8((y1s + b_off1) >> 16);
                dstRow[x * 2]     = 0xFF000000u | (static_cast<uint32_t>(b0) << 16) | (static_cast<uint32_t>(g0) << 8) | r0;
                dstRow[x * 2 + 1] = 0xFF000000u | (static_cast<uint32_t>(b1) << 16) | (static_cast<uint32_t>(g1) << 8) | r1;
            }
        }

#if defined(CV710_HAVE_X86_SIMD) || defined(CV710_HAVE_ARM_SIMD)
        // One-shot bit-exactness self-check of the active SIMD row converter
        // against the scalar reference, run on the first converted frame (once
        // per process). Covers all four BT.709/601 limited/full coefficient
        // sets and a row width that exercises both the vector loops and the
        // scalar tails (41 macropixels = e.g. 5x8+1 on AVX2, 10x4+1 on NEON).
        // On any mismatch the SIMD path is disabled for the session, so a
        // converter bug can never silently corrupt colours.
        bool simdSelfCheckPass() {
            const int pairs = 41;
            std::vector<uint32_t> src(static_cast<size_t>(pairs));
            std::vector<uint32_t> ref(static_cast<size_t>(pairs) * 2);
            std::vector<uint32_t> sim(static_cast<size_t>(pairs) * 2);
            uint32_t seed = 0x12345678u;
            for (int i = 0; i < pairs; i++) {
                seed = seed * 1664525u + 1013904223u;
                src[static_cast<size_t>(i)] = seed;
            }

            struct Coef { int cY, cRV, cGU, cGV, cBU, y_off; };
            const Coef sets[] = {
                {76309, 117489, 13975, 34925, 138438, 16},  // BT.709 limited
                {65536, 103206, 12276, 30679, 121608, 0},   // BT.709 full
                {76309, 104597, 25675, 53279, 132201, 16},  // BT.601 limited
                {65536,  91881, 22554, 46802, 116130, 0},   // BT.601 full
            };
            for (const Coef &c : sets) {
                convertRowScalarRef(src.data(), ref.data(), pairs, c.cY, c.cRV, c.cGU, c.cGV, c.cBU, c.y_off);
#if defined(CV710_HAVE_X86_SIMD)
                convertRowAvx2(src.data(), sim.data(), pairs, c.cY, c.cRV, c.cGU, c.cGV, c.cBU, c.y_off);
#elif defined(CV710_HAVE_ARM_SIMD)
                convertRowNeon(src.data(), sim.data(), pairs, c.cY, c.cRV, c.cGU, c.cGV, c.cBU, c.y_off);
#endif
                if (ref != sim) {
                    return false;
                }
                std::fill(sim.begin(), sim.end(), 0);
            }
            return true;
        }
#endif
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

        const bool useSimd = (step == 1) && !swapChroma;
#if defined(CV710_HAVE_X86_SIMD)
        const bool simdCapable = useSimd && avx2Available();
#elif defined(CV710_HAVE_ARM_SIMD)
        const bool simdCapable = useSimd;  // Advanced SIMD is mandatory on armv8-A
#else
        const bool simdCapable = false;
#endif

        // CV-21: one-shot bit-exact self-check of the SIMD row converter on the
        // first SIMD-capable frame. A mismatch disables SIMD for the session
        // rather than risking silently wrong colours (also guards the ARM64
        // NEON path, which cannot be validated on x86 build hosts). Guarded by
        // simdCapable so the check only ever calls a SIMD function the CPU can
        // actually run (x86: AVX2 absent => never call the target("avx2") fn).
        static bool simdChecked = false;
        static bool simdVerified = false;
        if (!simdChecked && simdCapable) {
            simdChecked = true;
#if defined(CV710_HAVE_X86_SIMD) || defined(CV710_HAVE_ARM_SIMD)
            simdVerified = simdSelfCheckPass();
            if (simdVerified) {
                printf("[Video] SIMD YUY2->RGBA row converter self-check passed (bit-exact vs scalar)\n");
            } else {
                fprintf(stderr, "[Video] SIMD YUY2->RGBA row converter self-check FAILED - using scalar path\n");
            }
            fflush(stdout);
#endif
        }
        const bool useSimdRows = simdCapable && simdVerified;

        auto processRows = [&](int y_start, int y_end) {
            for (int y = y_start; y < y_end; y++) {
                const uint32_t *srcRow = src + (y * step) * srcStrideWords;
                uint32_t *dstRow = dst + y * dstWidth;

                if (useSimdRows) {
#if defined(CV710_HAVE_X86_SIMD)
                    convertRowAvx2(srcRow, dstRow, pairsPerDstRow, cY, cRV, cGU, cGV, cBU, y_off);
                    continue;
#elif defined(CV710_HAVE_ARM_SIMD)
                    convertRowNeon(srcRow, dstRow, pairsPerDstRow, cY, cRV, cGU, cGV, cBU, y_off);
                    continue;
#endif
                }

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
            char toastBuf[64];
            snprintf(toastBuf, sizeof(toastBuf), "%dx%d%s", _srcWidth, _srcHeight,
                     _metrics.signalInfo.interlaced ? "i" : "p");
            _toastText = toastBuf;
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
                    _toastText = _hudPersistent ? "Diagnostic HUD on" : "Diagnostic HUD off";
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
                    _toastText = std::string("Color: ") + colorspaceShortName(_colorspaceMode) +
                                 (_colorspaceUserOverride ? " (Manual)" : " (Auto)");

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

        // Persistent diagnostic HUD (Tab/O): fully opaque while enabled.
        uint8_t hudAlpha = 0;
        if (_hudPersistent) {
            hudAlpha = 255;
        }

        // Transient toast (mode/resolution changes): fades in/out on its own.
        uint8_t toastAlpha = 0;
        if (_showOsd) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - _osdTimestamp).count();
            if (elapsedMs < 2500) {
                if (elapsedMs <= 1800) {
                    toastAlpha = 255;
                } else {
                    float fade = 1.0f - static_cast<float>(elapsedMs - 1800) / 700.0f;
                    if (fade < 0.0f) fade = 0.0f;
                    toastAlpha = static_cast<uint8_t>(255.0f * fade);
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
                if (toastAlpha > 0) {
                    renderToast(toastAlpha);
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
        if (!shouldRender && (hudAlpha > 0 || toastAlpha > 0) &&
            (now - _lastHudRender >= std::chrono::milliseconds(33))) {
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

            // CV-24: aspect-ratio handling. Default is Stretch (fill the window,
            // historical behaviour). Otherwise fit the picture into the window
            // preserving the chosen DAR and pillarbox/letterbox the rest in black.
            int winW = 1920, winH = 1080;
            SDL_GetWindowSize(_window, &winW, &winH);
            SDL_FRect videoDst{0.0f, 0.0f, static_cast<float>(winW), static_cast<float>(winH)};
            if (_aspectMode != AspectMode::Stretch) {
                float winAspect = (winH > 0) ? static_cast<float>(winW) / static_cast<float>(winH) : (16.0f / 9.0f);
                float dar = 16.0f / 9.0f;
                switch (_aspectMode) {
                    case AspectMode::R4x3:   dar = 4.0f / 3.0f; break;
                    case AspectMode::R16x9:  dar = 16.0f / 9.0f; break;
                    case AspectMode::Auto:
                    default:
                        if (_srcHeight <= 576) {
                            dar = 4.0f / 3.0f;  // 480p/576p classic SD anamorphic container
                        } else {
                            dar = (_texHeight > 0)
                                ? static_cast<float>(_texWidth) / static_cast<float>(_texHeight)
                                : 16.0f / 9.0f;
                        }
                        break;
                }
                float fitW = static_cast<float>(winW), fitH = static_cast<float>(winH);
                if (dar > winAspect) {
                    fitH = fitW / dar;   // wider than the window: width-constrained
                } else {
                    fitW = fitH * dar;   // taller than the window: height-constrained
                }
                videoDst.x = (static_cast<float>(winW) - fitW) * 0.5f;
                videoDst.y = (static_cast<float>(winH) - fitH) * 0.5f;
                videoDst.w = fitW;
                videoDst.h = fitH;

                // Plain black bars behind the fitted picture.
                SDL_SetRenderDrawColor(_renderer, 0, 0, 0, 255);
                SDL_RenderFillRect(_renderer, nullptr);
            }
            SDL_RenderTexture(_renderer, _texture, nullptr, &videoDst);
            if (prevBlend != SDL_BLENDMODE_BLEND) {
                SDL_SetTextureBlendMode(_texture, prevBlend);
            }

            if (hudAlpha > 0) {
                renderDiagnosticHud(hudAlpha);
            }
            if (toastAlpha > 0) {
                renderToast(toastAlpha);
            }

            SDL_RenderPresent(_renderer);
            _newFrameAvailable = false;
        }
    }

    namespace {
        // Rounded-rectangle fill built from horizontal spans so it composites
        // correctly with the renderer's alpha blending (SDL has no rounded-rect
        // primitive).
        void fillRoundRect(SDL_Renderer *r, float x, float y, float w, float h,
                           float rad, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t a) {
            if (rad < 0.0f) rad = 0.0f;
            if (rad * 2.0f > w) rad = w / 2.0f;
            if (rad * 2.0f > h) rad = h / 2.0f;
            SDL_SetRenderDrawColor(r, cr, cg, cb, a);
            if (rad <= 0.0f) {
                SDL_FRect rect{x, y, w, h};
                SDL_RenderFillRect(r, &rect);
                return;
            }
            SDL_FRect mid{x, y + rad, w, h - 2.0f * rad};
            SDL_RenderFillRect(r, &mid);
            SDL_FRect top{x + rad, y, w - 2.0f * rad, rad};
            SDL_RenderFillRect(r, &top);
            SDL_FRect bot{x + rad, y + h - rad, w - 2.0f * rad, rad};
            SDL_RenderFillRect(r, &bot);
            int steps = static_cast<int>(rad);
            for (int i = 0; i < steps; i++) {
                float dy = rad - static_cast<float>(i);
                float inset = rad - std::sqrt(std::max(0.0f, rad * rad - dy * dy));
                float span = rad - inset;
                SDL_FRect l{x + inset, y + i, span, 1.0f};
                SDL_FRect lb{x + inset, y + h - 1.0f - i, span, 1.0f};
                SDL_FRect rr{x + w - rad, y + i, span, 1.0f};
                SDL_FRect rb{x + w - rad, y + h - 1.0f - i, span, 1.0f};
                SDL_RenderFillRect(r, &l);
                SDL_RenderFillRect(r, &lb);
                SDL_RenderFillRect(r, &rr);
                SDL_RenderFillRect(r, &rb);
            }
        }

        void accentForColorspace(ColorspaceMode m, uint8_t &r, uint8_t &g, uint8_t &b) {
            switch (m) {
                case ColorspaceMode::BT709_Full:    r = 255; g = 183; b = 3;   break; // amber
                case ColorspaceMode::BT601_Limited: r = 6;   g = 214; b = 160; break; // emerald
                case ColorspaceMode::BT601_Full:    r = 138; g = 201; b = 38;  break; // lime
                case ColorspaceMode::UYVY_Swap:     r = 157; g = 78;  b = 221; break; // violet
                case ColorspaceMode::Direct_YUY2:   r = 58;  g = 134; b = 255; break; // blue
                default:                            r = 0;   g = 180; b = 216; break; // cyan
            }
        }
    }

    void SdlVideoOutput::renderDiagnosticHud(uint8_t alpha) {
        if (!_renderer || alpha == 0) return;

        int winW = 1920, winH = 1080;
        SDL_GetWindowSize(_window, &winW, &winH);

        SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

        // Build every string up front, then size the card to the widest content
        // so labels can never collide with values, regardless of which font
        // loaded (fallback 8x8 debug font is much wider than Inter).
        char inBuf[64], colBuf[64], audBuf[64], capBuf[64];
        float hwFps = _metrics.signalInfo.measuredFps > 0.0f ? _metrics.signalInfo.measuredFps : 60.0f;
        snprintf(inBuf, sizeof(inBuf), "%dx%d%s  %.2fHz",
                 _srcWidth, _srcHeight, _metrics.signalInfo.interlaced ? "i" : "p", hwFps);

        const char *csTag = _colorspaceUserOverride ? "Manual" :
            (aviIsRgb(_metrics.signalInfo.aviColorspace) ? "Auto" : "Auto");
        snprintf(colBuf, sizeof(colBuf), "%s  %s", colorspaceShortName(_colorspaceMode), csTag);

        float audKhz = static_cast<float>(_metrics.signalInfo.audioSampleRate) / 1000.0f;
        snprintf(audBuf, sizeof(audBuf), "%.1fk Hz Stereo  %s",
                 audKhz > 0.0f ? audKhz : 48.0f,
                 _metrics.signalInfo.audioLocked ? "Locked" : "Unlocked");

        snprintf(capBuf, sizeof(capBuf), "%.1ffps  valid %u  drop %u",
                 _metrics.liveFps, _metrics.validFrames, _metrics.droppedFrames);

        static const char *kTitle = "AVerMedia CV710";
        static const char *kHint = "Tab HUD   C Color   F/G Full   R Re-sync";
        static const char *kLabels[4] = {"INPUT", "COLOR", "AUDIO", "CAPTURE"};
        const char *kValues[4] = {inBuf, colBuf, audBuf, capBuf};

        auto textW = [this](const char *s) {
            if (!_textRenderer.loaded()) return static_cast<float>(std::strlen(s)) * 8.0f;
            return static_cast<float>(_textRenderer.measure(s));
        };

        // Widest label decides the label column width, widest value its own;
        // with a fixed gap between them they can never overlap.
        float labelColW = 0.0f, valueColW = 0.0f;
        for (int i = 0; i < 4; ++i) {
            labelColW = std::max(labelColW, textW(kLabels[i]));
            valueColW = std::max(valueColW, textW(kValues[i]));
        }

        const char *statusText;
        uint8_t sr, sg, sb;
        if (_metrics.signalInfo.valid && _metrics.signalInfo.locked) {
            statusText = "LOCKED";    sr = 46;  sg = 200; sb = 96;
        } else if (_metrics.signalInfo.valid) {
            statusText = "NO SIGNAL"; sr = 235; sg = 150; sb = 40;
        } else {
            statusText = "LIVE";      sr = 0;   sg = 170; sb = 220;
        }
        const float pillW = textW(statusText) + 18.0f;

        constexpr float kLeftPad   = 12.0f;
        constexpr float kValueGap  = 10.0f;
        constexpr float kRightPad  = 12.0f;
        constexpr float kTitleX    = 24.0f;
        constexpr float kPillGap   = 8.0f;
        constexpr float kPillRight = 10.0f;

        const float contentNeed = kLeftPad + labelColW + kValueGap + valueColW + kRightPad;
        const float headerNeed  = kTitleX + textW(kTitle) + kPillGap + pillW + kPillRight;
        const float footerNeed  = kLeftPad + textW(kHint) + kRightPad;

        const float margin = 14.0f;
        const float rad = 10.0f;
        const float cardW = std::max(contentNeed,
                              std::max(headerNeed, std::max(footerNeed, 248.0f)));
        const float cardX = static_cast<float>(winW) - cardW - margin;
        const float cardY = margin;

        const float lh = static_cast<float>(_textRenderer.loaded() ? _textRenderer.lineHeight() : 14);
        const float pitch = lh + 2.0f;
        const float rowTop = cardY + 35.0f;
        const float rowsBottom = rowTop + 4.0f * pitch - 2.0f;
        const float footerTop = rowsBottom + 4.0f;
        const float cardH = footerTop - cardY + lh + 9.0f;

        uint8_t accR, accG, accB;
        accentForColorspace(_colorspaceMode, accR, accG, accB);

        // Soft drop shadow.
        fillRoundRect(_renderer, cardX + 3.0f, cardY + 4.0f, cardW, cardH, rad,
                      0, 0, 0, static_cast<uint8_t>(alpha * 70 / 255));

        // Frosted obsidian card body (semi-transparent so the video shows through).
        fillRoundRect(_renderer, cardX, cardY, cardW, cardH, rad,
                      16, 20, 28, static_cast<uint8_t>(alpha * 200 / 255));

        // Hairline border, top and bottom only (keeps the left/right clean).
        SDL_SetRenderDrawColor(_renderer, 64, 78, 100, static_cast<uint8_t>(alpha * 170 / 255));
        SDL_FRect borderTop{cardX + rad, cardY, cardW - 2.0f * rad, 1.0f};
        SDL_FRect borderBot{cardX + rad, cardY + cardH - 1.0f, cardW - 2.0f * rad, 1.0f};
        SDL_RenderFillRect(_renderer, &borderTop);
        SDL_RenderFillRect(_renderer, &borderBot);

        // Accent glow line along the top edge.
        fillRoundRect(_renderer, cardX + rad, cardY, cardW - 2.0f * rad, 2.0f, 1.0f,
                      accR, accG, accB, alpha);

        // Header: accent dot + device title.
        SDL_FRect dot{cardX + 12.0f, cardY + 11.0f, 6.0f, 6.0f};
        SDL_SetRenderDrawColor(_renderer, accR, accG, accB, alpha);
        SDL_RenderFillRect(_renderer, &dot);
        if (_textRenderer.loaded()) {
            _textRenderer.draw(_renderer, kTitle, cardX + kTitleX, cardY + 7.0f, 238, 244, 255, alpha);
        } else {
            SDL_SetRenderDrawColor(_renderer, 238, 244, 255, alpha);
            SDL_RenderDebugText(_renderer, cardX + kTitleX + 2.0f, cardY + 11.0f, kTitle);
        }

        // Status pill (right side of the header).
        float pillH = lh + 4.0f;
        float pillX = cardX + cardW - kPillRight - pillW;
        float pillY = cardY + 8.0f;
        fillRoundRect(_renderer, pillX, pillY, pillW, pillH, pillH / 2.0f,
                      sr, sg, sb, static_cast<uint8_t>(alpha * 225 / 255));
        if (_textRenderer.loaded()) {
            _textRenderer.draw(_renderer, statusText, pillX + 9.0f, pillY + 2.0f, 10, 14, 20, alpha);
        } else {
            SDL_SetRenderDrawColor(_renderer, 10, 14, 20, alpha);
            SDL_RenderDebugText(_renderer, pillX + 7.0f, pillY + 3.0f, statusText);
        }

        // Divider under the header.
        SDL_SetRenderDrawColor(_renderer, 52, 64, 82, static_cast<uint8_t>(alpha * 170 / 255));
        SDL_FRect divider{cardX + kLeftPad, cardY + 28.0f, cardW - 2.0f * kLeftPad, 1.0f};
        SDL_RenderFillRect(_renderer, &divider);

        // Rows: dim label, bright value.
        struct HudRow { const char *label; const char *value; uint8_t vr, vg, vb; };
        const HudRow rows[4] = {
            {"INPUT",   inBuf,  200, 224, 250},
            {"COLOR",   colBuf, accR, accG, accB},
            {"AUDIO",   audBuf,
                static_cast<uint8_t>(_metrics.signalInfo.audioLocked ? 140 : 225),
                static_cast<uint8_t>(_metrics.signalInfo.audioLocked ? 230 : 180),
                static_cast<uint8_t>(_metrics.signalInfo.audioLocked ? 170 : 110)},
            {"CAPTURE", capBuf, 232, 214, 170},
        };

        const float labelX = cardX + kLeftPad;
        const float valueX = labelX + labelColW + kValueGap;
        float rowY = rowTop;
        for (const auto &row : rows) {
            if (_textRenderer.loaded()) {
                _textRenderer.draw(_renderer, row.label, labelX, rowY,
                                   120, 136, 158, static_cast<uint8_t>(alpha * 235 / 255));
                _textRenderer.draw(_renderer, row.value, valueX, rowY,
                                   row.vr, row.vg, row.vb, static_cast<uint8_t>(alpha * 240 / 255));
            } else {
                SDL_SetRenderDrawColor(_renderer, 120, 136, 158, static_cast<uint8_t>(alpha * 235 / 255));
                SDL_RenderDebugText(_renderer, labelX, rowY, row.label);
                SDL_SetRenderDrawColor(_renderer, row.vr, row.vg, row.vb, static_cast<uint8_t>(alpha * 240 / 255));
                SDL_RenderDebugText(_renderer, valueX, rowY, row.value);
            }
            rowY += pitch;
        }

        // Footer hotkey hint.
        if (_textRenderer.loaded()) {
            _textRenderer.draw(_renderer, kHint, cardX + kLeftPad, footerTop,
                               104, 120, 144, static_cast<uint8_t>(alpha * 200 / 255));
        } else {
            SDL_SetRenderDrawColor(_renderer, 104, 120, 144, static_cast<uint8_t>(alpha * 200 / 255));
            SDL_RenderDebugText(_renderer, cardX + kLeftPad, footerTop, kHint);
        }
    }

    void SdlVideoOutput::renderToast(uint8_t alpha) {
        if (!_renderer || alpha == 0 || _toastText.empty()) return;

        int winW = 1920, winH = 1080;
        SDL_GetWindowSize(_window, &winW, &winH);

        SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

        const float lh = static_cast<float>(_textRenderer.loaded() ? _textRenderer.lineHeight() : 14);
        const float W = static_cast<float>(winW);
        const float H = static_cast<float>(winH);

        uint8_t accR, accG, accB;
        accentForColorspace(_colorspaceMode, accR, accG, accB);

        float tw = static_cast<float>(_textRenderer.measure(_toastText));
        float pillH = lh + 8.0f;
        float pillW = tw + 34.0f;
        float pillX = (W - pillW) / 2.0f;
        float pillY = H - pillH - 26.0f;

        fillRoundRect(_renderer, pillX, pillY, pillW, pillH, pillH / 2.0f,
                      14, 18, 26, static_cast<uint8_t>(alpha * 195 / 255));

        SDL_FRect dot{pillX + 11.0f, pillY + (pillH - 6.0f) / 2.0f, 6.0f, 6.0f};
        SDL_SetRenderDrawColor(_renderer, accR, accG, accB, alpha);
        SDL_RenderFillRect(_renderer, &dot);

        if (_textRenderer.loaded()) {
            _textRenderer.draw(_renderer, _toastText, pillX + 23.0f, pillY + 4.0f, 232, 238, 248, alpha);
        } else {
            SDL_SetRenderDrawColor(_renderer, 232, 238, 248, alpha);
            SDL_RenderDebugText(_renderer, pillX + 22.0f, pillY + 6.0f, _toastText.c_str());
        }
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

    void SdlVideoOutput::loadFont() {
        if (!_renderer) return;

        // OSD glyph pixel height: small enough to feel like an overlay, large
        // enough to read at 1080p. (The old 8x8 debug font drew at 16px.)
        const float pixelHeight = 14.0f;

        // Explicit override wins.
        if (const char *env = std::getenv("CV710_FONT"); env != nullptr && *env != '\0') {
            if (_textRenderer.loadFromFile(env, pixelHeight, _renderer)) {
                printf("[Video] Loaded OSD font: %s\n", env);
                fflush(stdout);
                return;
            }
            fprintf(stderr, "[Video] CV710_FONT=%s could not be loaded; searching defaults\n", env);
        }

        // Optional bundled font (assets/fonts/ui.ttf), using the same base-path
        // walk as the standby bitmap so an in-tree assets/ folder is found.
        std::vector<std::string> dirs;
        dirs.emplace_back("");
        dirs.emplace_back("./");
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
            std::string path = dir + "assets/fonts/ui.ttf";
            if (_textRenderer.loadFromFile(path, pixelHeight, _renderer)) {
                printf("[Video] Loaded OSD font: %s\n", path.c_str());
                fflush(stdout);
                return;
            }
        }

        // System fonts: prefer modern humanist/geometric sans faces.
        static const char *candidates[] = {
            "/usr/share/fonts/TTF/ttf-google-fonts-typewolf/Inter[opsz,wght].ttf",
            "/usr/share/fonts/noto/NotoSans-Medium.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/TTF/ttf-google-fonts-typewolf/FiraSans-SemiBold.ttf",
            "/usr/share/fonts/TTF/ttf-google-fonts-typewolf/FiraSans-Regular.ttf",
            "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
        };
        for (const char *path : candidates) {
            if (_textRenderer.loadFromFile(path, pixelHeight, _renderer)) {
                printf("[Video] Loaded OSD font: %s\n", path);
                fflush(stdout);
                return;
            }
        }

        fprintf(stderr, "[Video] No TTF font found; OSD uses the SDL debug font "
                        "(install a sans font or set CV710_FONT)\n");
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

        _textRenderer.destroy();
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
