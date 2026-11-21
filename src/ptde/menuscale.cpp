#include "menuscale.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
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
//
// Text (UIScaleText): the transform places a text's box, not the size of its letters. A text is written through
// NS_FRPG::FrpgMenuFontPrinter (vtable 0x11D05BC): slot 0x04 (0xFA6700) begins a run in one of the game's 13 font
// sizes (a "begin text" command, 0xF4B150, which sets the text renderer's letter scale from the table at 0x12E3BA8:
// 10/24 to 36/24 of the font's 24 units, and the line height = that scale * 24), slot 0x34 (0xFA6630) sets the line
// height (command 0xF4A630), slot 0x0C (0xFA6AC0) writes a text into a box (placed through the transform). The
// renderer's state ([[buffer+0x20]+0x38] when played back): +0x5C, +0x60 the letter scale x, y; +0x6C the font's
// units (24); +0xB8 the line height; written under its lock (+0xC4, slots 0x08 lock(-1) and 0x10 unlock). In a scaled
// dialog a command of ours follows each begin (and each line height) and multiplies those by the text's scale, so the
// letters, the button icons inside a text and the lines' spacing shrink with the box they are written in.

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
constexpr uintptr_t kPrinterVtable = 0x011D05BC;  // NS_FRPG::FrpgMenuFontPrinter
constexpr int kSlotPrinterBegin = 0x04 / 4;       // 0xFA6700: begin a run of text in a font size (0..12)
constexpr int kSlotPrinterLine = 0x34 / 4;        // 0xFA6630: the line height (a short)
constexpr uintptr_t kTextLo = 0x00401000, kTextHi = 0x010CBE00;
constexpr uintptr_t kRdataLo = 0x010CC000, kRdataHi = 0x012A9600;
constexpr uintptr_t kDataLo = 0x012AA000, kDataHi = 0x014118BC;

// Each returns a value in EAX that some callers use (the text draw takes the matrix's address from GetTransform's), so
// the hooks pass it on.
using SetTransformFn = void*(__thiscall*)(void*, const float*);
using GetTransformFn = float*(__thiscall*)(void*, float*);
using SetClipFn = void*(__thiscall*)(void*, const float*);
using DrawFn = void*(__thiscall*)(void*, void*);
using PrinterFn = uint32_t(__thiscall*)(void*, uint32_t);
SetTransformFn o_set_transform = nullptr;
GetTransformFn o_get_transform = nullptr;
SetClipFn o_set_clip = nullptr;
PrinterFn o_printer_begin = nullptr;
PrinterFn o_printer_line = nullptr;

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

// A dialog's name in its menu file (the definition it was made from, +0x2C, starts with the name's address: "R04"),
// or empty.
std::wstring dialog_name(uintptr_t dlg) {
    uint32_t def = 0, text = 0;
    if (!read32(dlg + 0x2C, &def) || def < 0x10000 || !read32(def, &text) || text < 0x10000) return {};
    wchar_t buf[24] = {};
    if (!read_bytes(text, buf, sizeof(buf) - sizeof(wchar_t))) return {};
    for (wchar_t& c : buf) {
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return {};
    }
    return std::wstring(buf);
}

