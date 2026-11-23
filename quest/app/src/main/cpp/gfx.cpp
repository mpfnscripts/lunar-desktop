#include "gfx.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "stb_image.h"

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC glFramebufferTexture2DMultisampleEXT_ = nullptr;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC glRenderbufferStorageMultisampleEXT_ = nullptr;

void LoadGlExtensions() {
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    glFramebufferTexture2DMultisampleEXT_ = (PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC)eglGetProcAddress("glFramebufferTexture2DMultisampleEXT");
    glRenderbufferStorageMultisampleEXT_ = (PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC)eglGetProcAddress("glRenderbufferStorageMultisampleEXT");
}

static GLuint Compile(GLenum type, const char *src, const char *name) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader %s (%s) failed: %s", name, type == GL_VERTEX_SHADER ? "vs" : "fs", log);
    }
    return s;
}

GLuint CompileProgram(const char *vs, const char *fs, const char *name) {
    GLuint p = glCreateProgram();
    GLuint v = Compile(GL_VERTEX_SHADER, vs, name), f = Compile(GL_FRAGMENT_SHADER, fs, name);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        LOGE("program %s link failed: %s", name, log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

std::vector<uint8_t> ReadAsset(AAssetManager *am, const char *path) {
    std::vector<uint8_t> out;
    AAsset *a = AAssetManager_open(am, path, AASSET_MODE_BUFFER);
    if (!a) {
        LOGE("missing asset %s", path);
        return out;
    }
    out.resize(AAsset_getLength(a));
    AAsset_read(a, out.data(), out.size());
    AAsset_close(a);
    return out;
}

GLuint LoadTextureAsset(AAssetManager *am, const char *path, bool mipmaps, bool repeat, int *ow, int *oh) {
    auto data = ReadAsset(am, path);
    if (data.empty()) return 0;
    int w, h, n;
    stbi_uc *px = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!px) return 0;
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    if (mipmaps) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return t;
}

void Mesh::upload(const std::vector<float> &v, const std::vector<uint16_t> &idx) {
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 2, idx.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glBindVertexArray(0);
    count = (GLsizei)idx.size();
}

void Mesh::draw() const {
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_SHORT, nullptr);
}

void Mesh::destroy() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (ibo) glDeleteBuffers(1, &ibo);
    if (vao) glDeleteVertexArrays(1, &vao);
    vao = vbo = ibo = 0;
}

Mesh MakeSphere(int segs, int rings) {
    std::vector<float> v;
    std::vector<uint16_t> idx;
    for (int r = 0; r <= rings; r++) {
        float vth = (float)r / rings, phi = vth * (float)M_PI;
        for (int s = 0; s <= segs; s++) {
            float u = (float)s / segs, th = u * 2.f * (float)M_PI;
            float x = sinf(phi) * cosf(th), y = cosf(phi), z = sinf(phi) * sinf(th);
            v.insert(v.end(), {x, y, z, x, y, z, u, vth});
        }
    }
    for (int r = 0; r < rings; r++)
        for (int s = 0; s < segs; s++) {
            uint16_t a = r * (segs + 1) + s, b = a + segs + 1;
            idx.insert(idx.end(), {a, b, (uint16_t)(a + 1), (uint16_t)(a + 1), b, (uint16_t)(b + 1)});
        }
    Mesh m;
    m.upload(v, idx);
    return m;
}

Mesh MakeQuad() {
    std::vector<float> v = {-0.5f, -0.5f, 0, 0, 0, 1, 0, 1, 0.5f, -0.5f, 0, 0, 0, 1, 1, 1,
                            0.5f, 0.5f, 0, 0, 0, 1, 1, 0, -0.5f, 0.5f, 0, 0, 0, 1, 0, 0};
    std::vector<uint16_t> idx = {0, 1, 2, 0, 2, 3};
    Mesh m;
    m.upload(v, idx);
    return m;
}

Mesh MakeCurvedQuad(float width, float height, float radius, int segs) {
    std::vector<float> v;
    std::vector<uint16_t> idx;
    bool flat = radius <= 0.01f;
    float angle = flat ? 0 : width / radius;
    for (int s = 0; s <= segs; s++) {
        float u = (float)s / segs;
        float x, z, nx, nz;
        if (flat) {
            x = (u - 0.5f) * width;
            z = 0;
            nx = 0;
            nz = 1;
        } else {
            float a = (u - 0.5f) * angle;
            x = sinf(a) * radius;
            z = radius - cosf(a) * radius;  // edges bend toward the viewer (+Z)
            nx = -sinf(a);
            nz = cosf(a);
        }
        v.insert(v.end(), {x, -height / 2, z, nx, 0, nz, u, 1});
        v.insert(v.end(), {x, height / 2, z, nx, 0, nz, u, 0});
    }
    for (int s = 0; s < segs; s++) {
        uint16_t a = s * 2;
        idx.insert(idx.end(), {a, (uint16_t)(a + 2), (uint16_t)(a + 3), a, (uint16_t)(a + 3), (uint16_t)(a + 1)});
    }
    Mesh m;
    m.upload(v, idx);
    return m;
}

Mesh MakeDisc(int segs) {
    std::vector<float> v = {0, 0, 0, 0, 1, 0, 0.5f, 0.5f};
    std::vector<uint16_t> idx;
    for (int s = 0; s <= segs; s++) {
        float a = (float)s / segs * 2.f * (float)M_PI;
        v.insert(v.end(), {cosf(a), 0, sinf(a), 0, 1, 0, 0.5f + 0.5f * cosf(a), 0.5f + 0.5f * sinf(a)});
    }
    for (int s = 0; s < segs; s++) idx.insert(idx.end(), {0, (uint16_t)(s + 2), (uint16_t)(s + 1)});
    Mesh m;
    m.upload(v, idx);
    return m;
}

bool RenderTarget::create(int W, int H, bool withDepth, bool mipmaps) {
    destroy();
    w = W;
    h = H;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    int levels = 1;
    if (mipmaps) {
        int m = W > H ? W : H;
        while (m > 1) { m >>= 1; levels++; }
    }
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, W, H);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (withDepth) {
        glGenRenderbuffers(1, &depth);
        glBindRenderbuffer(GL_RENDERBUFFER, depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    }
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return ok;
}

void RenderTarget::destroy() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (tex) glDeleteTextures(1, &tex);
    if (depth) glDeleteRenderbuffers(1, &depth);
    fbo = tex = depth = 0;
}
