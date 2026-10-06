// dx_compat.h -- DirectDraw, Direct3D and DirectSound as a DirectX 5 program sees them, for the native Linux build
// (relink stage R2b): on Linux, hook/dx5.h's <ddraw.h> / <d3d.h> and ds5.h's <dsound.h> are this file
// (tools/build_linux.sh: -I hook/linux_inc). Only what the port uses: the structures at their exact 32-bit DirectX 5
// layouts (static_asserted -- DIRECTDRAW_VERSION / DIRECT3D_VERSION / DIRECTSOUND_VERSION 0x0500 as dx5.h / ds5.h set:
// D3DDEVICEDESC is DirectX 5's 0xcc bytes, DSBUFFERDESC 0x14, DDCAPS DDCAPS_DX5), the constants and error codes, the
// interfaces (the generated Base_* classes of com_base.h / com_dsound.h implement them: their methods, in vtable order,
// __stdcall with `this` on the stack as COM's -- GCC lays a single-inheritance vtable out as MSVC does: no virtual
// destructor, so slot n is the n-th method), and the interface ids the port compares.
#pragma once
#include "win32_compat.h"

// the SDK's source annotations on the generated methods' parameters: nothing
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Outptr_
#define _Pre_null_
#define _In_reads_bytes_(n)
#define _In_reads_bytes_opt_(n)
#define _Out_writes_bytes_(n)
#define _Out_writes_bytes_opt_(n)
#define _Outptr_result_bytebuffer_(n)
#define _Outptr_opt_result_bytebuffer_(n)

// ---- DirectDraw (ddraw.h) -----------------------------------------------------------------------------------------------
#define _FACDD 0x876
#define MAKE_DDHRESULT(code) MAKE_HRESULT(1, _FACDD, code)
#define DD_OK S_OK
#define DD_FALSE S_FALSE
#define DDERR_GENERIC E_FAIL
#define DDERR_INVALIDPARAMS E_INVALIDARG
#define DDERR_UNSUPPORTED E_NOTIMPL
#define DDERR_NOCOLORKEY MAKE_DDHRESULT(215)
#define DDERR_NOTFOUND MAKE_DDHRESULT(255)
#define DDERR_OUTOFVIDEOMEMORY MAKE_DDHRESULT(380)
#define DDERR_SURFACELOST MAKE_DDHRESULT(450)
#define DDERR_WASSTILLDRAWING MAKE_DDHRESULT(540)
#define DDENUMRET_CANCEL 0
#define DDENUMRET_OK 1
#define DD_ROP_SPACE (256 / 32)
#define DDBD_1 0x00004000
#define DDBD_2 0x00002000
#define DDBD_4 0x00001000
#define DDBD_8 0x00000800
#define DDBD_16 0x00000400
#define DDBD_24 0x00000200
#define DDBD_32 0x00000100
#define DDCAPS_3D 0x00000001
#define DDCAPS_BLT 0x00000040
#define DDCAPS_BLTSTRETCH 0x00000200
#define DDCAPS_ZBLTS 0x00100000
#define DDCAPS_COLORKEY 0x00400000
#define DDCAPS_BLTCOLORFILL 0x04000000
#define DDCKEYCAPS_SRCBLT 0x00000200
#define DDPF_ALPHAPIXELS 0x00000001
#define DDPF_ALPHA 0x00000002
#define DDPF_FOURCC 0x00000004
#define DDPF_PALETTEINDEXED8 0x00000020
#define DDPF_RGB 0x00000040
#define DDPF_ZBUFFER 0x00000400
#define DDSCAPS_BACKBUFFER 0x00000004
#define DDSCAPS_COMPLEX 0x00000008
#define DDSCAPS_FLIP 0x00000010
#define DDSCAPS_FRONTBUFFER 0x00000020
#define DDSCAPS_OFFSCREENPLAIN 0x00000040
#define DDSCAPS_PRIMARYSURFACE 0x00000200
#define DDSCAPS_SYSTEMMEMORY 0x00000800
#define DDSCAPS_TEXTURE 0x00001000
#define DDSCAPS_3DDEVICE 0x00002000
#define DDSCAPS_VIDEOMEMORY 0x00004000
#define DDSCAPS_ZBUFFER 0x00020000
#define DDSCAPS_MIPMAP 0x00400000
#define DDSCAPS_ALLOCONLOAD 0x04000000
#define DDSCAPS_LOCALVIDMEM 0x10000000
#define DDSCAPS_NONLOCALVIDMEM 0x20000000
#define DDSD_CAPS 0x00000001
#define DDSD_HEIGHT 0x00000002
#define DDSD_WIDTH 0x00000004
#define DDSD_PITCH 0x00000008
#define DDSD_BACKBUFFERCOUNT 0x00000020
#define DDSD_ZBUFFERBITDEPTH 0x00000040
#define DDSD_ALPHABITDEPTH 0x00000080
#define DDSD_LPSURFACE 0x00000800
#define DDSD_PIXELFORMAT 0x00001000
#define DDSD_CKDESTOVERLAY 0x00002000
#define DDSD_CKDESTBLT 0x00004000
#define DDSD_CKSRCOVERLAY 0x00008000
#define DDSD_CKSRCBLT 0x00010000
#define DDSD_MIPMAPCOUNT 0x00020000
#define DDSD_REFRESHRATE 0x00040000
#define DDSD_LINEARSIZE 0x00080000
#define DDCKEY_SRCBLT 0x00000008
#define DDBLT_COLORFILL 0x00000400
#define DDBLT_KEYSRC 0x00008000
#define DDBLT_WAIT 0x01000000
#define DDLOCK_WAIT 0x00000001
#define DDFLIP_WAIT 0x00000001
#define DDSCL_FULLSCREEN 0x00000001
#define DDSCL_NORMAL 0x00000008
#define DDSCL_EXCLUSIVE 0x00000010

