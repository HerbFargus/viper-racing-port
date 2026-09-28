// paint_tga.cpp -- M3 UI stage, step U4 (group C): the paint kit's TGA files, rewritten faithfully (library `paintkit`,
// tga.obj: WriteTGA, ReadTGA, compare_headers). The paint kit's export and import ("<user>paint\paint.tga"): a 24-bit
// uncompressed image, bottom row first, with a TGA 2.0 footer, from and into a 32-bit canvas.
//
// Written from the v1.0 disassembly, as paint_kit.cpp; the frames are the original's layout. Both walk the canvas's pixels
// as one run of w * h dwords (the pitch unused: the paint kit's canvases are 256 x 256, pitch 1024).
//
// Footprints: WriteTGA and ReadTGA create / open, write / read and close a file and allocate a buffer -- replay_only (the
// session replay: export and import in the paint kit); compare_headers writes nothing (a mismatch is logged).
//
// FIX CANDIDATEs (left faithful, marked in place): neither checks MemAlloc's buffer (out of memory: a write to address
// 0 onwards); both treat the canvas as one run of pixels (a canvas whose pitch isn't w * 4 -- none in the paint kit).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "ui_types.h"
#include "paint_kit.h"

namespace {
namespace paint_tga {
using namespace pkit;
using uit::tcall; using uit::ccall; using uit::crt_strcpy; using uit::Log_t;

// WriteTGA: the canvas as a 24-bit TGA ("TRUEVISION-XFILE." footer); false if any write failed (or the file can't be made)
static uint8_t __cdecl WriteTGA_n(gxCanvas* c, const char* name) {
    struct {
        volatile int32_t fd;                      // F+0x10
        uint8_t* volatile buf;                    // F+0x14
        TGAHeader hdr;                            // F+0x18
        uint8_t _2a[2];
        uint8_t footer[0x1c];                     // F+0x2c
        TGAHeader blank;                          // F+0x48 (zeroed, never used)
        uint8_t _5a[2];
    } fr;
    static_assert(offsetof(decltype(fr), footer) == 0x2c - 0x10 && offsetof(decltype(fr), blank) == 0x48 - 0x10, "WriteTGA's frame");
    uint8_t ok = 0;
    const int32_t w = c->w;
    int32_t h = c->h;
    {
        volatile uint32_t* z = (volatile uint32_t*)&fr.blank;
        z[0] = 0; z[1] = 0; z[2] = 0; z[3] = 0;
        *(volatile uint16_t*)&z[4] = 0;
    }
    fr.fd = ccall<int32_t>(F_FileCreate, name);
    if (fr.fd == 0) {
        UI_LogReport(PK_CP(0x00500f30), name);                                              // "Can't create %s"
        return ok;
    }
    // FIX CANDIDATE: the buffer isn't checked (out of memory: the rows are written from address 0 on)
    fr.buf = ccall<uint8_t*>(F_MemAlloc, imul(imul(h, w), 3));
    fr.hdr.id_length = 0;
    fr.hdr.color_map_type = 0;
    fr.hdr.image_type = 2;
    *(volatile uint32_t*)&fr.hdr.cmap[0] = 0;
    fr.hdr.cmap[4] = 0;
    fr.hdr.pixel_depth = 0x18;
    fr.hdr.descriptor = 8;
    fr.hdr.x_origin = 0;
    fr.hdr.y_origin = 0;
    fr.hdr.width = (uint16_t)w;
    fr.hdr.height = (uint16_t)h;
    ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)&fr.hdr, (int32_t)0x12);
    {
        volatile uint8_t* d = fr.buf;
        const volatile uint32_t* s = (const volatile uint32_t*)c->pixels;
        for (int32_t r = h; r > 0; r--)
            for (int32_t k = w; k > 0; k--) {
                const uint32_t p = *s++;
                d += 3;
                d[-1] = (uint8_t)(p >> 16);
                d[-2] = (uint8_t)(s[-1] >> 8);
                d[-3] = (uint8_t)s[-1];
            }
    }
    ok = 1;
    h = isub(h, 1);
    if (h >= 0) {
        const int32_t rowb = imul(w, 3);
        const uint8_t* row = fr.buf + imul(imul(h, w), 3);
        do {
            const uint8_t* p = row;
            row -= rowb;
            ok = (uint8_t)(ok & ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)p, rowb));
            h = isub(h, 1);
        } while (h >= 0);
    }
    if (ok == 0) UI_LogReport(PK_CP(0x00500f00));                                         // "Error writing color table"
    {
        volatile uint32_t* z = (volatile uint32_t*)fr.footer;
        for (int i = 0; i < 7; i++) z[i] = 0;
    }
    crt_strcpy((char*)fr.footer + 8, PK_CP(0x00500f1c));                                  // "TRUEVISION-XFILE."
    ccall<uint8_t>(F_FileWrite, (int32_t)fr.fd, (const void*)fr.footer, (int32_t)0x1c);
    ccall<void>(F_FileClose, (int32_t*)&fr.fd);
    ccall<void>(F_Delete, (void*)fr.buf);
    return ok;
}
static void fp_tga(Footprint& f, gxCanvas*, const char*) { f.replay_only = "a file written or read, a buffer allocated and freed"; }
PORT_FN(0x004cbf60, "WriteTGA", WriteTGA_n, fp_tga)

