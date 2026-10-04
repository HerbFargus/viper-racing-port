// net_ded.cpp -- multiplayer stage N2, group B: the dedicated server, rewritten faithfully (library `multi`: ded.obj).
//
// `race.exe -dedicated[:port]` (WinMain) runs MultiDedicatedServer instead of the game: ded_begin opens a console (the
// log goes to it through ded_log_hook), asks three questions (the server's name, a password, whether users may change the
// settings by chat), then a SessionMgr on a UDP socket (port 2001 unless given), a RaceServer proposing the game
// init_game_info sets up, and a task ("dedicated input", idle) reading console lines into handle_input: quit, restart,
// end race, `server: ...` (handed to the main loop's Server::Tick through Server::g_msg, which chats them as user 0), help.
// The main loop: Server::Tick -- the SessionMgr's and the RaceServer's ticks, ProceedToChooseCar as soon as everyone has
// approved, SendAll while racing. Chat (the RaceServer's chat callback, chat_cb) in the approve phase is track_chat's
// settings language ("server: track 3 aicars 2 simulation long hard ai damage on reversed"); while racing, "end race" goes
// back to the chat and then -- as every other line said while racing -- RestartRace runs (known original behaviour, kept).
//
// Written from the v1.0 disassembly: every call in the original's order, by its v1.0 address (this object's own functions
// too), virtual calls through the vtable, the console through race.exe's KERNEL32 import slots (AllocConsole,
// GetStdHandle, SetConsoleMode, SetConsoleTitleA, WriteConsoleA, ReadConsoleA, FreeConsole, Sleep) read at the call, so a
// harness can stub them. No N0 site is in this object (Server::Tick's TaskSleep(50) is not the lobby's).
//
// Footprints: everything here drives the console, the log, a task or the RaceServer: replay_only, except the string
// helpers (contains, get_numeric) and the proposal's setup (init_game_info, update_laps), which write their outputs only.
//
// The fixes (docs/PORTING.md, "Fixes"; `// FIX:` in place, VP_FIX): get_numeric's digit buffer holds the 12 digits it takes
// and their NUL (the original's 12 bytes put the NUL on its return address); idle terminates a line that fills its 0x400
// bytes (the last byte gives way, as prompt_user's does; the original handed handle_input no NUL); handle_input logs
// "Unrecognized command" only for a line it doesn't know (the original logged it after every command). Left as is: the
// console reads block (ReadConsoleA) until a line comes, and handle_input waits (Sleep(50)) for the main loop to take a
// server:/restart/end race line -- the main loop clears Server::g_msg only once it has finished with the line, so giving
// up the wait could free the line under it, and the main loop runs until "quit" (the input task is destroyed then);
// MultiDedicatedServer's main loop exits only on "quit"; and any line said while racing restarts the race (see chat).
#include <stdint.h>
#include <string.h>
#include "port.h"
#include "net_types.h"
#include "net_core.h"
#include "net_server.h"

