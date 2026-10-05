// edit_build.h -- M3 UI stage, step U5 (group B): the model builder's and the 3D Studio / DXF readers' layouts, statics and
// callees (library `edit`: modbuild.obj, cvt3ds.obj, adtools.obj), shared by hook/edit_build.cpp, hook/edit_3ds.cpp and
// test/world_edit_build.cpp.
//
// Recovered from the v1.0 disassembly (ModBuilderCreate's allocations, the accessors, BuildInfo / BuildGeometry, the chunk
// parsers' blocks). Fields are volatile: the rewrites read and write each one where the original does, in its order (a
// builder's arrays and the caller's points can alias).
//
//   ModBuilder (0x4c, MemAlloc'd by ModBuilderCreate(ntris, nsurfs); the model tool makes two of 4096, 4096): its vertices
//     (3 x ntris of them), triangles and surfaces, then the mrModelInfo BuildInfo / BuildGeometry fill (its arrays
//     allocated to the same capacities: vertices 3 x ntris, surfaces nsurfs, triangles ntris)
//   ModBuilderVertex (0x18): position, normal (make_vertex_normals)
//   ModBuilderTriangle (0x10): three vertex indices, three edge flags (1: a faceted edge, 0: smoothed), a byte never set
//   ModBuilderSurface (0x2c): texture points (vertex, u, v; capacity the builder's vertices), texture triangles (three
//     texture points and the triangle; capacity the builder's triangles), the texture's name[16], two material bytes, a group
//   mrModelInfo (0x28; gx_model.cpp): vertices (0x20: position, normal, u, v), surfaces (0x20: name[16], two material bytes,
//     a group, first / end vertex, first / end triangle -- shorts), triangles (8: three short vertices, a short)
//   ADParseBlock (0xc): a chunk id (0xffff: any), its parser (id, the chunk's data length, data), the parser's data; a list
//     ends with id 0
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ui_types.h"