// ---------------------------------------------------------------------------------------------------------------
// The scaled dialogs.
// kMenu: the in-game menu's screens (UIScaleMenus), about the middle of the screen; found by their names in the menu
// file (menu_member), not by class (each is a plain EzMenuDialog holding the screen's dialogs). kMenuText: a menu's
// letters only (what a character says: its backdrop keeps the screen's width).
enum Kind { kNone, kHud, kMark, kMenu, kMenuText };
// FixBossBarOverlap: the prompt (and the item windows above it) moved up while a boss's health bar is shown.
enum Lift { kLiftNone, kLiftPrompt, kLiftItems, kBossBar };
struct Rule {
    const char* cls;
    Kind kind;
    float ax, ay;    // kHud: the anchor (1280x720 layout units); kMark: the dialog's own middle (unused)
    bool container;  // its own pieces only: a dialog inside it without a rule of its own keeps the game's size
    Lift lift;
};
const Rule kRules[] = {
    // The HUD: FrpgMenuDlgPCGauge is the whole-screen HUD dialog; its own pieces are the health and stamina bars (top
    // left), and every other part of the HUD is a dialog inside it, each scaled toward its own corner or edge.
    {"FrpgMenuDlgPCGauge", kHud, 0.0f, 0.0f, true},
    {"FrpgMenuDlgFEPlayerState", kHud, 0.0f, 0.0f},       // humanity, status icons: top left
    {"FrpgMenuDlgFeShortcut", kHud, 0.0f, 720.0f},        // the equipment slots (each one a dialog): bottom left
    {"FrpgMenuDlgFEGetSoul", kHud, 1280.0f, 720.0f},      // the soul count and the souls gained: bottom right
    {"FrpgMenuDlgBossGauge", kHud, 737.3f, 720.0f, false, kBossBar},  // a boss's health bar: toward the bottom edge
    {"FrpgMenuDlgResistGauge", kHud, 685.0f, 505.5f},     // the build-up meters (poison, toxic, bleed, curse): about
                                                          // the middle of their stack (535..835 x 447..564)
    {"FrpgMenuDlgFECondition", kHud, 640.0f, 216.0f},     // "! POISONED !" and the like: about its middle
    {"FrpgMenuDlgMapAction", kHud, 640.0f, 720.0f, false, kLiftPrompt},  // prompts ("A: Talk", "A: Pick up item")
    {"FrpgMenuDlgItemGet", kHud, 640.0f, 720.0f, false, kLiftItems},     // items picked up
    {"FrpgMenuDlgItemGetPlate", kHud, 640.0f, 720.0f, false, kLiftItems},
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
std::atomic<int> g_text_pct{0};  // UIScaleText: 0 = each text at its dialog's scale, else percent of the game's size
std::atomic<int> g_menu_pct{0};  // UIScaleMenus: 0 = as UIScale, else percent
std::atomic<bool> g_fix_boss{false};

// The menus' scale in percent now (UIScaleMenus, or UIScale for auto), 100 when off or out of range.
int menu_pct() {
    int pct = g_menu_pct.load(std::memory_order_relaxed);
    if (pct == 0) pct = g_scale_pct.load(std::memory_order_relaxed);
    return pct < 25 || pct > 200 ? 100 : pct;
}

// A dialog being drawn smaller (or a dialog inside one drawn at another scale): `s`, `ax`, `ay` its scale about its
// anchor on the screen; `ox`, `oy` where that puts it: x' = x * s + ox (the anchor's ax * (1 - s), plus a move of its
// own; its clipping rectangles are mapped so); `a`, `bx`, `by` the change its first read of the transform (its parent)
// gets: x * a + bx, y * a + by (the scale itself, or the place of the dialog around it undone and then its own); `ts`
// the size of the letters of a text written in it; `screen`: a menu (what the game parks off its screen in it stays
// out of view).
struct Active {
    bool on = false;
    bool pending = false;
    bool container = false;
    bool screen = false;
    float s = 1.0f, ax = 0.0f, ay = 0.0f;
    float ox = 0.0f, oy = 0.0f;
    float a = 1.0f, bx = 0.0f, by = 0.0f;
    float ts = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------------
// The frame's 2D, on the game thread. Each top-level dialog is drawn once a frame, in the same order (2026-11-17,
// in the field with the inventory open: the button guide 'R04', the fade 'black', the save icon 'R08', 'F24', the
// marks over characters 'F21', the HUD 'F20-00', 'F20', 'Icon', 'F20-01', 'F23', 'F25', 'Icon', the back plate 'R11',
// the start menu 'R00', the menu's screen ('I01' the items), 'Icon', 'F03', the field messages 'F05' (prompts, item
// windows), 'F22', the movie 'R06'), so one drawn a second time starts the next frame.
constexpr float kNoBoss = 1.0e9f;
// FixBossBarOverlap, measured at 100% (2048x1152, 2026-11-17): a boss's name starts 598 units down its dialog's own
// 720 (the dialog is placed at 59,-29 .. 1335,679 for the first boss, 47 higher for a second), the prompt's box ends
// 5 above its dialog's bottom (563 .. 638 with one line, .. 663 with two), and the prompt's box covered the name and
// the bar. Kept this far above the name.
constexpr float kBossNameTop = 598.0f;
constexpr float kPromptBoxBottom = 5.0f;
constexpr float kBossGap = 6.0f;
struct FrameState {
    uintptr_t seen[32] = {};
    int n = 0;
    bool world = false, world_prev = false;              // the HUD 'F20' was drawn (in the world, not the title)
    bool back = false;                                   // the back plate 'R11' was drawn: the menus' layers follow
    bool menu = false, menu_prev = false;                // a menu is up (this frame, the last one)
    bool box = false, box_prev = false;                  // one of the game's boxes is up ('R03-..', OK / CANCEL)
    int view = 0, view_prev = 0;                         // which kind of menu (View; this frame, the last one)
    float menu_s = 1.0f;                                 // its scale
    float boss_top = kNoBoss, boss_top_prev = kNoBoss;   // the top of the highest boss bar shown
    float lift = 0.0f, lift_prev = 0.0f;                 // how far the prompt was moved up
    bool prompt = false, prompt_prev = false;            // a prompt is shown
};
FrameState g_frame;
float g_mouse_s = 1.0f;  // the menus' scale while they are up (the mouse's place in them, cursor_after)
float g_mouse_ax = 640.0f, g_mouse_ay = 360.0f;  // and the point they are scaled about

// The menus' anchors (1280x720 layout), each kind of menu toward its own: a screen (the inventory, Level Up, a shop)
// about the middle of the screen; the start menu on its own toward its box's top right corner ('R00_bg' at 828,52 -
// 1177,178); the list of choices at a bonfire or with a character toward its box's top left corner ('F04_bg' at
// 67,64 - 497,664); the message box 'F22' (99,478 - 1185,548, its OK below) toward the bottom edge; what a character
// says keeps its place and width, its letters scaled (its backdrop darkens the whole bottom of the screen). The button
// guide 'R04' and the icon layers go with the kind of menu shown (this frame's when known, else the last one's).
enum View { kViewNone, kViewStart, kViewTalk, kViewMessage, kViewScreen };
const Rule kMenuRule = {"", kMenu, 640.0f, 360.0f};
const Rule kStartRule = {"", kMenu, 1177.0f, 52.0f};
const Rule kTalkRule = {"", kMenu, 67.0f, 64.0f};
const Rule kMessageRule = {"", kMenu, 640.0f, 720.0f};
const Rule kSubtitleRule = {"", kMenuText, 640.0f, 720.0f};
const Rule kPlateRule = {"", kHud, 640.0f, 720.0f, false, kLiftPrompt};  // 'R03-2x': where the prompts go, as they do
const Rule* view_rule(int v) {
    return v == kViewStart ? &kStartRule : v == kViewTalk ? &kTalkRule : v == kViewMessage ? &kMessageRule : &kMenuRule;
}

void top_level_drawn(uintptr_t dlg) {
    FrameState& f = g_frame;
    for (int i = 0; i < f.n; ++i) {
        if (f.seen[i] != dlg) continue;
        if (g_probe_dialogs.load(std::memory_order_relaxed) > 0) {
            LOG_INFO("menuprobe frame: %08X again (seen %d of %d)", static_cast<unsigned>(dlg), i, f.n);
        }
        f.n = 0;
        f.world_prev = f.world;
        f.world = false;
        f.back = false;
        f.menu_prev = f.menu;
        f.menu = false;
        f.box_prev = f.box;
        f.box = false;
        f.view_prev = f.view;
        f.view = kViewNone;
        f.menu_s = menu_pct() / 100.0f;
        f.boss_top_prev = f.boss_top;
        f.boss_top = kNoBoss;
        f.lift_prev = f.lift;
        f.lift = 0.0f;
        f.prompt_prev = f.prompt;
        f.prompt = false;
        g_mouse_s = f.menu_prev ? f.menu_s : 1.0f;
        // A box takes the mouse (its buttons) wherever the menu under it is anchored.
        g_mouse_ax = f.box_prev ? kMenuRule.ax : view_rule(f.view_prev)->ax;
        g_mouse_ay = f.box_prev ? kMenuRule.ay : view_rule(f.view_prev)->ay;
        break;
    }
    if (f.n < 32) f.seen[f.n++] = dlg;
}

// A part of the in-game menus. In the world (the HUD 'F20' drawn) everything the game draws after the back plate 'R11'
// is a menu's layer, apart from the field's own ('F...': prompts, item windows) and the movie 'R06': the start menu
// 'R00' with the screen it opened (the items 'I01', the equipment 'I02', the status 'R02', the system 'I06', the
// options 'R01', the brightness 'T07', their submenus), the bonfire's and the shops' screens (Level Up 'C01', Attune
// Magic 'C05', the Bottomless Box 'C07', Repair 'S01', ...), the icon layers drawn with them (before or after);
// plus the talk group 'F04' (the bonfire's and the characters' lists of choices 'F04-01', what a character says
// 'F04-00') and the message box with its OK ('F22', FrpgMenuDlgRecall: "X has died"), the field layers that are
// menus; and the button guide and help line 'R04' (drawn first in the frame, so with a menu up in the last one). Seen
// 2026-11-17: in the field the frame ends 'R11' 'F03' 'F05' 'F22' 'R06'; the bonfire's screens come before 'F03'
// (Attune Magic, the box) or after it (Level Up, Repair).
bool menu_member(const std::wstring& n, const FrameState& f) {
    if (n == L"R04") return f.menu || f.menu_prev;
    if (!f.world || !f.back || n.empty()) return false;
    if (n.compare(0, 3, L"F04") == 0 || n == L"F22") return true;
    if (n[0] == L'F') return false;
    return n != L"R06" && n != L"R08" && n != L"R11" && n != L"black";
}

// The size of the letters in a dialog drawn at `s` (UIScaleText): its own scale, or the setting's.
float text_scale_for(float s) {
    const int pct = g_text_pct.load(std::memory_order_relaxed);
    return pct >= 25 && pct <= 200 ? pct / 100.0f : s;
}
// Per thread: the scale of the dialog being drawn, how deep the dialogs nest and the class of the one drawing. Kept in
// a TlsAlloc slot rather than in thread_local variables, so the DLL has no static TLS section (as sfx.cpp).
struct ThreadState {
    Active active;
    int depth = 0;                 // dialogs being drawn (nesting)
    const char* parent = nullptr;  // the class of the dialog drawing this one (probe)
};
DWORD g_tls_slot = TLS_OUT_OF_INDEXES;  // made by menuscale_install before any hook is placed

// This thread's state, made on its first draw; the game's last error is kept (TlsGetValue clears it). Null when it
// cannot be made or kept in the slot (or the slot is gone): the hook then leaves that call unscaled. One state shared
// for such threads would not do, since nested dialog draws re-enter here and would find it reset mid-draw.
ThreadState* thread_state() {
    if (g_tls_slot == TLS_OUT_OF_INDEXES) return nullptr;
    const DWORD error = GetLastError();
    auto* t = static_cast<ThreadState*>(TlsGetValue(g_tls_slot));
    if (!t) {
        t = new (std::nothrow) ThreadState();
        if (t && !TlsSetValue(g_tls_slot, t)) {
            delete t;
            t = nullptr;
        }
    }
    SetLastError(error);
    return t;
}

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
    ThreadState* const state = thread_state();
    if (!state) return draw(dlg, dc);
    ThreadState& th = *state;
    const auto base = reinterpret_cast<uintptr_t>(dlg);
    if (g_probe_dialogs.load(std::memory_order_relaxed) > 0 && g_probe_dialogs.fetch_sub(1) > 0) {
        float r[8] = {};
        uint32_t vis = 0;
        read_bytes(base + 0x78, r, sizeof(r));
        read32(base + 0x98, &vis);
        LOG_INFO("menuprobe dialog %p %s '%ls' vis %u alt %u cur %.1f,%.1f-%.1f,%.1f file %.1f,%.1f-%.1f,%.1f depth %d "
                 "in %s%s", dlg, info_of(base).name.c_str(), dialog_name(base).c_str(), vis & 0xFF, alt, r[0], r[1],
                 r[2], r[3], r[4], r[5], r[6], r[7], th.depth, th.parent ? th.parent : "-",
                 th.active.on ? " (in a scaled dialog)" : "");
    }
    struct Nest {
        ThreadState& th;
        const char* saved;
        Nest(ThreadState& state, const char* name) : th(state), saved(state.parent) {
            ++th.depth;
            th.parent = name;
        }
        ~Nest() {
            --th.depth;
            th.parent = saved;
        }
    } nest(th, info_of(base).name.c_str());
    const Rule* rule = info_of(base).rule;
    const Active outer = th.active;
    // Inside a scaled menu: a dialog the game parks wholly off its screen (Repair's second cursor at -63,-123) is not
    // drawn, since the menu scaled toward the middle would bring it into view. Its rectangle is in its parent's units,
    // which the context's transform (its parent's, the menu's scale on it) places; the menu's scale is undone for the
    // game's own place.
    if (outer.on && outer.screen && th.depth > 1) {
        float r[4] = {};
        float m[16];
        if (read_bytes(base + 0x78, r, sizeof(r))) {
            std::memcpy(m, static_cast<const char*>(dc) + 0x30, sizeof(m));
            const float s = outer.s > 0.0f ? outer.s : 1.0f;
            const auto gx = [&](float x, float y) { return (x * m[0] + y * m[4] + m[12] - outer.ox) / s; };
            const auto gy = [&](float x, float y) { return (x * m[1] + y * m[5] + m[13] - outer.oy) / s; };
            const float x0 = gx(r[0], r[1]), x1 = gx(r[2], r[3]), y0 = gy(r[0], r[1]), y1 = gy(r[2], r[3]);
            const float left = x0 < x1 ? x0 : x1, right = x0 < x1 ? x1 : x0;
            const float top = y0 < y1 ? y0 : y1, bottom = y0 < y1 ? y1 : y0;
            if (right <= 0.0f || left >= 1280.0f || bottom <= 0.0f || top >= 720.0f) return nullptr;
        }
    }
    FrameState& fs = g_frame;
    // A top-level dialog: the frame's bookkeeping, and the in-game menu's screens by their names.
    if (th.depth == 1) {
        top_level_drawn(base);
        if (!rule) {
            const std::wstring name = dialog_name(base);
            if (menu_member(name, fs)) {
                uint32_t vis = 0;
                read32(base + 0x98, &vis);
                const bool shown = (vis & 0xFF) != 0;
                if (name == L"F04-00") {
                    rule = &kSubtitleRule;
                } else if (name.compare(0, 3, L"F04") == 0) {
                    rule = &kTalkRule;
                    if (shown && fs.view < kViewTalk) fs.view = kViewTalk;
                } else if (name == L"R00") {
                    // On its own toward its corner; with a screen open (drawn after it: the last frame's) as the screen.
                    rule = fs.view_prev == kViewScreen ? &kMenuRule : &kStartRule;
                    if (shown && fs.view < kViewStart) fs.view = kViewStart;
                } else if (name == L"F22") {
                    rule = &kMessageRule;
                    if (shown && fs.view < kViewMessage) fs.view = kViewMessage;
                } else if (name == L"R04" || name == L"Icon") {
                    rule = view_rule(fs.view != kViewNone ? fs.view : fs.view_prev);
                } else {
                    rule = &kMenuRule;
                    if (shown) fs.view = kViewScreen;
                }
                // A menu is up while one of its dialogs is shown (the button guide and the icon layers only go with
                // one; a character's line is not one to point at): the button guide next frame and the mouse follow.
                if (shown && name != L"R04" && name != L"Icon" && name != L"F04-00") fs.menu = true;
            }
            if (name == L"F20") fs.world = true;
            if (name == L"R11") fs.back = true;
            if (g_probe_dialogs.load(std::memory_order_relaxed) > 0) {
                LOG_INFO("menuprobe top '%ls' menu %d (last frame %d) -> %s", name.c_str(), fs.menu ? 1 : 0,
                         fs.menu_prev ? 1 : 0, rule ? "menu scale" : "-");
            }
        }
    }
    // The game's general dialogs (2026-11-21, user report with screenshots: both stayed at the game's size). Their owner
    // 'R03' (FrpgMenuDlgGeneralOwner, shown all the time, drawn first in the frame before the HUD) holds the boxes
    // 'R03-00' .. 'R03-12' ("Are you sure you want to quit the game?" with OK and CANCEL is 'R03-02', 341,244 -
    // 941,477) and the plates 'R03-20' .. 'R03-22' ("Used Dungeon Cell Key": one or two lines where the prompts go,
    // 331,563 - 952,638 or 663). In the world (the last frame's HUD; not at the title) a box is a menu about the middle
    // (UIScaleMenus; the button guide and the mouse go with it), a plate goes as a prompt does (UIScale toward the
    // bottom edge, moved up with the prompts above a boss's bar).
    if (!rule && th.depth == 2 && fs.world_prev && info_of(base).name == "FrpgMenuDlgGeneral") {
        const std::wstring name = dialog_name(base);
        if (name.size() >= 6 && name.compare(0, 4, L"R03-") == 0) {
            if (name[4] == L'2') {
                rule = &kPlateRule;
            } else {
                rule = &kMenuRule;
                uint32_t vis = 0;
                read32(base + 0x98, &vis);
                if (vis & 0xFF) {
                    fs.menu = true;
                    fs.box = true;
                }
            }
            if (g_probe_dialogs.load(std::memory_order_relaxed) > 0) {
                LOG_INFO("menuprobe general '%ls' -> %s", name.c_str(), rule == &kPlateRule ? "as a prompt" : "menu scale");
            }
        }
    }
    // FixBossBarOverlap: where the boss bars shown this frame start, and whether a prompt is up.
    float lift = 0.0f;
    if (rule && rule->lift != kLiftNone) {
        uint32_t vis = 0;
        float r[4] = {};
        read32(base + 0x98, &vis);
        read_bytes(base + 0x78, r, sizeof(r));
        if (rule->lift == kBossBar && (vis & 0xFF)) {
            const float top = r[1] + kBossNameTop * (r[3] - r[1]) / 720.0f;
            if (top < fs.boss_top) fs.boss_top = top;
        } else if (rule->lift == kLiftPrompt && g_fix_boss.load(std::memory_order_relaxed)) {
            // The prompt's dialog (563 .. 638 in the layout): its box kept above the highest boss bar's name.
            if (vis & 0xFF) fs.prompt = true;
            const float boss = fs.boss_top < fs.boss_top_prev ? fs.boss_top : fs.boss_top_prev;
            if (boss < kNoBoss) {
                const float over = r[3] - kPromptBoxBottom - (boss - kBossGap);
                if (over > 0.0f) lift = over;
            }
            if ((vis & 0xFF) && lift > fs.lift) fs.lift = lift;
        } else if (rule->lift == kLiftItems && g_fix_boss.load(std::memory_order_relaxed)) {
            // The item windows (stacked above the prompt) go up with it.
            if (fs.prompt || fs.prompt_prev) lift = fs.lift > fs.lift_prev ? fs.lift : fs.lift_prev;
        }
    }
    // Inside a scaled dialog: one without a rule of its own goes with it (or keeps the game's size inside a container).
    if (!rule && (!outer.on || !outer.container)) return draw(dlg, dc);
    int pct = 100;
    if (rule) {
        pct = rule->kind == kMark       ? g_world_pct.load()
              : rule->kind == kMenu     ? menu_pct()
              : rule->kind == kMenuText ? 100
                                        : g_scale_pct.load();
    }
    if (pct < 25 || pct > 200) pct = 100;
    // A dialog inside a container without a rule of its own keeps the game's size, its letters too; a menu's letters
    // go with the menu.
    const float ts = !rule                      ? 1.0f
                     : rule->kind == kMenu      ? pct / 100.0f
                     : rule->kind == kMenuText ? menu_pct() / 100.0f
                                                : text_scale_for(pct / 100.0f);
    if (!outer.on && pct == 100 && ts == 1.0f && lift == 0.0f) return draw(dlg, dc);
    Active a;
    a.on = true;
    a.pending = true;
    a.container = rule && rule->container;
    a.s = pct / 100.0f;
    a.ts = ts;
    if (rule && rule->kind == kMark) {
        float r[4] = {};
        read_bytes(base + 0x78, r, sizeof(r));
        a.ax = 0.5f * (r[0] + r[2]);
        a.ay = 0.5f * (r[1] + r[3]);
    } else if (rule) {
        a.ax = rule->ax;
        a.ay = rule->ay;
    }
    // Its place: scaled about its anchor, after the move up (in its own units, so the move scales with it).
    a.ox = a.ax * (1.0f - a.s);
    a.oy = a.ay * (1.0f - a.s) - lift * a.s;
    // Its parent's transform carries the outer dialog's place: undone first (x -> (x - o) / s), then its own.
    const float os = outer.on ? outer.s : 1.0f;
    const float oox = outer.on ? outer.ox : 0.0f, ooy = outer.on ? outer.oy : 0.0f;
    a.a = a.s / os;
    a.bx = -oox * a.s / os + a.ox;
    a.by = -ooy * a.s / os + a.oy;
    a.screen = (rule && rule->kind == kMenu) || (outer.on && outer.screen);
    float parent[16];
    std::memcpy(parent, static_cast<const char*>(dc) + 0x30, sizeof(parent));
    th.active = a;
    void* ret = draw(dlg, dc);
    th.active = outer;
    o_set_transform(dc, parent);  // the context as before the dialog (it set back its scaled parent)
    return ret;
}

void* __fastcall hk_set_transform(void* dc, void*, const float* m) {
    ThreadState* const state = thread_state();
    if (!state) return o_set_transform(dc, m);
    ThreadState& th = *state;
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe set dc %p m %.3f %.3f %.3f %.3f t %.1f %.1f from %p%s", dc, m[0], m[1], m[4], m[5], m[12],
                 m[13], _ReturnAddress(), th.active.on ? " (scaled dialog)" : "");
    }
    return o_set_transform(dc, m);
}

