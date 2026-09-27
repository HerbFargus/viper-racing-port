// viperport -- milestone M2, stage 2: the game's DirectDraw and Direct3D, emulated on OpenGL 3.3.
//
// race.exe's DirectDrawCreate / DirectDrawEnumerateA imports are pointed here, and the game gets objects
// that implement exactly the DirectX 5 methods it calls (out/agents/r2d, r3d, rtex report.md list them).
// No game function is replaced, so every build (v1.0, v1.1, 1.2.x) works the same and every quirk of
// the game's own graphics code is kept. Everything else is a generated "unsupported" method that logs
// its first call (com_base.h).
//
//   screen    the render target is the window's full size. The game still thinks in its display mode
//             (640x480...): its viewports, clears and screen-space vertices are scaled into the 4:3 middle
//             of the target, and a viewport as wide as the game's screen spreads over the whole width with
//             its clip window widened to match (Hor+ widescreen: more to the sides, nothing cropped).
//             Lock of the back buffer hands the game's software 2D the middle, shrunk to its own
//             resolution; Unlock lays back only the pixels the 2D changed, so the 3D under the HUD stays
//             at full resolution
//   3D        D3DLVERTEX / D3DTLVERTEX triangle lists through one shader that reproduces the D3D5
//             fixed-function path the game uses: texture blend (decal / modulate / modulatealpha),
//             specular add, vertex fog from the specular alpha, colour key, alpha test, blending; the
//             D3DVIEWPORT2 clip window mapped exactly; flat or Gouraud shading
//   textures  every surface keeps its pixels in CPU memory (system and "video" copies alike); a video
//             texture becomes a GL texture on first use and re-uploads whenever a level changes. Nothing
//             is ever lost, so Alt-Tab no longer leaves garbage.
#define _CRT_SECURE_NO_WARNINGS
#include "dx5.h"
#include "com_base.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <set>
#include "SDL.h"
#include "SDL_opengl.h"
#include "viperport.h"
#include "port.h"

// ---- from the platform layer (platform.cpp) ----------------------------------------------------------
SDL_Window* platform_window();
void platform_set_view(int x0, int y0, int w, int h, int game_w, int game_h);

void com_unsupported(const char* method) {
    static std::set<const char*> seen;
    if (seen.insert(method).second) logf("ddraw_gl: the game called %s, which isn't emulated", method);
}

namespace {

// ---- OpenGL 3.3 entry points ---------------------------------------------------------------------------
#define GLFN(ret, name, args) typedef ret(APIENTRY* name##_t) args; name##_t p_##name;
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(GLuint, glCreateProgram, (void))
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))
GLFN(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*))
GLFN(void, glGenBuffers, (GLsizei, GLuint*))
GLFN(void, glBindBuffer, (GLenum, GLuint))
GLFN(void, glBufferData, (GLenum, ptrdiff_t, const void*, GLenum))
GLFN(void, glGenVertexArrays, (GLsizei, GLuint*))
GLFN(void, glBindVertexArray, (GLuint))
GLFN(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))
GLFN(void, glEnableVertexAttribArray, (GLuint))
GLFN(void, glGenFramebuffers, (GLsizei, GLuint*))
GLFN(void, glDeleteFramebuffers, (GLsizei, const GLuint*))
GLFN(void, glBindFramebuffer, (GLenum, GLuint))
GLFN(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint))
GLFN(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint))
GLFN(GLenum, glCheckFramebufferStatus, (GLenum))
GLFN(void, glGenRenderbuffers, (GLsizei, GLuint*))
GLFN(void, glDeleteRenderbuffers, (GLsizei, const GLuint*))
GLFN(void, glBindRenderbuffer, (GLenum, GLuint))
GLFN(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei))
GLFN(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
GLFN(void, glActiveTexture, (GLenum))
GLFN(void, glProvokingVertex, (GLenum))
#undef GLFN

bool load_gl() {
    bool ok = true;
#define LOAD(name) ok = ok && (p_##name = (name##_t)SDL_GL_GetProcAddress(#name)) != 0;
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog)
    LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glBindAttribLocation) LOAD(glLinkProgram) LOAD(glGetProgramiv)
    LOAD(glGetProgramInfoLog) LOAD(glUseProgram) LOAD(glGetUniformLocation) LOAD(glUniform1i) LOAD(glUniform1f) LOAD(glUniform2f)
    LOAD(glUniform3f) LOAD(glUniform4f) LOAD(glUniformMatrix4fv) LOAD(glGenBuffers) LOAD(glBindBuffer)
    LOAD(glBufferData) LOAD(glGenVertexArrays) LOAD(glBindVertexArray) LOAD(glVertexAttribPointer)
    LOAD(glEnableVertexAttribArray) LOAD(glGenFramebuffers) LOAD(glDeleteFramebuffers) LOAD(glBindFramebuffer)
    LOAD(glFramebufferTexture2D) LOAD(glFramebufferRenderbuffer) LOAD(glCheckFramebufferStatus)
    LOAD(glGenRenderbuffers) LOAD(glDeleteRenderbuffers) LOAD(glBindRenderbuffer) LOAD(glRenderbufferStorage)
    LOAD(glBlitFramebuffer) LOAD(glActiveTexture) LOAD(glProvokingVertex)
#undef LOAD
    return ok;
}

// the enums come from SDL_opengl_glext.h

// ---- the renderer ----------------------------------------------------------------------------------------
const char* VS = R"(
in vec3 aPos; in float aRhw; in vec4 aCol; in vec4 aSpec; in vec2 aUV;
uniform mat4 uM;          // WORLD x VIEW x PROJECTION, D3D row-vector order
uniform vec4 uClip;       // the D3DVIEWPORT2 clip window: ClipX, ClipY, ClipWidth, ClipHeight
uniform int uTL;          // 1: D3DTLVERTEX, in the game's screen pixels
uniform vec2 uRT;         // render-target size
uniform vec2 uMap;        // game pixels -> target pixels: scale, x offset
SHADE out vec4 vCol; SHADE out vec4 vSpec; out vec2 vUV;
void main() {
    if (uTL == 1) {
        vec2 t = vec2(uMap.y + aPos.x * uMap.x, aPos.y * uMap.x);
        gl_Position = vec4(t.x * 2.0 / uRT.x - 1.0, t.y * 2.0 / uRT.y - 1.0, aPos.z * 2.0 - 1.0, 1.0);
    } else {
        vec4 p = uM * vec4(aPos, 1.0);
        // D3D: sx = X + W (x/w - ClipX) / ClipW, sy = Y + H (ClipY - y/w) / ClipH, sz = z/w -- and the
        // render target's rows run top-down (row 0 = GL window y 0), so y needs no flip beyond D3D's own
        gl_Position = vec4(2.0 * (p.x - uClip.x * p.w) / uClip.z - p.w,
                           2.0 * (uClip.y * p.w - p.y) / uClip.w - p.w,
                           2.0 * p.z - p.w, p.w);
    }
    vCol = aCol; vSpec = aSpec; vUV = aUV;
}
)";

const char* FS = R"(
SHADE in vec4 vCol; SHADE in vec4 vSpec; in vec2 vUV;
uniform sampler2D uTex;
uniform int uTexOn, uBlend, uTexAlpha, uKeyed, uSpec, uFog, uAlphaFunc;
uniform float uAlphaRef;
uniform vec3 uFogCol;
out vec4 frag;
bool passes(float a) {                    // D3DCMPFUNC on 0..255 alpha
    float x = floor(a * 255.0 + 0.5), r = uAlphaRef;
    if (uAlphaFunc == 1) return false;    if (uAlphaFunc == 2) return x < r;
    if (uAlphaFunc == 3) return x == r;   if (uAlphaFunc == 4) return x <= r;
    if (uAlphaFunc == 5) return x > r;    if (uAlphaFunc == 6) return x != r;
    if (uAlphaFunc == 7) return x >= r;   return true;
}
void main() {
    vec4 c = vCol;
    if (uTexOn == 1) {
        vec4 t = texture(uTex, vUV);
        if (uKeyed == 1 && t.a < 0.5) discard;                     // COLORKEYENABLE on a keyed texture
        if (uBlend == 1) c = t;                                     // DECAL
        else if (uBlend == 4) c = t * vCol;                         // MODULATEALPHA
        else c = vec4(t.rgb * vCol.rgb, uTexAlpha == 1 ? t.a : vCol.a);   // MODULATE
    }
    if (uSpec == 1) c.rgb = min(c.rgb + vSpec.rgb, vec3(1.0));
    if (uFog == 1) c.rgb = mix(uFogCol, c.rgb, vSpec.a);           // vertex fog: specular alpha
    if (uAlphaFunc != 8 && !passes(c.a)) discard;                  // ALPHATESTENABLE
    frag = c;
}
)";

const char* COMP_VS = R"(
in vec2 aPos; out vec2 vUV;
void main() { vUV = vec2(aPos.x * 0.5 + 0.5, aPos.y * 0.5 + 0.5); gl_Position = vec4(aPos, 0.0, 1.0); }
)";
const char* COMP_FS = R"(
in vec2 vUV; uniform sampler2D uTex; out vec4 frag;
void main() { frag = texture(uTex, vUV); }    // premultiplied: only what the 2D drew is opaque
)";

struct Program {
    GLuint id = 0;
    GLint uM, uClip, uTL, uRT, uMap, uTex, uTexOn, uBlend, uTexAlpha, uKeyed, uSpec, uFog, uAlphaFunc, uAlphaRef, uFogCol;
};

struct Vertex32 { float x, y, z, w; uint32_t color, specular; float u, v; };  // D3D(T)LVERTEX, 32 bytes

struct Surface;

