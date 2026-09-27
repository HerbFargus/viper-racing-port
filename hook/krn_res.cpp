// krn_res.cpp -- M3 stage "kernel and utilities", group K4, rewritten: res.obj (the resource system), locale.obj
// (languages, Xlate, units, number / money / date formats), opt.obj (the options file) and tmetry.obj (telemetry
// meters).
//
// The resource system. Every track, car, texture, model and table the game loads comes through ResourceGet.
//   ResourceBegin(dir1, dir2)     the two directories resource files are looked for in, each made absolute
//                                 (_fullpath, MAX_PATH) with a '\' on the end: 0x509130 and 0x509238.
//   a resource SET                a "0TSR" archive: a 16-byte header {magic, count, preload, hdr_size}, then
//                                 `count` 36-byte TOC entries {name[16], type, version, size, refs, data}, then
//                                 each payload behind 8 bytes {backpointer, "!IGM"}, in TOC order. The first
//                                 `preload` payloads lie inside the first hdr_size bytes, which are read into
//                                 the heap with the TOC; the rest are reached through a read-only memory map of
//                                 the whole file (FileCreateMemoryMap), which stays mapped while the set is
//                                 loaded (its file stays open too: one of the file layer's 32 file slots).
//                                 load_resource_set(name) tries dir1 + name, then dir2 + name; a set already
//                                 loaded under that name (stricmp) just counts one more load.
//   the set list                  0x4eae40, newest first. ResourceGet("set/res") looks in the named set only,
//                                 "res" in every set, newest first, entries in TOC order (stricmp): the first
//                                 match wins. A name found in no set -- or starting with '*' (a file) or '~'
//                                 (a file in the user directory) -- is HUNTED: hunt_for_resource opens that file
//                                 (dir1 + name, then dir2 + name; '~': user dir + name), which must be a "0SER"
//                                 resource {magic, type, version, 0, "!IGM", payload}, and makes it a one-entry
//                                 set of its own (set name "", entry name the file's base name with a '*' stored
//                                 after its terminator: the singleton mark), put at the head of the list.
//   ResourceGet                   checks the type (a mismatch panics), sets bit 31 of the entry's size (fetched
//                                 before: `fresh` is its complement), counts a reference, returns data + 8.
//   ResourceForget                one reference less; a singleton's last reference frees it.
//   Maximize / Minimize           remap / unmap a set's file and repoint its mapped entries.
//
// Threads: resources are fetched from both threads (the physics loads car data); the set list and the entries are
// guarded by the "Resource" Multi lock. So each footprint lists the statics and entries its function writes;
// what opens, maps, allocates or frees is replay_only. ResourceGet / Try: the entry it will find (the footprint
// repeats the lookup, read only), the Try flag and the outputs; a name that would be hunted is replay_only.
//
// locale.obj: the languages are the *.lng files (LANG resources, hunted); LocaleBegin picks one from the options
// ("GLOBAL" "language"), else from Windows (GetLocaleInfoA), else "English", else the first; Xlate looks a key up
// in the current language's sorted (key, text) table with the game's bsearch; the 12 unit Xlators refresh the
// unit tables. opt.obj: the options are "[section]" / "key value" lines, "options.def" then <user dir>options.cfg,
// kept in a 256-item table (0x55a078, 0x90 bytes each), written back by OptionsFlush. tmetry.obj: named float
// metrics, meters (float*) collected into 4 sets that TelemetrySetUpdate copies the metrics into.
//
// Written from the v1.0 disassembly: every call in the original's order, by address (this group's own functions
// too); Windows through the game's own import slots (GetLocaleInfoA, GetCurrencyFormatA, GetDateFormatA); the C
// runtime through the game's statically linked copy (stricmp, sprintf, sscanf, strchr, strrchr, strncpy, atoi,
// atof, bsearch, memmove, _fullpath), with the ORIGINAL's comparator / callback addresses. The inlined string
// copies (repne scasb; rep movsd; rep movsb) are the same instructions, so overlapping and overlong copies behave
// exactly as the original's; the locals that overflow into each other keep the original's layout.
//
// Skipped: the $E static initialisers ($E1/$E2 of each object reach rcfunc_is_internal; locale's $E5..$E41 point
// `locale` at its LocaleInfo and construct the 12 unit Xlators; opt's $E5..$E50 set colour constants): the CRT
// runs them before any hook exists.
#include <stdint.h>
#include <string.h>
#include "viperport.h"
#include "port.h"

typedef int Edx;                                    // the unused edx of a __thiscall received as __fastcall

// ---- the inlined string operations, as the original's instructions -------------------------------------------
// strcpy: repne scasb (the length, NUL included), then rep movsd, rep movsb: forward, so an overlapping copy
// smears exactly as the original's does
static __forceinline void i_strcpy(char* dst, const char* src) {
    __asm { mov edi, src
            mov ecx, 0xffffffff
            sub eax, eax
            repne scasb
            not ecx
            sub edi, ecx
            mov eax, ecx
            shr ecx, 2
            mov esi, edi
            mov edi, dst
            rep movsd
            mov ecx, eax
            and ecx, 3
            rep movsb }
}
// strcat: the source's length first, then the destination's end (repne scasb; dec edi), then the copy
static __forceinline void i_strcat(char* dst, const char* src) {
    __asm { mov edi, src
            mov ecx, 0xffffffff
            sub eax, eax
            repne scasb
            not ecx
            sub edi, ecx
            mov edx, ecx
            mov esi, edi
            mov ecx, 0xffffffff
            mov edi, dst
            sub eax, eax
            repne scasb
            dec edi
            mov ecx, edx
            shr ecx, 2
            rep movsd
            mov ecx, edx
            and ecx, 3
            rep movsb }
}
// the inlined strlen (repne scasb; not ecx; dec ecx)
static __forceinline uint32_t i_strlen(const char* s) {
    uint32_t n;
    __asm { mov edi, s
            mov ecx, 0xffffffff
            sub eax, eax
            repne scasb
            not ecx
            dec ecx
            mov n, ecx }
    return n;
}

// ---- layouts -------------------------------------------------------------------------------------------------------
namespace {

struct FileMemoryMap { uint8_t* ptr; uint32_t handle; uint32_t fake; };   // fake: a byte (+ 3 bytes as they come)
static_assert(sizeof(FileMemoryMap) == 12, "FileMemoryMap");
struct FileInfo { FileMemoryMap map; int32_t fh; };                          // ResourceSetNode::FileInfo
static_assert(sizeof(FileInfo) == 0x10, "FileInfo");

struct ResourceTOCEntry {                  // 0x24, as in the file (refs and data are 0 there)
    char name[16];                         // +0x00 no terminator when all 16 are used
    uint32_t type;                         // +0x10
    uint32_t version;                      // +0x14
    uint32_t size;                         // +0x18 bit 31: fetched at least once (ResourceGet)
    int32_t refs;                          // +0x1c
    uint8_t* data;                         // +0x20 the payload's 8-byte head {backpointer, "!IGM"}
};
static_assert(sizeof(ResourceTOCEntry) == 0x24, "ResourceTOCEntry");

struct ResourceSetNode {                   // 0x3c (MemAlloc)
    char name[16];                         // +0x00 strcpy'd, unbounded ("" for a hunted singleton)
    ResourceSetNode* next;                 // +0x10
    ResourceTOCEntry* toc;                 // +0x14 the heap copy of the file's first hdr_size - 16 bytes
    uint32_t magic;                        // +0x18 the file's header, copied
    uint32_t count;                        // +0x1c
    uint32_t preload;                      // +0x20 (a hunted node leaves it as MemAlloc's 0xa3a3a3a3)
    uint32_t hdr_size;                     // +0x24
    int32_t loads;                         // +0x28 load_resource_set calls less unloads
    FileInfo file;                         // +0x2c
};
static_assert(sizeof(ResourceSetNode) == 0x3c && offsetof(ResourceSetNode, file) == 0x2c, "ResourceSetNode");

// locale.obj
struct Xlator { const char* key; const char* value; uint32_t cookie; };
static_assert(sizeof(Xlator) == 12, "Xlator");
struct LangPair { const char* key; const char* text; };
struct LangResource { char name[0x40]; int32_t count; LangPair pairs[1]; };   // offsets fixed up by fixup_res
static_assert(offsetof(LangResource, pairs) == 0x44, "LangResource");
struct LangInfo { char name[0x20]; char file[0x20]; };                       // 8 at 0x509438
static_assert(sizeof(LangInfo) == 0x40, "LangInfo");
struct LocaleTime { uint16_t year, month, dow, day, hour, minute, second, ms; };   // SYSTEMTIME

// opt.obj
struct OptionsItem {                       // 0x90, 256 at 0x55a078
    char section[0x20];                    // +0x00 (strcpy, unbounded)
    char key[0x20];                        // +0x20 (strcpy, unbounded)
    uint8_t b, _41[3];                     // +0x40 "yes"
    int32_t i;                             // +0x44 atoi
    uint32_t f;                            // +0x48 a float (atof; moved as bits)
    char s[0x40];                          // +0x4c the text (load_options: strcpy, unbounded)
    int32_t type;                          // +0x8c 0 read, 1 bool, 2 int, 3 string, 4 float: how OptionsFlush writes it
};
static_assert(sizeof(OptionsItem) == 0x90 && offsetof(OptionsItem, s) == 0x4c && offsetof(OptionsItem, type) == 0x8c, "OptionsItem");

// tmetry.obj
struct TmMetric { char name[0x40]; uint32_t value; };                        // 0x44, 0x200 at 0x563084
struct TmPending { int32_t set; char name[0x40]; uint32_t* dst; };           // 0x48, 0x200 at 0x56b88c
struct TmPair { int32_t metric; uint32_t* dst; };
struct TmSet { int32_t count; TmPair pairs[0x200]; };                        // 0x1004, 4 at 0x574890
static_assert(sizeof(TmMetric) == 0x44 && sizeof(TmPending) == 0x48 && sizeof(TmSet) == 0x1004, "telemetry");

}  // namespace

// ---- statics --------------------------------------------------------------------------------------------------------
// res.obj
#define g_res_multi     (*(volatile int32_t*)0x004eae3c)           // MultiBegin("Resource")
#define g_res_sets      (*(ResourceSetNode* volatile*)0x004eae40)  // the set list, newest first
#define g_res_began     (*(volatile uint8_t*)0x004eae44)
#define g_res_try       (*(volatile uint8_t*)0x004eae48)           // ResourceTry: no "returning NULL" report
#define g_dir1_len      (*(volatile int32_t*)0x00509128)
#define g_dir1          ((char*)0x00509130)                         // [0x104], ResourceBegin's first argument
#define g_dir2          ((char*)0x00509238)                         // [0x104], its second
#define g_dir2_len      (*(volatile int32_t*)0x0050933c)
// locale.obj
#define g_xl_cookie     (*(volatile uint32_t*)0x004eb108)          // Xlator::g_cookie
#define g_xl_warn       (*(volatile uint8_t*)0x004eb10c)           // report keys that don't translate
#define g_units_metric  0x004eb110u                                 // LocaleUnits (0x34)
#define g_units_english 0x004eb148u
#define g_locale        (*(char* volatile*)0x00509354)             // `locale`: the LocaleInfo (0x509358)
#define g_decimal       (*(volatile char*)0x00509358)
#define g_locale_scale  (*(volatile uint32_t*)0x0050935c)          // 1.0f
#define g_locale_units  ((void*)0x00509360)                         // a copy of one LocaleUnits
#define g_metric        (*(volatile uint8_t*)0x00509390)
#define g_mon_sep       ((char*)0x00509394)                         // [2] the currency decimal separator
#define g_langs         ((LangInfo*)0x00509438)                     // [8]
#define g_nlang         (*(volatile int32_t*)0x00509638)
#define g_cur_lang      (*(volatile int32_t*)0x0050963c)
#define g_lang_res      (*(LangResource* volatile*)0x00509640)
#define g_locale_section (*(const char* const*)0x004db470)         // -> "GLOBAL"
// opt.obj
#define g_opt_multi     (*(volatile int32_t*)0x004f53f8)
#define g_opt_path      ((char*)0x00559f58)                         // <user dir>options.cfg
#define g_opt_count     (*(volatile int32_t*)0x0055a05c)
#define g_opt_items     ((OptionsItem*)0x0055a078)                  // [0x100]
#define g_opt_global    (*(const char* const*)0x004dd694)          // -> "GLOBAL", the section before any [..]
#define g_opt_gx        (*(const char* const*)0x004dd698)          // -> "GX"
#define g_opt_control   (*(const char* const*)0x004dd69c)          // -> "CONTROL"
// tmetry.obj
#define g_tm_multi      (*(volatile int32_t*)0x004f56c0)
#define g_tm_nmetric    (*(volatile int32_t*)0x00563080)
#define g_tm_metrics    ((TmMetric*)0x00563084)
#define g_tm_npending   (*(volatile int32_t*)0x0056b888)
#define g_tm_pending    ((TmPending*)0x0056b88c)
#define g_tm_sets       ((TmSet*)0x00574890)

