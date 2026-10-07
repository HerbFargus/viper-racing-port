// gl_table.cpp -- the renderer's OpenGL calls, made through gl_api and recorded during a shadow check (gl_table.h).
#define _CRT_SECURE_NO_WARNINGS
#include "gl_table.h"
#include "perf.h"
#include <array>
#include <unordered_map>
#include <string.h>
#include <vector>
#include "viperport.h"
#include "port.h"

void platform_set_view(int x0, int y0, int w, int h, int game_w, int game_h);

GLApi gl_api;

bool gl_api_load() {
#ifdef _WIN32
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");     // (Windows: OpenGL 1.1's functions are opengl32.dll's exports)
#define VP_GL32_PROC(p, n) if (!p && gl32) p = (void*)GetProcAddress(gl32, n)
#else
#define VP_GL32_PROC(p, n)                            // (elsewhere SDL_GL_GetProcAddress finds every one)
#endif
    bool ok = true;
#define GL_API_FN(ret, name, args)                                                                                   \
    {                                                                                                                \
        void* p = SDL_GL_GetProcAddress("gl" #name);                                                                 \
        VP_GL32_PROC(p, "gl" #name);                                                                                 \
        *(void**)&gl_api.name = p;                                                                                   \
        if (!p) logf("gl_table: gl" #name " is missing"), ok = false;                                                \
    }
#include "gl_api.inc"
#undef GL_API_FN
#undef VP_GL32_PROC
    gl_api.SwapWindow = SDL_GL_SwapWindow;
    gl_api.GetDrawableSize = SDL_GL_GetDrawableSize;
    gl_api.SetView = platform_set_view;
    return ok;
}

namespace {

enum Id : uint16_t {
    I_ActiveTexture = 1, I_BindBuffer, I_BindFramebuffer, I_BindRenderbuffer, I_BindTexture, I_BindVertexArray,
    I_BlendFunc, I_BlitFramebuffer, I_BufferData, I_CheckFramebufferStatus, I_Clear, I_ClearColor, I_ClearDepth,
    I_CullFace, I_DeleteFramebuffers, I_DeleteRenderbuffers, I_DeleteTextures, I_DepthFunc, I_DepthMask, I_Disable,
    I_DrawArrays, I_DrawElements, I_Enable, I_FramebufferRenderbuffer, I_FramebufferTexture2D, I_FrontFace,
    I_GenFramebuffers, I_GenRenderbuffers, I_GenTextures, I_PixelStorei, I_ReadPixels, I_RenderbufferStorage,
    I_Scissor, I_TexImage2D, I_TexParameterf, I_TexParameteri, I_TexSubImage2D, I_Uniform1f, I_Uniform1i,
    I_Uniform2f, I_Uniform3f, I_Uniform4f, I_UniformMatrix4fv, I_UseProgram, I_Viewport, I_SwapWindow,
    I_GetDrawableSize, I_SetView,
};

#ifdef VP_GCC
__thread int t_direct;                       // inside glr::Direct: the start-up's calls
#else
__declspec(thread) int t_direct;                       // inside glr::Direct: the start-up's calls
#endif

// what the calls hand back, queued by the original's pass for the rewrite's (per check, per thread)
struct Feed { unsigned check = ~0u; int ph = 0; std::vector<uint8_t> q; size_t at = 0; };
#ifdef VP_GCC
__thread Feed* t_feed;
#else
__declspec(thread) Feed* t_feed;
#endif

int phase() {
    if (t_direct) return 0;
    int ph = shadow_com_phase();
    if (!ph) return 0;
    if (!t_feed) t_feed = new Feed;
    unsigned c = shadow_com_check();
    if (t_feed->check != c) { t_feed->check = c; t_feed->q.clear(); t_feed->at = 0; t_feed->ph = ph; }
    else if (t_feed->ph != ph) { t_feed->ph = ph; t_feed->at = 0; }
    return ph;
}

void save(const void* p, size_t n) {
    std::vector<uint8_t>& q = t_feed->q;
    uint32_t k = (uint32_t)n;
    q.insert(q.end(), (const uint8_t*)&k, (const uint8_t*)&k + 4);
    q.insert(q.end(), (const uint8_t*)p, (const uint8_t*)p + n);
}
// what the original's pass got at this point; zeros if the rewrite asks for more than it did (the calls, recorded,
// already differ)
void take(void* p, size_t n) {
    std::vector<uint8_t>& q = t_feed->q;
    uint32_t k = 0;
    if (t_feed->at + 4 <= q.size()) { memcpy(&k, &q[t_feed->at], 4); t_feed->at += 4; }
    if (t_feed->at + k > q.size()) k = 0;
    size_t m = k < n ? k : n;
    if (m) memcpy(p, &q[t_feed->at], m);
    if (m < n) memset((uint8_t*)p + m, 0, n - m);
    t_feed->at += k;
}

inline uint32_t w(uint32_t v) { return v; }
inline uint32_t w(int v) { return (uint32_t)v; }
inline uint32_t w(float v) { uint32_t u; memcpy(&u, &v, 4); return u; }
inline uint32_t w(uint8_t v) { return v; }

// record a call: its id, its arguments by value, the data it reads (by content); the pass's phase
template <class... A> int rec(Id id, const void* data, size_t nd, A... a) {
    int ph = phase();
    if (!ph) return 0;
    uint32_t v[sizeof...(A) + 1] = {w(a)...};
    shadow_com_effect((uint16_t)(GLREC_BASE + id), 0, v, sizeof...(A) * 4, data, nd);
    return ph;
}
#define LIVE(ph) ((ph) != 2)

GLint g_pack = 4, g_unpack = 4;                        // the pixel-store alignments, for the sizes of pixel data

size_t pixel_bytes(GLsizei wd, GLsizei ht, GLenum fmt, GLenum type, GLint align) {
    if (wd <= 0 || ht <= 0) return 0;
    size_t bpp = type == GL_UNSIGNED_BYTE ? (fmt == GL_RGB || fmt == GL_BGR ? 3 : 4) : 2;
    size_t row = (size_t)wd * bpp;
    row = (row + align - 1) / align * align;
    return row * ht;
}

// ---- the live side's state cache (draw-performance stage P1) ------------------------------------------------------------
// gl_core.cpp sets the whole GL state again for every one of the game's draws -- ~35 calls a draw, ~19,000 a frame in a
// race -- most of them the value already set. Each wrapper below records its call exactly as before (the shadow checks
// and world_gx_dd compare that stream, unchanged); only the LIVE call to the driver is skipped when the cache knows the
// state already holds that value. So the GPU gets the same state and the same pixels; the driver gets fewer calls. The
// cache knows only what went through glr: it's forgotten after setup (gl_core.cpp's direct gl_api calls) and after every
// swap (gl_dxgi.cpp's present binds framebuffers directly), and a deleted texture's entries go with it.
struct GlCache {
    bool valid = false;
    GLuint draw_fb, read_fb, program, vao, array_buf, active;
    GLuint tex[8];                               // GL_TEXTURE_2D per unit
    std::unordered_map<GLenum, uint8_t> caps;    // Enable / Disable
    GLint vp[4], sc[4];
    bool vp_ok, sc_ok, blend_ok, df_ok, cc_ok;
    GLenum blend_s, blend_d, depth_func, front, cull;
    int depth_mask;
    GLfloat cc[4];
    std::unordered_map<uint64_t, std::array<uint32_t, 16>> uniforms;   // program << 32 | location: the value's words
    std::unordered_map<uint64_t, uint32_t> texparams;                   // texture << 32 | pname: the value's bits
} C;
const GLuint UNKNOWN = 0xffffffffu;

void cache_reset_all() {
    C.valid = true;
    C.draw_fb = C.read_fb = C.program = C.vao = C.array_buf = C.active = UNKNOWN;
    for (GLuint& t : C.tex) t = UNKNOWN;
    C.caps.clear();
    C.vp_ok = C.sc_ok = C.blend_ok = C.df_ok = C.cc_ok = false;
    C.front = C.cull = UNKNOWN;
    C.depth_mask = -1;
    C.uniforms.clear();
    C.texparams.clear();
}
// the live call: counted (perf), and true when the driver must get it
inline bool live(int ph) {
    if (ph == 2) return false;
    if (!C.valid) cache_reset_all();
    perf::g_gl_calls++;
    return true;
}
inline void skipped() { perf::g_gl_skipped++; }
// a uniform: true when it must be sent (the value differs from what the current program's location holds)
bool uniform_changed(GLint loc, const uint32_t* v, size_t n) {
    if (C.program == UNKNOWN || loc < 0) return true;
    std::array<uint32_t, 16> a{};
    memcpy(a.data(), v, n * 4);
    auto key = (uint64_t)C.program << 32 | (uint32_t)loc;
    auto it = C.uniforms.find(key);
    if (it != C.uniforms.end() && it->second == a) return false;
    C.uniforms[key] = a;
    return true;
}
// a texture parameter of the texture bound to the active unit
bool texparam_changed(GLenum pname, uint32_t bits) {
    const GLuint unit = C.active == UNKNOWN ? UNKNOWN : C.active - GL_TEXTURE0;
    if (unit >= 8 || C.tex[unit] == UNKNOWN) return true;
    auto key = (uint64_t)C.tex[unit] << 32 | pname;
    auto it = C.texparams.find(key);
    if (it != C.texparams.end() && it->second == bits) return false;
    C.texparams[key] = bits;
    return true;
}
template <class T> uint32_t bits_of(T v) { uint32_t b = 0; memcpy(&b, &v, sizeof v); return b; }

}  // namespace

const char* gl_record_name(uint16_t m) {
    static const char* const n[] = {
    "glActiveTexture", "glBindBuffer", "glBindFramebuffer", "glBindRenderbuffer", "glBindTexture",
    "glBindVertexArray", "glBlendFunc", "glBlitFramebuffer", "glBufferData", "glCheckFramebufferStatus", "glClear",
    "glClearColor", "glClearDepth", "glCullFace", "glDeleteFramebuffers", "glDeleteRenderbuffers",
    "glDeleteTextures", "glDepthFunc", "glDepthMask", "glDisable", "glDrawArrays", "glDrawElements", "glEnable",
    "glFramebufferRenderbuffer", "glFramebufferTexture2D", "glFrontFace", "glGenFramebuffers",
    "glGenRenderbuffers", "glGenTextures", "glPixelStorei", "glReadPixels", "glRenderbufferStorage", "glScissor",
    "glTexImage2D", "glTexParameterf", "glTexParameteri", "glTexSubImage2D", "glUniform1f", "glUniform1i",
    "glUniform2f", "glUniform3f", "glUniform4f", "glUniformMatrix4fv", "glUseProgram", "glViewport",
    "SDL_GL_SwapWindow", "SDL_GL_GetDrawableSize", "platform_set_view"};
    unsigned i = (unsigned)m - GLREC_BASE - 1;
    return m > GLREC_BASE && i < sizeof n / sizeof n[0] ? n[i] : 0;
}

namespace glr {

Direct::Direct() { t_direct++; }
Direct::~Direct() { t_direct--; }

void ActiveTexture(GLenum a) {
    if (!live(rec(I_ActiveTexture, 0, 0, a))) return;
    if (C.active == a) return skipped();
    C.active = a;
    gl_api.ActiveTexture(a);
}
void BindBuffer(GLenum a, GLuint b) {
    if (!live(rec(I_BindBuffer, 0, 0, a, b))) return;
    if (a == GL_ARRAY_BUFFER) {                 // (the element buffer's binding is the vertex array's: not cached)
        if (C.array_buf == b) return skipped();
        C.array_buf = b;
    }
    gl_api.BindBuffer(a, b);
}
void BindFramebuffer(GLenum a, GLuint b) {
    if (!live(rec(I_BindFramebuffer, 0, 0, a, b))) return;
    const bool d = a == GL_FRAMEBUFFER || a == GL_DRAW_FRAMEBUFFER, r = a == GL_FRAMEBUFFER || a == GL_READ_FRAMEBUFFER;
    if ((!d || C.draw_fb == b) && (!r || C.read_fb == b)) return skipped();
    if (d) C.draw_fb = b;
    if (r) C.read_fb = b;
    gl_api.BindFramebuffer(a, b);
}
void BindRenderbuffer(GLenum a, GLuint b) { if (LIVE(rec(I_BindRenderbuffer, 0, 0, a, b))) gl_api.BindRenderbuffer(a, b); }
void BindTexture(GLenum a, GLuint b) {
    if (!live(rec(I_BindTexture, 0, 0, a, b))) return;
    const GLuint unit = C.active == UNKNOWN ? UNKNOWN : C.active - GL_TEXTURE0;
    if (a == GL_TEXTURE_2D && unit < 8) {
        if (C.tex[unit] == b) return skipped();
        C.tex[unit] = b;
    }
    gl_api.BindTexture(a, b);
}
void BindVertexArray(GLuint a) {
    if (!live(rec(I_BindVertexArray, 0, 0, a))) return;
    if (C.vao == a) return skipped();
    C.vao = a;
    gl_api.BindVertexArray(a);
}
void BlendFunc(GLenum a, GLenum b) {
    if (!live(rec(I_BlendFunc, 0, 0, a, b))) return;
    if (C.blend_ok && C.blend_s == a && C.blend_d == b) return skipped();
    C.blend_ok = true, C.blend_s = a, C.blend_d = b;
    gl_api.BlendFunc(a, b);
}
void BlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl) {
    if (LIVE(rec(I_BlitFramebuffer, 0, 0, a, b, c, d, e, f, g, h, m, fl))) gl_api.BlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}
void BufferData(GLenum t, ptrdiff_t n, const void* p, GLenum u) {
    if (LIVE(rec(I_BufferData, p, p ? (size_t)n : 0, t, (uint32_t)n, u))) gl_api.BufferData(t, n, p, u);
}
GLenum CheckFramebufferStatus(GLenum t) {
    int ph = rec(I_CheckFramebufferStatus, 0, 0, t);
    GLenum r = 0;
    if (ph == 2) { take(&r, sizeof r); return r; }
    r = gl_api.CheckFramebufferStatus(t);
    if (ph == 1) save(&r, sizeof r);
    return r;
}
void Clear(GLbitfield m) { if (LIVE(rec(I_Clear, 0, 0, m))) gl_api.Clear(m); }
void ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (!live(rec(I_ClearColor, 0, 0, r, g, b, a))) return;
    const GLfloat v[4] = {r, g, b, a};
    if (C.cc_ok && !memcmp(C.cc, v, sizeof v)) return skipped();
    C.cc_ok = true, memcpy(C.cc, v, sizeof v);
    gl_api.ClearColor(r, g, b, a);
}
void ClearDepth(GLdouble d) {
    uint32_t b[2];
    memcpy(b, &d, 8);
    if (LIVE(rec(I_ClearDepth, 0, 0, b[0], b[1]))) gl_api.ClearDepth(d);
}
void CullFace(GLenum a) {
    if (!live(rec(I_CullFace, 0, 0, a))) return;
    if (C.cull == a) return skipped();
    C.cull = a;
    gl_api.CullFace(a);
}
void DeleteFramebuffers(GLsizei n, const GLuint* p) { if (LIVE(rec(I_DeleteFramebuffers, p, (size_t)n * 4, n))) gl_api.DeleteFramebuffers(n, p); }
void DeleteRenderbuffers(GLsizei n, const GLuint* p) { if (LIVE(rec(I_DeleteRenderbuffers, p, (size_t)n * 4, n))) gl_api.DeleteRenderbuffers(n, p); }
void DeleteTextures(GLsizei n, const GLuint* p) {
    if (!live(rec(I_DeleteTextures, p, (size_t)n * 4, n))) return;
    for (GLsizei i = 0; i < n; i++) cache_forget_texture(p[i]);
    gl_api.DeleteTextures(n, p);
}
void DepthFunc(GLenum a) {
    if (!live(rec(I_DepthFunc, 0, 0, a))) return;
    if (C.df_ok && C.depth_func == a) return skipped();
    C.df_ok = true, C.depth_func = a;
    gl_api.DepthFunc(a);
}
void DepthMask(GLboolean a) {
    if (!live(rec(I_DepthMask, 0, 0, a))) return;
    if (C.depth_mask == (int)a) return skipped();
    C.depth_mask = a;
    gl_api.DepthMask(a);
}
void Disable(GLenum a) {
    if (!live(rec(I_Disable, 0, 0, a))) return;
    auto it = C.caps.find(a);
    if (it != C.caps.end() && it->second == 0) return skipped();
    C.caps[a] = 0;
    gl_api.Disable(a);
}
void DrawArrays(GLenum m, GLint f, GLsizei n) { if (LIVE(rec(I_DrawArrays, 0, 0, m, f, n))) gl_api.DrawArrays(m, f, n); }
void DrawElements(GLenum m, GLsizei n, GLenum t, const void* off) {
    if (LIVE(rec(I_DrawElements, 0, 0, m, n, t, (uint32_t)(uintptr_t)off))) gl_api.DrawElements(m, n, t, off);
}
void Enable(GLenum a) {
    if (!live(rec(I_Enable, 0, 0, a))) return;
    auto it = C.caps.find(a);
    if (it != C.caps.end() && it->second == 1) return skipped();
    C.caps[a] = 1;
    gl_api.Enable(a);
}
void FramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) {
    if (LIVE(rec(I_FramebufferRenderbuffer, 0, 0, a, b, c, d))) gl_api.FramebufferRenderbuffer(a, b, c, d);
}
void FramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) {
    if (LIVE(rec(I_FramebufferTexture2D, 0, 0, a, b, c, d, e))) gl_api.FramebufferTexture2D(a, b, c, d, e);
}
void FrontFace(GLenum a) {
    if (!live(rec(I_FrontFace, 0, 0, a))) return;
    if (C.front == a) return skipped();
    C.front = a;
    gl_api.FrontFace(a);
}

