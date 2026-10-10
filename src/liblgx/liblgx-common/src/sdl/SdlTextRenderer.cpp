#include "SdlTextRenderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// Vendored public-domain single-header libraries (third_party/stb).
// stb_rect_pack must come first (with its implementation) because
// stb_truetype's packing API builds on it.
#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace sdl {

    namespace {
        constexpr int kAtlasSize = 1024;
        constexpr int kFirstChar = 32;
        constexpr int kCharCount = 95;  // ASCII 32..126
    }

    SdlTextRenderer::~SdlTextRenderer() {
        destroy();
    }

    bool SdlTextRenderer::loadFromMemory(const void *data, std::size_t size,
                                         float pixelHeight, SDL_Renderer *renderer) {
        destroy();
        if (data == nullptr || size == 0 || pixelHeight <= 0.0f || renderer == nullptr) {
            return false;
        }

        _fontData.assign(static_cast<const std::uint8_t *>(data),
                         static_cast<const std::uint8_t *>(data) + size);
        if (_fontData.size() < 4) {
            _fontData.clear();
            return false;
        }

        auto *font = new stbtt_fontinfo();
        if (!stbtt_InitFont(font, _fontData.data(), 0)) {
            delete font;
            destroy();
            return false;
        }
        _font = font;
        _pixelHeight = pixelHeight;

        if (!buildAtlas(renderer)) {
            destroy();
            return false;
        }

        _ready = true;
        return true;
    }

    bool SdlTextRenderer::loadFromFile(const std::string &path, float pixelHeight,
                                       SDL_Renderer *renderer) {
        FILE *f = std::fopen(path.c_str(), "rb");
        if (!f) {
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        bool ok = false;
        if (len > 0) {
            std::vector<std::uint8_t> buf(static_cast<std::size_t>(len));
            ok = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
            std::fclose(f);
            if (ok) {
                ok = loadFromMemory(buf.data(), buf.size(), pixelHeight, renderer);
            }
        } else {
            std::fclose(f);
        }
        return ok;
    }

    bool SdlTextRenderer::buildAtlas(SDL_Renderer *renderer) {
        auto *font = static_cast<stbtt_fontinfo *>(_font);
        if (font == nullptr) {
            return false;
        }

        std::vector<std::uint8_t> coverage(static_cast<std::size_t>(kAtlasSize) * kAtlasSize, 0);

        stbtt_pack_context pack;
        if (!stbtt_PackBegin(&pack, coverage.data(), kAtlasSize, kAtlasSize, 0, 1, nullptr)) {
            return false;
        }
        stbtt_PackSetOversampling(&pack, 2, 2);

        _charData.assign(kCharCount * sizeof(stbtt_packedchar), 0);
        auto *chars = reinterpret_cast<stbtt_packedchar *>(_charData.data());

        stbtt_pack_range range{};
        range.font_size = _pixelHeight;
        range.first_unicode_codepoint_in_range = kFirstChar;
        range.array_of_unicode_codepoints = nullptr;
        range.num_chars = kCharCount;
        range.chardata_for_range = chars;
        if (!stbtt_PackFontRanges(&pack, _fontData.data(), 0, &range, 1)) {
            stbtt_PackEnd(&pack);
            return false;
        }
        stbtt_PackEnd(&pack);

        int asc, desc, gap;
        stbtt_GetFontVMetrics(font, &asc, &desc, &gap);
        const float scale = stbtt_ScaleForPixelHeight(font, _pixelHeight);
        _ascent = asc * scale;
        const float descent = desc * scale;
        _lineHeight = static_cast<int>(std::lround(_ascent - descent + gap * scale));
        if (_lineHeight < 1) {
            _lineHeight = 1;
        }

        SDL_Surface *surface = SDL_CreateSurface(kAtlasSize, kAtlasSize, SDL_PIXELFORMAT_RGBA32);
        if (!surface) {
            return false;
        }
        // White glyphs with alpha = coverage; tint and alpha are applied at draw
        // time via the texture colour/alpha mods.
        for (int y = 0; y < kAtlasSize; ++y) {
            for (int x = 0; x < kAtlasSize; ++x) {
                std::uint8_t cov = coverage[static_cast<std::size_t>(y) * kAtlasSize + x];
                std::uint8_t *px = static_cast<std::uint8_t *>(surface->pixels) +
                                   y * surface->pitch + x * 4;
                px[0] = 255; px[1] = 255; px[2] = 255; px[3] = cov;
            }
        }

        _texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
        if (!_texture) {
            return false;
        }
        SDL_SetTextureScaleMode(_texture, SDL_SCALEMODE_LINEAR);

        _atlasW = kAtlasSize;
        _atlasH = kAtlasSize;
        return true;
    }

    int SdlTextRenderer::measure(const std::string &text) const {
        if (!_ready || _charData.empty()) {
            // Rough fallback (old debug-font width) before a font is loaded.
            return static_cast<int>(text.size()) * 8;
        }
        const auto *chars = reinterpret_cast<const stbtt_packedchar *>(_charData.data());
        float x = 0.0f, y = 0.0f;
        stbtt_aligned_quad q{};
        for (unsigned char c : text) {
            if (c < kFirstChar || c > 126) continue;
            stbtt_GetPackedQuad(chars, _atlasW, _atlasH, c - kFirstChar, &x, &y, &q, 0);
        }
        return static_cast<int>(std::ceil(x));
    }

    void SdlTextRenderer::draw(SDL_Renderer *renderer, const std::string &text,
                               float x, float y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) const {
        if (!_ready || renderer == nullptr || _texture == nullptr || text.empty()) {
            return;
        }
        const auto *chars = reinterpret_cast<const stbtt_packedchar *>(_charData.data());

        SDL_SetTextureBlendMode(_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(_texture, r, g, b);
        SDL_SetTextureAlphaMod(_texture, a);

        // Snap to integer pixels for crisp glyphs, then let stb align advances.
        // stbtt_GetPackedQuad treats ypos as the *baseline*, so convert the
        // caller's line-top y into a baseline by adding the ascent.
        float gx = std::floor(x + 0.5f);
        float gy = std::floor(y + 0.5f) + _ascent;
        stbtt_aligned_quad q{};
        for (unsigned char c : text) {
            if (c < kFirstChar || c > 126) continue;
            stbtt_GetPackedQuad(chars, _atlasW, _atlasH, c - kFirstChar, &gx, &gy, &q, 1);
            SDL_FRect src{q.s0 * static_cast<float>(_atlasW), q.t0 * static_cast<float>(_atlasH),
                          (q.s1 - q.s0) * static_cast<float>(_atlasW),
                          (q.t1 - q.t0) * static_cast<float>(_atlasH)};
            SDL_FRect dst{q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0};
            SDL_RenderTexture(renderer, _texture, &src, &dst);
        }
    }

    void SdlTextRenderer::destroy() {
        if (_texture) {
            SDL_DestroyTexture(_texture);
            _texture = nullptr;
        }
        delete static_cast<stbtt_fontinfo *>(_font);
        _font = nullptr;
        _fontData.clear();
        _charData.clear();
        _atlasW = _atlasH = 0;
        _lineHeight = 0;
        _ascent = 0.0f;
        _pixelHeight = 0.0f;
        _ready = false;
    }

}  // namespace sdl