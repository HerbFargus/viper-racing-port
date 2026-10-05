// edit_3ds.cpp -- M3 UI stage, step U5 (group B): the model tool's 3D Studio files, rewritten faithfully (library `edit`:
// cvt3ds.obj -- Read3DS, Write3DS and every chunk parser; adtools.obj -- the chunk reader / writer ADOpen ... ADWrite and
// ADParseChunks). Layouts and addresses: edit_build.h.
//
// The model tool's 3DS import (ModBuilderImport3DS, edit_build.cpp) reads a .3ds through Read3DS: ADParseChunks walks the
// chunk tree with a table of parsers per level (primary 4d4d > editor 3d3d > objects 4000 > triangle mesh 4100 > vertices
// 4110 / faces 4120 / mapping 4140 / local axes 4160; materials afff > texture map a200 > a300 its name), filling an
// mrModelInfo whose arrays the caller made (one surface per object; the materials' texture names land on the surfaces by
// their order). Its export writes the model's surfaces as materials "NewMat #n" (with the surface's texture as a .TGA map)
// and objects "Surfnn" (vertices in inches, x mirrored; mapping v flipped; faces wound back), then a keyframer section.
// The reader keeps a debug log, out.txt in the current directory (ADOpen), as the original does.
//
// Written from the v1.0 disassembly: every call in the original's order by its v1.0 address (this group's own too, so a
// hooked rewrite is what runs), the C runtime's stdio by its v1.0 address. The parsers' tables are built field by field
// as the original builds them; each parser receives the chunk id as the dword ADParseChunks pushes (the id and the low half
// of the length -- the parsers read only the id). x87: a register value is a double, a stored one a float, the original's
// constants; floats the original moves as integers are moved as their bits (edit_build.h).
//
// Footprints: everything here reads or writes a file (the stream the reader or writer has open, and out.txt) -- replay_only
// (the session replay: the model tool's 3DS import and export) -- except fixup_vertex (pure: one vertex through a frame)
// and ADGetNesting (reads a static, writes nothing).
//
// FIX CANDIDATEs (left faithful, marked in place; all need a malformed or unusual .3ds, which an editor user can import):
// a chunk whose length is under 6 (its header) hangs ADParseChunks (it seeks back onto the same chunk for ever); a file
// cut short leaves the values a read didn't get as the stack held them (vertices, mapping, face indices, counts from
// garbage -- and garbage that moves with the callers' frames, so a rewritten caller reads other garbage there than the
// original's would); the reader writes the caller's arrays with no bound (Import3DS's: 4096 vertices, 4096 triangles, 64 surfaces, on its
// stack); a texture name of 16 or more characters runs over its surface's fields; a face list or a mapping list whose
// counts don't match the vertices makes indices / coordinates past what was read; ADOpen leaves out.txt open when the .3ds
// can't be opened (Read3DS then doesn't ADClose); ADClose leaves the log's FILE* set (a later ADParseChunks without ADOpen
// would write to a closed stream -- none does); a face list's error line is printed with a %04x and no argument (stdout:
// nowhere in the game). The writer's chunk stack holds 1024 open chunks (Write3DS nests 6).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "x87.h"
#include "edit_build.h"