float* __fastcall hk_get_transform(void* dc, void*, float* out) {
    ThreadState* const state = thread_state();
    if (!state) return o_get_transform(dc, out);
    ThreadState& th = *state;
    float* r = o_get_transform(dc, out);
    if (th.active.on && th.active.pending && out) {
        th.active.pending = false;
        map_after(out, th.active.a, th.active.bx, th.active.by);
    }
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe get dc %p from %p", dc, _ReturnAddress());
    }
    return r;
}

void* __fastcall hk_set_clip(void* dc, void*, const float* r) {
    ThreadState* const state = thread_state();
    if (!state) return o_set_clip(dc, r);
    ThreadState& th = *state;
    if (g_probe.load(std::memory_order_relaxed) > 0 && g_probe.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe clip dc %p %.1f,%.1f-%.1f,%.1f from %p%s", dc, r[0], r[1], r[2], r[3], _ReturnAddress(),
                 th.active.on ? " (scaled dialog)" : "");
    }
    if (th.active.on && r) {
        const float s = th.active.s;
        const float ox = th.active.ox, oy = th.active.oy;
        float q[4] = {r[0] * s + ox, r[1] * s + oy, r[2] * s + ox, r[3] * s + oy};
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
const PieceRule* piece_rule(uintptr_t piece, const char* parent) {
    if (!parent) return nullptr;
    uint32_t holder = 0, text = 0;
    if (!read32(piece + 0x1C, &holder) || !holder || !read32(holder, &text) || !text) return nullptr;
    wchar_t name[32] = {};
    if (!read_bytes(text, name, sizeof(name) - sizeof(wchar_t))) return nullptr;
    for (const PieceRule& r : kPieceRules) {
        if (std::strcmp(parent, r.dlg) == 0 && std::wcsncmp(name, r.prefix, std::wcslen(r.prefix)) == 0) return &r;
    }
    return nullptr;
}

void* __fastcall hk_piece_draw(void* piece, void*, void* dc) {
    ThreadState* const state = thread_state();
    if (!state) return g_piece_trampoline(piece, dc);
    ThreadState& th = *state;
    if (th.active.on && g_probe_pieces.load(std::memory_order_relaxed) > 0 && g_probe_pieces.fetch_sub(1) > 0) {
        const float* m = reinterpret_cast<const float*>(static_cast<const char*>(dc) + 0x30);
        LOG_INFO("menuprobe piece %ls in %s (scope s %.2f%s) at %.1f,%.1f scale %.3f", piece_name(
                     reinterpret_cast<uintptr_t>(piece)).c_str(), th.parent ? th.parent : "-", th.active.s,
                 th.active.container ? ", container" : "", m[12], m[13], m[0]);
    }
    const PieceRule* rule =
        th.active.on && th.active.container ? piece_rule(reinterpret_cast<uintptr_t>(piece), th.parent) : nullptr;
    if (!rule) return g_piece_trampoline(piece, dc);
    // Its place as its container set it carries the container's scale: undone, then the piece's own.
    const Active outer = th.active;
    Active a = outer;
    a.container = false;
    a.s = rule->keep ? 1.0f : outer.s;
    a.ts = rule->keep ? 1.0f : outer.ts;
    a.ax = rule->ax;
    a.ay = rule->ay;
    a.ox = a.ax * (1.0f - a.s);
    a.oy = a.ay * (1.0f - a.s);
    const float k = a.s / outer.s;
    const float bx = -outer.ox * k + a.ox;
    const float by = -outer.oy * k + a.oy;
    float placed[16], m[16];
    std::memcpy(placed, static_cast<const char*>(dc) + 0x30, sizeof(placed));
    std::memcpy(m, placed, sizeof(m));
    map_after(m, k, bx, by);
    o_set_transform(dc, m);
    a.pending = false;
    th.active = a;
    void* ret = g_piece_trampoline(piece, dc);
    th.active = outer;
    o_set_transform(dc, placed);
    return ret;
}

// ---------------------------------------------------------------------------------------------------------------
// The letters' size (UIScaleText). Our command in the menus' command buffer (the context's +0x24: +0x14 the write
// (and, played back, read) position, +0x18 + +0x1C its end; each command the address of its function, 4-aligned,
// then its arguments, played back as fn(ecx = the buffer)): the text renderer's letter scale and line height times
// `ts` (what & 1: the letters and the line height, after a begin; what & 2: the line height alone, after the game set
// it).
enum : uint32_t { kTextLetters = 1, kTextLine = 2 };

void __fastcall cmd_text_scale(uintptr_t buf) {
    auto* pos = reinterpret_cast<uint32_t*>(buf + 0x14);
    const uint32_t at = (*pos + 3) & ~3u;
    const uint32_t what = *reinterpret_cast<const uint32_t*>(at);
    const float ts = *reinterpret_cast<const float*>(at + 4);
    *pos = at + 8;
    const uintptr_t st = *reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(buf + 0x20) + 0x38);
    void* lock = *reinterpret_cast<void* const*>(st + 0xC4);
    void* const* lvt = *reinterpret_cast<void* const* const*>(lock);
    reinterpret_cast<void(__thiscall*)(void*, int)>(lvt[2])(lock, -1);
    auto* f = reinterpret_cast<float*>(st);
    if (what & kTextLetters) {
        f[0x5C / 4] *= ts;
        f[0x60 / 4] *= ts;
    }
    f[0xB8 / 4] *= ts;
    reinterpret_cast<void(__thiscall*)(void*)>(lvt[4])(lock);
}

