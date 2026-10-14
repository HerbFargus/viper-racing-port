// car_parts.h -- the per-car lookup of a car's plain-named parts (FIX: docs/FIXES.md, "Drawing").
//
// The game loads every racing car's "<car>.car" set at the start of a race (LoadCars, in car-list order) and asks
// for some of each car's models by a plain name: ball.mod, wheel_1-3.mod, fwheel_1-3.mod, needle.mod, spin_l/r.mod,
// arm_*.mod, diskglow.mod and the brakelt.mod fallback. A plain name is looked for in every loaded set, newest first
// (krn_res.cpp), so every car got the parts of the newest car set that has them: one car's horn ball for every
// player, and a kart's wheels on the stock Vipers (seen in game). The stock cars' parts live in race.res, which
// loads before the cars.
//
// car_part asks the car's own set first ("<car>.car/<part>": a set-qualified name looks in that set only), then
// race.res's ("race.res/<part>": the stock part, for a car that has none of its own), and only then the plain name,
// the original's lookup. The models are only drawn (the ball's collision sphere and mass are fixed numbers, a wheel
// model is scaled to the .cf's tyre), so the physics, replays and network races are unchanged. A car's files are
// unchanged too: it keeps its plain-named parts, which the original exe still shares as before.
#pragma once
#include <stdio.h>
#include <stdint.h>
#include "port.h"

static inline const char* car_part(char* buf, size_t size, const char* car, const char* part) {
    typedef uint8_t(__cdecl * Exists_t)(const char*);
    const Exists_t exists = (Exists_t)0x00419d10;                  // ResourceExists
    if (VP_FIX && car && car[0]) {
        snprintf(buf, size, "%s.car/%s", car, part);
        if (exists(buf)) return buf;
        snprintf(buf, size, "race.res/%s", part);
        if (exists(buf)) return buf;
    }
    return part;
}
