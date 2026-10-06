// w32_handle.cpp -- the stand-ins' handle table (see w32_handle.h). Portable C++: the same on Windows and Linux.
#include "w32_handle.h"
#include <mutex>
#include <vector>
#include <deque>

namespace w32 {

namespace {
const uint32_t FIRST = 0x1004;                   // handle values: FIRST + 4 * slot
const uint32_t SLOTS = 16384;
std::mutex g_mu;
std::vector<std::shared_ptr<Object>> g_slots;    // index = (handle - FIRST) / 4
std::deque<uint32_t> g_free;                     // freed slots, reused oldest first (a stale handle stays dead long)
uint32_t g_open;

bool index_of(Handle h, uint32_t* i) {
    if (h < FIRST || (h - FIRST) % 4) return false;
    *i = (h - FIRST) / 4;
    return *i < g_slots.size();
}

thread_local uint32_t t_last_error;
}  // namespace

uint32_t Object::wait(uint32_t) {
    set_last_error(ERR_INVALID_HANDLE);
    return WAIT_FAILED_;
}

Handle add(std::shared_ptr<Object> obj) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t i;
    if (g_slots.size() < SLOTS) {                // fresh slots first, so a closed handle's value comes back last
        i = (uint32_t)g_slots.size();
        g_slots.push_back(std::move(obj));
    } else if (!g_free.empty()) {
        i = g_free.front();
        g_free.pop_front();
        g_slots[i] = std::move(obj);
    } else {
        return 0;
    }
    g_open++;
    return FIRST + 4 * i;
}

std::shared_ptr<Object> get(Handle h) {
    std::lock_guard<std::mutex> lk(g_mu);
    uint32_t i;
    if (!index_of(h, &i)) return nullptr;
    return g_slots[i];
}

std::shared_ptr<Object> get(Handle h, Kind k) {
    std::shared_ptr<Object> o = get(h);
    return o && o->kind == k ? o : nullptr;
}

bool close(Handle h) {
    std::shared_ptr<Object> o;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        uint32_t i;
        if (!index_of(h, &i) || !g_slots[i]) {
            set_last_error(ERR_INVALID_HANDLE);
            return false;
        }
        o = std::move(g_slots[i]);
        g_slots[i].reset();
        g_free.push_back(i);
        g_open--;
    }
    if (o.use_count() == 1) o->closed();         // the last handle (a duplicate or a live thread may still hold it)
    return true;
}

Handle duplicate(Handle h) {
    std::shared_ptr<Object> o = get(h);
    if (!o) {
        set_last_error(ERR_INVALID_HANDLE);
        return 0;
    }
    return add(o);
}

uint32_t open_count() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_open;
}

void set_last_error(uint32_t e) { t_last_error = e; }
uint32_t last_error() { return t_last_error; }

}  // namespace w32
