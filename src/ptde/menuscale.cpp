#include "menuscale.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>

#include <intrin.h>

// The game's menus and HUD (EzMenu, FROM's "DynamicUI"): every dialog is an object of its own class
// (NS_FRPG::FrpgMenuDlg*: FEPlayerState the health and stamina bars, FeShortcut the equipment slots, FEEnemyGauge an
// enemy's bar, ...), drawn on the game's side through one device context, NS_FRPG::FrpgMenuDc (vtable 0x11D0404),
// into a command buffer that the render thread plays back (0xF4AE40). The context keeps a current transform
// (FrpgMenuDc+0x30, 4x4 floats, rows: x' = x * m0 + y * m4 + m12; set through its slot 0x44 = 0xC24420, read through
// slot 0x48 = 0x516340, which returns the address it filled): an image is recorded as its rectangle and placed through
// the transform when played back (a "set matrix" command, 0xF4A870, is recorded whenever the transform changes); a
// text is placed through it when recorded (0xC26360 reads it). A clipping rectangle (slot 0x50 = 0xC23F00) is
// recorded as it is.
//
// A dialog draws through EzMenuDialog::Draw (slot 0x48 of every dialog class, 0x787E90: 0x786560, or 0x787720 when
// +0xA0 is set): it reads the context's transform as its parent (its first read), places itself and each of its
// pieces from it (a child dialog the same way, from the transform its parent set), and sets the parent back at the
// end. Its own place: the rectangle in its menu file (+0x88 left, +0x8C top, +0x90 right, +0x94 bottom) mapped onto
// its current one (+0x78..+0x84; the game moves the marks over things in the world by moving that one), drawn while
// +0x98.
//
// UIScale at the source: when a dialog of a class that is scaled (kRules) starts to draw, its parent read returns the
// parent followed by a scale about the dialog's anchor (a corner of the screen for the HUD, the dialog's own middle
// for a mark over something in the world), so everything in it (its images, its texts, its child dialogs) is placed
// smaller by the game's own arithmetic, and the context's transform is set back to the true parent after it. Which
// dialog a piece belongs to is the game's own answer, wherever it lies on the screen and whatever overlaps it.

