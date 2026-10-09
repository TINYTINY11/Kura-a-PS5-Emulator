// Kura first-startup experience (M3 stage 1.5).
//
// This is the *emulator's* power-on UI — the frame real firmware will one
// day boot inside of. It is not emulated output and never pretends to be:
// the pages show Kura's own setup wizard and the honest firmware-pipeline
// status (including the encryption wall). Win32 + GDI only, no deps.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common/log.hpp"
#include "common/settings.hpp"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

// ----------------------------------------------------------------- ids ------
constexpr wchar_t kClass[] = L"KuraBootWindow";
constexpr int kWinW = 920;
constexpr int kWinH = 600;

enum ControlId {
    IDC_PATH = 1001,
    IDC_BROWSE = 1002,
    IDC_BOOT = 1003,
    IDC_WIZARD = 1004,
    IDC_LOGLEVEL = 1005,
    IDC_OUT = 1006,
    IDC_VERDICT = 1007,
};

constexpr UINT kWmLine = WM_APP + 1;
constexpr UINT kWmDone = WM_APP + 2;

enum class Page { Splash, Wizard, Boot };

// ----------------------------------------------------------------- state ----
HWND g_hwnd = nullptr;
HWND g_wizTitle = nullptr, g_wizHint = nullptr, g_pathLabel = nullptr,
     g_path = nullptr, g_browse = nullptr, g_boot = nullptr,
     g_logLabel = nullptr, g_log = nullptr;
HWND g_bootTitle = nullptr, g_out = nullptr, g_verdict = nullptr,
     g_again = nullptr;

Page g_page = Page::Splash;
ULONGLONG g_splash_t0 = 0;
HFONT g_font_logo = nullptr, g_font_title = nullptr, g_font_body = nullptr,
      g_font_small = nullptr;
kura::settings::Settings g_settings;
std::atomic<bool> g_busy{false};
std::string g_verdict_text = "Waiting…";

// ------------------------------------------------------------- helpers ------
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

char* dupe(const char* s) {
    const std::size_t n = std::strlen(s) + 1;
    char* p = static_cast<char*>(std::malloc(n));
    if (p) std::memcpy(p, s, n);
    return p;
}

// kura_pup colorizes its console output; strip ANSI escapes for the edit box.
std::string strip_ansi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1B') {
            while (i < in.size() && in[i] != 'm') ++i;
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const auto slash = p.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? p : p.substr(0, slash);
}

void set_text(HWND h, const wchar_t* t) { SetWindowTextW(h, t); }