static const char* S(uint32_t a) { return (const char*)(uintptr_t)a; }   // one of the game's own strings

// ---- the functions they call, by address -----------------------------------------------------------------------------
typedef void(__cdecl* Log_t)(const char*, ...);
static const Log_t LogPanic = (Log_t)0x004112b0;
static const Log_t LogReport = (Log_t)0x00411150;
typedef void(__cdecl* AssertMsg_t)(int, const char*, ...);
static const AssertMsg_t ASSERT_MSG_res = (AssertMsg_t)0x0041a1d0;         // bare rets in this build
static const AssertMsg_t ASSERT_MSG_opt = (AssertMsg_t)0x00471970;
typedef int(__cdecl* MultiBegin_t)(const char*);
static const MultiBegin_t MultiBegin = (MultiBegin_t)0x004150a0;
typedef void(__cdecl* Multi3_t)(int, const char*, int);
static const Multi3_t MultiEnter = (Multi3_t)0x00415180;                   // _MultiEnter(handle, file, line)
static const Multi3_t MultiLeave = (Multi3_t)0x004151d0;
static const Multi3_t MultiEnd = (Multi3_t)0x00415220;
typedef void*(__cdecl* MemAlloc_t)(int);
static const MemAlloc_t MemAlloc = (MemAlloc_t)0x004140e0;
typedef void(__cdecl* Delete_t)(void*);
static const Delete_t op_delete = (Delete_t)0x00414390;                     // operator delete -> MemFree
// the file layer
typedef int(__cdecl* FileOpen_t)(const char*);
static const FileOpen_t FileOpen = (FileOpen_t)0x00411780;
static const FileOpen_t FileCreate = (FileOpen_t)0x004115f0;
typedef void(__cdecl* FileClose_t)(int*);                                  // FileClose(int&): closes and zeroes it
static const FileClose_t FileClose = (FileClose_t)0x00411850;
typedef uint8_t(__cdecl* FileReadExact_t)(int, void*, int);
static const FileReadExact_t FileReadExact = (FileReadExact_t)0x004118b0;
typedef uint8_t(__cdecl* FileWrite_t)(int, const void*, int);
static const FileWrite_t FileWrite = (FileWrite_t)0x00411a30;
typedef uint8_t(__cdecl* FileReadLine_t)(int, char*, int);
static const FileReadLine_t FileReadLine = (FileReadLine_t)0x004119b0;
typedef int(__cdecl* FileSize_t)(int);
static const FileSize_t FileSize = (FileSize_t)0x00411890;
// FileMemoryMap FileCreateMemoryMap(int): a struct returned through a hidden pointer, which it also returns
typedef FileMemoryMap*(__cdecl* FileCreateMemoryMap_t)(FileMemoryMap*, int);
static const FileCreateMemoryMap_t FileCreateMemoryMap = (FileCreateMemoryMap_t)0x00411f30;
typedef void(__cdecl* FileDestroyMemoryMap_t)(FileMemoryMap*);
static const FileDestroyMemoryMap_t FileDestroyMemoryMap = (FileDestroyMemoryMap_t)0x00412000;
typedef void*(__cdecl* FileFindFirst_t)(const char*, char*, int);
static const FileFindFirst_t FileFindFirst = (FileFindFirst_t)0x00411c20;
typedef uint8_t(__cdecl* FileFindNext_t)(void*, char*, int);
static const FileFindNext_t FileFindNext = (FileFindNext_t)0x00411c80;
typedef void(__cdecl* FileFindClose_t)(void*);
static const FileFindClose_t FileFindClose = (FileFindClose_t)0x00411cd0;
typedef const char*(__cdecl* Str_t)();
static const Str_t Win32GetUserDirectory = (Str_t)0x00412cc0;
static const Str_t Win32GetErrorString = (Str_t)0x00416070;
typedef uint8_t(__cdecl* Flag_t)();
static const Flag_t JoyIsPresent = (Flag_t)0x00418fc0;
typedef int(__cdecl* Int_t)();
static const Int_t VidGetMegsRam = (Int_t)0x00454940;
// the game's C runtime
typedef int(__cdecl* Stricmp_t)(const char*, const char*);
static const Stricmp_t stricmp_o = (Stricmp_t)0x004da350;
typedef char*(__cdecl* Strchr_t)(const char*, int);
static const Strchr_t strchr_o = (Strchr_t)0x004ce0c0;
static const Strchr_t strrchr_o = (Strchr_t)0x004cf130;
typedef int(__cdecl* Sprintf_t)(char*, const char*, ...);
static const Sprintf_t sprintf_o = (Sprintf_t)0x004cf0a0;
typedef int(__cdecl* Sscanf_t)(const char*, const char*, ...);
static const Sscanf_t sscanf_o = (Sscanf_t)0x004ce190;
typedef char*(__cdecl* Strncpy_t)(char*, const char*, uint32_t);
static const Strncpy_t strncpy_o = (Strncpy_t)0x004cf3a0;
typedef int(__cdecl* Atoi_t)(const char*);
static const Atoi_t atoi_o = (Atoi_t)0x004cf990;
typedef double(__cdecl* Atof_t)(const char*);                             // ST0
static const Atof_t atof_o = (Atof_t)0x004cfe40;
typedef int(__cdecl* Cmp_t)(const void*, const void*);
typedef void*(__cdecl* Bsearch_t)(const void*, const void*, uint32_t, uint32_t, Cmp_t);
static const Bsearch_t bsearch_o = (Bsearch_t)0x004cfca0;
typedef void*(__cdecl* Memmove_t)(void*, const void*, uint32_t);
static const Memmove_t memmove_o = (Memmove_t)0x004cf400;
typedef char*(__cdecl* Fullpath_t)(char*, const char*, uint32_t);
static const Fullpath_t fullpath_o = (Fullpath_t)0x004cfbe0;
// Windows, through the game's import slots (call dword ptr [slot])
typedef int(__stdcall* GetLocaleInfoA_t)(uint32_t, uint32_t, char*, int);
typedef int(__stdcall* GetCurrencyFormatA_t)(uint32_t, uint32_t, const char*, const void*, char*, int);
typedef int(__stdcall* GetDateFormatA_t)(uint32_t, uint32_t, const LocaleTime*, const char*, char*, int);
#define IAT_GetLocaleInfoA    (*(GetLocaleInfoA_t volatile*)0x005d74f0)
#define IAT_GetCurrencyFormatA (*(GetCurrencyFormatA_t volatile*)0x005d74f4)
#define IAT_GetDateFormatA    (*(GetDateFormatA_t volatile*)0x005d74f8)
// this group's own functions (so the hooked rewrite, or the original, is what runs)
typedef void(__fastcall* FileInfoFn_t)(FileInfo*, Edx);
static const FileInfoFn_t FileInfo_map = (FileInfoFn_t)0x004195c0;
static const FileInfoFn_t FileInfo_unmap = (FileInfoFn_t)0x004195f0;
static const FileInfoFn_t FileInfo_unload = (FileInfoFn_t)0x00419600;
typedef uint8_t(__fastcall* IsSingleton_t)(const ResourceTOCEntry*, Edx);
static const IsSingleton_t is_singleton_o = (IsSingleton_t)0x00419620;
typedef uint8_t(__cdecl* NameFlag_t)(const char*);
static const NameFlag_t load_resource_set_name = (NameFlag_t)0x00419720;
typedef uint8_t(__cdecl* LoadFile_t)(int, const char*);
static const LoadFile_t load_resource_set_file = (LoadFile_t)0x00419840;
typedef void(__cdecl* NameFn_t)(const char*);
static const NameFn_t ResourceSetMinimize_o = (NameFn_t)0x00419b10;       // apply_to_all_sets' callbacks
static const NameFn_t ResourceSetMaximize_o = (NameFn_t)0x00419a80;
typedef void(__cdecl* ApplyAll_t)(void*);
static const ApplyAll_t apply_to_all_sets_o = (ApplyAll_t)0x00419b70;
typedef void*(__cdecl* ResourceGet_t)(const char*, uint32_t, uint32_t*, int32_t*, uint8_t*, uint8_t*);
static const ResourceGet_t ResourceGet_o = (ResourceGet_t)0x00419fa0;
static const ResourceGet_t ResourceTry_o = (ResourceGet_t)0x00419ce0;
typedef void(__cdecl* Split_t)(char*, char*, const char*);
static const Split_t split_res_path_o = (Split_t)0x00419e60;
static const NameFlag_t FileExists_o = (NameFlag_t)0x00419f00;
typedef ResourceSetNode*(__cdecl* Hunt_t)(const char*);
static const Hunt_t hunt_for_resource_o = (Hunt_t)0x0041a1e0;
typedef uint8_t(__cdecl* Forget_t)(void*);
static const Forget_t ResourceForget_o = (Forget_t)0x0041a450;
static const NameFn_t destroy_hunted_name_o = (NameFn_t)0x0041a4d0;
typedef void(__cdecl* DestroyNode_t)(ResourceSetNode*);
static const DestroyNode_t destroy_hunted_node_o = (DestroyNode_t)0x0041a520;
typedef void*(__cdecl* OneShot_t)(void*, const char*, ...);
static const OneShot_t OneShot_o = (OneShot_t)0x0041a620;
typedef void(__cdecl* Void_t)();
static const Void_t enumerate_language_resources_o = (Void_t)0x0041a910;
typedef LangResource*(__cdecl* GetRes_t)(const char*);
static const GetRes_t get_resource_o = (GetRes_t)0x0041aa10;
typedef void(__cdecl* Fixup_t)(LangResource*);
static const Fixup_t fixup_res_o = (Fixup_t)0x0041aa90;
static const NameFlag_t set_lang_by_name_o = (NameFlag_t)0x0041aab0;
typedef void(__cdecl* IntFn_t)(int);
static const IntFn_t LocaleSetLang_o = (IntFn_t)0x0041acc0;
static const Void_t reset_units_o = (Void_t)0x0041ad20;
typedef const char*(__cdecl* Lookup_t)(const char*);
static const Lookup_t lookup_o = (Lookup_t)0x0041aef0;
static const Cmp_t map_compare_o = (Cmp_t)0x0041af40;                      // bsearch's comparator
static const Void_t Xlator_InvalidateCache_o = (Void_t)0x0041afa0;
typedef void(__fastcall* XlatorFn_t)(Xlator*, Edx);
static const XlatorFn_t Xlator_xlate_o = (XlatorFn_t)0x0041afb0;
typedef uint8_t(__cdecl* LoadOptions_t)(const char*, uint8_t);
static const LoadOptions_t load_options_o = (LoadOptions_t)0x004715b0;
typedef uint8_t(__cdecl* FindItem_t)(OptionsItem**, const char*, const char*);
static const FindItem_t find_item_o = (FindItem_t)0x00471810;
static const Void_t OptionsFlush_o = (Void_t)0x00471110;
typedef void(__cdecl* SetF_t)(const char*, const char*, uint32_t);        // OptionsSet(float), the float as bits
static const SetF_t OptionsSet_f_o = (SetF_t)0x004713a0;
typedef void(__cdecl* SetI_t)(const char*, const char*, int);
static const SetI_t OptionsSet_i_o = (SetI_t)0x00471430;
typedef void(__cdecl* SetB_t)(const char*, const char*, uint8_t);
static const SetB_t OptionsSet_b_o = (SetB_t)0x004714c0;
typedef void(__cdecl* SetS_t)(const char*, const char*, const char*);
static const SetS_t OptionsSet_s_o = (SetS_t)0x00471560;
typedef void(__cdecl* GetS_t)(const char*, const char*, char*, int);
static const GetS_t OptionsGet_s_o = (GetS_t)0x00471500;

