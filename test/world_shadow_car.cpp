// world_shadow_car.cpp -- an in-game shadow check of Car::Update (hook/port.cpp), reproduced offline on a car that
// dents: what the check's footprint must hold for the rewrite pass to start where the original pass did.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /DVP_FAITHFUL test\world_shadow_car.cpp
//        /Fo<dir>\ /Fe<dir>\world_shadow_car.exe /link /BASE:0x10000000 /DYNAMICBASE:NO /FIXED /MACHINE:X86
//   run:   world_shadow_car.exe [worlds] [seed]        (out\race_v10.exe, or the path in VP_RACE_EXE)
//
// test/world_car.cpp's worlds (included whole: its cars, stubs and loader), plus what they leave out and a crash
// needs: the car's five live models (LOD 0 a random body mesh with a surface and UVs, LODs 1..4 mapped onto it
// through the vertex maps, as Car::Car builds them), each wheel's hub vertex pointing INTO the LOD-0 mesh (as
// Car::Setup points it with damage on), damage on, and one to three queued external impulses, most of them
// big enough to dent (over 741.67 once PhobDyno::Update has weighted them) and aimed at a hub. Then
// PhobDyno::Update resolves them through the real Car::ResolveExternalImpulse -> ApplyDamage ->
// ActuallyApplyDamage, which dents the mesh and marks the damage grid, and the next Wheel::Update reads the moved
// hub. PhysReplayAddEvent is a logging stub. Each world runs three checks:
//   1. the rewrite against the original from the same whole state (world_car's check, with the models): a real
//      rewrite bug on the damage path would show here;
//   2. every byte the original changed -- the car and the models' vertices -- must lie in Car::Update's footprint;
//   3. the shadow check exactly as port.cpp makes it in game: save the footprint, run the original, keep its
//      result, restore ONLY the footprint, run the rewrite, compare the footprint. A write the footprint misses
//      survives into the rewrite pass. Built against the footprint before the fix (the car, deity and splash
//      only), the models' dents leaked: damage_grid / damaged and, through the hub vertices, frame mismatches --
//      the ones the Burninator shadow race logged, while replays were identical.
#define _CRT_SECURE_NO_WARNINGS
#include <vector>
#define main world_car_main
#include "world_car.cpp"
#undef main

// ---- the renderer's model, as Car's code sees it (hook/phys_car.cpp) -----------------------------------------
struct MrVertex { P3 pos; float nrm[3]; float u, v; };
struct MrSurface { uint8_t _00[0x18]; int16_t first_vertex, end_vertex, first_face, end_face; };
struct MrModelInfo { int32_t num_verts; MrVertex* verts; int32_t num_surfaces; MrSurface* surfaces; };
static_assert(sizeof(MrVertex) == 32 && sizeof(MrSurface) == 0x20, "mr");
enum { MAXV = 480 };
struct Model {
    uint8_t model_info[0x20];          // the handle: +0x10 its MrModelInfo*, +0x15 a flag (mrModelBuild writes both)
    MrModelInfo info;
    MrSurface surface;
    MrVertex verts[MAXV];
    uint16_t map[MAXV];                // this LOD's vertex -> its LOD-0 vertex (Car +0x4d8)
};
static Model g_model[5];
static Model g_model_start[5], g_model_orig[5], g_model_rew[5];

static uint8_t* car_bytes(int i) { return (uint8_t*)car(i); }
static int32_t* live_models(Car* c) { return (int32_t*)((uint8_t*)c + 0x4c4); }
static uint16_t** lod_maps(Car* c) { return (uint16_t**)((uint8_t*)c + 0x4d8); }

// PhysReplayAddEvent (0x42d8b0): the damage event Car::ResolveExternalImpulse records
static void __cdecl stub_replay_event(int type, const void* data, int size) {
    log_put(0x4e570000u); log_put((uint32_t)type); log_put((uint32_t)size);
    for (int i = 0; i + 4 <= size && i < 16; i += 4) { uint32_t w; memcpy(&w, (const uint8_t*)data + i, 4); log_put(w); }
}