typedef struct _DDCOLORKEY {
    DWORD dwColorSpaceLowValue;
    DWORD dwColorSpaceHighValue;
} DDCOLORKEY, *LPDDCOLORKEY;
typedef struct _DDSCAPS {
    DWORD dwCaps;
} DDSCAPS, *LPDDSCAPS;
typedef struct _DDPIXELFORMAT {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    union {
        DWORD dwRGBBitCount;
        DWORD dwYUVBitCount;
        DWORD dwZBufferBitDepth;
        DWORD dwAlphaBitDepth;
    };
    union {
        DWORD dwRBitMask;
        DWORD dwYBitMask;
    };
    union {
        DWORD dwGBitMask;
        DWORD dwUBitMask;
    };
    union {
        DWORD dwBBitMask;
        DWORD dwVBitMask;
    };
    union {
        DWORD dwRGBAlphaBitMask;
        DWORD dwYUVAlphaBitMask;
        DWORD dwRGBZBitMask;
        DWORD dwYUVZBitMask;
    };
} DDPIXELFORMAT, *LPDDPIXELFORMAT;
typedef struct _DDSURFACEDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    union {
        LONG lPitch;
        DWORD dwLinearSize;
    };
    DWORD dwBackBufferCount;
    union {
        DWORD dwMipMapCount;
        DWORD dwZBufferBitDepth;
        DWORD dwRefreshRate;
    };
    DWORD dwAlphaBitDepth;
    DWORD dwReserved;
    LPVOID lpSurface;
    DDCOLORKEY ddckCKDestOverlay;
    DDCOLORKEY ddckCKDestBlt;
    DDCOLORKEY ddckCKSrcOverlay;
    DDCOLORKEY ddckCKSrcBlt;
    DDPIXELFORMAT ddpfPixelFormat;
    DDSCAPS ddsCaps;
} DDSURFACEDESC, *LPDDSURFACEDESC;
typedef struct _DDCAPS_DX5 {
    DWORD dwSize;
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCKeyCaps;
    DWORD dwFXCaps;
    DWORD dwFXAlphaCaps;
    DWORD dwPalCaps;
    DWORD dwSVCaps;
    DWORD dwAlphaBltConstBitDepths;
    DWORD dwAlphaBltPixelBitDepths;
    DWORD dwAlphaBltSurfaceBitDepths;
    DWORD dwAlphaOverlayConstBitDepths;
    DWORD dwAlphaOverlayPixelBitDepths;
    DWORD dwAlphaOverlaySurfaceBitDepths;
    DWORD dwZBufferBitDepths;
    DWORD dwVidMemTotal;
    DWORD dwVidMemFree;
    DWORD dwMaxVisibleOverlays;
    DWORD dwCurrVisibleOverlays;
    DWORD dwNumFourCCCodes;
    DWORD dwAlignBoundarySrc;
    DWORD dwAlignSizeSrc;
    DWORD dwAlignBoundaryDest;
    DWORD dwAlignSizeDest;
    DWORD dwAlignStrideAlign;
    DWORD dwRops[DD_ROP_SPACE];
    DDSCAPS ddsCaps;
    DWORD dwMinOverlayStretch;
    DWORD dwMaxOverlayStretch;
    DWORD dwMinLiveVideoStretch;
    DWORD dwMaxLiveVideoStretch;
    DWORD dwMinHwCodecStretch;
    DWORD dwMaxHwCodecStretch;
    DWORD dwReserved1;
    DWORD dwReserved2;
    DWORD dwReserved3;
    DWORD dwSVBCaps;
    DWORD dwSVBCKeyCaps;
    DWORD dwSVBFXCaps;
    DWORD dwSVBRops[DD_ROP_SPACE];
    DWORD dwVSBCaps;
    DWORD dwVSBCKeyCaps;
    DWORD dwVSBFXCaps;
    DWORD dwVSBRops[DD_ROP_SPACE];
    DWORD dwSSBCaps;
    DWORD dwSSBCKeyCaps;
    DWORD dwSSBFXCaps;
    DWORD dwSSBRops[DD_ROP_SPACE];
    DWORD dwMaxVideoPorts;
    DWORD dwCurrVideoPorts;
    DWORD dwSVBCaps2;
    DWORD dwNLVBCaps;
    DWORD dwNLVBCaps2;
    DWORD dwNLVBCKeyCaps;
    DWORD dwNLVBFXCaps;
    DWORD dwNLVBRops[DD_ROP_SPACE];
} DDCAPS_DX5, *LPDDCAPS_DX5;
typedef DDCAPS_DX5 DDCAPS;
typedef DDCAPS* LPDDCAPS;
// (only ever passed by pointer)
typedef struct _DDBLTFX* LPDDBLTFX;
typedef struct _DDBLTBATCH* LPDDBLTBATCH;
typedef struct _DDOVERLAYFX* LPDDOVERLAYFX;

struct IDirectDraw;
struct IDirectDraw2;
struct IDirectDrawSurface;
struct IDirectDrawSurface2;
struct IDirectDrawSurface3;
struct IDirectDrawPalette;
struct IDirectDrawClipper;
typedef IDirectDraw* LPDIRECTDRAW;
typedef IDirectDraw2* LPDIRECTDRAW2;
typedef IDirectDrawSurface* LPDIRECTDRAWSURFACE;
typedef IDirectDrawSurface2* LPDIRECTDRAWSURFACE2;
typedef IDirectDrawSurface3* LPDIRECTDRAWSURFACE3;
typedef IDirectDrawPalette* LPDIRECTDRAWPALETTE;
typedef IDirectDrawClipper* LPDIRECTDRAWCLIPPER;
typedef BOOL(__stdcall* LPDDENUMCALLBACKA)(GUID*, LPSTR, LPSTR, LPVOID);
typedef HRESULT(__stdcall* LPDDENUMMODESCALLBACK)(LPDDSURFACEDESC, LPVOID);
typedef HRESULT(__stdcall* LPDDENUMSURFACESCALLBACK)(LPDIRECTDRAWSURFACE, LPDDSURFACEDESC, LPVOID);

// ---- Direct3D (d3dtypes.h, d3dcaps.h, d3d.h) -----------------------------------------------------------------------------
typedef float D3DVALUE, *LPD3DVALUE;
typedef DWORD D3DCOLOR, *LPD3DCOLOR;
typedef DWORD D3DCOLORMODEL;
typedef DWORD D3DTEXTUREHANDLE, *LPD3DTEXTUREHANDLE;
typedef DWORD D3DMATERIALHANDLE, *LPD3DMATERIALHANDLE;
typedef DWORD D3DMATRIXHANDLE, *LPD3DMATRIXHANDLE;
#define D3DCOLOR_MONO 1
#define D3DCOLOR_RGB 2
#define D3DENUMRET_CANCEL DDENUMRET_CANCEL
#define D3DENUMRET_OK DDENUMRET_OK
#define D3DCLEAR_TARGET 0x00000001
#define D3DCLEAR_ZBUFFER 0x00000002
#define D3DCLIP_LEFT 0x00000001
#define D3DCLIP_RIGHT 0x00000002
#define D3DCLIP_TOP 0x00000004
#define D3DCLIP_BOTTOM 0x00000008
#define D3DCLIP_FRONT 0x00000010
#define D3DCLIP_BACK 0x00000020
#define D3DTRANSFORM_CLIPPED 0x00000001
#define D3DTRANSFORM_UNCLIPPED 0x00000002
#define D3DDP_WAIT 0x00000001
#define D3DDP_DONOTCLIP 0x00000004
#define D3DDP_DONOTUPDATEEXTENTS 0x00000008
#define D3DDEVCAPS_FLOATTLVERTEX 0x00000001
#define D3DDEVCAPS_TEXTUREVIDEOMEMORY 0x00000200
#define D3DDEVCAPS_DRAWPRIMTLVERTEX 0x00000400
#define D3DDEVCAPS_CANRENDERAFTERFLIP 0x00000800
#define D3DDEVCAPS_TEXTURENONLOCALVIDMEM 0x00001000
#define D3DPTEXTURECAPS_PERSPECTIVE 0x00000001
#define D3DPTEXTURECAPS_POW2 0x00000002
#define D3DPTEXTURECAPS_ALPHA 0x00000004
#define D3DPTEXTURECAPS_TRANSPARENCY 0x00000008
#define D3DTRANSFORMCAPS_CLIP 0x00000001

