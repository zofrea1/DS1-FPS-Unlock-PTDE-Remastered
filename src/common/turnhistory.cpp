#include "turnhistory.h"

#include <cmath>
#include <cstring>

namespace {

constexpr int kSlots = 8;
constexpr int kHistory = 256;
constexpr double kFastShare = 0.8;
constexpr double kTickEps = 1e-3;
constexpr double kTimeEps = 1e-5;
constexpr double kPi = 3.1415927410125732;   // the games' float pi
constexpr double kTwoPi = 6.2831854820251465;  // and 2 pi
constexpr double kDegToRad = 0.017453292384743690;

struct Sample {
    double t;
    float angle;
    uint32_t dir;  // raw bits, handed back as the game's value
};

struct State {
    const uint8_t* turn = nullptr;
    uint32_t stamp = 0;
    double clock = 0.0;
    float last_dt = 0.0f;
    Sample hist[kHistory];
    int head = 0;
    int count = 0;
    double fired_tick = -1.0;
};

TurnHistoryLayout g_layout;
double g_rate = 30.0;
State g_state[kSlots];
uint32_t g_stamp = 0;

State* find_state(const uint8_t* turn) {
    for (State& s : g_state) {
        if (s.turn == turn) return &s;
    }
    return nullptr;
}

State& state_for(const uint8_t* turn) {
    if (State* s = find_state(turn)) {
        s->stamp = ++g_stamp;
        return *s;
    }
    State* pick = &g_state[0];
    for (State& s : g_state) {
        if (s.turn == nullptr) {
            pick = &s;
            break;
        }
        if (s.stamp < pick->stamp) pick = &s;
    }
    *pick = State();
    pick->turn = turn;
    pick->stamp = ++g_stamp;
    return *pick;
}

// The games' wrap of an angle difference into [-pi, pi].
double wrap(double x) {
    const double c = x < 0.0 ? -kPi : kPi;
    return x - static_cast<double>(static_cast<int>((c + x) / kTwoPi)) * kTwoPi;
}

const Sample& at(const State& s, int back) {  // back = 0 newest
    return s.hist[(s.head - 1 - back + kHistory) % kHistory];
}

// Newest sample at or before time x (the oldest one if none is).
int sample_at(const State& s, double x) {
    for (int k = 0; k < s.count; ++k) {
        if (at(s, k).t <= x + kTimeEps) return k;
    }
    return s.count - 1;
}

}  // namespace

void turnhistory_configure(const TurnHistoryLayout& layout, double rate) {
    g_layout = layout;
    g_rate = rate > 0.0 ? rate : 30.0;
}

void turnhistory_record(const uint8_t* turn, float dt) {
    State& s = state_for(turn);
    const int n = *reinterpret_cast<const int*>(turn + g_layout.size);
    const int idx = *reinterpret_cast<const int*>(turn + g_layout.index);
    if (n < 1 || n > 64 || idx < 0 || idx >= n) return;
    // The game resets the ring (every entry = the character's facing) on its own; then the entry before this one no
    // longer is the last sample, and the history starts again from here.
    if (s.count > 0 && n >= 2) {
        uint32_t prev = 0, last = 0;
        std::memcpy(&prev, turn + g_layout.angle + 4 * ((idx - 1 + n) % n), 4);
        std::memcpy(&last, &at(s, 0).angle, 4);
        if (prev != last) {
            s.count = 0;
            s.fired_tick = -1.0;
        }
    }
    if (dt > 0.0f) s.clock += dt;
    s.last_dt = dt;
    Sample e;
    e.t = s.clock;
    std::memcpy(&e.angle, turn + g_layout.angle + 4 * idx, 4);
    std::memcpy(&e.dir, turn + g_layout.dir + 4 * idx, 4);
    s.hist[s.head] = e;
    s.head = (s.head + 1) % kHistory;
    if (s.count < kHistory) ++s.count;
}

int turnhistory_check(const uint8_t* turn, float threshold_deg, uint32_t* out) {
    State* s = find_state(turn);
    const double p = 1.0 / g_rate;
    if (s == nullptr || s->count < 2 || !(s->last_dt > 0.0f) || static_cast<double>(s->last_dt) >= kFastShare * p) {
        return -1;
    }
    const int n = *reinterpret_cast<const int*>(turn + g_layout.size);
    if (n < 2 || n > 64) return -1;
    const double now = s->clock;
    const double tick = std::ceil(now * g_rate - kTickEps) / g_rate;  // the native frame the game looks at next
    const int ref_k = sample_at(*s, tick - (n - 1) * p);
    const Sample& ref = at(*s, ref_k);
    // Like the game: the newest entry first, then the older ones in order, keeping the strictly largest swing.
    int best_k = 0;
    float best = static_cast<float>(std::fabs(wrap(static_cast<double>(at(*s, 0).angle) - ref.angle)));
    for (int k = ref_k - 1; k >= 1; --k) {
        const float d = static_cast<float>(std::fabs(wrap(static_cast<double>(at(*s, k).angle) - ref.angle)));
        if (best < d) {
            best = d;
            best_k = k;
        }
    }
    if (!(static_cast<float>(threshold_deg * kDegToRad) < best)) return 0;
    if (s->fired_tick == tick) return 0;  // the game looks once per native frame
    s->fired_tick = tick;
    // The game hands back the turn of the entry one frame before the largest swing.
    const int before = sample_at(*s, at(*s, best_k).t - p);
    *out = at(*s, before).dir;
    return 1;
}
