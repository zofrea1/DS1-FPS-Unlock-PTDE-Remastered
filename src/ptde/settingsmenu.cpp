#include "settingsmenu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "atoc.h"
#include "drawdist.h"
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
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")

// FPS Unlock Settings: a page drawn over the game with the settings a player would reasonably change while playing
// (the frame cap, the display, the picture, the draw distance; a bundled mod adds its own page). The fixes, speed-ups
// and diagnostics stay in the INI only. Opened with a key (SettingsMenuKey, F1 by default) or, on a controller, with Y
// while the game's PC settings screen is up; while it is open the game gets no controller, keyboard or mouse input.
// Every change is written to the INI at once (only the value on that key's line changes); a row the mod can apply
// while the game runs takes effect at once, the others say they apply after a restart.
//
// The page is text drawn with GDI into a bitmap, uploaded to a texture when it changes, and drawn over the finished
// frame in the device's Present (state saved and put back with a state block, the render target and depth buffer by
// hand). The PC settings screen (NS_FRPG::FrpgMenuDlgPcOption, method table 0x11D28E4) is noticed by its Draw (slot
// 0x48, thiscall(dlg, dc), on the game thread): while it draws, Y opens the page and a line at the bottom says so.
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

// ---------------------------------------------------------------- model
struct Row {
    SettingsMenuRow def{};
    std::vector<SettingsMenuChoice> choices;
    std::string custom;          // an INI value not among the choices (kept as the first entry)
    std::wstring custom_label;
    int current = 0;
    bool restart = false;        // changed, applies after a restart
    std::string text;            // text rows
};
struct Page {
    std::wstring title;
    std::vector<Row> rows;
};

std::recursive_mutex g_lock;  // pages, rows, the INI file
std::vector<Page> g_pages;
std::wstring g_ini;
int g_key = VK_F1;
bool g_enabled = false;

volatile LONG g_open = 0;
volatile LONG g_swallow = 0;  // after closing: the game gets no input until every button is let go
int g_page = 0, g_row = 0, g_scroll = 0;
bool g_editing = false;
std::string g_edit;
int g_edit_page = -1, g_edit_row = -1;  // the row being typed into
bool g_was_key[0x5B] = {};
bool g_was_space = false, g_was_minus = false;

// The controller: each slot's latest reading (the game thread writes, the render thread reads).
volatile LONG g_pad_buttons[4] = {};
volatile LONG g_pad_stick[4] = {};  // lx in the low 16 bits, ly in the high 16

volatile LONG g_dirty = 1;

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}
std::wstring widen(const std::string& s) {
    std::wstring w(s.size(), L' ');
    for (size_t i = 0; i < s.size(); ++i) w[i] = static_cast<unsigned char>(s[i]);
    return w;
}

