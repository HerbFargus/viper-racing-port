// world_lodmap.cpp -- Car::Car's LOD vertex maps (hook/phys_car.cpp): the grid search (lodmap_search) against the
// original's brute-force loop (lodmap_original), entry by entry, on random meshes and on real cars' meshes.
//
//   build (x86 tools, from the repo root):
//     cl /nologo /O2 /arch:IA32 /fp:precise /MT /W3 /EHsc /std:c++17 /FC /DVP_FUZZ test\world_lodmap.cpp
//        /Fo<outdir>\ /Fe<outdir>\world_lodmap.exe /link /MACHINE:X86
//   run:   world_lodmap.exe [iterations] [seed]          random meshes (default 100000 iterations)
//          world_lodmap.exe mesh <file> [repeats]        a real car's LODs 0-4 (tools: a .car read with vrmod into
//                                                        "LODM", int32 count, then per LOD int32 n + n 32-byte vertex
//                                                        records exactly as in the .mod): both versions timed
//
// Random meshes, each iteration one of: uniform clouds; welded seams (the same position in many records); queries
// exactly on LOD-0 vertices (decimated LODs); near-ties (points on a sphere around the query nudged by an ulp or
// two, lattices with the query at a cell centre: exact ties); queries around 1e4 away (the 1e8 start) and beyond;
// n0 = 1; NaN, infinite and > 1e15 coordinates (the fallback); huge magnitudes (base up to 1e15, tiny spreads);
// denormal-small coordinates; flat and line meshes; a car-like shell big enough for the game's own threshold; far
// clusters. The x87 precision rotates 24 / 53 / 64 bits (the physics thread runs Car::Car at 24). The search runs
// without the game's n0 x n threshold, so every size takes the fast path; where it says it can't (a coordinate out
// of bounds), the original loop runs, as in Car::Car, and that's counted. Doesn't need race_v10.exe.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hook/phys_car.cpp"

// ---- what phys_car.cpp links against, standing in for the DLL and test/fuzz.cpp ----------------------------------
void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
uint32_t A(uint32_t v10) { return v10; }
bool have(uint32_t) { return true; }
bool build_is_v10() { return true; }
void Footprint::add(void* p, uint32_t bytes, const char* what) {
    if (n < MAX) r[n++] = {p, bytes, what};
}
void Footprint::object(void* obj, const char* what) { add(obj, 4, what); }
static uint32_t g_state = 0x2545f491u;
bool g_fuzz_specials = true;
uint32_t fuzz_rand() {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 17;
    g_state ^= g_state << 5;
    return g_state;
}
float fuzz_float() { return 0; }
void fuzz_fill(Arena&) {}
int fuzz_report(int it, const Arena&, const void*, const void*, size_t) { return it + 1; }
int fuzz_guarded(void (*fn)(void*), void*) { return 0; }

// ---- random values ------------------------------------------------------------------------------------------------
static double uni() { return (double)(fuzz_rand() & 0xffffff) / 16777216.0; }     // [0, 1)
static double range(double a, double b) { return a + (b - a) * uni(); }
static int ri(int a, int b) { return a + (int)(fuzz_rand() % (uint32_t)(b - a + 1)); }   // [a, b]
static bool chance(int pct) { return (int)(fuzz_rand() % 100) < pct; }
static float fl(double x) { return (float)x; }
static float nudge(float x, int ulps) {                     // x moved by ulps units in the last place
    for (; ulps > 0; ulps--) x = nextafterf(x, HUGE_VALF);
    for (; ulps < 0; ulps++) x = nextafterf(x, -HUGE_VALF);
    return x;
}
static void setp(mrVertex& v, double x, double y, double z) {
    memset(&v, 0, sizeof v);
    v.pos.x = fl(x);
    v.pos.y = fl(y);
    v.pos.z = fl(z);
}
static void unit(double* d) {                               // a random direction
    double l;
    do {
        d[0] = range(-1, 1), d[1] = range(-1, 1), d[2] = range(-1, 1);
        l = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    } while (l < 1e-4 || l > 1);
    l = sqrt(l);
    d[0] /= l, d[1] /= l, d[2] /= l;
}