struct GL {
    bool ready = false;
    DWORD thread = 0;
    SDL_GLContext ctx = 0;
    Program smooth, flat;
    GLuint comp = 0, comp_tex_uniform = 0;
    GLuint vao = 0, vbo = 0, ibo = 0, quad_vao = 0, quad_vbo = 0;
    GLuint fbo = 0, fbo_color = 0, fbo_depth = 0, page_tex = 0;
    GLuint small_fbo = 0, small_color = 0;           // the middle of the target at the game's resolution
    int w = 640, h = 480;                            // the game's display mode
    int rt_w = 0, rt_h = 0;                          // the render target: the window's drawable size
    float scale = 1, ox = 0;                         // game pixels -> target pixels (x offset of the 4:3 middle)
    std::vector<uint16_t> page;                      // the back buffer as the game's 2D sees it (565)
    std::vector<uint16_t> under;                     // the page as it was handed out: what the 2D changed shows
    std::vector<uint32_t> overlay;                   // the 2D's pixels, premultiplied RGBA
    bool page_locked = false;
    // D3D device state
    DWORD rs[64];
    D3DMATRIX world, view, proj;
    D3DVIEWPORT2 vp;
    std::set<Surface*> textures;                     // live texture surfaces, to validate handles
    uint64_t frames = 0, draws = 0, triangles = 0;
} g;

void set_defaults() {                                // the D3D5 device's initial render states
    memset(g.rs, 0, sizeof g.rs);
    g.rs[D3DRENDERSTATE_TEXTUREADDRESS] = D3DTADDRESS_WRAP;
    g.rs[D3DRENDERSTATE_TEXTUREADDRESSU] = D3DTADDRESS_WRAP;
    g.rs[D3DRENDERSTATE_TEXTUREADDRESSV] = D3DTADDRESS_WRAP;
    g.rs[D3DRENDERSTATE_TEXTUREPERSPECTIVE] = 1;
    g.rs[D3DRENDERSTATE_ZENABLE] = 1;
    g.rs[D3DRENDERSTATE_FILLMODE] = D3DFILL_SOLID;
    g.rs[D3DRENDERSTATE_SHADEMODE] = D3DSHADE_GOURAUD;
    g.rs[D3DRENDERSTATE_ZWRITEENABLE] = 1;
    g.rs[D3DRENDERSTATE_TEXTUREMAG] = D3DFILTER_NEAREST;
    g.rs[D3DRENDERSTATE_TEXTUREMIN] = D3DFILTER_NEAREST;
    g.rs[D3DRENDERSTATE_SRCBLEND] = D3DBLEND_ONE;
    g.rs[D3DRENDERSTATE_DESTBLEND] = D3DBLEND_ZERO;
    g.rs[D3DRENDERSTATE_TEXTUREMAPBLEND] = D3DTBLEND_MODULATE;
    g.rs[D3DRENDERSTATE_CULLMODE] = D3DCULL_CCW;
    g.rs[D3DRENDERSTATE_ZFUNC] = D3DCMP_LESSEQUAL;
    g.rs[D3DRENDERSTATE_ALPHAFUNC] = D3DCMP_ALWAYS;
    D3DMATRIX id = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    g.world = g.view = g.proj = id;
    memset(&g.vp, 0, sizeof g.vp);
    g.vp.dwWidth = g.w, g.vp.dwHeight = g.h;
    g.vp.dvClipX = -1, g.vp.dvClipWidth = 2, g.vp.dvClipY = 1, g.vp.dvClipHeight = 2, g.vp.dvMaxZ = 1;
}

GLuint compile(GLenum type, const char* prelude, const char* src) {
    GLuint s = p_glCreateShader(type);
    const char* parts[2] = {prelude, src};
    p_glShaderSource(s, 2, parts, 0);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(s, sizeof log, 0, log);
        logf("ddraw_gl: shader compile failed: %s", log);
    }
    return s;
}

GLuint link(const char* prelude, const char* vs, const char* fs, bool full) {
    GLuint p = p_glCreateProgram();
    p_glAttachShader(p, compile(GL_VERTEX_SHADER, prelude, vs));
    p_glAttachShader(p, compile(GL_FRAGMENT_SHADER, prelude, fs));
    if (full) {
        p_glBindAttribLocation(p, 0, "aPos"); p_glBindAttribLocation(p, 1, "aRhw");
        p_glBindAttribLocation(p, 2, "aCol"); p_glBindAttribLocation(p, 3, "aSpec"); p_glBindAttribLocation(p, 4, "aUV");
    } else {
        p_glBindAttribLocation(p, 0, "aPos");
    }
    p_glLinkProgram(p);
    GLint ok = 0;
    p_glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(p, sizeof log, 0, log);
        logf("ddraw_gl: shader link failed: %s", log);
    }
    return p;
}

void make_program(Program& pr, bool flat) {
    const char* prelude = flat ? "#version 330 core\n#define SHADE flat\n" : "#version 330 core\n#define SHADE\n";
    pr.id = link(prelude, VS, FS, true);
#define U(n) pr.n = p_glGetUniformLocation(pr.id, #n);
    U(uM) U(uClip) U(uTL) U(uRT) U(uMap) U(uTex) U(uTexOn) U(uBlend) U(uTexAlpha) U(uKeyed) U(uSpec) U(uFog) U(uAlphaFunc)
    U(uAlphaRef) U(uFogCol)
#undef U
}

void make_target();

void resize_target(int w, int h) {                   // SetDisplayMode: the game's own resolution
    g.w = w, g.h = h;
    g.page.assign((size_t)w * h, 0);
    g.under.assign((size_t)w * h, 0);
    g.overlay.assign((size_t)w * h, 0);
    if (!g.ready) return;
    if (g.small_fbo) p_glDeleteFramebuffers(1, &g.small_fbo), glDeleteTextures(1, &g.small_color);
    glGenTextures(1, &g.small_color);
    glBindTexture(GL_TEXTURE_2D, g.small_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    p_glGenFramebuffers(1, &g.small_fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.small_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.small_color, 0);
    glBindTexture(GL_TEXTURE_2D, g.page_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    make_target();
    logf("ddraw_gl: display mode %dx%d, drawn at %dx%d", w, h, g.rt_w, g.rt_h);
}

void make_target() {                                 // the render target at the window's size
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(platform_window(), &ww, &wh);
    if (ww <= 0 || wh <= 0) ww = g.w, wh = g.h;
    g.scale = (float)wh / g.h;
    if (g.w * g.scale > ww) g.scale = (float)ww / g.w;   // a window narrower than 4:3: fit the width
    g.ox = (ww - g.w * g.scale) * 0.5f;
    if (ww == g.rt_w && wh == g.rt_h && g.fbo) return;
    g.rt_w = ww, g.rt_h = wh;
    if (g.fbo) {
        p_glDeleteFramebuffers(1, &g.fbo);
        glDeleteTextures(1, &g.fbo_color);
        p_glDeleteRenderbuffers(1, &g.fbo_depth);
    }
    glGenTextures(1, &g.fbo_color);
    glBindTexture(GL_TEXTURE_2D, g.fbo_color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ww, wh, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glGenRenderbuffers(1, &g.fbo_depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g.fbo_depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ww, wh);
    p_glGenFramebuffers(1, &g.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.fbo_color, 0);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g.fbo_depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) logf("ddraw_gl: render target incomplete");
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClearDepth(1.0);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

// a rectangle in the game's screen pixels -> the target's; one as wide as the game's screen spans the
// target's whole width (the widescreen sides)
struct Rect { int x, y, w, h; bool full; };
Rect map_rect(int x, int y, int w, int h) {
    Rect r;
    r.full = x <= 0 && x + w >= g.w;
    r.x = r.full ? 0 : (int)floorf(g.ox + x * g.scale + 0.5f);
    r.w = r.full ? g.rt_w : (int)floorf(g.ox + (x + w) * g.scale + 0.5f) - r.x;
    r.y = (int)floorf(y * g.scale + 0.5f);
    r.h = (int)floorf((y + h) * g.scale + 0.5f) - r.y;
    return r;
}

// the clip window for the target: widened by the extra width when the viewport spans the screen
void clip_window(const D3DVIEWPORT2& vp, float out[4]) {
    Rect r = map_rect(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight);
    float k = r.full ? (float)r.w / (vp.dwWidth * g.scale) : 1.0f;
    out[0] = vp.dvClipX * k, out[1] = vp.dvClipY, out[2] = vp.dvClipWidth * k, out[3] = vp.dvClipHeight;
}

bool gl_start() {
    if (g.ready) return true;
    SDL_Window* win = platform_window();
    if (!win) {
        logf("ddraw_gl: no SDL window (viperport.ini [platform] sdl=1 is needed)");
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    g.ctx = SDL_GL_CreateContext(win);
    if (!g.ctx) {
        logf("ddraw_gl: can't create an OpenGL 3.3 context: %s", SDL_GetError());
        return false;
    }
    if (!load_gl()) {
        logf("ddraw_gl: OpenGL 3.3 functions missing");
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    g.thread = GetCurrentThreadId();
    logf("ddraw_gl: OpenGL %s on %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    make_program(g.smooth, false);
    make_program(g.flat, true);
    g.comp = link("#version 330 core\n", COMP_VS, COMP_FS, false);
    g.comp_tex_uniform = p_glGetUniformLocation(g.comp, "uTex");
    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    p_glGenBuffers(1, &g.vbo);
    p_glGenBuffers(1, &g.ibo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void*)0);
    p_glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 32, (void*)12);
    p_glVertexAttribPointer(2, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void*)16);
    p_glVertexAttribPointer(3, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, 32, (void*)20);
    p_glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, 32, (void*)24);
    for (int i = 0; i < 5; i++) p_glEnableVertexAttribArray(i);
    static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
    p_glGenVertexArrays(1, &g.quad_vao);
    p_glBindVertexArray(g.quad_vao);
    p_glGenBuffers(1, &g.quad_vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.quad_vbo);
    p_glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, 0x88E4 /*GL_STATIC_DRAW*/);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void*)0);
    p_glEnableVertexAttribArray(0);
    glGenTextures(1, &g.page_tex);
    glBindTexture(GL_TEXTURE_2D, g.page_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);           // D3D flat shading takes the first vertex
    g.ready = true;
    set_defaults();
    resize_target(g.w, g.h);
    return true;
}