namespace {
namespace edit_3ds {
using namespace ebld;
using uit::crt_strlen; using uit::crt_copy;

typedef void* FILEp;
#define AD_IN EB_GP(void, S_AD_IN)
#define AD_OUT EB_GP(void, S_AD_OUT)
#define AD_LOG EB_GP(void, S_AD_LOG)
static __forceinline uint8_t file_flag(void* fp) { return *(volatile uint8_t*)((uint8_t*)fp + 0xc); }   // FILE::_flag (0x10: EOF)
static __forceinline uint32_t c_fread(void* p, uint32_t sz, uint32_t n, void* fp) { return ccall<uint32_t>(F_fread, p, sz, n, fp); }
static __forceinline uint32_t c_fwrite(const void* p, uint32_t sz, uint32_t n, void* fp) { return ccall<uint32_t>(F_fwrite, p, sz, n, fp); }
static __forceinline int32_t c_ftell(void* fp) { return ccall<int32_t>(F_ftell, fp); }
static __forceinline int32_t c_fseek(void* fp, int32_t off, int32_t whence) { return ccall<int32_t>(F_fseek, fp, off, whence); }
static __forceinline void* c_fopen(const char* name, const char* mode) { return ccall<void*>(F_fopen, name, mode); }
static __forceinline int32_t c_fclose(void* fp) { return ccall<int32_t>(F_fclose, fp); }
static __forceinline char* c_strchr(const char* s, int32_t c) { return ccall<char*>(F_strchr, s, c); }

// this group's functions, by their v1.0 addresses
static __forceinline void out_tab() { ccall<void>(F_out_tab); }
static __forceinline uint8_t ADOpen(const char* n) { return ccall<uint8_t>(F_ADOpen, n); }
static __forceinline void ADClose() { ccall<void>(F_ADClose); }
static __forceinline uint8_t ADReadChunk(uint32_t id, int32_t* len) { return ccall<uint8_t>(F_ADReadChunk, id, len); }
static __forceinline uint8_t ADRead(void* p, int32_t n) { return ccall<uint8_t>(F_ADRead, p, n); }
static __forceinline uint8_t ADReadString(char* p, int32_t n) { return ccall<uint8_t>(F_ADReadString, p, n); }
static __forceinline uint8_t ADReadShort(volatile int16_t* p) { return ccall<uint8_t>(F_ADReadShort, (void*)p); }
static __forceinline uint8_t ADReadLong(volatile int32_t* p) { return ccall<uint8_t>(F_ADReadLong, (void*)p); }
static __forceinline uint8_t ADReadFloat(volatile float* p) { return ccall<uint8_t>(F_ADReadFloat, (void*)p); }
static __forceinline uint8_t ADReadChar(volatile char* p) { return ccall<uint8_t>(F_ADReadChar, (void*)p); }
static __forceinline uint8_t ADParseChunks(ADParseBlock* b) { return ccall<uint8_t>(F_ADParseChunks, (void*)b); }
static __forceinline uint8_t ADCreate(const char* n) { return ccall<uint8_t>(F_ADCreate, n); }
static __forceinline void ADCreateClose() { ccall<void>(F_ADCreateClose); }
static __forceinline uint8_t ADCreateChunk(uint32_t id) { return ccall<uint8_t>(F_ADCreateChunk, id); }
static __forceinline void ADCloseChunk() { ccall<void>(F_ADCloseChunk); }
static __forceinline uint8_t ADWriteShort(uint32_t v) { return ccall<uint8_t>(F_ADWriteShort, v); }
static __forceinline uint8_t ADWriteLong(uint32_t v) { return ccall<uint8_t>(F_ADWriteLong, v); }
static __forceinline uint8_t ADWriteString(const char* s) { return ccall<uint8_t>(F_ADWriteString, s); }
static __forceinline uint8_t ADWriteFloat(uint32_t bits) { return ccall<uint8_t>(F_ADWriteFloat, bits); }   // the float's bits
static __forceinline uint8_t ADWriteByte(uint32_t v) { return ccall<uint8_t>(F_ADWriteByte, v); }
static __forceinline uint8_t ADWrite(const void* p, int32_t n) { return ccall<uint8_t>(F_ADWrite, p, n); }
static __forceinline void ADWriteColorChunk(int32_t r, int32_t g, int32_t b) { ccall<void>(F_ADWriteColorChunk, r, g, b); }
static __forceinline void ADWritePercentageChunk(uint32_t s) { ccall<void>(F_ADWritePercentageChunk, s); }
static __forceinline void fixup_vertex(MrVertex* v, const void* fr) { ccall<void>(F_fixup_vertex, (void*)v, fr); }

static __forceinline void blk(ADParseBlock* b, uint16_t id, uint32_t fn, void* data) {
    b->id = id; b->fn = (ADParser)(uintptr_t)fn; b->data = data;
}
static __forceinline uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void fp_file(Footprint& f) { f.replay_only = "reads or writes a 3DS file (the C runtime's streams) and out.txt"; }
static void fp_parser(Footprint& f, uint32_t, int32_t, void*) { fp_file(f); }

// ==== adtools.obj ===========================================================================================================
// ADParseChunks: the chunks from here to the end of the enclosing one, each handed to its parser in the block (or skipped);
// false if a parser failed
static uint8_t __cdecl ADParseChunks_n(ADParseBlock* b) {
    struct { volatile uint8_t ok; uint8_t _1; volatile uint8_t hdr[6]; } fr;     // F+3 ok, F+0x14 id, F+0x16 length
    EB_G32(S_AD_NEST) = EB_G32(S_AD_NEST) + 1;
    fr.ok = 1;
    if (!(file_flag(AD_IN) & 0x10)) {
        do {
            if (c_fread((void*)fr.hdr, 6, 1, AD_IN) == 0) break;
            const int32_t at = c_ftell(AD_IN) - 6;
            if (at >= EB_G32(S_AD_LIMIT)) break;
            const int32_t outer = EB_G32(S_AD_LIMIT);
            EB_G32(S_AD_LIMIT) = *(volatile int32_t*)(fr.hdr + 2) + at;
            if (AD_LOG) {
                out_tab();
                EB_fprintf(AD_LOG, EB_CP(0x004ff42c), (uint32_t)*(volatile uint16_t*)fr.hdr,                // "+%04x [%d]\n"
                           *(volatile int32_t*)(fr.hdr + 2) - 6);
            }
            if (b) {
                uint8_t found = 0;
                int32_t k = 0;
                if (b[0].id != 0) {
                    const ADParseBlock* e = b;
                    for (;;) {
                        const uint16_t eid = e->id;
                        if (*(volatile uint16_t*)fr.hdr == eid || eid == 0xffff) {
                            const ADParser fn = b[k].fn;
                            if (fn) {
                                void* const data = b[k].data;
                                const uint32_t idw = *(volatile uint32_t*)fr.hdr;   // the id and the length's low half
                                fr.ok = (uint8_t)(fr.ok & fn(idw, *(volatile int32_t*)(fr.hdr + 2) - 6, data));
                            }
                            found = 1;
                            break;
                        }
                        e++;
                        k++;
                        if (e->id == 0) break;
                    }
                }
                if (!found) {
                    for (int32_t t = 0; t < EB_G32(S_AD_NEST); t++) EB_printf(EB_CP(0x004ff438));      // "  "
                    EB_printf(EB_CP(0x004ff43c), (uint32_t)*(volatile uint16_t*)fr.hdr,                 // "+%04x UNKNOWN [%d]\n"
                              *(volatile int32_t*)(fr.hdr + 2) - 6);
                }
            } else {
                EB_printf(EB_CP(0x004ff450), (uint32_t)*(volatile uint16_t*)fr.hdr,                     // "block %04x [%d]\n"
                          *(volatile int32_t*)(fr.hdr + 2));
            }
            // FIX CANDIDATE: a length under 6 seeks back onto this chunk's header (or into it): the loop never ends
            const int32_t next = *(volatile int32_t*)(fr.hdr + 2) + at;
            void* const fp = AD_IN;
            EB_G32(S_AD_LIMIT) = outer;
            c_fseek(fp, next, 0);
        } while (!(file_flag(AD_IN) & 0x10));
    }
    const uint8_t r = fr.ok;
    EB_G32(S_AD_NEST) = EB_G32(S_AD_NEST) - 1;
    return r;
}
static void fp_ADParseChunks(Footprint& f, ADParseBlock*) { fp_file(f); }
PORT_FN(0x004bba30, "ADParseChunks", ADParseChunks_n, fp_ADParseChunks)

// out_tab: two spaces per nesting level into the log
static void __cdecl out_tab_n() {
    for (int32_t i = 0; i < EB_G32(S_AD_NEST);) {
        void* const log = AD_LOG;
        i++;
        EB_fprintf(log, EB_CP(0x004ff464));                                                     // "  "
    }
}
static void fp_void(Footprint& f) { fp_file(f); }
PORT_FN(0x004bbbd0, "out_tab", out_tab_n, fp_void)

// ADOpen: the file to read ("rb") and out.txt ("wt", the log); false if the file can't be opened (out.txt is opened anyway)
static uint8_t __cdecl ADOpen_n(const char* name) {
    AD_LOG = 0;
    AD_IN = c_fopen(name, EB_CP(0x004ff468));                                                   // "rb"
    void* const log = c_fopen(EB_CP(0x004ff470), EB_CP(0x004ff46c));                           // "out.txt", "wt"
    AD_LOG = log;
    if (!log) EB_printf(EB_CP(0x004ff478));                                                     // "Couldn't open out.txt\n"
    else {
        AD_LOG = log;
        EB_fprintf(log, EB_CP(0x004ff490), name);                                               // " --- 3DS FILE: %s ---\n"
    }
    EB_G32(S_AD_LIMIT) = 100000000;
    const uint8_t r = AD_IN != 0;
    EB_G32(S_AD_NEST) = 0;
    return r;
}
// FIX CANDIDATE: out.txt is opened (and left open) even when `name` can't be (Read3DS then returns without ADClose) -- a
// FILE leaked per failed 3DS import (an editor user typing a name that isn't there)
static void fp_ADOpen(Footprint& f, const char*) { fp_file(f); }
PORT_FN(0x004bbc00, "ADOpen", ADOpen_n, fp_ADOpen)

// ADClose: closes the log (if open) and the file
static void __cdecl ADClose_n() {
    void* const log = AD_LOG;
    if (log) c_fclose(log);
    // FIX CANDIDATE: the log's FILE* stays set (closed); only ADOpen clears it, and every reader opens with ADOpen first
    c_fclose(AD_IN);
}
PORT_FN(0x004bbc80, "ADClose", ADClose_n, fp_void)

// ADReadChunk: the next chunk's header, which must be `id`: its data length into *len (if given)
static uint8_t __cdecl ADReadChunk_n(uint16_t id, int32_t* len) {
    volatile uint8_t hdr[8];
    if (c_fread((void*)hdr, 6, 1, AD_IN) != 1) return 0;
    if (*(volatile uint16_t*)hdr != id) {
        EB_printf(EB_CP(0x004ff4b8), (uint32_t)id, (uint32_t)*(volatile uint16_t*)hdr);          // "ReadChunk: expecting %04x, got %04x\n"
        return 0;
    }
    const int32_t n = *(volatile int32_t*)(hdr + 2) - 6;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff4a8), (uint32_t)*(volatile uint16_t*)hdr, n);           // "  *%04x [%d]\n"
    }
    if (len) *(volatile int32_t*)len = n;
    return 1;
}
static void fp_ADReadChunk(Footprint& f, uint16_t, int32_t*) { fp_file(f); }
PORT_FN(0x004bbcb0, "ADReadChunk", ADReadChunk_n, fp_ADReadChunk)