static const uint32_t k_tsr = 0x52535430;   // "0TSR"
static const uint32_t k_ser = 0x52455330;   // "0SER"
static const uint32_t k_igm = 0x4d474921;   // "!IGM"
static const uint32_t k_lang = 0x4c414e47;  // 'LANG'

// ======================================================================================================================
// res.obj
// ======================================================================================================================

// ResourceSetNode::FileInfo::map (0x4195c0): map the set's file
static void __fastcall FileInfo_map_rw(FileInfo* self, Edx) {
    FileMemoryMap tmp;
    FileMemoryMap* m = FileCreateMemoryMap(&tmp, self->fh);
    self->map.ptr = m->ptr;
    self->map.handle = m->handle;
    self->map.fake = m->fake;
}
static void fp_file_map(Footprint& f, FileInfo*, Edx) { f.replay_only = "it maps a resource set's file"; }
PORT_FN(0x004195c0, "ResourceSetNode::FileInfo::map", FileInfo_map_rw, fp_file_map)

// ::unmap (0x4195f0)
static void __fastcall FileInfo_unmap_rw(FileInfo* self, Edx) {
    if (self->map.ptr) FileDestroyMemoryMap(&self->map);
}
static void fp_file_unmap(Footprint& f, FileInfo*, Edx) { f.replay_only = "it unmaps a resource set's file"; }
PORT_FN(0x004195f0, "ResourceSetNode::FileInfo::unmap", FileInfo_unmap_rw, fp_file_unmap)

// ::unload (0x419600): close the set's file
static void __fastcall FileInfo_unload_rw(FileInfo* self, Edx) {
    if (self->fh) {
        FileClose(&self->fh);
        *(volatile int32_t*)&self->fh = 0;
    }
}
static void fp_file_unload(Footprint& f, FileInfo*, Edx) { f.replay_only = "it closes a resource set's file"; }
PORT_FN(0x00419600, "ResourceSetNode::FileInfo::unload", FileInfo_unload_rw, fp_file_unload)

// ResourceTOCEntry::is_singleton (0x419620): the byte after the name's terminator is '*' (hunt_for_resource's mark)
static uint8_t __fastcall is_singleton_rw(const ResourceTOCEntry* self, Edx) {
    return self->name[i_strlen(self->name) + 1] == '*';
}
static void fp_pure_entry(Footprint& f, const ResourceTOCEntry*, Edx) { f.pure = true; }
PORT_FN(0x00419620, "ResourceTOCEntry::is_singleton", is_singleton_rw, fp_pure_entry)

// ResourceBegin (0x419640): the two resource directories, absolute, with a '\' on the end
static uint8_t __cdecl ResourceBegin_rw(const char* dir1, const char* dir2) {
    g_res_multi = MultiBegin(S(0x004eae4c));                        // "Resource"
    memset(g_dir2, 0, 0x104);
    memset(g_dir1, 0, 0x104);
    if (fullpath_o(g_dir2, dir2, 0x104)) {
        int32_t n = (int32_t)i_strlen(g_dir2);
        g_dir2_len = n;
        if (g_dir2[n - 1] != '\\') {
            g_dir2[n] = '\\';
            g_dir2_len = n + 1;
        }
        if (fullpath_o(g_dir1, dir1, 0x104)) {
            n = (int32_t)i_strlen(g_dir1);
            g_dir1_len = n;
            if (g_dir1[n - 1] != '\\') {
                g_dir1[n] = '\\';
                g_dir1_len = n + 1;
            }
            g_res_began = 1;
        }
    }
    return g_res_began;
}
static void fp_resource_begin(Footprint& f, const char*, const char*) { f.replay_only = "MultiBegin makes the resource lock"; }
PORT_FN(0x00419640, "ResourceBegin", ResourceBegin_rw, fp_resource_begin)

// ResourceSetAttemptLoad (0x419710)
static uint8_t __cdecl ResourceSetAttemptLoad_rw(const char* name) { return load_resource_set_name(name); }
static void fp_load_set(Footprint& f, const char*) { f.replay_only = "it opens, reads and maps a resource set"; }
PORT_FN(0x00419710, "ResourceSetAttemptLoad", ResourceSetAttemptLoad_rw, fp_load_set)

// load_resource_set(char const*) (0x419720): a set already loaded counts one more load; else dir1 + name, then
// dir2 + name (each directory's buffer has the name appended and cut off again)
static uint8_t __cdecl load_resource_set_name_rw(const char* name) {
    for (ResourceSetNode* n = g_res_sets; n; n = n->next)
        if (!stricmp_o(name, n->name)) {
            ((volatile ResourceSetNode*)n)->loads++;
            return 1;
        }
    i_strcat(g_dir1, name);
    int32_t fh = FileOpen(g_dir1);
    g_dir1[g_dir1_len] = 0;
    if (!fh) {
        i_strcat(g_dir2, name);
        fh = FileOpen(g_dir2);
        g_dir2[g_dir2_len] = 0;
    }
    if (!fh) return 0;
    uint8_t ok = load_resource_set_file(fh, name);
    if (!ok) FileClose(&fh);
    return ok;
}
PORT_FN(0x00419720, "load_resource_set(name)", load_resource_set_name_rw, fp_load_set)

// load_resource_set(int, char const*) (0x419840): the header, the node, the TOC block (the first hdr_size - 16
// bytes: the TOC and the preloaded payloads, each preloaded payload's head pointed back at its entry), the map, and
// every other entry pointed into the map. FileReadExact's answer for the TOC block is not checked.
static uint8_t __cdecl load_resource_set_file_rw(int fh, const char* name) {
    char shots[5][0x200];                                           // the OneShot timers (empty in this build)
    uint32_t hdr[4];
    OneShot_o(shots[0], S(0x004eae58), name);                       // "Loading %s"
    OneShot_o(shots[1], S(0x004eae64), name);                       // "Reading header %s"
    if (!FileReadExact(fh, hdr, 0x10)) return 0;
    if (hdr[0] != k_tsr) return 0;
    OneShot_o(shots[2], S(0x004eae78), name);                       // "Alloc1 %s"
    ResourceSetNode* node = (ResourceSetNode*)MemAlloc(0x3c);
    if (node) {
        node->file.map.ptr = 0;
        *(uint8_t*)&node->file.map.fake = 0;
        node->file.fh = 0;
    } else {
        node = 0;
    }
    i_strcpy(node->name, name);                                     // (a null node faults here, as the original)
    OneShot_o(shots[3], S(0x004eae94), name);                       // "Alloc2 %s"
    int32_t toc_bytes = (int32_t)(hdr[3] - 0x10);
    ResourceTOCEntry* toc = (ResourceTOCEntry*)MemAlloc(toc_bytes);
    ((volatile ResourceSetNode*)node)->toc = toc;
    OneShot_o(shots[4], S(0x004eaeb0), name);                       // "Reading %s"
    FileReadExact(fh, node->toc, toc_bytes);
    volatile uint32_t* h = (volatile uint32_t*)&node->magic;
    h[0] = hdr[0];
    h[1] = hdr[1];
    h[2] = hdr[2];
    h[3] = hdr[3];
    ((volatile ResourceSetNode*)node)->loads = 1;
    ((volatile ResourceSetNode*)node)->file.fh = fh;
    FileMemoryMap tmp;
    FileMemoryMap* m = FileCreateMemoryMap(&tmp, fh);
    volatile FileMemoryMap* nm = &node->file.map;
    nm->ptr = m->ptr;
    nm->handle = m->handle;
    nm->fake = m->fake;
    uint32_t off = ((volatile ResourceSetNode*)node)->count * 0x24;
    OneShot_o(shots[4], S(0x004eaebc), name);                       // "Fixing up %s"
    uint32_t at = 0;                                                // i * 0x24
    for (uint32_t i = 0; hdr[1] > i; i++, at += 0x24) {
        if (hdr[2] > i) {
            uint8_t* t = (uint8_t*)((volatile ResourceSetNode*)node)->toc;
            *(uint8_t* volatile*)(t + at + 0x20) = off + t;
            t = (uint8_t*)((volatile ResourceSetNode*)node)->toc + at;
            **(uint8_t* volatile* volatile*)(t + 0x20) = t;         // the payload's head -> its entry
        } else {
            uint8_t* p = ((volatile ResourceSetNode*)node)->file.map.ptr;
            *(uint8_t* volatile*)((uint8_t*)((volatile ResourceSetNode*)node)->toc + at + 0x20) = p + off + 0x10;
        }
        off += *(volatile uint32_t*)((uint8_t*)((volatile ResourceSetNode*)node)->toc + at + 0x18) + 8;
    }
    ((volatile ResourceSetNode*)node)->next = g_res_sets;
    g_res_sets = node;
    return 1;
}
static void fp_load_set_file(Footprint& f, int, const char*) { f.replay_only = "it reads, allocates and maps a resource set"; }
PORT_FN(0x00419840, "load_resource_set(file)", load_resource_set_file_rw, fp_load_set_file)

// ResourceSetMustLoad (0x419a30)
static void __cdecl ResourceSetMustLoad_rw(const char* name) {
    MultiEnter(g_res_multi, 0, 0);
    if (!load_resource_set_name(name)) LogPanic(S(0x004eaecc), name);   // "Can't load resource set \"%s\""
    MultiLeave(g_res_multi, 0, 0);
}
static void fp_must_load(Footprint& f, const char*) { f.replay_only = "it opens, reads and maps a resource set"; }
PORT_FN(0x00419a30, "ResourceSetMustLoad", ResourceSetMustLoad_rw, fp_must_load)

// ResourceSetMaximize (0x419a80): map the first set of that name again, and repoint its mapped entries
static void __cdecl ResourceSetMaximize_rw(const char* name) {
    ResourceSetNode* n;
    for (n = g_res_sets; n; n = n->next)
        if (!stricmp_o(name, n->name)) break;
    if (!n) {
        LogPanic(S(0x004eaeec), name);                              // "ResourceSetMaximize: Can't find %s"
        return;
    }
    volatile ResourceSetNode* v = n;
    FileInfo_map(&n->file, 0);
    uint32_t count = v->count;
    uint32_t off = count * 0x24 + 0x10;
    if (!count) return;
    uint32_t i = 0, at = 0;
    do {
        if (!(v->preload > i)) *(uint8_t* volatile*)((uint8_t*)v->toc + at + 0x20) = v->file.map.ptr + off;
        at += 0x24;
        i++;
        off += (*(volatile uint32_t*)((uint8_t*)v->toc + at - 0xc) & 0x7fffffff) + 8;
    } while (v->count > i);
}
static void fp_set_maximize(Footprint& f, const char*) { f.replay_only = "it maps a resource set's file"; }
PORT_FN(0x00419a80, "ResourceSetMaximize", ResourceSetMaximize_rw, fp_set_maximize)

