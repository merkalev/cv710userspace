#ifndef LGX2USERSPACE_SDLTEXTRENDERER_H
#define LGX2USERSPACE_SDLTEXTRENDERER_H

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sdl {

    // Minimal antialiased TTF text renderer for the OSD/HUD/toast.
    //
    // Uses the vendored public-domain stb_truetype (third_party/stb): the font
    // is loaded once, ASCII glyphs 32..126 are packed into a single atlas
    // texture (2x oversampled), and draw() blits glyph quads with a tint and
    // alpha. Text is drawn at the renderer's native resolution (no 2x debug
    // font scaling), which is what makes it look modern and lets the HUD be
    // smaller. There is deliberately no dependency on SDL_ttf or FreeType.
    //
    // stb_truetype is compiled only in the .cpp; this header stays free of it.
    class SdlTextRenderer {
    public:
        SdlTextRenderer() = default;
        ~SdlTextRenderer();

        SdlTextRenderer(const SdlTextRenderer &) = delete;
        SdlTextRenderer &operator=(const SdlTextRenderer &) = delete;

        // Load a TTF/OTF from memory. The bytes are copied and kept alive for
        // the lifetime of the renderer. Returns false (and destroys any current
        // atlas) on failure.
        bool loadFromMemory(const void *data, std::size_t size, float pixelHeight, SDL_Renderer *renderer);

        // Load a font file from disk (see also SdlVideoOutput::loadFont for the
        // candidate search). Returns false if the file cannot be read or parsed.
        bool loadFromFile(const std::string &path, float pixelHeight, SDL_Renderer *renderer);

        bool loaded() const { return _ready; }

        // Vertical space (pixels) of a text line at the loaded size.
        int lineHeight() const { return _lineHeight; }

        // Advance width of a single ASCII string in pixels.
        int measure(const std::string &text) const;

        // Draw `text` with its top-left at (x, y) using the given tint and alpha.
        // The position is snapped to integer pixels for crisp glyphs.
        void draw(SDL_Renderer *renderer, const std::string &text,
                  float x, float y, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255,
                  uint8_t a = 255) const;

        void destroy();

    private:
        bool buildAtlas(SDL_Renderer *renderer);

        std::vector<std::uint8_t> _fontData;
        // stbtt_packedchar[kCharCount] stored as bytes to keep stb out of this
        // header (the concrete type is known only in the .cpp).
        std::vector<std::uint8_t> _charData;
        SDL_Texture *_texture{nullptr};
        void *_font{nullptr};  // stbtt_fontinfo*
        int _atlasW{0}, _atlasH{0};
        int _lineHeight{0};
        float _ascent{0.0f};
        float _pixelHeight{0.0f};
        bool _ready{false};
    };

}  // namespace sdl

#endif  // LGX2USERSPACE_SDLTEXTRENDERER_H