enum { MAXV = 4096 };
static mrVertex g_v0[MAXV], g_v[MAXV];
static uint16_t g_map_o[MAXV], g_map_f[MAXV];

enum Kind { LM_UNIFORM, LM_WELDED, LM_ON_VERTS, LM_SPHERE_TIES, LM_LATTICE, LM_FAR, LM_ONE, LM_NONFINITE, LM_HUGE, LM_TINY,
            LM_FLAT, LM_SHELL, LM_CLUSTERS, NKIND };
static const char* const k_kind[NKIND] = {"uniform", "welded", "on LOD-0 vertices", "sphere near-ties", "lattice ties",
                                          "~1e4 away and beyond", "n0 = 1", "NaN / inf / > 1e15", "huge magnitudes",
                                          "denormal-small", "flat and line", "car-like shell", "far clusters"};

static void uniform_box(mrVertex* v, int n, const double* c, double s) {
    for (int i = 0; i < n; i++) setp(v[i], c[0] + range(-s, s), c[1] + range(-s, s), c[2] + range(-s, s));
}
// one iteration's meshes; returns n0 and n
static void make(int kind, int& n0, int& n) {
    double c[3] = {range(-100, 100), range(-100, 100), range(-100, 100)};
    double s = pow(10.0, range(-3, 3));
    switch (kind) {
    case LM_UNIFORM:
        n0 = ri(1, 300), n = ri(1, 200);
        uniform_box(g_v0, n0, c, s);
        uniform_box(g_v, n, c, s * 1.5);
        break;
    case LM_WELDED: {
        const int u = ri(1, 30);
        mrVertex uq[30];
        uniform_box(uq, u, c, s);
        n0 = ri(1, 400), n = ri(1, 200);
        for (int i = 0; i < n0; i++) g_v0[i] = uq[fuzz_rand() % u];
        for (int i = 0; i < n; i++) {
            if (chance(50)) g_v[i] = uq[fuzz_rand() % u];
            else uniform_box(&g_v[i], 1, c, s);
        }
        break;
    }
    case LM_ON_VERTS:
        n0 = ri(1, 400), n = ri(1, 300);
        uniform_box(g_v0, n0, c, s);
        for (int i = 0; i < n0; i++) if (chance(20)) g_v0[i] = g_v0[fuzz_rand() % (i + 1)];   // some welds too
        for (int i = 0; i < n; i++) {
            if (chance(80)) g_v[i] = g_v0[fuzz_rand() % n0];
            else uniform_box(&g_v[i], 1, c, s);
        }
        break;
    case LM_SPHERE_TIES: {
        // each query has its own shell of LOD-0 points at one radius, every coordinate nudged by -2..2 ulps: the
        // distances agree to within float rounding, and which one the rule takes depends on the rounded best
        n = ri(1, 12);
        n0 = 0;
        const double R = s * range(0.01, 1);
        for (int q = 0; q < n; q++) {
            const double qc[3] = {c[0] + range(-50, 50) * s, c[1] + range(-50, 50) * s, c[2] + range(-50, 50) * s};
            setp(g_v[q], qc[0], qc[1], qc[2]);
            const int m = ri(2, 30);
            for (int i = 0; i < m && n0 < MAXV; i++, n0++) {
                double d[3];
                unit(d);
                mrVertex& p = g_v0[n0];
                setp(p, qc[0] + R * d[0], qc[1] + R * d[1], qc[2] + R * d[2]);
                p.pos.x = nudge(p.pos.x, ri(-2, 2));
                p.pos.y = nudge(p.pos.y, ri(-2, 2));
                p.pos.z = nudge(p.pos.z, ri(-2, 2));
                if (chance(10) && i) g_v0[n0] = g_v0[n0 - 1];                // and an exact duplicate now and then
            }
        }
        // shuffle LOD 0 (which k comes first matters)
        for (int i = n0 - 1; i > 0; i--) {
            const int k = (int)(fuzz_rand() % (uint32_t)(i + 1));
            mrVertex t = g_v0[i];
            g_v0[i] = g_v0[k];
            g_v0[k] = t;
        }
        if (chance(30)) for (int q = 0; q < n; q++) g_v[q].pos.y = nudge(g_v[q].pos.y, ri(-3, 3));
        break;
    }
    case LM_LATTICE: {
        // integer lattice (scaled by a power of 2: exact) with queries at cell centres, faces, edges: 8 / 4 / 2 exact ties
        const double h = ldexp(1.0, ri(-10, 10));
        const int a = ri(1, 7), b = ri(1, 7), cz = ri(1, 7);
        n0 = 0;
        for (int x = 0; x < a; x++)
            for (int y = 0; y < b; y++)
                for (int z = 0; z < cz; z++) setp(g_v0[n0++], c[0] + x * h, c[1] + y * h, c[2] + z * h);
        c[0] = (double)fl(c[0]), c[1] = (double)fl(c[1]), c[2] = (double)fl(c[2]);
        for (int i = n0 - 1; i > 0 && chance(70); i--) {                     // shuffled or not
            const int k = (int)(fuzz_rand() % (uint32_t)(i + 1));
            mrVertex t = g_v0[i];
            g_v0[i] = g_v0[k];
            g_v0[k] = t;
        }
        n = ri(1, 100);
        for (int i = 0; i < n; i++)
            setp(g_v[i], g_v0[0].pos.x + ri(-2, 2 * a) * 0.5 * h, g_v0[0].pos.y + ri(-2, 2 * b) * 0.5 * h,
                 g_v0[0].pos.z + ri(-2, 2 * cz) * 0.5 * h);
        break;
    }
    case LM_FAR: {
        // LOD 0 in a small box; queries at 1e4 (1 + k 1e-7) away -- either side of the 1e8 start, as rounded -- and far
        n0 = ri(1, 300), n = ri(1, 200);
        const double c0[3] = {0, 0, 0};
        const double bs = range(0.01, 10);
        uniform_box(g_v0, n0, c0, bs);
        for (int i = 0; i < n; i++) {
            double d[3];
            unit(d);
            const mrVertex& p = g_v0[fuzz_rand() % n0];
            double r;
            const int k = ri(0, 3);
            if (k == 0) r = 1e4 * (1 + ri(-30, 30) * 1e-7);
            else if (k == 1) r = range(9000, 11000);
            else if (k == 2) r = pow(10.0, range(4, 12));
            else r = range(0, 2 * bs);
            setp(g_v[i], p.pos.x + r * d[0], p.pos.y + r * d[1], p.pos.z + r * d[2]);
            if (chance(30)) {                                   // exactly 1e4 along an axis
                g_v[i] = p;
                (&g_v[i].pos.x)[ri(0, 2)] += chance(50) ? 1e4f : -1e4f;
            }
        }
        break;
    }
    case LM_ONE:
        n0 = 1, n = ri(1, 100);
        setp(g_v0[0], c[0], c[1], c[2]);
        for (int i = 0; i < n; i++) {
            double d[3];
            unit(d);
            const double r = chance(30) ? 1e4 * (1 + ri(-5, 5) * 1e-7) : pow(10.0, range(-6, 6));
            setp(g_v[i], c[0] + r * d[0], c[1] + r * d[1], c[2] + r * d[2]);
            if (chance(10)) g_v[i] = g_v0[0];
        }
        break;
    case LM_NONFINITE: {
        n0 = ri(1, 200), n = ri(1, 100);
        uniform_box(g_v0, n0, c, s);
        uniform_box(g_v, n, c, s);
        const int bad = ri(1, 3);
        for (int b = 0; b < bad; b++) {
            mrVertex* arr = chance(50) ? g_v0 : g_v;
            const int cnt = arr == g_v0 ? n0 : n;
            float* f = &arr[fuzz_rand() % cnt].pos.x + ri(0, 2);
            const int k = ri(0, 5);
            uint32_t u = k == 0 ? 0x7fc00000u : k == 1 ? 0xffc00000u : k == 2 ? 0x7f800000u : k == 3 ? 0xff800000u
                       : k == 4 ? (0x7f800001u | (fuzz_rand() & 0x3fffff)) : 0;                     // qNaN, inf, sNaN
            if (k == 5) *f = chance(50) ? 2e15f : -3e38f;                                         // finite, too big
            else memcpy(f, &u, 4);
        }
        break;
    }
    case LM_HUGE: {
        // a big base and a spread from tiny to huge: cancellation in the differences, cell indices on a big origin
        const double base = pow(10.0, range(6, 15)) * (chance(50) ? 1 : -1);
        const double cc[3] = {base * range(0.5, 1), base * range(-1, 1), chance(50) ? base : 0};
        const double sp = fabs(base) * pow(10.0, range(-9, 0));
        n0 = ri(1, 300), n = ri(1, 200);
        uniform_box(g_v0, n0, cc, sp);
        uniform_box(g_v, n, cc, sp * range(0.5, 2));
        for (int i = 0; i < n; i++) if (chance(30)) g_v[i] = g_v0[fuzz_rand() % n0];
        if (chance(20)) {                                       // past 1e15 (the fallback), up to 3e38
            const float big = fl(pow(10.0, range(15.1, 38.4)));
            (chance(50) ? g_v0[fuzz_rand() % n0].pos.x : g_v[fuzz_rand() % n].pos.z) = chance(50) ? big : -big;
        }
        break;
    }
    case LM_TINY: {
        // coordinates down at 1e-30 .. 1e-45 (denormals): the squares underflow float but not the x87's double
        const double sc = pow(10.0, range(-45, -30));
        const double cc[3] = {chance(50) ? 0 : sc * range(-10, 10), 0, chance(50) ? 0 : sc};
        n0 = ri(1, 200), n = ri(1, 100);
        uniform_box(g_v0, n0, cc, sc);
        uniform_box(g_v, n, cc, sc * 2);
        for (int i = 0; i < n; i++) if (chance(30)) g_v[i] = g_v0[fuzz_rand() % n0];
        if (chance(20)) g_v[0].pos.x = -0.0f;
        break;
    }
    case LM_FLAT: {
        n0 = ri(1, 400), n = ri(1, 200);
        uniform_box(g_v0, n0, c, s);
        uniform_box(g_v, n, c, s * 1.2);
        const int axes = ri(1, 2);                              // a plane or a line
        for (int a = 0; a < axes; a++) {
            const int ax = (a + ri(0, 2)) % 3;
            const float val = (&g_v0[0].pos.x)[ax];
            for (int i = 0; i < n0; i++) (&g_v0[i].pos.x)[ax] = val;
            for (int i = 0; i < n; i++) if (chance(50)) (&g_v[i].pos.x)[ax] = val;
        }
        break;
    }
    case LM_SHELL: {
        // a car-sized ellipsoid shell, welded seams, LODs from its vertices nudged: big enough for the game's threshold
        n0 = ri(1500, 4000), n = ri(500, 1500);
        const double ax[3] = {range(0.8, 1.0), range(0.5, 0.7), range(2.0, 2.4)};
        for (int i = 0; i < n0; i++) {
            if (i && chance(15)) { g_v0[i] = g_v0[fuzz_rand() % i]; continue; }
            double d[3];
            unit(d);
            setp(g_v0[i], ax[0] * d[0], 0.5 + ax[1] * d[1], ax[2] * d[2]);
        }
        for (int i = 0; i < n; i++) {
            g_v[i] = g_v0[fuzz_rand() % n0];
            if (chance(40)) {
                g_v[i].pos.x += fl(range(-0.02, 0.02));
                g_v[i].pos.z += fl(range(-0.02, 0.02));
            }
        }
        break;
    }
    case LM_CLUSTERS: {
        // a few tight clusters far apart; queries in, near and between them
        const int nc = ri(2, 6);
        double cs[6][3];
        for (int k = 0; k < nc; k++) for (int a = 0; a < 3; a++) cs[k][a] = range(-5000, 5000);
        n0 = ri(nc, 400), n = ri(1, 200);
        for (int i = 0; i < n0; i++) uniform_box(&g_v0[i], 1, cs[i % nc], range(0.001, 2));
        for (int i = 0; i < n; i++) {
            const double* a = cs[fuzz_rand() % nc], * b = cs[fuzz_rand() % nc];
            const double t = chance(50) ? range(0, 1) : 0;
            const double p[3] = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
            uniform_box(&g_v[i], 1, p, range(0.001, 5));
        }
        break;
    }
    }
}