// ResourceSetMinimize (0x419b10): unmap it (its mapped entries keep pointing into the old view)
static void __cdecl ResourceSetMinimize_rw(const char* name) {
    ResourceSetNode* n;
    for (n = g_res_sets; n; n = n->next)
        if (!stricmp_o(name, n->name)) {
            FileInfo_unmap(&n->file, 0);
            return;
        }
    LogPanic(S(0x004eaf10), name);                                  // "ResourceSetMinimize: Can't find %s"
}
static void fp_set_minimize(Footprint& f, const char*) { f.replay_only = "it unmaps a resource set's file"; }
PORT_FN(0x00419b10, "ResourceSetMinimize", ResourceSetMinimize_rw, fp_set_minimize)

// ResourceMinimizeAll (0x419b60) / ResourceMaximizeAll (0x419ba0): the ORIGINAL's addresses as the callbacks
static void __cdecl ResourceMinimizeAll_rw() { apply_to_all_sets_o((void*)ResourceSetMinimize_o); }
static void fp_all_sets(Footprint& f) { f.replay_only = "it maps or unmaps every resource set's file"; }
PORT_FN(0x00419b60, "ResourceMinimizeAll", ResourceMinimizeAll_rw, fp_all_sets)
static void __cdecl ResourceMaximizeAll_rw() { apply_to_all_sets_o((void*)ResourceSetMaximize_o); }
PORT_FN(0x00419ba0, "ResourceMaximizeAll", ResourceMaximizeAll_rw, fp_all_sets)

// apply_to_all_sets (0x419b70): every set that isn't a hunted singleton, by its name
static void __cdecl apply_to_all_sets_rw(void* fn) {       // void(__cdecl*)(char const*)
    for (ResourceSetNode* n = g_res_sets; n; n = ((volatile ResourceSetNode*)n)->next)
        if (!is_singleton_o(((volatile ResourceSetNode*)n)->toc, 0)) ((NameFn_t)fn)(n->name);
}
static void fp_apply_all(Footprint& f, void*) { f.replay_only = "it runs a callback on every resource set"; }
PORT_FN(0x00419b70, "apply_to_all_sets", apply_to_all_sets_rw, fp_apply_all)

// ResourceSetUnload (0x419bb0): one load less; the last frees the TOC block, unmaps, closes and frees the node
// (reporting the preloaded entries still referenced -- the mapped ones aren't checked)
static void __cdecl ResourceSetUnload_rw(const char* name) {
    MultiEnter(g_res_multi, 0, 0);
    ResourceSetNode* prev = 0;
    ResourceSetNode* n;
    for (n = g_res_sets; n; prev = n, n = n->next)
        if (!stricmp_o(name, n->name)) break;
    if (!n) {
        LogReport(S(0x004eaf64), name);                             // "Tried to unload absent resource set \"%s\""
        MultiLeave(g_res_multi, 0, 0);
        return;
    }
    volatile ResourceSetNode* v = n;
    int32_t loads = v->loads;
    if (loads > 1) {
        v->loads = loads - 1;
        MultiLeave(g_res_multi, 0, 0);
        return;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; v->preload > i; i++, at += 0x24) {
        ResourceTOCEntry* e = (ResourceTOCEntry*)((uint8_t*)v->toc + at);
        int32_t refs = ((volatile ResourceTOCEntry*)e)->refs;
        if (refs) LogReport(S(0x004eaf34), name, e, refs);          // "ResourceSetUnload(\"%s\"): \"%s\" still used by %d"
    }
    op_delete(v->toc);
    if (prev) ((volatile ResourceSetNode*)prev)->next = v->next;
    else g_res_sets = v->next;
    FileInfo_unmap(&n->file, 0);
    FileInfo_unload(&n->file, 0);
    op_delete(n);
    MultiLeave(g_res_multi, 0, 0);
}
static void fp_set_unload(Footprint& f, const char*) { f.replay_only = "it frees a resource set, unmaps and closes its file"; }
PORT_FN(0x00419bb0, "ResourceSetUnload", ResourceSetUnload_rw, fp_set_unload)

// ---- the lookup, as ResourceGet makes it, for the footprints (read only) ---------------------------------------------
// the entry ResourceGet would find for `path` (0: it would hunt a file, or the path overruns its buffers)
static ResourceTOCEntry* fp_find_entry(const char* path) {
    size_t len = strlen(path);
    if (len >= 0x40) return 0;                                      // overruns ResourceGet's own buffers
    char set[0x40], res[0x40];
    memcpy(set, path, len + 1);
    char* slash = strrchr(set, '/');
    if (slash) {
        *slash = 0;
        memcpy(res, slash + 1, strlen(slash + 1) + 1);
    } else {
        set[0] = 0;
        memcpy(res, path, len + 1);
    }
    if (strlen(res) >= 0x20 || res[0] == '*' || res[0] == '~') return 0;
    for (ResourceSetNode* n = g_res_sets; n; n = n->next) {
        if (set[0] && stricmp_o(set, n->name)) continue;
        for (int32_t i = 0; i < (int32_t)n->count; i++)
            if (!stricmp_o(res, n->toc[i].name)) return &n->toc[i];
    }
    return 0;
}
static void fp_resource_get(Footprint& f, const char* path, uint32_t, uint32_t* ver, int32_t* size, uint8_t* first, uint8_t* fresh) {
    ResourceTOCEntry* e = fp_find_entry(path);
    if (!e) {
        f.replay_only = "the resource would be hunted for as a file (or its name overruns ResourceGet's buffers)";
        return;
    }
    f.add(e, sizeof *e, "TOC entry");
    f.add((void*)0x004eae48, 1, "ResourceTry flag");
    if (ver) f.add(ver, 4, "version");
    if (size) f.add(size, 4, "size");
    if (first) f.add(first, 1, "first");
    if (fresh) f.add(fresh, 1, "fresh");
}

// ResourceTry (0x419ce0): ResourceGet without the "returning NULL" report
static void* __cdecl ResourceTry_rw(const char* path, uint32_t type, uint32_t* ver, int32_t* size, uint8_t* first, uint8_t* fresh) {
    g_res_try = 1;
    return ResourceGet_o(path, type, ver, size, first, fresh);
}
PORT_FN(0x00419ce0, "ResourceTry", ResourceTry_rw, fp_resource_get)

// ResourceExists (0x419d10): '*' a file (FileExists: from the working directory, not the resource directories), '~'
// a file in the user directory; else a name in a set (never hunted). The locals keep the original's layout: the
// set name's 32 bytes run into the resource name's, then the path buffer.
static uint8_t __cdecl ResourceExists_rw(const char* path) {
    struct { char set[0x20]; char res[0x20]; char buf[0x104]; } L;
    MultiEnter(g_res_multi, 0, 0);
    char c = path[0];
    if (c == '*') {
        uint8_t r = FileExists_o(path + 1);
        MultiLeave(g_res_multi, 0, 0);
        return r;
    }
    if (c == '~') {
        sprintf_o(L.buf, S(0x004eaf90), Win32GetUserDirectory(), path + 1);   // "%s%s"
        uint8_t r = FileExists_o(L.buf);
        MultiLeave(g_res_multi, 0, 0);
        return r;
    }
    split_res_path_o(L.set, L.res, path);
    for (ResourceSetNode* n = g_res_sets; n; n = ((volatile ResourceSetNode*)n)->next) {
        if (L.set[0] && stricmp_o(L.set, n->name)) continue;
        int32_t cnt = (int32_t)((volatile ResourceSetNode*)n)->count;
        uint32_t at = 0;
        for (int32_t i = 0; i < cnt; i++, at += 0x24)
            if (!stricmp_o(L.res, (const char*)((volatile ResourceSetNode*)n)->toc + at)) {
                MultiLeave(g_res_multi, 0, 0);
                return 1;
            }
    }
    MultiLeave(g_res_multi, 0, 0);
    return 0;
}
static void fp_resource_exists(Footprint& f, const char* path) {
    if (path[0] == '*' || path[0] == '~') f.replay_only = "it opens a file to see if it exists";
    else if (strlen(path) >= 0x20) f.replay_only = "the path overruns ResourceExists' set-name buffer";
}
PORT_FN(0x00419d10, "ResourceExists", ResourceExists_rw, fp_resource_exists)

// split_res_path (0x419e60): "set/res" -> set, res; "res" -> "", res (the whole path copied into `set` first)
static void __cdecl split_res_path_rw(char* set, char* res, const char* path) {
    i_strcpy(set, path);
    char* p = strrchr_o(set, '/');
    if (p) {
        *p = 0;
        i_strcpy(res, p + 1);
    } else {
        set[0] = 0;
        i_strcpy(res, path);
    }
}
static void fp_split(Footprint& f, char* set, char* res, const char* path) {
    uint32_t n = (uint32_t)strlen(path) + 1;
    f.add(set, n, "set name");
    f.add(res, n, "resource name");
}
PORT_FN(0x00419e60, "split_res_path", split_res_path_rw, fp_split)

// FileExists (0x419f00)
static uint8_t __cdecl FileExists_rw(const char* name) {
    int32_t fh = FileOpen(name);
    if (fh) {
        FileClose(&fh);
        return 1;
    }
    return 0;
}
static void fp_file_exists(Footprint& f, const char*) { f.replay_only = "it opens a file"; }
PORT_FN(0x00419f00, "FileExists", FileExists_rw, fp_file_exists)

// ResourceTryDiscardable (0x419f40) / ResourceGetDiscardable (0x419f70)
static const void* __cdecl ResourceTryDiscardable_rw(const char* path, uint32_t type, uint32_t* ver, int32_t* size) {
    return ResourceTry_o(path, type, ver, size, 0, 0);
}
static void fp_discardable(Footprint& f, const char* path, uint32_t type, uint32_t* ver, int32_t* size) {
    fp_resource_get(f, path, type, ver, size, 0, 0);
}
PORT_FN(0x00419f40, "ResourceTryDiscardable", ResourceTryDiscardable_rw, fp_discardable)
static const void* __cdecl ResourceGetDiscardable_rw(const char* path, uint32_t type, uint32_t* ver, int32_t* size) {
    return ResourceGet_o(path, type, ver, size, 0, 0);
}
PORT_FN(0x00419f70, "ResourceGetDiscardable", ResourceGetDiscardable_rw, fp_discardable)

