#include "font.h"
#include "common.h"
#include "gfx.h"

uint32_t NextCodepoint(const char *&s) {
    unsigned char c = (unsigned char)*s;
    if (!c) return 0;
    uint32_t cp;
    int n;
    if (c < 0x80) { cp = c; n = 0; }
    else if ((c >> 5) == 6) { cp = c & 0x1f; n = 1; }
    else if ((c >> 4) == 14) { cp = c & 0x0f; n = 2; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 3; }
    else { s++; return '?'; }
    s++;
    for (int i = 0; i < n; i++) {
        if ((*s & 0xc0) != 0x80) return '?';
        cp = (cp << 6) | (*s & 0x3f);
        s++;
    }
    return cp;
}

bool FontAtlas::load(AAssetManager *am) {
    const char *files[FONT_COUNT] = {"fonts/Inter-Regular.otf", "fonts/Inter-SemiBold.otf", "fonts/Inter-Light.otf"};
    std::vector<uint32_t> cps;
    for (uint32_t c = 32; c < 127; c++) cps.push_back(c);
    uint32_t extra[] = {0xB0, 0xB7, 0xD7, 0x2013, 0x2014, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2713, 0xE9};
    for (uint32_t c : extra) cps.push_back(c);

    const int W = 2048, H = 2048;
    std::vector<uint8_t> atlas(W * H, 0);
    int penX = 1, penY = 1, rowH = 0;
    for (int f = 0; f < FONT_COUNT; f++) {
        m_fontData[f] = ReadAsset(am, files[f]);
        if (m_fontData[f].empty() || !stbtt_InitFont(&m_info[f], m_fontData[f].data(), stbtt_GetFontOffsetForIndex(m_fontData[f].data(), 0))) {
            LOGE("font %s failed", files[f]);
            return false;
        }
        m_scale[f] = stbtt_ScaleForPixelHeight(&m_info[f], kBakePx);
        int asc, desc, gap;
        stbtt_GetFontVMetrics(&m_info[f], &asc, &desc, &gap);
        m_ascent[f] = asc * m_scale[f];
        m_descent[f] = desc * m_scale[f];
        for (uint32_t cp : cps) {
            if (!stbtt_FindGlyphIndex(&m_info[f], cp)) continue;
            int gw = 0, gh = 0, xo = 0, yo = 0;
            unsigned char *sdf = stbtt_GetCodepointSDF(&m_info[f], m_scale[f], cp, kPad, kOnEdge, (float)kOnEdge / kPad, &gw, &gh, &xo, &yo);
            int adv, lsb;
            stbtt_GetCodepointHMetrics(&m_info[f], cp, &adv, &lsb);
            Glyph g = {};
            g.advance = adv * m_scale[f];
            if (sdf) {
                if (penX + gw + 1 >= W) {
                    penX = 1;
                    penY += rowH + 1;
                    rowH = 0;
                }
                if (penY + gh + 1 >= H) {
                    stbtt_FreeSDF(sdf, nullptr);
                    break;
                }
                for (int y = 0; y < gh; y++) memcpy(&atlas[(penY + y) * W + penX], sdf + y * gw, gw);
                g.u0 = (float)penX / W;
                g.v0 = (float)penY / H;
                g.u1 = (float)(penX + gw) / W;
                g.v1 = (float)(penY + gh) / H;
                g.x0 = (float)xo;
                g.y0 = (float)yo;
                g.x1 = (float)(xo + gw);
                g.y1 = (float)(yo + gh);
                penX += gw + 1;
                if (gh > rowH) rowH = gh;
                stbtt_FreeSDF(sdf, nullptr);
            }
            m_glyphs[f][cp] = g;
        }
    }
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, W, H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    LOGI("font atlas built (used %d rows)", penY + rowH);
    return true;
}

const Glyph *FontAtlas::glyph(int font, uint32_t cp) const {
    auto it = m_glyphs[font].find(cp);
    if (it == m_glyphs[font].end()) {
        it = m_glyphs[font].find('?');
        if (it == m_glyphs[font].end()) return nullptr;
    }
    return &it->second;
}

float FontAtlas::kern(int font, uint32_t a, uint32_t b) const {
    return stbtt_GetCodepointKernAdvance(&m_info[font], (int)a, (int)b) * m_scale[font];
}

float FontAtlas::measure(int font, const char *s, float px, float ls) const {
    float k = px / kBakePx, w = 0;
    uint32_t prev = 0;
    int n = 0;
    while (*s) {
        uint32_t cp = NextCodepoint(s);
        const Glyph *g = glyph(font, cp);
        if (!g) continue;
        if (prev) w += kern(font, prev, cp) * k;
        w += g->advance * k + ls * px;
        prev = cp;
        n++;
    }
    if (n) w -= ls * px;
    return w;
}