static uint8_t __cdecl ADRead_n(void* p, int32_t n) {
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff4e0), n);                                               // "  DATA [%d]\n"
    }
    return c_fread(p, (uint32_t)n, 1, AD_IN) == 1;
}
static void fp_ADRead(Footprint& f, void*, int32_t) { fp_file(f); }
PORT_FN(0x004bbd50, "ADRead", ADRead_n, fp_ADRead)

// ADReadString: a 0-terminated string, at most n - 1 characters kept (the rest of a longer one is left in the file)
static uint8_t __cdecl ADReadString_n(char* buf, int32_t n) {
    volatile char* p = buf;
    while (n > 1) {
        if (c_fread((void*)p, 1, 1, AD_IN) == 0) break;
        if (*p == 0) break;
        p++;
        n--;
    }
    *p = 0;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff4f0), buf);                                             // "  CSTR \"%s\"\n"
    }
    return n > 1;
}
static void fp_ADReadString(Footprint& f, char*, int32_t) { fp_file(f); }
PORT_FN(0x004bbdb0, "ADReadString", ADReadString_n, fp_ADReadString)

static uint8_t __cdecl ADReadShort_n(int16_t* p) {
    const uint8_t r = c_fread(p, 2, 1, AD_IN) == 1;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff500), (int32_t) * (volatile int16_t*)p);               // "  SHORT %d\n"
    }
    return r;
}
static void fp_ADReadShort(Footprint& f, int16_t*) { fp_file(f); }
PORT_FN(0x004bbe20, "ADReadShort", ADReadShort_n, fp_ADReadShort)

static uint8_t __cdecl ADReadLong_n(int32_t* p) {
    const uint8_t r = c_fread(p, 4, 1, AD_IN) == 1;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff50c), *(volatile int32_t*)p);                           // "  LONG %d\n"
    }
    return r;
}
static void fp_ADReadLong(Footprint& f, int32_t*) { fp_file(f); }
PORT_FN(0x004bbe70, "ADReadLong", ADReadLong_n, fp_ADReadLong)

static uint8_t __cdecl ADReadFloat_n(float* p) {
    const uint8_t r = c_fread(p, 4, 1, AD_IN) == 1;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff518), (double)*(volatile float*)p);                     // "  FLOAT %f\n"
    }
    return r;
}
static void fp_ADReadFloat(Footprint& f, float*) { fp_file(f); }
PORT_FN(0x004bbec0, "ADReadFloat", ADReadFloat_n, fp_ADReadFloat)

static uint8_t __cdecl ADReadChar_n(char* p) {
    const uint8_t r = c_fread(p, 1, 1, AD_IN) == 1;
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff524), (uint32_t) * (volatile uint8_t*)p);              // "  CHAR %d\n"
    }
    return r;
}
static void fp_ADReadChar(Footprint& f, char*) { fp_file(f); }
PORT_FN(0x004bbf10, "ADReadChar", ADReadChar_n, fp_ADReadChar)

static uint8_t __cdecl ADSkip_n(int32_t n) {
    if (AD_LOG) {
        out_tab();
        EB_fprintf(AD_LOG, EB_CP(0x004ff530), n);                                               // "  SKIP %d bytes\n"
    }
    c_fseek(AD_IN, n, 1);
    return 1;
}
static void fp_ADSkip(Footprint& f, int32_t) { fp_file(f); }
PORT_FN(0x004bbf60, "ADSkip", ADSkip_n, fp_ADSkip)

static int32_t __cdecl ADGetNesting_n() { return EB_G32(S_AD_NEST); }
static void fp_ADGetNesting(Footprint&) {}                                                      // writes nothing
PORT_FN(0x004bbfb0, "ADGetNesting", ADGetNesting_n, fp_ADGetNesting)

// ADCreate: the file to write ("wb")
static uint8_t __cdecl ADCreate_n(const char* name) {
    EB_G32(S_AD_DEPTH) = 0;
    void* const fp = c_fopen(name, EB_CP(0x004ff544));                                          // "wb"
    AD_OUT = fp;
    if (fp) return 1;
    EB_printf(EB_CP(0x004ff548), name);                                                         // "Couldn't open %s for output\n"
    return 0;
}
static void fp_ADCreate(Footprint& f, const char*) { fp_file(f); }
PORT_FN(0x004bbfc0, "ADCreate", ADCreate_n, fp_ADCreate)

static void __cdecl ADCreateClose_n() {
    const int32_t d = EB_G32(S_AD_DEPTH);
    if (d) EB_Log(EB_CP(0x004ff568), d);                                                        // "error: chunk stack has %d chunks"
    c_fclose(AD_OUT);
}
PORT_FN(0x004bc000, "ADCreateClose", ADCreateClose_n, fp_void)

// ADCreateChunk: a chunk header (its length 0 until ADCloseChunk), its offset pushed on the chunk stack
static uint8_t __cdecl ADCreateChunk_n(uint16_t id) {
    volatile uint8_t hdr[8];
    *(volatile uint32_t*)(hdr + 2) = 0;
    *(volatile uint16_t*)hdr = id;
    const int32_t at = c_ftell(AD_OUT);
    const int32_t d = EB_G32(S_AD_DEPTH);
    EB_G32(S_AD_DEPTH) = EB_G32(S_AD_DEPTH) + 1;
    // FIX CANDIDATE: the chunk stack has 1024 entries, unchecked (Write3DS nests 6 deep)
    EB_G32(S_AD_STACK + (uint32_t)d * 4) = at;
    if (c_fwrite((const void*)hdr, 6, 1, AD_OUT) != 1) EB_Log(EB_CP(0x004ff58c), (uint32_t)id);   // "Error writing chunk %04x"
    return 1;
}
static void fp_ADCreateChunk(Footprint& f, uint16_t) { fp_file(f); }
PORT_FN(0x004bc030, "ADCreateChunk", ADCreateChunk_n, fp_ADCreateChunk)

// ADCloseChunk: the innermost open chunk's length written into its header
static void __cdecl ADCloseChunk_n() {
    volatile int32_t len;
    const int32_t end = c_ftell(AD_OUT);
    const int32_t d = EB_G32(S_AD_DEPTH) - 1;
    EB_G32(S_AD_DEPTH) = d;
    const int32_t at = EB_G32(S_AD_STACK + (uint32_t)d * 4);
    c_fseek(AD_OUT, at + 2, 0);
    len = end - at;
    c_fwrite((const void*)&len, 4, 1, AD_OUT);
    c_fseek(AD_OUT, end, 0);
}
PORT_FN(0x004bc0a0, "ADCloseChunk", ADCloseChunk_n, fp_void)