// ResourceGet (0x419fa0). The locals keep the original's layout: the resource name (32 bytes) below the set
// name (64), which is where the whole path is copied first.
static void* __cdecl ResourceGet_rw(const char* path, uint32_t type, uint32_t* ver, int32_t* size, uint8_t* first, uint8_t* fresh) {
    struct { const char* name; char res[0x20]; char set[0x40]; } L;
    ResourceSetNode* n;
    uint32_t i;
    MultiEnter(g_res_multi, 0, 0);
    split_res_path_o(L.set, L.res, path);
    L.name = L.res;
    if (L.res[0] == '*') {
        L.name = L.res + 1;
        goto hunt;
    }
    if (L.res[0] == '~') goto hunt;
    for (n = g_res_sets; n; n = ((volatile ResourceSetNode*)n)->next) {
        if (L.set[0] && stricmp_o(L.set, n->name)) continue;
        int32_t cnt = (int32_t)((volatile ResourceSetNode*)n)->count;
        uint32_t at = 0;
        for (i = 0; (int32_t)i < cnt; i++, at += 0x24)
            if (!stricmp_o(L.res, (const char*)((volatile ResourceSetNode*)n)->toc + at)) goto hit;
        continue;
    hit:
        if (!(((volatile ResourceSetNode*)n)->preload > i)) {
            ResourceTOCEntry* e = (ResourceTOCEntry*)((uint8_t*)((volatile ResourceSetNode*)n)->toc + i * 0x24);
            ASSERT_MSG_res(*(volatile uint32_t*)e->data == 0, S(0x004eaf98), e);   // "Discardable resource %s must set TOC to NULL"
        }
        goto found;
    }
hunt:
    i = 0;
    n = hunt_for_resource_o(L.name);
found:
    if (n) {
        volatile ResourceSetNode* v = n;
        uint32_t at = i * 0x24;
        volatile ResourceTOCEntry* e = (volatile ResourceTOCEntry*)((uint8_t*)v->toc + at);
        uint32_t et = e->type;
        if (type != et) {
            LogPanic(S(0x004eafc8), L.name, type, et);              // "ResourceGet(\"%s\"): Expected type 0x%x, got type 0x%x"
            goto fail;
        }
        uint32_t s = e->size;
        uint8_t had = (s & 0x80000000u) != 0;
        e->size = s | 0x80000000u;
        if (fresh) *fresh = had == 0;
        if (size) *size = (int32_t)(((volatile ResourceTOCEntry*)((uint8_t*)v->toc + at))->size & 0x7fffffff);
        if (first) *first = ((volatile ResourceTOCEntry*)((uint8_t*)v->toc + at))->refs == 0;
        e = (volatile ResourceTOCEntry*)((uint8_t*)v->toc + at);
        int32_t refs = e->refs;
        if (refs == 0 || *(volatile uint32_t*)e->data != 0) e->refs = refs + 1;
        *ver = ((volatile ResourceTOCEntry*)((uint8_t*)v->toc + at))->version;
        MultiLeave(g_res_multi, 0, 0);
        g_res_try = 0;
        return ((volatile ResourceTOCEntry*)((uint8_t*)v->toc + at))->data + 8;
    }
fail:
    if (!g_res_try) LogReport(S(0x004eb000), L.name);               // "ResourceGet(\"%s\") returning NULL!"
    g_res_try = 0;
    MultiLeave(g_res_multi, 0, 0);
    return 0;
}
PORT_FN(0x00419fa0, "ResourceGet", ResourceGet_rw, fp_resource_get)

// ASSERT_MSG (0x41a1d0): a bare ret in this build (the extra arguments are the caller's)
static void __cdecl ASSERT_MSG_res_rw(int, const char*) {}
static void fp_assert(Footprint& f, int, const char*) { f.pure = true; }
PORT_FN(0x0041a1d0, "ASSERT_MSG(res.obj)", ASSERT_MSG_res_rw, fp_assert)

// hunt_for_resource (0x41a1e0): a resource that is a file of its own ("0SER" type version 0 "!IGM" payload),
// made a one-entry set at the head of the list. The locals keep the original's layout: the file handle, then one
// MAX_PATH buffer that holds the '~' path and then the 12-byte header.
static ResourceSetNode* __cdecl hunt_for_resource_rw(const char* name) {
    struct { int32_t fh; union { char path[0x104]; uint32_t hdr[3]; }; } L;
    ResourceSetNode* n = 0;
    if (name[0] == '~') {
        const char* ud = Win32GetUserDirectory();
        sprintf_o(L.path, S(0x004eb024), ud, name + 1);             // "%s%s"
        L.fh = FileOpen(L.path);
    } else {
        i_strcat(g_dir1, name);
        L.fh = FileOpen(g_dir1);
        g_dir1[g_dir1_len] = 0;
        if (!L.fh) {
            i_strcat(g_dir2, name);
            L.fh = FileOpen(g_dir2);
            g_dir2[g_dir2_len] = 0;
        }
    }
    if (!L.fh) return 0;
    if (FileReadExact(L.fh, L.hdr, 0xc) && L.hdr[0] == k_ser) {
        n = (ResourceSetNode*)MemAlloc(0x3c);
        if (n) {
            n->file.map.ptr = 0;
            *(uint8_t*)&n->file.map.fake = 0;
            n->file.fh = 0;
        } else {
            n = 0;
        }
        volatile ResourceSetNode* v = n;
        *(volatile char*)v->name = 0;
        v->count = 1;
        v->toc = (ResourceTOCEntry*)MemAlloc(0x24);
        const char* base = strrchr_o(name, '\\');
        base = base ? base + 1 : name;
        i_strcpy(v->toc->name, base);
        char* nm = v->toc->name;
        nm[i_strlen(nm) + 1] = '*';                                 // the singleton mark, after the terminator
        volatile ResourceTOCEntry* e = v->toc;
        e->type = L.hdr[1];
        e->version = L.hdr[2];
        int32_t fs = FileSize(L.fh);
        v->toc->size = (uint32_t)(fs - 0x14);
        v->toc->refs = 0;
        e = v->toc;
        uint8_t* d = (uint8_t*)MemAlloc((int32_t)(e->size + 8));
        e->data = d;
        e = v->toc;
        uint8_t ok = FileReadExact(L.fh, e->data, (int32_t)(e->size + 8));
        e = v->toc;
        d = e->data;
        if (ok) {
            *(ResourceTOCEntry* volatile*)d = (ResourceTOCEntry*)e;
            v->next = g_res_sets;
            g_res_sets = n;
        } else {
            op_delete(d);
            op_delete(v->toc);
            op_delete(n);
            n = 0;
        }
    }
    FileClose(&L.fh);
    return n;
}
static void fp_hunt(Footprint& f, const char*) { f.replay_only = "it opens and reads a resource file, allocates"; }
PORT_FN(0x0041a1e0, "hunt_for_resource", hunt_for_resource_rw, fp_hunt)

// ResourceForget (0x41a450): the payload's head points back at its entry (0 for a mapped one: nothing to do);
// a singleton's last reference frees it
static uint8_t __cdecl ResourceForget_rw(void* p) {
    uint8_t* q = (uint8_t*)p;
    ASSERT_MSG_res(*(volatile uint32_t*)(q - 4) == k_igm, S(0x004eb05c), p);   // "Attempted to forget bad-cookie resource at 0x%x"
    volatile ResourceTOCEntry* e = *(ResourceTOCEntry* volatile*)(q - 8);
    if (!e) return 0;
    int32_t refs = e->refs;
    if (refs) e->refs = refs - 1;
    else LogPanic(S(0x004eb08c), e);                                // "Someone tried to forget \"%s\" too many times!"
    if (is_singleton_o((const ResourceTOCEntry*)e, 0)) {
        if (e->refs == 0) {
            destroy_hunted_name_o((const char*)e);
            return 1;
        }
        return 0;
    }
    return e->refs == 0;
}
static void fp_forget(Footprint& f, void* p) {
    ResourceTOCEntry* e = *(ResourceTOCEntry**)((uint8_t*)p - 8);
    if (!e) return;
    const char* nm = e->name;
    if (nm[strlen(nm) + 1] == '*' && (uint32_t)e->refs <= 1) f.replay_only = "a file resource's last reference frees it";
    else f.add(e, sizeof *e, "TOC entry");
}
PORT_FN(0x0041a450, "ResourceForget", ResourceForget_rw, fp_forget)

// destroy_hunted_resource(char const*) (0x41a4d0): the first hunted set whose entry has that name
static void __cdecl destroy_hunted_name_rw(const char* name) {
    for (ResourceSetNode* n = g_res_sets; n; n = n->next)
        if (n->name[0] == 0 && !stricmp_o(n->toc->name, name)) {
            destroy_hunted_node_o(n);
            return;
        }
    LogPanic(S(0x004eb0bc), name);                                  // "Singleton Resource \"%s\" had no set entry! What the?"
}
static void fp_destroy_name(Footprint& f, const char*) { f.replay_only = "it frees a file resource"; }
PORT_FN(0x0041a4d0, "destroy_hunted_resource(name)", destroy_hunted_name_rw, fp_destroy_name)

// destroy_hunted_resource(ResourceSetNode*) (0x41a520): unlink, then free the payload, the entry and the node
static void __cdecl destroy_hunted_node_rw(ResourceSetNode* node) {
    ResourceSetNode* c = g_res_sets;
    if (node == c) {
        g_res_sets = node->next;
    } else {
        c = g_res_sets;
        while (c) {
            ResourceSetNode* nx = ((volatile ResourceSetNode*)c)->next;
            if (node == nx) {
                ((volatile ResourceSetNode*)c)->next = node->next;
                break;
            }
            c = nx;
        }
    }
    ASSERT_MSG_res(c != 0, S(0x004eb0f0), c);                        // "Broske is a loser (%x)"
    op_delete(((volatile ResourceSetNode*)node)->toc->data);
    op_delete(((volatile ResourceSetNode*)node)->toc);
    op_delete(node);
}
static void fp_destroy_node(Footprint& f, ResourceSetNode*) { f.replay_only = "it frees a file resource"; }
PORT_FN(0x0041a520, "destroy_hunted_resource(node)", destroy_hunted_node_rw, fp_destroy_node)

// ResourceEnd (0x41a5a0): the lock goes; the sets stay
static void __cdecl ResourceEnd_rw() {
    int32_t h = g_res_multi;
    g_res_began = 0;
    MultiEnd(h, 0, 0);
}
static void fp_resource_end(Footprint& f) { f.replay_only = "MultiEnd frees the resource lock"; }
PORT_FN(0x0041a5a0, "ResourceEnd", ResourceEnd_rw, fp_resource_end)

// ResourceWrite (0x41a5c0): a file resource's head: "0SER" type version, then 0 "!IGM"
static void __cdecl ResourceWrite_rw(int fh, uint32_t type, uint32_t version) {
    uint32_t L[5];
    L[3] = type;
    L[4] = version;
    L[2] = k_ser;
    L[0] = 0;
    L[1] = k_igm;
    FileWrite(fh, &L[2], 0xc);
    FileWrite(fh, &L[0], 8);
}
static void fp_resource_write(Footprint& f, int, uint32_t, uint32_t) { f.replay_only = "it writes a file"; }
PORT_FN(0x0041a5c0, "ResourceWrite", ResourceWrite_rw, fp_resource_write)

// OneShot::OneShot (0x41a620): an empty profiling timer in this build (__cdecl, `this` first; returns it)
static void* __cdecl OneShot_rw(void* self, const char*) { return self; }
static void fp_oneshot(Footprint&, void*, const char*) {}   // (it returns its pointer argument: nothing to fuzz)
PORT_FN(0x0041a620, "OneShot::OneShot", OneShot_rw, fp_oneshot)

// ======================================================================================================================
// locale.obj
// ======================================================================================================================