bool on_gl_thread() {
    if (!g.ready) return false;
    if (GetCurrentThreadId() == g.thread) return true;
    static bool said;
    if (!said) logf("ddraw_gl: a drawing call from another thread (%lu) was skipped", GetCurrentThreadId()), said = true;
    return false;
}

// ---- surfaces ------------------------------------------------------------------------------------------------
enum Format { F565, F555, F1555, F4444 };

void pixel_format(Format f, DDPIXELFORMAT& pf) {
    memset(&pf, 0, sizeof pf);
    pf.dwSize = sizeof pf;
    pf.dwFlags = DDPF_RGB;
    pf.dwRGBBitCount = 16;
    switch (f) {
    case F565: pf.dwRBitMask = 0xF800, pf.dwGBitMask = 0x07E0, pf.dwBBitMask = 0x001F; break;
    case F555: pf.dwRBitMask = 0x7C00, pf.dwGBitMask = 0x03E0, pf.dwBBitMask = 0x001F; break;
    case F1555: pf.dwFlags |= DDPF_ALPHAPIXELS; pf.dwRBitMask = 0x7C00, pf.dwGBitMask = 0x03E0, pf.dwBBitMask = 0x001F;
                pf.dwRGBAlphaBitMask = 0x8000; break;
    case F4444: pf.dwFlags |= DDPF_ALPHAPIXELS; pf.dwRBitMask = 0x0F00, pf.dwGBitMask = 0x00F0, pf.dwBBitMask = 0x000F;
                pf.dwRGBAlphaBitMask = 0xF000; break;
    }
}

Format format_of(const DDPIXELFORMAT& pf) {
    if (pf.dwFlags & DDPF_ALPHAPIXELS) return pf.dwRGBAlphaBitMask == 0xF000 ? F4444 : F1555;
    return pf.dwGBitMask == 0x03E0 ? F555 : F565;
}

// ---- shadow checks (port.h) -----------------------------------------------------------------------------------
// A shadow check runs a graphics function twice -- the original, then the rewrite -- and every call either
// makes into these objects is recorded (shadow_com_effect) and compared. The original's pass runs live, and
// what each call hands back (out-parameters, return values, new objects, a Lock's pixels) is queued. The
// rewrite's pass makes no call at all: it's handed the queued results in order, so it sees exactly what the
// original saw, and nothing is drawn twice. A rewrite that calls differently shows up as differing outputs.
enum ComId : uint16_t {
    C_QI = 1, C_ADDREF, C_RELEASE,
    C_T_HANDLE, C_T_LOAD, C_T_PALCHANGED,
    C_S_DESC, C_S_PIXFMT, C_S_CAPS, C_S_ISLOST, C_S_RESTORE, C_S_FLIPSTATUS, C_S_BLTSTATUS, C_S_ADDATT, C_S_DELATT,
    C_S_GETATT, C_S_SETKEY, C_S_GETKEY, C_S_LOCK, C_S_UNLOCK, C_S_FLIP, C_S_BLT, C_S_BLTFAST,
    C_M_SET, C_M_GET, C_M_HANDLE,
    C_V_SET, C_V_GET, C_V_BACKGROUND, C_V_CLEAR, C_V_TRANSFORM,
    C_D_CAPS, C_D_STATS, C_D_ADDVP, C_D_DELVP, C_D_SETVP, C_D_GETVP, C_D_BEGIN, C_D_END, C_D_ENUMTEX, C_D_SETRS,
    C_D_GETRS, C_D_SETLS, C_D_SETXF, C_D_GETXF, C_D_DRAW, C_D_DRAWIDX, C_D_DRAWIDX_I,
    C_3_MATERIAL, C_3_VIEWPORT, C_3_DEVICE,
    C_DD_CAPS, C_DD_VIDMEM, C_DD_COOP, C_DD_ENUMMODES, C_DD_SETMODE, C_DD_RESTOREMODE, C_DD_GETMODE, C_DD_VBLANK,
    C_DD_SURFACE,
};

// tex: the texture handle this pass has bound (its own SetRenderState calls; the emulation's state has already
// moved on to the original's by the rewrite's pass), from tex0, the handle when the check began
struct ComFeed { unsigned check = ~0u; int ph = 0; std::vector<uint8_t> q; size_t at = 0; DWORD tex0 = 0, tex = 0; };
static __declspec(thread) ComFeed* t_feed;
static void feed_sync(int ph) {
    if (!t_feed) t_feed = new ComFeed;
    unsigned c = shadow_com_check();
    if (t_feed->check != c) {
        t_feed->check = c; t_feed->q.clear(); t_feed->at = 0; t_feed->ph = ph;
        t_feed->tex = t_feed->tex0 = g.rs[D3DRENDERSTATE_TEXTUREHANDLE];
    } else if (t_feed->ph != ph) { t_feed->ph = ph; t_feed->tex = t_feed->tex0; }
}

// 0 outside a check, 1 the original's pass (run live, queue the results), 2 the rewrite's (take them)
int com_begin(ComId id, const void* self, const void* args, size_t nargs, const void* data = 0, size_t ndata = 0) {
    int ph = shadow_com_phase();
    if (!ph) return 0;
    feed_sync(ph);
    shadow_com_effect(id, self, args, nargs, data, ndata);
    return ph;
}
// the vertices as a check records them: without what Direct3D never reads -- an LVERTEX's reserved dword, and u/v
// with no texture bound -- which the game leaves as stack garbage (its triangles are built in locals)
static const void* draw_record(D3DVERTEXTYPE vt, const void* v, DWORD n, std::vector<uint8_t>& buf) {
    int ph = shadow_com_phase();
    if (!ph || !v) return v;
    feed_sync(ph);
    bool lv = vt == D3DVT_LVERTEX, notex = t_feed->tex == 0;
    if (!lv && !notex) return v;
    buf.assign((const uint8_t*)v, (const uint8_t*)v + (size_t)n * 32);
    for (size_t i = 0; i < n; i++) {
        uint8_t* p = &buf[i * 32];
        if (lv) memset(p + 12, 0, 4);
        if (notex) memset(p + 24, 0, 8);
    }
    return buf.data();
}
void com_save(const void* p, size_t n) {
    uint32_t k = p ? (uint32_t)n : 0;
    std::vector<uint8_t>& q = t_feed->q;
    q.insert(q.end(), (const uint8_t*)&k, (const uint8_t*)&k + 4);
    if (k) q.insert(q.end(), (const uint8_t*)p, (const uint8_t*)p + k);
}
void com_take(void* p, size_t n) {
    std::vector<uint8_t>& q = t_feed->q;
    uint32_t k = 0;
    if (t_feed->at + 4 <= q.size()) { memcpy(&k, &q[t_feed->at], 4); t_feed->at += 4; }
    if (t_feed->at + k > q.size()) k = 0;
    if (p && n) {
        size_t m = k < n ? k : n;
        memcpy(p, &q[t_feed->at], m);
        if (m < n) memset((uint8_t*)p + m, 0, n - m);
    }
    t_feed->at += k;
}
// an effect: live once; the rewrite's pass is handed the original's return value
template <class R, class F> R effect(int ph, F live) {
    R r = R();
    if (ph == 2) { com_take(&r, sizeof r); return r; }
    r = live();
    if (ph == 1) com_save(&r, sizeof r);
    return r;
}
// a query: live once; what it wrote (the listed out-parameters) is handed to the rewrite's pass too
struct Out { void* p; size_t n; };
template <class F> HRESULT query(int ph, std::initializer_list<Out> outs, F live) {
    HRESULT r = DD_OK;
    if (ph == 2) {
        com_take(&r, sizeof r);
        for (const Out& o : outs) com_take(o.p, o.p ? o.n : 0);
        return r;
    }
    r = live();
    if (ph == 1) {
        com_save(&r, sizeof r);
        for (const Out& o : outs) com_save(o.p, o.n);
    }
    return r;
}
struct A1 { uint32_t a; };
struct A2 { uint32_t a, b; };
struct A3 { uint32_t a, b, c; };
struct A4 { uint32_t a, b, c, d; };
struct A5 { uint32_t a, b, c, d, e; };
#define U(x) ((uint32_t)(uintptr_t)(x))

void present();                                        // Flip (below)

struct Surface;
struct Texture2 : Base_IDirect3DTexture2 {
    Surface* s;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2, LPD3DTEXTUREHANDLE h) override;
    HRESULT STDMETHODCALLTYPE Load(LPDIRECT3DTEXTURE2 src) override;
    HRESULT load(LPDIRECT3DTEXTURE2 src);
    HRESULT STDMETHODCALLTYPE PaletteChanged(DWORD a, DWORD b) override {
        A2 x = {a, b};
        return effect<HRESULT>(com_begin(C_T_PALCHANGED, this, &x, sizeof x), [&] { return (HRESULT)DD_OK; });
    }
};

struct Surface : Base_IDirectDrawSurface3 {
    LONG refs = 1;
    DDSCAPS caps = {};
    DWORD flags = 0;                     // DDSD_* it was created with
    int w = 0, h = 0, backbuffers = 0, mip_count = 1;
    Format fmt = F565;
    std::vector<uint16_t> pixels;
    Surface* root = 0;                   // the first level of a mip chain (itself for the root)
    Surface* next_mip = 0;
    Surface* back = 0;                   // a primary's back buffer
    bool is_back = false;
    bool keyed = false;
    uint16_t key = 0;
    uint32_t gen = 1;                    // bumped whenever the pixels change
    GLuint tex = 0;                      // the GL texture (a root that was ever drawn with)
    std::vector<uint32_t> uploaded;      // the gen of each level at its last upload
    Texture2 t2;