typedef enum _D3DRENDERSTATETYPE {
    D3DRENDERSTATE_TEXTUREHANDLE = 1,
    D3DRENDERSTATE_ANTIALIAS = 2,
    D3DRENDERSTATE_TEXTUREADDRESS = 3,
    D3DRENDERSTATE_TEXTUREPERSPECTIVE = 4,
    D3DRENDERSTATE_WRAPU = 5,
    D3DRENDERSTATE_WRAPV = 6,
    D3DRENDERSTATE_ZENABLE = 7,
    D3DRENDERSTATE_FILLMODE = 8,
    D3DRENDERSTATE_SHADEMODE = 9,
    D3DRENDERSTATE_LINEPATTERN = 10,
    D3DRENDERSTATE_MONOENABLE = 11,
    D3DRENDERSTATE_ROP2 = 12,
    D3DRENDERSTATE_PLANEMASK = 13,
    D3DRENDERSTATE_ZWRITEENABLE = 14,
    D3DRENDERSTATE_ALPHATESTENABLE = 15,
    D3DRENDERSTATE_LASTPIXEL = 16,
    D3DRENDERSTATE_TEXTUREMAG = 17,
    D3DRENDERSTATE_TEXTUREMIN = 18,
    D3DRENDERSTATE_SRCBLEND = 19,
    D3DRENDERSTATE_DESTBLEND = 20,
    D3DRENDERSTATE_TEXTUREMAPBLEND = 21,
    D3DRENDERSTATE_CULLMODE = 22,
    D3DRENDERSTATE_ZFUNC = 23,
    D3DRENDERSTATE_ALPHAREF = 24,
    D3DRENDERSTATE_ALPHAFUNC = 25,
    D3DRENDERSTATE_DITHERENABLE = 26,
    D3DRENDERSTATE_ALPHABLENDENABLE = 27,
    D3DRENDERSTATE_FOGENABLE = 28,
    D3DRENDERSTATE_SPECULARENABLE = 29,
    D3DRENDERSTATE_ZVISIBLE = 30,
    D3DRENDERSTATE_SUBPIXEL = 31,
    D3DRENDERSTATE_SUBPIXELX = 32,
    D3DRENDERSTATE_STIPPLEDALPHA = 33,
    D3DRENDERSTATE_FOGCOLOR = 34,
    D3DRENDERSTATE_FOGTABLEMODE = 35,
    D3DRENDERSTATE_FOGTABLESTART = 36,
    D3DRENDERSTATE_FOGTABLEEND = 37,
    D3DRENDERSTATE_FOGTABLEDENSITY = 38,
    D3DRENDERSTATE_STIPPLEENABLE = 39,
    D3DRENDERSTATE_EDGEANTIALIAS = 40,
    D3DRENDERSTATE_COLORKEYENABLE = 41,
    D3DRENDERSTATE_BORDERCOLOR = 43,
    D3DRENDERSTATE_TEXTUREADDRESSU = 44,
    D3DRENDERSTATE_TEXTUREADDRESSV = 45,
    D3DRENDERSTATE_MIPMAPLODBIAS = 46,
    D3DRENDERSTATE_ZBIAS = 47,
    D3DRENDERSTATE_RANGEFOGENABLE = 48,
    D3DRENDERSTATE_ANISOTROPY = 49,
    D3DRENDERSTATE_FLUSHBATCH = 50,
    D3DRENDERSTATE_STIPPLEPATTERN00 = 64,
    D3DRENDERSTATE_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;
typedef enum _D3DBLEND {
    D3DBLEND_ZERO = 1,
    D3DBLEND_ONE = 2,
    D3DBLEND_SRCCOLOR = 3,
    D3DBLEND_INVSRCCOLOR = 4,
    D3DBLEND_SRCALPHA = 5,
    D3DBLEND_INVSRCALPHA = 6,
    D3DBLEND_DESTALPHA = 7,
    D3DBLEND_INVDESTALPHA = 8,
    D3DBLEND_DESTCOLOR = 9,
    D3DBLEND_INVDESTCOLOR = 10,
    D3DBLEND_SRCALPHASAT = 11,
    D3DBLEND_BOTHSRCALPHA = 12,
    D3DBLEND_BOTHINVSRCALPHA = 13,
    D3DBLEND_FORCE_DWORD = 0x7fffffff
} D3DBLEND;
typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER = 1,
    D3DCMP_LESS = 2,
    D3DCMP_EQUAL = 3,
    D3DCMP_LESSEQUAL = 4,
    D3DCMP_GREATER = 5,
    D3DCMP_NOTEQUAL = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS = 8,
    D3DCMP_FORCE_DWORD = 0x7fffffff
} D3DCMPFUNC;
typedef enum _D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3, D3DCULL_FORCE_DWORD = 0x7fffffff } D3DCULL;
typedef enum _D3DFILLMODE {
    D3DFILL_POINT = 1,
    D3DFILL_WIREFRAME = 2,
    D3DFILL_SOLID = 3,
    D3DFILL_FORCE_DWORD = 0x7fffffff
} D3DFILLMODE;
typedef enum _D3DSHADEMODE {
    D3DSHADE_FLAT = 1,
    D3DSHADE_GOURAUD = 2,
    D3DSHADE_PHONG = 3,
    D3DSHADE_FORCE_DWORD = 0x7fffffff
} D3DSHADEMODE;
typedef enum _D3DTEXTUREFILTER {
    D3DFILTER_NEAREST = 1,
    D3DFILTER_LINEAR = 2,
    D3DFILTER_MIPNEAREST = 3,
    D3DFILTER_MIPLINEAR = 4,
    D3DFILTER_LINEARMIPNEAREST = 5,
    D3DFILTER_LINEARMIPLINEAR = 6,
    D3DFILTER_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREFILTER;
typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP = 1,
    D3DTADDRESS_MIRROR = 2,
    D3DTADDRESS_CLAMP = 3,
    D3DTADDRESS_BORDER = 4,
    D3DTADDRESS_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREADDRESS;
typedef enum _D3DTEXTUREBLEND {
    D3DTBLEND_DECAL = 1,
    D3DTBLEND_MODULATE = 2,
    D3DTBLEND_DECALALPHA = 3,
    D3DTBLEND_MODULATEALPHA = 4,
    D3DTBLEND_DECALMASK = 5,
    D3DTBLEND_MODULATEMASK = 6,
    D3DTBLEND_COPY = 7,
    D3DTBLEND_ADD = 8,
    D3DTBLEND_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREBLEND;
typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1,
    D3DPT_LINELIST = 2,
    D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN = 6,
    D3DPT_FORCE_DWORD = 0x7fffffff
} D3DPRIMITIVETYPE;
typedef enum _D3DVERTEXTYPE {
    D3DVT_VERTEX = 1,
    D3DVT_LVERTEX = 2,
    D3DVT_TLVERTEX = 3,
    D3DVT_FORCE_DWORD = 0x7fffffff
} D3DVERTEXTYPE;
typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTRANSFORMSTATE_WORLD = 1,
    D3DTRANSFORMSTATE_VIEW = 2,
    D3DTRANSFORMSTATE_PROJECTION = 3,
    D3DTRANSFORMSTATE_FORCE_DWORD = 0x7fffffff
} D3DTRANSFORMSTATETYPE;
typedef enum _D3DLIGHTSTATETYPE {
    D3DLIGHTSTATE_MATERIAL = 1,
    D3DLIGHTSTATE_AMBIENT = 2,
    D3DLIGHTSTATE_COLORMODEL = 3,
    D3DLIGHTSTATE_FOGMODE = 4,
    D3DLIGHTSTATE_FOGSTART = 5,
    D3DLIGHTSTATE_FOGEND = 6,
    D3DLIGHTSTATE_FOGDENSITY = 7,
    D3DLIGHTSTATE_FORCE_DWORD = 0x7fffffff
} D3DLIGHTSTATETYPE;