// the writers: the argument's own bytes (its stack slot), true if written
static uint8_t __cdecl ADWriteShort_n(uint32_t v) { return c_fwrite(&v, 2, 1, AD_OUT) == 1; }
static void fp_u32(Footprint& f, uint32_t) { fp_file(f); }
PORT_FN(0x004bc120, "ADWriteShort", ADWriteShort_n, fp_u32)
static uint8_t __cdecl ADWriteLong_n(uint32_t v) { return c_fwrite(&v, 4, 1, AD_OUT) == 1; }
PORT_FN(0x004bc150, "ADWriteLong", ADWriteLong_n, fp_u32)
static uint8_t __cdecl ADWriteString_n(const char* s) {
    void* const fp = AD_OUT;
    return c_fwrite(s, crt_strlen(s) + 1, 1, fp) == 1;
}
static void fp_ADWriteString(Footprint& f, const char*) { fp_file(f); }
PORT_FN(0x004bc180, "ADWriteString", ADWriteString_n, fp_ADWriteString)
static uint8_t __cdecl ADWriteFloat_n(uint32_t bits) { return c_fwrite(&bits, 4, 1, AD_OUT) == 1; }
PORT_FN(0x004bc1c0, "ADWriteFloat", ADWriteFloat_n, fp_u32)
static uint8_t __cdecl ADWriteByte_n(uint32_t v) { return c_fwrite(&v, 1, 1, AD_OUT) == 1; }
PORT_FN(0x004bc1f0, "ADWriteByte", ADWriteByte_n, fp_u32)
static uint8_t __cdecl ADWrite_n(const void* p, int32_t n) { return c_fwrite(p, (uint32_t)n, 1, AD_OUT) == 1; }
static void fp_ADWrite(Footprint& f, const void*, int32_t) { fp_file(f); }
PORT_FN(0x004bc220, "ADWrite", ADWrite_n, fp_ADWrite)

// ==== cvt3ds.obj ============================================================================================================
// Read3DS: a .3ds into `info` (its arrays the caller's); false if it can't be opened or a parser failed
static uint8_t __cdecl Read3DS_n(MrModelInfo* info, const char* name) {
    ADParseBlock b[2];
    if (!ADOpen(name)) {
        EB_Log(EB_CP(0x004ff31c), name);                                                        // "Couldn't open %s"
        return 0;
    }
    b[1].id = 0;
    b[0].data = info;
    b[1].fn = 0;
    b[0].fn = (ADParser)(uintptr_t)F_parse_primary;
    b[1].data = 0;
    b[0].id = 0x4d4d;
    const uint8_t r = ADParseChunks(b);
    ADClose();
    return r;
}
static void fp_Read3DS(Footprint& f, MrModelInfo*, const char*) { fp_file(f); }
PORT_FN(0x004ba3d0, "Read3DS", Read3DS_n, fp_Read3DS)