// ReadTGA: a 24-bit TGA of the canvas's size into it (opaque); false if it can't be opened, its header doesn't match
// (compare_headers) or a row can't be read
static uint8_t __cdecl ReadTGA_n(gxCanvas* c, const char* name) {
    struct {
        volatile uint8_t _10[3];
        volatile uint8_t ok;                      // F+0x13
        volatile int32_t fd;                      // F+0x14
        uint8_t* volatile buf;                    // F+0x18
        TGAHeader want;                           // F+0x1c
        uint8_t _2e[2];
        TGAHeader got;                            // F+0x30
        uint8_t _42[2];
    } fr;
    static_assert(offsetof(decltype(fr), want) == 0x1c - 0x10 && offsetof(decltype(fr), got) == 0x30 - 0x10, "ReadTGA's frame");
    fr.ok = 0;
    fr.fd = ccall<int32_t>(F_FileOpen, name);
    if (fr.fd == 0) {
        UI_LogReport(PK_CP(0x00500f9c), name);                                              // "Can't open %s"
        return fr.ok;
    }
    {
        volatile uint32_t* z = (volatile uint32_t*)&fr.want;
        z[0] = 0;
        z[1] = 0;
        z[2] = 0;
    }
    int32_t w = c->w;
    int32_t h = c->h;
    *(volatile uint32_t*)((uint8_t*)&fr.want + 0xc) = 0;
    *(volatile uint16_t*)((uint8_t*)&fr.want + 0x10) = 0;
    fr.want.color_map_type = 0;
    fr.want.image_type = 2;
    fr.want.width = (uint16_t)w;
    fr.want.height = (uint16_t)h;
    fr.want.pixel_depth = 0x18;
    fr.want.descriptor = 8;
    if (!ccall<uint8_t>(F_FileReadExact, (int32_t)fr.fd, (void*)&fr.got, (int32_t)0x12)) {
        UI_LogReport(PK_CP(0x00500f84));                                                    // "Can't read TGA header"
    } else if (!ccall<uint8_t>(F_compare_headers, (const void*)&fr.got, (const void*)&fr.want)) {
        UI_LogReport(PK_CP(0x00500f68));                                                    // "Bad TGA header or format"
    } else {
        // FIX CANDIDATE: the buffer isn't checked (out of memory: the rows are read to address 0 on)
        fr.buf = ccall<uint8_t*>(F_MemAlloc, imul(imul(c->w, c->h), 3));
        h = isub(h, 1);
        fr.ok = 1;
        if (h >= 0) {
            const int32_t rowb = imul(w, 3);
            uint8_t* row = fr.buf + imul(imul(w, h), 3);
            do {
                uint8_t* p = row;
                row -= rowb;
                fr.ok = (uint8_t)(fr.ok & ccall<uint8_t>(F_FileReadExact, (int32_t)fr.fd, (void*)p, rowb));
                h = isub(h, 1);
            } while (h >= 0);
        }
        if (fr.ok != 0) {
            volatile uint32_t* d = (volatile uint32_t*)c->pixels;
            const volatile uint8_t* s = fr.buf;
            for (int32_t r = 0; c->h > r; r++)
                for (int32_t k = 0; c->w > k;) {
                    uint32_t g = s[1];
                    const uint32_t rr = (uint32_t)s[2] << 16;
                    g = (g | 0xffff0000u) << 8;
                    d++;
                    g |= rr;
                    s += 3;
                    k++;
                    g |= s[-3];
                    d[-1] = g;
                }
            fr.ok = 1;
        } else {
            UI_LogReport(PK_CP(0x00500f4c));                                                // "Can't read TGA image bits"
        }
        ccall<void>(F_Delete, (void*)fr.buf);
    }
    ccall<void>(F_FileClose, (int32_t*)&fr.fd);
    return fr.ok;
}
PORT_FN(0x004cc100, "ReadTGA", ReadTGA_n, fp_tga)

