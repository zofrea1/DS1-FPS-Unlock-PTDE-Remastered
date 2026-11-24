#include "settingspage.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// FPS Unlock Settings: a page drawn over the game with the settings a player would reasonably change while playing
// (each game lists its own; a bundled mod adds its own page). The fixes, speed-ups and diagnostics stay in the INI
// only. Opened with a key (SettingsMenuKey, F1 by default) or, on a controller, with Y while the game's PC settings
// screen is up; while it is open the game gets no controller, keyboard or mouse input. Every change is written to the
// INI at once (only the value on that key's line changes); a row the mod can apply while the game runs takes effect
// at once, the others say they apply after a restart.
//
// This part is the same in both games: the page is text drawn with GDI into a 32-bit picture that the game's own
// code then draws over its finished frame (src/ptde/settingsmenu.cpp with Direct3D 9, src/dsr/settingsmenu.cpp with
// Direct3D 11).
namespace {

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
void (*g_blank)(bool) = nullptr;

volatile LONG g_open = 0;
volatile LONG g_swallow = 0;  // after closing: the game gets no input until every button is let go
int g_page = 0, g_row = 0, g_scroll = 0;
bool g_editing = false;
std::string g_edit;
int g_edit_page = -1, g_edit_row = -1;  // the row being typed into
bool g_was_key[0x5B] = {};
bool g_was_space = false, g_was_minus = false;

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
// Changes not yet in the INI, in the order made. Another program (an antivirus scan of the file just written, an
// editor) can hold the INI for a moment so that it cannot be replaced (seen: Win32 5 while a row was changed several
// times a second); what could not be written is kept and tried again every frame until it is (flush_ini).
std::vector<std::pair<std::string, std::string>> g_pending;
ULONGLONG g_retry_at = 0;
DWORD g_failing = 0;        // the Win32 error of the current run of failures (0: none), logged once per run
ULONGLONG g_fail_since = 0;  // when that run began; still failing 5 s later is an error
bool g_fail_reported = false;

bool flush_ini() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_pending.empty()) return true;
    std::string text;
    DWORD err = 0;
    const char* step = "read";
    if (read_ini(&text)) {
        for (const auto& kv : g_pending) {
            size_t v = 0, e = 0;
            if (find_key(text, kv.first.c_str(), &v, &e)) {
                text.replace(v, e - v, kv.second);
            } else {
                const bool crlf = text.find("\r\n") != std::string::npos;
                if (!text.empty() && text.back() != '\n') text += crlf ? "\r\n" : "\n";
                text += kv.first + " = " + kv.second + (crlf ? "\r\n" : "\n");
            }
        }
        const std::wstring tmp = g_ini + L".settingsmenu.tmp";
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        step = "write";
        if (f == INVALID_HANDLE_VALUE) {
            err = GetLastError();
        } else {
            DWORD put = 0;
            const BOOL ok = WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
            if (!ok || put != text.size()) err = ok ? ERROR_WRITE_FAULT : GetLastError();
            CloseHandle(f);
            if (!err) {
                step = "replace";
                if (!MoveFileExW(tmp.c_str(), g_ini.c_str(), MOVEFILE_REPLACE_EXISTING)) err = GetLastError();
            }
            if (err) DeleteFileW(tmp.c_str());
        }
    } else {
        err = ERROR_READ_FAULT;
    }
    if (err) {
        const ULONGLONG now = GetTickCount64();
        if (!g_failing) {
            g_fail_since = now;
            g_fail_reported = false;
        }
        if (g_failing != err) {
            LOG_INFO("FPS Unlock Settings: could not %s the INI (Win32=%lu; another program may hold it for a "
                     "moment); %zu change(s) kept and tried again",
                     step, err, g_pending.size());
        }
        if (!g_fail_reported && now - g_fail_since > 5000) {
            LOG_ERROR("FPS Unlock Settings: the INI could not be written for 5 s (Win32=%lu); %zu change(s) are not "
                      "saved yet (still trying)",
                      err, g_pending.size());
            g_fail_reported = true;
        }
        g_failing = err;
        g_retry_at = now + 100;
        return false;
    }
    for (const auto& kv : g_pending) {
        LOG_INFO("FPS Unlock Settings: %s = %s written to the INI%s", kv.first.c_str(), kv.second.c_str(),
                 g_failing ? " (on a later try)" : "");
    }
    g_pending.clear();
    g_failing = 0;
    return true;
}