// ------------------------------------------------------------ layout --------
void layout() {
    const int pad = 32;
    auto show = [&](HWND h, bool vis) { ShowWindow(h, vis ? SW_SHOW : SW_HIDE); };

    const bool wiz = g_page == Page::Wizard;
    show(g_wizTitle, wiz);
    show(g_wizHint, wiz);
    show(g_pathLabel, wiz);
    show(g_path, wiz);
    show(g_browse, wiz);
    show(g_logLabel, wiz);
    show(g_log, wiz);
    show(g_boot, wiz);

    const bool boot = g_page == Page::Boot;
    show(g_bootTitle, boot);
    show(g_out, boot);
    show(g_verdict, boot);
    show(g_again, boot);

    if (wiz) {
        int y = 150;
        SetWindowPos(g_wizTitle, nullptr, pad, y, 600, 44, SWP_NOZORDER);
        y += 56;
        SetWindowPos(g_wizHint, nullptr, pad, y, kWinW - 2 * pad, 64, SWP_NOZORDER);
        y += 76;
        SetWindowPos(g_pathLabel, nullptr, pad, y, 400, 20, SWP_NOZORDER);
        y += 26;
        SetWindowPos(g_path, nullptr, pad, y, kWinW - 2 * pad - 110, 28, SWP_NOZORDER);
        SetWindowPos(g_browse, nullptr, kWinW - pad - 100, y, 100, 28, SWP_NOZORDER);
        y += 44;
        SetWindowPos(g_logLabel, nullptr, pad, y, 200, 20, SWP_NOZORDER);
        y += 26;
        SetWindowPos(g_log, nullptr, pad, y, 240, 24, SWP_NOZORDER);
        y += 44;
        SetWindowPos(g_boot, nullptr, pad, y, 180, 34, SWP_NOZORDER);
    }
    if (boot) {
        int y = 40;
        SetWindowPos(g_bootTitle, nullptr, pad, y, 600, 40, SWP_NOZORDER);
        y += 52;
        SetWindowPos(g_out, nullptr, pad, y, kWinW - 2 * pad, kWinH - y - 130,
                     SWP_NOZORDER);
        y = kWinH - 106;
        SetWindowPos(g_verdict, nullptr, pad, y, kWinW - 2 * pad, 28, SWP_NOZORDER);
        y += 34;
        SetWindowPos(g_again, nullptr, pad, y, 180, 30, SWP_NOZORDER);
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

void append_line(const std::string& utf8) {
    if (g_out == nullptr) return;
    std::wstring w = widen(strip_ansi(utf8)) + L"\r\n";
    const int len = GetWindowTextLengthW(g_out);
    SendMessageW(g_out, EM_SETSEL, static_cast<WPARAM>(len), len);
    SendMessageW(g_out, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
}

// -------------------------------------------------------------- worker ------
void start_worker() {
    if (g_busy.exchange(true)) return;
    SendMessageW(g_out, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L""));
    g_verdict_text = "Inspecting firmware…";
    set_text(g_verdict, L"Inspecting firmware…");

    const std::wstring tool = L"\"" + exe_dir() + L"\\kura_pup.exe\"";
    std::wstring path = L"";
    {
        const int n = GetWindowTextLengthW(g_path);
        std::wstring buf(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(g_path, buf.data(), n + 1);
        buf.resize(static_cast<std::size_t>(n));
        path = buf;
    }
    // cmd.exe strips the outer quotes when /c's argument starts with one —
    // wrap the whole command, same trick as the CLI.
    const std::wstring inner = tool + L" \"" + path + L"\"";
    const std::wstring cmd = L"\"" + inner + L"\"";

    std::thread([cmd] {
        FILE* f = _wpopen(cmd.c_str(), L"r");
        if (f == nullptr) {
            PostMessageW(g_hwnd, kWmLine, 0,
                         reinterpret_cast<LPARAM>(dupe("failed to launch kura_pup.exe")));
            PostMessageW(g_hwnd, kWmDone, static_cast<WPARAM>(-1), 0);
            return;
        }
        char buf[512];
        while (std::fgets(buf, sizeof(buf), f) != nullptr) {
            PostMessageW(g_hwnd, kWmLine, 0, reinterpret_cast<LPARAM>(dupe(buf)));
        }
        const int rc = _pclose(f);
        PostMessageW(g_hwnd, kWmDone, static_cast<WPARAM>(rc), 0);
    }).detach();
}

void goto_wizard() {
    g_page = Page::Wizard;
    layout();
}

// ------------------------------------------------------------- painting -----
void paint(HDC dc) {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(0x16, 0x16, 0x1A));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    if (g_page == Page::Splash) {
        const double t = static_cast<double>(GetTickCount64() - g_splash_t0) / 1400.0;
        const double a = t < 1.0 ? t : 1.0;
        const double a2 = t < 0.5 ? 0.0 : (t - 0.5) * 2.0; // subtitle lags
        const auto lerp = [](double a) {
            const int v = static_cast<int>(0x30 + a * (0xF6 - 0x30));
            return RGB(v, v, v);
        };
        HFONT old = static_cast<HFONT>(SelectObject(dc, g_font_logo));
        SetTextColor(dc, lerp(a));
        const wchar_t* logo = L"KURA";
        SIZE sz{};
        GetTextExtentPoint32W(dc, logo, 4, &sz);
        TextOutW(dc, (kWinW - sz.cx) / 2, kWinH / 2 - 70, logo, 4);

        SelectObject(dc, g_font_small);
        SetTextColor(dc, lerp(a2));
        const wchar_t* sub = L"PS5 EMULATOR \x2014 FIRST STARTUP";
        const int sub_len = static_cast<int>(wcslen(sub));
        GetTextExtentPoint32W(dc, sub, sub_len, &sz);
        TextOutW(dc, (kWinW - sz.cx) / 2, kWinH / 2 + 6, sub, sub_len);

        // slim progress line
        HPEN pen = CreatePen(PS_SOLID, 2, lerp(a));
        HPEN oldpen = static_cast<HPEN>(SelectObject(dc, pen));
        MoveToEx(dc, kWinW / 2 - 120, kWinH / 2 + 48, nullptr);
        LineTo(dc, kWinW / 2 - 120 + static_cast<int>(240 * a), kWinH / 2 + 48);
        SelectObject(dc, oldpen);
        DeleteObject(pen);

        SelectObject(dc, old);
    }
}

void try_window_effects(HWND w) {
    // Win11 fluent: translucent acrylic backdrop + rounded corners + dark
    // title. All calls fail gracefully on older systems (we paint our own
    // opaque background anyway).
    const int backdrop = 2; // DWMSBT_TRANSIENTWINDOW
    DwmSetWindowAttribute(w, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop,
                          sizeof(backdrop));
    const int corner = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(w, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner,
                          sizeof(corner));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(w, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                          sizeof(dark));
}

void make_fonts() {
    g_font_logo = CreateFontW(-56, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_font_title = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
    g_font_body = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, L"Segoe UI");
    g_font_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
}

HWND make_child(const wchar_t* cls, const wchar_t* text, DWORD ex, DWORD style,
                int id) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10,
                             g_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_body), TRUE);
    return h;
}

