// FilterShaderConstants and FilterSamplerStates logic against a fake device: every call goes through the filter to a
// device that applies it, and also straight to a reference device; after each call both must hold the same state.
// Repeats are frequent (values from a small palette), runs overlap and go out of range, and the cache is forgotten (a
// state block applied) and recording toggled now and then.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "d3dperf.cpp"

static uint32_t g_dev[256][4], g_ref[256][4];
static DWORD g_dev_ss[21][14], g_ref_ss[21][14];
static bool g_recording_model = false;
static long g_dev_calls = 0;

static HRESULT WINAPI fake_vsc(IDirect3DDevice9*, UINT start, const float* data, UINT count) {
    ++g_dev_calls;
    if (g_recording_model) return D3D_OK;  // recorded, not applied
    for (UINT i = 0; i < count && start + i < 256; ++i) std::memcpy(g_dev[start + i], data + i * 4, 16);
    return D3D_OK;
}
static HRESULT WINAPI fake_ss(IDirect3DDevice9*, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) {
    ++g_dev_calls;
    if (g_recording_model) return D3D_OK;
    const int idx = sampler_index(sampler);
    if (idx >= 0 && type > 0 && type < 14) g_dev_ss[idx][type] = value;
    return D3D_OK;
}

int main() {
    std::mt19937 rng(5);
    g_vsc_a = &fake_vsc;
    g_set_sampler = &fake_ss;
    g_vsc_enabled = true;
    g_ss_enabled = true;
    // Both devices start from the same (unknown to the filter) contents.
    for (auto& r : g_dev) for (auto& v : r) v = rng();
    std::memcpy(g_ref, g_dev, sizeof(g_dev));
    for (auto& s : g_dev_ss) for (auto& v : s) v = rng() % 4;
    std::memcpy(g_ref_ss, g_dev_ss, sizeof(g_dev_ss));
    uint32_t palette[8][4];
    for (auto& p : palette) for (auto& v : p) v = rng();
    int fails = 0;
    long calls = 0;
    for (int it = 0; it < 2000000 && fails < 10; ++it) {
        const int what = static_cast<int>(rng() % 100);
        if (what == 0) {
            forget_samplers();  // a state block applied: the device state changes behind the filter
            const UINT r = rng() % 256;
            g_dev[r][0] ^= 1;
            g_ref[r][0] ^= 1;
            continue;
        }
        if (what == 1) {
            g_recording = !g_recording;
            g_recording_model = g_recording;
            continue;
        }
        if (what < 60) {
            UINT start = rng() % 260, count = 1 + rng() % 40;
            float data[64 * 4];
            for (UINT i = 0; i < count; ++i) std::memcpy(&data[i * 4], palette[rng() % 8], 16);
            set_vsc(g_vsc_a, nullptr, start, data, count);
            ++calls;
            if (!g_recording_model)
                for (UINT i = 0; i < count && start + i < 256; ++i) std::memcpy(g_ref[start + i], &data[i * 4], 16);
            if (std::memcmp(g_dev, g_ref, sizeof(g_dev)) != 0) {
                std::printf("CONSTANTS MISMATCH at call %d (start %u count %u)\n", it, start, count);
                ++fails;
                std::memcpy(g_dev, g_ref, sizeof(g_dev));
            }
        } else {
            const DWORD sampler = (rng() % 8 == 0) ? 256 + rng() % 5 : rng() % 16;
            const auto type = static_cast<D3DSAMPLERSTATETYPE>(1 + rng() % 13);
            const DWORD value = rng() % 4;
            hook_set_sampler(nullptr, sampler, type, value);
            ++calls;
            const int idx = sampler_index(sampler);
            if (!g_recording_model) g_ref_ss[idx][type] = value;
            if (std::memcmp(g_dev_ss, g_ref_ss, sizeof(g_dev_ss)) != 0) {
                std::printf("SAMPLER MISMATCH at call %d\n", it);
                ++fails;
                std::memcpy(g_dev_ss, g_ref_ss, sizeof(g_dev_ss));
            }
        }
    }
    std::printf("%ld filtered calls checked, %ld reached the device (constants: %ld calls, %ld answered, %ld regs of %ld "
                "sent); %d mismatches\n",
                calls, g_dev_calls, g_vsc_calls, g_vsc_skipped, g_vsc_sent, g_vsc_regs, fails);
    return fails ? 1 : 0;
}