// the original's answer's character: did it take a k other than the first with the least e (the rule's quirks)?
static int quirks(int n0, int n) {
    int q = 0;
    for (int j = 0; j < n; j++) {
        double m = 0;
        int km = -1;
        for (int k = 0; k < n0; k++) {
            const double e = lodmap_d2(g_v0[k], g_v[j]);
            if (km < 0 || e < m) m = e, km = k;
        }
        if (km >= 0 && m < 1e8 && g_map_o[j] != (uint16_t)km) q++;
    }
    return q;
}

static int random_run(int iterations) {
    long long entries = 0, fast_entries = 0, quirk = 0, fallbacks = 0, mismatches = 0, zero_far = 0;
    long long per_kind[NKIND] = {}, bad_kind[NKIND] = {}, fb_kind[NKIND] = {};
    static const unsigned pcs[3] = {_PC_24, _PC_53, _PC_64};
    for (int it = 0; it < iterations; it++) {
        int kind = (int)(fuzz_rand() % NKIND);
        if (kind == LM_SHELL && !chance(10)) kind = LM_UNIFORM;  // the big ones, less often
        int n0 = 0, n = 0;
        make(kind, n0, n);
        unsigned cw;
        _controlfp_s(&cw, pcs[it % 3], _MCW_PC);
        memset(g_map_o, 0xcd, sizeof g_map_o);
        memset(g_map_f, 0xab, sizeof g_map_f);
        lodmap_original(g_v0, n0, g_v, n, g_map_o);
        const bool fast = lodmap_search(g_v0, n0, g_v, n, g_map_f);
        if (!fast) lodmap_original(g_v0, n0, g_v, n, g_map_f), fallbacks++, fb_kind[kind]++;
        else fast_entries += n;
        _controlfp_s(&cw, _PC_53, _MCW_PC);
        per_kind[kind]++;
        entries += n;
        int bad = 0;
        for (int j = 0; j < n; j++)
            if (g_map_o[j] != g_map_f[j]) {
                if (mismatches + bad < 10)
                    printf("  MISMATCH iteration %d (%s, n0 %d, n %d, pc %d): vertex %d (%.9g %.9g %.9g) original %u, "
                           "fast %u\n", it, k_kind[kind], n0, n, it % 3, j, g_v[j].pos.x, g_v[j].pos.y, g_v[j].pos.z,
                           g_map_o[j], g_map_f[j]);
                bad++;
            }
        mismatches += bad;
        if (bad) bad_kind[kind]++;
        if (fast && n0 <= 600) quirk += quirks(n0, n);
        for (int j = 0; j < n; j++) zero_far += g_map_o[j] == 0 && kind == LM_FAR;
    }
    for (int k = 0; k < NKIND; k++)
        printf("  %-22s %7lld iterations, %5lld fallbacks, %lld with mismatches\n", k_kind[k], per_kind[k], fb_kind[k],
               bad_kind[k]);
    printf("lodmap: %d iterations, %lld map entries (%lld by the grid search, %lld runs fell back), %lld entries where "
           "the rule took other than the first least e; %lld mismatches\n",
           iterations, entries, fast_entries, fallbacks, quirk, mismatches);
    (void)zero_far;
    return mismatches ? 1 : 0;
}

