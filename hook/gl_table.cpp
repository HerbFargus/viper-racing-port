// gl_table.cpp -- the renderer's OpenGL calls, made through gl_api and recorded during a shadow check (gl_table.h).
#define _CRT_SECURE_NO_WARNINGS
#include "gl_table.h"
#include <string.h>
#include <vector>
#include "viperport.h"
#include "port.h"

void platform_set_view(int x0, int y0, int w, int h, int game_w, int game_h);

GLApi gl_api;

bool gl_api_load() {
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    bool ok = true;
#define GL_API_FN(ret, name, args)                                                                                   \
    {                                                                                                                \
        void* p = SDL_GL_GetProcAddress("gl" #name);                                                                 \
        if (!p && gl32) p = (void*)GetProcAddress(gl32, "gl" #name);                                                 \
        *(void**)&gl_api.name = p;                                                                                   \
        if (!p) logf("gl_table: gl" #name " is missing"), ok = false;                                                \
    }
#include "gl_api.inc"
#undef GL_API_FN
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

__declspec(thread) int t_direct;                       // inside glr::Direct: the start-up's calls

// what the calls hand back, queued by the original's pass for the rewrite's (per check, per thread)
struct Feed { unsigned check = ~0u; int ph = 0; std::vector<uint8_t> q; size_t at = 0; };
__declspec(thread) Feed* t_feed;

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

void ActiveTexture(GLenum a) { if (LIVE(rec(I_ActiveTexture, 0, 0, a))) gl_api.ActiveTexture(a); }
void BindBuffer(GLenum a, GLuint b) { if (LIVE(rec(I_BindBuffer, 0, 0, a, b))) gl_api.BindBuffer(a, b); }
void BindFramebuffer(GLenum a, GLuint b) { if (LIVE(rec(I_BindFramebuffer, 0, 0, a, b))) gl_api.BindFramebuffer(a, b); }
void BindRenderbuffer(GLenum a, GLuint b) { if (LIVE(rec(I_BindRenderbuffer, 0, 0, a, b))) gl_api.BindRenderbuffer(a, b); }
void BindTexture(GLenum a, GLuint b) { if (LIVE(rec(I_BindTexture, 0, 0, a, b))) gl_api.BindTexture(a, b); }
void BindVertexArray(GLuint a) { if (LIVE(rec(I_BindVertexArray, 0, 0, a))) gl_api.BindVertexArray(a); }
void BlendFunc(GLenum a, GLenum b) { if (LIVE(rec(I_BlendFunc, 0, 0, a, b))) gl_api.BlendFunc(a, b); }
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
void ClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { if (LIVE(rec(I_ClearColor, 0, 0, r, g, b, a))) gl_api.ClearColor(r, g, b, a); }
void ClearDepth(GLdouble d) {
    uint32_t b[2];
    memcpy(b, &d, 8);
    if (LIVE(rec(I_ClearDepth, 0, 0, b[0], b[1]))) gl_api.ClearDepth(d);
}
void CullFace(GLenum a) { if (LIVE(rec(I_CullFace, 0, 0, a))) gl_api.CullFace(a); }
void DeleteFramebuffers(GLsizei n, const GLuint* p) { if (LIVE(rec(I_DeleteFramebuffers, p, (size_t)n * 4, n))) gl_api.DeleteFramebuffers(n, p); }
void DeleteRenderbuffers(GLsizei n, const GLuint* p) { if (LIVE(rec(I_DeleteRenderbuffers, p, (size_t)n * 4, n))) gl_api.DeleteRenderbuffers(n, p); }
void DeleteTextures(GLsizei n, const GLuint* p) { if (LIVE(rec(I_DeleteTextures, p, (size_t)n * 4, n))) gl_api.DeleteTextures(n, p); }
void DepthFunc(GLenum a) { if (LIVE(rec(I_DepthFunc, 0, 0, a))) gl_api.DepthFunc(a); }
void DepthMask(GLboolean a) { if (LIVE(rec(I_DepthMask, 0, 0, a))) gl_api.DepthMask(a); }
void Disable(GLenum a) { if (LIVE(rec(I_Disable, 0, 0, a))) gl_api.Disable(a); }
void DrawArrays(GLenum m, GLint f, GLsizei n) { if (LIVE(rec(I_DrawArrays, 0, 0, m, f, n))) gl_api.DrawArrays(m, f, n); }
void DrawElements(GLenum m, GLsizei n, GLenum t, const void* off) {
    if (LIVE(rec(I_DrawElements, 0, 0, m, n, t, (uint32_t)(uintptr_t)off))) gl_api.DrawElements(m, n, t, off);
}
void Enable(GLenum a) { if (LIVE(rec(I_Enable, 0, 0, a))) gl_api.Enable(a); }
void FramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) {
    if (LIVE(rec(I_FramebufferRenderbuffer, 0, 0, a, b, c, d))) gl_api.FramebufferRenderbuffer(a, b, c, d);
}
void FramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) {
    if (LIVE(rec(I_FramebufferTexture2D, 0, 0, a, b, c, d, e))) gl_api.FramebufferTexture2D(a, b, c, d, e);
}
void FrontFace(GLenum a) { if (LIVE(rec(I_FrontFace, 0, 0, a))) gl_api.FrontFace(a); }

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
void Scissor(GLint a, GLint b, GLsizei c, GLsizei d) { if (LIVE(rec(I_Scissor, 0, 0, a, b, c, d))) gl_api.Scissor(a, b, c, d); }
void TexImage2D(GLenum t, GLint l, GLint i, GLsizei wd, GLsizei ht, GLint b, GLenum f, GLenum ty, const void* p) {
    if (LIVE(rec(I_TexImage2D, p, p ? pixel_bytes(wd, ht, f, ty, g_unpack) : 0, t, l, i, wd, ht, b, f, ty)))
        gl_api.TexImage2D(t, l, i, wd, ht, b, f, ty, p);
}
void TexParameterf(GLenum a, GLenum b, GLfloat c) { if (LIVE(rec(I_TexParameterf, 0, 0, a, b, c))) gl_api.TexParameterf(a, b, c); }
void TexParameteri(GLenum a, GLenum b, GLint c) { if (LIVE(rec(I_TexParameteri, 0, 0, a, b, c))) gl_api.TexParameteri(a, b, c); }
void TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei wd, GLsizei ht, GLenum f, GLenum ty, const void* p) {
    if (LIVE(rec(I_TexSubImage2D, p, p ? pixel_bytes(wd, ht, f, ty, g_unpack) : 0, t, l, x, y, wd, ht, f, ty)))
        gl_api.TexSubImage2D(t, l, x, y, wd, ht, f, ty, p);
}
void Uniform1f(GLint l, GLfloat a) { if (LIVE(rec(I_Uniform1f, 0, 0, l, a))) gl_api.Uniform1f(l, a); }
void Uniform1i(GLint l, GLint a) { if (LIVE(rec(I_Uniform1i, 0, 0, l, a))) gl_api.Uniform1i(l, a); }
void Uniform2f(GLint l, GLfloat a, GLfloat b) { if (LIVE(rec(I_Uniform2f, 0, 0, l, a, b))) gl_api.Uniform2f(l, a, b); }
void Uniform3f(GLint l, GLfloat a, GLfloat b, GLfloat c) { if (LIVE(rec(I_Uniform3f, 0, 0, l, a, b, c))) gl_api.Uniform3f(l, a, b, c); }
void Uniform4f(GLint l, GLfloat a, GLfloat b, GLfloat c, GLfloat d) {
    if (LIVE(rec(I_Uniform4f, 0, 0, l, a, b, c, d))) gl_api.Uniform4f(l, a, b, c, d);
}
void UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* m) {
    if (LIVE(rec(I_UniformMatrix4fv, m, (size_t)n * 64, l, n, t))) gl_api.UniformMatrix4fv(l, n, t, m);
}
void UseProgram(GLuint p) { if (LIVE(rec(I_UseProgram, 0, 0, p))) gl_api.UseProgram(p); }
void Viewport(GLint a, GLint b, GLsizei c, GLsizei d) { if (LIVE(rec(I_Viewport, 0, 0, a, b, c, d))) gl_api.Viewport(a, b, c, d); }

void SwapWindow(SDL_Window* win) { if (LIVE(rec(I_SwapWindow, 0, 0))) gl_api.SwapWindow(win); }
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