typedef struct _D3DVECTOR {
    union {
        D3DVALUE x;
        D3DVALUE dvX;
    };
    union {
        D3DVALUE y;
        D3DVALUE dvY;
    };
    union {
        D3DVALUE z;
        D3DVALUE dvZ;
    };
} D3DVECTOR, *LPD3DVECTOR;
typedef struct _D3DCOLORVALUE {
    union {
        D3DVALUE r;
        D3DVALUE dvR;
    };
    union {
        D3DVALUE g;
        D3DVALUE dvG;
    };
    union {
        D3DVALUE b;
        D3DVALUE dvB;
    };
    union {
        D3DVALUE a;
        D3DVALUE dvA;
    };
} D3DCOLORVALUE, *LPD3DCOLORVALUE;
typedef struct _D3DRECT {
    union {
        LONG x1;
        LONG lX1;
    };
    union {
        LONG y1;
        LONG lY1;
    };
    union {
        LONG x2;
        LONG lX2;
    };
    union {
        LONG y2;
        LONG lY2;
    };
} D3DRECT, *LPD3DRECT;
typedef struct _D3DMATRIX {
    D3DVALUE _11, _12, _13, _14;
    D3DVALUE _21, _22, _23, _24;
    D3DVALUE _31, _32, _33, _34;
    D3DVALUE _41, _42, _43, _44;
} D3DMATRIX, *LPD3DMATRIX;
typedef struct _D3DVIEWPORT {
    DWORD dwSize;
    DWORD dwX;
    DWORD dwY;
    DWORD dwWidth;
    DWORD dwHeight;
    D3DVALUE dvScaleX;
    D3DVALUE dvScaleY;
    D3DVALUE dvMaxX;
    D3DVALUE dvMaxY;
    D3DVALUE dvMinZ;
    D3DVALUE dvMaxZ;
} D3DVIEWPORT, *LPD3DVIEWPORT;
typedef struct _D3DVIEWPORT2 {
    DWORD dwSize;
    DWORD dwX;
    DWORD dwY;
    DWORD dwWidth;
    DWORD dwHeight;
    D3DVALUE dvClipX;
    D3DVALUE dvClipY;
    D3DVALUE dvClipWidth;
    D3DVALUE dvClipHeight;
    D3DVALUE dvMinZ;
    D3DVALUE dvMaxZ;
} D3DVIEWPORT2, *LPD3DVIEWPORT2;
typedef struct _D3DMATERIAL {
    DWORD dwSize;
    union {
        D3DCOLORVALUE diffuse;
        D3DCOLORVALUE dcvDiffuse;
    };
    union {
        D3DCOLORVALUE ambient;
        D3DCOLORVALUE dcvAmbient;
    };
    union {
        D3DCOLORVALUE specular;
        D3DCOLORVALUE dcvSpecular;
    };
    union {
        D3DCOLORVALUE emissive;
        D3DCOLORVALUE dcvEmissive;
    };
    union {
        D3DVALUE power;
        D3DVALUE dvPower;
    };
    D3DTEXTUREHANDLE hTexture;
    DWORD dwRampSize;
} D3DMATERIAL, *LPD3DMATERIAL;
typedef struct _D3DTLVERTEX {
    union {
        D3DVALUE sx;
        D3DVALUE dvSX;
    };
    union {
        D3DVALUE sy;
        D3DVALUE dvSY;
    };
    union {
        D3DVALUE sz;
        D3DVALUE dvSZ;
    };
    union {
        D3DVALUE rhw;
        D3DVALUE dvRHW;
    };
    union {
        D3DCOLOR color;
        D3DCOLOR dcColor;
    };
    union {
        D3DCOLOR specular;
        D3DCOLOR dcSpecular;
    };
    union {
        D3DVALUE tu;
        D3DVALUE dvTU;
    };
    union {
        D3DVALUE tv;
        D3DVALUE dvTV;
    };
} D3DTLVERTEX, *LPD3DTLVERTEX;
typedef struct _D3DLVERTEX {
    union {
        D3DVALUE x;
        D3DVALUE dvX;
    };
    union {
        D3DVALUE y;
        D3DVALUE dvY;
    };
    union {
        D3DVALUE z;
        D3DVALUE dvZ;
    };
    DWORD dwReserved;
    union {
        D3DCOLOR color;
        D3DCOLOR dcColor;
    };
    union {
        D3DCOLOR specular;
        D3DCOLOR dcSpecular;
    };
    union {
        D3DVALUE tu;
        D3DVALUE dvTU;
    };
    union {
        D3DVALUE tv;
        D3DVALUE dvTV;
    };
} D3DLVERTEX, *LPD3DLVERTEX;
typedef struct _D3DVERTEX {
    union {
        D3DVALUE x;
        D3DVALUE dvX;
    };
    union {
        D3DVALUE y;
        D3DVALUE dvY;
    };
    union {
        D3DVALUE z;
        D3DVALUE dvZ;
    };
    union {
        D3DVALUE nx;
        D3DVALUE dvNX;
    };
    union {
        D3DVALUE ny;
        D3DVALUE dvNY;
    };
    union {
        D3DVALUE nz;
        D3DVALUE dvNZ;
    };
    union {
        D3DVALUE tu;
        D3DVALUE dvTU;
    };
    union {
        D3DVALUE tv;
        D3DVALUE dvTV;
    };
} D3DVERTEX, *LPD3DVERTEX;
typedef struct _D3DHVERTEX {
    DWORD dwFlags;
    union {
        D3DVALUE hx;
        D3DVALUE dvHX;
    };
    union {
        D3DVALUE hy;
        D3DVALUE dvHY;
    };
    union {
        D3DVALUE hz;
        D3DVALUE dvHZ;
    };
} D3DHVERTEX, *LPD3DHVERTEX;
typedef struct _D3DTRANSFORMDATA {
    DWORD dwSize;
    LPVOID lpIn;
    DWORD dwInSize;
    LPVOID lpOut;
    DWORD dwOutSize;
    LPD3DHVERTEX lpHOut;
    DWORD dwClip;
    DWORD dwClipIntersection;
    DWORD dwClipUnion;
    D3DRECT drExtent;
} D3DTRANSFORMDATA, *LPD3DTRANSFORMDATA;
typedef struct _D3DSTATS {
    DWORD dwSize;
    DWORD dwTrianglesDrawn;
    DWORD dwLinesDrawn;
    DWORD dwPointsDrawn;
    DWORD dwSpansDrawn;
    DWORD dwVerticesProcessed;
} D3DSTATS, *LPD3DSTATS;
typedef struct _D3DTRANSFORMCAPS {
    DWORD dwSize;
    DWORD dwCaps;
} D3DTRANSFORMCAPS, *LPD3DTRANSFORMCAPS;
typedef struct _D3DLIGHTINGCAPS {
    DWORD dwSize;
    DWORD dwCaps;
    DWORD dwLightingModel;
    DWORD dwNumLights;
} D3DLIGHTINGCAPS, *LPD3DLIGHTINGCAPS;
typedef struct _D3DPrimCaps {
    DWORD dwSize;
    DWORD dwMiscCaps;
    DWORD dwRasterCaps;
    DWORD dwZCmpCaps;
    DWORD dwSrcBlendCaps;
    DWORD dwDestBlendCaps;
    DWORD dwAlphaCmpCaps;
    DWORD dwShadeCaps;
    DWORD dwTextureCaps;
    DWORD dwTextureFilterCaps;
    DWORD dwTextureBlendCaps;
    DWORD dwTextureAddressCaps;
    DWORD dwStippleWidth;
    DWORD dwStippleHeight;
} D3DPRIMCAPS, *LPD3DPRIMCAPS;
// DirectX 5's D3DDEVICEDESC (0xcc bytes: through dwMaxStippleHeight), as the Windows SDK has it at DIRECT3D_VERSION 0x0500
typedef struct _D3DDeviceDesc {
    DWORD dwSize;
    DWORD dwFlags;
    D3DCOLORMODEL dcmColorModel;
    DWORD dwDevCaps;
    D3DTRANSFORMCAPS dtcTransformCaps;
    BOOL bClipping;
    D3DLIGHTINGCAPS dlcLightingCaps;
    D3DPRIMCAPS dpcLineCaps;
    D3DPRIMCAPS dpcTriCaps;
    DWORD dwDeviceRenderBitDepth;
    DWORD dwDeviceZBufferBitDepth;
    DWORD dwMaxBufferSize;
    DWORD dwMaxVertexCount;
    DWORD dwMinTextureWidth, dwMinTextureHeight;
    DWORD dwMaxTextureWidth, dwMaxTextureHeight;
    DWORD dwMinStippleWidth, dwMaxStippleWidth;
    DWORD dwMinStippleHeight, dwMaxStippleHeight;
} D3DDEVICEDESC, *LPD3DDEVICEDESC;
// (only ever passed by pointer)
typedef struct _D3DCLIPSTATUS* LPD3DCLIPSTATUS;
typedef struct _D3DLIGHTDATA* LPD3DLIGHTDATA;
typedef struct _D3DFINDDEVICESEARCH* LPD3DFINDDEVICESEARCH;
typedef struct _D3DFINDDEVICERESULT* LPD3DFINDDEVICERESULT;

