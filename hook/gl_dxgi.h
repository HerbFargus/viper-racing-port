// gl_dxgi.h -- the renderer's picture presented through a DXGI flip-model swap chain (hi-res 2D stage, H1).
//
// A full-screen OpenGL window on NVIDIA bypasses the desktop's compositor, so Windows screenshots (PrintScreen,
// Win+Shift+S, Game Bar) and the taskbar's preview showed a stale frame. When the driver offers WGL_NV_DX_interop2,
// the finished frame is instead copied, on the GPU, into a D3D11 texture shared with OpenGL and presented through a
// flip-model swap chain on the game's window, which the compositor always sees.
//
// One consistent behaviour, no switch: if anything is missing or fails (no extension -- AMD, Intel, Wine -- or a
// failing call, at start or later), one line is logged and SDL_GL_SwapWindow presents for the rest of the run, as
// before. d3d11.dll is loaded at run time and the WGL functions come from the context: nothing to link.
//
// Never part of a check: gl_core.cpp installs dxgi::present as gl_api.SwapWindow's live function, so the recorded
// call stream (glr::SwapWindow) is unchanged, and a check's rewrite pass, which makes no live calls, never reaches it.
// The OpenGL calls made here go straight to gl_api, unrecorded. A harness (start_headless) never starts it.
#pragma once
#include "gl_table.h"

namespace dxgi {
// with the renderer's OpenGL context current on this thread; false: SDL_GL_SwapWindow presents
bool start(SDL_Window* win);
// shows the w x h colour of src_fbo (its row 0 the top of the picture) and returns with draw framebuffer 0 and
// read framebuffer src_fbo bound. Without the swap chain (never started, or failed): SDL_GL_SwapWindow(win) --
// the caller has already blitted the picture into the window's own framebuffer, so the fallback shows this frame.
void present(SDL_Window* win, GLuint src_fbo, int w, int h);
bool active();
}  // namespace dxgi