namespace {
namespace net_ded {
using namespace nsv;
using nt::ccall;
using nt::crt_copy;
using nt::crt_strlen;
using nt::tcall;
using nt::vcall;

typedef void(__cdecl* Log_t)(const char*, ...);
#define LOG ((Log_t)(uintptr_t)F_LogReport)
#define PANIC ((Log_t)(uintptr_t)F_LogPanic)
#define CP(a) ((const char*)(uintptr_t)(a))
#define U8(a) (*(volatile uint8_t*)(uintptr_t)(a))
#define U32(a) (*(volatile uint32_t*)(uintptr_t)(a))
#define IAT(T, slot) (*(T volatile*)(uintptr_t)(slot))

static const char* const R_CONSOLE = "the dedicated server's console";
static const char* const R_LOG = "logs (LogReport)";
static const char* const R_SERVER = "drives the RaceServer and the SessionMgr";

// ---- the proposal ---------------------------------------------------------------------------------------------------------
static void __fastcall init_game_info(Server* self, Edx) {
    nt::crt_zero(&self->prop, 0xf);
    volatile uint8_t* g = (volatile uint8_t*)&self->prop + 8;
    *(volatile int32_t*)(g + 0x00) = 0;
    *(volatile int32_t*)(g + 0x04) = 0;
    *(volatile int32_t*)(g + 0x08) = 0;
    *(volatile int32_t*)(g + 0x0c) = 3;
    *(volatile int32_t*)(g + 0x10) = 1;
    *(volatile int32_t*)(g + 0x18) = 0;
    *(volatile int32_t*)(g + 0x1c) = 0;
    *(volatile int32_t*)(g + 0x20) = 0;
    g[0x24] = 1;
    g[0x25] = 0;
    *(volatile int32_t*)(g + 0x28) = 0;
    *(volatile int32_t*)(g + 0x2c) = 0;
    *(volatile int32_t*)(g + 0x30) = 0;
    tcall<void>(D_update_laps, self);
}
static void fp_igi(Footprint& f, Server* self, Edx) { f.add(&self->prop, sizeof self->prop, "the proposal"); }
PORT_FN(0x004a3170, "Server::init_game_info", init_game_info, fp_igi)

static void __fastcall update_laps(Server* self, Edx) {
    const char* name = ccall<const char*>(F_GetTrackName, self->prop.track);
    self->prop.laps = ccall<int>(F_GetLapCountFromType, self->prop.race_type, name);
}
static void fp_ul(Footprint& f, Server* self, Edx) { f.add(&self->prop.laps, 4, "the laps"); }
PORT_FN(0x004a31c0, "Server::update_laps", update_laps, fp_ul)

// ---- the main loop's step ---------------------------------------------------------------------------------------------------
static void __fastcall Server_Tick(Server* self, Edx) {
    tcall<void>(F_SM_Tick, self->sm);
    vcall<void>(self->rs, RS_Tick);
    const int phase = vcall<int>(self->rs, RS_GetPhase);
    if (phase == 0 && vcall<uint8_t>(self->rs, RS_EveryoneApproved)) vcall<void>(self->rs, RS_ProceedToChooseCar);
    if (phase < 2) ccall<void>(F_TaskSleep, 0x32);
    if (phase == 3) vcall<void>(self->rs, RS_SendAll);
    if (U32(G_MSG)) {
        tcall<void>(D_chat, self, 0, (const char*)(uintptr_t)U32(G_MSG));
        U32(G_MSG) = 0;
    }
}
static void fp_stick(Footprint& f, Server*, Edx) { f.replay_only = R_SERVER; }
PORT_FN(0x004a31f0, "Server::Tick", Server_Tick, fp_stick)

// ---- chat ------------------------------------------------------------------------------------------------------------------------
// "server:" lines in the approve phase: track N (1..8), aicars N (0..6), and the settings by their values' names; any
// change re-proposes the game
static void __fastcall track_chat(Server* self, Edx, int user, const char* text) {
    (void)user;
    struct Entry { uint32_t name; const uint32_t* values; int32_t off; int32_t size; };
    const uint32_t server_kw[2] = {0x004fb5e4, 0};                                          // "server:"
    const uint32_t realism[4] = {0x004fb5ec, 0x004fb5f4, 0x004fb60c, 0};                   // arcade .. simulation
    const uint32_t race_type[5] = {0x004fb618, 0x004fb620, 0x004fb628, 0x004fb630, 0};     // sprint .. insane
    const uint32_t ai[4] = {0x004fb638, 0x004fb640, 0x004fb650, 0};                        // easy ai .. hard ai
    const uint32_t damage[3] = {0x004fb658, 0x004fb664, 0};                                // damage off / on
    const uint32_t reversed[3] = {0x004fb670, 0x004fb678, 0};                              // forward / reversed
    const Entry tab[5] = {{0x004fb684, realism, 8, 4}, {0x004fb68c, race_type, 0x18, 4}, {0x004fb698, ai, 0x28, 4},
                          {0x004fb6a4, damage, 0x2c, 1}, {0x004fb6b0, reversed, 0x2d, 1}};
    bool changed = false;
    int32_t v;
    if (ccall<int>(D_contains_list, text, (const uint32_t*)server_kw) == -1) return;
    if (ccall<uint8_t>(D_get_numeric, text, CP(0x004fb6bc), &v, 1, 8)) {                  // "track "
        v--;
        self->prop.track = v;
        const char* name = ccall<const char*>(F_GetTrackName, v);
        changed = true;
        LOG(CP(0x004fb6c4), v + 1, name);
    }
    if (ccall<uint8_t>(D_get_numeric, text, CP(0x004fb6e4), &v, 0, 6)) {                  // "aicars "
        changed = true;
        LOG(CP(0x004fb6ec), v);
        self->prop.aicars = v;
    }
    for (int e = 0; e < 5; e++) {
        const int k = ccall<int>(D_contains_list, text, tab[e].values);
        if (k == -1) continue;
        uint8_t* at = (uint8_t*)&self->prop + tab[e].off;
        if (tab[e].size == 4) *(volatile int32_t*)at = k;
        else if (tab[e].size == 1) *(volatile uint8_t*)at = k != 0 ? 1 : 0;
        else PANIC(CP(0x004fb710), tab[e].size);
        changed = true;
        LOG(CP(0x004fb72c), CP(tab[e].name), CP(tab[e].values[k]));
    }
    if (changed) {
        tcall<void>(D_update_laps, self);
        vcall<void>(self->rs, RS_Propose, (const void*)&self->prop, 0);
    }
}
static void fp_tchat(Footprint& f, Server*, Edx, int, const char*) { f.replay_only = R_SERVER; }
PORT_FN(0x004a3270, "Server::track_chat", track_chat, fp_tchat)

// the index of the first word of the list in the text, or -1
static int __cdecl contains_list(const char* text, const uint32_t* list) {
    for (const uint32_t* w = list; *w; w++)
        if (ccall<const char*>(F_strstr, text, CP(*w))) return (int)(w - list);
    return -1;
}
static void fp_cl(Footprint& f, const char*, const uint32_t*) { f.add(&f, 0, "nothing"); }
PORT_FN(0x004a3520, "contains(list)", contains_list, fp_cl)

// "key<digits>": the number (up to 12 digits), out of [lo, hi] -> lo
static uint8_t __cdecl get_numeric(const char* text, const char* key, int32_t* out, int lo, int hi) {
    const char* p = ccall<const char*>(F_strstr, text, key);
    if (!p) return 0;
    // FIX: the buffer holds 12 digits and their NUL. The original's is 12 bytes: 12 digits put the NUL on the low byte of
    // its return address ("server: track 000000000003" crashed the dedicated server). (The rewrite's buffer is this size in
    // every build.)
    char buf[13];
    char* d = buf;
    p += crt_strlen(key);
    while (ccall<int>(F_isdigit, (int)*(const volatile signed char*)p) && *(const volatile char*)p) {
        *d++ = *p++;
        if (d - buf >= 0xc) break;
    }
    *d = 0;
    int v = ccall<int>(F_atoi, (const char*)buf);
    if (v < lo || v > hi) v = lo;
    *out = v;
    return 1;
}
static void fp_gn(Footprint& f, const char*, const char*, int32_t* out, int, int) { f.add(out, 4, "the number"); }
PORT_FN(0x004a3560, "get_numeric", get_numeric, fp_gn)

// a chat line (user 0: the console): settings while approving (users only if the console allowed it); while racing,
// "end race" goes back to the chat -- and every line (that one too) restarts the race
static void __fastcall chat(Server* self, Edx, int user, const char* text) {
    const int phase = vcall<int>(self->rs, RS_GetPhase);
    if (phase == 0) {
        if (!U8(G_CHAT_CONFIG) && user) return;
        tcall<void>(D_track_chat, self, user, text);
        return;
    }
    if (phase != 3) return;
    if (ccall<int>(D_contains, text, CP(0x004fb744))) {     // "end race"
        vcall<void>(self->rs, RS_GoBackToChat);
        LOG(CP(0x004fb750));
    } else if (ccall<int>(D_contains, text, CP(0x004fb760))) {   // "restart"
        LOG(CP(0x004fb768));
    }
    vcall<void>(self->rs, RS_RestartRace);
}
static void fp_chat(Footprint& f, Server*, Edx, int, const char*) { f.replay_only = R_SERVER; }
PORT_FN(0x004a35f0, "Server::chat", chat, fp_chat)

static int __cdecl contains(const char* a, const char* b) { return (int)(uintptr_t)ccall<const char*>(F_strstr, a, b); }
static void fp_c(Footprint& f, const char*, const char*) { f.add(&f, 0, "nothing"); }
PORT_FN(0x004a3680, "contains", contains, fp_c)

// ---- the dedicated server -----------------------------------------------------------------------------------------------------
static void __cdecl MultiDedicatedServer(int16_t port) {
    if (!ccall<uint8_t>(D_ded_begin)) return;
    if (!port) port = 0x7d1;
    void* sock = ccall<void*>(F_SocketCreateUDP, port);
    if (!sock) {
        LOG(CP(0x004fb8fc), 0x7d1, 0x7d1);
        ccall<void>(D_ded_end);
        return;
    }
    LOG(CP(0x004fb77c), (int)port, (int)port);
    void* sm = ccall<void*>(F_CreateSessionMgr, sock, 8, 0, 1);
    if (!sm) {
        LOG(CP(0x004fb8e4));
        ccall<void>(D_ded_end);
        return;
    }
    LOG(CP(0x004fb7a4));
    char name[0x20], password[0x10], answer[0xa];
    crt_copy(name, CP(0x004fb7b8), 13);                      // "Viper Server"
    for (int i = 13; i < 0x20; i++) name[i] = 0;
    password[0] = *CP(0x004fb7c8);
    for (int i = 1; i < 0x10; i++) password[i] = 0;
    ccall<void>(D_prompt_user, CP(0x004fb7cc), (char*)name, 0x20u);
    ccall<void>(D_prompt_user, CP(0x004fb7fc), (char*)password, 0x10u);
    ccall<void>(D_prompt_user, CP(0x004fb82c), (char*)answer, 0xau);
    const uint8_t allow = ccall<int>(F_tolower, (int)(signed char)answer[0]) != 'n' ? 1 : 0;
    U8(G_CHAT_CONFIG) = allow;
    LOG(CP(0x004fb868), allow ? CP(0x004fb85c) : CP(0x004fb860));
    const int task = ccall<int>(F_TaskCreate, CP(0x004fb8a0), (void*)(uintptr_t)D_idle);
    const char* pw = password[0] ? password : 0;
    Server server;
    tcall<void>(D_init_game_info, &server);
    server.sm = sm;
    server.rs = ccall<void*>(S_CreateRaceServer, sm, (const char*)name, pw);
    if (server.rs) {
        vcall<void>(server.rs, RS_Propose, (const void*)&server.prop, 0);
        vcall<void>(server.rs, RS_SetChatCallback, (void*)(uintptr_t)D_chat_cb, (void*)0);
    }
    U32(G_GLOBAL) = (uint32_t)(uintptr_t)&server;
    if (server.sm && tcall<uint8_t>(F_SM_Ok, server.sm) && server.rs) {
        LOG(CP(0x004fb8b0), (const char*)name);
        do {
            ccall<void>(D_spin);
            tcall<void>(D_Server_Tick, &server);
        } while (!U8(G_QUIT));
    } else {
        LOG(CP(0x004fb8d0));
    }
    ccall<void>(F_TaskDestroy, task);
    if (server.rs) vcall<void*>(server.rs, RS_DTOR, 1u);
    U32(G_GLOBAL) = 0;
    tcall<void>(F_SM_Shutdown, sm);
    while (!tcall<uint8_t>(F_SM_CanDestroySafely, sm)) {
        tcall<void>(F_SM_Tick, sm);
        ccall<void>(D_idle);
    }
    tcall<void>(F_SM_dtor, sm);
    ccall<void>(F_op_delete, sm);
    ccall<void>(D_ded_end);
}
static void fp_mds(Footprint& f, int16_t) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a36a0, "MultiDedicatedServer", MultiDedicatedServer, fp_mds)

static uint8_t __cdecl ded_begin() {
    IAT(AllocConsole_f, I_AllocConsole)();
    U32(G_HIN) = IAT(GetStdHandle_f, I_GetStdHandle)((uint32_t)-10);
    U32(G_HOUT) = IAT(GetStdHandle_f, I_GetStdHandle)((uint32_t)-11);
    IAT(SetConsoleMode_f, I_SetConsoleMode)(U32(G_HIN), 7);
    IAT(SetConsoleTitleA_f, I_SetConsoleTitleA)(CP(0x004fb920));
    ccall<void>(F_LogInstallHook, (void*)(uintptr_t)D_ded_log_hook);
    LOG(CP(0x004fb940));
    LOG(CP(0x004fb970), ccall<const char*>(F_VersionGetBuildString));
    LOG(CP(0x004fb974));
    ccall<void>(F_UsefulBegin);
    ccall<void>(F_ResourceSetMustLoad, CP(0x004fb9a4));     // "common.res"
    ccall<void>(F_RaceBegin);
    return 1;
}
static void fp_db(Footprint& f) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a3950, "ded_begin", ded_begin, fp_db)