struct IDirect3D;
struct IDirect3D2;
struct IDirect3DDevice2;
struct IDirect3DLight;
struct IDirect3DMaterial2;
struct IDirect3DTexture2;
struct IDirect3DViewport2;
typedef IDirect3D* LPDIRECT3D;
typedef IDirect3D2* LPDIRECT3D2;
typedef IDirect3DDevice2* LPDIRECT3DDEVICE2;
typedef IDirect3DLight* LPDIRECT3DLIGHT;
typedef IDirect3DMaterial2* LPDIRECT3DMATERIAL2;
typedef IDirect3DTexture2* LPDIRECT3DTEXTURE2;
typedef IDirect3DViewport2* LPDIRECT3DVIEWPORT2;
typedef HRESULT(__stdcall* LPD3DENUMDEVICESCALLBACK)(GUID* lpGuid, LPSTR lpDeviceDescription, LPSTR lpDeviceName,
                                                      LPD3DDEVICEDESC, LPD3DDEVICEDESC, LPVOID);
typedef HRESULT(__stdcall* LPD3DENUMTEXTUREFORMATSCALLBACK)(LPDDSURFACEDESC lpDdsd, LPVOID lpContext);

// ---- DirectSound (dsound.h, DIRECTSOUND_VERSION 0x0500) -------------------------------------------------------------------
#define _FACDS 0x878
#define MAKE_DSHRESULT(code) MAKE_HRESULT(1, _FACDS, code)
#define DS_OK 0
#define DSERR_GENERIC E_FAIL
#define DSERR_INVALIDPARAM E_INVALIDARG
#define DSERR_UNSUPPORTED E_NOTIMPL
#define DSERR_OUTOFMEMORY E_OUTOFMEMORY
#define DSERR_NOINTERFACE E_NOINTERFACE
#define DSERR_ALLOCATED MAKE_DSHRESULT(10)
#define DSERR_CONTROLUNAVAIL MAKE_DSHRESULT(30)
#define DSERR_INVALIDCALL MAKE_DSHRESULT(50)
#define DSERR_PRIOLEVELNEEDED MAKE_DSHRESULT(70)
#define DSERR_BADFORMAT MAKE_DSHRESULT(100)
#define DSERR_NODRIVER MAKE_DSHRESULT(120)
#define DSERR_ALREADYINITIALIZED MAKE_DSHRESULT(130)
#define DSERR_BUFFERLOST MAKE_DSHRESULT(150)
#define DSERR_OTHERAPPHASPRIO MAKE_DSHRESULT(160)
#define DSERR_UNINITIALIZED MAKE_DSHRESULT(170)
#define DSCAPS_PRIMARYMONO 0x00000001
#define DSCAPS_PRIMARYSTEREO 0x00000002
#define DSCAPS_PRIMARY8BIT 0x00000004
#define DSCAPS_PRIMARY16BIT 0x00000008
#define DSCAPS_CONTINUOUSRATE 0x00000010
#define DSCAPS_EMULDRIVER 0x00000020
#define DSCAPS_CERTIFIED 0x00000040
#define DSCAPS_SECONDARYMONO 0x00000100
#define DSCAPS_SECONDARYSTEREO 0x00000200
#define DSCAPS_SECONDARY8BIT 0x00000400
#define DSCAPS_SECONDARY16BIT 0x00000800
#define DSBCAPS_PRIMARYBUFFER 0x00000001
#define DSBCAPS_STATIC 0x00000002
#define DSBCAPS_LOCHARDWARE 0x00000004
#define DSBCAPS_LOCSOFTWARE 0x00000008
#define DSBCAPS_CTRL3D 0x00000010
#define DSBCAPS_CTRLFREQUENCY 0x00000020
#define DSBCAPS_CTRLPAN 0x00000040
#define DSBCAPS_CTRLVOLUME 0x00000080
#define DSBCAPS_CTRLPOSITIONNOTIFY 0x00000100
#define DSBCAPS_STICKYFOCUS 0x00004000
#define DSBCAPS_GLOBALFOCUS 0x00008000
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000
#define DSBCAPS_MUTE3DATMAXDISTANCE 0x00020000
#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_BUFFERLOST 0x00000002
#define DSBSTATUS_LOOPING 0x00000004
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER 0x00000002
#define DSSCL_NORMAL 1
#define DSSCL_PRIORITY 2
#define DSSCL_EXCLUSIVE 3
#define DSSCL_WRITEPRIMARY 4
#define DSBVOLUME_MIN -10000
#define DSBVOLUME_MAX 0
#define DSBPAN_LEFT -10000
#define DSBPAN_CENTER 0
#define DSBPAN_RIGHT 10000