// LocaleBegin (0x41a7f0): the languages; the options' language, else Windows' (LOCALE_SNATIVELANGNAME), else
// "English", else the first; then the measurement system, the decimal point and the currency's decimal point
static void __cdecl LocaleBegin_rw() {
    char buf[0x100];
    g_xl_warn = 1;
    enumerate_language_resources_o();
    const char* sec = g_locale_section;
    g_cur_lang = -1;
    buf[0] = 0;
    OptionsGet_s_o(sec, S(0x004eb318), buf, 0x100);                 // "language"
    GetLocaleInfoA_t gli;
    if (!set_lang_by_name_o(buf)) {
        gli = IAT_GetLocaleInfoA;
        gli(0x400, 4, buf, 0x100);                                  // LOCALE_USER_DEFAULT, LOCALE_SNATIVELANGNAME
        if (!set_lang_by_name_o(buf) && !set_lang_by_name_o(S(0x004eb324)))   // "English"
            LocaleSetLang_o(0);
    } else {
        gli = IAT_GetLocaleInfoA;
    }
    gli(0x400, 0xd, buf, 0x100);                                    // LOCALE_IMEASURE: "0" metric
    uint8_t metric = buf[0] == '0';
    g_metric = metric;
    memcpy(g_locale_units, (const void*)(uintptr_t)(metric ? g_units_metric : g_units_english), 0x34);
    gli(0x400, 0xe, buf, 0x100);                                    // LOCALE_SDECIMAL
    g_decimal = buf[0];
    gli(0x400, 0x16, g_mon_sep, 2);                                 // LOCALE_SMONDECIMALSEP
    g_locale_scale = 0x3f800000;                                    // 1.0f
}
static void fp_locale_begin(Footprint& f) { f.replay_only = "it loads the languages"; }
PORT_FN(0x0041a7f0, "LocaleBegin", LocaleBegin_rw, fp_locale_begin)

// enumerate_language_resources (0x41a910): each *.lng file that loads as a LANG resource: its file name and its
// language name (the resource's first string) into the next of the 8 LangInfos -- unbounded, both of them
static void __cdecl enumerate_language_resources_rw() {
    char name[0x80];
    g_nlang = 0;
    void* h = FileFindFirst(S(0x004eb32c), name, 0x80);             // "*.lng"
    if (h != (void*)-1) {
        do {
            LangResource* r = get_resource_o(name);
            if (r) {
                i_strcpy(g_langs[g_nlang].file, name);
                i_strcpy(g_langs[g_nlang].name, r->name);
                g_nlang++;
                ResourceForget_o(r);
            }
        } while (FileFindNext(h, name, 0x80));
        FileFindClose(h);
    }
    if (g_nlang == 0) LogPanic(S(0x004eb334));                      // "No language resources found!"
}
static void fp_enum_langs(Footprint& f) { f.replay_only = "it loads every language resource"; }
PORT_FN(0x0041a910, "enumerate_language_resources", enumerate_language_resources_rw, fp_enum_langs)

// get_resource (0x41aa10): a LANG resource, fixed up the first time it's fetched (a version other than 0 is
// reported and returned without the fix-up)
static LangResource* __cdecl get_resource_rw(const char* name) {
    uint32_t ver;
    uint8_t fresh;
    LangResource* r = (LangResource*)ResourceGet_o(name, k_lang, &ver, 0, 0, &fresh);
    if (!r) {
        LogReport(S(0x004eb368), name);                             // "Can't load %s"
        return r;
    }
    if (ver) {
        LogReport(S(0x004eb354), name);                             // "Bad version on %s"
        return r;
    }
    if (fresh) fixup_res_o(r);
    return r;
}
static void fp_get_resource(Footprint& f, const char*) { f.replay_only = "a language resource is hunted as a file"; }
PORT_FN(0x0041aa10, "get_resource", get_resource_rw, fp_get_resource)

// fixup_res (0x41aa90): the (key, text) offsets made pointers
static void __cdecl fixup_res_rw(LangResource* r) {
    volatile LangResource* v = r;
    int32_t i = 0;
    if (v->count > i) {
        volatile uint32_t* p = (volatile uint32_t*)&r->pairs[0];
        do {
            p[0] += (uint32_t)(uintptr_t)r;
            p += 2;
            i++;
            p[-1] += (uint32_t)(uintptr_t)r;
        } while (v->count > i);
    }
}
static void fp_fixup_lang(Footprint& f, LangResource* r) {
    int32_t n = r->count;
    if (n > 0 && n < 0x100000) f.add(&r->pairs[0], (uint32_t)n * 8, "language pairs");
    else if (n > 0) f.replay_only = "a huge pair count";
}
PORT_FN(0x0041aa90, "fixup_res(locale.obj)", fixup_res_rw, fp_fixup_lang)

// set_lang_by_name (0x41aab0)
static uint8_t __cdecl set_lang_by_name_rw(const char* name) {
    for (int32_t i = 0; i < g_nlang; i++)
        if (!stricmp_o(name, g_langs[i].name)) {
            LocaleSetLang_o(i);
            return 1;
        }
    return 0;
}
static void fp_set_lang_name(Footprint& f, const char*) { f.replay_only = "it loads a language resource"; }
PORT_FN(0x0041aab0, "set_lang_by_name", set_lang_by_name_rw, fp_set_lang_name)

// LocaleEnd (0x41ab00): the language goes back into the options
static void __cdecl LocaleEnd_rw() {
    if (g_lang_res) ResourceForget_o(g_lang_res);
    g_lang_res = 0;
    g_xl_warn = 0;
    int32_t cur = g_cur_lang;
    OptionsSet_s_o(g_locale_section, S(0x004eb378), g_langs[cur].name);   // "language"
}
static void fp_locale_end(Footprint& f) { f.replay_only = "it forgets the language resource"; }
PORT_FN(0x0041ab00, "LocaleEnd", LocaleEnd_rw, fp_locale_end)

// LocaleMoney (0x41ab50): Windows' currency format of whole units, or of cents as "%d.%d" (sic: 5 cents is ".5");
// without cents, cut at the last currency decimal point
static void __cdecl LocaleMoney_rw(char* const out, int amount, uint8_t cents) {
    char tmp[0x20];
    if (!cents) {
        sprintf_o(tmp, S(0x004eb38c), amount);                      // "%d"
    } else {
        uint32_t sgn = (uint32_t)(amount >> 31);
        int32_t mag = (int32_t)(((uint32_t)amount ^ sgn) - sgn);    // cdq; xor; sub (INT_MIN stays INT_MIN)
        int32_t rem = mag % 100;
        sprintf_o(tmp, S(0x004eb384), amount / 100, rem);           // "%d.%d"
    }
    int ok = IAT_GetCurrencyFormatA(0x400, 0, tmp, 0, out, 0x40);
    if ((uint8_t)ok && !cents) {                                    // test al, al
        char* p = strrchr_o(out, (int)*(volatile signed char*)g_mon_sep);
        if (p) *p = 0;
    }
}
static void fp_money(Footprint& f, char* const out, int, uint8_t) { f.add(out, 0x40, "money text"); }
PORT_FN(0x0041ab50, "LocaleMoney", LocaleMoney_rw, fp_money)

// LocaleConvertNumeric (0x41abe0): every '.' becomes the locale's decimal point
static void __cdecl LocaleConvertNumeric_rw(char* const s) {
    if (*g_locale != '.') {
        char* p;
        while ((p = strchr_o(s, '.')) != 0) *p = *g_locale;
    }
}
static void fp_convert_numeric(Footprint& f, char* const s) { f.add(s, (uint32_t)strlen(s) + 1, "number text"); }
PORT_FN(0x0041abe0, "LocaleConvertNumeric", LocaleConvertNumeric_rw, fp_convert_numeric)

// LocaleFormatShortDate (0x41ac10): (out, n, day, month, year)
static void __cdecl LocaleFormatShortDate_rw(char* const out, int n, int day, int month, int year) {
    LocaleTime st;
    st.year = (uint16_t)year;
    st.month = (uint16_t)month;
    st.dow = 0;
    st.day = (uint16_t)day;
    st.hour = 0;
    st.minute = 0;
    st.second = 0;
    st.ms = 0;
    if (!IAT_GetDateFormatA(0x400, 1, &st, 0, out, n))             // DATE_SHORTDATE
        LogReport(S(0x004eb390), Win32GetErrorString());            // "GetDateFormat fails: %s"
}
static void fp_short_date(Footprint& f, char* const out, int n, int, int, int) {
    if (n > 0) f.add(out, (uint32_t)n, "date text");
}
PORT_FN(0x0041ac10, "LocaleFormatShortDate", LocaleFormatShortDate_rw, fp_short_date)

// LocaleGetLang (0x41ac90): -1 is the current one
static const LangInfo* __cdecl LocaleGetLang_rw(int i) {
    if (i == -1) i = g_cur_lang;
    if (i >= 0 && g_nlang > i) return &g_langs[i];
    return 0;
}
static void fp_none_i(Footprint&, int) {}
PORT_FN(0x0041ac90, "LocaleGetLang", LocaleGetLang_rw, fp_none_i)

// LocaleSetLang (0x41acc0): a second language panics (the first is forgotten if the panic returns)
static void __cdecl LocaleSetLang_rw(int i) {
    if (i < 0 || i >= g_nlang) return;
    if (g_lang_res) {
        LogPanic(S(0x004eb3a8));                                    // "Can't swap langs at runtime!"
        ResourceForget_o(g_lang_res);
    }
    g_lang_res = get_resource_o(g_langs[i].file);
    Xlator_InvalidateCache_o();
    reset_units_o();
    g_cur_lang = i;
}
static void fp_set_lang(Footprint& f, int) { f.replay_only = "it loads a language resource"; }
PORT_FN(0x0041acc0, "LocaleSetLang", LocaleSetLang_rw, fp_set_lang)

// reset_units (0x41ad20): each unit Xlator's text (translated again if the cache moved on) into the unit tables
static const uint32_t k_units[12][2] = {
    {0x00509408, 0x004eb110}, {0x005093d8, 0x004eb118}, {0x005093c8, 0x004eb120}, {0x005093f8, 0x004eb128},
    {0x00509398, 0x004eb130}, {0x00509418, 0x004eb138}, {0x00509428, 0x004eb148}, {0x00509348, 0x004eb150},
    {0x005093a8, 0x004eb158}, {0x005093b8, 0x004eb160}, {0x005093e8, 0x004eb168}, {0x00509648, 0x004eb170},
};
static void __cdecl reset_units_rw() {
    for (int k = 0; k < 12; k++) {
        volatile Xlator* x = (volatile Xlator*)(uintptr_t)k_units[k][0];
        if (x->cookie != g_xl_cookie) Xlator_xlate_o((Xlator*)x, 0);
        *(const char* volatile*)(uintptr_t)k_units[k][1] = x->value;
    }
}
static void fp_reset_units(Footprint& f) {
    for (int k = 0; k < 12; k++) f.add((void*)(uintptr_t)k_units[k][0], sizeof(Xlator), "unit Xlator");
    f.add((void*)(uintptr_t)g_units_metric, 0x68, "unit tables");
}
PORT_FN(0x0041ad20, "reset_units", reset_units_rw, fp_reset_units)

// Xlate (0x41aec0): the text, or "?!?" (reported while the locale is up)
static const char* __cdecl Xlate_rw(const char* key) {
    const char* r = lookup_o(key);
    if (!r) {
        if (g_xl_warn) LogReport(S(0x004eb3c8), key);               // "WARNING: Can't XLAT %s"
        r = S(0x004eb3e0);                                          // "?!?"
    }
    return r;
}
static void fp_none_s(Footprint&, const char*) {}
PORT_FN(0x0041aec0, "Xlate", Xlate_rw, fp_none_s)

// lookup (0x41aef0): the game's bsearch over the language's sorted pairs, with map_compare (the original's)
static const char* __cdecl lookup_rw(const char* key) {
    LangPair k;
    if (!g_lang_res) return 0;
    LangResource* r = g_lang_res;
    k.text = 0;
    k.key = key;
    LangPair* p = (LangPair*)bsearch_o(&k, &r->pairs[0], (uint32_t)r->count, 8, map_compare_o);
    if (!p) return 0;
    return p->text;
}
PORT_FN(0x0041aef0, "lookup", lookup_rw, fp_none_s)

