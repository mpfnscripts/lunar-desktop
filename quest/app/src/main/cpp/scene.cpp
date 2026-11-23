#include "scene.h"

#define GLSL_HEADER "#version 300 es\nprecision highp float;\n"
#define GLSL_COMMON                                                                                         \
    "vec3 toLin(vec3 c){ return mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(0.04045,c)); }\n"        \
    "float hash(vec3 p){ p=fract(p*0.3183099+0.1); p*=17.0; return fract(p.x*p.y*p.z*(p.x+p.y+p.z)); }\n"

static const char *kMeshVS = GLSL_HEADER R"(
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal; layout(location=2) in vec2 aUV;
uniform mat4 uMVP; uniform mat4 uModel;
out vec3 vWorld; out vec3 vNormal; out vec2 vUV; out vec3 vObj;
void main(){ vec4 w=uModel*vec4(aPos,1.0); vWorld=w.xyz; vNormal=mat3(uModel)*aNormal; vUV=aUV; vObj=aPos; gl_Position=uMVP*vec4(aPos,1.0); })";

// Sky: theme gradient, two light pools and procedural twinkling stars (DESIGN.md §2)
static const char *kSkyFS = GLSL_HEADER GLSL_COMMON R"(
uniform vec3 uTop, uBg, uBottom, uAccent, uAccent2, uMoonDir;
uniform float uTime, uStars, uFade;
in vec3 vObj; out vec4 o;
float starLayer(vec3 d, float scale, float density, float t){
  vec3 p=d*scale; vec3 cell=floor(p); vec3 f=fract(p)-0.5;
  float h=hash(cell);
  if(h<density) return 0.0;
  vec3 off=vec3(hash(cell+1.3),hash(cell+2.7),hash(cell+5.1))-0.5;
  float dist=length(f-off*0.6);
  float px=length(fwidth(p))*0.9;
  float size=mix(0.03,0.10,hash(cell+7.0));
  float r=max(size,px);
  float base=mix(0.2,0.8,hash(cell+11.0))*(size/r);
  float tw=0.55+0.45*sin(t*mix(0.4,2.0,hash(cell+3.0))+h*60.0);
  return (1.0-smoothstep(0.0,r,dist))*base*tw;
}
void main(){
  vec3 d=normalize(vObj);
  float t=clamp(0.5-d.y*0.55,0.0,1.0);
  vec3 c= t<0.45 ? mix(uTop,uBg,t/0.45) : mix(uBg,uBottom,(t-0.45)/0.55);
  float a1=max(dot(d,uMoonDir),0.0);
  c+=uAccent*0.20*pow(a1,6.0);
  vec3 d2=normalize(vec3(-0.55,-0.35,-0.75));
  c+=uAccent2*0.22*pow(max(dot(d,d2),0.0),5.0);
  if(uStars>0.5 && d.y>-0.25){
    float s=starLayer(d,70.0,0.86,uTime)+starLayer(d,150.0,0.90,uTime*1.3)*0.7;
    vec3 tint=mix(vec3(1.0),vec3(201.0,212.0,255.0)/255.0,step(0.86,hash(floor(d*70.0)+9.0)));
    c+=toLin(tint)*s*smoothstep(-0.25,0.05,d.y);
  }
  o=vec4(c*uFade,1.0);
})";

static const char *kMoonFS = GLSL_HEADER GLSL_COMMON R"(
uniform sampler2D uTex; uniform vec3 uLight, uTint, uRim; uniform float uSpin;
in vec3 vNormal; in vec2 vUV; out vec4 o;
void main(){
  vec3 n=normalize(vNormal);
  vec3 tex=texture(uTex,vec2(vUV.x+uSpin,vUV.y)).rgb;
  float diff=clamp(dot(n,uLight)*0.85+0.2,0.0,1.0);
  vec3 c=tex*uTint*diff*1.25;
  o=vec4(c,1.0);
})";

static const char *kBillboardVS = GLSL_HEADER R"(
layout(location=0) in vec3 aPos; layout(location=1) in vec3 aNormal; layout(location=2) in vec2 aUV;
uniform mat4 uMVP; out vec2 vUV;
void main(){ vUV=aUV; gl_Position=uMVP*vec4(aPos,1.0); })";

