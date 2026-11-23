#include "ui.h"
#include "gfx.h"

static const char *kVS = R"(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aLocal;
layout(location=2) in vec2 aHalf;
layout(location=3) in vec2 aRK;
layout(location=4) in vec4 aA;
layout(location=5) in vec4 aB;
layout(location=6) in vec4 aC;
layout(location=7) in vec4 aP;
layout(location=8) in vec2 aUV;
uniform vec2 uSize;
out vec2 vLocal; out vec2 vHalf; flat out vec2 vRK; out vec4 vA; out vec4 vB; out vec4 vC; flat out vec4 vP; out vec2 vUV;
void main(){
  vLocal=aLocal; vHalf=aHalf; vRK=aRK; vA=aA; vB=aB; vC=aC; vP=aP; vUV=aUV;
  gl_Position=vec4(aPos.x/uSize.x*2.0-1.0, 1.0-aPos.y/uSize.y*2.0, 0.0, 1.0);
})";

static const char *kFS = R"(#version 300 es
precision highp float;
uniform sampler2D uFont;
uniform sampler2D uImage;
uniform float uLinear;
in vec2 vLocal; in vec2 vHalf; flat in vec2 vRK; in vec4 vA; in vec4 vB; in vec4 vC; flat in vec4 vP; in vec2 vUV;
out vec4 o;
float sdRound(vec2 p, vec2 b, float r){ vec2 q=abs(p)-b+r; return length(max(q,0.0))+min(max(q.x,q.y),0.0)-r; }
vec3 toLin(vec3 c){ return mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(0.04045,c)); }
vec3 toSrgb(vec3 c){ c=clamp(c,0.0,1.0); return mix(c*12.92, 1.055*pow(c,vec3(1.0/2.4))-0.055, step(0.0031308,c)); }
vec4 pm(vec4 c){ return vec4(c.rgb*c.a, c.a); }
void main(){
  int kind=int(vRK.y+0.5);
  vec4 col=vec4(0.0);
  if(kind==0){
    float d=sdRound(vLocal, vHalf, min(vRK.x, min(vHalf.x, vHalf.y)));
    float aa=max(fwidth(d),0.0005)*0.8;
    float fe=max(vP.y,0.0);
    float fill=1.0-smoothstep(-fe-aa, fe+aa, d);
    float t=clamp((vLocal.y+vHalf.y)/(2.0*vHalf.y),0.0,1.0);
    col=pm(mix(vA,vB,t))*fill;
    if(vP.x>0.0){
      float band=smoothstep(-vP.x-aa,-vP.x+aa,d)*(1.0-smoothstep(-aa,aa,d));
      float ba=vC.a*band;
      if(vP.z>0.0) ba*=1.0-smoothstep(0.0,vP.z,t);
      col=col*(1.0-ba)+vec4(vC.rgb*ba,ba);
    }
  } else if(kind==1){
    float dist=texture(uFont,vUV).r;
    float edge=180.0/255.0;
    float aa=max(fwidth(dist),0.002)*0.75;
    float a=smoothstep(edge-aa,edge+aa,dist)*vA.a;
    col=vec4(vA.rgb*a,a);
    if(vB.a>0.0){ float g=smoothstep(edge-vP.x,edge,dist)*vB.a*(1.0-a); g*=g; col+=vec4(vB.rgb*g,g); }
  } else if(kind==2){
    vec2 p=vLocal/vHalf;
    float r=length(p);
    float aa=max(fwidth(r),0.001);
    float disc=1.0-smoothstep(1.0-aa,1.0+aa,r);
    float l=length(p-vec2(-0.28,-0.40))/1.40;
    vec3 c= l<0.42 ? mix(vec3(1.0), vA.rgb, l/0.42) : mix(vA.rgb, vB.rgb, clamp((l-0.42)/0.58,0.0,1.0));
    if(vP.z>0.5){
      vec3 cr=vec3(110.0,124.0,175.0)/255.0;
      float c1=(1.0-smoothstep(0.15,0.17,length(p-vec2(-0.40,0.28))))*0.45;
      float c2=(1.0-smoothstep(0.23,0.25,length(p-vec2(0.24,-0.28))))*0.38;
      float c3=(1.0-smoothstep(0.11,0.13,length(p-vec2(0.08,0.52))))*0.32;
      c=mix(c,cr,c1); c=mix(c,cr,c2); c=mix(c,cr,c3);
    }
    float e=length((p-vec2(-0.4,-1.0))/vec2(2.4,1.6));
    float g= e<0.42 ? mix(0.42,0.06,e/0.42) : mix(0.06,0.0,clamp((e-0.42)/0.13,0.0,1.0));
    c=mix(c,vec3(1.0),g);
    float rim=smoothstep(0.62,1.0,r)*0.42;
    c=mix(c,vec3(8.0,12.0,34.0)/255.0,rim);
    if(vP.w>0.5){ // bite (logo crescent)
      float bite=1.0-smoothstep(0.98-aa,0.98+aa,length(p-vec2(0.62,-0.38)));
      disc*=1.0-bite*0.92;
    }
    col=vec4(c*disc,disc);
    float halo=r>1.0? exp(-(r-1.0)*3.2)*vP.x : 0.0;
    col+=vec4(vC.rgb*halo,halo)*(1.0-disc);
  } else if(kind==3){
    vec4 t=texture(uImage,vUV);
    t.rgb=toSrgb(t.rgb);
    float d=sdRound(vLocal,vHalf,vRK.x);
    float aa=max(fwidth(d),0.0005)*0.8;
    float m=1.0-smoothstep(-aa,aa,d);
    col=pm(t*vA)*m;
  } else {
    vec2 pa=vLocal-vP.xy, ba=vP.zw-vP.xy;
    float h=clamp(dot(pa,ba)/max(dot(ba,ba),1e-5),0.0,1.0);
    float d=length(pa-ba*h)-vRK.x;
    float aa=max(fwidth(d),0.0005)*0.8;
    float a=(1.0-smoothstep(-aa,aa,d))*vA.a;
    col=vec4(vA.rgb*a,a);
  }
  if(uLinear>0.5 && col.a>0.0){ col.rgb=toLin(col.rgb/col.a)*col.a; }
  o=col;
})";

