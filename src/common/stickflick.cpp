#include "stickflick.h"

#include <cmath>
#include <cstring>

namespace {

constexpr int kSlots = 4;
constexpr int kHistory = 256;        // stick samples kept: over 1/30 s even at 1000 FPS
constexpr double kFastShare = 0.8;   // frames shorter than this share of a native frame use the real-time rules
constexpr double kTickEps = 1e-3;    // in native frames: a frame this close to a native frame boundary is on it
constexpr double kTimeEps = 1e-5;    // seconds
constexpr uint32_t kFlags = 0x84;    // action flags, one byte each, current frame
constexpr uint32_t kHoldTimes = 0xF0;  // float per action: how long it has been held
constexpr int kActR1 = 0;            // R1 (right hand light attack); the kick is R1 with a forward flick
constexpr int kActR1Alt = 7;         // written from the same R1 bit
constexpr int kActR2 = 5;            // R2; the jumping attack is R2 with a forward flick

struct Sample {
    double t;
    float m;  // largest stick component (what the neutral test compares with 0.2)
};

struct State {
    const uint8_t* manip = nullptr;
    uint32_t stamp = 0;
    double clock = 0.0;  // sum of the frame times this manipulator was updated with
    Sample hist[kHistory];
    int head = 0;
    int count = 0;
    bool has_prev = false;
    double t_prev = 0.0;
    float prev_v[4] = {};
    double start_tick[4] = {};  // native frame on which each direction's flick started (virtual clock)
    bool pending = false;       // forward flick that starts on the next update (a held press goes first)
    double pending_tick = 0.0;
    bool fast = false;          // this update used the real-time rules
    bool fwd_shown = false;     // the forward flick is shown this update
    float fwd = 0.0f;
    float fwd_prev = 0.0f;
    bool raw_prev[2] = {};
    bool holding[2] = {};
    double hold_until[2] = {};
};

StickFlickLayout g_layout;
StickFlickConsts g_consts;
double g_rate = 30.0;
State g_state[kSlots];
uint32_t g_stamp = 0;
StickFlickStats g_stats;

State* find_state(const uint8_t* manip) {
    for (State& s : g_state) {
        if (s.manip == manip) return &s;
    }
    return nullptr;
}

State& state_for(const uint8_t* manip) {
    if (State* s = find_state(manip)) {
        s->stamp = ++g_stamp;
        return *s;
    }
    State* pick = &g_state[0];
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

double period() {
    return 1.0 / g_rate;
}

// First native frame boundary at or after t.
double tick_ceil(double t) {
    return std::ceil(t * g_rate - kTickEps) / g_rate;
}

void push_sample(State& s, double t, float m) {
    s.hist[s.head] = Sample{t, m};
    s.head = (s.head + 1) % kHistory;
    if (s.count < kHistory) ++s.count;
}

// The stick's largest component at time x, interpolated between the updates around it.
float m_at(const State& s, double x) {
    if (s.count == 0) return 0.0f;
    const Sample* newer = nullptr;
    for (int i = 0; i < s.count; ++i) {
        const Sample& e = s.hist[(s.head - 1 - i + kHistory) % kHistory];
        if (e.t <= x + kTimeEps) {
            if (newer == nullptr || newer->t - e.t <= kTimeEps) return e.m;
            const double f = (x - e.t) / (newer->t - e.t);
            return static_cast<float>(e.m + (newer->m - e.m) * (f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f)));
        }
        newer = &e;
    }
    return newer->m;  // older than the history: the oldest sample
}

}  // namespace

void stickflick_configure(const StickFlickLayout& layout, const StickFlickConsts& consts, double rate) {
    g_layout = layout;
    g_consts = consts;
    g_rate = rate > 0.0 ? rate : 30.0;
}

