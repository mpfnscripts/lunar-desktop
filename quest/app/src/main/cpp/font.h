// Signed-distance-field fonts (Inter) baked with stb_truetype into one atlas texture.
#pragma once
#include <GLES3/gl32.h>
#include <android/asset_manager.h>
#include <unordered_map>
#include <vector>
#include "stb_truetype.h"

enum FontId { FONT_REGULAR = 0, FONT_SEMIBOLD = 1, FONT_LIGHT = 2, FONT_COUNT = 3 };

struct Glyph {
    float u0, v0, u1, v1;  // atlas uv
    float x0, y0, x1, y1;  // quad relative to pen at baseline, in bake pixels
    float advance;
};

class FontAtlas {
public:
    static constexpr float kBakePx = 48.f;
    static constexpr int kPad = 6;
    static constexpr unsigned char kOnEdge = 180;

    bool load(AAssetManager *am);
    GLuint texture() const { return m_tex; }
    const Glyph *glyph(int font, uint32_t cp) const;
    float kern(int font, uint32_t a, uint32_t b) const;
    float ascent(int font) const { return m_ascent[font]; }
    float descent(int font) const { return m_descent[font]; }
    // width in pixels of utf8 text at size px (letterSpacing in em)
    float measure(int font, const char *utf8, float px, float letterSpacing = 0.f) const;

private:
    GLuint m_tex = 0;
    std::vector<uint8_t> m_fontData[FONT_COUNT];
    stbtt_fontinfo m_info[FONT_COUNT];
    float m_scale[FONT_COUNT];
    float m_ascent[FONT_COUNT], m_descent[FONT_COUNT];
    std::unordered_map<uint32_t, Glyph> m_glyphs[FONT_COUNT];
};

uint32_t NextCodepoint(const char *&s);