// ---- real meshes ---------------------------------------------------------------------------------------------------
static int64_t ticks() {                                    // integers: the timing can't depend on the precision
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
static double secs(int64_t t) {                             // (call at 53-bit precision)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return (double)t / (double)f.QuadPart;
}
static int mesh_run(const char* path, int repeats) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("can't open %s\n", path); return 2; }
    char magic[4];
    int32_t nl = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "LODM", 4) || fread(&nl, 4, 1, f) != 1 || nl < 2 || nl > 5) {
        printf("%s isn't a LODM file\n", path);
        fclose(f);
        return 2;
    }
    static mrVertex lods[5][40000];
    int32_t cnt[5] = {};
    for (int l = 0; l < nl; l++) {
        if (fread(&cnt[l], 4, 1, f) != 1 || cnt[l] < 0 || cnt[l] > 40000 ||
            fread(lods[l], 32, (size_t)cnt[l], f) != (size_t)cnt[l]) {
            printf("%s: LOD %d is short\n", path, l);
            fclose(f);
            return 2;
        }
    }
    fclose(f);
    static uint16_t mo[40000], mf[40000], mb[40000];
    unsigned cw;
    int bad_total = 0;
    int64_t to_total = 0, tf_total = 0, tb_total = 0;
    printf("%s: LOD 0 %d vertices\n", path, cnt[0]);
    for (int pc = 0; pc < 2; pc++) {
        for (int l = 1; l < nl; l++) {
            _controlfp_s(&cw, pc ? _PC_53 : _PC_24, _MCW_PC);
            int64_t to = INT64_MAX, tf = INT64_MAX, tb = INT64_MAX;
            bool fast = true;
            for (int r = 0; r < repeats; r++) {
                int64_t t = ticks();
                lodmap_original(lods[0], cnt[0], lods[l], cnt[l], mo);
                t = ticks() - t;
                if (t < to) to = t;
                t = ticks();
                fast = lodmap_search(lods[0], cnt[0], lods[l], cnt[l], mf);
                if (!fast) lodmap_original(lods[0], cnt[0], lods[l], cnt[l], mf);
                t = ticks() - t;
                if (t < tf) tf = t;
                t = ticks();
                lodmap_build(lods[0], cnt[0], lods[l], cnt[l], mb);        // what Car::Car runs (with its threshold)
                t = ticks() - t;
                if (t < tb) tb = t;
            }
            _controlfp_s(&cw, _PC_53, _MCW_PC);
            if (!fast) printf("  LOD %d: the grid search fell back\n", l);
            int bad = 0, badb = 0;
            for (int j = 0; j < cnt[l]; j++) bad += mo[j] != mf[j], badb += mo[j] != mb[j];
            printf("  %s LOD %d: %5d vertices x %5d = %10lld steps; original %9.3f ms, grid %7.3f ms, Car::Car's "
                   "choice (%s) %9.3f ms; %d + %d mismatches\n",
                   pc ? "pc53" : "pc24", l, cnt[l], cnt[0], (long long)cnt[l] * cnt[0], secs(to) * 1e3, secs(tf) * 1e3,
                   (int64_t)cnt[0] * cnt[l] >= LODMAP_FAST_MIN ? "grid" : "original", secs(tb) * 1e3, bad, badb);
            bad_total += bad + badb;
            if (!pc) to_total += to, tf_total += tf, tb_total += tb;
        }
    }
    printf("one Car::Car's maps (LODs 1-%d, 24-bit precision): original %.3f s, grid %.4f s, as built %.4f s; "
           "%d mismatches\n", nl - 1, secs(to_total), secs(tf_total), secs(tb_total), bad_total);
    return bad_total ? 1 : 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc > 2 && !strcmp(argv[1], "mesh")) return mesh_run(argv[2], argc > 3 ? atoi(argv[3]) : 1);
    const int iterations = argc > 1 ? atoi(argv[1]) : 100000;
    if (argc > 2) g_state = (uint32_t)strtoul(argv[2], 0, 0) | 1;
    return random_run(iterations);
}