bool write_ini(const char* key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    bool found = false;
    for (auto& kv : g_pending) {
        if (kv.first == key) {
            kv.second = value;
            found = true;
        }
    }
    if (!found) g_pending.emplace_back(key, value);
    return flush_ini();
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

// ---------------------------------------------------------------- drawing
struct Surface {
    int w = 0, h = 0;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    uint32_t* bits = nullptr;
    SettingsPageImage image;
};
Surface g_panel, g_hint;
int g_built_w = 0, g_built_h = 0;
std::wstring g_hint_line;

bool make_surface(Surface& s, int w, int h) {
    if (s.w == w && s.h == h && s.dc) return true;
    if (s.dc) {
        SelectObject(s.dc, s.old);
        DeleteObject(s.bmp);
        DeleteDC(s.dc);
        s.dc = nullptr;
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
    return true;
}

// The picture handed out: the surface's pixels, a new version.
void publish(Surface& s) {
    GdiFlush();
    s.image.w = s.w;
    s.image.h = s.h;
    s.image.bits = s.bits;
    ++s.image.version;
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
    GdiFlush();
    const size_t n = static_cast<size_t>(s.w) * s.h;
    for (size_t i = 0; i < n; ++i) s.bits[i] = (s.bits[i] & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

constexpr COLORREF kColBack = RGB(14, 12, 10), kEdge = RGB(120, 96, 58), kGold = RGB(214, 186, 128),
                   kWhite = RGB(232, 228, 218), kGrey = RGB(150, 144, 132), kSel = RGB(96, 34, 14),
                   kLive = RGB(150, 196, 140), kLater = RGB(222, 164, 92);

// The page's size on a screen W x H: drawn for 1080 lines, scaled with the screen, and never wider than it (a
// portrait screen).
float scale(int screen_w, int screen_h) {
    const float by_h = static_cast<float>(screen_h) / 1080.0f, by_w = static_cast<float>(screen_w) / 1240.0f;
    return by_w < by_h ? by_w : by_h;
}

void build_panel(int screen_w, int screen_h) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const float k = scale(screen_w, screen_h);
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
        // More rows above or below the ones shown.
        if (g_scroll > 0) text(s, smallf, kGrey, W - K(60), top - K(20), K(30), K(20), L"\x25B2", DT_CENTER);
        if (g_scroll + visible < static_cast<int>(p.rows.size())) {
            text(s, smallf, kGrey, W - K(60), help_top - K(22), K(30), K(20), L"\x25BC", DT_CENTER);
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
    publish(s);
}

void build_hint(int screen_w, int screen_h, const wchar_t* line) {
    const float k = scale(screen_w, screen_h);
    auto K = [k](int v) { return static_cast<int>(v * k + 0.5f); };
    const int W = K(520), H = K(44);
    if (g_hint.w == W && g_hint.h == H && g_hint.dc && g_hint_line == line) return;
    if (!make_surface(g_hint, W, H)) return;
    g_hint_line = line;
    fill(g_hint, 0, 0, W, H, kColBack);
    HFONT f = font(K(24), false);
    text(g_hint, f, kGold, 0, 0, W, H, line, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DeleteObject(f);
    set_alpha(g_hint, 200);
    publish(g_hint);
}

// ---------------------------------------------------------------- input
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
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

void open_page(bool open) {
    InterlockedExchange(&g_open, open ? 1 : 0);
    InterlockedExchange(&g_swallow, 1);
    if (g_blank) g_blank(true);
    g_editing = false;
    InterlockedExchange(&g_dirty, 1);
    LOG_INFO("FPS Unlock Settings: %s", open ? "opened" : "closed");
}

void update_input(const SettingsPageInput& in) {
    const ULONGLONG now = GetTickCount64();
    const uint16_t pad = in.pad;
    const int lx = in.lx, ly = in.ly;
    const bool kb = in.keyboard && in.key;
    auto key = [&in](int vk) { return in.key(vk); };
    const bool open = g_open != 0;
    // Opening and closing.
    const bool open_key = kb && g_key != 0 && key(g_key);
    if (pressed(0, open_key, false, now)) {
        open_page(!open);
        return;
    }
    if (!open) {
        if (pressed(1, (pad & kY) != 0, false, now) && in.pad_opens) open_page(true);
        if (g_swallow && pad == 0 && !(kb && (key(VK_ESCAPE) || key(VK_BACK) || key(VK_RETURN)))) {
            InterlockedExchange(&g_swallow, 0);
            if (g_blank) g_blank(false);
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
        const bool shift = kb && key(VK_SHIFT);
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

void settingspage_init(const wchar_t* ini_path, int key, void (*blank)(bool on)) {
    g_ini = ini_path;
    g_key = key;
    g_blank = blank;
    g_enabled = true;
}

void settingspage_add_page(const wchar_t* title, const SettingsMenuRow* rows, int count) {
    if (!g_enabled) return;
    add_page(title, rows, count);
}

bool settingspage_enabled() { return g_enabled; }
int settingspage_key() { return g_key; }
bool settingspage_is_open() { return g_open != 0; }
void settingspage_set_open(bool open) {
    if (g_enabled && (g_open != 0) != open) open_page(open);
}

void settingspage_frame(const SettingsPageInput& in) {
    if (!g_enabled) return;
    if (g_failing && GetTickCount64() >= g_retry_at) flush_ini();
    update_input(in);
}

const SettingsPageImage* settingspage_panel(int screen_w, int screen_h) {
    if (!g_enabled) return nullptr;
    if (InterlockedExchange(&g_dirty, 0) != 0 || g_built_w != screen_w || g_built_h != screen_h || !g_panel.dc) {
        build_panel(screen_w, screen_h);
        g_built_w = screen_w;
        g_built_h = screen_h;
    }
    return g_panel.dc ? &g_panel.image : nullptr;
}

const SettingsPageImage* settingspage_hint(int screen_w, int screen_h, const wchar_t* line) {
    if (!g_enabled) return nullptr;
    build_hint(screen_w, screen_h, line);
    return g_hint.dc ? &g_hint.image : nullptr;
}
