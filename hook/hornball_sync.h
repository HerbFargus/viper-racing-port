// hornball_sync.h -- the horn balls kept the same on every port player's screen in a network race (docs/FIXES.md,
// "Multiplayer": horn balls).
//
// In the original a network car's throw isn't sent: the horn is one bit of its 15-a-second car packet, and every
// machine throws that car's ball itself, from its own estimate of where the car is -- on the network thread when the bit
// arrives (NetCar::do_horn: dropped when the packet is over 0.1 s old) and on the physics thread when the car is updated
// (Car::SetHorn: only if THIS player has the hack on). Each machine's copy then flies on its own, with its own ball size
// and mass (vrmod's hornball patch, per player).
//
// Between port players the throw is a message of its own, on a packet class the original drops unread (class 3: an
// original client dispatches only 1 and 2, an original server too), so a game with an original player -- or hosted on
// one -- carries on as before:
//   HELLO    client -> host, once connected: "I'm the port"
//   WELCOME  host -> every port client, on each HELLO: the host's ball size and mass, and the port players' user ids.
//            A client that has one is in sync mode for that game; every ball it makes has the host's size and mass.
//   THROW    the thrower -> host -> the other port players, reliable: the car, and its ball's state the moment it was
//            thrown (orientation, position, velocity). Each receiver puts its copy there and moves it on by the delay
//            (the car's own packet lag plus the time since the message came: gravity, no contacts), and ignores that
//            car's horn bit -- so a throw is never missed, never late by a packet, and starts where the thrower's did.
//   BALL     the thrower -> host -> the other port players, unreliable, with its car packets (15 a second) for 15 s
//            after each throw: the ball's position, velocity and spin, and the thrower's physics time. Each receiver
//            pulls its copy towards where that says the ball is now (Ball::Update), and puts it there outright when
//            it's over 4 m off -- so the copies stay together after contacts, each still colliding on its own machine
//            (a hit on this machine's own car is real here; the host's copy knocks its AI cars).
// The network thread (RaceClient / RaceServer) and the physics thread (Car::SetHorn, Ball::Update) hand throws over
// through per-car slots, a sequence number published after the data (a car throws at most every 2 s); the ball stream,
// written 15 times a second, goes through a seqlock (odd while it's being written).
#pragma once
#include <atomic>
#include <stdint.h>
#include <string.h>

namespace hbs {

enum : uint32_t { CLASS = 3, MAGIC = 0x42485056u /* "VPHB" */, VERSION = 1 };
enum : uint8_t { T_HELLO = 1, T_WELCOME = 2, T_THROW = 3, T_BALL = 4 };
enum : int { HDR = 9 };                    // [0..2] the session layer's, [3] the type, [4..7] MAGIC, [8] VERSION

struct Welcome {                           // after the header
    uint32_t mass, radius;                 // the host's create_ball immediates (bits): pounds, inches
    int32_t port_ids[8];                   // user ids of the port players (0: none)
};
struct Throw {                             // after the header
    int8_t car;                            // the car's index (the race packets' record index)
    uint8_t _pad[3];
    float s[15];                           // the ball just thrown: rotation (9), position (3), velocity (3)
};
struct BallState {                         // after the header
    int8_t car;
    uint8_t _pad[3];
    float time;                            // the thrower's physics time (orders the stream; its clock, not ours)
    float s[9];                            // position (3), velocity (3), spin (3)
};
static_assert(sizeof(Welcome) == 40 && sizeof(Throw) == 64 && sizeof(BallState) == 44, "hornball sync packets");
enum { STREAM_SECONDS = 15 };              // a ball is streamed this long after its throw

struct Slot {
    std::atomic<uint32_t> seq;             // bumped after the data is written
    float s[15];
    float recv_time, latency;              // a received throw: when it came (physics time), the car's packet lag
};

struct Stream {                            // a ball streamed to us: the network thread writes, Ball::Update reads
    std::atomic<uint32_t> seq;             // odd while being written
    float s[9];
    float recv_time, latency;              // when it came (our physics time), the car's packet lag
    float sender_time;                     // the newest taken (older ones dropped: the stream is unreliable)
};
struct State {
    // client (the network thread writes, the physics thread reads)
    std::atomic<int> active;               // a port host welcomed this client
    std::atomic<uint32_t> mass, radius;    // its ball
    std::atomic<int> remote[8];            // car i is another port player's: its throws come as messages
    std::atomic<int> local[8];             // car i is driven here: its throws go out as messages
    Slot out[8];                           // throws made here (physics -> network)
    uint32_t sent[8];
    Slot in[8];                            // throws received (network -> physics)
    uint32_t applied[8];
    Stream stream[8];                      // balls streamed by other port players
    const void* client;                    // the RaceClient the state belongs to
    uint8_t hello_sent;
    int32_t port_ids[8];
    // server
    const void* server;
    int32_t srv_ids[8];                    // the user id that said HELLO, per user index
};
inline State& S() {
    static State s;                        // zero-initialised (static storage)
    return s;
}

inline void put_header(uint8_t* p, uint8_t type) {
    p[3] = type;
    const uint32_t m = MAGIC;
    memcpy(p + 4, &m, 4);
    p[8] = (uint8_t)VERSION;
}
inline bool read_header(const uint8_t* p, int len, uint8_t* type) {
    if (len < HDR) return false;
    uint32_t m;
    memcpy(&m, p + 4, 4);
    if (m != MAGIC || p[8] != VERSION) return false;
    *type = p[3];
    return true;
}
inline void stream_write(Stream& st, const float* s, float recv_time, float latency, float sender_time) {
    st.seq.fetch_add(1, std::memory_order_acq_rel);          // odd: being written
    memcpy(st.s, s, sizeof st.s);
    st.recv_time = recv_time;
    st.latency = latency;
    st.sender_time = sender_time;
    st.seq.fetch_add(1, std::memory_order_release);          // even: done
}
// a consistent copy, or false (being written, or torn: try again next tick)
inline bool stream_read(const Stream& st, float* s, float* recv_time, float* latency) {
    const uint32_t a = st.seq.load(std::memory_order_acquire);
    if (a == 0 || (a & 1)) return false;
    memcpy(s, st.s, sizeof st.s);
    *recv_time = st.recv_time;
    *latency = st.latency;
    std::atomic_thread_fence(std::memory_order_acquire);
    return st.seq.load(std::memory_order_relaxed) == a;
}
inline void publish(Slot& sl, const float* s, float recv_time, float latency) {
    memcpy(sl.s, s, sizeof sl.s);
    sl.recv_time = recv_time;
    sl.latency = latency;
    sl.seq.fetch_add(1, std::memory_order_release);
}
// a fresh client: nothing pending either way (the sequence numbers taken as seen)
inline void reset_client(const void* client) {
    State& st = S();
    st.active.store(0);
    for (int i = 0; i < 8; i++) {
        st.remote[i].store(0);
        st.local[i].store(0);
        st.sent[i] = st.out[i].seq.load(std::memory_order_acquire);
        st.applied[i] = st.in[i].seq.load(std::memory_order_acquire);
        st.port_ids[i] = 0;
        st.stream[i].sender_time = -1e30f;
    }
    st.client = client;
    st.hello_sent = 0;
}

}  // namespace hbs