static int g_hub_index[4];
static void build_models_and_crash() {
    Car* c = car(0);
    memset(g_model, 0, sizeof g_model);
    const int n0 = 48 + (int)(rnd() % (MAXV - 48));
    Model& m0 = g_model[0];
    for (int i = 0; i < n0; i++) {
        MrVertex& v = m0.verts[i];
        v.pos.x = range(-0.95f, 0.95f); v.pos.y = range(-0.45f, 0.65f); v.pos.z = range(-2.3f, 2.3f);
        v.nrm[0] = range(-1, 1); v.nrm[1] = range(-1, 1); v.nrm[2] = range(-1, 1);
        v.u = uni(); v.v = uni();
        m0.map[i] = (uint16_t)i;
    }
    // each wheel's hub: a LOD-0 vertex (distinct), at the hub point world_car gave it
    for (int w = 0; w < 4; w++) {
        int k;
        bool again;
        do { k = (int)(rnd() % n0); again = false; for (int j = 0; j < w; j++) again |= g_hub_index[j] == k; } while (again);
        g_hub_index[w] = k;
        FullWheel* fw = (FullWheel*)&c->wheels[w];
        m0.verts[k].pos = *fw->hub_vertex;
        fw->hub_vertex = &m0.verts[k].pos;
    }
    for (int lod = 0; lod < 5; lod++) {
        Model& m = g_model[lod];
        int n = n0;
        if (lod > 0) {
            n = n0 >> lod;
            if (n < 8) n = 8;
            for (int j = 0; j < n; j++) {
                m.map[j] = (uint16_t)(chance(20) ? g_hub_index[rnd() % 4] : (int)(rnd() % n0));
                m.verts[j] = m0.verts[m.map[j]];
                m.verts[j].u = uni(); m.verts[j].v = uni();
            }
        }
        m.surface.first_vertex = (int16_t)(chance(80) ? 0 : (int)(rnd() % 8));
        m.surface.end_vertex = (int16_t)(chance(80) ? n : n - (int)(rnd() % 8));
        m.info.num_verts = n;
        m.info.verts = m.verts;
        m.info.num_surfaces = 1;
        m.info.surfaces = &m.surface;
        *(MrModelInfo**)(m.model_info + 0x10) = &m.info;
        live_models(c)[lod] = (int32_t)(uintptr_t)m.model_info;
        lod_maps(c)[lod] = m.map;
    }
    // the crash: queued impulses, most over the damage threshold once weighted, most aimed at a hub
    *g_damage_on = chance(92);
    c->impulse_weight = chance(50) ? 1.0f : range(0.5f, 3.0f);
    const int n = 1 + (int)(rnd() % 3);
    const float* M = c->frame.rot.m;
    for (int e = 0; e < n; e++) {
        uint8_t* x = (uint8_t*)&c->external_impulses[e];
        const P3& t = m0.verts[chance(65) ? g_hub_index[rnd() % 4] : (int)(rnd() % n0)].pos;
        // ActuallyApplyDamage pushes the body point out 30% from (0, 1, 0) before snapping it to a vertex: aim so
        // that it lands on t
        double q[3] = {t.x / 1.3 + range(-0.04f, 0.04f), (t.y - 1.0) / 1.3 + 1.0 + range(-0.04f, 0.04f),
                       t.z / 1.3 + range(-0.04f, 0.04f)};
        P3 point, imp;
        point.x = (float)(c->frame.pos.x + q[0] * M[0] + q[1] * M[3] + q[2] * M[6]);
        point.y = (float)(c->frame.pos.y + q[0] * M[1] + q[1] * M[4] + q[2] * M[7]);
        point.z = (float)(c->frame.pos.z + q[0] * M[2] + q[1] * M[5] + q[2] * M[8]);
        double d[3] = {range(-1, 1), range(-1, 1), range(-1, 1)};
        double l = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-9;
        const float mag = (chance(85) ? range(800.0f, 30000.0f) : range(0.0f, 740.0f)) * c->impulse_weight;
        imp.x = (float)(d[0] / l * mag); imp.y = (float)(d[1] / l * mag); imp.z = (float)(d[2] / l * mag);
        memcpy(x, &imp, 12);
        memcpy(x + 12, &point, 12);
        int32_t surface = chance(70) ? 0 : (int32_t)(rnd() % 25);
        memcpy(x + 24, &surface, 4);
    }
    c->num_external_impulses = n;
}

// ---- the footprint's spans, saved and restored as port.cpp does -----------------------------------------------
static std::vector<uint8_t> g_span_before, g_span_after;
static void spans_save(const Footprint& fp, std::vector<uint8_t>& to) {
    to.clear();
    for (int i = 0; i < fp.n; i++) to.insert(to.end(), (uint8_t*)fp.r[i].p, (uint8_t*)fp.r[i].p + fp.r[i].n);
}
static void spans_restore(const Footprint& fp, const std::vector<uint8_t>& from) {
    size_t at = 0;
    for (int i = 0; i < fp.n; i++) { memcpy(fp.r[i].p, &from[at], fp.r[i].n); at += fp.r[i].n; }
}