void record_text_scale(void* printer, uint32_t what, float ts) {
    const uintptr_t dc = *reinterpret_cast<const uintptr_t*>(static_cast<const char*>(printer) + 4);
    const uintptr_t buf = dc ? *reinterpret_cast<const uintptr_t*>(dc + 0x24) : 0;
    if (!buf) return;
    auto* pos = reinterpret_cast<uint32_t*>(buf + 0x14);
    const uint32_t end = *reinterpret_cast<const uint32_t*>(buf + 0x18) + *reinterpret_cast<const uint32_t*>(buf + 0x1C);
    const uint32_t at = (*pos + 3) & ~3u;
    if (at + 12 >= end) return;  // no room: the text keeps the game's size (the game's own next command fails anyway)
    *reinterpret_cast<uint32_t*>(at) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&cmd_text_scale));
    *reinterpret_cast<uint32_t*>(at + 4) = what;
    *reinterpret_cast<float*>(at + 8) = ts;
    *pos = at + 12;
}

std::atomic<int> g_probe_text{0};

uint32_t __fastcall hk_printer_begin(void* printer, void*, uint32_t font) {
    ThreadState* const state = thread_state();
    if (!state) return o_printer_begin(printer, font);
    ThreadState& th = *state;
    const uint32_t r = o_printer_begin(printer, font);
    if (th.active.on && th.active.ts != 1.0f) record_text_scale(printer, kTextLetters | kTextLine, th.active.ts);
    if (g_probe_text.load(std::memory_order_relaxed) > 0 && g_probe_text.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe text: font size %u in %s, letters at %.2f (dialog at %.2f) from %p", font,
                 th.parent ? th.parent : "-", th.active.on ? th.active.ts : 1.0f, th.active.on ? th.active.s : 1.0f,
                 _ReturnAddress());
    }
    return r;
}