// Write3DS: the model as a .3ds: a material per surface (its texture, as .TGA), an object per surface, the keyframer's
// (MAXSCENE, a node per object); false if it can't be created
static uint8_t __cdecl Write3DS_n(const MrModelInfo* info, const char* name) {
    char tex[0x100];                                   // F+0x48 (the texture's map name; then "NewMat #n" for the faces)
    char nm[0x100];                                    // F+0x148 ("NewMat #n", then "Surfnn")
    uint8_t r = 0;
    if (!ADCreate(name)) return r;
    if (ADCreateChunk(0x4d4d)) {
        if (ADCreateChunk(2)) { ADWriteLong(3); ADCloseChunk(); }
        if (ADCreateChunk(0x3d3d)) {
            if (ADCreateChunk(0x3d3e)) { ADWriteLong(3); ADCloseChunk(); }
            // the materials
            for (int32_t i = 0; info->nsurfs > i;) {
                const MrSurface* s = info->surfs + i;
                i++;
                EB_sprintf(nm, EB_CP(0x004ff330), i);                                           // "NewMat #%d"
                if (ADCreateChunk(0xafff)) {
                    if (ADCreateChunk(0xa000)) { ADWriteString(nm); ADCloseChunk(); }
                    if (ADCreateChunk(0xa010)) { ADWriteColorChunk(0x19, 0x19, 0x19); ADCloseChunk(); }
                    if (ADCreateChunk(0xa020)) { ADWriteColorChunk(0xb2, 0xb2, 0xb2); ADCloseChunk(); }
                    if (ADCreateChunk(0xa030)) { ADWriteColorChunk(0xe5, 0xe5, 0xe5); ADCloseChunk(); }
                    if (ADCreateChunk(0xa040)) { ADWritePercentageChunk(0x28); ADCloseChunk(); }
                    if (ADCreateChunk(0xa041)) { ADWritePercentageChunk(0x1e); ADCloseChunk(); }
                    if (ADCreateChunk(0xa050)) { ADWritePercentageChunk(0); ADCloseChunk(); }
                    if (ADCreateChunk(0xa052)) { ADWritePercentageChunk(0); ADCloseChunk(); }
                    if (ADCreateChunk(0xa053)) { ADWritePercentageChunk(0); ADCloseChunk(); }
                    if (ADCreateChunk(0xa100)) { ADWriteShort(3); ADCloseChunk(); }
                    if (ADCreateChunk(0xa084)) { ADWritePercentageChunk(0); ADCloseChunk(); }
                    if (ADCreateChunk(0xa08a)) ADCloseChunk();
                    if (ADCreateChunk(0xa087)) { ADWriteFloat(0x3f800000); ADCloseChunk(); }
                    if (ADCreateChunk(0xa200)) {
                        // the surface's texture name, its extension replaced by .TGA
                        crt_copy(tex, s->name, crt_strlen(s->name) + 1);
                        if (c_strchr(tex, 0x2e)) *c_strchr(tex, 0x2e) = 0;
                        {
                            char* e = tex + crt_strlen(tex);
                            *(volatile uint32_t*)e = *(const volatile uint32_t*)(uintptr_t)0x004ff33c;   // ".TGA"
                            *(volatile char*)(e + 4) = *(const volatile char*)(uintptr_t)0x004ff340;
                        }
                        ADWritePercentageChunk(0x64);
                        if (ADCreateChunk(0xa300)) { ADWriteString(tex); ADCloseChunk(); }
                        if (ADCreateChunk(0xa351)) { ADWriteShort(0); ADCloseChunk(); }
                        if (ADCreateChunk(0xa353)) { ADWriteFloat(0); ADCloseChunk(); }
                        ADCloseChunk();
                    }
                    ADCloseChunk();
                }
            }
            if (ADCreateChunk(0x100)) { ADWriteFloat(0x3f800000); ADCloseChunk(); }
            // the objects
            for (int32_t i = 0; info->nsurfs > i;) {
                const MrSurface* s = info->surfs + i;
                i++;
                EB_sprintf(tex, EB_CP(0x004ff344), i);                                          // "NewMat #%d"
                if (!ADCreateChunk(0x4000)) continue;
                EB_sprintf(nm, EB_CP(0x004ff350), i);                                           // "Surf%02d"
                ADWriteString(nm);
                if (ADCreateChunk(0x4100)) {
                    if (ADCreateChunk(0x4110)) {                       // the vertices, in inches (x mirrored)
                        ADWriteShort((uint32_t)(uint16_t)(s->v1 - s->v0));
                        for (int32_t k = s->v0; s->v1 > k; k++) {
                            ADWriteFloat(fbits((float)((double)info->verts[k].x * (double)-39.370079040527344f)));
                            ADWriteFloat(fbits((float)((double)info->verts[k].y * (double)39.370079040527344f)));
                            ADWriteFloat(fbits((float)((double)info->verts[k].z * (double)39.370079040527344f)));
                        }
                        ADCloseChunk();
                    }
                    if (ADCreateChunk(0x4140)) {                       // the mapping, v flipped
                        ADWriteShort((uint32_t)(uint16_t)(s->v1 - s->v0));
                        for (int32_t k = s->v0; s->v1 > k; k++) {
                            ADWriteFloat(eb_bits(&info->verts[k].u));
                            ADWriteFloat(fbits((float)(1.0 - (double)info->verts[k].v)));
                        }
                        ADCloseChunk();
                    }
                    if (ADCreateChunk(0x4160)) {                       // the local axes: identity
                        volatile uint32_t m[12];
                        m[0] = 0x3f800000; m[1] = 0; m[2] = 0; m[3] = 0; m[4] = 0x3f800000; m[5] = 0; m[6] = 0;
                        m[7] = 0; m[8] = 0x3f800000; m[9] = 0; m[10] = 0; m[11] = 0;
                        ADWrite((const void*)m, 0x30);
                        ADCloseChunk();
                    }
                    if (ADCreateChunk(0x4120)) {                       // the faces, wound back
                        ADWriteShort((uint32_t)(uint16_t)(s->t1 - s->t0));
                        for (int32_t k = s->t0; s->t1 > k; k++) {
                            ADWriteShort((uint32_t)(uint16_t)(info->tris[k].v[0] - s->v0));
                            ADWriteShort((uint32_t)(uint16_t)(info->tris[k].v[2] - s->v0));
                            ADWriteShort((uint32_t)(uint16_t)(info->tris[k].v[1] - s->v0));
                            ADWriteShort(6);
                        }
                        if (ADCreateChunk(0x4130)) {                   // all of them in the surface's material
                            ADWriteString(tex);
                            ADWriteShort((uint32_t)(uint16_t)(s->t1 - s->t0));
                            for (int32_t k = 0; (int32_t)s->t1 - (int32_t)s->t0 > k; k++) ADWriteShort((uint32_t)k);
                            ADCloseChunk();
                        }
                        ADCloseChunk();
                    }
                    ADCloseChunk();
                }
                ADCloseChunk();
            }
            ADCloseChunk();
        }
        // the keyframer
        if (ADCreateChunk(0xb000)) {
            if (ADCreateChunk(0xb00a)) {
                ADWriteShort(5);
                ADWriteString(EB_CP(0x004ff35c));                                               // "MAXSCENE"
                ADWriteLong(100);
                ADCloseChunk();
            }
            if (ADCreateChunk(0xb008)) { ADWriteLong(0); ADWriteLong(100); ADCloseChunk(); }
            if (ADCreateChunk(0xb009)) { ADWriteLong(0); ADCloseChunk(); }
            for (int32_t i = 0; info->nsurfs > i; i++) {
                if (!ADCreateChunk(0xb002)) continue;
                if (ADCreateChunk(0xb030)) { ADWriteShort((uint32_t)i); ADCloseChunk(); }
                if (ADCreateChunk(0xb010)) {
                    EB_sprintf(nm, EB_CP(0x004ff368), i + 1);                                   // "Surf%02d"
                    ADWriteString(nm);
                    ADWriteShort(0x4000);
                    ADWriteShort(0);
                    ADWriteShort(0xffffffff);
                    ADCloseChunk();
                }
                if (ADCreateChunk(0xb013)) { ADWriteFloat(0); ADWriteFloat(0); ADWriteFloat(0); ADCloseChunk(); }
                volatile uint8_t* const kb = (volatile uint8_t*)tex;          // a track's header: flags, 8 bytes, 1 key
                *(volatile uint32_t*)kb = 0; *(volatile uint32_t*)(kb + 4) = 0; *(volatile uint32_t*)(kb + 8) = 0;
                *(volatile uint16_t*)(kb + 0xc) = 0;
                *(volatile uint16_t*)(kb + 0xa) = 1;
                if (ADCreateChunk(0xb020)) {
                    ADWrite((const void*)kb, 0xe); ADWriteShort(0); ADWriteLong(0);
                    ADWriteFloat(0); ADWriteFloat(0); ADWriteFloat(0);
                    ADCloseChunk();
                }
                if (ADCreateChunk(0xb021)) {
                    ADWrite((const void*)kb, 0xe); ADWriteShort(0); ADWriteLong(0);
                    ADWriteFloat(0); ADWriteFloat(0xbf800000); ADWriteFloat(0); ADWriteFloat(0);
                    ADCloseChunk();
                }
                if (ADCreateChunk(0xb022)) {
                    ADWrite((const void*)kb, 0xe); ADWriteShort(0); ADWriteLong(0);
                    ADWriteFloat(0x3f800000); ADWriteFloat(0x3f800000); ADWriteFloat(0x3f800000);
                    ADCloseChunk();
                }
                ADCloseChunk();
            }
            ADCloseChunk();
        }
        r = 1;
        ADCloseChunk();
    }
    ADCreateClose();
    return r;
}
static void fp_Write3DS(Footprint& f, const MrModelInfo*, const char*) { fp_file(f); }
PORT_FN(0x004ba440, "Write3DS", Write3DS_n, fp_Write3DS)

// a colour chunk (0x11: three bytes) / a percentage chunk (0x30: a short)
static void __cdecl ADWriteColorChunk_n(int32_t r, int32_t g, int32_t b) {
    if (!ADCreateChunk(0x11)) return;
    ADWriteByte((uint32_t)r);
    ADWriteByte((uint32_t)g);
    ADWriteByte((uint32_t)b);
    ADCloseChunk();
}
static void fp_ADWriteColorChunk(Footprint& f, int32_t, int32_t, int32_t) { fp_file(f); }
PORT_FN(0x004badf0, "ADWriteColorChunk", ADWriteColorChunk_n, fp_ADWriteColorChunk)
static void __cdecl ADWritePercentageChunk_n(uint32_t s) {
    if (!ADCreateChunk(0x30)) return;
    ADWriteShort(s);
    ADCloseChunk();
}
PORT_FN(0x004bae30, "ADWritePercentageChunk", ADWritePercentageChunk_n, fp_u32)