typedef struct _DSCAPS {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwMinSecondarySampleRate;
    DWORD dwMaxSecondarySampleRate;
    DWORD dwPrimaryBuffers;
    DWORD dwMaxHwMixingAllBuffers;
    DWORD dwMaxHwMixingStaticBuffers;
    DWORD dwMaxHwMixingStreamingBuffers;
    DWORD dwFreeHwMixingAllBuffers;
    DWORD dwFreeHwMixingStaticBuffers;
    DWORD dwFreeHwMixingStreamingBuffers;
    DWORD dwMaxHw3DAllBuffers;
    DWORD dwMaxHw3DStaticBuffers;
    DWORD dwMaxHw3DStreamingBuffers;
    DWORD dwFreeHw3DAllBuffers;
    DWORD dwFreeHw3DStaticBuffers;
    DWORD dwFreeHw3DStreamingBuffers;
    DWORD dwTotalHwMemBytes;
    DWORD dwFreeHwMemBytes;
    DWORD dwMaxContigFreeHwMemBytes;
    DWORD dwUnlockTransferRateHwBuffers;
    DWORD dwPlayCpuOverheadSwBuffers;
    DWORD dwReserved1;
    DWORD dwReserved2;
} DSCAPS, *LPDSCAPS;
typedef const DSCAPS* LPCDSCAPS;
typedef struct _DSBCAPS {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwUnlockTransferRate;
    DWORD dwPlayCpuOverhead;
} DSBCAPS, *LPDSBCAPS;
typedef const DSBCAPS* LPCDSBCAPS;
typedef struct _DSBUFFERDESC {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwBufferBytes;
    DWORD dwReserved;
    LPWAVEFORMATEX lpwfxFormat;
} DSBUFFERDESC, *LPDSBUFFERDESC;
typedef const DSBUFFERDESC* LPCDSBUFFERDESC;
typedef struct _DS3DBUFFER {
    DWORD dwSize;
    D3DVECTOR vPosition;
    D3DVECTOR vVelocity;
    DWORD dwInsideConeAngle;
    DWORD dwOutsideConeAngle;
    D3DVECTOR vConeOrientation;
    LONG lConeOutsideVolume;
    D3DVALUE flMinDistance;
    D3DVALUE flMaxDistance;
    DWORD dwMode;
} DS3DBUFFER, *LPDS3DBUFFER;
typedef const DS3DBUFFER* LPCDS3DBUFFER;
typedef struct _DS3DLISTENER {
    DWORD dwSize;
    D3DVECTOR vPosition;
    D3DVECTOR vVelocity;
    D3DVECTOR vOrientFront;
    D3DVECTOR vOrientTop;
    D3DVALUE flDistanceFactor;
    D3DVALUE flRolloffFactor;
    D3DVALUE flDopplerFactor;
} DS3DLISTENER, *LPDS3DLISTENER;
typedef const DS3DLISTENER* LPCDS3DLISTENER;

struct IDirectSound;
struct IDirectSoundBuffer;
struct IDirectSound3DBuffer;
struct IDirectSound3DListener;
struct IKsPropertySet;
typedef IDirectSound* LPDIRECTSOUND;
typedef IDirectSoundBuffer* LPDIRECTSOUNDBUFFER;
typedef IDirectSound3DBuffer* LPDIRECTSOUND3DBUFFER;
typedef IDirectSound3DListener* LPDIRECTSOUND3DLISTENER;
typedef IKsPropertySet* LPKSPROPERTYSET;

