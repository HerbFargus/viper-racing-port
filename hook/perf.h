// perf.h -- where a frame's time goes: [debug] perf=1 in viperport.ini (off by default).
//
// Measures, never changes anything: wall time from the standard library's steady clock (never the game's clocks, which a
// session records), added up per frame into buckets, and logged every 5 s and at exit:
//   readback   the 2D page's Lock: the 3D read back from the GPU (gl_core.cpp read_page) -- waits for the GPU to finish
//   3D         the game's 3D work handed to OpenGL (draws, clears: gl_core.cpp draw / clear)
//   2D         the page's Unlock: the 2D's pixels uploaded and drawn over the 3D (draw_page)
//   present    the frame to the window (blit, swap: present) -- waits for the GPU and for vsync
//   game+rest  everything else on the main thread between two frames: the game's own code, sound, input, the port
//   driver     of each of those, the time spent inside the GL driver's calls (the rest is the port's own work)
//   GL calls   the calls made to the driver a frame, and those skipped as already set (gl_table.cpp's state cache)
//   physics    each physics update (PhysTaskUpdate, on the physics thread: 0-4 ticks of 16 ms each)
// Off, each point costs a test of one flag.
#pragma once
#include <stdint.h>
#include <atomic>
#include <chrono>

void logf(const char* fmt, ...);

