// vrmod_image.h -- vrmod's patches to v1.0 race.exe, for the world harnesses: the bytes viper-mod-manager's patch set
// wrote into the user's v1.0-RC install's race.exe (game-files\installs\v1.0-RC: race.exe against its
// race.exe.vrmod-original, the stock file), in .text and .rdata (its .data path strings left out). The harnesses load
// out\race_v10.exe at 0x400000; vrmod_apply() turns it into that vrmod race.exe in place, vrmod_unapply() back.
//
//   enginefix (Obstacle::Reset, IdealLine::advance_bead), modassert (unsafe_check: ret), resolution (mode 4 = 1920 x 1080
//   in gxSetMode, gxChangeMode, set_mode, mode_callback), tablefix + needlefix (gxTriangle's tables 0x4000, both bounds
//   0x800, the fill test's stub at 0x4da490 in .text slack), vrampatch (find_mem: the add NOP'd), vertexbuffer
//   (mr_model_begin: push 0x80000), aspectfix (dviewport::set_viewport: Hor+, R0 = 0.65 at 0x4e0294 in .rdata slack),
//   hornball (create_ball's mass 20700 and radius 36; Ball::Throw's cooldown 0.1, speed 77.8, ahead 4, up 1),
//   carlist (HackOptionsControl::Added's Vehicle list).
//
// The rewrites that take these over (docs/FIXES.md, "vrmod's patches") are checked against them: a harness applies
// them, sets the player's values at random where vrmod lets the player choose (vrmod_put32), and compares vrmod's
// original with the rewrite.
#pragma once
#include <stdint.h>
#include <string.h>

struct VrmodEdit { uint32_t va; const char* hex; };
static const VrmodEdit k_vrmod_edits[] = {
    {0x00415020, "c3"},
    {0x00421849, "c744242cffffffff31db837f040075098b472c894704895f0890909090909090"},
    {0x0043d3dd, "909090"},
    {0x0044dfae, "8007"},
    {0x0044dfb8, "3804"},
    {0x0044e0d1, "8007"},
    {0x0044e0db, "3804"},
    {0x00450762, "40"},
    {0x00450781, "40"},
    {0x00450788, "40"},
    {0x0045078f, "40"},
    {0x00450796, "40"},
    {0x004507a4, "40"},
    {0x004507b2, "40"},
    {0x004507c2, "20"},
    {0x004507d1, "40"},
    {0x004507e1, "20"},
    {0x004507ec, "40"},
    {0x004507f8, "40"},
    {0x00450808, "20"},
    {0x00450813, "40"},
    {0x00450825, "40"},
    {0x00450835, "20"},
    {0x00450840, "40"},
    {0x0045084c, "40"},
    {0x0045085c, "20"},
    {0x00450867, "40"},
    {0x004508a4, "40"},
    {0x004508ab, "e9e09b08009090909090"},
    {0x004508bd, "20"},
    {0x004508f5, "40"},
    {0x0045098c, "81fb0008000090907d"},
    {0x00454baf, "80070000b93804"},
    {0x00455156, "9090909090909090"},
    {0x004553b9, "8007"},
    {0x004553c2, "3804"},
    {0x0045576b, "000008"},
    {0x0045ec5f, "da742414d9c0d90594024e00d8d1dfe09e7604ddd9eb02ddd8d9542420d8c0d8f1ddd9d9542424d80d54b04d00d95c241cc74424300000803fd9442420d8c0909090"},
    {0x004636f3, "b8a146"},
    {0x00463702, "1042"},
    {0x00486360, "ff"},
    {0x00486365, "6f687c"},
    {0x0048636c, "86"},
    {0x004da490, "81fe000800007d0fa1f8fb4e0039700c7e05e90e64f7ffe92864f7ff"},
    {0x004dc39c, "cdcccc3d"},
    {0x004dc3a4, "398e9b4200008040000080"},
    {0x004e0294, "6666263f"},
};
static uint8_t g_vrmod_saved[1024];

static int vrmod_hex(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
// the image as the vrmod install has it (on = true), or back to the stock file's bytes
static void vrmod_set(bool on) {
    uint32_t at = 0;
    for (const VrmodEdit& e : k_vrmod_edits) {
        uint8_t* p = (uint8_t*)(uintptr_t)e.va;
        const size_t n = strlen(e.hex) / 2;
        for (size_t i = 0; i < n; i++, at++) {
            if (on) {
                g_vrmod_saved[at] = p[i];
                p[i] = (uint8_t)(vrmod_hex(e.hex[2 * i]) << 4 | vrmod_hex(e.hex[2 * i + 1]));
            } else {
                p[i] = g_vrmod_saved[at];
            }
        }
    }
}
static void vrmod_apply() { vrmod_set(true); }
static void vrmod_unapply() { vrmod_set(false); }
// a value vrmod lets the player choose, written where vrmod writes it (an operand in the code, a float in .rdata)
static void vrmod_put32(uint32_t at, uint32_t v) { memcpy((void*)(uintptr_t)at, &v, 4); }
static uint32_t vrmod_get32(uint32_t at) { uint32_t v; memcpy(&v, (const void*)(uintptr_t)at, 4); return v; }
