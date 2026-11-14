// test_worldpoint.exe: the call-site thunk of src/ptde/layout.cpp (world_point_thunk) against a stand-in for the
// game's 0x4F4030 with the same convention: the camera in EAX, the world position in EDI, the result's address on the
// stack (removed by the callee), the address returned in EAX. Checks the result, that the fix ran once on it, that
// the stack is balanced and that EBX, ESI, EDI, EBP, ECX and EDX come back as the stand-in left them.
#include <cstdio>
#include <cstring>

volatile float g_seen_cam = 0, g_seen_pos = 0;  // written by the stand-in (assembly)
volatile int g_fix_calls = 0;

// The stand-in: out[0] = *EAX + *EDI, out[1] = *EAX - *EDI; leaves ECX = 0x11111111, EDX = 0x22222222.
__declspec(naked) void game_world_to_layout() {
    __asm {
        push ebp
        mov ebp, esp
        mov ecx, [ebp + 8]
        movss xmm0, dword ptr [eax]
        movss dword ptr [g_seen_cam], xmm0
        movss xmm1, dword ptr [edi]
        movss dword ptr [g_seen_pos], xmm1
        movss xmm2, xmm0
        addss xmm2, xmm1
        movss dword ptr [ecx], xmm2
        subss xmm0, xmm1
        movss dword ptr [ecx + 4], xmm0
        mov eax, ecx
        mov ecx, 0x11111111
        mov edx, 0x22222222
        mov esp, ebp
        pop ebp
        ret 4
    }
}

void __cdecl world_point_fix(float* out) {
    ++g_fix_calls;
    out[0] = 640.0f + (out[0] - 640.0f) * 1.5f;
}

void* g_world_to_layout = reinterpret_cast<void*>(&game_world_to_layout);

// The same instructions as layout.cpp's.
__declspec(naked) void world_point_thunk() {
    __asm {
        push dword ptr [esp + 4]
        call dword ptr [g_world_to_layout]
        push ecx
        push edx
        push eax
        call world_point_fix
        pop eax
        pop edx
        pop ecx
        ret 4
    }
}

int main() {
    float cam = 700.0f, pos = 60.0f, out[4] = {};
    unsigned esp_before = 0, esp_after = 0, r_ebx = 0, r_esi = 0, r_edi = 0, r_ecx = 0, r_edx = 0;
    float* ret = nullptr;
    float* pcam = &cam;
    float* ppos = &pos;
    float* pout = out;
    __asm {
        push ebx
        push esi
        push edi
        mov ebx, 0xB0B0B0B0
        mov esi, 0x51515151
        mov esp_before, esp
        mov eax, pout
        push eax
        mov edi, ppos
        mov eax, pcam
        call world_point_thunk
        mov esp_after, esp
        mov ret, eax
        mov r_ebx, ebx
        mov r_esi, esi
        mov r_edi, edi
        mov r_ecx, ecx
        mov r_edx, edx
        pop edi
        pop esi
        pop ebx
    }
    int bad = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++bad;
    };
    check(g_seen_cam == 700.0f && g_seen_pos == 60.0f, "the game routine got EAX and EDI");
    check(out[0] == 640.0f + (760.0f - 640.0f) * 1.5f && out[1] == 640.0f, "result written, then fixed once");
    check(g_fix_calls == 1, "fix called once");
    check(ret == out, "EAX returns the result's address");
    check(esp_before == esp_after, "stack balanced (the argument removed once)");
    check(r_ebx == 0xB0B0B0B0 && r_esi == 0x51515151 && r_edi == reinterpret_cast<unsigned>(ppos), "EBX, ESI, EDI kept");
    check(r_ecx == 0x11111111 && r_edx == 0x22222222, "ECX, EDX as the game routine left them");
    std::printf(bad ? "FAILED\n" : "all passed\n");
    return bad;
}