// ---------------------------------------------------------------- the INI
bool read_ini(std::string* text) {
    HANDLE f = CreateFileW(g_ini.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    text->assign(size, '\0');
    DWORD got = 0;
    const BOOL ok = size == 0 || ReadFile(f, &(*text)[0], size, &got, nullptr);
    CloseHandle(f);
    text->resize(got);
    return ok != FALSE;
}

// The line of KEY (the first that is not a comment or a section): its start and the start/end of its value.
bool find_key(const std::string& text, const char* key, size_t* value_at, size_t* value_end) {
    const std::string want = lower(key);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        size_t p = at;
        while (p < end && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p < end && text[p] != '#' && text[p] != ';' && text[p] != '[') {
            const size_t eq = text.find('=', p);
            if (eq != std::string::npos && eq < end && lower(trim(text.substr(p, eq - p))) == want) {
                size_t v = eq + 1;
                while (v < end && (text[v] == ' ' || text[v] == '\t')) ++v;
                size_t e = end;
                while (e > v && (text[e - 1] == '\r' || text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
                *value_at = v;
                *value_end = e;
                return true;
            }
        }
        at = end + 1;
    }
    return false;
}

bool ini_value(const std::string& text, const char* key, std::string* out) {
    size_t v = 0, e = 0;
    if (!find_key(text, key, &v, &e)) return false;
    *out = text.substr(v, e - v);
    return true;
}

// Writes KEY = VALUE: the value on the key's line is replaced, or the line is added at the end.
bool write_ini(const char* key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::string text;
    if (!read_ini(&text)) {
        LOG_ERROR("FPS Unlock Settings: could not read the INI to write %s", key);
        return false;
    }
    size_t v = 0, e = 0;
    if (find_key(text, key, &v, &e)) {
        text.replace(v, e - v, value);
    } else {
        const bool crlf = text.find("\r\n") != std::string::npos;
        if (!text.empty() && text.back() != '\n') text += crlf ? "\r\n" : "\n";
        text += std::string(key) + " = " + value + (crlf ? "\r\n" : "\n");
    }
    const std::wstring tmp = g_ini + L".settingsmenu.tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LOG_ERROR("FPS Unlock Settings: could not write the INI (%s = %s; Win32=%lu)", key, value.c_str(),
                  GetLastError());
        return false;
    }
    DWORD put = 0;
    const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
    CloseHandle(f);
    if (!ok || put != text.size() || !MoveFileExW(tmp.c_str(), g_ini.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        LOG_ERROR("FPS Unlock Settings: could not replace the INI (%s = %s; Win32=%lu)", key, value.c_str(),
                  GetLastError());
        return false;
    }
    LOG_INFO("FPS Unlock Settings: %s = %s written to the INI", key, value.c_str());
    return true;
}

bool same_value(const std::string& a, const std::string& b) {
    const std::string x = lower(trim(a)), y = lower(trim(b));
    if (x == y) return true;
    char* ea = nullptr;
    char* eb = nullptr;
    const double da = std::strtod(x.c_str(), &ea), db = std::strtod(y.c_str(), &eb);
    return ea != x.c_str() && eb != y.c_str() && *ea == 0 && *eb == 0 && da == db;
}

void load_row(Row& row, const std::string& text) {
    std::string value;
    const bool found = ini_value(text, row.def.key, &value);
    if (row.def.text) {
        row.text = found ? value : "";
        return;
    }
    row.current = 0;
    if (!found) return;  // the first choice is the default
    for (size_t i = 0; i < row.choices.size(); ++i) {
        if (same_value(row.choices[i].ini, value)) {
            row.current = static_cast<int>(i);
            return;
        }
    }
    row.custom = value;
    row.custom_label = widen(value);
    row.choices.insert(row.choices.begin(), SettingsMenuChoice{row.custom.c_str(), row.custom_label.c_str()});
    row.current = 0;
}

void add_page(const wchar_t* title, const SettingsMenuRow* rows, int count) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::string text;
    read_ini(&text);
    Page page;
    page.title = title;
    page.rows.reserve(count);
    for (int i = 0; i < count; ++i) {
        Row row;
        row.def = rows[i];
        row.choices.assign(rows[i].choices, rows[i].choices + rows[i].choice_count);
        page.rows.push_back(row);
    }
    g_pages.push_back(std::move(page));
    for (Row& row : g_pages.back().rows) {
        load_row(row, text);
        // custom points into the row's own string: fix up after the row has its final place
        if (!row.custom.empty()) {
            row.choices[0].ini = row.custom.c_str();
            row.choices[0].label = row.custom_label.c_str();
        }
    }
    InterlockedExchange(&g_dirty, 1);
}