// ---- the parsers: (the chunk's id, its data's length, the parser's data) ------------------------------------------------
// the file: its version (2) and the editor section (3d3d)
static uint8_t __cdecl parse_primary_n(uint32_t, int32_t, void* data) {
    ADParseBlock b[3];
    b[0].id = 2;
    b[0].data = 0;
    b[2].id = 0;
    b[1].data = data;
    b[2].fn = 0;
    b[2].data = 0;
    b[0].fn = (ADParser)(uintptr_t)F_parse_m3d_version;
    b[1].fn = (ADParser)(uintptr_t)F_parse_edit;
    b[1].id = 0x3d3d;
    return ADParseChunks(b);
}
PORT_FN(0x004bae60, "parse_primary", parse_primary_n, fp_parser)

static uint8_t __cdecl parse_m3d_version_n(uint32_t, int32_t, void*) {
    volatile int32_t v[2];                             // (8 bytes: a local the compiler can't make with a push -- a read
    return ADReadLong(&v[0]) != 0;                     // that fails leaves it as the stack held it, as the original's)
}
PORT_FN(0x004baeb0, "parse_m3d_version", parse_m3d_version_n, fp_parser)

// the editor section: the objects (4000) and the materials (afff) into the info; its counts set from the reader's
static uint8_t __cdecl parse_edit_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    ADParseBlock b[5];
    EB_G32(S_3DS_NOBJS) = 0;
    EB_G32(S_3DS_NMATS) = 0;
    EB_G32(S_3DS_NVERTS) = 0;
    EB_G32(S_3DS_NTRIS) = 0;
    EB_G32(S_3DS_NUV) = 0;
    b[0].fn = (ADParser)(uintptr_t)F_parse_float;
    b[0].data = 0;
    b[1].fn = (ADParser)(uintptr_t)F_parse_object;
    b[2].fn = (ADParser)(uintptr_t)F_parse_mesh_version;
    b[1].data = info;
    b[2].data = 0;
    b[3].fn = (ADParser)(uintptr_t)F_parse_material_list;
    b[0].id = 0x100;
    b[4].id = 0;
    b[3].data = info;
    b[4].fn = 0;
    b[4].data = 0;
    b[1].id = 0x4000;
    b[2].id = 0x3d3e;
    b[3].id = 0xafff;
    const uint8_t r = ADParseChunks(b);
    info->nsurfs = EB_G32(S_3DS_NOBJS);
    info->ntris = EB_G32(S_3DS_NTRIS);
    info->nverts = EB_G32(S_3DS_NVERTS);
    return r;
}
PORT_FN(0x004baed0, "parse_edit", parse_edit_n, fp_parser)

static uint8_t __cdecl parse_mesh_version_n(uint32_t, int32_t, void*) {
    volatile int32_t v[2];
    ADReadLong(&v[0]);
    return 1;
}
PORT_FN(0x004baf80, "parse_mesh_version", parse_mesh_version_n, fp_parser)

// a material: its name, colours and percentages (read past), its texture map (a200)
static uint8_t __cdecl parse_material_list_n(uint32_t, int32_t, void* data) {
    ADParseBlock b[15];
    const uint32_t color = F_parse_material_color, pct = F_parse_material_percent;
    b[0].id = 0xa000;
    b[0].fn = (ADParser)(uintptr_t)F_parse_materials_name;
    b[0].data = 0;
    b[1].fn = (ADParser)(uintptr_t)color;
    b[2].fn = (ADParser)(uintptr_t)color;
    b[1].data = (void*)(uintptr_t)0x004ff374;                          // "AMBIENT"
    b[2].data = (void*)(uintptr_t)0x004ff37c;                          // "DIFFUSE"
    b[3].data = (void*)(uintptr_t)0x004ff384;                          // "SPECULAR"
    b[1].id = 0xa010;
    b[4].data = (void*)(uintptr_t)0x004ff390;                          // "SHININESS"
    b[2].id = 0xa020;
    b[3].id = 0xa030;
    b[5].data = (void*)(uintptr_t)0x004ff39c;                          // "SHINY2"
    b[4].id = 0xa040;
    b[5].id = 0xa041;
    b[6].data = (void*)(uintptr_t)0x004ff3a4;                          // "SHINY3"
    b[6].id = 0xa042;
    b[7].id = 0xa050;
    b[7].data = (void*)(uintptr_t)0x004ff3ac;                          // "TRANSPARENCY"
    b[8].id = 0xa052;
    b[3].fn = (ADParser)(uintptr_t)color;
    b[4].fn = (ADParser)(uintptr_t)pct;
    b[5].fn = (ADParser)(uintptr_t)pct;
    b[6].fn = (ADParser)(uintptr_t)pct;
    b[7].fn = (ADParser)(uintptr_t)pct;
    b[8].fn = (ADParser)(uintptr_t)pct;
    b[9].fn = (ADParser)(uintptr_t)pct;
    b[8].data = (void*)(uintptr_t)0x004ff3bc;                          // "XPFALL"
    b[9].data = (void*)(uintptr_t)0x004ff3c4;                          // "REFBLUR"
    b[10].fn = (ADParser)(uintptr_t)F_parse_short;
    b[10].data = (void*)(uintptr_t)0x004ff3cc;                         // "SHADING"
    b[11].data = (void*)(uintptr_t)0x004ff3d4;                         // "SELF_ILPCT"
    b[9].id = 0xa053;
    b[12].fn = (ADParser)(uintptr_t)F_parse_float;
    b[12].data = (void*)(uintptr_t)0x004ff3e0;                         // "WIRESIZE"
    b[10].id = 0xa100;
    b[13].fn = (ADParser)(uintptr_t)F_parse_texture;
    b[11].fn = (ADParser)(uintptr_t)pct;
    b[14].id = 0;
    b[13].data = data;
    b[14].fn = 0;
    b[14].data = 0;
    b[11].id = 0xa084;
    b[12].id = 0xa087;
    b[13].id = 0xa200;
    return ADParseChunks(b);
}
PORT_FN(0x004bafa0, "parse_material_list", parse_material_list_n, fp_parser)

static uint8_t __cdecl parse_materials_name_n(uint32_t, int32_t, void*) {
    char buf[0x100];
    ADReadString(buf, 0x100);
    return 1;
}
PORT_FN(0x004bb110, "parse_materials_name", parse_materials_name_n, fp_parser)