// mode 0: glow texture; mode 1: soft rounded-rect glow (uSize = half size, uRadius)
static const char *kGlowFS = GLSL_HEADER R"(
uniform sampler2D uTex; uniform vec3 uColor; uniform float uAlpha; uniform int uMode; uniform vec2 uSize; uniform float uRadius;
in vec2 vUV; out vec4 o;
void main(){
  float a;
  if(uMode==0){ a=texture(uTex,vUV).a; }
  else {
    vec2 p=(vUV-0.5)*2.0*(uSize+vec2(uRadius*2.0));
    vec2 q=abs(p)-uSize+uRadius; float d=length(max(q,0.0))+min(max(q.x,q.y),0.0)-uRadius;
    a=exp(-max(d,0.0)*8.0)*(d>0.0?1.0:0.0);
  }
  o=vec4(uColor*a*uAlpha, 0.0);
})";

static const char *kGroundFS = GLSL_HEADER GLSL_COMMON R"(
uniform sampler2D uTex; uniform vec3 uBase, uHi, uAccent2, uCam; uniform float uFade;
in vec3 vWorld; out vec4 o;
void main(){
  vec2 uv=vWorld.xz*0.11;
  float h=texture(uTex,uv).r*0.7+texture(uTex,uv*0.27+0.3).r*0.3;
  vec3 c=mix(uBase,uHi,h);
  float dist=length(vWorld.xz-uCam.xz);
  c+=uAccent2*0.10*(1.0-smoothstep(0.0,6.0,dist));
  float a=(1.0-smoothstep(8.0,45.0,dist))*uFade;
  o=vec4(c*a,a);
})";

static const char *kPanelFS = GLSL_HEADER GLSL_COMMON R"(
uniform sampler2D uTex; uniform float uOpacity;
in vec2 vUV; out vec4 o;
void main(){
  vec4 c=texture(uTex,vec2(vUV.x,1.0-vUV.y));
  if(c.a>0.0) c.rgb=toLin(c.rgb/c.a)*c.a;
  o=c*uOpacity;
})";

static const char *kScreenFS = "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;\n" GLSL_COMMON R"(
uniform samplerExternalOES uTex; uniform vec3 uCursor; uniform float uAspect; uniform vec3 uAccent;
in vec2 vUV; out vec4 o;
void main(){
  vec3 c=toLin(texture(uTex,vUV).rgb);
  if(uCursor.z>0.5){
    vec2 d=(vUV-uCursor.xy)*vec2(uAspect,1.0);
    float r=length(d);
    float dot=1.0-smoothstep(0.0045,0.0060,r);
    float ring=(1.0-smoothstep(0.009,0.0105,r))*smoothstep(0.0065,0.008,r);
    float glow=exp(-r*180.0)*0.6;
    c=mix(c,vec3(1.0),dot);
    c=mix(c,uAccent,ring*0.9);
    c+=uAccent*glow;
  }
  vec2 e=min(vUV,1.0-vUV);
  float edge=smoothstep(0.0,0.002,min(e.x,e.y*uAspect));
  o=vec4(c*edge,edge);
})";

static const char *kLineVS = GLSL_HEADER R"(
layout(location=0) in vec4 aPos; uniform mat4 uVP; out float vA;
void main(){ vA=aPos.w; gl_Position=uVP*vec4(aPos.xyz,1.0); })";
static const char *kLineFS = GLSL_HEADER R"(
uniform vec3 uColor; in float vA; out vec4 o;
void main(){ o=vec4(uColor*vA, vA); })";

static const char *kStreamVS = GLSL_HEADER R"(
uniform vec2 uURange; out vec2 vUV;
void main(){
  vec2 p=vec2((gl_VertexID<<1)&2, gl_VertexID&2);
  vec2 pos=p*2.0-1.0;
  vec2 uv=vec2(p.x, 1.0-p.y);
  vUV=vec2(mix(uURange.x,uURange.y,uv.x), uv.y);
  gl_Position=vec4(pos,0.0,1.0);
})";
static const char *kStreamFS = "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;\n" GLSL_COMMON R"(
uniform samplerExternalOES uTex; in vec2 vUV; out vec4 o;
void main(){ o=vec4(toLin(texture(uTex,vUV).rgb),1.0); })";