namespace {

constexpr uintptr_t kDcVtable = 0x011D0404;
constexpr int kSlotSetTransform = 0x44 / 4;
constexpr int kSlotGetTransform = 0x48 / 4;
constexpr int kSlotSetClip = 0x50 / 4;
constexpr uintptr_t kDrawThunk = 0x00787E90;  // EzMenuDialog::Draw, slot 0x48 of every dialog class
constexpr uintptr_t kDraw = 0x00786560, kDrawAlt = 0x00787720;
// EzMenuDlgObj::Draw (slot 0x48 of the pieces): `push edi; mov edi, ecx; test byte [edi+0x18], 2` (7 bytes moved into
// a trampoline), drawn with the context's transform as its parent set it.
constexpr uintptr_t kPieceDraw = 0x00788D80;
constexpr size_t kPieceDrawMoved = 7;
constexpr uintptr_t kTextLo = 0x00401000, kTextHi = 0x010CBE00;
constexpr uintptr_t kRdataLo = 0x010CC000, kRdataHi = 0x012A9600;
constexpr uintptr_t kDataLo = 0x012AA000, kDataHi = 0x014118BC;

// Each returns a value in EAX that some callers use (the text draw takes the matrix's address from GetTransform's), so
// the hooks pass it on.
using SetTransformFn = void*(__thiscall*)(void*, const float*);
using GetTransformFn = float*(__thiscall*)(void*, float*);
using SetClipFn = void*(__thiscall*)(void*, const float*);
using DrawFn = void*(__thiscall*)(void*, void*);
SetTransformFn o_set_transform = nullptr;
GetTransformFn o_get_transform = nullptr;
SetClipFn o_set_clip = nullptr;

std::atomic<int> g_probe{0};
std::atomic<int> g_probe_dialogs{0};
std::atomic<int> g_probe_pieces{0};
DrawFn g_piece_trampoline = nullptr;

bool read32(uintptr_t a, uint32_t* v) {
    __try {
        *v = *reinterpret_cast<const volatile uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool read_bytes(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The RTTI class name of the object at `obj` (".?AVFrpgMenuDlgFEPlayerState@NS_FRPG@@" -> "FrpgMenuDlgFEPlayerState"),
// or empty when it is none.
std::string class_of(uintptr_t obj) {
    uint32_t vt = 0, col = 0, sig = 1, td = 0;
    if (obj < 0x10000 || !read32(obj, &vt) || vt < kRdataLo || vt >= kRdataHi) return {};
    if (!read32(vt - 4, &col) || col < kRdataLo || col >= kRdataHi) return {};
    if (!read32(col, &sig) || sig != 0 || !read32(col + 12, &td) || td < kDataLo || td >= kDataHi) return {};
    char name[64] = {};
    if (!read_bytes(td + 8, name, sizeof(name) - 1) || std::strncmp(name, ".?AV", 4) != 0) return {};
    std::string s(name + 4);
    const size_t at = s.find('@');
    return at == std::string::npos ? s : s.substr(0, at);
}

// ---------------------------------------------------------------------------------------------------------------
// The scaled dialogs.
enum Kind { kNone, kHud, kMark };
struct Rule {
    const char* cls;
    Kind kind;
    float ax, ay;    // kHud: the anchor (1280x720 layout units); kMark: the dialog's own middle (unused)
    bool container;  // its own pieces only: a dialog inside it without a rule of its own keeps the game's size
};
const Rule kRules[] = {
    // The HUD: FrpgMenuDlgPCGauge is the whole-screen HUD dialog; its own pieces are the health and stamina bars (top
    // left), and every other part of the HUD is a dialog inside it, each scaled toward its own corner or edge.
    {"FrpgMenuDlgPCGauge", kHud, 0.0f, 0.0f, true},
    {"FrpgMenuDlgFEPlayerState", kHud, 0.0f, 0.0f},       // humanity, status icons: top left
    {"FrpgMenuDlgFeShortcut", kHud, 0.0f, 720.0f},        // the equipment slots (each one a dialog): bottom left
    {"FrpgMenuDlgFEGetSoul", kHud, 1280.0f, 720.0f},      // the soul count and the souls gained: bottom right
    {"FrpgMenuDlgBossGauge", kHud, 737.3f, 720.0f},       // a boss's health bar: down toward the bottom edge
    {"FrpgMenuDlgResistGauge", kHud, 685.0f, 505.5f},     // the build-up meters (poison, toxic, bleed, curse): about
                                                          // the middle of their stack (535..835 x 447..564)
    {"FrpgMenuDlgFECondition", kHud, 640.0f, 216.0f},     // "! POISONED !" and the like: about its middle
    {"FrpgMenuDlgMapAction", kHud, 640.0f, 720.0f},       // prompts ("A: Talk", "A: Pick up item")
    {"FrpgMenuDlgItemGet", kHud, 640.0f, 720.0f},         // items picked up
    {"FrpgMenuDlgItemGetPlate", kHud, 640.0f, 720.0f},
    {"FrpgMenuDlgBloodMsgDisplay", kHud, 640.0f, 0.0f},   // a message being read: toward the top edge
    // Marks over things in the world (UIScaleWorld), each about its own middle.
    {"FrpgMenuDlgFEEnemyGauge", kMark, 0.0f, 0.0f},
    {"FrpgMenuDlgFloatingPCGauge", kMark, 0.0f, 0.0f},
    {"FrpgMenuDlgTargettingSite", kMark, 0.0f, 0.0f},
};

// A container's own pieces (by their names in the menu file) anchored elsewhere than the container: the HUD dialog's
// own pieces are the health and stamina bars with their icons (top left, the container's anchor), the names of the
// item and the spell in the equipment slots, the slots' arrows, the soul count and the area's name.
struct PieceRule {
    const char* dlg;
    const wchar_t* prefix;
    bool keep;      // the game's size
    float ax, ay;   // else: the anchor
};
const PieceRule kPieceRules[] = {
    {"FrpgMenuDlgPCGauge", L"text_item_name", false, 0.0f, 720.0f},
    {"FrpgMenuDlgPCGauge", L"text_magic_name", false, 0.0f, 720.0f},
    {"FrpgMenuDlgPCGauge", L"F20-00_", false, 0.0f, 720.0f},
    {"FrpgMenuDlgPCGauge", L"soul_param", false, 1280.0f, 720.0f},
    {"FrpgMenuDlgPCGauge", L"map_name", true, 0.0f, 0.0f},
};

struct ClassInfo {
    std::string name;
    const Rule* rule = nullptr;
};
std::mutex g_cls_lock;
std::unordered_map<uint32_t, ClassInfo> g_cls;  // by vtable

const ClassInfo& info_of(uintptr_t dlg) {
    uint32_t vt = 0;
    read32(dlg, &vt);
    std::lock_guard<std::mutex> lock(g_cls_lock);
    auto it = g_cls.find(vt);
    if (it != g_cls.end()) return it->second;
    ClassInfo ci;
    ci.name = class_of(dlg);
    for (const Rule& r : kRules) {
        if (ci.name == r.cls) ci.rule = &r;
    }
    return g_cls.emplace(vt, std::move(ci)).first->second;
}

std::atomic<int> g_scale_pct{100};
std::atomic<int> g_world_pct{100};

// A dialog being drawn smaller (or a dialog inside one drawn at another scale): `s`, `ax`, `ay` its scale about its
// anchor on the screen (its clipping rectangles are mapped so); `a`, `bx`, `by` the change its first read of the
// transform (its parent) gets: x * a + bx, y * a + by (the scale itself, or the scale of the dialog around it undone
// and then its own).
struct Active {
    bool on = false;
    bool pending = false;
    bool container = false;
    float s = 1.0f, ax = 0.0f, ay = 0.0f;
    float a = 1.0f, bx = 0.0f, by = 0.0f;
};
thread_local Active t_active;
thread_local int t_depth = 0;                 // dialogs being drawn (nesting)
thread_local const char* t_parent = nullptr;  // the class of the dialog drawing this one (probe)

// m = m * F, F: x' = x * a + bx, y' = y * a + by in the layout's plane.
void map_after(float* m, float a, float bx, float by) {
    for (int row = 0; row < 4; ++row) {
        float* r = m + row * 4;
        const float w = r[3];
        r[0] = r[0] * a + w * bx;
        r[1] = r[1] * a + w * by;
    }
}

void* __fastcall hk_dialog_draw(void* dlg, void*, void* dc) {
    uint32_t alt = 0;
    read32(reinterpret_cast<uintptr_t>(dlg) + 0xA0, &alt);
    const auto draw = reinterpret_cast<DrawFn>(alt ? kDrawAlt : kDraw);
    const auto base = reinterpret_cast<uintptr_t>(dlg);
    if (g_probe_dialogs.load(std::memory_order_relaxed) > 0 && g_probe_dialogs.fetch_sub(1) > 0) {
        float r[8] = {};
        uint32_t vis = 0;
        read_bytes(base + 0x78, r, sizeof(r));
        read32(base + 0x98, &vis);
        LOG_INFO("menuprobe dialog %p %s vis %u alt %u cur %.1f,%.1f-%.1f,%.1f file %.1f,%.1f-%.1f,%.1f depth %d in %s%s",
                 dlg, info_of(base).name.c_str(), vis & 0xFF, alt, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                 t_depth, t_parent ? t_parent : "-", t_active.on ? " (in a scaled dialog)" : "");
    }
    struct Nest {
        const char* saved;
        Nest(const char* name) : saved(t_parent) {
            ++t_depth;
            t_parent = name;
        }
        ~Nest() {
            --t_depth;
            t_parent = saved;
        }
    } nest(info_of(base).name.c_str());
    const Rule* rule = info_of(base).rule;
    const Active outer = t_active;
    // Inside a scaled dialog: one without a rule of its own goes with it (or keeps the game's size inside a container).
    if (!rule && (!outer.on || !outer.container)) return draw(dlg, dc);
    int pct = 100;
    if (rule) pct = rule->kind == kMark ? g_world_pct.load() : g_scale_pct.load();
    if (pct < 25 || pct > 200) pct = 100;
    if (!outer.on && pct == 100) return draw(dlg, dc);
    Active a;
    a.on = true;
    a.pending = true;
    a.container = rule && rule->container;
    a.s = pct / 100.0f;
    if (rule && rule->kind == kMark) {
        float r[4] = {};
        read_bytes(base + 0x78, r, sizeof(r));
        a.ax = 0.5f * (r[0] + r[2]);
        a.ay = 0.5f * (r[1] + r[3]);
    } else if (rule) {
        a.ax = rule->ax;
        a.ay = rule->ay;
    }
    // Its parent's transform carries the outer dialog's scale: undone first (x -> (x - b) / s), then its own.
    const float os = outer.on ? outer.s : 1.0f;
    const float obx = outer.on ? outer.ax * (1.0f - outer.s) : 0.0f, oby = outer.on ? outer.ay * (1.0f - outer.s) : 0.0f;
    a.a = a.s / os;
    a.bx = -obx * a.s / os + a.ax * (1.0f - a.s);
    a.by = -oby * a.s / os + a.ay * (1.0f - a.s);
    float parent[16];
    std::memcpy(parent, static_cast<const char*>(dc) + 0x30, sizeof(parent));
    t_active = a;
    void* ret = draw(dlg, dc);
    t_active = outer;
    o_set_transform(dc, parent);  // the context as before the dialog (it set back its scaled parent)
    return ret;
}

void* __fastcall hk_set_transform(void* dc, void*, const float* m) {
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe set dc %p m %.3f %.3f %.3f %.3f t %.1f %.1f from %p%s", dc, m[0], m[1], m[4], m[5], m[12],
                 m[13], _ReturnAddress(), t_active.on ? " (scaled dialog)" : "");
    }
    return o_set_transform(dc, m);
}

float* __fastcall hk_get_transform(void* dc, void*, float* out) {
    float* r = o_get_transform(dc, out);
    if (t_active.on && t_active.pending && out) {
        t_active.pending = false;
        map_after(out, t_active.a, t_active.bx, t_active.by);
    }
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe get dc %p from %p", dc, _ReturnAddress());
    }
    return r;
}

void* __fastcall hk_set_clip(void* dc, void*, const float* r) {
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe clip dc %p %.1f,%.1f-%.1f,%.1f from %p%s", dc, r[0], r[1], r[2], r[3], _ReturnAddress(),
                 t_active.on ? " (scaled dialog)" : "");
    }
    if (t_active.on && r) {
        const float s = t_active.s;
        float q[4] = {r[0] * s + t_active.ax * (1.0f - s), r[1] * s + t_active.ay * (1.0f - s),
                      r[2] * s + t_active.ax * (1.0f - s), r[3] * s + t_active.ay * (1.0f - s)};
        return o_set_clip(dc, q);
    }
    return o_set_clip(dc, r);
}

