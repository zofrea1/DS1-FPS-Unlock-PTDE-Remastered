#include "controller.h"

#include "log.h"
#include "xinput_proxy.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace {

constexpr uint32_t kImageBase = 0x400000;

// Bind: call the game's GUID matcher (stdcall, 16-byte id, returns a slot or -1).
constexpr uint32_t kMatchCall = 0x643974;
constexpr uint32_t kMatchFn = 0x580FA0;
// Poll prologue: read the stored slot, and leave for the DirectInput path when it is negative.
constexpr uint32_t kPollIndex = 0x643B46;
constexpr uint32_t kPollResume = 0x643B55;   // lea ecx, [esp+10h] ; the state buffer
constexpr uint32_t kPollDinput = 0x643C18;
// The poll's XInputGetState call, replaced so an idle bound slot can yield to one in use.
constexpr uint32_t kPollGetState = 0x643B5B;
// After that call: a failure currently unbinds the pad for good.
constexpr uint32_t kPollResult = 0x643B60;
constexpr uint32_t kPollSuccess = 0x643B68;
constexpr uint32_t kPollUnbind = 0x643E71;

constexpr uint32_t kSlotOffset = 0x164;
constexpr uint32_t kDinputOffset = 0xEC;
constexpr unsigned kGraceMs = 500;

constexpr uint8_t kMatchExpect[5] = {0xE8, 0x27, 0xD6, 0xF3, 0xFF};
constexpr uint8_t kPollIndexExpect[15] = {0x8B, 0x86, 0x64, 0x01, 0x00, 0x00, 0x57, 0x85, 0xC0,
                                          0x0F, 0x8C, 0xC3, 0x00, 0x00, 0x00};
constexpr uint8_t kPollGetExpect[5] = {0xE8, 0xE2, 0x69, 0x47, 0x00};
constexpr uint8_t kPollResultExpect[8] = {0x85, 0xC0, 0x0F, 0x85, 0x09, 0x03, 0x00, 0x00};

#pragma pack(push, 1)
struct PadState {
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger;
    uint8_t right_trigger;
    int16_t lx, ly, rx, ry;
};
struct PadCaps {
    uint8_t type;
    uint8_t subtype;
    uint16_t flags;
    uint8_t rest[16];
};
#pragma pack(pop)

static_assert(sizeof(PadState) == 16, "XINPUT_STATE is 16 bytes");
static_assert(sizeof(PadCaps) == 20, "XINPUT_CAPABILITIES is 20 bytes");

using MatchFn = int(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t);
MatchFn g_match = reinterpret_cast<MatchFn>(kMatchFn);

// Set by the poll prologue so the GetState wrapper can retarget this device.
// The pointer itself is volatile: the cave writes it from outside C++.
uint8_t* volatile g_poll_device = nullptr;
ULONGLONG g_down_since = 0;

bool moved(int16_t v) {
    return v > 8000 || v < -8000;
}

bool idle_pad(const PadState& pad) {
    return pad.buttons == 0 && pad.left_trigger <= 30 && pad.right_trigger <= 30 && !moved(pad.lx) &&
           !moved(pad.ly) && !moved(pad.rx) && !moved(pad.ry);
}

// Wheels, flight sticks and music devices are not the pad the game should steal.
bool rejected_subtype(uint8_t subtype) {
    switch (subtype) {
    case 0x02:  // wheel
    case 0x04:  // flight stick
    case 0x05:  // dance pad
    case 0x06:  // guitar
    case 0x07:  // guitar alternate
    case 0x08:  // drum kit
    case 0x0B:  // guitar bass
        return true;
    default:
        return false;
    }
}

bool read_slot(int index, PadState* pad) {
    if (index < 0 || index > 3) {
        return false;
    }
    PadState state{};
    if (xinput_get_state(static_cast<DWORD>(index), &state) != 0) {
        return false;
    }
    PadCaps caps{};
    if (xinput_get_caps(static_cast<DWORD>(index), &caps) == 0 && rejected_subtype(caps.subtype)) {
        return false;
    }
    if (pad) {
        *pad = state;
    }
    return true;
}

// A pad the player is holding wins. Otherwise the lowest connected gamepad slot.
int pick_gamepad(int skip) {
    int idle = -1;
    for (int i = 0; i < 4; ++i) {
        if (i == skip) {
            continue;
        }
        PadState state{};
        if (!read_slot(i, &state)) {
            continue;
        }
        if (!idle_pad(state)) {
            return i;
        }
        if (idle < 0) {
            idle = i;
        }
    }
    return idle;
}