// compare_headers: the width, height, depth, colour map type, id length and origin the same (each mismatch logged:
// "TGA: bad <what> (<read> != <wanted>)"); the image type isn't compared
typedef void(__cdecl* Log3_t)(const char*, const char*, uint32_t, uint32_t);
#define LOG3 ((Log3_t)(uintptr_t)F_LogReport)
static uint8_t __cdecl compare_headers_n(const TGAHeader* a, const TGAHeader* b) {
    {
        const uint16_t x = a->width, y = b->width;
        if (x != y) {
            LOG3(PK_CP(0x00500fb4), PK_CP(0x00500fac), x, y);                             // "width"
            return 0;
        }
    }
    {
        const uint16_t y = b->height, x = a->height;
        if (y != x) {
            LOG3(PK_CP(0x00500fd4), PK_CP(0x00500fcc), x, y);                             // "height"
            return 0;
        }
    }
    {
        const uint8_t y = b->pixel_depth, x = a->pixel_depth;
        if (y != x) {
            LOG3(PK_CP(0x00500ff8), PK_CP(0x00500fec), x, y);                             // "pixel_depth"
            return 0;
        }
    }
    {
        const uint8_t y = b->color_map_type, x = a->color_map_type;
        if (y != x) {
            LOG3(PK_CP(0x00501020), PK_CP(0x00501010), x, y);                             // "color_map_type"
            return 0;
        }
    }
    {
        const uint8_t y = b->id_length, x = a->id_length;
        if (y != x) {
            LOG3(PK_CP(0x00501044), PK_CP(0x00501038), x, y);                             // "id_length"
            return 0;
        }
    }
    {
        const uint16_t y = b->x_origin, x = a->x_origin;
        if (y != x) {
            LOG3(PK_CP(0x00501068), PK_CP(0x0050105c), x, y);                             // "x_origin"
            return 0;
        }
    }
    {
        const uint16_t y = b->y_origin, x = a->y_origin;
        if (y != x) {
            LOG3(PK_CP(0x0050108c), PK_CP(0x00501080), x, y);                             // "y_origin"
            return 0;
        }
    }
    return 1;
}
static void fp_compare_headers(Footprint&, const TGAHeader*, const TGAHeader*) {}
PORT_FN(0x004cc2a0, "compare_headers", compare_headers_n, fp_compare_headers)

#undef LOG3
}  // namespace paint_tga
}  // namespace