namespace perf {

enum Bucket { READBACK, DRAW3D, PAGE2D, PRESENT, N_MAIN };

inline bool on;                                  // [debug] perf
inline uint64_t g_sum[N_MAIN], g_count[N_MAIN];  // main thread: ns, calls, since the last report
inline uint64_t g_frames, g_frame_ns, g_worst_ns, g_last_frame, g_report_at;
inline uint64_t g_total_frames, g_total_ns, g_total_sum[N_MAIN], g_total_count[N_MAIN];
inline int g_cur = N_MAIN;                       // the bucket being timed (N_MAIN: none)
inline uint64_t g_drv[N_MAIN + 1], g_total_drv[N_MAIN + 1];   // ns inside the GL driver, per bucket
inline uint64_t g_gl_calls, g_gl_skipped;      // main thread: counted always (cheap), reported when on
inline uint64_t g_gl_last_calls, g_gl_last_skipped, g_gl_start_calls, g_gl_start_skipped;
inline std::atomic<uint64_t> g_phys_ns, g_phys_ticks, g_total_phys_ns, g_total_phys_ticks;

inline uint64_t now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Scope {                                   // a timed span on the main thread
    Bucket b;
    int prev;
    uint64_t t0;
    explicit Scope(Bucket bucket) : b(bucket), prev(g_cur), t0(on ? now() : 0) { if (t0) g_cur = b; }
    ~Scope() {
        if (!on || !t0) return;
        g_sum[b] += now() - t0;
        g_count[b]++;
        g_cur = prev;
    }
};

struct DrvScope {                                // one call into the GL driver (gl_table.cpp: VP_DRV gl_api.X(...))
    bool go = true;
    uint64_t t0 = on ? now() : 0;
    ~DrvScope() {
        if (t0) g_drv[g_cur] += now() - t0;
    }
};
#define VP_DRV for (perf::DrvScope vp_drv_; vp_drv_.go; vp_drv_.go = false)

struct PhysScope {                               // one physics update (its own thread)
    uint64_t t0;
    PhysScope() : t0(on ? now() : 0) {}
    ~PhysScope() {
        if (!on || !t0) return;
        const uint64_t d = now() - t0;
        g_phys_ns += d;
        g_phys_ticks++;
        g_total_phys_ns += d;
        g_total_phys_ticks++;
    }
};

inline double ms(uint64_t ns) { return ns / 1e6; }

inline void log_block(const char* what, uint64_t frames, uint64_t frame_ns, const uint64_t* sum, const uint64_t* count,
                      uint64_t phys_ns, uint64_t phys_ticks, uint64_t worst_ns, double secs, uint64_t gl_calls,
                      uint64_t gl_skipped, const uint64_t* drv) {
    if (!frames) return;
    const double f = (double)frames;
    uint64_t main = 0;
    for (int i = 0; i < N_MAIN; i++) main += sum[i];
    const uint64_t rest = frame_ns > main ? frame_ns - main : 0;
    logf("perf: %s: %llu frames in %.1f s (%.1f fps), %.2f ms a frame = readback %.2f (%.1f a frame) + 3D %.2f (%.0f calls)"
         " + 2D %.2f + present %.2f + game+rest %.2f; physics %.2f ms an update (%.0f updates/s)",
         what, (unsigned long long)frames, secs, frames / secs, ms(frame_ns) / f, ms(sum[READBACK]) / f, count[READBACK] / f, ms(sum[DRAW3D]) / f, count[DRAW3D] / f,
         ms(sum[PAGE2D]) / f, ms(sum[PRESENT]) / f, ms(rest) / f,
         phys_ticks ? ms(phys_ns) / (double)phys_ticks : 0.0, phys_ticks / secs);
    logf("perf: %s: inside the GL driver, a frame: readback %.2f, 3D %.2f, 2D %.2f, present %.2f, elsewhere %.2f ms", what,
         ms(drv[READBACK]) / f, ms(drv[DRAW3D]) / f, ms(drv[PAGE2D]) / f, ms(drv[PRESENT]) / f, ms(drv[N_MAIN]) / f);
    logf("perf: %s: GL calls %.0f a frame made, %.0f skipped as already set", what, (gl_calls - gl_skipped) / f, gl_skipped / f);
    if (worst_ns) logf("perf: %s: the slowest frame took %.1f ms", what, ms(worst_ns));
}

// the end of a frame (gl_core.cpp present): its time, and every 5 s a report
inline void frame() {
    if (!on) return;
    const uint64_t t = now();
    if (!g_last_frame) {                         // (the first frame starts the clock)
        g_last_frame = g_report_at = t;
        for (int i = 0; i < N_MAIN; i++) g_sum[i] = g_count[i] = 0;
        for (int i = 0; i <= N_MAIN; i++) g_drv[i] = 0;
        g_gl_last_calls = g_gl_start_calls = g_gl_calls, g_gl_last_skipped = g_gl_start_skipped = g_gl_skipped;
        return;
    }
    const uint64_t d = t - g_last_frame;
    g_last_frame = t;
    g_frames++, g_frame_ns += d;
    if (d > g_worst_ns) g_worst_ns = d;
    if (t - g_report_at < 5000000000ull) return;
    const uint64_t pn = g_phys_ns.exchange(0), pt = g_phys_ticks.exchange(0);
    log_block("last 5 s", g_frames, g_frame_ns, g_sum, g_count, pn, pt, g_worst_ns, (t - g_report_at) / 1e9,
              g_gl_calls - g_gl_last_calls, g_gl_skipped - g_gl_last_skipped, g_drv);
    for (int i = 0; i <= N_MAIN; i++) g_total_drv[i] += g_drv[i], g_drv[i] = 0;
    g_gl_last_calls = g_gl_calls, g_gl_last_skipped = g_gl_skipped;
    g_total_frames += g_frames, g_total_ns += g_frame_ns;
    for (int i = 0; i < N_MAIN; i++) g_total_sum[i] += g_sum[i], g_total_count[i] += g_count[i], g_sum[i] = g_count[i] = 0;
    g_frames = g_frame_ns = g_worst_ns = 0;
    g_report_at = t;
}

inline void report_exit() {
    if (!on) return;
    uint64_t sum[N_MAIN], count[N_MAIN];
    for (int i = 0; i < N_MAIN; i++) sum[i] = g_total_sum[i] + g_sum[i], count[i] = g_total_count[i] + g_count[i];
    const uint64_t frames = g_total_frames + g_frames, ns = g_total_ns + g_frame_ns;
    uint64_t drv[N_MAIN + 1];
    for (int i = 0; i <= N_MAIN; i++) drv[i] = g_total_drv[i] + g_drv[i];
    log_block("whole run", frames, ns, sum, count, g_total_phys_ns.load(), g_total_phys_ticks.load(), 0, ns / 1e9,
              g_gl_calls - g_gl_start_calls, g_gl_skipped - g_gl_start_skipped, drv);
}

}  // namespace perf
