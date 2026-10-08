// gl_table.h -- every OpenGL call the renderer makes, through one table (M3 graphics, step G2).
//
// The renderer (gl_core.cpp) never calls OpenGL, SDL's GL functions or the platform's view directly: it calls
// the functions below, which go through `gl_api` -- the real entry points in the game, fakes in a harness.
//
// During a shadow check (port.h) each call is also recorded in the pass's output log, its arguments by value
// and whatever it reads through a pointer (pixels, vertices, a matrix) hashed past 64 bytes, and the two passes'
// logs are compared like any other output. The original's pass (phase 1) makes the calls; the rewrite's pass
// (phase 2) only records them -- the GPU sees each frame once. What a call hands back (new object names, a
// framebuffer's status, pixels read back, the window's size) is queued in phase 1 and handed to phase 2 in order,
// so both passes see the same answers. Calls made while the renderer starts (gl_start: shaders, buffers) go
// straight through, unrecorded: they happen once per process (see gl_core.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "SDL.h"
#include "SDL_opengl.h"

// the real entry points (or a harness's fakes). GL 1.1 is exported by opengl32; the rest come from the context.
// Their calling convention: Windows' APIENTRY (__stdcall); on Linux GL's own GLAPIENTRY (cdecl) -- the Linux build's
// windows.h (win32_compat.h) makes APIENTRY __stdcall, which a Linux libGL's functions are not (R2b)
#ifdef _WIN32
#define VP_GLAPIENTRY APIENTRY
#else
#define VP_GLAPIENTRY GLAPIENTRY
#endif
struct GLApi {
#define GL_API_FN(ret, name, args) ret(VP_GLAPIENTRY* name) args;
#include "gl_api.inc"
#undef GL_API_FN
    // SDL and the platform layer
    void (*SwapWindow)(SDL_Window*);
    void (*GetDrawableSize)(SDL_Window*, int*, int*);
    void (*SetView)(int x0, int y0, int w, int h, int game_w, int game_h);   // platform_set_view: the mouse's map
};
extern GLApi gl_api;
bool gl_api_load();                      // fill gl_api from the current context; false if a function is missing

// ---- the calls, recorded in a check --------------------------------------------------------------------------------
namespace glr {
// the renderer's start-up (shader compiles, vertex arrays): made directly, never recorded (see above)
struct Direct { Direct(); ~Direct(); };

void ActiveTexture(GLenum);
void BindBuffer(GLenum, GLuint);
void BindFramebuffer(GLenum, GLuint);
void BindRenderbuffer(GLenum, GLuint);
void BindTexture(GLenum, GLuint);
void BindVertexArray(GLuint);
void BlendFunc(GLenum, GLenum);
void BlitFramebuffer(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
void BufferData(GLenum, ptrdiff_t, const void*, GLenum);
GLenum CheckFramebufferStatus(GLenum);
void Clear(GLbitfield);
void ClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void ClearDepth(GLdouble);
void CullFace(GLenum);
void DeleteFramebuffers(GLsizei, const GLuint*);
void DeleteRenderbuffers(GLsizei, const GLuint*);
void DeleteTextures(GLsizei, const GLuint*);
void DepthFunc(GLenum);
void DepthMask(GLboolean);
void Disable(GLenum);
void DrawArrays(GLenum, GLint, GLsizei);
void DrawElements(GLenum, GLsizei, GLenum, const void*);   // from the bound element buffer: the pointer is an offset
void Enable(GLenum);
void FramebufferRenderbuffer(GLenum, GLenum, GLenum, GLuint);
void FramebufferTexture2D(GLenum, GLenum, GLenum, GLuint, GLint);
void FrontFace(GLenum);
void GenFramebuffers(GLsizei, GLuint*);
void GenRenderbuffers(GLsizei, GLuint*);
void GenTextures(GLsizei, GLuint*);
void PixelStorei(GLenum, GLint);
void ReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
void RenderbufferStorage(GLenum, GLenum, GLsizei, GLsizei);
void Scissor(GLint, GLint, GLsizei, GLsizei);
void TexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void TexParameterf(GLenum, GLenum, GLfloat);
void TexParameteri(GLenum, GLenum, GLint);
void TexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
void Uniform1f(GLint, GLfloat);
void Uniform1i(GLint, GLint);
void Uniform2f(GLint, GLfloat, GLfloat);
void Uniform3f(GLint, GLfloat, GLfloat, GLfloat);
void Uniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void UniformMatrix4fv(GLint, GLsizei, GLboolean, const GLfloat*);
void UseProgram(GLuint);
void Viewport(GLint, GLint, GLsizei, GLsizei);
void SwapWindow(SDL_Window*);
void cache_reset();                              // after GL state set outside glr (gl_api directly)
void cache_forget_texture(GLuint);               // after a texture deleted outside glr
void stream_buffers(GLuint vao, GLuint vbo, GLuint ibo);   // the draws' buffers: their uploads stream (live side)
void GetDrawableSize(SDL_Window*, int*, int*);
void SetView(int x0, int y0, int w, int h, int game_w, int game_h);
}  // namespace glr

// the recorder's ids, in the output log
enum : uint16_t { GLREC_BASE = 0x4000 };
const char* gl_record_name(uint16_t method);    // "glDrawElements"...; 0 for anything else
