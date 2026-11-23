// Immediate-mode 2D UI in the Lunar style (frosted glass, glowing moons, SDF text), drawn into a
// texture that the 3D scene shows as a floating panel.
#pragma once
#include <GLES3/gl32.h>
#include <string>
#include <vector>
#include "common.h"
#include "font.h"

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    Rect() {}
    Rect(float X, float Y, float W, float H) : x(X), y(Y), w(W), h(H) {}
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    Rect inset(float d) const { return Rect(x + d, y + d, w - 2 * d, h - 2 * d); }
    float cx() const { return x + w / 2; }
    float cy() const { return y + h / 2; }
};

enum Align { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

struct UiPointer {
    bool valid = false;   // pointer is over this panel
    float x = 0, y = 0;   // panel pixels
    bool down = false;    // trigger held
    bool pressed = false; // went down this frame
    bool released = false;
    float scroll = 0;     // thumbstick, -1..1
};

enum Icon {
    ICON_CLOSE, ICON_BACK, ICON_CHECK, ICON_PLUS, ICON_MINUS, ICON_SETTINGS, ICON_MONITOR, ICON_KEYBOARD,
    ICON_VR, ICON_POWER, ICON_RECENTER, ICON_LEFT, ICON_RIGHT, ICON_WIFI, ICON_REFRESH, ICON_MIC, ICON_VOLUME, ICON_STAR
};

class Ui {
public:
    bool init(FontAtlas *fonts);
    // Begin drawing into the currently bound framebuffer of size w x h.
    void begin(int w, int h, const Theme &theme, const UiPointer &ptr, float time, bool linearOut);
    void end();

    // ---- primitives ----
    void rect(const Rect &r, float radius, const Color &top, const Color &bottom, const Color &border = Color(0, 0, 0, 0),
              float borderW = 0, float feather = 0, float borderFade = 0);
    void circle(float cx, float cy, float rad, const Color &top, const Color &bottom, const Color &border = Color(0, 0, 0, 0), float borderW = 0,
                float feather = 0);
    void line(float x0, float y0, float x1, float y1, float width, const Color &c);
    void moon(float cx, float cy, float rad, float glow, bool craters = true);
    void image(const Rect &r, GLuint tex, float radius, const Color &tint = Color(1, 1, 1, 1));
    float text(float x, float y, const char *utf8, float px, const Color &c, int font = FONT_REGULAR, int align = ALIGN_LEFT,
               float letterSpacing = 0, const Color &glow = Color(0, 0, 0, 0), float maxWidth = 0);
    void icon(Icon ic, float cx, float cy, float size, const Color &c);
    void pushClip(const Rect &r);
    void popClip();

    // ---- Lunar components ----
    void glassPanel(const Rect &r, float radius, float opacityBoost = 0);
    void brand(float x, float y, float px, const char *second = "Desktop");
    bool button(int id, const Rect &r, const char *label, bool primary = false, bool active = false, Icon ic = (Icon)-1);
    bool roundButton(int id, float cx, float cy, float rad, Icon ic, bool primary = false, bool active = false, const char *tip = nullptr);
    bool tab(int id, const Rect &r, const char *label, bool on);
    int segmented(int id, const Rect &r, const char *const *labels, int count, int selected);
    bool toggle(int id, float x, float y, bool on);
    bool slider(int id, const Rect &r, float *value, float minV, float maxV);
    bool row(int id, const Rect &r, bool selected);
    void sectionHeader(float x, float y, const char *label);
    void pill(const Rect &r, const char *label, const Color &dot);

    bool hovered(const Rect &r) const { return m_ptr.valid && r.contains(m_ptr.x, m_ptr.y) && clipContains(m_ptr.x, m_ptr.y); }
    bool anyHot() const { return m_hotThisFrame; }
    const Theme &theme() const { return m_theme; }
    FontAtlas *fonts() { return m_fonts; }
    float time() const { return m_time; }
    int hapticRequest = 0;  // 1 = hover tick, 2 = press

private:
    struct Vtx {
        float px, py, lx, ly, hx, hy, radius, kind;
        float a[4], b[4], c[4], p[4];
        float u, v;
    };
    void quad(const Rect &r, float kind, float radius, const Color &a, const Color &b, const Color &c, const float p[4], float u0 = 0,
              float v0 = 0, float u1 = 1, float v1 = 1, float grow = 0);
    void flush();
    void setTexture(GLuint t);
    bool clipContains(float x, float y) const;
    bool interact(int id, const Rect &r);

    FontAtlas *m_fonts = nullptr;
    GLuint m_prog = 0, m_vao = 0, m_vbo = 0;
    GLint m_uSize = -1, m_uLinear = -1, m_uFont = -1, m_uImage = -1;
    std::vector<Vtx> m_verts;
    GLuint m_curTex = 0;
    int m_w = 0, m_h = 0;
    Theme m_theme;
    UiPointer m_ptr;
    float m_time = 0;
    std::vector<Rect> m_clips;
    int m_activeId = -1, m_lastHot = -1;
    bool m_hotThisFrame = false;
    int m_hotId = -1;
};