// the log's lines to the console
static void __cdecl ded_log_hook(const char* s) {
    uint32_t n = 0;
    IAT(WriteConsoleA_f, I_WriteConsoleA)(U32(G_HOUT), s, crt_strlen(s), &n, 0);
    IAT(WriteConsoleA_f, I_WriteConsoleA)(U32(G_HOUT), CP(0x004fb9b0), 1, &n, 0);
}
static void fp_dlh(Footprint& f, const char*) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a39e0, "ded_log_hook", ded_log_hook, fp_dlh)

static void __cdecl ded_end() {
    ccall<void>(F_RaceEnd);
    ccall<void>(F_ResourceSetUnload, CP(0x004fb9b4));       // "common.res"
    ccall<void>(F_UsefulEnd);
    ccall<void>(F_LogUninstallHook, (void*)(uintptr_t)D_ded_log_hook);
    IAT(FreeConsole_f, I_FreeConsole)();
}
static void fp_de(Footprint& f) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a3a40, "ded_end", ded_end, fp_de)

// the "dedicated input" task: console lines (trailing blanks cut) to handle_input until quit
static void __cdecl idle() {
    if (U8(G_QUIT)) return;
    char buf[0x400];
    uint32_t n;
    do {
        if (ccall<uint8_t>(F_TaskShouldIDie)) return;
        if (IAT(ReadConsoleA_f, I_ReadConsoleA)(U32(G_HIN), buf, 0x400, &n, 0)) {
            while (n > 0 && ccall<int>(F_isspace, (int)(signed char)buf[n - 1])) n--;
            if (n < 0x400) buf[n] = 0;
            // FIX: a read that fills the buffer (and doesn't end in a blank) is terminated in its last byte, which gives
            // way (as prompt_user's does); the original handed handle_input a line with no NUL
            else if (VP_FIX) buf[0x3ff] = 0;
            ccall<void>(D_handle_input, (const char*)buf);
        }
    } while (!U8(G_QUIT));
}
static void fp_idle(Footprint& f) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a3a70, "idle(ded.obj)", idle, fp_idle)