uint32_t __fastcall hk_printer_line(void* printer, void*, uint32_t height) {
    ThreadState* const state = thread_state();
    if (!state) return o_printer_line(printer, height);
    ThreadState& th = *state;
    const uint32_t r = o_printer_line(printer, height);
    if (th.active.on && th.active.ts != 1.0f) record_text_scale(printer, kTextLine, th.active.ts);
    if (g_probe_text.load(std::memory_order_relaxed) > 0 && g_probe_text.fetch_sub(1) > 0) {
        LOG_INFO("menuprobe text: line height %d in %s from %p", static_cast<int16_t>(height & 0xFFFF),
                 th.parent ? th.parent : "-", _ReturnAddress());
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// The mouse in a scaled menu. Once a frame the game turns the pointer's place into the layout's units (0xF70520, with
// esi = the mouse's state [0x137CF84], called from 0xC2B9BD and 0xF9187A, `ret 4`): +0x20, +0x24 the place now,
// +0x28, +0x2C the last frame's, +0x38 set when it moved; it reads the pointer only while [[0x1378458]+0x38] is set.
// The menus test that place against their dialogs' rectangles, which are drawn scaled about the middle of the screen
// (UIScaleMenus): the place is mapped back into the menus' own units, and "moved" stays the pointer's own (a menu
// opening at another scale is not a move, which would put its cursor under the pointer).
constexpr uintptr_t kCursorFn = 0x00F70520;
constexpr uintptr_t kCursorCalls[] = {0x00C2B9BD, 0x00F9187A};
uintptr_t g_cursor_fn = kCursorFn;
int g_raw_x = 0, g_raw_y = 0;
bool g_raw_valid = false;
float g_mouse_applied = 1.0f;

void __cdecl cursor_after(uintptr_t st, uint32_t read) {
    if (!read) return;
    auto* p = reinterpret_cast<int32_t*>(st);
    const int x = p[0x20 / 4], y = p[0x24 / 4];
    const int px = g_raw_valid ? g_raw_x : x, py = g_raw_valid ? g_raw_y : y;
    const float s = g_mouse_s, ax = g_mouse_ax, ay = g_mouse_ay;
    if (s != 1.0f || g_mouse_applied != 1.0f) {
        const auto back = [s](int v, float c) { return static_cast<int32_t>(std::floor(c + (v - c) / s + 0.5f)); };
        p[0x20 / 4] = back(x, ax);
        p[0x24 / 4] = back(y, ay);
        p[0x28 / 4] = back(px, ax);
        p[0x2C / 4] = back(py, ay);
        *reinterpret_cast<uint8_t*>(st + 0x38) = (x != px || y != py) ? 1 : 0;
    }
    g_mouse_applied = s;
    g_raw_x = x;
    g_raw_y = y;
    g_raw_valid = true;
}

__declspec(naked) void stub_cursor() {
    __asm {
        mov eax, dword ptr ds:[0x01378458]
        movzx eax, byte ptr [eax + 0x38]
        push eax                      // whether the game reads the pointer this time
        push dword ptr [esp + 8]      // its argument, again
        call dword ptr [g_cursor_fn]  // ret 4
        xchg eax, dword ptr [esp]     // its result kept; eax = the flag
        pushad
        push eax
        push esi
        call cursor_after
        add esp, 8
        popad
        pop eax
        ret 4
    }
}

bool install_cursor_hook() {
    for (uintptr_t site : kCursorCalls) {
        uint8_t code[5] = {};
        if (!read_bytes(site, code, sizeof(code)) || code[0] != 0xE8) return false;
        int32_t rel = 0;
        std::memcpy(&rel, code + 1, 4);
        if (site + 5 + rel != kCursorFn) return false;
    }
    for (uintptr_t site : kCursorCalls) {
        uint8_t code[5] = {0xE8};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&stub_cursor) - (site + 5));
        std::memcpy(code + 1, &rel, 4);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(site), 5, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(reinterpret_cast<void*>(site), code, 5);
        DWORD ignored = 0;
        VirtualProtect(reinterpret_cast<void*>(site), 5, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), 5);
    }
    return true;
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
bool g_cursor_ok = true;  // the mouse's place can follow a scaled menu (else the menus keep their size)

}  // namespace