// A piece's name in its menu file ([+0x1C] -> the name's text), or empty.
std::wstring piece_name(uintptr_t piece) {
    uint32_t holder = 0, text = 0;
    if (!read32(piece + 0x1C, &holder) || !holder || !read32(holder, &text) || !text) return {};
    wchar_t buf[48] = {};
    if (!read_bytes(text, buf, sizeof(buf) - sizeof(wchar_t))) return {};
    return std::wstring(buf);
}

// The rule for a piece of a scaled container, or null (it goes with the container).
const PieceRule* piece_rule(uintptr_t piece) {
    if (!t_parent) return nullptr;
    uint32_t holder = 0, text = 0;
    if (!read32(piece + 0x1C, &holder) || !holder || !read32(holder, &text) || !text) return nullptr;
    wchar_t name[32] = {};
    if (!read_bytes(text, name, sizeof(name) - sizeof(wchar_t))) return nullptr;
    for (const PieceRule& r : kPieceRules) {
        if (std::strcmp(t_parent, r.dlg) == 0 && std::wcsncmp(name, r.prefix, std::wcslen(r.prefix)) == 0) return &r;
    }
    return nullptr;
}

void* __fastcall hk_piece_draw(void* piece, void*, void* dc) {
    if (t_active.on && g_probe_pieces.load(std::memory_order_relaxed) > 0 && g_probe_pieces.fetch_sub(1) > 0) {
        const float* m = reinterpret_cast<const float*>(static_cast<const char*>(dc) + 0x30);
        LOG_INFO("menuprobe piece %ls in %s (scope s %.2f%s) at %.1f,%.1f scale %.3f", piece_name(
                     reinterpret_cast<uintptr_t>(piece)).c_str(), t_parent ? t_parent : "-", t_active.s,
                 t_active.container ? ", container" : "", m[12], m[13], m[0]);
    }
    const PieceRule* rule = t_active.on && t_active.container ? piece_rule(reinterpret_cast<uintptr_t>(piece)) : nullptr;
    if (!rule) return g_piece_trampoline(piece, dc);
    // Its place as its container set it carries the container's scale: undone, then the piece's own.
    const Active outer = t_active;
    Active a = outer;
    a.container = false;
    a.s = rule->keep ? 1.0f : outer.s;
    a.ax = rule->ax;
    a.ay = rule->ay;
    const float k = a.s / outer.s;
    const float bx = -outer.ax * (1.0f - outer.s) * k + a.ax * (1.0f - a.s);
    const float by = -outer.ay * (1.0f - outer.s) * k + a.ay * (1.0f - a.s);
    float placed[16], m[16];
    std::memcpy(placed, static_cast<const char*>(dc) + 0x30, sizeof(placed));
    std::memcpy(m, placed, sizeof(m));
    map_after(m, k, bx, by);
    o_set_transform(dc, m);
    a.pending = false;
    t_active = a;
    void* ret = g_piece_trampoline(piece, dc);
    t_active = outer;
    o_set_transform(dc, placed);
    return ret;
}

