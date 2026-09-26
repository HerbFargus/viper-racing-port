// fuzz.h -- run a pure rewrite against the original on random inputs, outside the game.
//
// Fuzz<F>::run<NEW, FOOTPRINT>(original, iterations) builds random arguments for F's parameter types --
// floats from a mix of ordinary, tiny, huge and special values; pointers into a random-filled arena, sized
// by what they point at, sometimes the same pointer twice (an output that is also an input) -- calls the
// original on one copy of the arena and the rewrite on another, and compares the arenas and the return
// values bit for bit. Only functions whose footprint says `pure` are run (the rest need the game).
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <tuple>
#include <type_traits>

struct Footprint;
typedef int (*FuzzRun)(uint32_t original, int iterations);
struct FuzzReg {
    uint32_t v10; const char* name; FuzzRun run; FuzzReg* next;
    static FuzzReg*& head() { static FuzzReg* h; return h; }
    FuzzReg(uint32_t v, const char* n, FuzzRun r) : v10(v), name(n), run(r), next(head()) { head() = this; }
};

// ---- random values ------------------------------------------------------------------------------------------
uint32_t fuzz_rand();                       // xorshift, test/fuzz.cpp
float fuzz_float();                         // a float from the mix
extern bool g_fuzz_specials;                // include NaN / inf / denormals in the mix

// ---- the arena: every pointer argument points into it -------------------------------------------------------
enum { ARENA = 4096 };
struct Arena { alignas(16) uint8_t a[ARENA], b[ARENA]; uint32_t used; };

template <typename T> struct Pointee { enum { size = sizeof(T) }; };
template <> struct Pointee<float> { enum { size = 64 }; };   // a float* is an array (MatrixSet: 9)
template <> struct Pointee<void> { enum { size = 64 }; };

// one argument, for the original (in arena a) and the rewrite (the same offset in arena b)
template <typename T> struct Arg {
    static void make(Arena&, T& x, T& y, uintptr_t*, int) {
        if constexpr (std::is_floating_point_v<T>) x = y = (T)fuzz_float();
        else x = y = (T)(fuzz_rand() % 64);
    }
};
template <typename T> struct Arg<T*> {
    static void make(Arena& ar, T*& x, T*& y, uintptr_t* prev, int nprev) {
        typedef std::remove_const_t<T> U;
        // a third of the time, alias an earlier pointer argument of the same type
        for (int i = 0; i < nprev; i++)
            if (prev[i * 2 + 1] == (uint32_t)Pointee<U>::size && fuzz_rand() % 3 == 0) {
                x = (T*)(ar.a + prev[i * 2]);
                y = (T*)(ar.b + prev[i * 2]);
                return;
            }
        uint32_t size = (uint32_t)Pointee<U>::size, off = (ar.used + 15) & ~15u;
        ar.used = off + size;
        x = (T*)(ar.a + off);
        y = (T*)(ar.b + off);
        prev[nprev * 2] = off;
        prev[nprev * 2 + 1] = size;
    }
};

void fuzz_fill(Arena& ar);                  // random floats through both halves, identically
int fuzz_report(int it, const Arena& ar, const void* ro, const void* rn, size_t rs);   // returns it + 1

// run fn(arg) under a structured-exception guard: nonzero if it faulted (test/fuzz.cpp)
int fuzz_guarded(void (*fn)(void*), void* arg);
template <typename... A> struct FpCtx {
    std::tuple<A...>* args;
    Footprint* fp;
    void (*footprint)(Footprint&, A...);
    static void call(void* c) {
        FpCtx* k = (FpCtx*)c;
        std::apply([&](A... x) { k->footprint(*k->fp, x...); }, *k->args);
    }
};

template <typename F> struct Fuzz;
#define VP_FUZZ_CC(CC)                                                                                     \
    template <typename R, typename... A> struct Fuzz<R(CC*)(A...)> {                                     \
        typedef R(CC* Fn)(A...);                                                                         \
        template <Fn NEW, void (*FP)(Footprint&, A...)>                                                  \
        static int run(uint32_t original, int iterations) {                                              \
            static Arena ar;                                                                             \
            for (int it = 0; it < iterations; it++) {                                                    \
                ar.used = 0;                                                                             \
                fuzz_fill(ar);                                                                           \
                std::tuple<A...> xa, xb;                                                                 \
                uintptr_t prev[32];                                                                      \
                int n = 0;                                                                               \
                std::apply([&](A&... x) {                                                                \
                    std::apply([&](A&... y) { (Arg<A>::make(ar, x, y, prev, n++), ...); }, xb);          \
                }, xa);                                                                                  \
                if (it == 0) {                   /* only pure functions run outside the game */          \
                    /* a footprint may follow pointers, which here are random: guard it */               \
                    Footprint fp;                                                                        \
                    FpCtx<A...> ctx = {&xa, &fp, FP};                                                    \
                    if (fuzz_guarded(&FpCtx<A...>::call, &ctx) || !fp.pure) return -1;                   \
                }                                                                                        \
                if constexpr (std::is_void_v<R>) {                                                     \
                    std::apply([&](A... x) { ((Fn)original)(x...); }, xa);                               \
                    std::apply([&](A... y) { NEW(y...); }, xb);                                          \
                    if (memcmp(ar.a, ar.b, ARENA)) return fuzz_report(it, ar, 0, 0, 0);             \
                } else {                                                                                 \
                    R ro = std::apply([&](A... x) { return ((Fn)original)(x...); }, xa);                 \
                    R rn = std::apply([&](A... y) { return NEW(y...); }, xb);                            \
                    if (memcmp(ar.a, ar.b, ARENA) || memcmp(&ro, &rn, sizeof ro))                        \
                        return fuzz_report(it, ar, &ro, &rn, sizeof ro);                             \
                }                                                                                        \
            }                                                                                            \
            return 0;                                                                                    \
        }                                                                                                \
    };
VP_FUZZ_CC(__cdecl)
VP_FUZZ_CC(__fastcall)
VP_FUZZ_CC(__stdcall)
#undef VP_FUZZ_CC