void change(Row& row, int step) {
    if (row.def.text || row.choices.empty()) return;
    const int n = static_cast<int>(row.choices.size());
    row.current = ((row.current + step) % n + n) % n;
    const char* value = row.choices[row.current].ini;
    write_ini(row.def.key, value);
    if (row.def.apply) {
        row.def.apply(value);
    } else {
        row.restart = true;
    }
    InterlockedExchange(&g_dirty, 1);
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
void apply_draw_pieces(const char* v) {
    const std::string s = lower(v);
    drawdist_set_mode(s == "all" ? 2 : s == "adjacent+" ? 3 : s == "adjacent" ? 1 : 0);
}
void apply_draw_objects(const char* v) { drawdist_set_widen(lower(v) == "false" ? 7 : 15); }
void apply_lod(const char* v) { drawdist_set_lod(to_int(v)); }
void apply_fx_lod(const char* v) { drawdist_set_effect_lod(to_int(v)); }

const SettingsMenuChoice kFps[] = {{"30", L"30"},   {"40", L"40"},   {"45", L"45"},   {"60", L"60"},   {"72", L"72"},
                                   {"90", L"90"},   {"100", L"100"}, {"120", L"120"}, {"144", L"144"}, {"165", L"165"},
                                   {"180", L"180"}, {"200", L"200"}, {"225", L"225"}, {"240", L"240"}, {"288", L"288"},
                                   {"300", L"300"}, {"360", L"360"}, {"480", L"480"}};
const SettingsMenuChoice kRefresh[] = {{"0", L"Highest"}, {"-1", L"The game's own (60 Hz)"}, {"60", L"60 Hz"},
                                       {"75", L"75 Hz"},  {"100", L"100 Hz"}, {"120", L"120 Hz"}, {"144", L"144 Hz"},
                                       {"165", L"165 Hz"}, {"240", L"240 Hz"}, {"360", L"360 Hz"}};
const SettingsMenuChoice kAutoOnOff[] = {{"auto", L"Auto"}, {"true", L"On"}, {"false", L"Off"}};
const SettingsMenuChoice kOnOff[] = {{"true", L"On"}, {"false", L"Off"}};
const SettingsMenuChoice kPercentUi[] = {{"100", L"100%"}, {"95", L"95%"}, {"90", L"90%"}, {"85", L"85%"},
                                         {"80", L"80%"},   {"75", L"75%"}, {"70", L"70%"}, {"65", L"65%"},
                                         {"60", L"60%"},   {"55", L"55%"}, {"50", L"50%"}};
const SettingsMenuChoice kDof[] = {{"100", L"100% (the game's)"}, {"75", L"75%"}, {"50", L"50%"}, {"25", L"25%"},
                                   {"0", L"Off"}, {"125", L"125%"}, {"150", L"150%"}, {"200", L"200%"}};
const SettingsMenuChoice kSamples[] = {{"2", L"2x (the game's)"}, {"4", L"4x"}, {"8", L"8x"}};
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
    {"FullscreenRefreshRate", L"Fullscreen refresh rate", L"In fullscreen the game asks for 60 Hz; Highest uses your "
     L"display's fastest rate at the game's resolution.", kRefresh, _countof(kRefresh), nullptr, false},
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
    {"AntialiasingSamples", L"Anti-aliasing samples", L"Samples of the game's own anti-aliasing (its Antialiasing "
     L"option). More: smoother edges, more GPU time.", kSamples, _countof(kSamples), nullptr, false},
    {"AlphaToCoverage", L"Smooth foliage edges", L"Grass, leaves and hair get smooth edges with the game's "
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
struct Surface {
    int w = 0, h = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    IDirect3DTexture9* tex = nullptr;
    bool uploaded = false;
};
Surface g_panel, g_hint;
IDirect3DStateBlock9* g_sb = nullptr;
IDirect3DDevice9* g_sb_dev = nullptr;
int g_built_h = 0;

bool make_surface(Surface& s, int w, int h) {
    if (s.w == w && s.h == h && s.dc) return true;
    if (s.dc) {
        SelectObject(s.dc, s.old);
        DeleteObject(s.bmp);
        DeleteDC(s.dc);
        s.dc = nullptr;
    }
    if (s.tex) {
        s.tex->Release();
        s.tex = nullptr;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    s.dc = CreateCompatibleDC(nullptr);
    s.bmp = s.dc ? CreateDIBSection(s.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!s.bmp) {
        if (s.dc) DeleteDC(s.dc);
        s.dc = nullptr;
        return false;
    }
    s.old = SelectObject(s.dc, s.bmp);
    s.bits = static_cast<uint32_t*>(bits);
    s.w = w;
    s.h = h;
    s.uploaded = false;
    return true;
}

HFONT font(int px, bool bold) {
    return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_ROMAN,
                       L"Georgia");
}

void fill(Surface& s, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(s.dc, &r, b);
    DeleteObject(b);
}

void text(Surface& s, HFONT f, COLORREF c, int x, int y, int w, int h, const wchar_t* str, UINT flags) {
    HGDIOBJ was = SelectObject(s.dc, f);
    SetTextColor(s.dc, c);
    SetBkMode(s.dc, TRANSPARENT);
    RECT r{x, y, x + w, y + h};
    DrawTextW(s.dc, str, -1, &r, flags | DT_NOPREFIX);
    SelectObject(s.dc, was);
}

// Every pixel gets the alpha (GDI leaves it 0).
void set_alpha(Surface& s, uint8_t a) {
    const size_t n = static_cast<size_t>(s.w) * s.h;
    for (size_t i = 0; i < n; ++i) s.bits[i] = (s.bits[i] & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

constexpr COLORREF kColBack = RGB(14, 12, 10), kEdge = RGB(120, 96, 58), kGold = RGB(214, 186, 128),
                   kWhite = RGB(232, 228, 218), kGrey = RGB(150, 144, 132), kSel = RGB(96, 34, 14),
                   kLive = RGB(150, 196, 140), kLater = RGB(222, 164, 92);

void build_panel(int screen_h) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const float k = static_cast<float>(screen_h) / 1080.0f;
    auto K = [k](int v) { return static_cast<int>(v * k + 0.5f); };
    const int W = K(1160), H = K(860);
    if (!make_surface(g_panel, W, H)) return;
    Surface& s = g_panel;
    fill(s, 0, 0, W, H, kColBack);
    fill(s, 0, 0, W, K(2), kEdge);
    fill(s, 0, H - K(2), W, K(2), kEdge);
    fill(s, 0, 0, K(2), H, kEdge);
    fill(s, W - K(2), 0, K(2), H, kEdge);
    HFONT title = font(K(40), true), tab = font(K(26), false), rowf = font(K(29), false), smallf = font(K(23), false);
    text(s, title, kGold, K(40), K(22), W - K(80), K(56), L"FPS Unlock Settings", DT_LEFT | DT_SINGLELINE);
    // Page tabs, right-aligned on the title line.
    std::wstring tabs;
    for (size_t i = 0; i < g_pages.size(); ++i) {
        if (i) tabs += L"     ";
        tabs += g_pages[i].title;
    }
    {
        // The current page in gold: draw all grey, then the current one over it at its place.
        HGDIOBJ was = SelectObject(s.dc, tab);
        SIZE total{};
        GetTextExtentPoint32W(s.dc, tabs.c_str(), static_cast<int>(tabs.size()), &total);
        int x = W - K(40) - total.cx;
        for (size_t i = 0; i < g_pages.size(); ++i) {
            const std::wstring& t = g_pages[i].title;
            SIZE sz{};
            GetTextExtentPoint32W(s.dc, t.c_str(), static_cast<int>(t.size()), &sz);
            text(s, tab, static_cast<int>(i) == g_page ? kGold : kGrey, x, K(36), sz.cx + K(4), K(36), t.c_str(),
                 DT_LEFT | DT_SINGLELINE);
            if (static_cast<int>(i) == g_page) fill(s, x, K(70), sz.cx, K(2), kGold);
            SIZE gap{};
            GetTextExtentPoint32W(s.dc, L"     ", 5, &gap);
            x += sz.cx + gap.cx;
        }
        SelectObject(s.dc, was);
    }
    fill(s, K(30), K(92), W - K(60), K(1), kEdge);

    const int top = K(110), row_h = K(50), help_top = H - K(210);
    const int visible = (help_top - top) / row_h;
    if (g_page >= 0 && g_page < static_cast<int>(g_pages.size())) {
        Page& p = g_pages[g_page];
        if (g_row < g_scroll) g_scroll = g_row;
        if (g_row >= g_scroll + visible) g_scroll = g_row - visible + 1;
        for (int i = g_scroll; i < static_cast<int>(p.rows.size()) && i < g_scroll + visible; ++i) {
            Row& r = p.rows[i];
            const int y = top + (i - g_scroll) * row_h;
            const bool sel = i == g_row;
            if (sel) fill(s, K(30), y, W - K(60), row_h - K(4), kSel);
            std::wstring label = r.def.label;
            text(s, rowf, kWhite, K(50), y, W / 2, row_h - K(4), label.c_str(), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            std::wstring value;
            if (r.def.text) {
                // Shown only while it is typed (a password on a stream or a screenshot stays private).
                value = sel && g_editing ? widen(g_edit) + L"_"
                        : r.text.empty() ? std::wstring(L"(none)")
                                         : std::wstring(r.text.size(), L'\x2022');
            } else if (!r.choices.empty()) {
                value = std::wstring(sel ? L"\x2039  " : L"") + r.choices[r.current].label + (sel ? L"  \x203A" : L"");
            }
            text(s, rowf, sel ? kGold : kWhite, W / 2, y, W / 2 - K(50), row_h - K(4), value.c_str(),
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            if (r.restart) {
                text(s, smallf, kLater, W / 2 - K(140), y, K(160), row_h - K(4), L"restart", DT_RIGHT | DT_VCENTER |
                     DT_SINGLELINE);
            }
        }
        fill(s, K(30), help_top, W - K(60), K(1), kEdge);
        if (g_row >= 0 && g_row < static_cast<int>(p.rows.size())) {
            const Row& r = p.rows[g_row];
            text(s, smallf, kWhite, K(50), help_top + K(14), W - K(100), K(84), r.def.help, DT_LEFT | DT_WORDBREAK);
            text(s, smallf, r.def.apply && !r.def.when ? kLive : kLater, K(50), help_top + K(104), W - K(100), K(32),
                 r.def.when  ? r.def.when
                 : r.def.apply ? L"Applies at once"
                               : L"Applies after a restart of the game",
                 DT_LEFT | DT_SINGLELINE);
        }
    }
    text(s, smallf, kGrey, K(40), H - K(52), W - K(80), K(34),
         L"Controller: \x2191\x2193 choose   \x2190\x2192 change   LB/RB page   B close          "
         L"Keys: arrows   Q/E page   Esc close",
         DT_CENTER | DT_SINGLELINE);
    DeleteObject(title);
    DeleteObject(tab);
    DeleteObject(rowf);
    DeleteObject(smallf);
    set_alpha(s, 238);
    s.uploaded = false;
}

void build_hint(int screen_h) {
    const float k = static_cast<float>(screen_h) / 1080.0f;
    auto K = [k](int v) { return static_cast<int>(v * k + 0.5f); };
    const int W = K(520), H = K(44);
    if (g_hint.w == W && g_hint.h == H && g_hint.dc) return;
    if (!make_surface(g_hint, W, H)) return;
    fill(g_hint, 0, 0, W, H, kColBack);
    HFONT f = font(K(24), false);
    std::wstring line = L"Y: FPS Unlock Settings";
    if (g_key >= VK_F1 && g_key <= VK_F12) line += L"   (keyboard: F" + std::to_wstring(g_key - VK_F1 + 1) + L")";
    text(g_hint, f, kGold, 0, 0, W, H, line.c_str(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f);
    set_alpha(g_hint, 200);
    g_hint.uploaded = false;
}

bool upload(IDirect3DDevice9* dev, Surface& s) {
    if (!s.tex) {
        if (FAILED(dev->CreateTexture(s.w, s.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s.tex, nullptr))) {
            s.tex = nullptr;
            return false;
        }
        s.uploaded = false;
    }
    if (!s.uploaded) {
        D3DLOCKED_RECT lr{};
        if (FAILED(s.tex->LockRect(0, &lr, nullptr, 0))) return false;
        for (int y = 0; y < s.h; ++y) {
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, s.bits + static_cast<size_t>(y) * s.w,
                        static_cast<size_t>(s.w) * 4);
        }
        s.tex->UnlockRect(0);
        s.uploaded = true;
    }
    return true;
}

void draw(IDirect3DDevice9* dev, Surface& s, float x, float y) {
    struct V {
        float x, y, z, rhw, u, v;
    };
    const float x0 = x - 0.5f, y0 = y - 0.5f, x1 = x + s.w - 0.5f, y1 = y + s.h - 0.5f;
    const V v[4] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->SetTexture(0, s.tex);
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
    if (panel && (InterlockedExchange(&g_dirty, 0) != 0 || g_built_h != sh)) build_panel(sh);
    if (hint) build_hint(sh);
    g_built_h = sh;
    if ((panel && !upload(dev, g_panel)) || (hint && !upload(dev, g_hint))) {
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
            draw(dev, g_panel, static_cast<float>((static_cast<int>(bd.Width) - g_panel.w) / 2),
                 static_cast<float>((sh - g_panel.h) / 2));
        }
        if (hint) {
            draw(dev, g_hint, static_cast<float>((static_cast<int>(bd.Width) - g_hint.w) / 2),
                 static_cast<float>(sh - g_hint.h - sh / 54));
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

constexpr WORD kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
               kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kY = 0x8000;

// One logical control: pressed now, and the time it went down (for repeats).
struct Held {
    bool down = false;
    ULONGLONG since = 0, next = 0;
};
Held g_held[16];

// True on the press and then on repeats (after 0.38 s, every 0.11 s) for the arrows.
bool pressed(int i, bool down, bool repeats, ULONGLONG now) {
    Held& h = g_held[i];
    if (!down) {
        h.down = false;
        return false;
    }
    if (!h.down) {
        h.down = true;
        h.since = now;
        h.next = now + 380;
        return true;
    }
    if (repeats && now >= h.next) {
        h.next = now + 110;
        return true;
    }
    return false;
}

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void open_page(bool open) {
    InterlockedExchange(&g_open, open ? 1 : 0);
    InterlockedExchange(&g_swallow, 1);
    inputcache_set_blank(true);
    g_editing = false;
    InterlockedExchange(&g_dirty, 1);
    LOG_INFO("FPS Unlock Settings: %s", open ? "opened" : "closed");
}

void update_input() {
    const ULONGLONG now = GetTickCount64();
    WORD pad = 0;
    int lx = 0, ly = 0;
    for (int i = 0; i < 4; ++i) {
        pad |= static_cast<WORD>(g_pad_buttons[i]);
        const LONG st = g_pad_stick[i];
        const int x = static_cast<int16_t>(st & 0xFFFF), y = static_cast<int16_t>((st >> 16) & 0xFFFF);
        if (std::abs(x) > std::abs(lx)) lx = x;
        if (std::abs(y) > std::abs(ly)) ly = y;
    }
    const bool focus = game_window_focused();
    const bool open = g_open != 0;
    const bool kb = focus;
    // Opening and closing.
    const bool open_key = kb && g_key != 0 && key(g_key);
    const bool pc_screen = now - static_cast<ULONGLONG>(g_pcoption_seen) < 300;
    if (pressed(0, open_key, false, now)) {
        open_page(!open);
        return;
    }
    if (!open) {
        if (pressed(1, (pad & kY) != 0, false, now) && pc_screen) open_page(true);
        if (g_swallow && pad == 0 && !(kb && (key(VK_ESCAPE) || key(VK_BACK) || key(VK_RETURN)))) {
            InterlockedExchange(&g_swallow, 0);
            inputcache_set_blank(false);
        }
        return;
    }
    pressed(1, (pad & kY) != 0, false, now);
    if (pressed(2, (pad & (kB | kBack | kStart)) != 0 || (kb && key(VK_ESCAPE)), false, now) ||
        (!g_editing && pressed(3, kb && key(VK_BACK), false, now))) {
        if (g_editing) {
            g_editing = false;
            InterlockedExchange(&g_dirty, 1);
        } else {
            open_page(false);
        }
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_pages.empty()) return;
    bool moved = false;
    if (g_editing) {
        // Typing: only the text, Backspace and Enter (keep it; Escape or B, above, drop it). Nothing else moves, so a
        // letter that is also a key of the page (Q, E) is only a letter.
        if (g_edit_page < 0 || g_edit_page >= static_cast<int>(g_pages.size()) || g_edit_row < 0 ||
            g_edit_row >= static_cast<int>(g_pages[g_edit_page].rows.size())) {
            g_editing = false;
            return;
        }
        Row& r = g_pages[g_edit_page].rows[g_edit_row];
        if (pressed(10, (pad & kA) || (kb && key(VK_RETURN)), false, now)) {
            g_editing = false;
            r.text = g_edit;
            write_ini(r.def.key, r.text);
            if (r.def.apply) r.def.apply(r.text.c_str());
            InterlockedExchange(&g_dirty, 1);
            return;
        }
        if (pressed(3, kb && key(VK_BACK), true, now) && !g_edit.empty()) {
            g_edit.pop_back();
            moved = true;
        }
        const bool shift = key(VK_SHIFT);
        for (int vk = 0x30; vk <= 0x5A; ++vk) {
            if (vk > 0x39 && vk < 0x41) continue;
            const bool d = kb && key(vk);
            if (d && !g_was_key[vk] && g_edit.size() < 63) {
                char c = static_cast<char>(vk);
                if (vk >= 0x41 && !shift) c = static_cast<char>(c + 32);
                g_edit.push_back(c);
                moved = true;
            }
            g_was_key[vk] = d;
        }
        const bool sp = kb && key(VK_SPACE), mi = kb && key(VK_OEM_MINUS);
        if (sp && !g_was_space && g_edit.size() < 63) {
            g_edit.push_back(' ');
            moved = true;
        }
        if (mi && !g_was_minus && g_edit.size() < 63) {
            g_edit.push_back(shift ? '_' : '-');
            moved = true;
        }
        g_was_space = sp;
        g_was_minus = mi;
        if (moved) InterlockedExchange(&g_dirty, 1);
        return;
    }
    const bool up = (pad & kUp) || ly > 16000 || (kb && key(VK_UP));
    const bool down = (pad & kDown) || ly < -16000 || (kb && key(VK_DOWN));
    const bool left = (pad & kLeft) || lx < -16000 || (kb && key(VK_LEFT));
    const bool right = (pad & kRight) || lx > 16000 || (kb && key(VK_RIGHT));
    const bool prev_page = (pad & kLB) || (kb && (key('Q') || key(VK_PRIOR)));
    const bool next_page = (pad & kRB) || (kb && (key('E') || key(VK_NEXT)));
    const bool confirm = (pad & kA) || (kb && key(VK_RETURN));
    if (pressed(4, prev_page, false, now)) {
        g_page = (g_page + static_cast<int>(g_pages.size()) - 1) % static_cast<int>(g_pages.size());
        g_row = 0;
        g_scroll = 0;
        moved = true;
    }
    if (pressed(5, next_page, false, now)) {
        g_page = (g_page + 1) % static_cast<int>(g_pages.size());
        g_row = 0;
        g_scroll = 0;
        moved = true;
    }
    Page& page = g_pages[g_page];
    const int rows = static_cast<int>(page.rows.size());
    if (rows > 0) {
        if (pressed(6, up, true, now)) {
            g_row = (g_row + rows - 1) % rows;
            moved = true;
        }
        if (pressed(7, down, true, now)) {
            g_row = (g_row + 1) % rows;
            moved = true;
        }
        Row& r = page.rows[g_row];
        if (pressed(8, left, true, now)) change(r, -1);
        if (pressed(9, right, true, now)) change(r, +1);
        if (pressed(10, confirm, false, now)) {
            if (r.def.text) {
                g_editing = true;
                g_edit = r.text;
                g_edit_page = g_page;
                g_edit_row = g_row;
                // Keys already down (the one that started it) do not type.
                for (int vk = 0x30; vk <= 0x5A; ++vk) g_was_key[vk] = kb && key(vk);
                g_was_space = kb && key(VK_SPACE);
                g_was_minus = kb && key(VK_OEM_MINUS);
            } else {
                change(r, +1);
            }
            moved = true;
        }
    }
    if (moved) InterlockedExchange(&g_dirty, 1);
}

}  // namespace

void settingsmenu_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count) {
    if (!g_enabled) return;
    add_page(title, rows, count);
}

void settingsmenu_install(const wchar_t* ini_path, int key) {
    g_ini = ini_path;
    g_key = key;
    g_enabled = true;
    add_page(L"Display", kDisplay, _countof(kDisplay));
    add_page(L"Picture", kPicture, _countof(kPicture));
    add_page(L"Draw distance", kDistance, _countof(kDistance));
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

bool settingsmenu_enabled() { return g_enabled; }
void settingsmenu_set_open(bool open) {
    if (g_enabled && (g_open != 0) != open) open_page(open);
}
bool settingsmenu_is_open() { return g_open != 0; }

void settingsmenu_present(IDirect3DDevice9* device) {
    if (!g_enabled || !device) return;
    update_input();
    const bool open = g_open != 0;
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
    if (!g_enabled || index > 3) return;
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
    if (g_open || g_swallow) std::memset(b + 4, 0, 12);
}