bool make_piece_trampoline() {
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    std::memcpy(t, reinterpret_cast<const void*>(kPieceDraw), kPieceDrawMoved);
    t[kPieceDrawMoved] = 0xE9;
    const int32_t rel = static_cast<int32_t>((kPieceDraw + kPieceDrawMoved) -
                                             (reinterpret_cast<uintptr_t>(t) + kPieceDrawMoved + 5));
    std::memcpy(t + kPieceDrawMoved + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, 32);
    g_piece_trampoline = reinterpret_cast<DrawFn>(t);
    return true;
}

void* patch_vtable(uintptr_t vt, int slot, void* hook) {
    auto* p = reinterpret_cast<void**>(vt) + slot;
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* prev = *p;
    *p = hook;
    DWORD ignored = 0;
    VirtualProtect(p, sizeof(void*), old, &ignored);
    return prev;
}

bool write_jmp(uintptr_t site, const void* target) {
    uint8_t code[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (site + 5));
    std::memcpy(code + 1, &rel, 4);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(site), code, 5);
    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(site), 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    return true;
}

bool g_installed = false;

}  // namespace

bool menuscale_install() {
    if (g_installed) return true;
    // This build's code: the context's slots hold the functions found by RE (0xC24420, 0x516340, 0xC23F00), and the
    // dialogs' draw is `cmp dword [ecx+0xA0], 0; jne +5; jmp 0x786560; jmp 0x787720`.
    static const uint8_t kThunk[] = {0x83, 0xB9, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x75, 0x05, 0xE9};
    static const uint8_t kPiece[] = {0x57, 0x8B, 0xF9, 0xF6, 0x47, 0x18, 0x02};
    uint32_t a = 0, b = 0, c = 0;
    uint8_t thunk[sizeof(kThunk)] = {};
    if (!read32(kDcVtable + 4 * kSlotSetTransform, &a) || a != 0x00C24420 ||
        !read32(kDcVtable + 4 * kSlotGetTransform, &b) || b != 0x00516340 ||
        !read32(kDcVtable + 4 * kSlotSetClip, &c) || c != 0x00C23F00 ||
        !read_bytes(kDrawThunk, thunk, sizeof(thunk)) || std::memcmp(thunk, kThunk, sizeof(kThunk)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kPieceDraw), kPiece, sizeof(kPiece)) != 0) {
        LOG_ERROR("MenuScale: the menus' code is not what it is in this build (%08X %08X %08X); UIScale stays off", a,
                  b, c);
        return false;
    }
    o_set_transform = reinterpret_cast<SetTransformFn>(
        patch_vtable(kDcVtable, kSlotSetTransform, reinterpret_cast<void*>(&hk_set_transform)));
    o_get_transform = reinterpret_cast<GetTransformFn>(
        patch_vtable(kDcVtable, kSlotGetTransform, reinterpret_cast<void*>(&hk_get_transform)));
    o_set_clip =
        reinterpret_cast<SetClipFn>(patch_vtable(kDcVtable, kSlotSetClip, reinterpret_cast<void*>(&hk_set_clip)));
    g_installed = o_set_transform && o_get_transform && o_set_clip &&
                  write_jmp(kDrawThunk, reinterpret_cast<const void*>(&hk_dialog_draw)) && make_piece_trampoline() &&
                  write_jmp(kPieceDraw, reinterpret_cast<const void*>(&hk_piece_draw));
    LOG_INFO("MenuScale: menu hooks %s", g_installed ? "installed" : "FAILED");
    return g_installed;
}

bool menuscale_installed() { return g_installed; }

void menuscale_set(int ui_scale, int ui_scale_world) {
    if (ui_scale >= 0) g_scale_pct.store(ui_scale);
    if (ui_scale_world >= 0) g_world_pct.store(ui_scale_world);
    ui_scale = g_scale_pct.load();
    ui_scale_world = g_world_pct.load();
    LOG_INFO("UIScale: the HUD's dialogs are drawn at %d%% (the marks over things in the world at %d%%) by the game's "
             "own placement: each dialog toward its corner or edge, whatever overlaps it; the menus keep their size",
             ui_scale, ui_scale_world);
}

void menuscale_probe(int n) { g_probe.store(n); }
void menuscale_probe_dialogs(int n) {
    g_probe_dialogs.store(n);
    g_probe_pieces.store(n);
}