bool Ui::init(FontAtlas *fonts) {
    m_fonts = fonts;
    m_prog = CompileProgram(kVS, kFS, "ui");
    m_uSize = glGetUniformLocation(m_prog, "uSize");
    m_uLinear = glGetUniformLocation(m_prog, "uLinear");
    m_uFont = glGetUniformLocation(m_prog, "uFont");
    m_uImage = glGetUniformLocation(m_prog, "uImage");
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const int stride = sizeof(Vtx);
    int off = 0;
    auto attr = [&](int loc, int n) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, n, GL_FLOAT, GL_FALSE, stride, (void *)(intptr_t)off);
        off += n * 4;
    };
    attr(0, 2);
    attr(1, 2);
    attr(2, 2);
    attr(3, 2);
    attr(4, 4);
    attr(5, 4);
    attr(6, 4);
    attr(7, 4);
    attr(8, 2);
    glBindVertexArray(0);
    return m_prog != 0;
}

void Ui::begin(int w, int h, const Theme &theme, const UiPointer &ptr, float time, bool linearOut) {
    m_w = w;
    m_h = h;
    m_theme = theme;
    m_ptr = ptr;
    m_time = time;
    m_verts.clear();
    m_clips.clear();
    m_hotThisFrame = false;
    m_hotId = -1;
    m_curTex = 0;
    glUseProgram(m_prog);
    glUniform2f(m_uSize, (float)w, (float)h);
    glUniform1f(m_uLinear, linearOut ? 1.f : 0.f);
    glUniform1i(m_uFont, 0);
    glUniform1i(m_uImage, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_fonts->texture());
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
}

void Ui::end() {
    flush();
    glDisable(GL_SCISSOR_TEST);
    if (m_hotId != m_lastHot) {
        if (m_hotId >= 0) hapticRequest = hapticRequest > 1 ? hapticRequest : 1;
        m_lastHot = m_hotId;
    }
    if (m_ptr.released || !m_ptr.down) m_activeId = -1;
}

void Ui::flush() {
    if (m_verts.empty()) return;
    glUseProgram(m_prog);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, m_verts.size() * sizeof(Vtx), m_verts.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_verts.size());
    glBindVertexArray(0);
    m_verts.clear();
}

void Ui::setTexture(GLuint t) {
    if (t == m_curTex) return;
    flush();
    m_curTex = t;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, t);
    glActiveTexture(GL_TEXTURE0);
}

void Ui::pushClip(const Rect &r) {
    flush();
    Rect c = r;
    if (!m_clips.empty()) {
        const Rect &p = m_clips.back();
        float x0 = fmaxf(c.x, p.x), y0 = fmaxf(c.y, p.y), x1 = fminf(c.x + c.w, p.x + p.w), y1 = fminf(c.y + c.h, p.y + p.h);
        c = Rect(x0, y0, fmaxf(0.f, x1 - x0), fmaxf(0.f, y1 - y0));
    }
    m_clips.push_back(c);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    float sx = vp[2] / (float)m_w, sy = vp[3] / (float)m_h;
    glEnable(GL_SCISSOR_TEST);
    glScissor(vp[0] + (int)(c.x * sx), vp[1] + (int)((m_h - c.y - c.h) * sy), (int)ceilf(c.w * sx), (int)ceilf(c.h * sy));
}