// the names a Gen hands back: the rewrite's pass gets the original's
#define GEN(NAME)                                                                                                    \
    void NAME(GLsizei n, GLuint* out) {                                                                              \
        int ph = rec(I_##NAME, 0, 0, n);                                                                             \
        if (ph == 2) { take(out, (size_t)n * 4); return; }                                                           \
        gl_api.NAME(n, out);                                                                                         \
        if (ph == 1) save(out, (size_t)n * 4);                                                                       \
    }
GEN(GenFramebuffers)
GEN(GenRenderbuffers)
GEN(GenTextures)
#undef GEN

void PixelStorei(GLenum a, GLint v) {
    if (a == GL_PACK_ALIGNMENT) g_pack = v;
    if (a == GL_UNPACK_ALIGNMENT) g_unpack = v;
    if (LIVE(rec(I_PixelStorei, 0, 0, a, v))) gl_api.PixelStorei(a, v);
}
void ReadPixels(GLint x, GLint y, GLsizei wd, GLsizei ht, GLenum f, GLenum t, void* out) {
    int ph = rec(I_ReadPixels, 0, 0, x, y, wd, ht, f, t);
    size_t n = pixel_bytes(wd, ht, f, t, g_pack);
    if (ph == 2) { take(out, n); return; }
    gl_api.ReadPixels(x, y, wd, ht, f, t, out);
    if (ph == 1) save(out, n);
}
void RenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) {
    if (LIVE(rec(I_RenderbufferStorage, 0, 0, a, b, c, d))) gl_api.RenderbufferStorage(a, b, c, d);
}
void Scissor(GLint a, GLint b, GLsizei c, GLsizei d) {
    if (!live(rec(I_Scissor, 0, 0, a, b, c, d))) return;
    const GLint v[4] = {a, b, c, d};
    if (C.sc_ok && !memcmp(C.sc, v, sizeof v)) return skipped();
    C.sc_ok = true, memcpy(C.sc, v, sizeof v);
    gl_api.Scissor(a, b, c, d);
}
void TexImage2D(GLenum t, GLint l, GLint i, GLsizei wd, GLsizei ht, GLint b, GLenum f, GLenum ty, const void* p) {
    if (LIVE(rec(I_TexImage2D, p, p ? pixel_bytes(wd, ht, f, ty, g_unpack) : 0, t, l, i, wd, ht, b, f, ty)))
        gl_api.TexImage2D(t, l, i, wd, ht, b, f, ty, p);
}
void TexParameterf(GLenum a, GLenum b, GLfloat c) {
    if (!live(rec(I_TexParameterf, 0, 0, a, b, c))) return;
    if (a == GL_TEXTURE_2D && !texparam_changed(b, bits_of(c))) return skipped();
    gl_api.TexParameterf(a, b, c);
}
void TexParameteri(GLenum a, GLenum b, GLint c) {
    if (!live(rec(I_TexParameteri, 0, 0, a, b, c))) return;
    if (a == GL_TEXTURE_2D && !texparam_changed(b, bits_of(c))) return skipped();
    gl_api.TexParameteri(a, b, c);
}
void TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei wd, GLsizei ht, GLenum f, GLenum ty, const void* p) {
    if (LIVE(rec(I_TexSubImage2D, p, p ? pixel_bytes(wd, ht, f, ty, g_unpack) : 0, t, l, x, y, wd, ht, f, ty)))
        gl_api.TexSubImage2D(t, l, x, y, wd, ht, f, ty, p);
}
void Uniform1f(GLint l, GLfloat a) {
    if (!live(rec(I_Uniform1f, 0, 0, l, a))) return;
    const uint32_t v[1] = {bits_of(a)};
    if (!uniform_changed(l, v, 1)) return skipped();
    gl_api.Uniform1f(l, a);
}
void Uniform1i(GLint l, GLint a) {
    if (!live(rec(I_Uniform1i, 0, 0, l, a))) return;
    const uint32_t v[1] = {bits_of(a)};
    if (!uniform_changed(l, v, 1)) return skipped();
    gl_api.Uniform1i(l, a);
}
void Uniform2f(GLint l, GLfloat a, GLfloat b) {
    if (!live(rec(I_Uniform2f, 0, 0, l, a, b))) return;
    const uint32_t v[2] = {bits_of(a), bits_of(b)};
    if (!uniform_changed(l, v, 2)) return skipped();
    gl_api.Uniform2f(l, a, b);
}
void Uniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) {
    if (!live(rec(I_Uniform3f, 0, 0, l, a, b, c))) return;
    const uint32_t v[3] = {bits_of(a), bits_of(b), bits_of(c)};
    if (!uniform_changed(l, v, 3)) return skipped();
    gl_api.Uniform3f(l, a, b, c);
}
void Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    if (!live(rec(I_Uniform4f, 0, 0, l, a, b, c, d))) return;
    const uint32_t v[4] = {bits_of(a), bits_of(b), bits_of(c), bits_of(d)};
    if (!uniform_changed(l, v, 4)) return skipped();
    gl_api.Uniform4f(l, a, b, c, d);
}
void UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* m) {
    if (!live(rec(I_UniformMatrix4fv, m, (size_t)n * 64, l, n, t))) return;
    if (n == 1 && !t && !uniform_changed(l, (const uint32_t*)m, 16)) return skipped();
    gl_api.UniformMatrix4fv(l, n, t, m);
}
void UseProgram(GLuint p) {
    if (!live(rec(I_UseProgram, 0, 0, p))) return;
    if (C.program == p) return skipped();
    C.program = p;
    gl_api.UseProgram(p);
}
void Viewport(GLint a, GLint b, GLsizei c, GLsizei d) {
    if (!live(rec(I_Viewport, 0, 0, a, b, c, d))) return;
    const GLint v[4] = {a, b, c, d};
    if (C.vp_ok && !memcmp(C.vp, v, sizeof v)) return skipped();
    C.vp_ok = true, memcpy(C.vp, v, sizeof v);
    gl_api.Viewport(a, b, c, d);
}

