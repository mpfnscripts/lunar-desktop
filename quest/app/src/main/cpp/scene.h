// The Lunar environment (night sky, twinkling stars, big glowing moon, lunar ground) and the
// 3D pieces drawn into each eye: glass panels, the desktop screen, lasers, controller moons, VR stream.
#pragma once
#include <android/asset_manager.h>
#include "common.h"
#include "gfx.h"

class Scene {
public:
    bool init(AAssetManager *am);
    void setTheme(const Theme &t) { m_theme = t; }
    void beginEye(const Mat4 &view, const Mat4 &proj, const Vec3 &camPos);

    void drawEnvironment(float time, bool ground, bool bigMoon, bool stars, float fade = 1.f);
    void drawPanel(GLuint tex, const Mat4 &model, float opacity = 1.f);
    void drawScreen(GLuint extTex, const Mesh &mesh, const Mat4 &model, bool cursorVisible, float cx, float cy, float aspect);
    void drawScreenGlow(const Mat4 &model, float w, float h);
    void drawLaser(const Vec3 &a, const Vec3 &b, float alpha);
    void drawDot(const Vec3 &p, float radius, float alpha);
    void drawOrb(const LPose &grip, float pressed);
    // VR mode: draws one eye of the side-by-side video over the whole viewport
    void drawStreamEye(GLuint extTex, int eye, float u0 = 0, float u1 = 1);
    void clear(const Color &c);

private:
    Theme m_theme;
    Mat4 m_view, m_proj, m_vp;
    Vec3 m_cam;
    GLuint m_skyProg = 0, m_moonProg = 0, m_glowProg = 0, m_groundProg = 0, m_panelProg = 0, m_screenProg = 0, m_lineProg = 0,
           m_streamProg = 0, m_orbProg = 0;
    GLuint m_moonTex = 0, m_glowTex = 0, m_regolithTex = 0;
    Mesh m_sphere, m_quad, m_disc;
    GLuint m_lineVao = 0, m_lineVbo = 0, m_emptyVao = 0;
};