bool menuscale_install() {
    if (g_installed) return true;
    if (g_tls_slot == TLS_OUT_OF_INDEXES) g_tls_slot = TlsAlloc();
    if (g_tls_slot == TLS_OUT_OF_INDEXES) {
        LOG_ERROR("MenuScale: no thread slot (%lu); UIScale stays off", GetLastError());
        return false;
    }
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
    // The letters (UIScaleText): the font printer's begin and line height, and the text renderer's state as found
    // (the begin command 0xF4B150 reading the size table 0x12E3BA8 into +0x5C/+0x60 of the state, written through
    // the state at esi = state + 0x24: `mov [esi+0x38], eax; mov [esi+0x3C], ecx`).
    static const uint8_t kBeginCmd[] = {0x89, 0x46, 0x38, 0x89, 0x4E, 0x3C};
    static const uint8_t kLineCmd[] = {0xF3, 0x0F, 0x11, 0x86, 0x94, 0x00, 0x00, 0x00};
    uint32_t pb = 0, pl = 0;
    if (g_installed && read32(kPrinterVtable + 4 * kSlotPrinterBegin, &pb) && pb == 0x00FA6700 &&
        read32(kPrinterVtable + 4 * kSlotPrinterLine, &pl) && pl == 0x00FA6630 &&
        std::memcmp(reinterpret_cast<const void*>(0x00F4B308), kBeginCmd, sizeof(kBeginCmd)) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(0x00F4A679), kLineCmd, sizeof(kLineCmd)) == 0) {
        o_printer_begin = reinterpret_cast<PrinterFn>(
            patch_vtable(kPrinterVtable, kSlotPrinterBegin, reinterpret_cast<void*>(&hk_printer_begin)));
        o_printer_line = reinterpret_cast<PrinterFn>(
            patch_vtable(kPrinterVtable, kSlotPrinterLine, reinterpret_cast<void*>(&hk_printer_line)));
    }
    if (g_installed && (!o_printer_begin || !o_printer_line)) {
        LOG_ERROR("MenuScale: the font printer is not what it is in this build (%08X %08X); the HUD's letters keep "
                  "the game's size", pb, pl);
    }
    // The mouse's place in a scaled menu (UIScaleMenus).
    if (g_installed && !install_cursor_hook()) {
        LOG_ERROR("MenuScale: the mouse's place is not read as in this build; the menus keep the game's size");
        g_cursor_ok = false;
    }
    return g_installed;
}

