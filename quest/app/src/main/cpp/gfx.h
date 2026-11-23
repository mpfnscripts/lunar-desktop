// OpenGL ES helpers: shaders, textures from APK assets, simple meshes, render targets.
#pragma once
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/asset_manager.h>
#include <string>
#include <vector>
#include "common.h"

GLuint CompileProgram(const char *vs, const char *fs, const char *name);
GLuint LoadTextureAsset(AAssetManager *am, const char *path, bool mipmaps, bool repeat, int *w = nullptr, int *h = nullptr);
std::vector<uint8_t> ReadAsset(AAssetManager *am, const char *path);

struct Mesh {
    GLuint vao = 0, vbo = 0, ibo = 0;
    GLsizei count = 0;
    // interleaved: pos(3) normal(3) uv(2)
    void upload(const std::vector<float> &v, const std::vector<uint16_t> &idx);
    void draw() const;
    void destroy();
};

Mesh MakeSphere(int segs, int rings);
Mesh MakeQuad();  // -0.5..0.5 in XY, facing +Z, uv 0..1 (v down)
// Curved screen: a section of a cylinder (curve 0 = flat). width/height in metres, centred at origin facing +Z.
Mesh MakeCurvedQuad(float width, float height, float radius, int segs);
Mesh MakeDisc(int segs);

struct RenderTarget {
    GLuint fbo = 0, tex = 0, depth = 0;
    int w = 0, h = 0;
    bool create(int w, int h, bool withDepth, bool mipmaps);
    void destroy();
};

// Extension entry points
extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_;
extern PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT_;
extern PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC glRenderbufferStorageMultisampleEXT_;
void LoadGlExtensions();
