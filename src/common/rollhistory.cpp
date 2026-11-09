#include "rollhistory.h"

#include <cmath>

namespace {

constexpr int kSlots = 4;
constexpr int kHistory = 256;       // samples kept per manipulator: over 4/30 s even at 1000 FPS
constexpr double kFastShare = 0.8;  // frames shorter than this share of a native frame use real time
constexpr double kTimeEps = 1e-5;

struct Sample {
    double t;
    float y;
    float x;
};

struct State {
    const uint8_t* manip = nullptr;
    uint32_t stamp = 0;
    double clock = 0.0;
    Sample hist[kHistory];
    int head = 0;
    int count = 0;
};

RollHistoryLayout g_layout;
const int* g_ring_size = nullptr;
double g_rate = 30.0;
State g_state[kSlots];
uint32_t g_stamp = 0;

State& state_for(const uint8_t* manip) {
    State* pick = &g_state[0];
    for (State& s : g_state) {
        if (s.manip == manip) {
            s.stamp = ++g_stamp;
            return s;
        }
    }
    for (State& s : g_state) {
        if (s.manip == nullptr) {
            pick = &s;
            break;
        }
        if (s.stamp < pick->stamp) pick = &s;
    }
    *pick = State();
    pick->manip = manip;
    pick->stamp = ++g_stamp;
    return *pick;
}

// The game's own walk: from the newest entry back to the one after it (ring size - 1 entries), weight 1, then
// times 0.9 (as a float) per entry, keeping the strictly largest magnitude per axis.
void game_walk(const uint8_t* manip, int n, float* ymax, float* xmax) {
    const auto* ring_y = reinterpret_cast<const float*>(manip + g_layout.ring_y);
    const auto* ring_x = reinterpret_cast<const float*>(manip + g_layout.ring_x);
    const int index = *reinterpret_cast<const int*>(manip + g_layout.index);
    float y = 0.0f, x = 0.0f, w = 1.0f;
    int i = ((index % n) + n) % n;
    const int end = (((index + 1) % n) + n) % n;
    while (i != end) {
        const float vy = ring_y[i] * w;
        const float vx = ring_x[i] * w;
        if (std::fabs(vy) > std::fabs(y)) y = vy;
        if (std::fabs(vx) > std::fabs(x)) x = vx;
        w = w * 0.9f;
        i = (i + n - 1) % n;
    }
    *ymax = y;
    *xmax = x;
}

}  // namespace

void rollhistory_configure(const RollHistoryLayout& layout, const int* ring_size, double rate) {
    g_layout = layout;
    g_ring_size = ring_size;
    g_rate = rate > 0.0 ? rate : 30.0;
}

void rollhistory_compute(uint8_t* manip, float dt, float* out) {
    int n = g_ring_size ? *g_ring_size : 6;
    if (n < 2 || n > 64) n = 6;
    const auto* ring_y = reinterpret_cast<const float*>(manip + g_layout.ring_y);
    const auto* ring_x = reinterpret_cast<const float*>(manip + g_layout.ring_x);
    const int index = *reinterpret_cast<const int*>(manip + g_layout.index);
    const int cur = ((index % n) + n) % n;
    const float cy = ring_y[cur];
    const float cx = ring_x[cur];
    out[2] = cy;
    out[3] = cx;

    State& s = state_for(manip);
    const bool have = s.count > 0;
    if (dt > 0.0f) s.clock += dt;
    s.hist[s.head] = Sample{s.clock, cy, cx};
    s.head = (s.head + 1) % kHistory;
    if (s.count < kHistory) ++s.count;

    const double p = 1.0 / g_rate;
    if (!have || !(dt > 0.0f) || static_cast<double>(dt) >= kFastShare * p) {
        game_walk(manip, n, &out[0], &out[1]);
        return;
    }
    // Real time: every sample younger than (ring size - 2) native frames, weight 0.9 per native frame of age.
    const double span = (n - 2) * p + kTimeEps;
    float y = 0.0f, x = 0.0f;
    for (int k = 0; k < s.count; ++k) {
        const Sample& e = s.hist[(s.head - 1 - k + kHistory) % kHistory];
        const double age = s.clock - e.t;
        if (age > span) break;
        const float w = static_cast<float>(std::pow(0.9, age * g_rate));
        const float vy = e.y * w;
        const float vx = e.x * w;
        if (std::fabs(vy) > std::fabs(y)) y = vy;
        if (std::fabs(vx) > std::fabs(x)) x = vx;
    }
    out[0] = y;
    out[1] = x;
}
