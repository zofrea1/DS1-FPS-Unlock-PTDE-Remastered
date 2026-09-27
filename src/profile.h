#pragma once

#include <cstdint>

// Dark Souls Remastered 1.03.1, Steam executable dated 2022-10-11.
// SHA-1 9150CC63C617332ED3C2C66E7566ED67E3292DA0.
// Image base 0x140000000. RVAs below were checked against that file.

struct BuildProfile {
    const char* name;
    uint32_t timestamp;
    uint32_t image_size;
    uint32_t checksum;
    uint32_t qpc_iat;
    uint32_t pacer_first_return;
    uint32_t pacer_loop_return;
    uint32_t sim_vtable;
    uint32_t sim_fn;
    uint32_t fx_vtable;
    uint32_t fx_fn;
    uint32_t remo_slot;
    uint32_t remo_fn;
    uint32_t menu_query;
    uint32_t press_vtable;
    uint32_t press_fn;
    uint32_t press_alt_vtable;
    uint32_t press_alt_fn;
    uint32_t press_lookup;
    uint32_t press_alt_lookup;
    uint32_t repeat_vtable;
    uint32_t repeat_fn;
    uint32_t repeat_alt_vtable;
    uint32_t repeat_alt_fn;
    uint32_t repeat_lookup;
    uint32_t repeat_alt_lookup;
    uint32_t havok_call;
    uint32_t havok_fn;
    uint32_t flipper60_vtable;
    uint32_t flipper140_vtable;
};

inline constexpr BuildProfile kBuild{
    "2022 Steam 1.03.1 (9150CC63C617332ED3C2C66E7566ED67E3292DA0)",
    0x6344CA56,
    0x0319B000,
    0x02FF5493,
    0x2017344,
    0xCE3478,
    0xCE34AE,
    0x1A294B8,
    0x24F6B0,
    0x136B618,
    0x4F7450,
    0x1C71AF8,
    0x290400,
    0xED6750,
    0x14DB0B0,
    0xE190A0,
    0x14DB0B8,
    0xE190E0,
    0x19E570,
    0xE18740,
    0x14DADA8,
    0xE18370,
    0x14DADB0,
    0xE183B0,
    0x19E6B0,
    0xE17B80,
    0x15D58E,
    0x2A31D0,
    0x12AB268,
    0x12AB2C0,
};