// ---- the interfaces: each method of the SDK's, in vtable order (from com_base.h / com_dsound.h) ---------------------------
struct IDirectDraw : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compact() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER FAR*, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePalette(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE FAR*, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC, LPDIRECTDRAWSURFACE FAR *, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID,LPDDENUMSURFACESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE FlipToGDISurface() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS, LPDDCAPS) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD, LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(GUID FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestoreDisplayMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD, DWORD,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) = 0;
};
struct IDirectDraw2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compact() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateClipper(DWORD, LPDIRECTDRAWCLIPPER FAR*, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePalette(DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE FAR*, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSurface(LPDDSURFACEDESC, LPDIRECTDRAWSURFACE FAR *, IUnknown FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMMODESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID,LPDDENUMSURFACESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE FlipToGDISurface() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(LPDDCAPS, LPDDCAPS) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDisplayMode(LPDDSURFACEDESC) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFourCCCodes(LPDWORD, LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGDISurface(LPDIRECTDRAWSURFACE FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMonitorFrequency(LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetScanLine(LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVerticalBlankStatus(LPBOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(GUID FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE RestoreDisplayMode() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDisplayMode(DWORD, DWORD,DWORD, DWORD, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE WaitForVerticalBlank(DWORD, HANDLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAvailableVidMem(LPDDSCAPS, LPDWORD, LPDWORD) = 0;
};
struct IDirectDrawSurface3 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE AddAttachedSurface(LPDIRECTDRAWSURFACE3) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddOverlayDirtyRect(LPRECT) = 0;
    virtual HRESULT STDMETHODCALLTYPE Blt(LPRECT,LPDIRECTDRAWSURFACE3, LPRECT,DWORD, LPDDBLTFX) = 0;
    virtual HRESULT STDMETHODCALLTYPE BltBatch(LPDDBLTBATCH, DWORD, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE BltFast(DWORD,DWORD,LPDIRECTDRAWSURFACE3, LPRECT,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteAttachedSurface(DWORD,LPDIRECTDRAWSURFACE3) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumAttachedSurfaces(LPVOID,LPDDENUMSURFACESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumOverlayZOrders(DWORD,LPVOID,LPDDENUMSURFACESCALLBACK) = 0;
    virtual HRESULT STDMETHODCALLTYPE Flip(LPDIRECTDRAWSURFACE3, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAttachedSurface(LPDDSCAPS, LPDIRECTDRAWSURFACE3 FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBltStatus(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(LPDDSCAPS) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClipper(LPDIRECTDRAWCLIPPER FAR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetColorKey(DWORD, LPDDCOLORKEY) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDC(HDC FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFlipStatus(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOverlayPosition(LPLONG, LPLONG) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPalette(LPDIRECTDRAWPALETTE FAR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPixelFormat(LPDDPIXELFORMAT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSurfaceDesc(LPDDSURFACEDESC) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLost() = 0;
    virtual HRESULT STDMETHODCALLTYPE Lock(LPRECT,LPDDSURFACEDESC,DWORD,HANDLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE ReleaseDC(HDC) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restore() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClipper(LPDIRECTDRAWCLIPPER) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetColorKey(DWORD, LPDDCOLORKEY) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOverlayPosition(LONG, LONG) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPalette(LPDIRECTDRAWPALETTE) = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock(LPVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE3,LPRECT,DWORD, LPDDOVERLAYFX) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateOverlayDisplay(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE3) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDDInterface(LPVOID FAR *) = 0;
    virtual HRESULT STDMETHODCALLTYPE PageLock(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE PageUnlock(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSurfaceDesc(LPDDSURFACEDESC, DWORD) = 0;
};
struct IDirect3D2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumDevices(LPD3DENUMDEVICESCALLBACK,LPVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateLight(LPDIRECT3DLIGHT*,IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateMaterial(LPDIRECT3DMATERIAL2*,IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateViewport(LPDIRECT3DVIEWPORT2*,IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE FindDevice(LPD3DFINDDEVICESEARCH,LPD3DFINDDEVICERESULT) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID,LPDIRECTDRAWSURFACE,LPDIRECT3DDEVICE2*) = 0;
};
struct IDirect3DDevice2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCaps(LPD3DDEVICEDESC,LPD3DDEVICEDESC) = 0;
    virtual HRESULT STDMETHODCALLTYPE SwapTextureHandles(LPDIRECT3DTEXTURE2,LPDIRECT3DTEXTURE2) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStats(LPD3DSTATS) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddViewport(LPDIRECT3DVIEWPORT2) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteViewport(LPDIRECT3DVIEWPORT2) = 0;
    virtual HRESULT STDMETHODCALLTYPE NextViewport(LPDIRECT3DVIEWPORT2,LPDIRECT3DVIEWPORT2*,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumTextureFormats(LPD3DENUMTEXTUREFORMATSCALLBACK,LPVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginScene() = 0;
    virtual HRESULT STDMETHODCALLTYPE EndScene() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDirect3D(LPDIRECT3D2*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentViewport(LPDIRECT3DVIEWPORT2) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentViewport(LPDIRECT3DVIEWPORT2 *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRenderTarget(LPDIRECTDRAWSURFACE,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRenderTarget(LPDIRECTDRAWSURFACE *) = 0;
    virtual HRESULT STDMETHODCALLTYPE Begin(D3DPRIMITIVETYPE,D3DVERTEXTYPE,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE BeginIndexed(D3DPRIMITIVETYPE,D3DVERTEXTYPE,LPVOID,DWORD,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Vertex(LPVOID) = 0;
    virtual HRESULT STDMETHODCALLTYPE Index(WORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE End(DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE,LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetLightState(D3DLIGHTSTATETYPE,LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetLightState(D3DLIGHTSTATETYPE,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE,LPD3DMATRIX) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE,LPD3DMATRIX) = 0;
    virtual HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE,LPD3DMATRIX) = 0;
    virtual HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE,D3DVERTEXTYPE,LPVOID,DWORD,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE,D3DVERTEXTYPE,LPVOID,DWORD,LPWORD,DWORD,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetClipStatus(LPD3DCLIPSTATUS) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClipStatus(LPD3DCLIPSTATUS) = 0;
};
struct IDirect3DViewport2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPDIRECT3D) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewport(LPD3DVIEWPORT) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetViewport(LPD3DVIEWPORT) = 0;
    virtual HRESULT STDMETHODCALLTYPE TransformVertices(DWORD,LPD3DTRANSFORMDATA,DWORD,LPDWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE LightElements(DWORD,LPD3DLIGHTDATA) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBackground(D3DMATERIALHANDLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBackground(LPD3DMATERIALHANDLE,LPBOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBackgroundDepth(LPDIRECTDRAWSURFACE) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetBackgroundDepth(LPDIRECTDRAWSURFACE*,LPBOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE Clear(DWORD,LPD3DRECT,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddLight(LPDIRECT3DLIGHT) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteLight(LPDIRECT3DLIGHT) = 0;
    virtual HRESULT STDMETHODCALLTYPE NextLight(LPDIRECT3DLIGHT,LPDIRECT3DLIGHT*,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetViewport2(LPD3DVIEWPORT2) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetViewport2(LPD3DVIEWPORT2) = 0;
};
struct IDirect3DMaterial2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetMaterial(LPD3DMATERIAL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaterial(LPD3DMATERIAL) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2,LPD3DMATERIALHANDLE) = 0;
};
struct IDirect3DTexture2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetHandle(LPDIRECT3DDEVICE2,LPD3DTEXTUREHANDLE) = 0;
    virtual HRESULT STDMETHODCALLTYPE PaletteChanged(DWORD,DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE Load(LPDIRECT3DTEXTURE2) = 0;
};
struct IDirectSound : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateSoundBuffer(_In_ LPCDSBUFFERDESC pcDSBufferDesc, _Outptr_ LPDIRECTSOUNDBUFFER *ppDSBuffer, _Pre_null_ LPUNKNOWN pUnkOuter) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCaps(_Out_ LPDSCAPS pDSCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer(_In_ LPDIRECTSOUNDBUFFER pDSBufferOriginal, _Outptr_ LPDIRECTSOUNDBUFFER *ppDSBufferDuplicate) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND hwnd, DWORD dwLevel) = 0;
    virtual HRESULT STDMETHODCALLTYPE Compact() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSpeakerConfig(_Out_ LPDWORD pdwSpeakerConfig) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetSpeakerConfig(DWORD dwSpeakerConfig) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(_In_opt_ LPCGUID pcGuidDevice) = 0;
};
struct IDirectSoundBuffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetCaps(_Out_ LPDSBCAPS pDSBufferCaps) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPosition(_Out_opt_ LPDWORD pdwCurrentPlayCursor, _Out_opt_ LPDWORD pdwCurrentWriteCursor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFormat(_Out_writes_bytes_opt_(dwSizeAllocated) LPWAVEFORMATEX pwfxFormat, DWORD dwSizeAllocated, _Out_opt_ LPDWORD pdwSizeWritten) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVolume(_Out_ LPLONG plVolume) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPan(_Out_ LPLONG plPan) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFrequency(_Out_ LPDWORD pdwFrequency) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetStatus(_Out_ LPDWORD pdwStatus) = 0;
    virtual HRESULT STDMETHODCALLTYPE Initialize(_In_ LPDIRECTSOUND pDirectSound, _In_ LPCDSBUFFERDESC pcDSBufferDesc) = 0;
    virtual HRESULT STDMETHODCALLTYPE Lock(DWORD dwOffset, DWORD dwBytes, _Outptr_result_bytebuffer_(*pdwAudioBytes1) LPVOID *ppvAudioPtr1, _Out_ LPDWORD pdwAudioBytes1, _Outptr_opt_result_bytebuffer_(*pdwAudioBytes2) LPVOID *ppvAudioPtr2, _Out_opt_ LPDWORD pdwAudioBytes2, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE Play(DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentPosition(DWORD dwNewPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFormat(_In_ LPCWAVEFORMATEX pcfxFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVolume(LONG lVolume) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPan(LONG lPan) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFrequency(DWORD dwFrequency) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Unlock(_In_reads_bytes_(dwAudioBytes1) LPVOID pvAudioPtr1, DWORD dwAudioBytes1, _In_reads_bytes_opt_(dwAudioBytes2) LPVOID pvAudioPtr2, DWORD dwAudioBytes2) = 0;
    virtual HRESULT STDMETHODCALLTYPE Restore() = 0;
};
struct IDirectSound3DBuffer : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAllParameters(_Out_ LPDS3DBUFFER pDs3dBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConeAngles(_Out_ LPDWORD pdwInsideConeAngle, _Out_ LPDWORD pdwOutsideConeAngle) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConeOrientation(_Out_ D3DVECTOR* pvOrientation) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConeOutsideVolume(_Out_ LPLONG plConeOutsideVolume) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMaxDistance(_Out_ D3DVALUE* pflMaxDistance) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMinDistance(_Out_ D3DVALUE* pflMinDistance) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMode(_Out_ LPDWORD pdwMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(_Out_ D3DVECTOR* pvPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVelocity(_Out_ D3DVECTOR* pvVelocity) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllParameters(_In_ LPCDS3DBUFFER pcDs3dBuffer, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetConeAngles(DWORD dwInsideConeAngle, DWORD dwOutsideConeAngle, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetConeOrientation(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetConeOutsideVolume(LONG lConeOutsideVolume, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMaxDistance(D3DVALUE flMaxDistance, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMinDistance(D3DVALUE flMinDistance, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMode(DWORD dwMode, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) = 0;
};
struct IDirectSound3DListener : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAllParameters(_Out_ LPDS3DLISTENER pListener) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDistanceFactor(_Out_ D3DVALUE* pflDistanceFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDopplerFactor(_Out_ D3DVALUE* pflDopplerFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetOrientation(_Out_ D3DVECTOR* pvOrientFront, _Out_ D3DVECTOR* pvOrientTop) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPosition(_Out_ D3DVECTOR* pvPosition) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRolloffFactor(_Out_ D3DVALUE* pflRolloffFactor) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVelocity(_Out_ D3DVECTOR* pvVelocity) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllParameters(_In_ LPCDS3DLISTENER pcListener, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDistanceFactor(D3DVALUE flDistanceFactor, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDopplerFactor(D3DVALUE flDopplerFactor, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetOrientation(D3DVALUE xFront, D3DVALUE yFront, D3DVALUE zFront, D3DVALUE xTop, D3DVALUE yTop, D3DVALUE zTop, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPosition(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetRolloffFactor(D3DVALUE flRolloffFactor, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetVelocity(D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD dwApply) = 0;
    virtual HRESULT STDMETHODCALLTYPE CommitDeferredSettings() = 0;
};
struct IKsPropertySet : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Get(_In_ REFGUID rguidPropSet, ULONG ulId, _In_reads_bytes_opt_(ulInstanceLength) LPVOID pInstanceData, ULONG ulInstanceLength, _Out_writes_bytes_(ulDataLength) LPVOID pPropertyData, ULONG ulDataLength, _Out_opt_ PULONG pulBytesReturned) = 0;
    virtual HRESULT STDMETHODCALLTYPE Set(_In_ REFGUID rguidPropSet, ULONG ulId, _In_reads_bytes_opt_(ulInstanceLength) LPVOID pInstanceData, ULONG ulInstanceLength, _In_reads_bytes_(ulDataLength) LPVOID pPropertyData, ULONG ulDataLength) = 0;
    virtual HRESULT STDMETHODCALLTYPE QuerySupport(_In_ REFGUID rguidPropSet, ULONG ulId, _Out_ PULONG pulTypeSupport) = 0;
};

// ---- the interface ids the port compares (ddraw.h, d3d.h, unknwn.h) ---------------------------------------------------------
static const IID IID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID IID_IDirectDraw = {0x6C14DB80, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const IID IID_IDirectDraw2 = {0xB3A6F3E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
static const IID IID_IDirectDrawSurface = {0x6C14DB81, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const IID IID_IDirectDrawSurface2 = {0x57805885, 0x6eec, 0x11cf, {0x94, 0x41, 0xa8, 0x23, 0x03, 0xc1, 0x0e, 0x27}};
static const IID IID_IDirectDrawSurface3 = {0xDA044E00, 0x69B2, 0x11D0, {0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB}};
static const IID IID_IDirect3D2 = {0x6aae1ec1, 0x662a, 0x11d0, {0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a}};
static const IID IID_IDirect3DTexture2 = {0x93281502, 0x8CF8, 0x11D0, {0x89, 0xAB, 0x00, 0xA0, 0xC9, 0x05, 0x41, 0x29}};

// ---- layouts (DirectX 5's 32-bit sizes) ----------------------------------------------------------------------------------------
static_assert(sizeof(DDPIXELFORMAT) == 0x20 && offsetof(DDPIXELFORMAT, dwRBitMask) == 0x10, "DDPIXELFORMAT");
static_assert(sizeof(DDSURFACEDESC) == 0x6c && offsetof(DDSURFACEDESC, lPitch) == 0x10 &&
                  offsetof(DDSURFACEDESC, lpSurface) == 0x24 && offsetof(DDSURFACEDESC, ddckCKSrcBlt) == 0x40 &&
                  offsetof(DDSURFACEDESC, ddpfPixelFormat) == 0x48 && offsetof(DDSURFACEDESC, ddsCaps) == 0x68,
              "DDSURFACEDESC");
static_assert(sizeof(DDCAPS) == 0x16c && offsetof(DDCAPS, ddsCaps) == 0x84, "DDCAPS (DX5)");
static_assert(sizeof(D3DPRIMCAPS) == 56 && sizeof(D3DLIGHTINGCAPS) == 16 && sizeof(D3DTRANSFORMCAPS) == 8, "D3D caps");
static_assert(sizeof(D3DDEVICEDESC) == 0xcc && offsetof(D3DDEVICEDESC, dpcTriCaps) == 0x64 &&
                  offsetof(D3DDEVICEDESC, dwMaxVertexCount) == 0xa8, "D3DDEVICEDESC (DX5)");
static_assert(sizeof(D3DMATERIAL) == 0x50 && offsetof(D3DMATERIAL, hTexture) == 0x48, "D3DMATERIAL");
static_assert(sizeof(D3DVIEWPORT2) == 44 && sizeof(D3DVIEWPORT) == 44, "D3DVIEWPORT2");
static_assert(sizeof(D3DTLVERTEX) == 32 && sizeof(D3DLVERTEX) == 32 && sizeof(D3DVERTEX) == 32 && sizeof(D3DHVERTEX) == 16,
              "vertices");
static_assert(sizeof(D3DTRANSFORMDATA) == 52 && sizeof(D3DMATRIX) == 64 && sizeof(D3DRECT) == 16, "D3DTRANSFORMDATA");
static_assert(sizeof(D3DSTATS) == 24, "D3DSTATS");
static_assert(sizeof(DSCAPS) == 0x60 && sizeof(DSBCAPS) == 20 && sizeof(DSBUFFERDESC) == 0x14, "DirectSound 5");
static_assert(sizeof(DS3DBUFFER) == 64 && sizeof(DS3DLISTENER) == 64, "DirectSound 3D");
static_assert(sizeof(D3DRENDERSTATETYPE) == 4 && sizeof(D3DPRIMITIVETYPE) == 4, "the enums are DWORDs");