// map_compare (0x41af40)
static int __cdecl map_compare_rw(const void* a, const void* b) {
    return stricmp_o(*(const char* const*)a, *(const char* const*)b);
}
static void fp_none_pp(Footprint&, const void*, const void*) {}
PORT_FN(0x0041af40, "map_compare", map_compare_rw, fp_none_pp)

// CouldXlate (0x41af60)
static uint8_t __cdecl CouldXlate_rw(const char* key) { return lookup_o(key) != 0; }
PORT_FN(0x0041af60, "CouldXlate", CouldXlate_rw, fp_none_s)

// Xlator::Xlator (0x41af80): not yet translated (its cookie is the current one's complement)
static Xlator* __fastcall Xlator_ctor_rw(Xlator* self, Edx, const char* key) {
    volatile Xlator* v = self;
    v->value = 0;
    v->key = key;
    v->cookie = ~g_xl_cookie;
    return self;
}
static void fp_xlator_ctor(Footprint& f, Xlator* self, Edx, const char*) { f.add(self, sizeof(Xlator), "Xlator"); }
PORT_FN(0x0041af80, "Xlator::Xlator", Xlator_ctor_rw, fp_xlator_ctor)

// Xlator::InvalidateCache (0x41afa0)
static void __cdecl Xlator_InvalidateCache_rw() { g_xl_cookie = g_xl_cookie + 1; }
static void fp_invalidate(Footprint& f) { f.add((void*)0x004eb108, 4, "Xlator::g_cookie"); }
PORT_FN(0x0041afa0, "Xlator::InvalidateCache", Xlator_InvalidateCache_rw, fp_invalidate)

// Xlator::xlate (0x41afb0): a key that doesn't translate becomes "?!?" and stays stale (tried again every time)
static void __fastcall Xlator_xlate_rw(Xlator* self, Edx) {
    volatile Xlator* v = self;
    const char* r = lookup_o(v->key);
    v->value = r;
    if (!r) {
        if (g_xl_warn) LogReport(S(0x004eb3e4), v->key);            // "WARNING: Can't XLAT %s"
        v->value = S(0x004eb3fc);                                   // "?!?"
        return;
    }
    v->cookie = g_xl_cookie;
}
static void fp_xlate(Footprint& f, Xlator* self, Edx) { f.add(self, sizeof(Xlator), "Xlator"); }
PORT_FN(0x0041afb0, "Xlator::xlate", Xlator_xlate_rw, fp_xlate)

// ======================================================================================================================
// opt.obj
// ======================================================================================================================

// OptionsBegin (0x470ee0): options.def (must load), then <user dir>options.cfg; with no options.cfg, joystick
// steering if there's a joystick, and the 2 MB graphics settings on a card with 2 MB or less
static void __cdecl OptionsBegin_rw() {
    g_opt_multi = MultiBegin(S(0x004f5434));                        // "options"
    g_opt_count = 0;
    i_strcpy(g_opt_path, Win32GetUserDirectory());
    {
        char* e = g_opt_path + i_strlen(g_opt_path);
        const volatile uint32_t* src = (const volatile uint32_t*)0x004f543c;   // "options.cfg" and its NUL: 3 dwords
        uint32_t a = src[0], b = src[1], c = src[2];
        ((volatile uint32_t*)e)[0] = a;
        ((volatile uint32_t*)e)[1] = b;
        ((volatile uint32_t*)e)[2] = c;
    }
    if (!load_options_o(S(0x004f5448), 0)) LogPanic(S(0x004f5454));   // "options.def", "Can't load options.def"
    if (load_options_o(g_opt_path, 0)) return;
    if (JoyIsPresent()) {
        LogReport(S(0x004f546c));                                   // "Setting joystick controls"
        const char* sec = g_opt_control;
        OptionsSet_s_o(sec, S(0x004f5494), S(0x004f5488));          // steer_left = Joy Left
        OptionsSet_s_o(sec, S(0x004f54ac), S(0x004f54a0));          // steer_right = Joy Right
        OptionsSet_s_o(sec, S(0x004f54c4), S(0x004f54b8));
        OptionsSet_s_o(sec, S(0x004f54dc), S(0x004f54d0));
    }
    if (VidGetMegsRam() > 2) return;
    LogReport(S(0x004f54e4));                                       // "Setting 2 meg graphics options"
    const char* sec = g_opt_gx;
    OptionsSet_f_o(sec, S(0x004f5504), 0);                          // draw_distance 0.0f
    OptionsSet_f_o(sec, S(0x004f5514), 0);
    OptionsSet_b_o(sec, S(0x004f5524), 0);
    OptionsSet_i_o(sec, S(0x004f5530), 0);
    OptionsSet_b_o(sec, S(0x004f5538), 0);
    OptionsSet_i_o(sec, S(0x004f5544), 1);
    OptionsSet_b_o(sec, S(0x004f5550), 0);
    OptionsSet_b_o(sec, S(0x004f5554), 0);
    OptionsSet_i_o(sec, S(0x004f5560), 0);
    OptionsSet_i_o(sec, S(0x004f556c), 0);
    OptionsSet_b_o(sec, S(0x004f5574), 0);
    OptionsSet_i_o(sec, S(0x004f557c), 0);
    OptionsSet_i_o(sec, S(0x004f5584), 0);
}
static void fp_options_begin(Footprint& f) { f.replay_only = "it reads the options files"; }
PORT_FN(0x00470ee0, "OptionsBegin", OptionsBegin_rw, fp_options_begin)

// OptionsLoadModule (0x4710e0): a module's options file (a wrong version is only reported)
static void __cdecl OptionsLoadModule_rw(const char* path) {
    if (!load_options_o(path, 1)) LogPanic(S(0x004f558c), path);    // "Couldn't load %s"
}
static void fp_load_module(Footprint& f, const char*) { f.replay_only = "it reads an options file"; }
PORT_FN(0x004710e0, "OptionsLoadModule", OptionsLoadModule_rw, fp_load_module)

// OptionsFlush (0x471110): every item written back to <user dir>options.cfg, a "[section]" line whenever the
// section changes. The locals keep the original's layout: the section (32 bytes) runs into the line buffer.
static void __cdecl OptionsFlush_rw() {
    struct { int32_t fh; char cur[0x20]; char buf[0x100]; } L;
    MultiEnter(g_opt_multi, 0, 0);
    L.fh = FileCreate(g_opt_path);
    if (!L.fh) {
        LogReport(S(0x004f55e4), g_opt_path);                       // "Couldn't make options file: %s"
    } else {
        L.cur[0] = *(const volatile char*)0x004f55a0;               // ""
        memset(L.cur + 1, 0, 31);
        sprintf_o(L.buf, S(0x004f55a4), 1);                         // "version %d\r\n"
        FileWrite(L.fh, L.buf, (int)i_strlen(L.buf));
        for (int32_t i = 0; g_opt_count > i; i++) {
            OptionsItem* it = &g_opt_items[i];
            if (stricmp_o(it->section, L.cur)) {
                i_strcpy(L.cur, it->section);
                sprintf_o(L.buf, S(0x004f55b4), L.cur);             // "[%s]\r\n"
                FileWrite(L.fh, L.buf, (int)i_strlen(L.buf));
            }
            switch ((uint32_t)((volatile OptionsItem*)it)->type) {
            case 0:
            case 3:
                sprintf_o(L.buf, S(0x004f55bc), it->key, it->s);    // "%s %s\r\n"
                break;
            case 1:
                sprintf_o(L.buf, S(0x004f55dc), it->key, ((volatile OptionsItem*)it)->b ? S(0x004f55d4) : S(0x004f55d8));
                break;
            case 2:
                sprintf_o(L.buf, S(0x004f55c4), it->key, ((volatile OptionsItem*)it)->i);   // "%s %d\r\n"
                break;
            case 4:
                sprintf_o(L.buf, S(0x004f55cc), it->key, (double)*(volatile float*)&it->f);   // "%s %f\r\n"
                break;
            default:
                break;                                              // the previous line is written again
            }
            FileWrite(L.fh, L.buf, (int)i_strlen(L.buf));
        }
        FileClose(&L.fh);
    }
    MultiLeave(g_opt_multi, 0, 0);
}
static void fp_options_flush(Footprint& f) { f.replay_only = "it writes the options file"; }
PORT_FN(0x00471110, "OptionsFlush", OptionsFlush_rw, fp_options_flush)

// OptionsEnd (0x471330)
static void __cdecl OptionsEnd_rw() {
    OptionsFlush_o();
    MultiEnd(g_opt_multi, 0, 0);
}
static void fp_options_end(Footprint& f) { f.replay_only = "it writes the options file and frees the lock"; }
PORT_FN(0x00471330, "OptionsEnd", OptionsEnd_rw, fp_options_end)

// the typed Get / Set overloads: a Get of an item that isn't there sets it to what the caller holds
static void fp_items(Footprint& f) {
    f.add((void*)0x0055a05c, 4, "options count");
    f.add((void*)0x0055a078, 0x100 * sizeof(OptionsItem), "options items");
    int32_t n = g_opt_count;                                        // an item appended past the table's end
    if (n >= 0x100 && n < 0x10000) f.add(&g_opt_items[n], sizeof(OptionsItem), "options item past the table");
}
static void __cdecl OptionsGet_f_rw(const char* sec, const char* key, float* v) {
    OptionsItem* it;
    if (find_item_o(&it, sec, key)) *(volatile uint32_t*)v = ((volatile OptionsItem*)it)->f;
    else OptionsSet_f_o(sec, key, *(volatile uint32_t*)v);
}
static void fp_get_f(Footprint& f, const char*, const char*, float* v) { fp_items(f); f.add(v, 4, "value"); }
PORT_FN(0x00471350, "OptionsGet(float)", OptionsGet_f_rw, fp_get_f)
static void __cdecl OptionsSet_f_rw(const char* sec, const char* key, uint32_t bits) {
    OptionsItem* it;
    find_item_o(&it, sec, key);
    ((volatile OptionsItem*)it)->f = bits;
    ((volatile OptionsItem*)it)->type = 4;
}
static void fp_set_f(Footprint& f, const char*, const char*, uint32_t) { fp_items(f); }
PORT_FN(0x004713a0, "OptionsSet(float)", OptionsSet_f_rw, fp_set_f)
static void __cdecl OptionsGet_i_rw(const char* sec, const char* key, int* v) {
    OptionsItem* it;
    if (find_item_o(&it, sec, key)) *(volatile int*)v = ((volatile OptionsItem*)it)->i;
    else OptionsSet_i_o(sec, key, *(volatile int*)v);
}
static void fp_get_i(Footprint& f, const char*, const char*, int* v) { fp_items(f); f.add(v, 4, "value"); }
PORT_FN(0x004713e0, "OptionsGet(int)", OptionsGet_i_rw, fp_get_i)
static void __cdecl OptionsSet_i_rw(const char* sec, const char* key, int v) {
    OptionsItem* it;
    find_item_o(&it, sec, key);
    ((volatile OptionsItem*)it)->i = v;
    ((volatile OptionsItem*)it)->type = 2;
}
static void fp_set_i(Footprint& f, const char*, const char*, int) { fp_items(f); }
PORT_FN(0x00471430, "OptionsSet(int)", OptionsSet_i_rw, fp_set_i)
static void __cdecl OptionsGet_b_rw(const char* sec, const char* key, uint8_t* v) {
    OptionsItem* it;
    if (find_item_o(&it, sec, key)) *(volatile uint8_t*)v = ((volatile OptionsItem*)it)->b;
    else OptionsSet_b_o(sec, key, *(volatile uint8_t*)v);
}
static void fp_get_b(Footprint& f, const char*, const char*, uint8_t* v) { fp_items(f); f.add(v, 1, "value"); }
PORT_FN(0x00471470, "OptionsGet(bool)", OptionsGet_b_rw, fp_get_b)
static void __cdecl OptionsSet_b_rw(const char* sec, const char* key, uint8_t v) {
    OptionsItem* it;
    find_item_o(&it, sec, key);
    ((volatile OptionsItem*)it)->b = v;
    ((volatile OptionsItem*)it)->type = 1;
}
static void fp_set_b(Footprint& f, const char*, const char*, uint8_t) { fp_items(f); }
PORT_FN(0x004714c0, "OptionsSet(bool)", OptionsSet_b_rw, fp_set_b)
static void __cdecl OptionsGet_s_rw(const char* sec, const char* key, char* buf, int n) {
    OptionsItem* it;
    if (find_item_o(&it, sec, key)) {
        strncpy_o(buf, it->s, (uint32_t)n);
        buf[n - 1] = 0;
    } else {
        OptionsSet_s_o(sec, key, buf);
    }
}
static void fp_get_s(Footprint& f, const char*, const char*, char* buf, int n) {
    fp_items(f);
    if (n > 0) f.add(buf, (uint32_t)n, "text");
}
PORT_FN(0x00471500, "OptionsGet(string)", OptionsGet_s_rw, fp_get_s)
static void __cdecl OptionsSet_s_rw(const char* sec, const char* key, const char* v) {
    OptionsItem* it;
    find_item_o(&it, sec, key);
    strncpy_o(it->s, v, 0x40);
    ((volatile OptionsItem*)it)->s[0x3f] = 0;
    ((volatile OptionsItem*)it)->type = 3;
}
static void fp_set_s(Footprint& f, const char*, const char*, const char*) { fp_items(f); }
PORT_FN(0x00471560, "OptionsSet(string)", OptionsSet_s_rw, fp_set_s)