// a name for a byte: the car's fields, the models, or the footprint entry
static const char* describe(const uint8_t* p, char* buf) {
    for (int i = 0; i < 2; i++) {
        const uint8_t* b = car_bytes(i);
        if (p >= b && p < b + CAR_BUF) {
            uint32_t off = (uint32_t)(p - b);
            char tmp[96];
            if (off == 0x48c) sprintf(tmp, "car.damaged");
            else if (off >= 0x48e && off < 0x4ae) sprintf(tmp, "car.damage_grid[%u]", (off - 0x48e) / 2);
            else name_of(off, tmp);
            sprintf(buf, "%s%s", i ? "other " : "", tmp);
            return buf;
        }
    }
    for (int lod = 0; lod < 5; lod++) {
        const uint8_t* b = (const uint8_t*)&g_model[lod];
        if (p >= b && p < b + sizeof(Model)) {
            uint32_t off = (uint32_t)(p - b);
            if (off >= offsetof(Model, verts) && off < offsetof(Model, map)) {
                uint32_t k = (off - offsetof(Model, verts)) / 32;
                bool hub = false;
                for (int w = 0; w < 4; w++) hub |= lod == 0 && g_hub_index[w] == (int)k;
                sprintf(buf, "LOD %d vertex %u+%u%s", lod, k, (off - offsetof(Model, verts)) % 32, hub ? " (a wheel's hub)" : "");
            } else sprintf(buf, "LOD %d model +0x%x", lod, off);
            return buf;
        }
    }
    sprintf(buf, "%p", (const void*)p);
    return buf;
}
static bool in_fp(const Footprint& fp, const uint8_t* p) { return in_footprint(fp, p); }