void Ui::popClip() {
    flush();
    m_clips.pop_back();
    if (m_clips.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        Rect c = m_clips.back();
        m_clips.pop_back();
        pushClip(c);
    }
}

bool Ui::clipContains(float x, float y) const { return m_clips.empty() || m_clips.back().contains(x, y); }

void Ui::quad(const Rect &r, float kind, float radius, const Color &a, const Color &b, const Color &c, const float p[4], float u0, float v0,
              float u1, float v1, float grow) {
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    float hx = r.w / 2, hy = r.h / 2;
    float X0 = r.x - grow, Y0 = r.y - grow, X1 = r.x + r.w + grow, Y1 = r.y + r.h + grow;
    float gu = grow > 0 && r.w > 0 ? (u1 - u0) * grow / r.w : 0, gv = grow > 0 && r.h > 0 ? (v1 - v0) * grow / r.h : 0;
    auto mk = [&](float x, float y, float u, float v) {
        Vtx t;
        t.px = x;
        t.py = y;
        t.lx = x - cx;
        t.ly = y - cy;
        t.hx = hx;
        t.hy = hy;
        t.radius = radius;
        t.kind = kind;
        memcpy(t.a, &a, 16);
        memcpy(t.b, &b, 16);
        memcpy(t.c, &c, 16);
        memcpy(t.p, p, 16);
        t.u = u;
        t.v = v;
        return t;
    };
    Vtx q0 = mk(X0, Y0, u0 - gu, v0 - gv), q1 = mk(X1, Y0, u1 + gu, v0 - gv), q2 = mk(X1, Y1, u1 + gu, v1 + gv), q3 = mk(X0, Y1, u0 - gu, v1 + gv);
    m_verts.push_back(q0);
    m_verts.push_back(q1);
    m_verts.push_back(q2);
    m_verts.push_back(q0);
    m_verts.push_back(q2);
    m_verts.push_back(q3);
}

void Ui::rect(const Rect &r, float radius, const Color &top, const Color &bottom, const Color &border, float borderW, float feather,
              float borderFade) {
    float p[4] = {borderW, feather, borderFade, 0};
    quad(r, 0, radius, top, bottom, border, p, 0, 0, 1, 1, feather + 2);
}

void Ui::circle(float cx, float cy, float rad, const Color &top, const Color &bottom, const Color &border, float borderW, float feather) {
    rect(Rect(cx - rad, cy - rad, rad * 2, rad * 2), rad, top, bottom, border, borderW, feather);
}

void Ui::line(float x0, float y0, float x1, float y1, float width, const Color &c) {
    float r = width / 2;
    Rect b(fminf(x0, x1) - r, fminf(y0, y1) - r, fabsf(x1 - x0) + width, fabsf(y1 - y0) + width);
    float cx = b.x + b.w / 2, cy = b.y + b.h / 2;
    float p[4] = {x0 - cx, y0 - cy, x1 - cx, y1 - cy};
    quad(b, 4, r, c, c, c, p, 0, 0, 1, 1, 2);
}

void Ui::moon(float cx, float cy, float rad, float glow, bool craters) {
    float p[4] = {glow, 0, craters ? 1.f : 0.f, 0};
    float grow = glow > 0 ? rad * 1.2f : 2;
    quad(Rect(cx - rad, cy - rad, rad * 2, rad * 2), 2, rad, m_theme.moonLight, m_theme.moonDark, m_theme.accent, p, 0, 0, 1, 1, grow);
}

void Ui::image(const Rect &r, GLuint tex, float radius, const Color &tint) {
    setTexture(tex);
    float p[4] = {0, 0, 0, 0};
    quad(r, 3, radius, tint, tint, tint, p);
}

float Ui::text(float x, float y, const char *s, float px, const Color &c, int font, int align, float ls, const Color &glow, float maxWidth) {
    if (!s || !*s) return 0;
    std::string tmp;
    float w = m_fonts->measure(font, s, px, ls);
    if (maxWidth > 0 && w > maxWidth) {
        // ellipsize
        tmp = s;
        while (!tmp.empty()) {
            do { tmp.pop_back(); } while (!tmp.empty() && ((unsigned char)tmp.back() & 0xc0) == 0x80);
            std::string t2 = tmp + "\xE2\x80\xA6";
            if (m_fonts->measure(font, t2.c_str(), px, ls) <= maxWidth) {
                tmp = t2;
                break;
            }
        }
        s = tmp.c_str();
        w = m_fonts->measure(font, s, px, ls);
    }
    if (align == ALIGN_CENTER) x -= w / 2;
    else if (align == ALIGN_RIGHT) x -= w;
    float k = px / FontAtlas::kBakePx;
    // y is the vertical centre of the line
    float baseline = y + (m_fonts->ascent(font) + m_fonts->descent(font)) * 0.5f * k;
    float pen = x;
    uint32_t prev = 0;
    float p[4] = {glow.a > 0 ? 0.32f : 0.f, 0, 0, 0};
    while (*s) {
        uint32_t cp = NextCodepoint(s);
        const Glyph *g = m_fonts->glyph(font, cp);
        if (!g) continue;
        if (prev) pen += m_fonts->kern(font, prev, cp) * k;
        if (g->x1 > g->x0) {
            Rect r(pen + g->x0 * k, baseline + g->y0 * k, (g->x1 - g->x0) * k, (g->y1 - g->y0) * k);
            quad(r, 1, 0, c, glow, glow, p, g->u0, g->v0, g->u1, g->v1);
        }
        pen += g->advance * k + ls * px;
        prev = cp;
    }
    return w;
}