// load_options (0x4715b0): "version 1" first (a module may have another), then "[section]" and "key value" lines
// (the value unbounded into the item's 64-byte text, then read as int, float and "yes"). The locals keep the
// original's layout: the version / item slot, the file, the section (32 bytes) and the line (256).
static uint8_t __cdecl load_options_rw(const char* path, uint8_t module) {
    struct { union { int32_t ver; OptionsItem* item; }; int32_t fh; char section[0x20]; char line[0x100]; } L;
    uint8_t ok = 0;
    L.fh = FileOpen(path);
    if (!L.fh) {
        LogReport(S(0x004f567c), path);                             // "Couldn't open options file: %s"
        return ok;
    }
    if (FileReadLine(L.fh, L.line, 0x100)) {
        L.ver = -1;
        if (sscanf_o(L.line, S(0x004f5604), &L.ver) == 1 && L.ver == 1) ok = 1;   // "version %d"
    }
    if (!ok) {
        if (!module) {
            LogReport(S(0x004f5610), path);                         // "%s: Wrong version"
        } else {
            ok = 1;
            LogReport(S(0x004f5624), path);                         // "%s: Ignoring wrong version"
        }
    }
    i_strcpy(L.section, g_opt_global);
    if (ok) {
        do {
            if (!FileReadLine(L.fh, L.line, 0x100)) break;
            if (L.line[0] == '[') {
                i_strcpy(L.section, L.line + 1);
                char* p = strchr_o(L.section, ']');
                if (p) *p = 0;
                else LogReport(S(0x004f5640));                      // "Expecting ] in config section"
            } else if (L.line[0] != ';' && L.line[0] != 0) {
                char* p = strchr_o(L.line, ' ');
                if (p && *p) {
                    *p = 0;
                    p++;
                    find_item_o(&L.item, L.section, L.line);
                    i_strcpy(L.item->s, p);
                    int iv = atoi_o(L.item->s);
                    ((volatile OptionsItem*)L.item)->i = iv;
                    float fv = (float)atof_o(L.item->s);
                    *(volatile float*)&L.item->f = fv;
                    uint8_t yes = stricmp_o(L.item->s, S(0x004f5660)) == 0;   // "yes"
                    ((volatile OptionsItem*)L.item)->b = yes;
                    ((volatile OptionsItem*)L.item)->type = 0;
                } else {
                    LogReport(S(0x004f5664), L.section, L.line);    // "Item %s:%s has no value"
                }
            }
        } while (ok);
    }
    FileClose(&L.fh);
    return ok;
}
static void fp_load_options(Footprint& f, const char*, uint8_t) { f.replay_only = "it reads an options file"; }
PORT_FN(0x004715b0, "load_options", load_options_rw, fp_load_options)

// find_item (0x471810): an item, or a new one (zeroed, its type left as it was) at the end of the table --
// unbounded: the 257th overwrites the telemetry after the table
static uint8_t __cdecl find_item_rw(OptionsItem** out, const char* sec, const char* key) {
    ASSERT_MSG_opt(strchr_o(key, ' ') == 0, S(0x004f569c), sec, key);   // "find_item: %s:%s has a space in it"
    MultiEnter(g_opt_multi, 0, 0);
    for (int32_t i = 0; g_opt_count > i; i++) {
        OptionsItem* it = &g_opt_items[i];
        if (!stricmp_o(sec, it->section) && !stricmp_o(key, it->key)) {
            MultiLeave(g_opt_multi, 0, 0);
            *out = it;
            return 1;
        }
    }
    i_strcpy(g_opt_items[g_opt_count].section, sec);
    i_strcpy(g_opt_items[g_opt_count].key, key);
    volatile OptionsItem* it = &g_opt_items[g_opt_count];
    it->s[0] = 0;
    it->b = 0;
    it->i = 0;
    it->f = 0;
    *out = (OptionsItem*)it;
    g_opt_count = g_opt_count + 1;
    MultiLeave(g_opt_multi, 0, 0);
    return 0;
}
static void fp_find_item(Footprint& f, OptionsItem** out, const char*, const char*) {
    fp_items(f);
    f.add(out, 4, "item");
}
PORT_FN(0x00471810, "find_item", find_item_rw, fp_find_item)

// ASSERT_MSG (0x471970): a bare ret
static void __cdecl ASSERT_MSG_opt_rw(int, const char*) {}
PORT_FN(0x00471970, "ASSERT_MSG(opt.obj)", ASSERT_MSG_opt_rw, fp_assert)

// ======================================================================================================================
// tmetry.obj
// ======================================================================================================================

// TelemetryBegin (0x4719a0)
static void __cdecl TelemetryBegin_rw() {
    g_tm_multi = MultiBegin(S(0x004f56fc));                         // "Telemetry"
    g_tm_nmetric = 0;
    g_tm_npending = 0;
    for (volatile int32_t* p = (volatile int32_t*)0x00574890; p < (volatile int32_t*)0x005788a0; p = (volatile int32_t*)((uint8_t*)p + 0x1004))
        *p = 0;
}
static void fp_tm_begin(Footprint& f) { f.replay_only = "MultiBegin makes the telemetry lock"; }
PORT_FN(0x004719a0, "TelemetryBegin", TelemetryBegin_rw, fp_tm_begin)

// TelemetryEnd (0x4719e0)
static void __cdecl TelemetryEnd_rw() { MultiEnd(g_tm_multi, 0, 0); }
static void fp_tm_end(Footprint& f) { f.replay_only = "MultiEnd frees the telemetry lock"; }
PORT_FN(0x004719e0, "TelemetryEnd", TelemetryEnd_rw, fp_tm_end)

// TelemetryCreateMetric (0x471a00): a new metric (unbounded: the 513th overwrites the pending count); the meters
// waiting for that name join their sets
static int __cdecl TelemetryCreateMetric_rw(const char* name) {
    MultiEnter(g_tm_multi, 0, 0);
    int32_t idx = g_tm_nmetric;
    i_strcpy(g_tm_metrics[idx].name, name);
    *(volatile uint32_t*)&g_tm_metrics[idx].value = 0;
    g_tm_nmetric = g_tm_nmetric + 1;
    int32_t i = 0;
    TmPending* p = g_tm_pending;
    while (i < g_tm_npending) {
        if (!stricmp_o(name, p->name)) {
            volatile TmSet* s = &g_tm_sets[((volatile TmPending*)p)->set];
            s->pairs[s->count].metric = idx;
            s->pairs[s->count].dst = ((volatile TmPending*)p)->dst;
            s->count = s->count + 1;
            int32_t t = ((g_tm_npending - i) * 8 - 8) * 9;
            t = (t / 0x48) << 3;
            memmove_o(p, p + 1, (uint32_t)(t * 9));
            g_tm_npending = g_tm_npending - 1;
        } else {
            p++;
            i++;
        }
    }
    MultiLeave(g_tm_multi, 0, 0);
    return idx;
}
static void fp_tm_create(Footprint& f, const char*) { f.add((void*)0x00563080, 0x005788a0 - 0x00563080, "telemetry tables"); }
PORT_FN(0x00471a00, "TelemetryCreateMetric", TelemetryCreateMetric_rw, fp_tm_create)

// TelemetryUpdateMetric (0x471b00): the float moved as bits
static void __cdecl TelemetryUpdateMetric_rw(int i, uint32_t bits) { *(volatile uint32_t*)&g_tm_metrics[i].value = bits; }
static void fp_tm_update(Footprint& f, int i, uint32_t) { f.add(&g_tm_metrics[i].value, 4, "metric"); }
PORT_FN(0x00471b00, "TelemetryUpdateMetric", TelemetryUpdateMetric_rw, fp_tm_update)

// TelemetrySetUpdate (0x471b20): every meter of the set gets its metric (as bits)
static void __cdecl TelemetrySetUpdate_rw(int set) {
    volatile TmSet* s = &g_tm_sets[set];
    for (int32_t i = 0; s->count > i; i++) *s->pairs[i].dst = *(volatile uint32_t*)&g_tm_metrics[s->pairs[i].metric].value;
}
static void fp_tm_set_update(Footprint& f, int set) {
    TmSet* s = &g_tm_sets[set];
    for (int32_t i = 0; i < s->count && i < 0x200; i++) f.add(s->pairs[i].dst, 4, "meter");
    if (s->count > 0x200) f.replay_only = "a set with more meters than it holds";
}
PORT_FN(0x00471b20, "TelemetrySetUpdate", TelemetrySetUpdate_rw, fp_tm_set_update)

// TelemetrySetAddMeter (0x471b70): into the set now if the metric exists, else pending (at most 0x200)
static uint8_t __cdecl TelemetrySetAddMeter_rw(int set, const char* name, float* dst) {
    MultiEnter(g_tm_multi, 0, 0);
    for (int32_t i = 0; g_tm_nmetric > i; i++)
        if (!stricmp_o(name, g_tm_metrics[i].name)) {
            volatile TmSet* s = &g_tm_sets[set];
            s->pairs[s->count].metric = i;
            s->pairs[s->count].dst = (uint32_t*)dst;
            s->count = s->count + 1;
            MultiLeave(g_tm_multi, 0, 0);
            return 1;
        }
    if (g_tm_npending >= 0x200) {
        MultiLeave(g_tm_multi, 0, 0);
        return 0;
    }
    ((volatile TmPending*)&g_tm_pending[g_tm_npending])->set = set;
    ((volatile TmPending*)&g_tm_pending[g_tm_npending])->dst = (uint32_t*)dst;
    i_strcpy(g_tm_pending[g_tm_npending].name, name);
    g_tm_npending = g_tm_npending + 1;
    MultiLeave(g_tm_multi, 0, 0);
    return 1;
}
static void fp_tm_add(Footprint& f, int, const char*, float*) { f.add((void*)0x0056b888, 0x005788a0 - 0x0056b888, "telemetry meters"); }
PORT_FN(0x00471b70, "TelemetrySetAddMeter", TelemetrySetAddMeter_rw, fp_tm_add)