// a console line
static void __cdecl handle_input(const char* line) {
    bool known = true;
    if (!ccall<int>(F_strnicmp, line, CP(0x004fb9c0), 4)) {                // "quit"
        U8(G_QUIT) = 1;
        LOG(CP(0x004fb9c8));
    } else if (!ccall<int>(F_strnicmp, line, CP(0x004fb9d4), 7) ||        // "server:"
               !ccall<int>(F_strnicmp, line, CP(0x004fb9dc), 7) ||        // "restart"
               !ccall<int>(F_strnicmp, line, CP(0x004fb9e4), 8)) {        // "end race"
        U32(G_MSG) = (uint32_t)(uintptr_t)line;
        while (U32(G_MSG)) IAT(Sleep_f, I_Sleep)(0x32);    // the main loop's Server::Tick takes it
    } else if (!ccall<int>(F_strnicmp, line, CP(0x004fb9f0), 4) ||        // "help"
               !ccall<int>(F_strnicmp, line, CP(0x004fb9f8), 1)) {        // "?"
        LOG(CP(0x004fb9fc));
        LOG(CP(0x004fba1c));
        LOG(CP(0x004fba48));
        LOG(CP(0x004fba88));
        LOG(CP(0x004fbac4));
        LOG(CP(0x004fbb00));
    } else {
        known = false;
    }
    // FIX: "Unrecognized command" only for a line that is one; the original logged it after every command, the ones it
    // had just carried out too (cosmetic: the console's log only)
    if (!(VP_FIX && known)) LOG(CP(0x004fbb20), line);
}
static void fp_hi(Footprint& f, const char*) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a3b10, "handle_input", handle_input, fp_hi)