void Ui::icon(Icon ic, float cx, float cy, float s, const Color &c) {
    float w = fmaxf(2.f, s * 0.1f);
    float h = s / 2;
    switch (ic) {
        case ICON_CLOSE:
            line(cx - h * .7f, cy - h * .7f, cx + h * .7f, cy + h * .7f, w, c);
            line(cx - h * .7f, cy + h * .7f, cx + h * .7f, cy - h * .7f, w, c);
            break;
        case ICON_BACK:
        case ICON_LEFT:
            line(cx + h * .3f, cy - h * .7f, cx - h * .4f, cy, w, c);
            line(cx - h * .4f, cy, cx + h * .3f, cy + h * .7f, w, c);
            break;
        case ICON_RIGHT:
            line(cx - h * .3f, cy - h * .7f, cx + h * .4f, cy, w, c);
            line(cx + h * .4f, cy, cx - h * .3f, cy + h * .7f, w, c);
            break;
        case ICON_CHECK:
            line(cx - h * .7f, cy, cx - h * .2f, cy + h * .5f, w, c);
            line(cx - h * .2f, cy + h * .5f, cx + h * .7f, cy - h * .5f, w, c);
            break;
        case ICON_PLUS:
            line(cx, cy - h * .7f, cx, cy + h * .7f, w, c);
            /* fallthrough */
        case ICON_MINUS:
            line(cx - h * .7f, cy, cx + h * .7f, cy, w, c);
            break;
        case ICON_SETTINGS:
            for (int i = 0; i < 8; i++) {
                float a = i * (float)M_PI / 4;
                line(cx + cosf(a) * h * .55f, cy + sinf(a) * h * .55f, cx + cosf(a) * h * .85f, cy + sinf(a) * h * .85f, w * 1.4f, c);
            }
            circle(cx, cy, h * .55f, Color(0, 0, 0, 0), Color(0, 0, 0, 0), c, w);
            break;
        case ICON_MONITOR:
            rect(Rect(cx - h * .85f, cy - h * .65f, h * 1.7f, h * 1.1f), w * 1.2f, Color(0, 0, 0, 0), Color(0, 0, 0, 0), c, w);
            line(cx, cy + h * .45f, cx, cy + h * .75f, w, c);
            line(cx - h * .4f, cy + h * .8f, cx + h * .4f, cy + h * .8f, w, c);
            break;
        case ICON_KEYBOARD:
            rect(Rect(cx - h * .9f, cy - h * .5f, h * 1.8f, h * 1.0f), w * 1.2f, Color(0, 0, 0, 0), Color(0, 0, 0, 0), c, w);
            for (int i = 0; i < 4; i++) circle(cx - h * .5f + i * h * .33f, cy - h * .15f, w * .6f, c, c);
            line(cx - h * .45f, cy + h * .22f, cx + h * .45f, cy + h * .22f, w, c);
            break;
        case ICON_VR: {
            // four-point sparkle
            line(cx, cy - h * .9f, cx, cy + h * .9f, w, c);
            line(cx - h * .9f, cy, cx + h * .9f, cy, w, c);
            line(cx - h * .35f, cy - h * .35f, cx + h * .35f, cy + h * .35f, w * .8f, c);
            line(cx - h * .35f, cy + h * .35f, cx + h * .35f, cy - h * .35f, w * .8f, c);
            break;
        }
        case ICON_POWER:
            for (int i = 0; i < 14; i++) {
                float a0 = -M_PI / 2 + 0.45f + i * (2 * M_PI - 0.9f) / 14, a1 = a0 + (2 * M_PI - 0.9f) / 14;
                line(cx + cosf(a0) * h * .7f, cy + sinf(a0) * h * .7f, cx + cosf(a1) * h * .7f, cy + sinf(a1) * h * .7f, w, c);
            }
            line(cx, cy - h * .9f, cx, cy - h * .1f, w, c);
            break;
        case ICON_RECENTER:
            circle(cx, cy, h * .25f, c, c);
            circle(cx, cy, h * .75f, Color(0, 0, 0, 0), Color(0, 0, 0, 0), c, w);
            break;
        case ICON_WIFI:
            for (int k = 1; k <= 3; k++) {
                float rr = h * .32f * k;
                for (int i = 0; i < 6; i++) {
                    float a0 = -M_PI * 0.75f + i * (M_PI * 0.5f) / 6, a1 = a0 + (M_PI * 0.5f) / 6;
                    line(cx + cosf(a0) * rr, cy + h * .5f + sinf(a0) * rr, cx + cosf(a1) * rr, cy + h * .5f + sinf(a1) * rr, w, c);
                }
            }
            break;
        case ICON_REFRESH:
            for (int i = 0; i < 12; i++) {
                float a0 = 0.5f + i * (2 * M_PI - 1.2f) / 12, a1 = a0 + (2 * M_PI - 1.2f) / 12;
                line(cx + cosf(a0) * h * .7f, cy + sinf(a0) * h * .7f, cx + cosf(a1) * h * .7f, cy + sinf(a1) * h * .7f, w, c);
            }
            line(cx + h * .55f, cy - h * .65f, cx + h * .62f, cy - h * .2f, w, c);
            line(cx + h * .62f, cy - h * .2f, cx + h * .2f, cy - h * .28f, w, c);
            break;
        case ICON_MIC:
            rect(Rect(cx - h * .28f, cy - h * .85f, h * .56f, h * 1.1f), h * .28f, Color(0, 0, 0, 0), Color(0, 0, 0, 0), c, w);
            line(cx, cy + h * .55f, cx, cy + h * .85f, w, c);
            line(cx - h * .3f, cy + h * .85f, cx + h * .3f, cy + h * .85f, w, c);
            break;
        case ICON_VOLUME:
            line(cx - h * .7f, cy - h * .25f, cx - h * .7f, cy + h * .25f, w, c);
            line(cx - h * .7f, cy - h * .25f, cx - h * .1f, cy - h * .7f, w, c);
            line(cx - h * .7f, cy + h * .25f, cx - h * .1f, cy + h * .7f, w, c);
            line(cx - h * .1f, cy - h * .7f, cx - h * .1f, cy + h * .7f, w, c);
            line(cx + h * .3f, cy - h * .3f, cx + h * .3f, cy + h * .3f, w, c);
            line(cx + h * .65f, cy - h * .55f, cx + h * .65f, cy + h * .55f, w, c);
            break;
        case ICON_STAR:
            moon(cx, cy, h * .8f, 0.f, true);
            break;
    }
}