static const char *kOrbFS = GLSL_HEADER GLSL_COMMON R"(
uniform vec3 uLight, uMoonLight, uMoonDark, uAccent, uCam; uniform float uPressed;
in vec3 vNormal; in vec3 vWorld; out vec4 o;
void main(){
  vec3 n=normalize(vNormal);
  float l=clamp(dot(n,uLight)*0.5+0.5,0.0,1.0);
  vec3 c=mix(uMoonDark,uMoonLight,l);
  c=mix(c,vec3(1.0),pow(l,8.0)*0.6);
  vec3 v=normalize(uCam-vWorld);
  float rim=pow(1.0-max(dot(n,v),0.0),2.0);
  c+=uAccent*rim*(0.6+uPressed);
  o=vec4(c,1.0);
})";

static Vec3 Lin(const Color &c) {
    auto f = [](float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
    return Vec3(f(c.r), f(c.g), f(c.b));
}

static void U3(GLuint p, const char *n, const Vec3 &v) { glUniform3f(glGetUniformLocation(p, n), v.x, v.y, v.z); }
static void U1(GLuint p, const char *n, float v) { glUniform1f(glGetUniformLocation(p, n), v); }
static void UM(GLuint p, const char *n, const Mat4 &m) { glUniformMatrix4fv(glGetUniformLocation(p, n), 1, GL_FALSE, m.m); }

static const Vec3 kMoonDir = Vec3(0.4096f, 0.5736f, -0.7094f);

bool Scene::init(AAssetManager *am) {
    m_skyProg = CompileProgram(kMeshVS, kSkyFS, "sky");
    m_moonProg = CompileProgram(kMeshVS, kMoonFS, "moon");
    m_glowProg = CompileProgram(kBillboardVS, kGlowFS, "glow");
    m_groundProg = CompileProgram(kMeshVS, kGroundFS, "ground");
    m_panelProg = CompileProgram(kBillboardVS, kPanelFS, "panel");
    m_screenProg = CompileProgram(kBillboardVS, kScreenFS, "screen");
    m_lineProg = CompileProgram(kLineVS, kLineFS, "line");
    m_streamProg = CompileProgram(kStreamVS, kStreamFS, "stream");
    m_orbProg = CompileProgram(kMeshVS, kOrbFS, "orb");
    m_moonTex = LoadTextureAsset(am, "textures/moon.jpg", true, true);
    m_glowTex = LoadTextureAsset(am, "textures/glow.png", true, false);
    m_regolithTex = LoadTextureAsset(am, "textures/regolith.jpg", true, true);
    m_sphere = MakeSphere(48, 24);
    m_quad = MakeQuad();
    m_disc = MakeDisc(64);
    glGenVertexArrays(1, &m_lineVao);
    glGenBuffers(1, &m_lineVbo);
    glBindVertexArray(m_lineVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * 6 * 4, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
    glGenVertexArrays(1, &m_emptyVao);
    glBindVertexArray(0);
    return m_skyProg && m_panelProg;
}

void Scene::beginEye(const Mat4 &view, const Mat4 &proj, const Vec3 &cam) {
    m_view = view;
    m_proj = proj;
    m_vp = proj * view;
    m_cam = cam;
}

void Scene::clear(const Color &c) {
    Vec3 l = Lin(c);
    glClearColor(l.x, l.y, l.z, 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static Mat4 Billboard(const Vec3 &p, const Vec3 &cam, float sx, float sy) {
    Vec3 f = (cam - p).norm();
    Vec3 up(0, 1, 0);
    Vec3 r = up.cross(f).norm();
    if (r.len() < 0.01f) r = Vec3(1, 0, 0);
    Vec3 u = f.cross(r);
    Mat4 m = Mat4::Identity();
    m.m[0] = r.x * sx; m.m[1] = r.y * sx; m.m[2] = r.z * sx;
    m.m[4] = u.x * sy; m.m[5] = u.y * sy; m.m[6] = u.z * sy;
    m.m[8] = f.x; m.m[9] = f.y; m.m[10] = f.z;
    m.m[12] = p.x; m.m[13] = p.y; m.m[14] = p.z;
    return m;
}

void Scene::drawEnvironment(float time, bool ground, bool bigMoon, bool stars, float fade) {
    const Theme &t = m_theme;
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // sky dome follows the head (infinitely far away)
    glUseProgram(m_skyProg);
    Mat4 model = Mat4::Translate(m_cam) * Mat4::Scale(Vec3(80, 80, 80));
    UM(m_skyProg, "uMVP", m_vp * model);
    UM(m_skyProg, "uModel", model);
    U3(m_skyProg, "uTop", Lin(t.skyTop));
    U3(m_skyProg, "uBg", Lin(t.bg));
    U3(m_skyProg, "uBottom", Lin(t.skyBottom));
    U3(m_skyProg, "uAccent", Lin(t.accent));
    U3(m_skyProg, "uAccent2", Lin(t.accent2));
    U3(m_skyProg, "uMoonDir", kMoonDir);
    U1(m_skyProg, "uTime", fmodf(time, 3600.f));
    U1(m_skyProg, "uStars", stars ? 1.f : 0.f);
    U1(m_skyProg, "uFade", fade);
    m_sphere.draw();

    glEnable(GL_BLEND);
    if (bigMoon) {
        Vec3 mp = Vec3(0, 1.6f, 0) + kMoonDir * 30.f;
        // halo
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(m_glowProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_glowTex);
        glUniform1i(glGetUniformLocation(m_glowProg, "uTex"), 0);
        glUniform1i(glGetUniformLocation(m_glowProg, "uMode"), 0);
        U3(m_glowProg, "uColor", Lin(t.accent));
        U1(m_glowProg, "uAlpha", 0.55f * fade);
        UM(m_glowProg, "uMVP", m_vp * Billboard(mp - kMoonDir * 0.5f, m_cam, 20.f, 20.f));
        m_quad.draw();
        // the moon itself
        glDisable(GL_BLEND);
        glUseProgram(m_moonProg);
        glBindTexture(GL_TEXTURE_2D, m_moonTex);
        glUniform1i(glGetUniformLocation(m_moonProg, "uTex"), 0);
        Mat4 mm = Mat4::Translate(mp) * Mat4::Scale(Vec3(4, 4, 4));
        UM(m_moonProg, "uMVP", m_vp * mm);
        UM(m_moonProg, "uModel", mm);
        Vec3 fwd = kMoonDir * -1.f;
        Vec3 right = Vec3(0, 1, 0).cross(fwd).norm(), up = fwd.cross(right);
        U3(m_moonProg, "uLight", (right * -0.55f + up * 0.45f + fwd * 0.7f).norm());
        U3(m_moonProg, "uTint", Lin(t.moonLight) * fade);
        U1(m_moonProg, "uSpin", fmodf(time / 360.f, 1.f));
        m_sphere.draw();
        glEnable(GL_BLEND);
    }

    if (ground) {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(m_groundProg);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_regolithTex);
        glUniform1i(glGetUniformLocation(m_groundProg, "uTex"), 0);
        Mat4 gm = Mat4::Translate(Vec3(m_cam.x, 0, m_cam.z)) * Mat4::Scale(Vec3(60, 1, 60));
        UM(m_groundProg, "uMVP", m_vp * gm);
        UM(m_groundProg, "uModel", gm);
        U3(m_groundProg, "uBase", Lin(t.bg) * 0.55f);
        U3(m_groundProg, "uHi", Lin(t.moonDark) * 0.22f);
        U3(m_groundProg, "uAccent2", Lin(t.accent2));
        U3(m_groundProg, "uCam", m_cam);
        U1(m_groundProg, "uFade", fade);
        m_disc.draw();
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void Scene::drawPanel(GLuint tex, const Mat4 &model, float opacity) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUseProgram(m_panelProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(m_panelProg, "uTex"), 0);
    U1(m_panelProg, "uOpacity", opacity);
    UM(m_panelProg, "uMVP", m_vp * model);
    m_quad.draw();
    glDepthMask(GL_TRUE);
}

void Scene::drawScreenGlow(const Mat4 &model, float w, float h) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glUseProgram(m_glowProg);
    glUniform1i(glGetUniformLocation(m_glowProg, "uMode"), 1);
    float pad = 0.25f;
    glUniform2f(glGetUniformLocation(m_glowProg, "uSize"), w / 2, h / 2);
    U1(m_glowProg, "uRadius", pad / 2);
    U3(m_glowProg, "uColor", Lin(m_theme.accent));
    U1(m_glowProg, "uAlpha", 0.35f);
    UM(m_glowProg, "uMVP", m_vp * model * Mat4::Translate(Vec3(0, 0, -0.01f)) * Mat4::Scale(Vec3(w + pad * 2, h + pad * 2, 1)));
    m_quad.draw();
    glDepthMask(GL_TRUE);
}

void Scene::drawScreen(GLuint extTex, const Mesh &mesh, const Mat4 &model, bool cursorVisible, float cx, float cy, float aspect) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_screenProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, extTex);
    glUniform1i(glGetUniformLocation(m_screenProg, "uTex"), 0);
    U3(m_screenProg, "uCursor", Vec3(cx, cy, cursorVisible ? 1.f : 0.f));
    U1(m_screenProg, "uAspect", aspect);
    U3(m_screenProg, "uAccent", Lin(m_theme.accent));
    UM(m_screenProg, "uMVP", m_vp * model);
    mesh.draw();
}

void Scene::drawLaser(const Vec3 &a, const Vec3 &b, float alpha) {
    Vec3 dir = (b - a).norm();
    Vec3 toCam = (m_cam - (a + b) * 0.5f).norm();
    Vec3 side = dir.cross(toCam).norm() * 0.0015f;
    float v[] = {
        a.x - side.x, a.y - side.y, a.z - side.z, 0.0f,          a.x + side.x, a.y + side.y, a.z + side.z, 0.0f,
        b.x + side.x, b.y + side.y, b.z + side.z, alpha,         a.x - side.x, a.y - side.y, a.z - side.z, 0.0f,
        b.x + side.x, b.y + side.y, b.z + side.z, alpha,         b.x - side.x, b.y - side.y, b.z - side.z, alpha,
    };
    // fade in from the controller, strongest near the hit point
    v[3] = v[7] = v[15] = alpha * 0.15f;
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glUseProgram(m_lineProg);
    UM(m_lineProg, "uVP", m_vp);
    U3(m_lineProg, "uColor", Lin(m_theme.moonLight));
    glBindVertexArray(m_lineVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
}

void Scene::drawDot(const Vec3 &p, float radius, float alpha) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_glowProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_glowTex);
    glUniform1i(glGetUniformLocation(m_glowProg, "uTex"), 0);
    glUniform1i(glGetUniformLocation(m_glowProg, "uMode"), 0);
    U3(m_glowProg, "uColor", Lin(m_theme.moonLight));
    U1(m_glowProg, "uAlpha", alpha);
    UM(m_glowProg, "uMVP", m_vp * Billboard(p, m_cam, radius * 2, radius * 2));
    m_quad.draw();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void Scene::drawOrb(const LPose &grip, float pressed) {
    glDisable(GL_BLEND);
    glUseProgram(m_orbProg);
    Vec3 p(grip.position);
    // sit the moon just above the controller ring
    Vec3 up(lqRotate(grip.orientation, LVec3{0, 0.02f, 0.03f}));
    p = p + up;
    Mat4 m = Mat4::Translate(p) * Mat4::Scale(Vec3(0.018f, 0.018f, 0.018f));
    UM(m_orbProg, "uMVP", m_vp * m);
    UM(m_orbProg, "uModel", m);
    U3(m_orbProg, "uLight", Vec3(-0.5f, 0.7f, 0.5f).norm());
    U3(m_orbProg, "uMoonLight", Lin(m_theme.moonLight));
    U3(m_orbProg, "uMoonDark", Lin(m_theme.moonDark));
    U3(m_orbProg, "uAccent", Lin(m_theme.accent));
    U3(m_orbProg, "uCam", m_cam);
    U1(m_orbProg, "uPressed", pressed);
    m_sphere.draw();
    drawDot(p, 0.05f + pressed * 0.03f, 0.35f + pressed * 0.3f);
}

void Scene::drawStreamEye(GLuint extTex, int eye, float u0, float u1) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glUseProgram(m_streamProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, extTex);
    glUniform1i(glGetUniformLocation(m_streamProg, "uTex"), 0);
    float half = (u1 - u0) / 2;
    float a = u0 + eye * half;
    glUniform2f(glGetUniformLocation(m_streamProg, "uURange"), a, a + half);
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}