void SwapWindow(SDL_Window* win) {
    if (!live(rec(I_SwapWindow, 0, 0))) return;
    gl_api.SwapWindow(win);
    C.valid = false;                             // (gl_dxgi.cpp's present binds framebuffers directly)
}
void cache_reset() { C.valid = false; }
// a deleted texture: it unbinds from every unit, and its parameters go with it (its name can come back from GenTextures)
void cache_forget_texture(GLuint t) {
    for (GLuint& u : C.tex)
        if (u == t) u = 0;
    for (auto it = C.texparams.begin(); it != C.texparams.end();)
        it = (GLuint)(it->first >> 32) == t ? C.texparams.erase(it) : std::next(it);
}
void GetDrawableSize(SDL_Window* win, int* wd, int* ht) {
    int ph = rec(I_GetDrawableSize, 0, 0);
    int v[2] = {0, 0};
    if (ph == 2) take(v, sizeof v);
    else {
        gl_api.GetDrawableSize(win, &v[0], &v[1]);
        if (ph == 1) save(v, sizeof v);
    }
    *wd = v[0], *ht = v[1];
}
void SetView(int x0, int y0, int wd, int ht, int gw, int gh) {
    if (LIVE(rec(I_SetView, 0, 0, x0, y0, wd, ht, gw, gh))) gl_api.SetView(x0, y0, wd, ht, gw, gh);
}

}  // namespace glr