// ------------------------------------------------------------------------------------------------
// Lunar components
// ------------------------------------------------------------------------------------------------
static Color ClunkFace(const Theme &t) { return Color::Mix(t.accent, Color::Hex("#2e3033"), 0.12f); }
static Color ClunkField(const Theme &t) { return Color::Mix(t.accent, Color::Hex("#1b1c1e"), 0.05f); }
static Color ClunkSel(const Theme &t) { return Color::Mix(t.accent2, Color::Hex("#151515"), 0.78f); }

void Ui::glassPanel(const Rect &r, float radius, float boost) {
    const Theme &t = m_theme;
    if (t.clunk) {
        rect(r, 0, ClunkFace(t), ClunkFace(t), Color::Hex("#5f6368"), 3);
        return;
    }
    // drop shadow
    rect(Rect(r.x, r.y + 24, r.w, r.h), radius, Color(0, 0, 0, 0.5f), Color(0, 0, 0, 0.5f), Color(0, 0, 0, 0), 0, 34);
    // base so text stays readable over bright scenery
    rect(r, radius, t.bg.withA(0.42f + boost), t.bg.withA(0.55f + boost));
    // glass fill: accent 16% -> 5%
    rect(r, radius, t.accent.withA(0.16f), t.accent.withA(0.05f), Color(225 / 255.f, 232 / 255.f, 1.f, 0.16f), 1.5f);
    // bright top edge
    rect(r, radius, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(240 / 255.f, 244 / 255.f, 1.f, 0.5f), 2.f, 0, 0.06f);
    // gloss sheen over the top 46%
    rect(Rect(r.x + 1, r.y + 1, r.w - 2, r.h * 0.46f), radius, Color(1, 1, 1, 0.13f), Color(1, 1, 1, 0.0f));
}