bool menuscale_installed() { return g_installed; }

void menuscale_detach() {
    // The process is ending (DLL_PROCESS_DETACH): the slot back; a hook still called finds no state and draws unscaled.
    if (g_tls_slot == TLS_OUT_OF_INDEXES) return;
    const DWORD slot = g_tls_slot;
    g_tls_slot = TLS_OUT_OF_INDEXES;
    TlsFree(slot);
}

void menuscale_set(int ui_scale, int ui_scale_world, int ui_scale_text, int ui_scale_menus) {
    if (ui_scale >= 0) g_scale_pct.store(ui_scale);
    if (ui_scale_world >= 0) g_world_pct.store(ui_scale_world);
    if (ui_scale_text >= 0) g_text_pct.store(ui_scale_text);
    if (ui_scale_menus >= 0) g_menu_pct.store(g_cursor_ok ? ui_scale_menus : 100);
    ui_scale = g_scale_pct.load();
    ui_scale_world = g_world_pct.load();
    ui_scale_text = g_text_pct.load();
    char text[96];
    if (!o_printer_begin) {
        std::snprintf(text, sizeof(text), "their letters at the game's size (no font printer hooks)");
    } else if (ui_scale_text >= 25 && ui_scale_text <= 200) {
        std::snprintf(text, sizeof(text), "their letters at %d%% (UIScaleText)", ui_scale_text);
    } else {
        std::snprintf(text, sizeof(text), "their letters with them");
    }
    LOG_INFO("UIScale: the HUD's dialogs are drawn at %d%% (the marks over things in the world at %d%%), %s, by the "
             "game's own placement: each dialog toward its corner or edge, whatever overlaps it; the in-game menus at "
             "%d%% about the middle of the screen (UIScaleMenus %s)", ui_scale, ui_scale_world, text, menu_pct(),
             g_menu_pct.load() == 0 ? "auto" : "set");
}

void menuscale_fix_boss_bar(bool on) {
    g_fix_boss.store(on);
    LOG_INFO("FixBossBarOverlap: %s", on ? "the prompts and the item windows above them go up over a boss's health "
                                           "bar while one is shown (the game draws them over it)"
                                         : "off (the game's places)");
}

void menuscale_probe(int n) { g_probe.store(n); }
void menuscale_probe_dialogs(int n) {
    g_probe_dialogs.store(n);
    g_probe_pieces.store(n);
    g_probe_text.store(n);
}