namespace ebld {
using uit::Edx;

struct MbVertex {                                 // 0x18
    volatile float x, y, z;                       // +0 position
    volatile float nx, ny, nz;                    // +0xc normal
};
static_assert(sizeof(MbVertex) == 0x18, "MbVertex");
struct MbTriangle {                               // 0x10
    volatile int32_t v[3];                        // +0
    volatile uint8_t edge[3];                     // +0xc (v0 v1), (v1 v2), (v2 v0): 1 faceted
    volatile uint8_t pad;                         // +0xf (never set: whatever the caller's copy held)
};
static_assert(sizeof(MbTriangle) == 0x10, "MbTriangle");
struct MbPoint {                                  // 0xc: a texture point
    volatile int32_t vert;
    volatile float u, v;
};
static_assert(sizeof(MbPoint) == 0xc, "MbPoint");
struct MbTexTri {                                 // 0x10: a texture triangle
    volatile int32_t p[3];                        // texture points
    volatile int32_t tri;                         // +0xc the builder's triangle
};
static_assert(sizeof(MbTexTri) == 0x10, "MbTexTri");
struct MbSurface {                                // 0x2c
    MbPoint* volatile points;                     // +0
    volatile int32_t npoints, maxpoints;          // +4, +8
    MbTexTri* volatile tris;                      // +0xc
    volatile int32_t ntris, maxtris;              // +0x10, +0x14
    char name[16];                                // +0x18 the texture
    volatile uint8_t mat0, mat1;                  // +0x28
    volatile int16_t group;                       // +0x2a
};
static_assert(sizeof(MbSurface) == 0x2c && offsetof(MbSurface, name) == 0x18, "MbSurface");

struct MrVertex {                                 // 0x20
    volatile float x, y, z, nx, ny, nz, u, v;
};
static_assert(sizeof(MrVertex) == 0x20, "MrVertex");
struct MrSurface {                                // 0x20
    char name[16];                                // +0
    volatile uint8_t type, b11;                   // +0x10
    uint8_t _12[2];
    volatile int16_t group;                       // +0x14
    uint8_t _16[2];
    volatile int16_t v0, v1;                      // +0x18 first, end vertex
    volatile int16_t t0, t1;                      // +0x1c first, end triangle
};
static_assert(sizeof(MrSurface) == 0x20 && offsetof(MrSurface, v0) == 0x18, "MrSurface");
struct MrTriangle {                               // 8
    volatile int16_t v[3];
    volatile int16_t flags;
};
static_assert(sizeof(MrTriangle) == 8, "MrTriangle");
struct MrModelInfo {                              // 0x28
    volatile int32_t nverts;                      // +0
    MrVertex* volatile verts;                     // +4
    volatile int32_t nsurfs;                      // +8
    MrSurface* volatile surfs;                    // +0xc
    volatile int32_t ntris;                       // +0x10
    MrTriangle* volatile tris;                    // +0x14
    volatile int32_t n18; void* volatile p1c;     // +0x18
    volatile int32_t n20; void* volatile p24;     // +0x20
};
static_assert(sizeof(MrModelInfo) == 0x28, "MrModelInfo");

struct ModBuilder {                               // 0x4c
    MbVertex* volatile verts;                     // +0
    volatile int32_t nverts, maxverts;            // +4, +8
    MbTriangle* volatile tris;                    // +0xc
    volatile int32_t ntris, maxtris;              // +0x10, +0x14
    MbSurface* volatile surfs;                    // +0x18
    volatile int32_t nsurfs, maxsurfs;            // +0x1c, +0x20
    MrModelInfo info;                             // +0x24
};
static_assert(sizeof(ModBuilder) == 0x4c && offsetof(ModBuilder, info) == 0x24, "ModBuilder");

typedef uint8_t(__cdecl* ADParser)(uint32_t id, int32_t len, void* data);   // id: the chunk's id (a dword: high half garbage)
struct ADParseBlock {                             // 0xc
    volatile uint16_t id;
    uint8_t _2[2];
    volatile ADParser fn;
    void* volatile data;
};
static_assert(sizeof(ADParseBlock) == 0xc, "ADParseBlock");

struct EbP3 { volatile float x, y, z; };          // P3DBase
struct EbFrame { volatile float m[9]; volatile float p[3]; };   // Frame: rows, then the position
static_assert(sizeof(EbFrame) == 0x30, "EbFrame");

// ---- statics ------------------------------------------------------------------------------------------------------------
enum : uint32_t {
    // adtools.obj
    S_AD_IN = 0x004ff420,            // FILE*: the file read
    S_AD_OUT = 0x004ff424,           // FILE*: the file written
    S_AD_LIMIT = 0x004ff428,         // the end of the chunk being parsed (100000000 at ADOpen)
    S_AD_DEPTH = 0x005ce048,         // the chunks open for writing
    S_AD_LOG = 0x005ce04c,           // FILE*: out.txt, the reader's debug log (0: not open)
    S_AD_STACK = 0x005ce050,         // long[1024]: each open chunk's offset
    S_AD_NEST = 0x005cf050,          // ADParseChunks' nesting
    // cvt3ds.obj
    S_3DS_NVERTS = 0x005cdfc0, S_3DS_NTRIS = 0x005cdfc8, S_3DS_NMATS = 0x005cdfd0, S_3DS_NOBJS = 0x005cdfe0,
    S_3DS_FRAME = 0x005cdfe8,        // Frame: an object's axes (parse_object sets it to identity)
    S_3DS_NUV = 0x005ce024,
    // modbuild.obj: the DXF reader's
    S_DXF_CODE = 0x005cdd68,         // the group code read
    S_DXF_GROUP = 0x005cdd78,        // the entity's name (char[0x118])
    S_DXF_LINE = 0x005cde90,         // the value's line (char[0x100])
    S_DXF_INT = 0x005cdf98,          // the value as an integer
    S_DXF_FLOAT = 0x005cdfb0,        // the value as a float
    S_FACE_WARNED = 0x004fefe4,      // u8: get_face_normal has logged a degenerate triangle
    S_ADJUST_FDIV = 0x005024b8,
};

// ---- the game's functions (v1.0 addresses) ------------------------------------------------------------------------------
enum : uint32_t {
    // kernel
    F_LogReport = 0x00411150, F_FileOpen = 0x00411780, F_FileClose = 0x00411850, F_FileReadLine = 0x004119b0,
    F_MemAlloc = 0x004140e0, F_Delete = 0x00414390,
    // maths, models
    F_VectorLength = 0x00429100, F_VectorSub = 0x00429120, F_VectorNormalize = 0x0043e4a0, F_DotProduct = 0x0045d8e0,
    F_mrModelInfoGet = 0x004562f0, F_mrModelInfoForget = 0x00456460,
    // the C runtime
    F_strchr = 0x004ce0c0, F_sscanf = 0x004ce190, F_sprintf = 0x004cf0a0, F_printf = 0x004d01c0, F_fseek = 0x004d0200,
    F_fprintf = 0x004d02a0, F_ftell = 0x004d02e0, F_fread = 0x004d0490, F_fopen = 0x004d0610, F_fclose = 0x004d0630,
    F_fwrite = 0x004d06a0, F_stricmp = 0x004da350,
    // modbuild.obj
    F_ModBuilderClear = 0x004b71d0, F_dup_surface = 0x004b72d0, F_ModBuilderAddVertex = 0x004b7370,
    F_ModBuilderAddTriangle = 0x004b7470, F_is_triangle_same = 0x004b74f0, F_ModBuilderAddSurface = 0x004b7800,
    F_ModBuilderSetSurfaceTexture = 0x004b78e0, F_ModBuilderSetSurfaceMaterial = 0x004b7960,
    F_ModBuilderSetSurfaceGroup = 0x004b79c0, F_ModBuilderAddTextureTriangle = 0x004b7a00,
    F_ModBuilderAddTexturePoint = 0x004b7ba0, F_info_add_surface = 0x004b89d0, F_info_add_vertex = 0x004b8a60,
    F_info_add_triangle = 0x004b8b80, F_do_3dface = 0x004b9070, F_make_vertex = 0x004b91e0, F_do_polyline = 0x004b9210,
    F_ModBuilderImportMI = 0x004b95a0, F_get_face_normal = 0x004b99f0, F_edge_compare = 0x004b9b80,
    F_make_vertex_normals = 0x004b9bc0, F_dxf_next_group = 0x004ba000, F_dxf_read_item = 0x004ba070,
    F_dxf_next_item = 0x004ba180, F_ModBuilderVertex_ctor = 0x004ba1a0,
    // cvt3ds.obj
    F_Read3DS = 0x004ba3d0, F_ADWriteColorChunk = 0x004badf0, F_ADWritePercentageChunk = 0x004bae30,
    F_parse_primary = 0x004bae60, F_parse_m3d_version = 0x004baeb0, F_parse_edit = 0x004baed0,
    F_parse_mesh_version = 0x004baf80, F_parse_material_list = 0x004bafa0, F_parse_materials_name = 0x004bb110,
    F_parse_texture = 0x004bb140, F_parse_object = 0x004bb2e0, F_fixup_vertex = 0x004bb420, F_parse_tri_list = 0x004bb4e0,
    F_parse_vertex_list = 0x004bb570, F_parse_face_list = 0x004bb640, F_parse_face_material = 0x004bb760,
    F_parse_mapping_coords = 0x004bb7d0, F_parse_mesh_mat_group = 0x004bb860, F_parse_smooth_group = 0x004bb8b0,
    F_parse_local_axis = 0x004bb8e0, F_parse_material_color = 0x004bb900, F_parse_material_percent = 0x004bb940,
    F_parse_color = 0x004bb980, F_parse_percent = 0x004bb9b0, F_parse_short = 0x004bb9d0, F_parse_float = 0x004bb9f0,
    // adtools.obj
    F_ADParseChunks = 0x004bba30, F_out_tab = 0x004bbbd0, F_ADOpen = 0x004bbc00, F_ADClose = 0x004bbc80,
    F_ADReadChunk = 0x004bbcb0, F_ADRead = 0x004bbd50, F_ADReadString = 0x004bbdb0, F_ADReadShort = 0x004bbe20,
    F_ADReadLong = 0x004bbe70, F_ADReadFloat = 0x004bbec0, F_ADReadChar = 0x004bbf10, F_ADCreate = 0x004bbfc0,
    F_ADCreateClose = 0x004bc000, F_ADCreateChunk = 0x004bc030, F_ADCloseChunk = 0x004bc0a0, F_ADWriteShort = 0x004bc120,
    F_ADWriteLong = 0x004bc150, F_ADWriteString = 0x004bc180, F_ADWriteFloat = 0x004bc1c0, F_ADWriteByte = 0x004bc1f0,
    F_ADWrite = 0x004bc220,
};

// ---- memory and calls -----------------------------------------------------------------------------------------------------
#define EB_G8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define EB_G16(a) (*(volatile int16_t*)(uintptr_t)(a))
#define EB_G32(a) (*(volatile int32_t*)(uintptr_t)(a))
#define EB_GU32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define EB_GF(a) (*(volatile float*)(uintptr_t)(a))
#define EB_GP(T, a) (*(T* volatile*)(uintptr_t)(a))
#define EB_CP(a) ((const char*)(uintptr_t)(a))
using uit::ccall;
using uit::tcall;
typedef void(__cdecl* Log_t)(const char*, ...);
#define EB_Log ((Log_t)(uintptr_t)F_LogReport)
#define EB_printf ((Log_t)(uintptr_t)F_printf)
typedef int(__cdecl* FPrintf_t)(void*, const char*, ...);
#define EB_fprintf ((FPrintf_t)(uintptr_t)F_fprintf)
typedef int(__cdecl* SPrintf_t)(char*, const char*, ...);
#define EB_sprintf ((SPrintf_t)(uintptr_t)F_sprintf)
typedef int(__cdecl* SScanf_t)(const char*, const char*, ...);
#define EB_sscanf ((SScanf_t)(uintptr_t)F_sscanf)

// a float's bits, moved as the original moves it (mov, not fld / fstp: a signalling NaN stays signalling)
static __forceinline uint32_t eb_bits(const volatile float* p) { return *(const volatile uint32_t*)p; }
static __forceinline void eb_put(volatile float* p, uint32_t b) { *(volatile uint32_t*)p = b; }
static __forceinline void eb_cp(volatile float* d, const volatile float* s) { eb_put(d, eb_bits(s)); }
static __forceinline void eb_cp3(volatile float* d, const volatile float* s) {   // three, x y z in order
    eb_cp(d, s); eb_cp(d + 1, s + 1); eb_cp(d + 2, s + 2);
}
// a stored float (fstp dword): through a volatile, so the store is made where the original makes it
static __forceinline void eb_st(volatile float* p, double v) { *p = (float)v; }
}  // namespace ebld