// a texture map: its percentage, its file name (a300) -- the extension replaced by .tex (.bmp for a name starting '+')
// and copied to the next material's surface -- its tiling (a351) and blur (a353)
static uint8_t __cdecl parse_texture_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    char buf[0x100];
    volatile int16_t pct;
    volatile uint32_t misc;                            // the tiling short, then the blur float (one slot)
    if (!ADReadChunk(0x30, 0)) return 0;
    ADReadShort(&pct);
    if (!ADReadChunk(0xa300, 0)) return 0;
    if (ADReadString(buf, 0x100)) {
        if (c_strchr(buf, 0x2e)) *c_strchr(buf, 0x2e) = 0;
        if (buf[0] != 0) {
            const char* ext = buf[0] == 0x2b ? EB_CP(0x004ff3ec) : EB_CP(0x004ff3f4);              // ".bmp" / ".tex"
            char* e = buf + crt_strlen(buf);
            *(volatile uint32_t*)e = *(const volatile uint32_t*)ext;
            *(volatile char*)(e + 4) = *(const volatile char*)(ext + 4);
        }
        // FIX CANDIDATE: a name of 16 or more characters (with its extension) runs over the surface's fields and into the
        // next surface (a .3ds with a long map name), and a material past the objects' surfaces writes past the array
        {
            const uint32_t n = crt_strlen(buf) + 1;
            crt_copy(info->surfs[EB_G32(S_3DS_NMATS)].name, buf, n);
        }
        info->surfs[EB_G32(S_3DS_NMATS)].type = 0;
        info->surfs[EB_G32(S_3DS_NMATS)].b11 = 0;
        info->surfs[EB_G32(S_3DS_NMATS)].group = 0;
        EB_G32(S_3DS_NMATS) = EB_G32(S_3DS_NMATS) + 1;
    }
    if (ADReadChunk(0xa351, 0)) ADReadShort((volatile int16_t*)&misc);
    if (ADReadChunk(0xa353, 0)) ADReadFloat((volatile float*)&misc);
    return 1;
}
PORT_FN(0x004bb140, "parse_texture", parse_texture_n, fp_parser)

// an object: its name (read past), then its triangle mesh (4100) as the next surface; its vertices put through the object's
// axes (always identity: parse_local_axis reads them into a local)
static uint8_t __cdecl parse_object_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    ADParseBlock b[2];
    char name[0x100];
    b[0].id = 0x4100;
    b[0].fn = (ADParser)(uintptr_t)F_parse_tri_list;
    b[1].id = 0;
    b[0].data = info;
    b[1].fn = 0;
    b[1].data = 0;
    if (!ADReadString(name, 0x100)) return 0;
    {
        volatile float* const m = (volatile float*)(uintptr_t)S_3DS_FRAME;
        eb_put(m + 1, 0); eb_put(m + 0, 0x3f800000); eb_put(m + 2, 0); eb_put(m + 3, 0); eb_put(m + 4, 0x3f800000);
        eb_put(m + 5, 0); eb_put(m + 6, 0); eb_put(m + 7, 0); eb_put(m + 8, 0x3f800000);
        const int32_t o = EB_G32(S_3DS_NOBJS);
        eb_put(m + 9, 0);
        const int32_t t = EB_G32(S_3DS_NTRIS);
        eb_put(m + 10, 0); eb_put(m + 11, 0);
        MrSurface* const s = info->surfs + o;
        // FIX CANDIDATE: no bound on the objects (Import3DS's array: 64); the counts kept as shorts (past 32767 vertices the
        // first one turns negative and fixup_vertex runs from before the array)
        s->t0 = (int16_t)t;
        s->v0 = (int16_t)EB_G32(S_3DS_NVERTS);
        const uint8_t ok = ADParseChunks(b);
        s->t1 = (int16_t)EB_G32(S_3DS_NTRIS);
        const int16_t end = (int16_t)EB_G32(S_3DS_NVERTS);
        s->v1 = end;
        int32_t k = s->v0;
        if (end > k) {
            do {
                MrVertex* const v = info->verts + k;
                k++;
                fixup_vertex(v, (const void*)(uintptr_t)S_3DS_FRAME);
            } while (s->v1 > k);
        }
        if (ok) {
            EB_G32(S_3DS_NOBJS) = EB_G32(S_3DS_NOBJS) + 1;
            return 1;
        }
    }
    EB_Log(EB_CP(0x004ff3fc));                                                                  // " error"
    return 0;
}
PORT_FN(0x004bb2e0, "parse_object(cvt3ds.obj)", parse_object_n, fp_parser)

// fixup_vertex: a vertex's position through a frame (the rows' transpose, then the position added)
static void __cdecl fixup_vertex_n(MrVertex* v, const EbFrame* f) {
    volatile float t[3], r[3];
    eb_cp3(t, &v->x);
    eb_st(&r[0], ((double)f->m[6] * t[2] + (double)f->m[3] * t[1]) + (double)f->m[0] * t[0]);
    eb_st(&r[1], ((double)f->m[1] * t[0] + (double)f->m[7] * t[2]) + (double)f->m[4] * t[1]);
    eb_st(&r[2], ((double)f->m[5] * t[1] + (double)f->m[2] * t[0]) + (double)f->m[8] * t[2]);
    eb_st(&r[0], (double)f->p[0] + r[0]);
    eb_st(&r[1], (double)f->p[1] + r[1]);
    eb_st(&r[2], (double)f->p[2] + r[2]);
    eb_cp3(&v->x, r);
}
static void fp_fixup_vertex(Footprint& f, MrVertex* v, const EbFrame*) { f.add(v, 12, "the vertex"); f.pure = true; }
PORT_FN(0x004bb420, "fixup_vertex", fixup_vertex_n, fp_fixup_vertex)

// a triangle mesh: vertices (4110), faces (4120), face materials (4130), mapping (4140), local axes (4160)
static uint8_t __cdecl parse_tri_list_n(uint32_t, int32_t, void* data) {
    ADParseBlock b[6];
    b[0].id = 0x4110;
    b[0].data = data;
    b[1].data = data;
    b[0].fn = (ADParser)(uintptr_t)F_parse_vertex_list;
    b[1].fn = (ADParser)(uintptr_t)F_parse_face_list;
    b[2].fn = (ADParser)(uintptr_t)F_parse_face_material;
    b[3].fn = (ADParser)(uintptr_t)F_parse_mapping_coords;
    b[2].data = data;
    b[3].data = data;
    b[4].fn = (ADParser)(uintptr_t)F_parse_local_axis;
    b[1].id = 0x4120;
    b[2].id = 0x4130;
    b[3].id = 0x4140;
    b[4].data = data;
    b[5].id = 0;
    b[5].fn = 0;
    b[4].id = 0x4160;
    b[5].data = 0;
    return ADParseChunks(b);
}
PORT_FN(0x004bb4e0, "parse_tri_list", parse_tri_list_n, fp_parser)

// the vertices: inches to metres, x mirrored
static uint8_t __cdecl parse_vertex_list_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    volatile int16_t n;
    volatile float p[3];
    if (!ADReadShort(&n)) return 0;
    int16_t i = 0;
    if (n > i) {
        do {
            i++;
            // FIX CANDIDATE: a read that fails (the file cut short) leaves the coordinate as the stack held it
            ADReadFloat(&p[0]);
            ADReadFloat(&p[1]);
            ADReadFloat(&p[2]);
            // FIX CANDIDATE: no bound on the vertices (Import3DS's array: 4096)
            eb_st(&info->verts[EB_G32(S_3DS_NVERTS)].x, (double)p[0] * (double)-0.02539999969303608f);
            eb_st(&info->verts[EB_G32(S_3DS_NVERTS)].y, (double)p[1] * (double)0.02539999969303608f);
            eb_st(&info->verts[EB_G32(S_3DS_NVERTS)].z, (double)p[2] * (double)0.02539999969303608f);
            EB_G32(S_3DS_NVERTS) = EB_G32(S_3DS_NVERTS) + 1;
        } while (i < n);
    }
    return 1;
}
PORT_FN(0x004bb570, "parse_vertex_list", parse_vertex_list_n, fp_parser)