void create_controls() {
    g_wizTitle = make_child(L"Static", L"Welcome to Kura", 0, 0, 0);
    SendMessageW(g_wizTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_title), TRUE);
    g_wizHint = make_child(
        L"Static",
        L"Kura needs a PS5 system software file (PS5UPDATE.PUP) to inspect. "
        L"Nothing is uploaded, nothing contacts PSN \x2014 everything stays on this PC.",
        0, 0, 0);
    g_pathLabel = make_child(L"Static", L"PS5 firmware file (PS5UPDATE.PUP):", 0, 0, 0);
    g_path = make_child(L"Edit", g_settings.firmware_path.c_str()[0] != '\0'
                                    ? widen(g_settings.firmware_path).c_str()
                                    : L"",
                        WS_EX_CLIENTEDGE, ES_AUTOHSCROLL, IDC_PATH);
    g_browse = make_child(L"Button", L"Browse…", 0, BS_PUSHBUTTON, IDC_BROWSE);
    g_logLabel = make_child(L"Static", L"Log level:", 0, 0, 0);
    g_log = make_child(L"ComboBox", L"", WS_EX_CLIENTEDGE,
                       CBS_DROPDOWNLIST | WS_VSCROLL, IDC_LOGLEVEL);
    for (const wchar_t* lvl : {L"trace", L"debug", L"info", L"warn", L"error", L"off"})
        SendMessageW(g_log, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lvl));
    static const wchar_t* kLevels[] = {L"trace", L"debug", L"info",
                                       L"warn", L"error", L"off"};
    int sel = 2;
    for (int i = 0; i < 6; ++i)
        if (g_settings.log_level == narrow(kLevels[i])) sel = i;
    SendMessageW(g_log, CB_SETCURSEL, static_cast<WPARAM>(sel), 0);
    g_boot = make_child(L"Button", L"Save settings && boot", 0, BS_DEFPUSHBUTTON,
                        IDC_BOOT);

    g_bootTitle = make_child(L"Static", L"First boot", 0, 0, 0);
    SendMessageW(g_bootTitle, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_title), TRUE);
    g_out = make_child(L"Edit", L"", WS_EX_CLIENTEDGE,
                       ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                       IDC_OUT);
    g_verdict = make_child(L"Static", L"Waiting…", 0, 0, IDC_VERDICT);
    g_again = make_child(L"Button", L"Run setup again", 0, BS_PUSHBUTTON, IDC_WIZARD);
}

