#include "settingsmenu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "atoc.h"
#include "drawdist.h"
#include "foliage.h"
#include "footik.h"
#include "inputcache.h"
#include "layout.h"
#include "limiter.h"
#include "log.h"
#include "menuscale.h"
#include "resolution.h"

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

// FPS Unlock Settings in Prepare to Die Edition (the page itself, the INI and the controls: src/common/settingspage.cpp).
// Here: its rows, the Direct3D 9 drawing and the game's controls.
//
// The page's picture is uploaded to a texture when it changes and drawn over the finished frame in the device's
// Present (state saved and put back with a state block, the render target and depth buffer by hand). The PC settings
// screen (NS_FRPG::FrpgMenuDlgPcOption, method table 0x11D28E4) is noticed by its Draw (slot 0x48, thiscall(dlg, dc),
// on the game thread): while it draws, Y opens the page and a line at the bottom says so. The keys come from the
// game's DirectInput keyboard state (inputcache.cpp; no GetAsyncKeyState in the release DLL), the controller from
// every XInput read (xinput_proxy.cpp), and while the page is open (until every button is let go after closing) the
// game gets neither.
namespace {

constexpr uintptr_t kPcOptionVtable = 0x011D28E4;
constexpr int kDrawSlot = 0x48 / 4;

using DrawFn = void*(__fastcall*)(void* dlg, void* edx, void* dc);
DrawFn g_pcoption_draw = nullptr;
volatile LONGLONG g_pcoption_seen = 0;

void* __fastcall hk_pcoption_draw(void* dlg, void* edx, void* dc) {
    InterlockedExchange64(&g_pcoption_seen, static_cast<LONGLONG>(GetTickCount64()));
    return g_pcoption_draw(dlg, edx, dc);
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The controller: each slot's latest reading (the game thread writes, the render thread reads).
volatile LONG g_pad_buttons[4] = {};
volatile LONG g_pad_stick[4] = {};  // lx in the low 16 bits, ly in the high 16
volatile LONG g_blank = 0;          // the page has the controls (settingspage's blank callback)

void blank(bool on) {
    InterlockedExchange(&g_blank, on ? 1 : 0);
    inputcache_set_blank(on);
}

// ---------------------------------------------------------------- the FPS Unlock's rows
int to_int(const char* v) { return std::atoi(v); }

void apply_max_fps(const char* v) { limiter_set_fps(to_int(v)); }
void apply_ui_scale(const char* v) {
    if (menuscale_installed()) {
        menuscale_set(to_int(v), -1);
    } else {
        layout_set_scale(to_int(v));
    }
}
void apply_ui_scale_world(const char* v) {
    if (menuscale_installed()) menuscale_set(-1, to_int(v));
}
void apply_dof(const char* v) { resolution_set_dof_strength(static_cast<float>(std::atof(v) / 100.0)); }
void apply_atoc(const char* v) { atoc_set_mode(lower(v) == "false" ? 0 : 2); }
// AntialiasingSamples: off and on at once (the switch in atoc.cpp); a new sample count needs the targets made again.
void apply_aa(const char* v) {
    const std::string s = lower(v);
    atoc_aa_switch(s == "game" ? 0 : (s == "off" || s == "false" || s == "0" || s == "1") ? 2 : 1);
}
void apply_draw_pieces(const char* v) {
    const std::string s = lower(v);
    drawdist_set_mode(s == "all" ? 2 : s == "adjacent+" ? 3 : s == "adjacent" ? 1 : 0);
}
void apply_foot_ik_npcs(const char* v) { footik_set_npcs(lower(v) != "false"); }
void apply_tree_sway(const char* v) { foliage_set_trees(lower(v) == "true"); }
void apply_draw_objects(const char* v) { drawdist_set_widen(lower(v) == "false" ? 7 : 15); }
void apply_lod(const char* v) { drawdist_set_lod(to_int(v)); }
void apply_fx_lod(const char* v) { drawdist_set_effect_lod(to_int(v)); }

const SettingsMenuChoice kFps[] = {{"30", L"30"},   {"40", L"40"},   {"45", L"45"},   {"60", L"60"},   {"72", L"72"},
                                   {"90", L"90"},   {"100", L"100"}, {"120", L"120"}, {"144", L"144"}, {"165", L"165"},
                                   {"180", L"180"}, {"200", L"200"}, {"225", L"225"}, {"240", L"240"}, {"288", L"288"},
                                   {"300", L"300"}, {"360", L"360"}, {"480", L"480"}};
const SettingsMenuChoice kRefresh[] = {{"0", L"PC settings (else highest)"}, {"-1", L"The game's own"}, {"60", L"60 Hz"},
                                       {"75", L"75 Hz"},  {"100", L"100 Hz"}, {"120", L"120 Hz"}, {"144", L"144 Hz"},
                                       {"165", L"165 Hz"}, {"240", L"240 Hz"}, {"360", L"360 Hz"}};
const SettingsMenuChoice kAutoOnOff[] = {{"auto", L"Auto"}, {"true", L"On"}, {"false", L"Off"}};
const SettingsMenuChoice kOnOff[] = {{"true", L"On"}, {"false", L"Off"}};
const SettingsMenuChoice kPercentUi[] = {{"100", L"100%"}, {"95", L"95%"}, {"90", L"90%"}, {"85", L"85%"},
                                         {"80", L"80%"},   {"75", L"75%"}, {"70", L"70%"}, {"65", L"65%"},
                                         {"60", L"60%"},   {"55", L"55%"}, {"50", L"50%"}};
const SettingsMenuChoice kDof[] = {{"100", L"100% (the game's)"}, {"75", L"75%"}, {"50", L"50%"}, {"25", L"25%"},
                                   {"0", L"Off"}, {"125", L"125%"}, {"150", L"150%"}, {"200", L"200%"}};
const SettingsMenuChoice kSamples[] = {{"off", L"Off"}, {"2", L"2x"}, {"4", L"4x"}, {"8", L"8x"},
                                       {"game", L"The game's option"}};
const SettingsMenuChoice kAniso[] = {{"16", L"16x"}, {"8", L"8x"}, {"4", L"4x"}, {"2", L"2x"}, {"0", L"The game's"}};
const SettingsMenuChoice kShadow[] = {{"4096", L"4096"}, {"8192", L"8192"}, {"2048", L"2048 (the game's)"}};
const SettingsMenuChoice kSuper[] = {{"1", L"Off"}, {"1.25", L"1.25x"}, {"1.5", L"1.5x"}, {"1.75", L"1.75x"},
                                     {"2", L"2x"}};
const SettingsMenuChoice kColor[] = {{"true", L"10-bit"}, {"float", L"16-bit float"}, {"false", L"8-bit (the game's)"}};
const SettingsMenuChoice kPieces[] = {{"adjacent", L"Adjacent rooms"}, {"adjacent+", L"Adjacent+"}, {"all", L"All"},
                                      {"game", L"The game's"}};
const SettingsMenuChoice kLod[] = {{"100", L"100% (the game's)"}, {"125", L"125%"}, {"150", L"150%"}, {"200", L"200%"},
                                   {"300", L"300%"}, {"400", L"400%"}};

const SettingsMenuRow kDisplay[] = {
    {"MaxFPS", L"Frame rate cap", L"The highest frame rate the game runs at. Keep it a little below what your PC holds "
     L"in busy places, and below your display's refresh rate.", kFps, _countof(kFps), apply_max_fps, false},
    {"FullscreenRefreshRate", L"Fullscreen refresh rate", L"In fullscreen: the Frequency chosen in the game's PC settings (which now lists every rate), or your "
     L"display's fastest rate while that is the game's own 60 Hz.", kRefresh, _countof(kRefresh), nullptr, false},
    {"BorderlessFullscreen", L"Borderless fullscreen", L"With the game set to windowed: the window without its frame, "
     L"over the whole screen. Auto: only with DXVK.", kAutoOnOff, _countof(kAutoOnOff), nullptr, false},
    {"UIScale", L"HUD size", L"The size of the HUD and the menus.", kPercentUi, _countof(kPercentUi), apply_ui_scale,
     false},
    {"UIScaleWorld", L"Marks over the world", L"The size of names, health bars and other marks over characters and "
     L"things in the world.", kPercentUi, _countof(kPercentUi), apply_ui_scale_world, false},
    {"HideMouseCursor", L"Hide the mouse pointer", L"Hides the mouse pointer over the game window.", kOnOff,
     _countof(kOnOff), nullptr, false},
};
const SettingsMenuRow kPicture[] = {
    {"DepthOfFieldStrength", L"Depth of field", L"How strongly distant things blur.", kDof, _countof(kDof), apply_dof,
     false},
    {"AntialiasingSamples", L"Anti-aliasing", L"Smooth edges (multisampling), whatever the game's own Antialiasing "
     L"option says. More samples: smoother, more GPU time. The game's option: that option decides, 2x.", kSamples,
     _countof(kSamples), apply_aa, false, L"Off and on at once; a new sample count after a restart"},
    {"AlphaToCoverage", L"Smooth foliage edges", L"Grass, leaves and hair get smooth edges with the "
     L"anti-aliasing on.", kOnOff, _countof(kOnOff), apply_atoc, false},
    {"AnisotropicFiltering", L"Texture filtering", L"Anisotropic filtering: sharper ground and walls at an angle.",
     kAniso, _countof(kAniso), nullptr, false},
    {"ShadowMapResolution", L"Shadow detail", L"The shadow map's size: finer shadow edges at more GPU time.", kShadow,
     _countof(kShadow), nullptr, false},
    {"Supersampling", L"Supersampling", L"The scene drawn larger than the screen and scaled down: smoother edges, much "
     L"more GPU time.", kSuper, _countof(kSuper), nullptr, false},
    {"HighPrecisionColor", L"Colour precision", L"10-bit colour removes banding in fog and dark skies.", kColor,
     _countof(kColor), nullptr, false},
    {"HighPrecisionOutput", L"10-bit output", L"With DXVK on an HDR display: the picture sent in 10 bits, with no "
     L"dither.", kAutoOnOff, _countof(kAutoOnOff), nullptr, false},
    {"FoliageSway", L"Swaying foliage", L"Grass, bushes and leaves move in the wind, as in Remastered.", kOnOff,
     _countof(kOnOff), nullptr, false},
    {"FoliageTreeSway", L"Swaying trees", L"With swaying foliage on: the leaves of the trees around Firelink Shrine, in "
     L"the Undead Burg and Parish, on the way to Sen's Fortress and in Anor Londo sway in the wind too.", kOnOff,
     _countof(kOnOff), apply_tree_sway, false},
    {"FootIkNpcs", L"NPCs' feet on the ground", L"Human NPCs and summoned phantoms place their feet on stairs and "
     L"slopes, as your character does.", kOnOff, _countof(kOnOff), apply_foot_ik_npcs, false},
};
const SettingsMenuRow kDistance[] = {
    {"DrawMapPieces", L"Draw distance", L"Adjacent rooms: the map pieces the rooms next to yours see. All: every "
     L"loaded piece (costly). The game's: only what your room sees.", kPieces, _countof(kPieces), apply_draw_pieces,
     false},
    {"DrawMapObjects", L"Objects of nearby rooms", L"With the draw distance above: the objects (torches, ladders, "
     L"crates) of those rooms too.", kOnOff, _countof(kOnOff), apply_draw_objects, false},
    {"LodDistance", L"Detail distance", L"How far models keep their detailed versions.", kLod, _countof(kLod),
     apply_lod, false},
    {"EffectLodDistance", L"Effect distance", L"How far effects (fires, smoke, magic) keep their detail.", kLod,
     _countof(kLod), apply_fx_lod, false},
};

// ---------------------------------------------------------------- drawing
struct Tex {
    IDirect3DTexture9* tex = nullptr;
    unsigned version = 0;
    int w = 0, h = 0;
};
Tex g_panel_tex, g_hint_tex;
IDirect3DStateBlock9* g_sb = nullptr;
IDirect3DDevice9* g_sb_dev = nullptr;

void drop(Tex& t) {
    if (t.tex) t.tex->Release();
    t = Tex{};
}

bool upload(IDirect3DDevice9* dev, Tex& t, const SettingsPageImage* img) {
    if (!img || !img->bits) return false;
    if (t.tex && (t.w != img->w || t.h != img->h)) drop(t);
    if (!t.tex) {
        if (FAILED(dev->CreateTexture(img->w, img->h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, nullptr))) {
            t.tex = nullptr;
            return false;
        }
        t.w = img->w;
        t.h = img->h;
        t.version = img->version - 1;
    }
    if (t.version != img->version) {
        D3DLOCKED_RECT lr{};
        if (FAILED(t.tex->LockRect(0, &lr, nullptr, 0))) return false;
        for (int y = 0; y < img->h; ++y) {
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch,
                        img->bits + static_cast<size_t>(y) * img->w, static_cast<size_t>(img->w) * 4);
        }
        t.tex->UnlockRect(0);
        t.version = img->version;
    }
    return true;
}

void draw(IDirect3DDevice9* dev, const Tex& t, float x, float y) {
    struct V {
        float x, y, z, rhw, u, v;
    };
    const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + t.w - 0.5f, y1 = y + t.h - 0.5f;
    const V v[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->SetTexture(0, t.tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}

void render(IDirect3DDevice9* dev, bool panel, bool hint) {
    if (g_sb_dev != dev) {
        if (g_sb) g_sb->Release();
        g_sb = nullptr;
        g_sb_dev = dev;
    }
    if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) {
        g_sb = nullptr;
        return;
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC bd{};
    bb->GetDesc(&bd);
    const int sh = static_cast<int>(bd.Height);
    if (panel && !upload(dev, g_panel_tex, settingspage_panel(static_cast<int>(bd.Width), sh))) panel = false;
    if (hint) {
        std::wstring line = L"Y: FPS Unlock Settings";
        const int key = settingspage_key();
        if (key >= VK_F1 && key <= VK_F12) line += L"   (keyboard: F" + std::to_wstring(key - VK_F1 + 1) + L")";
        if (!upload(dev, g_hint_tex, settingspage_hint(static_cast<int>(bd.Width), sh, line.c_str()))) hint = false;
    }
    if (!panel && !hint) {
        bb->Release();
        return;
    }
    IDirect3DSurface9* old_rt = nullptr;
    IDirect3DSurface9* old_ds = nullptr;
    dev->GetRenderTarget(0, &old_rt);
    dev->GetDepthStencilSurface(&old_ds);
    g_sb->Capture();
    dev->SetRenderTarget(0, bb);
    dev->SetDepthStencilSurface(nullptr);
    D3DVIEWPORT9 vp{0, 0, bd.Width, bd.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    if (SUCCEEDED(dev->BeginScene())) {
        dev->SetVertexShader(nullptr);
        dev->SetPixelShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
        if (panel) {
            draw(dev, g_panel_tex, static_cast<float>((static_cast<int>(bd.Width) - g_panel_tex.w) / 2),
                 static_cast<float>((sh - g_panel_tex.h) / 2));
        }
        if (hint) {
            draw(dev, g_hint_tex, static_cast<float>((static_cast<int>(bd.Width) - g_hint_tex.w) / 2),
                 static_cast<float>(sh - g_hint_tex.h - sh / 54));
        }
        dev->SetTexture(0, nullptr);
        dev->EndScene();
    }
    dev->SetRenderTarget(0, old_rt);
    dev->SetDepthStencilSurface(old_ds);
    g_sb->Apply();
    if (old_rt) old_rt->Release();
    if (old_ds) old_ds->Release();
    bb->Release();
}

// ---------------------------------------------------------------- input
bool game_window_focused() {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// The keys, as the game's keyboard reads them (DirectInput key codes; no GetAsyncKeyState in the release DLL). Only
// while the game window has the focus does the game read its keyboard.
int dik_of(int vk) {
    static const uint8_t kLetters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                         0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (vk >= 'A' && vk <= 'Z') return kLetters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return 0x02 + (vk - '1');
    if (vk == '0') return 0x0B;
    if (vk >= VK_F1 && vk <= VK_F10) return 0x3B + (vk - VK_F1);
    switch (vk) {
        case VK_F11: return 0x57;
        case VK_F12: return 0x58;
        case VK_ESCAPE: return 0x01;
        case VK_BACK: return 0x0E;
        case VK_RETURN: return 0x1C;
        case VK_UP: return 0xC8;
        case VK_DOWN: return 0xD0;
        case VK_LEFT: return 0xCB;
        case VK_RIGHT: return 0xCD;
        case VK_PRIOR: return 0xC9;
        case VK_NEXT: return 0xD1;
        case VK_SPACE: return 0x39;
        case VK_OEM_MINUS: return 0x0C;
        default: return 0;
    }
}

bool key(int vk) {
    if (vk == VK_SHIFT) return inputcache_key(0x2A) || inputcache_key(0x36);
    if (vk == VK_RETURN && inputcache_key(0x9C)) return true;  // the keypad's Enter
    return inputcache_key(dik_of(vk));
}

SettingsPageInput read_input() {
    SettingsPageInput in;
    for (int i = 0; i < 4; ++i) {
        in.pad = static_cast<uint16_t>(in.pad | static_cast<uint16_t>(g_pad_buttons[i]));
        const LONG st = g_pad_stick[i];
        const int x = static_cast<int16_t>(st & 0xFFFF), y = static_cast<int16_t>((st >> 16) & 0xFFFF);
        if (std::abs(x) > std::abs(in.lx)) in.lx = x;
        if (std::abs(y) > std::abs(in.ly)) in.ly = y;
    }
    in.keyboard = game_window_focused();
    in.key = key;
    in.pad_opens = GetTickCount64() - static_cast<ULONGLONG>(g_pcoption_seen) < 300;
    return in;
}

}  // namespace

void settingsmenu_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count) {
    settingspage_add_page(title, rows, count);
}

void settingsmenu_install(const wchar_t* ini_path, int key) {
    settingspage_init(ini_path, key, blank);
    settingspage_add_page(L"Display", kDisplay, _countof(kDisplay));
    settingspage_add_page(L"Picture", kPicture, _countof(kPicture));
    settingspage_add_page(L"Draw distance", kDistance, _countof(kDistance));
    inputcache_require_hook();
    // The PC settings screen's Draw: its own method table entry (every dialog class has one).
    auto** slot = reinterpret_cast<void**>(kPcOptionVtable) + kDrawSlot;
    DWORD old = 0;
    __try {
        if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
            g_pcoption_draw = reinterpret_cast<DrawFn>(*slot);
            *slot = reinterpret_cast<void*>(&hk_pcoption_draw);
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_pcoption_draw = nullptr;
    }
    char k[16] = "none";
    if (key >= VK_F1 && key <= VK_F24) std::snprintf(k, sizeof(k), "F%d", key - VK_F1 + 1);
    LOG_INFO("FPS Unlock Settings: open with %s, or Y on a controller in the PC settings screen%s", k,
             g_pcoption_draw ? "" : " (the PC settings screen was not found: the key only)");
}

bool settingsmenu_enabled() { return settingspage_enabled(); }
void settingsmenu_set_open(bool open) { settingspage_set_open(open); }
bool settingsmenu_is_open() { return settingspage_is_open(); }

void settingsmenu_present(IDirect3DDevice9* device) {
    if (!settingspage_enabled() || !device) return;
    settingspage_frame(read_input());
    const bool open = settingspage_is_open();
    const bool hint = !open && GetTickCount64() - static_cast<ULONGLONG>(g_pcoption_seen) < 300;
    if (open || hint) render(device, open, hint);
}

void settingsmenu_reset() {
    if (g_sb) {
        g_sb->Release();
        g_sb = nullptr;
    }
    g_sb_dev = nullptr;
}

void settingsmenu_pad(unsigned long index, void* state, unsigned long result) {
    if (!settingspage_enabled() || index > 3) return;
    if (result != 0 || !state) {
        InterlockedExchange(&g_pad_buttons[index], 0);
        InterlockedExchange(&g_pad_stick[index], 0);
        return;
    }
    // XINPUT_STATE: packet (4), buttons (2), triggers (2), lx, ly, rx, ry.
    auto* b = static_cast<uint8_t*>(state);
    WORD buttons = 0;
    int16_t lx = 0, ly = 0;
    std::memcpy(&buttons, b + 4, 2);
    std::memcpy(&lx, b + 8, 2);
    std::memcpy(&ly, b + 10, 2);
    InterlockedExchange(&g_pad_buttons[index], buttons);
    InterlockedExchange(&g_pad_stick[index], static_cast<LONG>((static_cast<uint32_t>(static_cast<uint16_t>(ly)) << 16) |
                                                               static_cast<uint16_t>(lx)));
    if (g_blank) std::memset(b + 4, 0, 12);
}