int main(int argc, char** argv) {
    if (!GetEnvironmentVariableA("VP_WORLD_CHILD", 0, 0)) return relaunch();
    setvbuf(stdout, 0, _IONBF, 0);
    int iterations = argc > 1 ? atoi(argv[1]) : 20000;
    if (argc > 2) g_rng = ((uint32_t)strtoul(argv[2], 0, 0) * 2654435761u) | 1;
    char exe[MAX_PATH];
    if (!GetEnvironmentVariableA("VP_RACE_EXE", exe, sizeof exe)) {
        strcpy(exe, __FILE__);
        char* s = strstr(exe, "\\test\\world_shadow_car.cpp");
        if (s) strcpy(s, "\\out\\race_v10.exe");
    }
    if (!load_race_exe(exe)) return 2;
    patch_jmp(0x00465c40, (void*)&stub_terrain);
    patch_jmp(0x0041b6e0, (void*)&stub_random);
    patch_jmp(0x0042bc80, (void*)&stub_time);
    patch_jmp(0x004626d0, (void*)&stub_focus);
    patch_jmp(0x00472c60, (void*)&stub_engine_sound);
    patch_jmp(0x00439aa0, (void*)&stub_fadein);
    patch_jmp(0x00439e50, (void*)&stub_reset_damage);
    patch_jmp(0x004410e0, (void*)&stub_getball);
    patch_jmp(0x004412a0, (void*)&stub_throw);
    patch_jmp(0x00465ae0, (void*)&stub_terrain_xz);
    patch_jmp(0x00443640, (void*)&stub_deity_update);
    patch_jmp(0x00443a00, (void*)&stub_deity_teleport);
    patch_jmp(0x0042d8b0, (void*)&stub_replay_event);
    *(uint32_t*)g_deity_obj = 0x004dc588;
    *(void**)0x005218ac = g_deity_obj;
    *(void**)0x00521f7c = 0;
    for (int i = 0; i < 2; i++) g_carbuf[i] = (uint8_t*)VirtualAlloc(0, CAR_BUF, MEM_COMMIT, PAGE_READWRITE);

    static State start, orig, rew;
    static Footprint fp;
    int ran = 0, replay_only = 0, faults = 0, dents = 0, hub_moved = 0, grid_marked = 0;
    int rewrite_bad = 0, fp_bad = 0, shadow_bad = 0, shadow_fields[4] = {0};   // grid/damaged, frame, verts, other
    for (int it = 0; it < iterations; it++) {
        randomize_world();
        randomize_args();
        build_models_and_crash();
        Car* c = car(0);
        fp.n = 0; fp.pure = false; fp.replay_only = 0;
        fpof_Car_Update(fp, c, 0);
        if (fp.replay_only) { replay_only++; continue; }
        save(start);
        memcpy(g_model_start, g_model, sizeof g_model);

        // 1. the rewrite against the original, from the same whole state
        uint64_t r;
        int fo = run_guarded(K_UPDATE, false, &r);
        save(orig); orig.log = g_log;
        memcpy(g_model_orig, g_model, sizeof g_model);
        load(start); memcpy(g_model, g_model_start, sizeof g_model);
        int fn = run_guarded(K_UPDATE, true, &r);
        save(rew); rew.log = g_log;
        memcpy(g_model_rew, g_model, sizeof g_model);
        if (fo || fn) {
            faults++;
            if (fo != fn) { rewrite_bad++; printf("  world %d: original %s, rewrite %s\n", it, fo ? "faulted" : "ran", fn ? "faulted" : "ran"); }
            continue;
        }
        ran++;
        bool same = compare(orig, rew, it, K_UPDATE);
        if (memcmp(g_model_orig, g_model_rew, sizeof g_model)) {
            const uint8_t *a = (const uint8_t*)g_model_orig, *b = (const uint8_t*)g_model_rew;
            size_t k = 0;
            while (a[k] == b[k]) k++;
            char buf[96];
            if (same) printf("  MISMATCH, world %d (Car::Update)\n", it);
            printf("    %s differs\n", describe((const uint8_t*)g_model + k, buf));
            same = false;
        }
        if (!same) rewrite_bad++;
        const Car* sc = (const Car*)start.car[0];
        const Car* oc = (const Car*)orig.car[0];
        if (memcmp(g_model_start, g_model_orig, sizeof g_model)) dents++;
        if (memcmp((const uint8_t*)sc + 0x48e, (const uint8_t*)oc + 0x48e, 32)) grid_marked++;
        for (int w = 0; w < 4; w++)
            if (memcmp(&g_model_start[0].verts[g_hub_index[w]].pos, &g_model_orig[0].verts[g_hub_index[w]].pos, 12)) { hub_moved++; break; }

        // 2. what the original changed, against the footprint
        if (check_footprint(start, orig, fp, it, K_UPDATE)) fp_bad++;
        else {
            const uint8_t *a = (const uint8_t*)g_model_start, *b = (const uint8_t*)g_model_orig;
            for (size_t k = 0; k < sizeof g_model; k++)
                if (a[k] != b[k] && !in_fp(fp, (const uint8_t*)g_model + k)) {
                    char buf[96];
                    if (fp_bad < 10) printf("  FOOTPRINT, world %d (Car::Update): %s changed outside it\n", it, describe((const uint8_t*)g_model + k, buf));
                    fp_bad++;
                    break;
                }
        }

        // 3. the shadow check as the game makes it: only the footprint is saved and restored
        load(start); memcpy(g_model, g_model_start, sizeof g_model);
        spans_save(fp, g_span_before);
        run_guarded(K_UPDATE, false, &r);
        Log log_orig = g_log;
        spans_save(fp, g_span_after);
        spans_restore(fp, g_span_before);
        run_guarded(K_UPDATE, true, &r);
        size_t at = 0;
        bool bad = log_orig.n != g_log.n || memcmp(log_orig.e, g_log.e, 4 * (log_orig.n < 128 ? log_orig.n : 128));
        const uint8_t* where = 0;
        for (int i = 0; i < fp.n && !where; i++) {
            const uint8_t* p = (const uint8_t*)fp.r[i].p;
            for (uint32_t k = 0; k < fp.r[i].n; k++)
                if (p[k] != g_span_after[at + k]) { where = p + k; break; }
            at += fp.r[i].n;
        }
        if (where || bad) {
            shadow_bad++;
            char buf[96];
            if (where) {
                const char* d = describe(where, buf);
                int kind = strstr(d, "damage") ? 0 : strstr(d, "frame") ? 1 : strstr(d, "LOD") ? 2 : 3;
                shadow_fields[kind]++;
                if (shadow_bad <= 12) printf("  SHADOW MISMATCH, world %d (Car::Update): %s\n", it, d);
            } else if (shadow_bad <= 12) printf("  SHADOW MISMATCH, world %d (Car::Update): the stub logs (outputs) differ\n", it);
        }
    }
    printf("%d worlds: %d ran (%d replay-only, %d faulted); the original dented %d (damage grid marked in %d, a hub moved in %d)\n",
           iterations, ran, replay_only, faults, dents, grid_marked, hub_moved);
    printf("  rewrite vs original, whole state: %d differ\n", rewrite_bad);
    printf("  bytes the original changed outside Car::Update's footprint: %d worlds\n", fp_bad);
    printf("  shadow checks (only the footprint restored): %d mismatch (damage grid / damaged %d, frame %d, model verts %d, other %d)\n",
           shadow_bad, shadow_fields[0], shadow_fields[1], shadow_fields[2], shadow_fields[3]);
    return rewrite_bad || fp_bad || shadow_bad ? 1 : 0;
}