int __stdcall hook_match(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const int idx = g_match(a, b, c, d);
    if (idx >= 0 && read_slot(idx, nullptr)) {
        return idx;
    }
    const int picked = pick_gamepad(-1);
    if (picked >= 0) {
        LOG_INFO("Controller: matcher returned %d, which is not a connected gamepad; using slot %d", idx, picked);
        return picked;
    }
    return -1;
}

// The poll calls this with edi already saved by the prologue cave. A DirectInput
// device (+0xEC) is left alone; an unbound object adopts a gamepad.
int __stdcall adopt_pad(uint8_t* self) {
    if (!self || *reinterpret_cast<uint32_t*>(self + kDinputOffset) != 0) {
        return -1;
    }
    const int picked = pick_gamepad(-1);
    if (picked >= 0) {
        LOG_INFO("Controller: no XInput slot was bound; using slot %d", picked);
    }
    return picked;
}

DWORD __stdcall poll_get_state(DWORD index, void* state) {
    DWORD result = xinput_get_state(index, state);
    uint8_t* self = g_poll_device;
    if (result == 0) {
        g_down_since = 0;
        if (self && state && idle_pad(*static_cast<PadState*>(state))) {
            const int hot = pick_gamepad(static_cast<int>(index));
            // pick_gamepad also returns an idle slot. Only a pad that is actually
            // being held may take over from the idle one.
            if (hot >= 0 && hot != static_cast<int>(index)) {
                PadState other{};
                if (read_slot(hot, &other) && !idle_pad(other)) {
                    std::memcpy(state, &other, sizeof(other));
                    *reinterpret_cast<int*>(self + kSlotOffset) = hot;
                    LOG_INFO("Controller: slot %u is idle; following the pad in use on slot %d", index, hot);
                }
            }
        }
    }
    return result;
}

// 0 = `state` now holds a reading the game should keep. Nonzero = unbind.
int __stdcall recover_pad(uint8_t* self, void* state) {
    if (!self || !state) {
        return 1;
    }
    const int cur = *reinterpret_cast<int*>(self + kSlotOffset);
    const int alt = pick_gamepad(cur);
    PadState other{};
    const bool alt_ok = alt >= 0 && read_slot(alt, &other);
    // An idle second pad must not replace one that only blipped. A pad that is
    // being held takes over immediately.
    if (alt_ok && !idle_pad(other)) {
        std::memcpy(state, &other, sizeof(other));
        *reinterpret_cast<int*>(self + kSlotOffset) = alt;
        g_down_since = 0;
        LOG_INFO("Controller: slot %d dropped; using slot %d", cur, alt);
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_down_since == 0) {
        g_down_since = now;
    }
    if (now - g_down_since < kGraceMs) {
        std::memset(state, 0, sizeof(PadState));
        return 0;
    }
    g_down_since = 0;
    if (alt_ok) {
        std::memcpy(state, &other, sizeof(other));
        *reinterpret_cast<int*>(self + kSlotOffset) = alt;
        LOG_INFO("Controller: slot %d stayed disconnected; using slot %d", cur, alt);
        return 0;
    }
    LOG_INFO("Controller: slot %d stayed disconnected; letting the game release it", cur);
    return 1;
}

bool same_bytes(uint32_t address, const uint8_t* bytes, size_t size) {
    return std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

bool write_code(uint32_t address, const uint8_t* bytes, size_t size) {
    auto* p = reinterpret_cast<uint8_t*>(address);
    DWORD old = 0;
    if (!VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(p, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), p, size);
    DWORD ignored = 0;
    VirtualProtect(p, size, old, &ignored);
    return true;
}

bool write_branch(uint32_t site, size_t size, uint8_t opcode, const void* target) {
    uint8_t bytes[16];
    if (size < 5 || size > sizeof(bytes)) {
        return false;
    }
    bytes[0] = opcode;
    const auto rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(bytes + 1, &rel, 4);
    for (size_t i = 5; i < size; ++i) {
        bytes[i] = 0x90;
    }
    return write_code(site, bytes, size);
}

void emit_u8(uint8_t*& p, uint8_t value) {
    *p++ = value;
}

void emit_bytes(uint8_t*& p, std::initializer_list<uint8_t> bytes) {
    for (uint8_t b : bytes) {
        emit_u8(p, b);
    }
}

void emit_rel32(uint8_t*& p, uint8_t opcode, uintptr_t dest) {
    emit_u8(p, opcode);
    const auto rel = static_cast<int32_t>(dest - (reinterpret_cast<uintptr_t>(p) + 4));
    std::memcpy(p, &rel, 4);
    p += 4;
}

uint8_t* build_index_cave() {
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        return nullptr;
    }
    uint8_t* p = cave;
    // mov dword ptr [g_poll_device], esi
    emit_bytes(p, {0x89, 0x35});
    const auto slot = reinterpret_cast<uintptr_t>(&g_poll_device);
    std::memcpy(p, &slot, 4);
    p += 4;
    emit_bytes(p, {0x8B, 0x86, 0x64, 0x01, 0x00, 0x00});  // mov eax, [esi+164h]
    emit_u8(p, 0x57);                                      // push edi
    emit_bytes(p, {0x85, 0xC0});                           // test eax, eax
    uint8_t* jns = p;
    emit_bytes(p, {0x79, 0x00});  // jns ready
    emit_u8(p, 0x56);             // push esi
    emit_rel32(p, 0xE8, reinterpret_cast<uintptr_t>(&adopt_pad));
    emit_bytes(p, {0x83, 0xF8, 0xFF});  // cmp eax, -1
    uint8_t* je = p;
    emit_bytes(p, {0x74, 0x00});  // je dinput
    emit_bytes(p, {0x89, 0x86, 0x64, 0x01, 0x00, 0x00});  // mov [esi+164h], eax
    const int ready_disp = static_cast<int>(p - (jns + 2));
    if (ready_disp < -128 || ready_disp > 127) {
        return nullptr;
    }
    jns[1] = static_cast<uint8_t>(ready_disp);
    emit_rel32(p, 0xE9, kPollResume);
    const int dinput_disp = static_cast<int>(p - (je + 2));
    if (dinput_disp < -128 || dinput_disp > 127) {
        return nullptr;
    }
    je[1] = static_cast<uint8_t>(dinput_disp);
    emit_rel32(p, 0xE9, kPollDinput);
    return cave;
}