// the faces: each the object's vertices (from its surface's first), wound back; then its sub-chunks (4130 material
// groups, 4150 smoothing)
static uint8_t __cdecl parse_face_list_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    ADParseBlock b[3];
    volatile int16_t f[4];                             // F+0xe flags, +0x10 a, +0x12 b, +0x14 c
    volatile int16_t n;
    b[0].id = 0x4130;
    b[0].fn = (ADParser)(uintptr_t)F_parse_mesh_mat_group;
    b[1].fn = (ADParser)(uintptr_t)F_parse_smooth_group;
    b[1].id = 0x4150;
    b[2].id = 0;
    b[0].data = info;
    b[1].data = info;
    b[2].fn = 0;
    b[2].data = 0;
    if (!ADReadShort(&n)) {
        EB_printf(EB_CP(0x004ff404));                  // "+%04x FACE LIST [error]\n" -- FIX CANDIDATE: a %04x with no argument
        return 0;
    }
    int16_t i = 0;
    if (n > 0) {
        do {
            i++;
            ADReadShort(&f[1]);
            ADReadShort(&f[2]);
            ADReadShort(&f[3]);
            ADReadShort(&f[0]);
            // FIX CANDIDATE: no bound on the triangles (Import3DS's array: 4096), nor on the indices
            const MrSurface* const s = info->surfs + EB_G32(S_3DS_NOBJS);
            MrTriangle* const t = info->tris + EB_G32(S_3DS_NTRIS);
            *(volatile uint32_t*)&t->v[0] = 0;
            *(volatile uint32_t*)&t->v[2] = 0;
            t->v[0] = (int16_t)(s->v0 + f[1]);
            t->v[2] = (int16_t)(s->v0 + f[2]);
            t->v[1] = (int16_t)(s->v0 + f[3]);
            EB_G32(S_3DS_NTRIS) = EB_G32(S_3DS_NTRIS) + 1;
        } while (i < n);
    }
    ADParseChunks(b);
    return 1;
}
PORT_FN(0x004bb640, "parse_face_list", parse_face_list_n, fp_parser)

// a face material list (read past)
static uint8_t __cdecl parse_face_material_n(uint32_t, int32_t, void*) {
    char buf[0x100];
    volatile int16_t n, f;
    if (!ADReadString(buf, 0x100)) return 0;
    if (!ADReadShort(&n)) return 0;
    int16_t i = 0;
    if (n > i) {
        do {
            i++;
            f = 0;
            ADReadShort(&f);
        } while (i < n);
    }
    return 1;
}
PORT_FN(0x004bb760, "parse_face_material", parse_face_material_n, fp_parser)

// the mapping: u as read, v flipped, onto the vertices from the reader's count of them
static uint8_t __cdecl parse_mapping_coords_n(uint32_t, int32_t, void* data) {
    MrModelInfo* const info = (MrModelInfo*)data;
    volatile int16_t n;
    volatile float uv[2];
    if (!ADReadShort(&n)) return 0;
    int16_t i = 0;
    if (n > i) {
        do {
            i++;
            ADReadFloat(&uv[0]);
            ADReadFloat(&uv[1]);
            // FIX CANDIDATE: no bound (more coordinates than vertices write past the array)
            eb_cp(&info->verts[EB_G32(S_3DS_NUV)].u, &uv[0]);
            eb_st(&info->verts[EB_G32(S_3DS_NUV)].v, 1.0 - (double)uv[1]);
            EB_G32(S_3DS_NUV) = EB_G32(S_3DS_NUV) + 1;
        } while (i < n);
    }
    return 1;
}
PORT_FN(0x004bb7d0, "parse_mapping_coords", parse_mapping_coords_n, fp_parser)

// a face list's material group (read past; its count left as it was if the name can't be read)
static uint8_t __cdecl parse_mesh_mat_group_n(uint32_t, int32_t, void*) {
    char buf[0x100];
    volatile int16_t n, f;
    int16_t i = 0;
    ADReadString(buf, 0x100);
    ADReadShort(&n);
    if (n > i) {
        do {
            i++;
            ADReadShort(&f);
        } while (i < n);
    }
    return 1;
}
PORT_FN(0x004bb860, "parse_mesh_mat_group", parse_mesh_mat_group_n, fp_parser)

static uint8_t __cdecl parse_smooth_group_n(uint32_t, int32_t len, void*) {
    volatile int32_t v[2];
    int32_t n = (int32_t)((uint32_t)len >> 2);
    if (n > 0) {
        do ADReadLong(&v[0]);
        while (--n != 0);
    }
    return 1;
}
PORT_FN(0x004bb8b0, "parse_smooth_group", parse_smooth_group_n, fp_parser)

static uint8_t __cdecl parse_local_axis_n(uint32_t, int32_t, void*) {
    volatile uint8_t m[0x30];
    return ADRead((void*)m, 0x30) != 0;
}
PORT_FN(0x004bb8e0, "parse_local_axis", parse_local_axis_n, fp_parser)

static uint8_t __cdecl parse_material_color_n(uint32_t, int32_t, void*) {
    ADParseBlock b[2];
    b[1].id = 0;
    b[0].data = 0;
    b[1].fn = 0;
    b[1].data = 0;
    b[0].fn = (ADParser)(uintptr_t)F_parse_color;
    b[0].id = 0x11;
    return ADParseChunks(b);
}
PORT_FN(0x004bb900, "parse_material_color", parse_material_color_n, fp_parser)

static uint8_t __cdecl parse_material_percent_n(uint32_t, int32_t, void*) {
    ADParseBlock b[2];
    b[1].id = 0;
    b[0].data = 0;
    b[1].fn = 0;
    b[1].data = 0;
    b[0].fn = (ADParser)(uintptr_t)F_parse_percent;
    b[0].id = 0x30;
    return ADParseChunks(b);
}
PORT_FN(0x004bb940, "parse_material_percent", parse_material_percent_n, fp_parser)

static uint8_t __cdecl parse_color_n(uint32_t, int32_t, void*) {
    volatile char c[8];
    ADReadChar(&c[3]);
    ADReadChar(&c[2]);
    ADReadChar(&c[1]);
    return 1;
}
PORT_FN(0x004bb980, "parse_color", parse_color_n, fp_parser)

static uint8_t __cdecl parse_percent_n(uint32_t, int32_t, void*) {
    volatile int16_t s[4];
    ADReadShort(&s[1]);
    return 1;
}
PORT_FN(0x004bb9b0, "parse_percent", parse_percent_n, fp_parser)

static uint8_t __cdecl parse_short_n(uint32_t, int32_t, void*) {
    volatile int16_t s[4];
    ADReadShort(&s[1]);
    return 1;
}
PORT_FN(0x004bb9d0, "parse_short", parse_short_n, fp_parser)

static uint8_t __cdecl parse_float_n(uint32_t, int32_t, void*) {
    volatile float v[2];
    ADReadFloat(&v[0]);
    return 1;
}
PORT_FN(0x004bb9f0, "parse_float", parse_float_n, fp_parser)

}  // namespace edit_3ds
}  // namespace