// the prompt, then a line (trailing blanks cut; a read of the whole buffer loses its last byte)
static void __cdecl prompt_user(const char* prompt, char* buf, uint32_t size) {
    uint32_t written = 0, n;
    IAT(WriteConsoleA_f, I_WriteConsoleA)(U32(G_HOUT), prompt, crt_strlen(prompt), &written, 0);
    n = 0;
    IAT(ReadConsoleA_f, I_ReadConsoleA)(U32(G_HIN), buf, size, &n, 0);
    for (;;) {
        if (n >= size) { n--; continue; }
        if (n == 0) break;
        if (!ccall<int>(F_isspace, (int)(signed char)buf[n - 1])) break;
        n--;
    }
    buf[n] = 0;
}
static void fp_pu(Footprint& f, const char*, char*, uint32_t) { f.replay_only = R_CONSOLE; }
PORT_FN(0x004a3c90, "prompt_user", prompt_user, fp_pu)

static void __cdecl chat_cb(void*, int user, const char* text) {
    tcall<void>(D_chat, (void*)(uintptr_t)U32(G_GLOBAL), user, text);
}
static void fp_ccb(Footprint& f, void*, int, const char*) { f.replay_only = R_SERVER; }
PORT_FN(0x004a3d30, "Server::chat_cb", chat_cb, fp_ccb)

#undef LOG
#undef PANIC
#undef CP
#undef U8
#undef U32
#undef IAT
}  // namespace net_ded
}  // namespace