void Ui::brand(float x, float y, float px, const char *second) {
    const Theme &t = m_theme;
    if (t.clunk) {
        std::string s = std::string("Lunar ") + second;
        text(x, y, s.c_str(), px * 0.8f, Color::Hex("#e4e4e4"), FONT_SEMIBOLD);
        return;
    }
    float r = px * 0.95f;
    float p[4] = {0.55f, 0, 0, 1};
    quad(Rect(x, y - r, r * 2, r * 2), 2, r, t.moonLight, t.moonDark, t.accent, p, 0, 0, 1, 1, r * 1.2f);
    float tx = x + r * 2 + px * 0.6f;
    Color c = Color::Mix(Color(1, 1, 1), t.moonLight, 0.5f);
    tx += text(tx, y, "LUNAR", px, c, FONT_SEMIBOLD, ALIGN_LEFT, 0.42f) + px * 0.42f + px * 0.3f;
    std::string up = second;
    for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
    text(tx, y, up.c_str(), px, c, FONT_LIGHT, ALIGN_LEFT, 0.42f);
}

bool Ui::interact(int id, const Rect &r) {
    bool hov = hovered(r);
    if (hov) {
        m_hotId = id;
        m_hotThisFrame = true;
        if (m_ptr.pressed) m_activeId = id;
    }
    bool clicked = hov && m_ptr.released && m_activeId == id;
    if (clicked) hapticRequest = 2;
    return clicked;
}

bool Ui::button(int id, const Rect &r, const char *label, bool primary, bool active, Icon ic) {
    const Theme &t = m_theme;
    bool clicked = interact(id, r);
    bool hov = hovered(r), held = hov && m_activeId == id && m_ptr.down;
    Rect rr = held ? r.inset(2) : r;
    float rad = rr.h / 2;
    Color textC = t.ink;
    if (t.clunk) {
        Color face = active || held ? Color::Mix(t.accent, Color::Hex("#232528"), 0.14f) : (hov ? Color::Mix(t.accent, Color::Hex("#3c3f43"), 0.18f) : ClunkFace(t));
        rect(rr, 0, face, face, Color::Hex(active || held ? "#4a4d52" : "#5f6368"), 3);
        textC = Color::Hex("#e4e4e4");
    } else if (primary) {
        if (hov) rect(rr, rad, t.accent.withA(0.5f), t.accent.withA(0.5f), Color(0, 0, 0, 0), 0, 18);
        rect(rr, rad, Color::Mix(Color(1, 1, 1), t.moonLight, 0.4f), t.moonDark, Color(1, 1, 1, 0.7f), 1.5f);
        rect(rr, rad, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(1, 1, 1, 0.9f), 2.f, 0, 0.1f);
        textC = t.bg;
    } else {
        if (hov) rect(rr, rad, t.accent.withA(0.25f), t.accent.withA(0.25f), Color(0, 0, 0, 0), 0, 14);
        float a0 = active ? 0.42f : 0.22f, a1 = active ? 0.16f : 0.06f;
        rect(rr, rad, t.accent.withA(a0 + (hov ? 0.08f : 0)), t.accent.withA(a1), Color(225 / 255.f, 232 / 255.f, 1, active ? 0.38f : 0.16f), 1.5f);
        rect(rr, rad, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(1, 1, 1, hov || active ? 0.55f : 0.4f), 2.f, 0, 0.12f);
        if (active) textC = Color(1, 1, 1);
    }
    float fs = rr.h * 0.40f;
    if ((int)ic >= 0 && label && *label) {
        float tw = m_fonts->measure(FONT_REGULAR, label, fs, 0.03f);
        float total = fs * 1.2f + fs * 0.5f + tw;
        float x0 = rr.cx() - total / 2;
        icon(ic, x0 + fs * 0.6f, rr.cy(), fs * 1.2f, textC);
        text(x0 + fs * 1.7f, rr.cy(), label, fs, textC, primary ? FONT_SEMIBOLD : FONT_REGULAR, ALIGN_LEFT, 0.03f);
    } else if ((int)ic >= 0) {
        icon(ic, rr.cx(), rr.cy(), rr.h * 0.5f, textC);
    } else {
        text(rr.cx(), rr.cy(), label, fs, textC, primary ? FONT_SEMIBOLD : FONT_REGULAR, ALIGN_CENTER, 0.03f, Color(0, 0, 0, 0), rr.w - rad);
    }
    return clicked;
}