uint8_t* build_result_cave() {
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) {
        return nullptr;
    }
    uint8_t* p = cave;
    emit_bytes(p, {0x85, 0xC0});  // test eax, eax
    uint8_t* jz = p;
    emit_bytes(p, {0x74, 0x00});                    // jz success
    emit_bytes(p, {0x8D, 0x44, 0x24, 0x10});        // lea eax, [esp+10h]
    emit_u8(p, 0x50);                               // push eax
    emit_u8(p, 0x56);                               // push esi
    emit_rel32(p, 0xE8, reinterpret_cast<uintptr_t>(&recover_pad));
    emit_bytes(p, {0x85, 0xC0});  // test eax, eax
    uint8_t* jnz = p;
    emit_bytes(p, {0x75, 0x00});  // jnz unbind
    const int ok_disp = static_cast<int>(p - (jz + 2));
    if (ok_disp < -128 || ok_disp > 127) {
        return nullptr;
    }
    jz[1] = static_cast<uint8_t>(ok_disp);
    emit_rel32(p, 0xE9, kPollSuccess);
    const int unbind_disp = static_cast<int>(p - (jnz + 2));
    if (unbind_disp < -128 || unbind_disp > 127) {
        return nullptr;
    }
    jnz[1] = static_cast<uint8_t>(unbind_disp);
    emit_rel32(p, 0xE9, kPollUnbind);
    return cave;
}

}  // namespace

bool controller_install(const Settings& settings) {
    if (!settings.fix_controller) {
        LOG_INFO("FixController is false. Controller selection is unchanged.");
        return true;
    }
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        LOG_ERROR("Controller hooks need the game at its preferred base. Not patching.");
        return false;
    }
    if (!same_bytes(kMatchCall, kMatchExpect, sizeof(kMatchExpect)) ||
        !same_bytes(kPollIndex, kPollIndexExpect, sizeof(kPollIndexExpect)) ||
        !same_bytes(kPollGetState, kPollGetExpect, sizeof(kPollGetExpect)) ||
        !same_bytes(kPollResult, kPollResultExpect, sizeof(kPollResultExpect))) {
        LOG_ERROR("Controller sites do not match this build. Not patching.");
        return false;
    }
    uint8_t* index_cave = build_index_cave();
    uint8_t* result_cave = build_result_cave();
    if (!index_cave || !result_cave) {
        LOG_ERROR("Could not allocate the controller hooks");
        return false;
    }
    const bool ok = write_branch(kMatchCall, sizeof(kMatchExpect), 0xE8, reinterpret_cast<void*>(&hook_match)) &&
                    write_branch(kPollGetState, sizeof(kPollGetExpect), 0xE8, reinterpret_cast<void*>(&poll_get_state)) &&
                    write_branch(kPollIndex, sizeof(kPollIndexExpect), 0xE9, index_cave) &&
                    write_branch(kPollResult, sizeof(kPollResultExpect), 0xE9, result_cave);
    if (!ok) {
        LOG_ERROR("Could not write the controller hooks");
        return false;
    }
    LOG_INFO("Controller: a matching old XInput id is kept; otherwise the gamepad in use is chosen "
             "(wheels and other non-pads are left alone)");
    return true;
}
