// gx_gl.h -- the renderer's draw call for the DirectX layer's draw wrappers (gx_dx.cpp), without DirectX's headers
#pragma once
#include <stdint.h>

// a triangle list (vtype: 2 D3DVT_LVERTEX, 3 D3DVT_TLVERTEX), indexed when idx isn't 0; the HRESULT
int32_t gfx_draw_triangles(uint32_t vtype, const void* v, uint32_t nv, const uint16_t* idx, uint32_t ni);