bool Ui::roundButton(int id, float cx, float cy, float rad, Icon ic, bool primary, bool active, const char *tip) {
    const Theme &t = m_theme;
    Rect r(cx - rad, cy - rad, rad * 2, rad * 2);
    bool clicked = interact(id, r);
    bool hov = hovered(r), held = hov && m_activeId == id && m_ptr.down;
    if (held) rad *= 0.95f;
    Color ic_c = t.ink;
    if (t.clunk) {
        Color face = held || active ? Color::Mix(t.accent, Color::Hex("#232528"), 0.14f) : ClunkFace(t);
        rect(Rect(cx - rad, cy - rad, rad * 2, rad * 2), 0, face, face, Color::Hex("#5f6368"), 3);
        ic_c = Color::Hex("#e4e4e4");
    } else if (primary) {
        circle(cx, cy, rad, t.accent.withA(hov ? 0.85f : 0.6f), t.accent.withA(hov ? 0.85f : 0.6f), Color(0, 0, 0, 0), 0, 22);
        float p[4] = {0, 0, 0, 0};
        quad(Rect(cx - rad, cy - rad, rad * 2, rad * 2), 2, rad, t.moonLight, t.moonDark, t.accent, p, 0, 0, 1, 1, 2);
        ic_c = t.bg;
    } else {
        if (hov) circle(cx, cy, rad, t.accent.withA(0.35f), t.accent.withA(0.35f), Color(0, 0, 0, 0), 0, 14);
        float a0 = active ? 0.42f : 0.22f, a1 = active ? 0.16f : 0.06f;
        circle(cx, cy, rad, t.accent.withA(a0), t.accent.withA(a1), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
        rect(Rect(cx - rad, cy - rad, rad * 2, rad * 2), rad, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(1, 1, 1, hov ? 0.55f : 0.4f), 2.f, 0, 0.12f);
        if (active) ic_c = Color(1, 1, 1);
    }
    icon(ic, cx, cy, rad * 0.85f, ic_c);
    if (tip && hov && !t.clunk) text(cx, cy + rad + 26, tip, 22, t.inkSoft, FONT_REGULAR, ALIGN_CENTER, 0.04f);
    return clicked;
}

bool Ui::tab(int id, const Rect &r, const char *label, bool on) {
    const Theme &t = m_theme;
    bool clicked = interact(id, r);
    bool hov = hovered(r);
    Color c = on ? Color(1, 1, 1) : (hov ? t.ink : t.inkSoft);
    if (t.clunk) {
        Color face = on ? Color::Mix(t.accent, Color::Hex("#232528"), 0.14f) : ClunkFace(t);
        rect(r, 0, face, face, Color::Hex("#5f6368"), 2);
        c = Color::Hex("#e4e4e4");
    } else if (on) {
        rect(r, r.h / 2, t.accent.withA(0.34f), t.accent.withA(0.10f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
        rect(r, r.h / 2, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(1, 1, 1, 0.35f), 2.f, 0, 0.12f);
    }
    text(r.cx(), r.cy(), label, r.h * 0.42f, c, FONT_REGULAR, ALIGN_CENTER, 0.08f);
    return clicked;
}

int Ui::segmented(int id, const Rect &r, const char *const *labels, int count, int sel) {
    const Theme &t = m_theme;
    if (t.clunk) rect(r, 0, ClunkField(t), ClunkField(t), Color::Hex("#4a4d52"), 2);
    else rect(r, r.h / 2, Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.45f), Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.45f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
    float pad = 5, w = (r.w - pad * 2) / count;
    int result = -1;
    for (int i = 0; i < count; i++) {
        Rect b(r.x + pad + i * w, r.y + pad, w, r.h - pad * 2);
        bool clicked = interact(id * 32 + i, b);
        bool hov = hovered(b), on = i == sel;
        Color c = on ? Color(1, 1, 1) : (hov ? t.ink : t.inkSoft);
        if (t.clunk) {
            if (on) rect(b, 0, ClunkSel(t), ClunkSel(t));
            c = Color::Hex("#e4e4e4");
        } else if (on) {
            rect(b, b.h / 2, t.accent.withA(0.30f), t.accent.withA(0.30f), Color(0, 0, 0, 0), 0, 10);
            rect(b, b.h / 2, t.accent.withA(0.40f), t.accent.withA(0.14f), Color(1, 1, 1, 0.4f), 1.5f, 0, 0.2f);
        } else if (hov) {
            rect(b, b.h / 2, t.accent.withA(0.10f), t.accent.withA(0.10f));
        }
        text(b.cx(), b.cy(), labels[i], b.h * 0.44f, c, FONT_REGULAR, ALIGN_CENTER, 0.02f, Color(0, 0, 0, 0), b.w - 8);
        if (clicked) result = i;
    }
    return result;
}

bool Ui::toggle(int id, float x, float y, bool on) {
    const Theme &t = m_theme;
    Rect r(x, y, 72, 40);
    bool clicked = interact(id, r);
    if (t.clunk) {
        rect(r, 0, on ? ClunkSel(t) : ClunkField(t), on ? ClunkSel(t) : ClunkField(t), Color::Hex("#4a4d52"), 2);
        Rect k(on ? r.x + r.w - 36 : r.x + 4, r.y + 4, 32, 32);
        rect(k, 0, ClunkFace(t), ClunkFace(t), Color::Hex("#5f6368"), 2);
    } else {
        rect(r, 20, on ? t.accent.withA(0.55f) : Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.6f), on ? t.accent.withA(0.55f) : Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.6f),
             Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
        float kx = on ? r.x + r.w - 20 : r.x + 20;
        float p[4] = {0.35f, 0, 0, 0};
        quad(Rect(kx - 15, r.cy() - 15, 30, 30), 2, 15, t.moonLight, t.moonDark, t.accent, p, 0, 0, 1, 1, 18);
    }
    return clicked;
}

bool Ui::slider(int id, const Rect &r, float *value, float minV, float maxV) {
    const Theme &t = m_theme;
    Rect hit(r.x - 10, r.y - 16, r.w + 20, r.h + 32);
    interact(id, hit);
    bool changed = false;
    if (m_activeId == id && m_ptr.down && m_ptr.valid) {
        float f = (m_ptr.x - r.x) / r.w;
        f = f < 0 ? 0 : (f > 1 ? 1 : f);
        float nv = minV + f * (maxV - minV);
        if (nv != *value) {
            *value = nv;
            changed = true;
        }
    }
    float f = (*value - minV) / (maxV - minV);
    float cy = r.cy();
    Rect track(r.x, cy - 5, r.w, 10);
    if (t.clunk) {
        rect(track, 0, ClunkField(t), ClunkField(t), Color::Hex("#4a4d52"), 2);
        rect(Rect(r.x + f * r.w - 8, cy - 14, 16, 28), 0, ClunkFace(t), ClunkFace(t), Color::Hex("#5f6368"), 2);
        return changed;
    }
    rect(track, 5, Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.6f), Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.6f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
    if (f > 0) rect(Rect(r.x, cy - 5, fmaxf(10.f, f * r.w), 10), 5, Color(1, 1, 1), t.accent);
    float kx = r.x + f * r.w;
    float p[4] = {0.6f, 0, 0, 0};
    quad(Rect(kx - 13, cy - 13, 26, 26), 2, 13, t.moonLight, t.moonDark, t.accent, p, 0, 0, 1, 1, 18);
    return changed;
}

bool Ui::row(int id, const Rect &r, bool selected) {
    const Theme &t = m_theme;
    bool clicked = interact(id, r);
    bool hov = hovered(r);
    if (t.clunk) {
        if (selected) rect(r, 0, ClunkSel(t), ClunkSel(t));
        else if (hov) rect(r, 0, Color::Mix(t.accent, Color::Hex("#3c3f43"), 0.18f), Color::Mix(t.accent, Color::Hex("#3c3f43"), 0.18f));
    } else if (selected) {
        rect(r, 22, t.accent.withA(0.22f), Color(170 / 255.f, 186 / 255.f, 245 / 255.f, 0.08f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
        rect(r, 22, Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(1, 1, 1, 0.3f), 2.f, 0, 0.1f);
    } else if (hov) {
        rect(r, 22, t.accent.withA(0.08f), t.accent.withA(0.08f));
    }
    return clicked;
}

void Ui::sectionHeader(float x, float y, const char *label) {
    std::string up = label;
    for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
    text(x, y, up.c_str(), 20, m_theme.clunk ? Color::Hex("#e4e4e4") : m_theme.inkSoft, FONT_SEMIBOLD, ALIGN_LEFT, m_theme.clunk ? 0 : 0.2f);
}

void Ui::pill(const Rect &r, const char *label, const Color &dot) {
    const Theme &t = m_theme;
    if (t.clunk) rect(r, 0, ClunkField(t), ClunkField(t), Color::Hex("#4a4d52"), 2);
    else rect(r, r.h / 2, Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.45f), Color(5 / 255.f, 8 / 255.f, 22 / 255.f, 0.45f), Color(225 / 255.f, 232 / 255.f, 1, 0.16f), 1.5f);
    float fs = r.h * 0.38f;
    std::string up = label;
    for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
    if (dot.a > 0) {
        circle(r.x + r.h * 0.55f, r.cy(), 6, dot, dot, Color(0, 0, 0, 0), 0, 0);
        if (!t.clunk) circle(r.x + r.h * 0.55f, r.cy(), 6, dot.withA(0.5f), dot.withA(0.5f), Color(0, 0, 0, 0), 0, 8);
    }
    text(r.x + r.h * (dot.a > 0 ? 0.95f : 0.5f), r.cy(), up.c_str(), fs, t.clunk ? Color::Hex("#e4e4e4") : t.ink, FONT_REGULAR, ALIGN_LEFT,
         t.clunk ? 0 : 0.1f, Color(0, 0, 0, 0), r.w - r.h);
}