float* stickflick_compute(uint8_t* manip, float* out, float dt, float side, float forward) {
    State& s = state_for(manip);
    const float on = *g_consts.on;
    const float window = *g_consts.window;
    const float neutral_max = *g_consts.neutral;
    float* timers = reinterpret_cast<float*>(manip + g_layout.timers);
    uint8_t* neutral = manip + g_layout.neutral;
    const double p = period();

    const bool fast = s.has_prev && dt > 0.0f && static_cast<double>(dt) < kFastShare * p;
    if (dt > 0.0f) s.clock += dt;
    const double t = s.clock;

    // The game's four directions: forward, back, side negative, side positive.
    const float v[4] = {forward, -forward, -side, side};
    float m = 0.0f;
    if (forward > 0.0f) m = forward;
    if (-forward > m) m = -forward;
    if (-side > m) m = -side;
    if (side > m) m = side;
    push_sample(s, t, m);

    // A held press that runs out on this update is seen before a forward flick that only starts after its
    // native frame (see stickflick_after_update).
    double release_tick = 0.0;
    bool releasing = false;
    if (fast) {
        for (int h = 0; h < 2; ++h) {
            if (s.holding[h] && t >= s.hold_until[h] - kTimeEps) {
                if (!releasing || s.hold_until[h] < release_tick) release_tick = s.hold_until[h];
                releasing = true;
            }
        }
    }

    const int shown_frames = static_cast<int>(std::floor(static_cast<double>(window) * g_rate + 1e-6)) + 1;
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    s.fwd_shown = false;
    for (int d = 0; d < 4; ++d) {
        float& timer = timers[d];
        bool show = false;
        if (timer > 0.0f) {
            if (v[d] < on) {
                timer = 0.0f;
            } else {
                show = fast ? t <= s.start_tick[d] + (shown_frames - 1) * p + kTimeEps : window >= timer;
                timer += dt;
            }
        } else if (v[d] >= on) {
            bool start = false;
            double tick = t;
            if (!fast) {
                start = *neutral != 0;  // the game's rule: neutral on the previous update
            } else if (d == 0 && s.pending) {
                start = true;
                tick = s.pending_tick;
            } else {
                // When the stick crossed the threshold, and the native frame that would first see it there.
                const float vp = s.prev_v[d];
                double frac = 0.0;
                if (vp < on && v[d] > vp) frac = static_cast<double>(on - vp) / static_cast<double>(v[d] - vp);
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
                tick = tick_ceil(s.t_prev + static_cast<double>(dt) * frac);
                start = m_at(s, tick - p) <= neutral_max;
            }
            if (start && fast && d == 0 && releasing && tick > release_tick + kTimeEps) {
                s.pending = true;  // the held press goes first; the flick starts on the next update
                s.pending_tick = tick;
            } else if (start) {
                if (d == 0) s.pending = false;
                s.start_tick[d] = tick;
                show = true;
                timer += dt;
                if (fast) ++g_stats.flicks;
            }
        }
        if (show) {
            if (d < 2) {
                out[1] = forward;
            } else {
                out[0] = side;
            }
            if (d == 0) s.fwd_shown = true;
        }
    }
    if (!fast || v[0] < on) s.pending = false;
    *neutral = neutral_max >= m ? 1 : 0;

    for (int d = 0; d < 4; ++d) s.prev_v[d] = v[d];
    s.t_prev = t;
    s.has_prev = true;
    s.fast = fast;
    s.fwd_prev = s.fwd;
    s.fwd = forward;
    return out;
}

void stickflick_after_update(uint8_t* manip) {
    State* s = find_state(manip);
    if (s == nullptr || !s->has_prev) return;
    uint8_t* flags = manip + kFlags;
    float* held = reinterpret_cast<float*>(manip + kHoldTimes);
    const float* timers = reinterpret_cast<const float*>(manip + g_layout.timers);
    const float on = *g_consts.on;
    const float neutral_max = *g_consts.neutral;
    const double t = s->clock;
    const double p = period();
    for (int h = 0; h < 2; ++h) {
        const int a = h == 0 ? kActR1 : kActR2;
        const bool raw = flags[a] != 0;
        if (s->holding[h]) {
            const bool release = !s->fast || s->fwd_shown || !raw || t >= s->hold_until[h] - kTimeEps;
            if (release) {
                s->holding[h] = false;
                if (s->fwd_shown) ++g_stats.held_into_flick;
                if (!raw) {
                    // A tap that ended while it was held: it still counts as a press, on this update.
                    flags[a] = 1;
                    if (h == 0) flags[kActR1Alt] = 1;
                }
            } else {
                flags[a] = 0;
                held[a] = 0.0f;
                if (h == 0) {
                    flags[kActR1Alt] = 0;
                    held[kActR1Alt] = 0.0f;
                }
            }
        } else if (raw && !s->raw_prev[h] && s->fast && !s->fwd_shown) {
            // A fresh press with the stick on its way out of neutral toward forward, before the flick
            // threshold, while a flick could still start in this native frame: wait for it, at most until
            // the native frame ends (a 30 FPS frame would have seen both together).
            const double tick = tick_ceil(t);
            if (tick - t > kTimeEps && s->fwd > 0.0f && s->fwd < on && s->fwd > s->fwd_prev && timers[0] <= 0.0f &&
                m_at(*s, tick - p) <= neutral_max) {
                s->holding[h] = true;
                s->hold_until[h] = tick;
                ++g_stats.holds;
                flags[a] = 0;
                held[a] = 0.0f;
                if (h == 0) {
                    flags[kActR1Alt] = 0;
                    held[kActR1Alt] = 0.0f;
                }
            }
        }
        s->raw_prev[h] = raw;
    }
}

StickFlickStats stickflick_stats() {
    return g_stats;
}