// ---------------------------------------------------------------- procs -----
LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        // CreateWindowExW hasn't returned yet — g_hwnd is still null, but
        // the HWND parameter is already valid. Assign it FIRST or every
        // child control gets parented to null (they'd vanish off-screen).
        g_hwnd = w;
        make_fonts();
        create_controls();
        return 0;

    case WM_TIMER: {
        const double t = static_cast<double>(GetTickCount64() - g_splash_t0) / 1400.0;
        InvalidateRect(w, nullptr, FALSE);
        if (t >= 1.0) {
            KillTimer(w, 1);
            if (g_settings.first_run_done && !g_settings.firmware_path.empty()) {
                g_page = Page::Boot;
                layout();
                start_worker();
            } else {
                goto_wizard();
            }
        }
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_BROWSE) {
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = w;
            ofn.lpstrFilter = L"PS5 update files (*.PUP)\0*.PUP\0All files\0*.*\0";
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Select PS5UPDATE.PUP";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_path, file);
            return 0;
        }
        if (id == IDC_BOOT) {
            wchar_t buf[MAX_PATH * 2] = L"";
            GetWindowTextW(g_path, buf, MAX_PATH * 2);
            if (buf[0] == L'\0' || GetFileAttributesW(buf) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(w, L"Pick an existing PS5UPDATE.PUP file first.",
                            L"Kura", MB_OK | MB_ICONWARNING);
                return 0;
            }
            const int sel = static_cast<int>(SendMessageW(g_log, CB_GETCURSEL, 0, 0));
            static const char* kLevels[] = {"trace", "debug", "info",
                                            "warn", "error", "off"};
            g_settings.log_level = kLevels[(sel >= 0 && sel < 6) ? sel : 2];
            g_settings.firmware_path = narrow(buf);
            g_settings.first_run_done = true;
            kura::settings::save(kura::settings::default_path(), g_settings);
            kura::log::set_min_level(
                [&] {
                    kura::log::Level l = kura::log::Level::Info;
                    kura::log::parse_level(g_settings.log_level, l);
                    return l;
                }());
            g_page = Page::Boot;
            layout();
            start_worker();
            return 0;
        }
        if (id == IDC_WIZARD) {
            if (!g_busy.load()) goto_wizard();
            return 0;
        }
        return 0;
    }

    case kWmLine:
        append_line(reinterpret_cast<const char*>(lp));
        std::free(reinterpret_cast<void*>(lp));
        return 0;

    case kWmDone: {
        g_busy.store(false);
        const int rc = static_cast<int>(wp);
        if (rc == 0) {
            g_verdict_text = "Firmware structure OK - extraction stage can proceed.";
            set_text(g_verdict, L"Firmware structure OK \x2014 extraction stage can proceed.");
        } else if (rc == 3) {
            g_verdict_text =
                "Halted at the encryption wall: keys required (docs/RE-pup.md).";
            set_text(g_verdict,
                     L"Halted at the encryption wall: keys required (docs/RE-pup.md). "
                     L"Real PS5 boot resumes at M3\x2013M6.");
        } else {
            g_verdict_text = "Firmware inspection failed (see log above).";
            set_text(g_verdict, L"Firmware inspection failed (see log above).");
        }
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        // Labels sit on our dark background — control defaults would be
        // black-on-black.
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, RGB(0xD8, 0xD8, 0xDC));
        SetBkColor(dc, RGB(0x16, 0x16, 0x1A));
        static HBRUSH bg = CreateSolidBrush(RGB(0x16, 0x16, 0x1A));
        return reinterpret_cast<LRESULT>(bg);
    }

    case WM_CTLCOLOREDIT: {
        // Only theme the boot-log pane; the wizard's path edit stays the
        // familiar white text field.
        if (reinterpret_cast<HWND>(lp) == g_out) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, RGB(0xC8, 0xE0, 0xC8));
            SetBkColor(dc, RGB(0x10, 0x10, 0x14));
            static HBRUSH pane = CreateSolidBrush(RGB(0x10, 0x10, 0x14));
            return reinterpret_cast<LRESULT>(pane);
        }
        break; // default handling: white edit, black text
    }

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(w, msg, wp, lp);
        if (hit == HTCLIENT) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            HWND child = ChildWindowFromPoint(w, pt);
            if (child == w || child == nullptr) return HTCAPTION; // drag on bg
        }
        return hit;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(w, &ps);
        paint(dc);
        EndPaint(w, &ps);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // Console logging still works — route it nowhere visible; the GUI shows
    // pipeline output in its own pane. File sink remains available later.
    kura::settings::load(kura::settings::default_path(), g_settings);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, kClass, L"Kura \x2014 PS5 emulator",
                             WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, kWinW, kWinH,
                             nullptr, nullptr, inst, nullptr);
    try_window_effects(g_hwnd);
    layout();
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    g_splash_t0 = GetTickCount64();
    SetTimer(g_hwnd, 1, 20, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