    Surface() { t2.s = this; }
    ~Surface() {
        if (tex && g.ready) glDeleteTextures(1, &tex);
        g.textures.erase(this);
        delete next_mip;                 // a chain's levels belong to its root
        if (back) back->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override {
        return query(com_begin(C_QI, this, &riid, sizeof riid), {{out, sizeof *out}}, [&] {
            if (riid == IID_IDirect3DTexture2) { *out = &t2; add_ref(); return (HRESULT)DD_OK; }
            if (riid == IID_IDirectDrawSurface || riid == IID_IDirectDrawSurface2 || riid == IID_IDirectDrawSurface3 ||
                riid == IID_IUnknown) { *out = this; add_ref(); return (HRESULT)DD_OK; }
            *out = 0;
            return (HRESULT)E_NOINTERFACE;
        });
    }
    ULONG add_ref() { return root && root != this ? 1 : InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE AddRef() override { return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return add_ref(); }); }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&]() -> ULONG {
            if (root && root != this) return 1;                   // levels live as long as their root
            LONG n = InterlockedDecrement(&refs);
            if (n == 0) delete this;
            return (ULONG)n;
        });
    }

    void describe(LPDDSURFACEDESC d) {
        DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
        memset(d, 0, size);
        d->dwSize = size;
        d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        d->dwWidth = w, d->dwHeight = h;
        d->lPitch = w * 2;
        d->ddsCaps = caps;
        if (backbuffers) d->dwFlags |= DDSD_BACKBUFFERCOUNT, d->dwBackBufferCount = backbuffers;
        if (mip_count > 1) d->dwFlags |= DDSD_MIPMAPCOUNT, d->dwMipMapCount = mip_count;
        if (caps.dwCaps & DDSCAPS_ZBUFFER) {
            d->dwFlags = (d->dwFlags & ~DDSD_PIXELFORMAT) | DDSD_ZBUFFERBITDEPTH;
            d->dwZBufferBitDepth = 16;
        } else {
            pixel_format(fmt, d->ddpfPixelFormat);
        }
        if (keyed) d->dwFlags |= DDSD_CKSRCBLT, d->ddckCKSrcBlt.dwColorSpaceLowValue = d->ddckCKSrcBlt.dwColorSpaceHighValue = key;
    }

    HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC d) override {
        DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
        return query(com_begin(C_S_DESC, this, &size, 4), {{d, size}}, [&] { describe(d); return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT pf) override {
        return query(com_begin(C_S_PIXFMT, this, 0, 0), {{pf, sizeof *pf}}, [&] { pixel_format(fmt, *pf); return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS c) override {
        return query(com_begin(C_S_CAPS, this, 0, 0), {{c, sizeof *c}}, [&] { *c = caps; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE IsLost() override { return query(com_begin(C_S_ISLOST, this, 0, 0), {}, [] { return (HRESULT)DD_OK; }); }
    HRESULT STDMETHODCALLTYPE Restore() override { return effect<HRESULT>(com_begin(C_S_RESTORE, this, 0, 0), [] { return (HRESULT)DD_OK; }); }
    HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD f) override {
        return query(com_begin(C_S_FLIPSTATUS, this, &f, 4), {}, [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD f) override {
        return query(com_begin(C_S_BLTSTATUS, this, &f, 4), {}, [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE3 a) override {   // the z-buffer
        A1 x = {U(a)};
        return effect<HRESULT>(com_begin(C_S_ADDATT, this, &x, sizeof x), [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD f, LPDIRECTDRAWSURFACE3 a) override {
        A2 x = {f, U(a)};
        return effect<HRESULT>(com_begin(C_S_DELATT, this, &x, sizeof x), [] { return (HRESULT)DD_OK; });
    }

    HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS c, LPDIRECTDRAWSURFACE3* out) override {
        return query(com_begin(C_S_GETATT, this, c, sizeof *c), {{out, sizeof *out}}, [&] {
            if ((c->dwCaps & DDSCAPS_BACKBUFFER) && back) { *out = back; back->add_ref(); return (HRESULT)DD_OK; }
            if ((c->dwCaps & DDSCAPS_MIPMAP) && next_mip) { *out = next_mip; return (HRESULT)DD_OK; }
            *out = 0;
            return (HRESULT)DDERR_NOTFOUND;
        });
    }

    HRESULT STDMETHODCALLTYPE SetColorKey(DWORD f, LPDDCOLORKEY k) override {
        A2 x = {f, k ? (uint32_t)k->dwColorSpaceLowValue : 0xFFFFFFFFu};
        return effect<HRESULT>(com_begin(C_S_SETKEY, this, &x, sizeof x), [&] {
            keyed = k != 0;
            key = k ? (uint16_t)k->dwColorSpaceLowValue : 0;
            gen++;
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetColorKey(DWORD f, LPDDCOLORKEY k) override {
        return query(com_begin(C_S_GETKEY, this, &f, 4), {{k, sizeof *k}}, [&] {
            if (!keyed) return (HRESULT)DDERR_NOCOLORKEY;
            k->dwColorSpaceLowValue = k->dwColorSpaceHighValue = key;
            return (HRESULT)DD_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE Lock(LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) override;
    // a check compares the pixels as each pass leaves them at the Unlock (its Lock handed both passes the same ones)
    const void* lk_p = 0;
    size_t lk_n = 0;
    HRESULT STDMETHODCALLTYPE Unlock(LPVOID p) override {
        A1 x = {U(p)};
        int ph = shadow_com_phase();
        return effect<HRESULT>(com_begin(C_S_UNLOCK, this, &x, sizeof x, ph ? lk_p : 0, ph ? lk_n : 0),
                               [&] { return unlock(); });
    }
    HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE3 t, DWORD f) override {
        A2 x = {U(t), f};
        return effect<HRESULT>(com_begin(C_S_FLIP, this, &x, sizeof x), [&] { present(); return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE Blt(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD f, LPDDBLTFX fx) override {
        // (pointers to the caller's structures are recorded by what they hold: the two passes' stacks differ)
        struct { RECT d, s; uint32_t src, f, has; } x = {dst ? *dst : RECT{}, srcr ? *srcr : RECT{}, U(src), f,
                                                         (dst ? 1u : 0u) | (srcr ? 2u : 0u) | (fx ? 4u : 0u)};
        return effect<HRESULT>(com_begin(C_S_BLT, this, &x, sizeof x, fx, fx ? fx->dwSize : 0),
                               [&] { return blt(dst, src, srcr); });
    }
    HRESULT STDMETHODCALLTYPE BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcr, DWORD f) override {
        struct { RECT s; uint32_t x, y, src, f, has; } a = {srcr ? *srcr : RECT{}, x, y, U(src), f, srcr ? 1u : 0u};
        return effect<HRESULT>(com_begin(C_S_BLTFAST, this, &a, sizeof a), [&] { return blt_fast(x, y, src, srcr); });
    }
    HRESULT lock(LPDDSURFACEDESC d);
    HRESULT unlock();
    HRESULT blt(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcr);
    HRESULT blt_fast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcr);

    void copy_from(Surface* s, int sx, int sy, int dx, int dy, int cw, int ch) {
        for (int y = 0; y < ch; y++) {
            int ty = dy + y, fy = sy + y;
            if (ty < 0 || ty >= h || fy < 0 || fy >= s->h) continue;
            for (int x = 0; x < cw; x++) {
                int tx = dx + x, fx = sx + x;
                if (tx >= 0 && tx < w && fx >= 0 && fx < s->w) pixels[ty * w + tx] = s->pixels[fy * s->w + fx];
            }
        }
        gen++;
    }
};

HRESULT STDMETHODCALLTYPE Texture2::QueryInterface(REFIID riid, LPVOID* out) { return s->QueryInterface(riid, out); }
ULONG STDMETHODCALLTYPE Texture2::AddRef() { return s->AddRef(); }
ULONG STDMETHODCALLTYPE Texture2::Release() { return s->Release(); }
HRESULT STDMETHODCALLTYPE Texture2::GetHandle(LPDIRECT3DDEVICE2 dev, LPD3DTEXTUREHANDLE h) {
    A1 x = {U(dev)};
    return query(com_begin(C_T_HANDLE, this, &x, sizeof x), {{h, sizeof *h}}, [&] {
        *h = (D3DTEXTUREHANDLE)(uintptr_t)s;
        g.textures.insert(s);
        return (HRESULT)DD_OK;
    });
}
HRESULT STDMETHODCALLTYPE Texture2::Load(LPDIRECT3DTEXTURE2 src) {
    A1 x = {U(src)};
    return effect<HRESULT>(com_begin(C_T_LOAD, this, &x, sizeof x), [&] { return load(src); });
}
HRESULT Texture2::load(LPDIRECT3DTEXTURE2 src) {    // copy every level, as D3D does
    Surface* from = ((Texture2*)src)->s;
    for (Surface* d = s; d && from; d = d->next_mip, from = from->next_mip)
        if (d->w == from->w && d->h == from->h) d->pixels = from->pixels, d->gen++;
        else d->copy_from(from, 0, 0, 0, 0, d->w, d->h);
    return DD_OK;
}

// ---- the render target and the page the game's 2D draws on ------------------------------------------------
void state_dirty();

void read_page() {                                    // Lock of the back buffer: the 3D as it stands
    if (!on_gl_thread()) return;
    // the 4:3 middle, shrunk to the game's resolution (the 2D reads it: alpha pastes, XOR)
    int x0 = (int)floorf(g.ox + 0.5f), x1 = (int)floorf(g.ox + g.w * g.scale + 0.5f);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.small_fbo);
    glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(x0, 0, x1, g.rt_h, 0, 0, g.w, g.h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.small_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 2);
    glReadPixels(0, 0, g.w, g.h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, g.page.data());
    g.under = g.page;
}

void draw_page() {                                    // Unlock: what the 2D drew, over the full-resolution 3D
    if (!on_gl_thread()) return;
    size_t n = g.page.size(), changed = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = g.page[i];
        if (c == g.under[i]) { g.overlay[i] = 0; continue; }
        uint32_t r = ((c >> 11) & 31) * 255 / 31, gg = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        g.overlay[i] = 0xFF000000u | (b << 16) | (gg << 8) | r;
        changed++;
    }
    if (!changed) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    int x0 = (int)floorf(g.ox + 0.5f), x1 = (int)floorf(g.ox + g.w * g.scale + 0.5f);
    glViewport(x0, 0, x1 - x0, (int)floorf(g.h * g.scale + 0.5f));
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.page_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g.w, g.h, GL_RGBA, GL_UNSIGNED_BYTE, g.overlay.data());
    p_glUseProgram(g.comp);
    p_glUniform1i(g.comp_tex_uniform, 0);
    p_glBindVertexArray(g.quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    state_dirty();
}

void present() {
    if (!on_gl_thread()) return;
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    glDisable(GL_SCISSOR_TEST);
    // the target's row 0 is the top of the picture; the window's is the bottom
    p_glBlitFramebuffer(0, 0, g.rt_w, g.rt_h, 0, g.rt_h, g.rt_w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SDL_GL_SwapWindow(platform_window());
    int x0 = (int)floorf(g.ox + 0.5f), x1 = (int)floorf(g.ox + g.w * g.scale + 0.5f);
    platform_set_view(x0, 0, x1 - x0, (int)floorf(g.h * g.scale + 0.5f), g.w, g.h);   // for the mouse
    // the sides only get drawn by a full-width 3D view; anything else there would be stale
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glScissor(0, 0, x0, g.rt_h);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(x1, 0, g.rt_w - x1, g.rt_h);
    glClear(GL_COLOR_BUFFER_BIT);
    make_target();                                     // follows the window if it changed size
    g.frames++;
    state_dirty();
}

// Lock hands out pixels: the rewrite's pass gets the same pointer, and the pixels the original's pass got
HRESULT STDMETHODCALLTYPE Surface::Lock(LPRECT r, LPDDSURFACEDESC d, DWORD f, HANDLE e) {
    struct { RECT r; uint32_t f, e, has; } x = {r ? *r : RECT{}, f, U(e), r ? 1u : 0u};
    int ph = com_begin(C_S_LOCK, this, &x, sizeof x);
    DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
    if (ph == 2) {
        HRESULT hr = DD_OK;
        com_take(&hr, sizeof hr);
        com_take(d, size);
        size_t n = d->lpSurface ? (size_t)d->lPitch * d->dwHeight : 0;
        com_take(n ? d->lpSurface : 0, n);
        lk_p = d->lpSurface, lk_n = n;
        return hr;
    }
    HRESULT hr = lock(d);
    if (ph == 1) {
        size_t n = d->lpSurface ? (size_t)d->lPitch * d->dwHeight : 0;
        com_save(&hr, sizeof hr);
        com_save(d, size);
        com_save(d->lpSurface, n);
        lk_p = d->lpSurface, lk_n = n;
    }
    return hr;
}

HRESULT Surface::lock(LPDDSURFACEDESC d) {
    describe(d);
    if (is_back) {
        read_page();
        d->lpSurface = g.page.data();
        d->lPitch = g.w * 2;
        d->dwWidth = g.w, d->dwHeight = g.h;
        g.page_locked = true;
    } else {
        d->lpSurface = pixels.data();
    }
    d->dwFlags |= DDSD_LPSURFACE;
    return DD_OK;
}

HRESULT Surface::unlock() {
    if (is_back && g.page_locked) {
        g.page_locked = false;
        draw_page();
    } else {
        gen++;
    }
    return DD_OK;
}

HRESULT Surface::blt(LPRECT dst, LPDIRECTDRAWSURFACE3 src, LPRECT srcr) {
    Surface* s = (Surface*)src;
    if (!s) { com_unsupported("IDirectDrawSurface3::Blt (colour fill)"); return DD_OK; }
    RECT d = dst ? *dst : RECT{0, 0, w, h}, r = srcr ? *srcr : RECT{0, 0, s->w, s->h};
    int dw = d.right - d.left, dh = d.bottom - d.top, sw = r.right - r.left, sh = r.bottom - r.top;
    if (dw == sw && dh == sh) {
        copy_from(s, r.left, r.top, d.left, d.top, dw, dh);
    } else {                                                      // stretched: nearest, as the cards did
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                int fx = r.left + x * sw / dw, fy = r.top + y * sh / dh, tx = d.left + x, ty = d.top + y;
                if (tx >= 0 && tx < w && ty >= 0 && ty < h && fx >= 0 && fx < s->w && fy >= 0 && fy < s->h)
                    pixels[ty * w + tx] = s->pixels[fy * s->w + fx];
            }
        gen++;
    }
    return DD_OK;
}

HRESULT Surface::blt_fast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE3 src, LPRECT srcr) {
    Surface* s = (Surface*)src;
    RECT r = srcr ? *srcr : RECT{0, 0, s->w, s->h};
    copy_from(s, r.left, r.top, (int)x, (int)y, r.right - r.left, r.bottom - r.top);
    return DD_OK;
}

// a texture's levels as a GL texture, re-uploaded when any level changed since
GLuint texture_for(Surface* root) {
    int n = 0;
    for (Surface* l = root; l; l = l->next_mip) n++;
    bool fresh = !root->tex;
    if (fresh) glGenTextures(1, &root->tex);
    glBindTexture(GL_TEXTURE_2D, root->tex);
    if (fresh) glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, n - 1);
    root->uploaded.resize(n, 0);
    int i = 0;
    std::vector<uint32_t> rgba;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    for (Surface* l = root; l; l = l->next_mip, i++) {
        if (root->uploaded[i] == l->gen) continue;
        root->uploaded[i] = l->gen;
        const uint16_t* p = l->pixels.data();
        if (root->keyed && (l->fmt == F565 || l->fmt == F555)) {
            // the colour key becomes alpha 0 (the shader discards it); the texel's own colour otherwise
            rgba.resize((size_t)l->w * l->h);
            for (size_t k = 0; k < rgba.size(); k++) {
                uint16_t c = p[k];
                if (c == root->key) { rgba[k] = 0; continue; }
                uint32_t r, gg, b;
                if (l->fmt == F565) r = (c >> 11) & 31, gg = (c >> 5) & 63, b = c & 31, gg = gg * 255 / 63;
                else r = (c >> 10) & 31, gg = (c >> 5) & 31, b = c & 31, gg = gg * 255 / 31;
                rgba[k] = 0xFF000000u | ((b * 255 / 31) << 16) | (gg << 8) | (r * 255 / 31);
            }
            glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else if (l->fmt == F4444) {
            glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, p);
        } else if (l->fmt == F1555 || l->fmt == F555) {
            glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, l->w, l->h, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, p);
        } else {
            glTexImage2D(GL_TEXTURE_2D, i, GL_RGB8, l->w, l->h, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, p);
        }
    }
    return root->tex;
}

// ---- drawing -------------------------------------------------------------------------------------------------
struct Applied { bool valid = false; DWORD rs[64]; } applied;
void state_dirty() { applied.valid = false; }

GLenum blend_factor(DWORD b) {
    switch (b) {
    case D3DBLEND_ZERO: return GL_ZERO;
    case D3DBLEND_ONE: return GL_ONE;
    case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

GLenum depth_func(DWORD f) {
    static const GLenum m[] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 9 ? m[f] : GL_LEQUAL;
}

GLint min_filter(DWORD f, bool mips) {
    switch (f) {
    case D3DFILTER_LINEAR: return GL_LINEAR;
    case D3DFILTER_MIPNEAREST: return mips ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    case D3DFILTER_MIPLINEAR: return mips ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR;
    case D3DFILTER_LINEARMIPNEAREST: return mips ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST;
    case D3DFILTER_LINEARMIPLINEAR: return mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
    }
    return GL_NEAREST;
}

void mat_mul(const D3DMATRIX& a, const D3DMATRIX& b, float* out) {     // row-major a x b
    const float* x = &a._11;
    const float* y = &b._11;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = x[r * 4] * y[c] + x[r * 4 + 1] * y[4 + c] + x[r * 4 + 2] * y[8 + c] + x[r * 4 + 3] * y[12 + c];
}

void draw(D3DVERTEXTYPE vtype, const void* verts, DWORD nverts, const WORD* idx, DWORD nidx) {
    if (!on_gl_thread()) return;
    if (vtype != D3DVT_LVERTEX && vtype != D3DVT_TLVERTEX) { com_unsupported("DrawPrimitive with D3DVT_VERTEX"); return; }
    const DWORD* rs = g.rs;
    Program& pr = rs[D3DRENDERSTATE_SHADEMODE] == D3DSHADE_FLAT ? g.flat : g.smooth;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glUseProgram(pr.id);
    p_glBindVertexArray(g.vao);
    bool tl = vtype == D3DVT_TLVERTEX;
    // viewport: D3D maps into the viewport rect and clips to it, TL vertices included
    Rect r = map_rect(g.vp.dwX, g.vp.dwY, g.vp.dwWidth, g.vp.dwHeight);
    if (tl) glViewport(0, 0, g.rt_w, g.rt_h);
    else glViewport(r.x, r.y, r.w, r.h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(r.x, r.y, r.w, r.h);
    float clip[4];
    clip_window(g.vp, clip);
    p_glUniform1i(pr.uTL, tl);
    p_glUniform2f(pr.uRT, (float)g.rt_w, (float)g.rt_h);
    p_glUniform2f(pr.uMap, g.scale, g.ox);
    p_glUniform4f(pr.uClip, clip[0], clip[1], clip[2], clip[3]);
    float wvp[16], wv[16];
    D3DMATRIX t;
    mat_mul(g.world, g.view, wv);
    memcpy(&t, wv, sizeof t);
    mat_mul(t, g.proj, wvp);
    p_glUniformMatrix4fv(pr.uM, 1, GL_FALSE, wvp);
    // render states
    if (rs[D3DRENDERSTATE_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_factor(rs[D3DRENDERSTATE_SRCBLEND]), blend_factor(rs[D3DRENDERSTATE_DESTBLEND]));
    } else {
        glDisable(GL_BLEND);
    }
    if (rs[D3DRENDERSTATE_ZENABLE]) glEnable(GL_DEPTH_TEST), glDepthFunc(depth_func(rs[D3DRENDERSTATE_ZFUNC]));
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[D3DRENDERSTATE_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);
    // D3D fronts are clockwise on screen; the target's rows run top-down, so on GL's y-up window they're CCW
    glFrontFace(GL_CCW);
    if (rs[D3DRENDERSTATE_CULLMODE] == D3DCULL_NONE) glDisable(GL_CULL_FACE);
    else glEnable(GL_CULL_FACE), glCullFace(rs[D3DRENDERSTATE_CULLMODE] == D3DCULL_CW ? GL_FRONT : GL_BACK);
    DWORD blend = rs[D3DRENDERSTATE_TEXTUREMAPBLEND];
    p_glUniform1i(pr.uBlend, blend == D3DTBLEND_DECAL || blend == D3DTBLEND_COPY ? 1 : blend == D3DTBLEND_MODULATEALPHA ? 4 : 2);
    p_glUniform1i(pr.uSpec, rs[D3DRENDERSTATE_SPECULARENABLE] ? 1 : 0);
    p_glUniform1i(pr.uFog, rs[D3DRENDERSTATE_FOGENABLE] ? 1 : 0);
    DWORD fc = rs[D3DRENDERSTATE_FOGCOLOR];
    p_glUniform3f(pr.uFogCol, ((fc >> 16) & 255) / 255.0f, ((fc >> 8) & 255) / 255.0f, (fc & 255) / 255.0f);
    p_glUniform1i(pr.uAlphaFunc, rs[D3DRENDERSTATE_ALPHATESTENABLE] ? (int)rs[D3DRENDERSTATE_ALPHAFUNC] : 8);
    p_glUniform1f(pr.uAlphaRef, (float)(rs[D3DRENDERSTATE_ALPHAREF] & 0xff));
    // texture
    Surface* tex = (Surface*)(uintptr_t)rs[D3DRENDERSTATE_TEXTUREHANDLE];
    if (tex && g.textures.count(tex)) {
        p_glActiveTexture(GL_TEXTURE0);
        texture_for(tex);
        bool mips = tex->next_mip != 0;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(rs[D3DRENDERSTATE_TEXTUREMIN], mips));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, rs[D3DRENDERSTATE_TEXTUREMAG] == D3DFILTER_NEAREST ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, rs[D3DRENDERSTATE_TEXTUREADDRESSU] == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE
                        : rs[D3DRENDERSTATE_TEXTUREADDRESSU] == D3DTADDRESS_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, rs[D3DRENDERSTATE_TEXTUREADDRESSV] == D3DTADDRESS_CLAMP ? GL_CLAMP_TO_EDGE
                        : rs[D3DRENDERSTATE_TEXTUREADDRESSV] == D3DTADDRESS_MIRROR ? GL_MIRRORED_REPEAT : GL_REPEAT);
        float bias;
        memcpy(&bias, &rs[D3DRENDERSTATE_MIPMAPLODBIAS], 4);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
        p_glUniform1i(pr.uTex, 0);
        p_glUniform1i(pr.uTexOn, 1);
        p_glUniform1i(pr.uTexAlpha, tex->fmt == F4444 || tex->fmt == F1555);
        p_glUniform1i(pr.uKeyed, rs[D3DRENDERSTATE_COLORKEYENABLE] && tex->keyed ? 1 : 0);
    } else {
        p_glUniform1i(pr.uTexOn, 0);
        p_glUniform1i(pr.uKeyed, 0);
    }
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (ptrdiff_t)nverts * 32, verts, GL_STREAM_DRAW);
    if (idx) {
        p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ibo);
        p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (ptrdiff_t)nidx * 2, idx, GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, nidx, GL_UNSIGNED_SHORT, 0);
        g.triangles += nidx / 3;
    } else {
        glDrawArrays(GL_TRIANGLES, 0, nverts);
        g.triangles += nverts / 3;
    }
    g.draws++;
}

// ---- Direct3D objects ----------------------------------------------------------------------------------------
struct Material : Base_IDirect3DMaterial2 {
    LONG refs = 1;
    D3DMATERIAL m = {};
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(LPD3DMATERIAL p) override {
        return effect<HRESULT>(com_begin(C_M_SET, this, 0, 0, p, sizeof *p), [&] { m = *p; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetMaterial(LPD3DMATERIAL p) override {
        return query(com_begin(C_M_GET, this, 0, 0), {{p, sizeof *p}}, [&] { *p = m; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2 dev, LPD3DMATERIALHANDLE h) override {
        A1 x = {U(dev)};
        return query(com_begin(C_M_HANDLE, this, &x, sizeof x), {{h, sizeof *h}}, [&] {
            *h = (D3DMATERIALHANDLE)(uintptr_t)this;
            return (HRESULT)DD_OK;
        });
    }
};

struct Viewport : Base_IDirect3DViewport2 {
    LONG refs = 1;
    D3DVIEWPORT2 vp = {};
    Material* background = 0;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }
    HRESULT STDMETHODCALLTYPE SetViewport2(LPD3DVIEWPORT2 v) override {
        return effect<HRESULT>(com_begin(C_V_SET, this, 0, 0, v, sizeof *v), [&] { vp = *v; g.vp = vp; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetViewport2(LPD3DVIEWPORT2 v) override {
        DWORD size = v->dwSize ? v->dwSize : sizeof *v;
        return query(com_begin(C_V_GET, this, &size, 4), {{v, sizeof *v}}, [&] { *v = vp; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE SetBackground(D3DMATERIALHANDLE h) override {
        A1 x = {(uint32_t)h};
        return effect<HRESULT>(com_begin(C_V_BACKGROUND, this, &x, sizeof x), [&] {
            background = (Material*)(uintptr_t)h;
            return (HRESULT)DD_OK;
        });
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD n, LPD3DRECT rects, DWORD flags) override {
        A2 x = {n, flags};
        return effect<HRESULT>(com_begin(C_V_CLEAR, this, &x, sizeof x, rects, rects ? n * sizeof *rects : 0),
                               [&] { return clear(n, rects, flags); });
    }
    HRESULT clear(DWORD n, LPD3DRECT rects, DWORD flags) {
        if (!on_gl_thread()) return DD_OK;
        p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
        glEnable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        GLbitfield bits = 0;
        if (flags & D3DCLEAR_TARGET) {
            bits |= GL_COLOR_BUFFER_BIT;
            D3DCOLORVALUE c = background ? background->m.diffuse : D3DCOLORVALUE{0, 0, 0, 1};
            glClearColor(c.r, c.g, c.b, 1.0f);
        }
        if (flags & D3DCLEAR_ZBUFFER) bits |= GL_DEPTH_BUFFER_BIT, glClearDepth(1.0);
        for (DWORD i = 0; i < n; i++) {
            Rect r = map_rect(rects[i].x1, rects[i].y1, rects[i].x2 - rects[i].x1, rects[i].y2 - rects[i].y1);
            glScissor(r.x, r.y, r.w, r.h);
            glClear(bits);
        }
        state_dirty();
        return DD_OK;
    }

    // what it computes depends on the transforms and viewport the calls before it set: queued like any result
    HRESULT STDMETHODCALLTYPE TransformVertices(DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen) override {
        struct { uint32_t n, flags, in_size, out_size, has_h, has_off; } x = {n, flags, d->dwInSize, d->dwOutSize,
                                                                             d->lpHOut ? 1u : 0u, offscreen ? 1u : 0u};
        int ph = com_begin(C_V_TRANSFORM, this, &x, sizeof x, d->lpIn, (size_t)n * d->dwInSize);
        return query(ph, {{d->lpOut, (size_t)n * d->dwOutSize}, {d->lpHOut, (size_t)n * sizeof(D3DHVERTEX)},
                          {&d->dwClipIntersection, 4}, {&d->dwClipUnion, 4}, {offscreen, 4}},
                     [&] { return transform(n, d, flags, offscreen); });
    }
    HRESULT transform(DWORD n, LPD3DTRANSFORMDATA d, DWORD flags, LPDWORD offscreen) {
        // on the CPU, as D3D did: W x V x P, then the viewport mapping; clip codes for anything outside
        float m[16], wv[16];
        D3DMATRIX t;
        mat_mul(g.world, g.view, wv);
        memcpy(&t, wv, sizeof t);
        mat_mul(t, g.proj, m);
        float cw[4];
        clip_window(vp, cw);                           // as the target draws it (widened when full width)
        Rect vr = map_rect(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight);
        DWORD all = ~0u, any = 0;
        for (DWORD i = 0; i < n; i++) {
            const float* in = (const float*)((const char*)d->lpIn + i * d->dwInSize);
            float p[4];
            for (int c = 0; c < 4; c++) p[c] = in[0] * m[c] + in[1] * m[4 + c] + in[2] * m[8 + c] + m[12 + c];
            DWORD code = 0;
            if (p[0] < cw[0] * p[3]) code |= D3DCLIP_LEFT;
            if (p[0] > (cw[0] + cw[2]) * p[3]) code |= D3DCLIP_RIGHT;
            if (p[1] > vp.dvClipY * p[3]) code |= D3DCLIP_TOP;
            if (p[1] < (vp.dvClipY - vp.dvClipHeight) * p[3]) code |= D3DCLIP_BOTTOM;
            if (p[2] < 0) code |= D3DCLIP_FRONT;
            if (p[2] > p[3]) code |= D3DCLIP_BACK;
            all &= code, any |= code;
            float rw = p[3] != 0 ? 1.0f / p[3] : 0.0f;
            D3DTLVERTEX* out = (D3DTLVERTEX*)((char*)d->lpOut + i * d->dwOutSize);
            float tx = vr.x + vr.w * (p[0] * rw - cw[0]) / cw[2];          // target pixels
            out->sx = (tx - g.ox) / g.scale;                                 // back to the game's
            out->sy = vp.dwY + vp.dwHeight * (vp.dvClipY - p[1] * rw) / vp.dvClipHeight;
            out->sz = p[2] * rw;
            out->rhw = rw;
            if (d->lpHOut) {
                D3DHVERTEX* h = (D3DHVERTEX*)d->lpHOut + i;
                h->dwFlags = code, h->hx = p[0], h->hy = p[1], h->hz = p[2];
            }
        }
        d->dwClipIntersection = all, d->dwClipUnion = any;
        if (offscreen) *offscreen = (flags & D3DTRANSFORM_CLIPPED) ? all : 0;
        return DD_OK;
    }
};

struct Device : Base_IDirect3DDevice2 {
    LONG refs = 1;
    Viewport* current = 0;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) override {
        A2 x = {hal ? (uint32_t)hal->dwSize : 0, hel ? (uint32_t)hel->dwSize : 0};
        return query(com_begin(C_D_CAPS, this, &x, sizeof x), {{hal, x.a}, {hel, x.b}}, [&] { return caps(hal, hel); });
    }
    HRESULT caps(LPD3DDEVICEDESC hal, LPD3DDEVICEDESC hel) {
        for (LPD3DDEVICEDESC d : {hal, hel}) {
            if (!d) continue;
            DWORD size = d->dwSize;
            memset(d, 0, size);
            d->dwSize = size;
            d->dwFlags = 0xFFF;                                   // every field valid
            d->dcmColorModel = D3DCOLOR_RGB;
            d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX |
                           D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM;
            d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
            d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
            d->bClipping = TRUE;
            d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
            for (D3DPRIMCAPS* p : {&d->dpcLineCaps, &d->dpcTriCaps}) {
                p->dwSize = sizeof(D3DPRIMCAPS);
                p->dwMiscCaps = 0xFFFFFFFF;
                p->dwRasterCaps = 0xFFFFFFFF;
                p->dwZCmpCaps = p->dwAlphaCmpCaps = 0xFF;
                p->dwSrcBlendCaps = p->dwDestBlendCaps = 0x1FFF;
                p->dwShadeCaps = 0xFFFFFFFF;
                p->dwTextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_ALPHA |
                                   D3DPTEXTURECAPS_TRANSPARENCY;
                p->dwTextureFilterCaps = 0x3F;
                p->dwTextureBlendCaps = 0xFF;
                p->dwTextureAddressCaps = 0x7;
            }
            d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
            d->dwDeviceZBufferBitDepth = DDBD_16 | DDBD_24;
            d->dwMaxBufferSize = 0;
            d->dwMaxVertexCount = 65535;
            if (size >= sizeof(D3DDEVICEDESC)) {
                d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
                d->dwMaxTextureWidth = d->dwMaxTextureHeight = 2048;
            }
        }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStats(LPD3DSTATS s) override {
        DWORD n = s->dwSize;
        return query(com_begin(C_D_STATS, this, &n, 4), {{s, n}}, [&] { memset(s, 0, n); s->dwSize = n; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE AddViewport(LPDIRECT3DVIEWPORT2 v) override {
        A1 x = {U(v)};
        return effect<HRESULT>(com_begin(C_D_ADDVP, this, &x, sizeof x), [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE DeleteViewport(LPDIRECT3DVIEWPORT2 v) override {
        A1 x = {U(v)};
        return effect<HRESULT>(com_begin(C_D_DELVP, this, &x, sizeof x), [&] {
            if ((Viewport*)v == current) current = 0;
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE SetCurrentViewport(LPDIRECT3DVIEWPORT2 v) override {
        A1 x = {U(v)};
        return effect<HRESULT>(com_begin(C_D_SETVP, this, &x, sizeof x), [&] {
            current = (Viewport*)v;
            if (current) g.vp = current->vp;
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetCurrentViewport(LPDIRECT3DVIEWPORT2* v) override {
        return query(com_begin(C_D_GETVP, this, 0, 0), {{v, sizeof *v}}, [&] { *v = current; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE BeginScene() override { return effect<HRESULT>(com_begin(C_D_BEGIN, this, 0, 0), [] { return (HRESULT)DD_OK; }); }
    HRESULT STDMETHODCALLTYPE EndScene() override { return effect<HRESULT>(com_begin(C_D_END, this, 0, 0), [] { return (HRESULT)DD_OK; }); }

    // (a callback into the game for each format: run in both passes, as the data never changes)
    HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMTEXTUREFORMATSCALLBACK cb, LPVOID ctx) override {
        A1 x = {U(cb)};                                // (not ctx: the caller's stack)
        com_begin(C_D_ENUMTEX, this, &x, sizeof x);
        for (Format f : {F565, F1555, F4444}) {
            DDSURFACEDESC d = {};
            d.dwSize = sizeof d;
            d.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
            pixel_format(f, d.ddpfPixelFormat);
            if (cb(&d, ctx) == D3DENUMRET_CANCEL) break;
        }
        return DD_OK;
    }

    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override {
        A2 x = {(uint32_t)s, v};
        int ph = com_begin(C_D_SETRS, this, &x, sizeof x);
        if (ph && s == D3DRENDERSTATE_TEXTUREHANDLE) t_feed->tex = v;
        return effect<HRESULT>(ph, [&] {
            if ((DWORD)s < 64) {
                g.rs[s] = v;
                if (s == D3DRENDERSTATE_TEXTUREADDRESS) g.rs[D3DRENDERSTATE_TEXTUREADDRESSU] = g.rs[D3DRENDERSTATE_TEXTUREADDRESSV] = v;
            }
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) override {
        A1 x = {(uint32_t)s};
        return query(com_begin(C_D_GETRS, this, &x, sizeof x), {{v, 4}}, [&] { *v = (DWORD)s < 64 ? g.rs[s] : 0; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE SetLightState(D3DLIGHTSTATETYPE s, DWORD v) override {
        A2 x = {(uint32_t)s, v};
        return effect<HRESULT>(com_begin(C_D_SETLS, this, &x, sizeof x), [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) override {
        A1 x = {(uint32_t)t};
        return effect<HRESULT>(com_begin(C_D_SETXF, this, &x, sizeof x, m, sizeof *m), [&] {
            if (t == D3DTRANSFORMSTATE_WORLD) g.world = *m;
            else if (t == D3DTRANSFORMSTATE_VIEW) g.view = *m;
            else if (t == D3DTRANSFORMSTATE_PROJECTION) g.proj = *m;
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE t, LPD3DMATRIX m) override {
        A1 x = {(uint32_t)t};
        return query(com_begin(C_D_GETXF, this, &x, sizeof x), {{m, sizeof *m}}, [&] {
            *m = t == D3DTRANSFORMSTATE_WORLD ? g.world : t == D3DTRANSFORMSTATE_VIEW ? g.view : g.proj;
            return (HRESULT)DD_OK;
        });
    }
    // every vertex type the game draws is 32 bytes (D3DVERTEX, D3DLVERTEX, D3DTLVERTEX)
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, DWORD f) override {
        A4 x = {(uint32_t)pt, (uint32_t)vt, n, f};
        std::vector<uint8_t> buf;
        return effect<HRESULT>(com_begin(C_D_DRAW, this, &x, sizeof x, draw_record(vt, v, n, buf), (size_t)n * 32), [&] {
            if (pt != D3DPT_TRIANGLELIST) { com_unsupported("DrawPrimitive (not a triangle list)"); return (HRESULT)DD_OK; }
            draw(vt, v, n, 0, 0);
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE pt, D3DVERTEXTYPE vt, LPVOID v, DWORD n, LPWORD idx, DWORD ni, DWORD f) override {
        A5 x = {(uint32_t)pt, (uint32_t)vt, n, ni, f};
        std::vector<uint8_t> buf;
        int ph = com_begin(C_D_DRAWIDX, this, &x, sizeof x, draw_record(vt, v, n, buf), (size_t)n * 32);
        if (ph) shadow_com_effect(C_D_DRAWIDX_I, this, 0, 0, idx, (size_t)ni * 2);
        return effect<HRESULT>(ph, [&] {
            if (pt != D3DPT_TRIANGLELIST) { com_unsupported("DrawIndexedPrimitive (not a triangle list)"); return (HRESULT)DD_OK; }
            draw(vt, v, n, idx, ni);
            return (HRESULT)DD_OK;
        });
    }
};

struct Direct3D : Base_IDirect3D2 {
    LONG refs = 1;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }
    // a new object, like any result: the rewrite's pass is handed the one the original's pass made
    HRESULT STDMETHODCALLTYPE CreateMaterial(LPDIRECT3DMATERIAL2* out, IUnknown*) override {
        return query(com_begin(C_3_MATERIAL, this, 0, 0), {{out, sizeof *out}}, [&] { *out = new Material; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE CreateViewport(LPDIRECT3DVIEWPORT2* out, IUnknown*) override {
        return query(com_begin(C_3_VIEWPORT, this, 0, 0), {{out, sizeof *out}}, [&] { *out = new Viewport; return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID c, LPDIRECTDRAWSURFACE sf, LPDIRECT3DDEVICE2* out) override {
        struct { GUID c; uint32_t sf; } x = {c, U(sf)};
        return query(com_begin(C_3_DEVICE, this, &x, sizeof x), {{out, sizeof *out}}, [&] {
            if (!gl_start()) return (HRESULT)DDERR_GENERIC;
            set_defaults();
            *out = new Device;
            return (HRESULT)DD_OK;
        });
    }
};

// ---- DirectDraw objects ----------------------------------------------------------------------------------------
struct DirectDraw2 : Base_IDirectDraw2 {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override {
        return query(com_begin(C_QI, this, &riid, sizeof riid), {{out, sizeof *out}}, [&] {
            if (riid == IID_IDirect3D2) { *out = new Direct3D; return (HRESULT)DD_OK; }
            if (riid == IID_IDirectDraw2 || riid == IID_IUnknown) { *out = this; InterlockedIncrement(&refs); return (HRESULT)DD_OK; }
            *out = 0;
            return (HRESULT)E_NOINTERFACE;
        });
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }

    HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS hal, LPDDCAPS hel) override {
        A2 x = {hal ? (uint32_t)hal->dwSize : 0, hel ? (uint32_t)hel->dwSize : 0};
        return query(com_begin(C_DD_CAPS, this, &x, sizeof x), {{hal, x.a}, {hel, x.b}}, [&] { return caps(hal, hel); });
    }
    HRESULT caps(LPDDCAPS hal, LPDDCAPS hel) {
        for (LPDDCAPS c : {hal, hel}) {
            if (!c) continue;
            DWORD size = c->dwSize;
            memset(c, 0, size);
            c->dwSize = size;
            c->dwCaps = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_ZBLTS | DDCAPS_BLTCOLORFILL;
            c->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
            c->dwVidMemTotal = c->dwVidMemFree = 64u << 20;
            c->ddsCaps.dwCaps = DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_MIPMAP |
                                DDSCAPS_VIDEOMEMORY | DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER;
            c->dwZBufferBitDepths = DDBD_16 | DDBD_24;
        }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS caps, LPDWORD total, LPDWORD free) override {
        A1 x = {caps ? (uint32_t)caps->dwCaps : 0xFFFFFFFFu};
        return query(com_begin(C_DD_VIDMEM, this, &x, sizeof x), {{total, 4}, {free, 4}}, [&] {
            // 64 MB of card memory and no AGP: the top tier, and far from the 4 GB wrap that crashed v1.1
            DWORD mem = caps && (caps->dwCaps & DDSCAPS_NONLOCALVIDMEM) ? 0 : 64u << 20;
            if (total) *total = mem;
            if (free) *free = mem;
            return (HRESULT)DD_OK;
        });
    }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND w, DWORD f) override {
        A2 x = {U(w), f};
        return effect<HRESULT>(com_begin(C_DD_COOP, this, &x, sizeof x), [] { return gl_start() ? (HRESULT)DD_OK : (HRESULT)DDERR_GENERIC; });
    }
    // (a callback into the game for each mode: run in both passes, as the data never changes)
    HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD f, LPDDSURFACEDESC, LPVOID ctx, LPDDENUMMODESCALLBACK cb) override {
        A2 x = {f, U(cb)};                             // (not ctx: the caller's stack)
        com_begin(C_DD_ENUMMODES, this, &x, sizeof x);
        static const int modes[][2] = {{512, 384}, {640, 480}, {800, 600}, {1024, 768}};
        for (auto& m : modes) {
            DDSURFACEDESC d = {};
            d.dwSize = sizeof d;
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS;
            d.dwWidth = m[0], d.dwHeight = m[1], d.lPitch = m[0] * 2;
            d.ddsCaps.dwCaps = DDSCAPS_3DDEVICE;
            pixel_format(F565, d.ddpfPixelFormat);
            if (cb(&d, ctx) == DDENUMRET_CANCEL) break;
        }
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD rate, DWORD f) override {
        A5 x = {w, h, bpp, rate, f};
        return effect<HRESULT>(com_begin(C_DD_SETMODE, this, &x, sizeof x), [&] { resize_target((int)w, (int)h); return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE RestoreDisplayMode() override {
        return effect<HRESULT>(com_begin(C_DD_RESTOREMODE, this, 0, 0), [] { return (HRESULT)DD_OK; });
    }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC d) override {
        DWORD size = d->dwSize ? d->dwSize : sizeof *d;
        return query(com_begin(C_DD_GETMODE, this, &size, 4), {{d, size}}, [&] { return display_mode(d); });
    }
    HRESULT display_mode(LPDDSURFACEDESC d) {
        DWORD size = d->dwSize ? d->dwSize : sizeof *d;
        memset(d, 0, size);
        d->dwSize = size;
        d->dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
        d->dwWidth = g.w, d->dwHeight = g.h, d->lPitch = g.w * 2;
        pixel_format(F565, d->ddpfPixelFormat);
        return DD_OK;
    }
    HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD f, HANDLE e) override {
        A2 x = {f, U(e)};
        return effect<HRESULT>(com_begin(C_DD_VBLANK, this, &x, sizeof x), [] { return (HRESULT)DD_OK; });
    }

    HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out, IUnknown*) override {
        DWORD size = d->dwSize ? d->dwSize : sizeof *d;
        return query(com_begin(C_DD_SURFACE, this, 0, 0, d, size), {{out, sizeof *out}}, [&] { return create_surface(d, out); });
    }
    HRESULT create_surface(LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out) {
        DWORD caps = d->ddsCaps.dwCaps;
        Surface* s = new Surface;
        s->caps = d->ddsCaps;
        s->flags = d->dwFlags;
        s->root = s;
        if (caps & DDSCAPS_PRIMARYSURFACE) {
            s->w = g.w, s->h = g.h;
            s->backbuffers = (d->dwFlags & DDSD_BACKBUFFERCOUNT) ? d->dwBackBufferCount : 0;
            if (caps & DDSCAPS_FLIP) {
                Surface* b = new Surface;
                b->caps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP;
                b->w = g.w, b->h = g.h, b->root = b, b->is_back = true;
                s->back = b;
            }
        } else if (caps & DDSCAPS_ZBUFFER) {
            s->w = d->dwWidth, s->h = d->dwHeight;
            s->caps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        } else {
            s->w = d->dwWidth, s->h = d->dwHeight;
            s->fmt = (d->dwFlags & DDSD_PIXELFORMAT) ? format_of(d->ddpfPixelFormat) : F565;
            s->pixels.assign((size_t)s->w * s->h, 0);
            if (d->dwFlags & DDSD_CKSRCBLT) s->keyed = true, s->key = (uint16_t)d->ddckCKSrcBlt.dwColorSpaceLowValue;
            int levels = (d->dwFlags & DDSD_MIPMAPCOUNT) ? (int)d->dwMipMapCount : 1;
            s->mip_count = levels;
            Surface* prev = s;
            for (int i = 1; i < levels && prev->w > 1 && prev->h > 1; i++) {
                Surface* l = new Surface;
                l->caps = s->caps;
                l->caps.dwCaps |= DDSCAPS_MIPMAP;
                l->w = prev->w / 2, l->h = prev->h / 2;
                l->fmt = s->fmt, l->keyed = s->keyed, l->key = s->key, l->root = s;
                l->pixels.assign((size_t)l->w * l->h, 0);
                prev->next_mip = l;
                prev = l;
            }
        }
        *out = (LPDIRECTDRAWSURFACE)s;
        return DD_OK;
    }
};

struct DirectDraw1 : Base_IDirectDraw {                   // what DirectDrawCreate returns; the game QIs it at once
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override {
        return query(com_begin(C_QI, this, &riid, sizeof riid), {{out, sizeof *out}}, [&] {
            if (riid == IID_IDirectDraw2) { *out = new DirectDraw2; return (HRESULT)DD_OK; }
            if (riid == IID_IDirectDraw || riid == IID_IUnknown) { *out = this; InterlockedIncrement(&refs); return (HRESULT)DD_OK; }
            *out = 0;
            return (HRESULT)E_NOINTERFACE;
        });
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return effect<ULONG>(com_begin(C_ADDREF, this, 0, 0), [&] { return (ULONG)InterlockedIncrement(&refs); });
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return effect<ULONG>(com_begin(C_RELEASE, this, 0, 0), [&] {
            LONG n = InterlockedDecrement(&refs);
            if (!n) delete this;
            return (ULONG)n;
        });
    }
};

HRESULT WINAPI emu_DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    *out = new DirectDraw1;
    return DD_OK;
}

HRESULT WINAPI emu_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    cb(0, (LPSTR)"OpenGL (viperport)", (LPSTR)"display", ctx);    // the primary display only
    return DD_OK;
}

}  // namespace

// point the main module's imports of `dll!name` at `to`
bool patch_import(const char* dll, const char* name, void* to) {
    uint8_t* base = (uint8_t*)GetModuleHandleA(0);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((char*)ibn->Name, name) != 0) continue;
            DWORD old;
            VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)(uintptr_t)to;
            VirtualProtect(&iat->u1.Function, 4, old, &old);
            return true;
        }
    }
    return false;
}

// ---- switching it on -------------------------------------------------------------------------------------------
bool renderer_install() {
    bool a = patch_import("DDRAW.dll", "DirectDrawCreate", (void*)emu_DirectDrawCreate);
    bool b = patch_import("DDRAW.dll", "DirectDrawEnumerateA", (void*)emu_DirectDrawEnumerateA);
    if (!a || !b) {
        logf("renderer: NOT switching to OpenGL -- the DDRAW imports weren't found (DirectDrawCreate %d, Enumerate %d)", a, b);
        return false;
    }
    logf("renderer: OpenGL 3.3 (DirectDraw and Direct3D emulated)");
    return true;
}

void renderer_report() {
    if (g.frames) logf("exit: renderer: %llu frames, %llu draw calls, %llu triangles", g.frames, g.draws, g.triangles);
}
