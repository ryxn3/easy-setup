// Easy Installer - native C++ / Win32 + Direct2D
// Clean dark UI, GPU-drawn, smooth animations. No external dependencies.
// Build with MSVC: run Build.bat

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <set>
#include <algorithm>
#include "icons_data.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <cstdio>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
static inline D2D1_COLOR_F RGBH(int hex, float a = 1.0f) {
    return D2D1::ColorF(((hex >> 16) & 0xFF) / 255.0f,
                        ((hex >> 8) & 0xFF) / 255.0f,
                        (hex & 0xFF) / 255.0f, a);
}
// Windows 11 (Fluent) dark theme
static const int COL_BG        = 0x202020;  // Mica dark base
static const int COL_SIDEBAR   = 0x1C1C1C;  // navigation pane
static const int COL_ROW_HOVER = 0x2D2D2D;  // subtle layer on hover
static const int COL_ROW_SEL   = 0x333333;  // selected layer
static const int COL_CARD      = 0x2B2B2B;  // CardBackgroundFillColorDefault
static const int COL_CARD_HI   = 0x323232;
static const int COL_TEXT      = 0xF3F3F3;  // TextFillColorPrimary
static const int COL_TEXT_DIM  = 0xA8A8A8;  // TextFillColorSecondary
static const int COL_TEXT_FAINT= 0x767676;  // TextFillColorTertiary
static const int COL_BORDER    = 0x333333;  // subtle card stroke
static const int COL_BORDER_HI = 0x454545;
static const int COL_ACCENT    = 0x60CDFF;  // SystemAccentColorLight2 (dark accent)
static const int COL_ACCENT_DK = 0x0078D4;  // primary-button blue
static const int COL_TRACK     = 0x454545;  // control-off track / disabled
static const int COL_GOOD      = 0x6CCB5F;
static const int COL_BAD       = 0xFF99A4;

// ---------------------------------------------------------------------------
enum Page { PAGE_HOME = 0, PAGE_INSTALLERS, PAGE_SETTINGS, PAGE_MISC, PAGE_COUNT };
enum InstStatus { ST_NONE = 0, ST_QUEUED, ST_INSTALLING, ST_DONE, ST_FAILED };

struct Installer {
    std::wstring name, id, category;
    bool  checked = false;
    bool  installed = false;
    int   status = ST_NONE;
    float hover = 0.0f, check = 0.0f, statusA = 0.0f;
};
struct Setting {
    std::wstring category, title, desc, subkey;
    HKEY root = HKEY_CURRENT_USER;
    std::vector<std::wstring> names;
    DWORD onValue = 1, offValue = 0;
    bool  adminReq = false, on = false;
    float hover = 0.0f, toggle = 0.0f;
};
enum MiscAction {
    MA_OPEN_STARTUP_USER, MA_OPEN_STARTUP_ALL, MA_DISABLE_STARTUP, MA_ENABLE_STARTUP,
    MA_EMPTY_RECYCLE, MA_FLUSH_DNS, MA_ENV_VARS, MA_COPY_SYSINFO, MA_RESTART_EXPLORER,
    MA_RUNCMD,      // run 'param' hidden (RunDetached)
    MA_SHELLEXEC,   // ShellExecute 'param' (open a URI / .msc / settings page)
    MA_CLEAR_TEMP   // wipe %TEMP%
};
struct MiscItem {
    std::wstring title, desc, confirm, param;
    MiscAction action;
    float hover = 0.0f, btn = 0.0f;
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
HWND g_hwnd = nullptr;
ComPtr<ID2D1Factory>          g_d2dFactory;
ComPtr<ID2D1HwndRenderTarget> g_rt;
ComPtr<IDWriteFactory>        g_dwrite;
ComPtr<IDWriteTextFormat>     g_fH1, g_fTitle, g_fBody, g_fSmall, g_fMono, g_fNav, g_fBig, g_fIcon, g_fTiny;
ComPtr<IDWriteTextFormat>     g_fIconNav, g_fIconBig, g_fIconSm; // Segoe MDL2 Assets

// Segoe MDL2 Assets glyph codepoints (native Windows icon font)
static const wchar_t ICON_HOME[]   = { 0xE80F, 0 }; // Home
static const wchar_t ICON_DL[]     = { 0xE896, 0 }; // Download
static const wchar_t ICON_SET[]    = { 0xE713, 0 }; // Settings
static const wchar_t ICON_TOOLS[]  = { 0xE90F, 0 }; // Repair / tools
static const wchar_t ICON_SEARCH[] = { 0xE721, 0 }; // Search
static const wchar_t ICON_CHEV[]   = { 0xE76C, 0 }; // ChevronRight
static const wchar_t ICON_CHEV_L[] = { 0xE76B, 0 }; // ChevronLeft
ComPtr<ID2D1SolidColorBrush>  g_brush;
ComPtr<IWICImagingFactory>    g_wic;
std::map<std::wstring, ComPtr<IWICFormatConverter>> g_iconSrc;  // device-independent (loaded once)
std::map<std::wstring, ComPtr<ID2D1Bitmap>>        g_iconBmp;  // device-dependent (per render target)

float g_scale = 1.0f;
Page  g_page = PAGE_HOME;
float g_pageAnim = 1.0f;
int   g_mouseX = -1, g_mouseY = -1;
float g_scroll = 0.0f, g_scrollTarget = 0.0f, g_scrollMax = 0.0f;
bool  g_isAdmin = false;
double g_now = 0.0;
int   g_cursor = 0; // 0 arrow, 1 hand, 2 ibeam

float g_navIndicator = 0.0f;
float g_navHover[PAGE_COUNT] = {0};

std::wstring g_search;
bool  g_searchFocus = false;
float g_searchHover = 0.0f;
int   g_settingsCat = -1;  // -1 = grid, >=0 = category index

std::mutex g_logMutex;
std::vector<std::wstring> g_log;
std::atomic<bool> g_installing{false};
std::atomic<bool> g_scanDone{false};
std::atomic<int>  g_instDone{0}, g_instTotal{0};
std::mutex g_statusMutex;
std::wstring g_status{L"Ready"};
void SetStatus(const std::wstring& s) { std::lock_guard<std::mutex> lk(g_statusMutex); g_status = s; }
std::wstring GetStatus() { std::lock_guard<std::mutex> lk(g_statusMutex); return g_status; }

std::vector<Installer> g_installers;
std::vector<Setting>   g_settings;
std::vector<MiscItem>  g_misc;

struct MonitorInfo {
    std::wstring deviceName, friendlyName;
    int width = 0, height = 0, refreshRate = 0, orientation = 0;
    bool isPrimary = false;
    int posX = 0, posY = 0;
    std::vector<std::pair<int,int>> availRes;  // unique resolutions sorted largest first
    std::vector<int> availHz;                  // Hz values for current selResIdx, sorted highest first
    int selResIdx = 0, selHzIdx = 0;
};
std::vector<MonitorInfo> g_monitors;
int   g_ddOpen = -1;   // -1=closed; monIdx*2+0=res, monIdx*2+1=hz
float g_ddPopX=0, g_ddPopW=0, g_ddBtnT=0, g_ddBtnB=0;

struct Hit { D2D1_RECT_F r; int kind; int index; };
enum { HIT_NAV, HIT_INSTALLER_ROW, HIT_INSTALL_BTN, HIT_SETTING_ROW, HIT_MISC_BTN, HIT_ADMIN_BTN,
       HIT_SELECTALL, HIT_CLEAR, HIT_HOME_CARD, HIT_SEARCH, HIT_CATEGORY,
       HIT_SETTINGS_CAT, HIT_SETTINGS_BACK,
       HIT_MONITOR_ROTATE, HIT_MONITOR_PRIMARY,
       HIT_MONITOR_RES_PREV, HIT_MONITOR_RES_NEXT,
       HIT_MONITOR_HZ_PREV, HIT_MONITOR_HZ_NEXT,
       HIT_MONITOR_APPLY,
       HIT_DD_TOGGLE, HIT_DD_ITEM };
std::vector<Hit> g_hits;

// ---------------------------------------------------------------------------
float g_dt = 1.0f / 60.0f;  // seconds elapsed since last frame (set each frame)
static inline float S(float x) { return x * g_scale; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
// Frame-rate independent easing. k is the classic per-frame fraction tuned for 60fps;
// we re-derive it from the real elapsed time so motion is smooth at any refresh rate.
static inline float ease(float c, float t, float k) {
    if (k >= 1.0f) return t;
    float rate = -60.0f * logf(1.0f - k);
    float a = 1.0f - expf(-rate * g_dt);
    return c + (t - c) * a;
}
static inline double NowSeconds() {
    static LARGE_INTEGER freq = { 0 };
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}
static bool inRect(const D2D1_RECT_F& r, int x, int y) {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}
static std::wstring lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

void AddLog(const std::wstring& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_log.push_back(s);
    if (g_log.size() > 500) g_log.erase(g_log.begin(), g_log.begin() + 120);
}

bool CheckIsAdmin() {
    BOOL admin = FALSE; PSID grp = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &grp)) {
        CheckTokenMembership(nullptr, grp, &admin); FreeSid(grp);
    }
    return admin == TRUE;
}
void RelaunchAsAdmin() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas"; sei.lpFile = path; sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) PostQuitMessage(0);
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
bool RegReadDword(HKEY root, const std::wstring& sub, const std::wstring& name, DWORD& out) {
    HKEY k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    DWORD type = 0, cb = sizeof(DWORD), val = 0;
    LONG r = RegQueryValueExW(k, name.c_str(), nullptr, &type, (LPBYTE)&val, &cb);
    if (r == ERROR_SUCCESS && type == REG_DWORD) { out = val; RegCloseKey(k); return true; }
    // Also accept legacy REG_SZ numeric values (e.g. Control Panel\Mouse)
    if (type == REG_SZ) {
        wchar_t buf[32] = {}; cb = sizeof(buf);
        r = RegQueryValueExW(k, name.c_str(), nullptr, &type, (LPBYTE)buf, &cb);
        RegCloseKey(k);
        if (r == ERROR_SUCCESS) { out = (DWORD)_wtoi(buf); return true; }
        return false;
    }
    RegCloseKey(k);
    return false;
}
bool RegWriteDword(HKEY root, const std::wstring& sub, const std::wstring& name, DWORD val) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_READ | KEY_SET_VALUE | KEY_WOW64_64KEY,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    // Write in the same format as the existing value (REG_SZ for legacy Control Panel keys)
    DWORD existType = REG_DWORD, existCb = 0;
    RegQueryValueExW(k, name.c_str(), nullptr, &existType, nullptr, &existCb);
    LONG r;
    if (existType == REG_SZ) {
        wchar_t buf[32]; swprintf_s(buf, L"%u", val);
        r = RegSetValueExW(k, name.c_str(), 0, REG_SZ, (const BYTE*)buf, (DWORD)((wcslen(buf) + 1) * sizeof(wchar_t)));
    } else {
        r = RegSetValueExW(k, name.c_str(), 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Process helpers
// ---------------------------------------------------------------------------
void RunDetached(const std::wstring& cmd) {
    std::wstring c = cmd;
    STARTUPINFOW si = { sizeof(si) }; PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
}

// Run a command, streaming output either to the log or into *out.
int RunCaptureImpl(const std::wstring& cmd, std::wstring* out) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    // Valid, inheritable NUL stdin (this is a GUI app: no real console stdin)
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = hNul;

    std::wstring c = cmd; PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        DWORD e = GetLastError();
        CloseHandle(rd); CloseHandle(wr); if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        if (!out) AddLog(L"  ! could not start process (error " + std::to_wstring(e) + L")");
        return -1;
    }
    CloseHandle(wr);

    std::string acc; char buf[4096]; DWORD n = 0;
    auto flush = [&](const std::string& line) {
        int wl = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
        std::wstring w(wl ? wl - 1 : 0, L' ');
        if (wl) MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, &w[0], wl);
        if (out) { *out += w; *out += L'\n'; }
        else if (!w.empty()) AddLog(L"  " + w);
    };
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
        acc.append(buf, n);
        size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, pos); acc.erase(0, pos + 1);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            // skip winget's progress-bar spinner spam
            if (line.find_first_not_of(" -\\|/\r\b") == std::string::npos) continue;
            flush(line);
        }
    }
    CloseHandle(rd);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return (int)code;
}
int RunCapture(const std::wstring& cmd) { return RunCaptureImpl(cmd, nullptr); }

void InstallThread(std::vector<int> idxs) {
    g_installing = true; g_instTotal = (int)idxs.size(); g_instDone = 0;
    for (int i : idxs) { g_installers[i].status = ST_QUEUED; }
    for (size_t k = 0; k < idxs.size(); ++k) {
        Installer& it = g_installers[idxs[k]];
        it.status = ST_INSTALLING;
        SetStatus(L"Installing " + it.name + L"  (" + std::to_wstring(k + 1) + L"/" +
                  std::to_wstring(idxs.size()) + L")");
        AddLog(L"");
        AddLog(L"== " + it.name + L" ==");
        std::wstring cmd = L"cmd.exe /c winget install --id " + it.id +
            L" -e --silent --accept-package-agreements --accept-source-agreements --disable-interactivity";
        int code = RunCapture(cmd);
        it.status = (code == 0) ? ST_DONE : ST_FAILED;
        if (code == 0) it.installed = true;
        g_instDone = (int)k + 1;
        AddLog(code == 0 ? L"  -> installed" : L"  -> failed (exit " + std::to_wstring(code) + L")");
    }
    int ok = 0; for (int i : idxs) if (g_installers[i].status == ST_DONE) ok++;
    SetStatus(L"Done – " + std::to_wstring(ok) + L"/" + std::to_wstring(idxs.size()) + L" installed");
    g_installing = false;
}

void ScanInstalledThread() {
    std::wstring out;
    RunCaptureImpl(L"cmd.exe /c winget list --accept-source-agreements", &out);
    std::wstring lo = lower(out);
    for (auto& it : g_installers)
        if (lo.find(lower(it.id)) != std::wstring::npos) it.installed = true;
    g_scanDone = true;
}

// ---------------------------------------------------------------------------
// Misc actions
// ---------------------------------------------------------------------------
int SetAllStartupApps(bool enable) {
    struct KL { HKEY root; const wchar_t* sub; };
    KL locs[] = {
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run" },
    };
    int changed = 0;
    for (auto& L : locs) {
        HKEY k;
        if (RegOpenKeyExW(L.root, L.sub, 0, KEY_READ | KEY_SET_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) continue;
        wchar_t name[512]; DWORD idx = 0; std::vector<std::wstring> vals;
        for (;;) { DWORD nl = 512; if (RegEnumValueW(k, idx++, name, &nl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; vals.push_back(name); }
        for (auto& vn : vals) {
            BYTE data[64]; DWORD cb = sizeof(data), type = 0;
            if (RegQueryValueExW(k, vn.c_str(), nullptr, &type, data, &cb) == ERROR_SUCCESS && cb >= 12) {
                data[0] = enable ? 2 : 3; for (int b = 4; b < 12; ++b) data[b] = 0;
                if (RegSetValueExW(k, vn.c_str(), 0, type, data, cb) == ERROR_SUCCESS) changed++;
            }
        }
        RegCloseKey(k);
    }
    return changed;
}
void CopySysInfo() {
    std::wstring product, cpu;
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t b[256]; DWORD cb = sizeof(b);
        if (RegQueryValueExW(k, L"ProductName", nullptr, nullptr, (LPBYTE)b, &cb) == ERROR_SUCCESS) product = b;
        RegCloseKey(k);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t b[256]; DWORD cb = sizeof(b);
        if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr, (LPBYTE)b, &cb) == ERROR_SUCCESS) cpu = b;
        RegCloseKey(k);
    }
    MEMORYSTATUSEX mem = { sizeof(mem) }; GlobalMemoryStatusEx(&mem);
    wchar_t out[1024];
    swprintf(out, 1024, L"OS:  %s\r\nCPU: %s\r\nRAM: %.1f GB", product.c_str(), cpu.c_str(),
             mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    std::wstring text = out;
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) { memcpy(GlobalLock(h), text.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        CloseClipboard();
    }
}
int ClearTempFiles() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring base = tmp;
    std::wstring pat = base + L"*";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    int n = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring nm = fd.cFileName;
            if (nm == L"." || nm == L"..") continue;
            std::wstring full = base + nm;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring cmd = L"cmd.exe /c rmdir /s /q \"" + full + L"\"";
                RunDetached(cmd);
            } else {
                if (DeleteFileW(full.c_str())) n++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return n;
}
void RunMisc(MiscItem& m) {
    wchar_t p[MAX_PATH];
    switch (m.action) {
    case MA_OPEN_STARTUP_USER:
        if (SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, p) == S_OK) ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOW);
        SetStatus(L"Opened your Startup folder"); break;
    case MA_OPEN_STARTUP_ALL:
        if (SHGetFolderPathW(nullptr, CSIDL_COMMON_STARTUP, nullptr, 0, p) == S_OK) ShellExecuteW(nullptr, L"open", p, nullptr, nullptr, SW_SHOW);
        SetStatus(L"Opened the all-users Startup folder"); break;
    case MA_DISABLE_STARTUP: { int n = SetAllStartupApps(false); SetStatus(L"Disabled " + std::to_wstring(n) + L" startup entries"); break; }
    case MA_ENABLE_STARTUP:  { int n = SetAllStartupApps(true);  SetStatus(L"Re-enabled " + std::to_wstring(n) + L" startup entries"); break; }
    case MA_EMPTY_RECYCLE: SHEmptyRecycleBinW(g_hwnd, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND); SetStatus(L"Recycle Bin emptied"); break;
    case MA_FLUSH_DNS: RunDetached(L"cmd.exe /c ipconfig /flushdns"); SetStatus(L"DNS cache flushed"); break;
    case MA_ENV_VARS: RunDetached(L"rundll32.exe sysdm.cpl,EditEnvironmentVariables"); SetStatus(L"Opened Environment Variables"); break;
    case MA_COPY_SYSINFO: CopySysInfo(); SetStatus(L"System info copied to clipboard"); break;
    case MA_RESTART_EXPLORER: RunDetached(L"cmd.exe /c taskkill /f /im explorer.exe & start explorer.exe"); SetStatus(L"Explorer restarted"); break;
    case MA_RUNCMD: RunDetached(m.param); SetStatus(L"Opened: " + m.title); break;
    case MA_SHELLEXEC: ShellExecuteW(nullptr, L"open", m.param.c_str(), nullptr, nullptr, SW_SHOW); SetStatus(L"Opened: " + m.title); break;
    case MA_CLEAR_TEMP: { int n = ClearTempFiles(); SetStatus(L"Cleared temp files (" + std::to_wstring(n) + L"+ removed)"); break; }
    }
}

// ---------------------------------------------------------------------------
// Monitor helpers
// ---------------------------------------------------------------------------
static std::wstring ParseEDIDName(const std::vector<BYTE>& edid) {
    if ((int)edid.size() < 128) return L"";
    for (int i = 54; i <= 108; i += 18) {
        if (i + 18 > (int)edid.size()) break;
        if (edid[i]==0 && edid[i+1]==0 && edid[i+2]==0 && edid[i+3]==0xFC) {
            char name[14] = {};
            for (int j = 0; j < 13; ++j) { char c=(char)edid[i+5+j]; if (c=='\n'||c=='\r') break; name[j]=c; }
            int len = (int)strlen(name);
            while (len > 0 && name[len-1]==' ') name[--len]=0;
            if (len == 0) continue;
            int wlen = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (wlen > 1) { std::wstring r(wlen-1, L'\0'); MultiByteToWideChar(CP_ACP, 0, name, -1, &r[0], wlen); return r; }
        }
    }
    return L"";
}

static std::wstring GetMonitorFriendlyName(const wchar_t* adapterName) {
    DISPLAY_DEVICEW mon = { sizeof(mon) };
    if (EnumDisplayDevicesW(adapterName, 0, &mon, EDD_GET_DEVICE_INTERFACE_NAME)) {
        std::wstring iface = mon.DeviceID;
        // iface = \\?\DISPLAY#DEL4123#5&xxx#{guid}  ->  DISPLAY\DEL4123\5&xxx
        if (iface.size() > 4 && iface[0]==L'\\') {
            iface = iface.substr(4);                          // strip leading prefix
            auto last = iface.rfind(L'#');
            if (last != std::wstring::npos) iface = iface.substr(0, last); // strip #{guid}
            for (auto& c : iface) if (c==L'#') c=L'\\';
            std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\" + iface + L"\\Device Parameters";
            HKEY hk;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hk) == ERROR_SUCCESS) {
                DWORD type=0, sz=0;
                if (RegQueryValueExW(hk, L"EDID", nullptr, &type, nullptr, &sz)==ERROR_SUCCESS && sz>=128) {
                    std::vector<BYTE> edid(sz);
                    if (RegQueryValueExW(hk, L"EDID", nullptr, &type, edid.data(), &sz)==ERROR_SUCCESS) {
                        RegCloseKey(hk);
                        std::wstring n = ParseEDIDName(edid);
                        if (!n.empty()) return n;
                    } else RegCloseKey(hk);
                } else RegCloseKey(hk);
            }
        }
    }
    DISPLAY_DEVICEW mon2 = { sizeof(mon2) };
    if (EnumDisplayDevicesW(adapterName, 0, &mon2, 0) && mon2.DeviceString[0])
        return mon2.DeviceString;
    return L"Unknown Monitor";
}

void ScanMonitors() {
    g_monitors.clear();
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        MonitorInfo mi;
        mi.deviceName   = dd.DeviceName;
        mi.isPrimary    = (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        mi.friendlyName = GetMonitorFriendlyName(dd.DeviceName);
        DEVMODEW dm = { sizeof(dm) };
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            mi.width = (int)dm.dmPelsWidth;  mi.height = (int)dm.dmPelsHeight;
            mi.refreshRate = (int)dm.dmDisplayFrequency;
            mi.orientation = (int)dm.dmDisplayOrientation;
            mi.posX = (int)dm.dmPosition.x;  mi.posY = (int)dm.dmPosition.y;
        }
        // Enumerate unique resolutions
        std::set<std::pair<int,int>> seenRes;
        DEVMODEW dm2 = { sizeof(dm2) };
        for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &dm2); ++m)
            if (dm2.dmBitsPerPel >= 16)
                seenRes.insert({ (int)dm2.dmPelsWidth, (int)dm2.dmPelsHeight });
        for (auto& p : seenRes) mi.availRes.push_back(p);
        std::sort(mi.availRes.begin(), mi.availRes.end(), [](auto& a, auto& b){
            return a.first * a.second > b.first * b.second; });
        for (int r = 0; r < (int)mi.availRes.size(); ++r)
            if (mi.availRes[r].first==mi.width && mi.availRes[r].second==mi.height) { mi.selResIdx=r; break; }
        // Enumerate Hz for current resolution
        std::set<int> seenHz;
        for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &dm2); ++m)
            if ((int)dm2.dmPelsWidth==mi.width && (int)dm2.dmPelsHeight==mi.height && dm2.dmBitsPerPel>=16)
                seenHz.insert((int)dm2.dmDisplayFrequency);
        for (int hz : seenHz) mi.availHz.push_back(hz);
        std::sort(mi.availHz.rbegin(), mi.availHz.rend());
        for (int h = 0; h < (int)mi.availHz.size(); ++h)
            if (mi.availHz[h]==mi.refreshRate) { mi.selHzIdx=h; break; }
        g_monitors.push_back(mi);
    }
}
void SetMonitorOrientation(int idx) {
    if (idx < 0 || idx >= (int)g_monitors.size()) return;
    DEVMODEW dm = { sizeof(dm) };
    const std::wstring& dev = g_monitors[idx].deviceName;
    if (!EnumDisplaySettingsW(dev.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    int next = (dm.dmDisplayOrientation + 1) % 4;
    bool curLandscape  = (dm.dmDisplayOrientation % 2 == 0);
    bool nextLandscape = (next % 2 == 0);
    if (curLandscape != nextLandscape) { DWORD t = dm.dmPelsWidth; dm.dmPelsWidth = dm.dmPelsHeight; dm.dmPelsHeight = t; }
    dm.dmDisplayOrientation = next;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (ChangeDisplaySettingsExW(dev.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL)
        ScanMonitors();
    else
        SetStatus(L"Orientation change failed – check display driver settings");
}
void SetMonitorPrimary(int idx) {
    if (idx < 0 || idx >= (int)g_monitors.size() || g_monitors[idx].isPrimary) return;
    int shiftX = g_monitors[idx].posX, shiftY = g_monitors[idx].posY;
    for (auto& m : g_monitors) {
        DEVMODEW dm = { sizeof(dm) };
        if (!EnumDisplaySettingsW(m.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) continue;
        dm.dmPosition.x -= shiftX;  dm.dmPosition.y -= shiftY;
        dm.dmFields = DM_POSITION;
        DWORD flags = CDS_UPDATEREGISTRY | CDS_NORESET;
        if (m.deviceName == g_monitors[idx].deviceName) flags |= CDS_SET_PRIMARY;
        ChangeDisplaySettingsExW(m.deviceName.c_str(), &dm, nullptr, flags, nullptr);
    }
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    ScanMonitors();
}

void UpdateMonitorHz(int idx) {
    if (idx < 0 || idx >= (int)g_monitors.size()) return;
    auto& mon = g_monitors[idx];
    if (mon.selResIdx >= (int)mon.availRes.size()) return;
    int w = mon.availRes[mon.selResIdx].first, h = mon.availRes[mon.selResIdx].second;
    std::set<int> seenHz;
    DEVMODEW dm = { sizeof(dm) };
    for (DWORD m = 0; EnumDisplaySettingsW(mon.deviceName.c_str(), m, &dm); ++m)
        if ((int)dm.dmPelsWidth==w && (int)dm.dmPelsHeight==h && dm.dmBitsPerPel>=16)
            seenHz.insert((int)dm.dmDisplayFrequency);
    mon.availHz.clear();
    for (int hz : seenHz) mon.availHz.push_back(hz);
    std::sort(mon.availHz.rbegin(), mon.availHz.rend());
    mon.selHzIdx = 0;
}

void ApplyMonitorMode(int idx) {
    if (idx < 0 || idx >= (int)g_monitors.size()) return;
    auto& mon = g_monitors[idx];
    if (mon.availRes.empty() || mon.selResIdx >= (int)mon.availRes.size()) return;
    int w = mon.availRes[mon.selResIdx].first, h = mon.availRes[mon.selResIdx].second;
    int hz = mon.availHz.empty() ? mon.refreshRate : mon.availHz[mon.selHzIdx];
    DEVMODEW dm = { sizeof(dm) };
    if (!EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h; dm.dmDisplayFrequency = hz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    if (ChangeDisplaySettingsExW(mon.deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr) == DISP_CHANGE_SUCCESSFUL)
        ScanMonitors();
    else
        SetStatus(L"Resolution change failed \u2013 mode may not be supported");
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
void SeedData() {
    auto I = [&](const wchar_t* cat, const wchar_t* name, const wchar_t* id) {
        Installer it; it.category = cat; it.name = name; it.id = id; g_installers.push_back(it);
    };
    // Games & Launchers
    I(L"Games & Launchers", L"Steam",               L"Valve.Steam");
    I(L"Games & Launchers", L"Epic Games Launcher", L"EpicGames.EpicGamesLauncher");
    I(L"Games & Launchers", L"EA App",              L"ElectronicArts.EADesktop");
    I(L"Games & Launchers", L"Ubisoft Connect",     L"Ubisoft.Connect");
    I(L"Games & Launchers", L"GOG Galaxy",          L"GOG.Galaxy");
    I(L"Games & Launchers", L"Battle.net",          L"Blizzard.BattleNet");
    I(L"Games & Launchers", L"itch.io",             L"ItchIo.Itch");
    // Communication
    I(L"Communication",     L"Discord",             L"Discord.Discord");
    I(L"Communication",     L"Zoom",                L"Zoom.Zoom");
    I(L"Communication",     L"Microsoft Teams",     L"Microsoft.Teams");
    I(L"Communication",     L"Telegram Desktop",    L"Telegram.TelegramDesktop");
    I(L"Communication",     L"Slack",               L"SlackTechnologies.Slack");
    I(L"Communication",     L"Signal",              L"OpenWhisperSystems.Signal");
    I(L"Communication",     L"Thunderbird",         L"Mozilla.Thunderbird");
    I(L"Communication",     L"Element",             L"Element.Element");
    // Browsers
    I(L"Browsers",          L"Google Chrome",       L"Google.Chrome");
    I(L"Browsers",          L"Mozilla Firefox",     L"Mozilla.Firefox");
    I(L"Browsers",          L"Brave",               L"Brave.Brave");
    I(L"Browsers",          L"Microsoft Edge",      L"Microsoft.Edge");
    I(L"Browsers",          L"Opera",               L"Opera.Opera");
    I(L"Browsers",          L"Opera GX",            L"Opera.OperaGX");
    I(L"Browsers",          L"Vivaldi",             L"Vivaldi.Vivaldi");
    I(L"Browsers",          L"Tor Browser",         L"TorProject.TorBrowser");
    // Media & Utilities
    I(L"Media & Utilities", L"Spotify",             L"Spotify.Spotify");
    I(L"Media & Utilities", L"VLC Media Player",    L"VideoLAN.VLC");
    I(L"Media & Utilities", L"OBS Studio",          L"OBSProject.OBSStudio");
    I(L"Media & Utilities", L"ShareX",              L"ShareX.ShareX");
    I(L"Media & Utilities", L"Audacity",            L"Audacity.Audacity");
    I(L"Media & Utilities", L"HandBrake",           L"HandBrake.HandBrake");
    I(L"Media & Utilities", L"GIMP",                L"GIMP.GIMP");
    I(L"Media & Utilities", L"Blender",             L"BlenderFoundation.Blender");
    I(L"Media & Utilities", L"Krita",               L"KDE.Krita");
    I(L"Media & Utilities", L"Inkscape",            L"Inkscape.Inkscape");
    // Dev Tools
    I(L"Dev Tools",         L"Visual Studio Code",  L"Microsoft.VisualStudioCode");
    I(L"Dev Tools",         L"Git",                 L"Git.Git");
    I(L"Dev Tools",         L"Notepad++",           L"Notepad++.Notepad++");
    I(L"Dev Tools",         L"Python 3",            L"Python.Python.3.12");
    I(L"Dev Tools",         L"Node.js LTS",         L"OpenJS.NodeJS.LTS");
    I(L"Dev Tools",         L"Windows Terminal",    L"Microsoft.WindowsTerminal");
    I(L"Dev Tools",         L"Docker Desktop",      L"Docker.DockerDesktop");
    I(L"Dev Tools",         L"Postman",             L"Postman.Postman");
    I(L"Dev Tools",         L"Sublime Text",        L"SublimeHQ.SublimeText.4");
    I(L"Dev Tools",         L"GitHub Desktop",      L"GitHub.GitHubDesktop");
    I(L"Dev Tools",         L"JetBrains Toolbox",   L"JetBrains.Toolbox");
    // Productivity
    I(L"Productivity",      L"Notion",              L"Notion.Notion");
    I(L"Productivity",      L"Obsidian",            L"Obsidian.Obsidian");
    I(L"Productivity",      L"LibreOffice",         L"TheDocumentFoundation.LibreOffice");
    I(L"Productivity",      L"OnlyOffice",          L"ONLYOFFICE.DesktopEditors");
    I(L"Productivity",      L"Acrobat Reader",      L"Adobe.Acrobat.Reader.64-bit");
    // Security
    I(L"Security",          L"Bitwarden",           L"Bitwarden.Bitwarden");
    I(L"Security",          L"1Password",           L"AgileBits.1Password");
    I(L"Security",          L"KeePassXC",           L"KeePassXCTeam.KeePassXC");
    I(L"Security",          L"Proton VPN",          L"Proton.ProtonVPN");
    I(L"Security",          L"WireGuard",           L"WireGuard.WireGuard");
    // System Tools
    I(L"System Tools",      L"CPU-Z",               L"CPUID.CPU-Z");
    I(L"System Tools",      L"GPU-Z",               L"TechPowerUp.GPU-Z");
    I(L"System Tools",      L"HWiNFO",              L"REALiX.HWiNFO");
    I(L"System Tools",      L"CrystalDiskInfo",     L"CrystalDewWorld.CrystalDiskInfo");
    I(L"System Tools",      L"7-Zip",               L"7zip.7zip");
    I(L"System Tools",      L"qBittorrent",         L"qBittorrent.qBittorrent");
    I(L"System Tools",      L"Rufus",               L"Rufus.Rufus");
    I(L"System Tools",      L"Everything",          L"voidtools.Everything");
    I(L"System Tools",      L"PowerToys",           L"Microsoft.PowerToys");

    const std::wstring adv  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
    const std::wstring cds  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Search";
    const std::wstring cper = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    const std::wstring ccab = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CabinetState";
    const std::wstring ccdm = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
    auto SET = [&](const wchar_t* cat, const wchar_t* title, const wchar_t* desc, HKEY root, const std::wstring& sub,
                   std::vector<std::wstring> names, DWORD on, DWORD off, bool admin) {
        Setting s; s.category = cat; s.title = title; s.desc = desc; s.root = root; s.subkey = sub;
        s.names = names; s.onValue = on; s.offValue = off; s.adminReq = admin;
        g_settings.push_back(s);
    };
    // Taskbar
    SET(L"Taskbar", L"Left-align taskbar",           L"Classic left-aligned taskbar. Off = Windows 11 centered.",         HKEY_CURRENT_USER, adv,  {L"TaskbarAl"},                  0, 1, false);
    SET(L"Taskbar", L"Show taskbar on all monitors", L"Mirror the taskbar onto every connected display.",                 HKEY_CURRENT_USER, adv,  {L"MMTaskbarEnabled"},           1, 0, false);
    SET(L"Taskbar", L"Never combine taskbar buttons",L"Show separate labelled buttons per window.",                       HKEY_CURRENT_USER, adv,  {L"TaskbarGlomLevel"},           2, 0, false);
    SET(L"Taskbar", L"Show Task View button",        L"Toggle the Task View icon on the taskbar.",                        HKEY_CURRENT_USER, adv,  {L"ShowTaskViewButton"},         1, 0, false);
    SET(L"Taskbar", L"Show search box on taskbar",   L"Full search box instead of just the icon.",                        HKEY_CURRENT_USER, cds,  {L"SearchboxTaskbarMode"},       2, 0, false);
    SET(L"Taskbar", L"Hide Widgets button",          L"Remove the Widgets icon from the taskbar.",                        HKEY_CURRENT_USER, adv,  {L"TaskbarDa"},                  0, 1, false);
    SET(L"Taskbar", L"Always show all tray icons",   L"Stop Windows hiding tray icons behind the arrow.",                 HKEY_CURRENT_USER, adv,  {L"EnableAutoTray"},             0, 1, false);
    SET(L"Taskbar", L"Show seconds in the clock",    L"Add a seconds counter to the system-tray clock.",                  HKEY_CURRENT_USER, adv,  {L"ShowSecondsInSystemClock"},   1, 0, false);
    SET(L"Taskbar", L"Enable \"End task\" in taskbar",L"Right-click a taskbar button to end its process immediately.",    HKEY_CURRENT_USER, adv,  {L"TaskbarEndTask"},             1, 0, false);
    // File Explorer
    SET(L"File Explorer", L"Show file extensions",       L"Show .txt, .exe and other extensions in File Explorer.",      HKEY_CURRENT_USER, adv,  {L"HideFileExt"},                        0, 1, false);
    SET(L"File Explorer", L"Show hidden files",          L"Reveal hidden files and folders in File Explorer.",           HKEY_CURRENT_USER, adv,  {L"Hidden"},                             1, 2, false);
    SET(L"File Explorer", L"Open Explorer to This PC",   L"Start File Explorer on This PC instead of Home.",            HKEY_CURRENT_USER, adv,  {L"LaunchTo"},                           1, 2, false);
    SET(L"File Explorer", L"Show full path in title bar",L"Display the full folder path in Explorer's title bar.",       HKEY_CURRENT_USER, ccab, {L"FullPath"},                           1, 0, false);
    SET(L"File Explorer", L"Expand to current folder",   L"Auto-expand the sidebar to the open folder.",                HKEY_CURRENT_USER, adv,  {L"NavPaneExpandToCurrentFolder"},      1, 0, false);
    SET(L"File Explorer", L"Item check boxes",           L"Show check boxes to select files in Explorer.",               HKEY_CURRENT_USER, adv,  {L"AutoCheckSelect"},                    1, 0, false);
    SET(L"File Explorer", L"Compact view in Explorer",   L"Tighter row spacing in File Explorer lists.",                 HKEY_CURRENT_USER, adv,  {L"UseCompactMode"},                     1, 0, false);
    // Start Menu & Search
    SET(L"Start Menu & Search", L"Web results in Start search",  L"Include Bing web results in the Start menu search.",           HKEY_CURRENT_USER, cds,  {L"BingSearchEnabled"},                  1, 0, false);
    SET(L"Start Menu & Search", L"Show most-used apps in Start", L"List your frequently used apps in the Start menu.",            HKEY_CURRENT_USER, adv,  {L"Start_TrackProgs"},                   1, 0, false);
    SET(L"Start Menu & Search", L"Show recently added items",    L"Show recently opened files/apps in Start & Jump Lists.",       HKEY_CURRENT_USER, adv,  {L"Start_TrackDocs"},                    1, 0, false);
    SET(L"Start Menu & Search", L"Suggested content in Start",   L"App and content suggestions in the Start menu.",              HKEY_CURRENT_USER, ccdm, {L"SystemPaneSuggestionsEnabled"},      1, 0, false);
    // Appearance
    SET(L"Appearance", L"Dark mode",               L"Switch apps and Windows to the dark theme.",                      HKEY_CURRENT_USER, cper, {L"AppsUseLightTheme", L"SystemUsesLightTheme"}, 0, 1, false);
    SET(L"Appearance", L"Transparency effects",    L"Acrylic/transparency on taskbar and menus.",                      HKEY_CURRENT_USER, cper, {L"EnableTransparency"},  1, 0, false);
    SET(L"Appearance", L"Accent color on title bars",L"Tint window title bars and borders with the accent color.",     HKEY_CURRENT_USER, cper, {L"ColorPrevalence"},     1, 0, false);
    // Window Management
    SET(L"Window Management", L"Snap layout suggestions",      L"Show snap-assist suggestions when snapping a window.",    HKEY_CURRENT_USER, adv, {L"SnapAssist"},        1, 0, false);
    SET(L"Window Management", L"Aero Shake (shake to minimize)",L"Shake a window's title bar to minimize the others.",     HKEY_CURRENT_USER, adv, {L"DisallowShaking"},   0, 1, false);
    // Privacy
    SET(L"Privacy", L"Tips, tricks & suggestions",   L"Windows tip notifications and suggestions (ads).",              HKEY_CURRENT_USER, ccdm, {L"SubscribedContent-338389Enabled"},    1, 0, false);
    SET(L"Privacy", L"Auto-install suggested apps",  L"Let Windows silently install promoted apps.",                   HKEY_CURRENT_USER, ccdm, {L"SilentInstalledAppsEnabled"},         1, 0, false);
    SET(L"Privacy", L"Lock screen tips & facts",     L"Fun facts, tips and ads on the lock screen.",                   HKEY_CURRENT_USER, ccdm, {L"RotatingLockScreenOverlayEnabled"},  1, 0, false);
    // Developer
    SET(L"Developer", L"Enable Developer Mode",          L"Allow sideloading apps and other developer features.", HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",    {L"AllowDevelopmentWithoutDevLicense"}, 1, 0, true);
    SET(L"Developer", L"Verbose startup/shutdown status",L"Show detailed messages during sign-in and shutdown.",  HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System", {L"VerboseStatus"},                     1, 0, true);
    // Monitor Settings
    const std::wstring cgfx   = L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";
    const std::wstring cgame  = L"Software\\Microsoft\\GameBar";
    SET(L"Monitor Settings", L"Show desktop icons",                  L"Toggle visibility of all icons on the desktop.",                   HKEY_CURRENT_USER,  adv,   {L"HideIcons"},          0, 1, false);
    SET(L"Monitor Settings", L"Hardware-accelerated GPU scheduling", L"Lowers GPU latency and improves frame pacing. Requires reboot.",   HKEY_LOCAL_MACHINE, cgfx,  {L"HwSchMode"},          2, 1, true);
    SET(L"Monitor Settings", L"Game Mode",                          L"Prioritise CPU and GPU resources for the active game.",            HKEY_CURRENT_USER,  cgame, {L"AllowAutoGameMode"},  1, 0, false);
    // Mouse Settings
    const std::wstring cmouse = L"Control Panel\\Mouse";
    SET(L"Mouse Settings", L"Enhance pointer precision", L"Applies acceleration to mouse movement. Disable for raw-input gaming.", HKEY_CURRENT_USER, cmouse, {L"MouseSpeed"},           1, 0, false);
    SET(L"Mouse Settings", L"Swap mouse buttons",        L"Swap primary and secondary buttons for left-handed use.",              HKEY_CURRENT_USER, cmouse, {L"SwapMouseButtons"},     1, 0, false);
    SET(L"Mouse Settings", L"Hide pointer while typing", L"Vanish the cursor when keyboard input is detected.",                    HKEY_CURRENT_USER, cmouse, {L"MouseVanish"},          1, 0, false);
    SET(L"Mouse Settings", L"Snap to default button",    L"Automatically move the pointer to the default button in dialogs.",     HKEY_CURRENT_USER, cmouse, {L"SnapToDefaultButton"}, 1, 0, false);

    for (auto& s : g_settings) {
        DWORD v = 0;
        if (RegReadDword(s.root, s.subkey, s.names[0], v)) s.on = (v == s.onValue);
        s.toggle = s.on ? 1.0f : 0.0f;
    }

    auto M = [&](const wchar_t* t, const wchar_t* d, MiscAction a, const wchar_t* confirm) {
        MiscItem m; m.title = t; m.desc = d; m.action = a; if (confirm) m.confirm = confirm; g_misc.push_back(m);
    };
    auto MC = [&](const wchar_t* t, const wchar_t* d, const wchar_t* cmd, const wchar_t* confirm = nullptr) {
        MiscItem m; m.title = t; m.desc = d; m.action = MA_RUNCMD; m.param = cmd; if (confirm) m.confirm = confirm; g_misc.push_back(m);
    };
    auto MO = [&](const wchar_t* t, const wchar_t* d, const wchar_t* target) {
        MiscItem m; m.title = t; m.desc = d; m.action = MA_SHELLEXEC; m.param = target; g_misc.push_back(m);
    };
    // Startup
    M(L"Open Startup folder (you)", L"Open your personal startup shortcuts folder.", MA_OPEN_STARTUP_USER, nullptr);
    M(L"Open Startup folder (all users)", L"Open the startup folder shared by every account.", MA_OPEN_STARTUP_ALL, nullptr);
    M(L"Disable all startup apps", L"Flip every Task Manager startup entry to Disabled.", MA_DISABLE_STARTUP, L"Disable every startup entry currently listed in Task Manager?");
    M(L"Re-enable all startup apps", L"Undo: flip every startup entry back to Enabled.", MA_ENABLE_STARTUP, nullptr);
    // Cleanup
    M(L"Empty Recycle Bin", L"Permanently delete everything in the Recycle Bin.", MA_EMPTY_RECYCLE, L"Permanently delete everything in the Recycle Bin?");
    M(L"Clear temp files", L"Delete the contents of your %TEMP% folder.", MA_CLEAR_TEMP, L"Delete everything in your TEMP folder?");
    MC(L"Run Disk Cleanup", L"Launch the Windows Disk Cleanup wizard.", L"cleanmgr.exe");
    M(L"Flush DNS cache", L"Clear cached DNS lookups (ipconfig /flushdns).", MA_FLUSH_DNS, nullptr);
    M(L"Restart Explorer", L"Apply taskbar / Explorer setting changes now.", MA_RESTART_EXPLORER, nullptr);
    // System consoles
    MC(L"Device Manager", L"View and manage hardware devices.", L"cmd.exe /c start devmgmt.msc");
    MC(L"Disk Management", L"Partition and format drives.", L"cmd.exe /c start diskmgmt.msc");
    MC(L"Services", L"Start, stop and configure Windows services.", L"cmd.exe /c start services.msc");
    MC(L"Task Manager", L"Open Task Manager.", L"taskmgr.exe");
    MC(L"Registry Editor", L"Open the Windows registry editor.", L"regedit.exe");
    MC(L"Event Viewer", L"Browse system and application logs.", L"cmd.exe /c start eventvwr.msc");
    MC(L"System Information", L"Detailed hardware and OS report (msinfo32).", L"msinfo32.exe");
    MC(L"DirectX Diagnostics", L"GPU / audio diagnostics (dxdiag).", L"dxdiag.exe");
    MC(L"Reliability Monitor", L"Stability history and crash reports.", L"cmd.exe /c perfmon /rel");
    // Classic control panels
    MC(L"Programs & Features", L"Uninstall or change installed programs.", L"cmd.exe /c start appwiz.cpl");
    MC(L"Network Connections", L"Manage network adapters (ncpa.cpl).", L"cmd.exe /c start ncpa.cpl");
    MC(L"Power Options", L"Edit power plans (powercfg.cpl).", L"cmd.exe /c start powercfg.cpl");
    MC(L"Mouse Settings", L"Classic mouse control panel.", L"cmd.exe /c start main.cpl");
    MC(L"Sound Settings", L"Classic sound / playback devices.", L"cmd.exe /c start mmsys.cpl");
    M(L"Edit environment variables", L"Open the classic environment variables dialog.", MA_ENV_VARS, nullptr);
    MC(L"God Mode", L"Open the master control-panel task list.", L"cmd.exe /c start shell:::{ED7BA470-8E54-465E-825C-99712043E01C}");
    MC(L"Open hosts file", L"Edit the Windows hosts file in Notepad.", L"notepad.exe C:\\Windows\\System32\\drivers\\etc\\hosts");
    // Settings pages
    MO(L"Windows Update", L"Open the Windows Update settings page.", L"ms-settings:windowsupdate");
    MO(L"Installed apps", L"Open the Apps & features settings page.", L"ms-settings:appsfeatures");
    MO(L"Optional features", L"Add or remove Windows optional features.", L"ms-settings:optionalfeatures");
    MO(L"Startup apps (Settings)", L"Manage startup apps in Settings.", L"ms-settings:startupapps");
    MO(L"Display settings", L"Resolution, scaling and multi-monitor.", L"ms-settings:display");
    // Admin-flavored maintenance
    MC(L"SFC scan (repair system files)", L"Runs sfc /scannow in an elevated console.", L"powershell.exe -Command \"Start-Process cmd -ArgumentList '/k sfc /scannow' -Verb RunAs\"");
    MC(L"DISM restore health", L"Repairs the Windows image (elevated).", L"powershell.exe -Command \"Start-Process cmd -ArgumentList '/k DISM /Online /Cleanup-Image /RestoreHealth' -Verb RunAs\"");
    MC(L"Create restore point", L"Make a System Restore checkpoint (elevated).", L"powershell.exe -Command \"Start-Process powershell -ArgumentList '-Command Checkpoint-Computer -Description EasySetup -RestorePointType MODIFY_SETTINGS' -Verb RunAs\"");
    // Info
    M(L"Copy system info", L"Copy OS, CPU and RAM details to the clipboard.", MA_COPY_SYSINFO, nullptr);
}

// ---------------------------------------------------------------------------
// Direct2D resources
// ---------------------------------------------------------------------------
void CreateTextFormats() {
    auto mk = [&](float size, DWRITE_FONT_WEIGHT w, ComPtr<IDWriteTextFormat>& out, const wchar_t* face = L"Segoe UI") {
        g_dwrite->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &out);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    mk(S(30), DWRITE_FONT_WEIGHT_BOLD,      g_fBig);
    mk(S(23), DWRITE_FONT_WEIGHT_SEMI_BOLD, g_fH1);
    mk(S(15), DWRITE_FONT_WEIGHT_SEMI_BOLD, g_fTitle);
    mk(S(14), DWRITE_FONT_WEIGHT_NORMAL,    g_fBody);
    mk(S(12), DWRITE_FONT_WEIGHT_NORMAL,    g_fSmall);
    mk(S(11), DWRITE_FONT_WEIGHT_SEMI_BOLD, g_fTiny);
    mk(S(13), DWRITE_FONT_WEIGHT_SEMI_BOLD, g_fNav);
    mk(S(15), DWRITE_FONT_WEIGHT_BOLD,      g_fIcon);
    mk(S(12), DWRITE_FONT_WEIGHT_NORMAL,    g_fMono, L"Consolas");
    mk(S(17), DWRITE_FONT_WEIGHT_NORMAL,    g_fIconNav, L"Segoe MDL2 Assets");
    mk(S(26), DWRITE_FONT_WEIGHT_NORMAL,    g_fIconBig, L"Segoe MDL2 Assets");
    mk(S(13), DWRITE_FONT_WEIGHT_NORMAL,    g_fIconSm,  L"Segoe MDL2 Assets");
}
HRESULT CreateDeviceResources() {
    if (g_rt) return S_OK;
    RECT rc; GetClientRect(g_hwnd, &rc);
    HRESULT hr = g_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)),
        &g_rt);
    if (FAILED(hr)) return hr;
    g_rt->SetDpi(96.0f, 96.0f);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_brush);
    return hr;
}
void DiscardDeviceResources() { g_iconBmp.clear(); g_brush.Reset(); g_rt.Reset(); }

// Decode all embedded brand PNGs into device-independent WIC sources (once).
void LoadIconSources() {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&g_wic)))) return;
    for (int i = 0; i < g_iconBlobCount; ++i) {
        const IconBlob& b = g_iconBlobs[i];
        ComPtr<IWICStream> stream;
        if (FAILED(g_wic->CreateStream(&stream))) continue;
        if (FAILED(stream->InitializeFromMemory((BYTE*)b.data, b.len))) continue;
        ComPtr<IWICBitmapDecoder> dec;
        if (FAILED(g_wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec))) continue;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(dec->GetFrame(0, &frame))) continue;
        ComPtr<IWICFormatConverter> conv;
        if (FAILED(g_wic->CreateFormatConverter(&conv))) continue;
        if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                    WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) continue;
        g_iconSrc[b.id] = conv;
    }
}
// Lazily create (and cache) a device bitmap for the current render target.
ID2D1Bitmap* GetIconBitmap(const std::wstring& id) {
    auto itb = g_iconBmp.find(id);
    if (itb != g_iconBmp.end()) return itb->second.Get();
    auto its = g_iconSrc.find(id);
    if (its == g_iconSrc.end()) return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(g_rt->CreateBitmapFromWicBitmap(its->second.Get(), nullptr, &bmp))) return nullptr;
    g_iconBmp[id] = bmp;
    return bmp.Get();
}

// ---------------------------------------------------------------------------
// Draw primitives
// ---------------------------------------------------------------------------
D2D1_COLOR_F HSV(float h, float s, float v, float a = 1.0f) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; } else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    return D2D1::ColorF(r + m, g + m, b + m, a);
}
void SetC(int hex, float a = 1.0f) { g_brush->SetColor(RGBH(hex, a)); }
void FillRR(float l, float t, float r, float b, float rad, int hex, float a = 1.0f) {
    SetC(hex, a); D2D1_ROUNDED_RECT rr = { { l, t, r, b }, rad, rad }; g_rt->FillRoundedRectangle(rr, g_brush.Get());
}
void FillRRc(float l, float t, float r, float b, float rad, D2D1_COLOR_F col) {
    g_brush->SetColor(col); D2D1_ROUNDED_RECT rr = { { l, t, r, b }, rad, rad }; g_rt->FillRoundedRectangle(rr, g_brush.Get());
}
void StrokeRR(float l, float t, float r, float b, float rad, int hex, float a, float w) {
    SetC(hex, a); D2D1_ROUNDED_RECT rr = { { l, t, r, b }, rad, rad }; g_rt->DrawRoundedRectangle(rr, g_brush.Get(), w);
}
enum TAlign { TXA_LEFT, TXA_CENTER, TXA_RIGHT };
void Text(const std::wstring& s, const ComPtr<IDWriteTextFormat>& fmt, float l, float t, float r, float b,
          int hex, float a = 1.0f, TAlign al = TXA_LEFT) {
    fmt->SetTextAlignment(al == TXA_LEFT ? DWRITE_TEXT_ALIGNMENT_LEADING :
                          al == TXA_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    SetC(hex, a);
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), fmt.Get(), D2D1::RectF(l, t, r, b), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void TextC(const std::wstring& s, const ComPtr<IDWriteTextFormat>& fmt, float l, float t, float r, float b,
           D2D1_COLOR_F col, TAlign al = TXA_LEFT) {
    fmt->SetTextAlignment(al == TXA_LEFT ? DWRITE_TEXT_ALIGNMENT_LEADING :
                          al == TXA_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_brush->SetColor(col);
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), fmt.Get(), D2D1::RectF(l, t, r, b), g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

int BrandColor(const std::wstring& name, const std::wstring& id) {
    std::wstring k = lower(id + L" " + name);
    struct BC { const wchar_t* s; int c; };
    static const BC map[] = {
        {L"steam",0x3A6EA5},{L"epic",0x2F2F2F},{L"electronicarts",0xF12E2E},{L"ubisoft",0x1F6FEB},{L"gog",0x9B4DCA},
        {L"battlenet",0x1787D6},{L"discord",0x5865F2},{L"zoom",0x2D8CFF},{L"teams",0x6264A7},{L"telegram",0x2AABEE},
        {L"slack",0x4A154B},{L"chrome",0x4285F4},{L"firefox",0xE66000},{L"brave",0xFB542B},{L"spotify",0x1DB954},
        {L"vlc",0xE85E00},{L"7zip",0x2E7D32},{L"qbittorrent",0x2F67B2},{L"obs",0x4B4B52},{L"sharex",0x5A9BD4},
        {L"visualstudiocode",0x2C81C6},{L"git.git",0xF05133},{L"notepad",0x6BA539},{L"powertoys",0x4B6EAF},
        {L"python",0x3776AB},{L"nodejs",0x539E43},
        // expanded catalogue
        {L"itch",0xFA5C5C},{L"signal",0x3A76F0},{L"thunderbird",0x2A66C8},{L"element",0x0DBD8B},
        {L"microsoft.edge",0x2C7DD6},{L"opera.operagx",0xEB0029},{L"opera",0xE7500A},{L"vivaldi",0xEF3939},{L"tor",0x7D4698},
        {L"audacity",0x0000CC},{L"handbrake",0xC65D21},{L"gimp",0x655741},{L"blender",0xE87D0D},{L"krita",0x3C36A0},
        {L"inkscape",0x2B2B2B},{L"rufus",0xC8102E},{L"everything",0x2D6E9E},
        {L"windowsterminal",0x2C6E75},{L"docker",0x2496ED},{L"postman",0xFF6C37},{L"sublime",0xFF9800},
        {L"github",0x2B3137},{L"jetbrains",0x000000},
        {L"notion",0x2F2F2F},{L"obsidian",0x7C3AED},{L"libreoffice",0x18A303},{L"onlyoffice",0xFF6F3D},{L"acrobat",0xEC1C24},
        {L"bitwarden",0x175DDC},{L"1password",0x1A8CFF},{L"keepass",0x2C6E9B},{L"proton",0x6D4AFF},{L"wireguard",0x88171A},
        {L"cpu-z",0xB01E28},{L"gpu-z",0x2E7D32},{L"hwinfo",0x1C6EA4},{L"crystaldisk",0x2B8FB3},
    };
    for (auto& e : map) if (k.find(e.s) != std::wstring::npos) return e.c;
    unsigned h = 2166136261u; for (wchar_t c : id) h = (h ^ (unsigned)c) * 16777619u;
    return -1 - (int)(h % 360); // encode hue as negative sentinel
}
void DrawAppIcon(float l, float t, float sz, const Installer& it) {
    int bc = BrandColor(it.name, it.id);
    D2D1_COLOR_F col = bc >= 0 ? RGBH(bc) : HSV((float)((-1 - bc)), 0.52f, 0.80f);
    FillRRc(l, t, l + sz, t + sz, S(9), col);
    FillRRc(l, t, l + sz, t + sz * 0.5f, S(9), D2D1::ColorF(1, 1, 1, 0.08f)); // top gloss
    ID2D1Bitmap* bmp = GetIconBitmap(it.id);
    if (bmp) {
        float pad = sz * 0.22f;
        D2D1_RECT_F dst = { l + pad, t + pad, l + sz - pad, t + sz - pad };
        g_rt->DrawBitmap(bmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        std::wstring init(1, towupper(it.name[0]));
        TextC(init, g_fIcon, l, t - S(1), l + sz, t + sz, D2D1::ColorF(1, 1, 1, 0.95f), TXA_CENTER);
    }
}

void DrawSpinner(float cx, float cy, float r, int hex) {
    for (int i = 0; i < 8; ++i) {
        float ang = (float)(i / 8.0 * 6.2831853);
        float x = cx + cosf(ang) * r, y = cy + sinf(ang) * r;
        float ph = (float)fmod((double)i / 8.0 + g_now * 1.1, 1.0);
        SetC(hex, 0.15f + 0.85f * ph);
        D2D1_ELLIPSE e = { { x, y }, S(1.7f), S(1.7f) };
        g_rt->FillEllipse(e, g_brush.Get());
    }
}
void DrawTick(float cx, float cy, float sz, D2D1_COLOR_F col, float w) {
    g_brush->SetColor(col);
    D2D1_POINT_2F a = { cx - sz * 0.5f, cy + sz * 0.04f };
    D2D1_POINT_2F b = { cx - sz * 0.12f, cy + sz * 0.42f };
    D2D1_POINT_2F c = { cx + sz * 0.5f, cy - sz * 0.4f };
    g_rt->DrawLine(a, b, g_brush.Get(), w); g_rt->DrawLine(b, c, g_brush.Get(), w);
}
void DrawStatus(float cx, float cy, int status) {
    switch (status) {
    case ST_QUEUED: {
        SetC(COL_TEXT_FAINT, 1); D2D1_ELLIPSE e = { { cx, cy }, S(7), S(7) };
        g_rt->DrawEllipse(e, g_brush.Get(), S(1.4f)); break; }
    case ST_INSTALLING: DrawSpinner(cx, cy, S(7), COL_ACCENT); break;
    case ST_DONE: {
        FillRRc(cx - S(9), cy - S(9), cx + S(9), cy + S(9), S(9), RGBH(COL_GOOD));
        DrawTick(cx, cy, S(11), D2D1::ColorF(1, 1, 1), S(1.8f)); break; }
    case ST_FAILED: {
        FillRRc(cx - S(9), cy - S(9), cx + S(9), cy + S(9), S(9), RGBH(COL_BAD));
        g_brush->SetColor(D2D1::ColorF(1, 1, 1));
        D2D1_POINT_2F a = { cx - S(4), cy - S(4) }, b = { cx + S(4), cy + S(4) };
        D2D1_POINT_2F c = { cx + S(4), cy - S(4) }, d = { cx - S(4), cy + S(4) };
        g_rt->DrawLine(a, b, g_brush.Get(), S(1.8f)); g_rt->DrawLine(c, d, g_brush.Get(), S(1.8f)); break; }
    }
}
void DrawCheckbox(float cx, float cy, float prog, bool hover) {
    // Win11 checkbox: accent fill + dark checkmark when checked
    float half = S(9);
    if (prog > 0.02f) FillRRc(cx - half, cy - half, cx + half, cy + half, S(4), RGBH(COL_ACCENT, prog));
    StrokeRR(cx - half, cy - half, cx + half, cy + half, S(4),
             prog > 0.5f ? COL_ACCENT : (hover ? COL_TEXT_DIM : COL_TRACK), 1, S(1.4f));
    if (prog > 0.1f) DrawTick(cx, cy, S(10), D2D1::ColorF(0.06f, 0.06f, 0.06f, clampf(prog * 1.4f, 0, 1)), S(1.8f));
}
void DrawToggle(float l, float t, float prog) {
    // Win11 toggle: off = outlined track + light knob; on = accent track + dark knob
    float w = S(40), h = S(20);
    D2D1_COLOR_F ct = RGBH(COL_TRACK), ca = RGBH(COL_ACCENT);
    g_brush->SetColor(D2D1::ColorF(ct.r + (ca.r - ct.r) * prog, ct.g + (ca.g - ct.g) * prog, ct.b + (ca.b - ct.b) * prog, prog < 0.5f ? 0.0f : 1.0f));
    D2D1_ROUNDED_RECT rr = { { l, t, l + w, t + h }, h / 2, h / 2 };
    if (prog >= 0.5f) g_rt->FillRoundedRectangle(rr, g_brush.Get());
    // track outline (visible in off state)
    g_brush->SetColor(RGBH(prog >= 0.5f ? COL_ACCENT : 0x9A9A9A, prog >= 0.5f ? 1.0f : 1.0f));
    g_rt->DrawRoundedRectangle(rr, g_brush.Get(), S(1.4f));
    float knob = S(6) + S(1.5f) * prog;                 // knob grows slightly when on
    float kx = l + S(4) + knob + (w - S(8) - knob * 2) * prog, ky = t + h / 2;
    D2D1_COLOR_F koff = RGBH(0xCFCFCF), kon = RGBH(0x1B1B1B);
    g_brush->SetColor(D2D1::ColorF(koff.r + (kon.r - koff.r) * prog, koff.g + (kon.g - koff.g) * prog, koff.b + (kon.b - koff.b) * prog));
    D2D1_ELLIPSE e = { { kx, ky }, knob, knob }; g_rt->FillEllipse(e, g_brush.Get());
}

// ---------------------------------------------------------------------------
const float NAVW = 220;
const wchar_t* NAV_LABELS[PAGE_COUNT] = { L"Home", L"Installers", L"Windows Settings", L"Misc" };
const wchar_t* NAV_GLYPH[PAGE_COUNT]  = { ICON_HOME, ICON_DL, ICON_SET, ICON_TOOLS };

int CountChecked() { int n = 0; for (auto& it : g_installers) if (it.checked) n++; return n; }
bool MatchesSearch(const Installer& it) {
    if (g_search.empty()) return true;
    std::wstring q = lower(g_search);
    return lower(it.name).find(q) != std::wstring::npos || lower(it.id).find(q) != std::wstring::npos;
}

void DrawSidebar(float W, float H) {
    FillRR(0, 0, S(NAVW), H, 0, COL_SIDEBAR);
    SetC(COL_BORDER);
    D2D1_POINT_2F p1 = { S(NAVW), 0 }, p2 = { S(NAVW), H };
    g_rt->DrawLine(p1, p2, g_brush.Get(), 1.0f);

    // brand mark: mini app-grid matching the app icon
    FillRRc(S(22), S(24), S(52), S(54), S(8), RGBH(0x23232A));
    FillRRc(S(28), S(30), S(35), S(37), S(2), RGBH(0xFF5D5D));
    FillRRc(S(39), S(30), S(46), S(37), S(2), RGBH(0x4ECB8A));
    FillRRc(S(28), S(41), S(35), S(48), S(2), RGBH(0x5E9EFF));
    FillRRc(S(39), S(41), S(46), S(48), S(2), RGBH(0xF5A623));
    Text(L"Easy Setup", g_fTitle, S(62), S(24), S(NAVW), S(46), COL_TEXT, 1, TXA_LEFT);
    Text(L"one-click setup", g_fSmall, S(62), S(44), S(NAVW), S(62), COL_TEXT_FAINT, 1, TXA_LEFT);

    float top = S(96), rowH = S(46);
    float activeY = top + g_page * rowH;
    g_navIndicator = ease(g_navIndicator, activeY, 0.30f);
    FillRR(S(12), g_navIndicator + S(5), S(NAVW) - S(12), g_navIndicator + rowH - S(5), S(10), COL_ROW_SEL);
    FillRR(S(12), g_navIndicator + S(13), S(15), g_navIndicator + rowH - S(13), S(2), COL_ACCENT);

    for (int i = 0; i < PAGE_COUNT; ++i) {
        float y = top + i * rowH;
        if (i != g_page && g_navHover[i] > 0.01f)
            FillRR(S(12), y + S(5), S(NAVW) - S(12), y + rowH - S(5), S(10), COL_ROW_HOVER, g_navHover[i] * 0.6f);
        bool active = (i == g_page);
        Text(NAV_GLYPH[i], g_fIconNav, S(26), y, S(56), y + rowH, active ? COL_ACCENT : COL_TEXT_DIM, 1, TXA_CENTER);
        Text(NAV_LABELS[i], g_fNav, S(58), y, S(NAVW) - S(12), y + rowH, active ? COL_TEXT : COL_TEXT_DIM, 1, TXA_LEFT);
        g_hits.push_back({ { 0, y, S(NAVW), y + rowH }, HIT_NAV, i });
    }

    float by = H - S(60);
    bool a = g_isAdmin;
    bool ah = false; for (auto& h : g_hits) if (h.kind == HIT_ADMIN_BTN && inRect(h.r, g_mouseX, g_mouseY)) ah = true;
    FillRR(S(16), by, S(NAVW) - S(16), by + S(40), S(8), (!a && ah) ? COL_ROW_HOVER : COL_CARD);
    StrokeRR(S(16), by, S(NAVW) - S(16), by + S(40), S(8), COL_BORDER, 1, 1);
    // status dot + label (no emoji)
    { D2D1_ELLIPSE e = { { S(30), by + S(20) }, S(4), S(4) }; g_brush->SetColor(RGBH(a ? COL_GOOD : COL_ACCENT)); g_rt->FillEllipse(e, g_brush.Get()); }
    Text(a ? L"Administrator" : L"Run as admin", g_fSmall, S(42), by, S(NAVW) - S(16), by + S(40),
         a ? COL_GOOD : COL_TEXT, 1, TXA_LEFT);
    if (!a) g_hits.push_back({ { S(16), by, S(NAVW) - S(16), by + S(40) }, HIT_ADMIN_BTN, 0 });
}

void DrawHeader(float x, float W, const std::wstring& title, const std::wstring& sub) {
    Text(title, g_fH1, x, S(26), W, S(58), COL_TEXT, 1, TXA_LEFT);
    Text(sub,   g_fBody, x, S(58), W, S(82), COL_TEXT_DIM, 1, TXA_LEFT);
}

void DrawHome(float ox, float W, float H) {
    DrawHeader(ox, W, L"Welcome back", L"Set up a fresh Windows PC in a couple of clicks.");
    struct Card { const wchar_t* t; const wchar_t* d; const wchar_t* g; Page p; int accent; };
    Card cards[] = {
        { L"Installers",       L"Pick apps and install the latest versions in one go.", ICON_DL,    PAGE_INSTALLERS, COL_ACCENT },
        { L"Windows Settings", L"Flip common taskbar, Explorer and developer toggles.",  ICON_SET,   PAGE_SETTINGS,   COL_GOOD },
        { L"Misc",             L"Startup apps, DNS, Recycle Bin and other quick tools.",  ICON_TOOLS, PAGE_MISC,       0xC08AE0 },
    };
    float y = S(116), cardH = S(94);
    for (int i = 0; i < 3; ++i) {
        float t = y + i * (cardH + S(14)), l = ox, r = W;
        float hv = 0; for (auto& h : g_hits) if (h.kind == HIT_HOME_CARD && h.index == (int)cards[i].p && inRect(h.r, g_mouseX, g_mouseY)) hv = 1;
        FillRR(l, t, r, t + cardH, S(14), hv > 0 ? COL_CARD_HI : COL_CARD);
        StrokeRR(l, t, r, t + cardH, S(14), hv > 0 ? COL_BORDER_HI : COL_BORDER, 1, 1);
        FillRRc(l + S(20), t + S(23), l + S(68), t + S(71), S(12), RGBH(cards[i].accent, 0.16f));
        Text(cards[i].g, g_fIconBig, l + S(20), t + S(19), l + S(68), t + S(75), cards[i].accent, 1, TXA_CENTER);
        Text(cards[i].t, g_fTitle, l + S(86), t + S(22), r - S(60), t + S(48), COL_TEXT, 1, TXA_LEFT);
        Text(cards[i].d, g_fSmall, l + S(86), t + S(48), r - S(60), t + S(76), COL_TEXT_DIM, 1, TXA_LEFT);
        Text(ICON_CHEV, g_fIconSm, r - S(52), t, r - S(24), t + cardH, COL_TEXT_FAINT, 1, TXA_CENTER);
        g_hits.push_back({ { l, t, r, t + cardH }, HIT_HOME_CARD, (int)cards[i].p });
    }
    // little stat line
    float sy = y + 3 * (cardH + S(14)) + S(6);
    std::wstring stat = std::to_wstring(g_installers.size()) + L" apps  ·  " +
                        std::to_wstring(g_settings.size()) + L" settings  ·  " +
                        std::to_wstring(g_misc.size()) + L" tools";
    Text(stat, g_fSmall, ox, sy, W, sy + S(20), COL_TEXT_FAINT, 1, TXA_LEFT);
}

void DrawInstallers(float ox, float W, float H) {
    int checked = CountChecked();
    std::wstring sub = std::to_wstring(g_installers.size()) + L" apps available";
    if (checked) sub += L"  ·  " + std::to_wstring(checked) + L" selected";
    if (!g_scanDone) sub += L"  ·  checking what's installed...";
    DrawHeader(ox, W, L"Installers", sub);

    // search box
    float sbT = S(92), sbH = S(38);
    bool sf = g_searchFocus;
    FillRR(ox, sbT, W, sbT + sbH, S(10), COL_CARD);
    StrokeRR(ox, sbT, W, sbT + sbH, S(10), sf ? COL_ACCENT : (g_searchHover > 0.3f ? COL_BORDER_HI : COL_BORDER), 1, sf ? S(1.6f) : 1.0f);
    Text(ICON_SEARCH, g_fIconSm, ox + S(14), sbT, ox + S(38), sbT + sbH, COL_TEXT_FAINT, 1, TXA_LEFT);
    std::wstring shown = g_search;
    if (shown.empty() && !sf) Text(L"Search apps...", g_fBody, ox + S(40), sbT, W - S(20), sbT + sbH, COL_TEXT_FAINT, 1, TXA_LEFT);
    else {
        std::wstring caret = (sf && fmod(g_now, 1.0) < 0.5) ? L"|" : L"";
        Text(shown + caret, g_fBody, ox + S(40), sbT, W - S(20), sbT + sbH, COL_TEXT, 1, TXA_LEFT);
    }
    g_hits.push_back({ { ox, sbT, W, sbT + sbH }, HIT_SEARCH, 0 });

    // list
    float listTop = S(142), listBottom = H - S(150);
    g_rt->PushAxisAlignedClip(D2D1::RectF(ox - S(6), listTop, W + S(24), listBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = listTop + S(4) - g_scroll;
    float rowH = S(60);
    std::wstring lastCat;
    for (size_t i = 0; i < g_installers.size(); ++i) {
        auto& it = g_installers[i];
        if (!MatchesSearch(it)) continue;
        if (it.category != lastCat) {
            lastCat = it.category;
            int inCat = 0, chk = 0;
            for (auto& o : g_installers) if (o.category == lastCat && MatchesSearch(o)) { inCat++; if (o.checked) chk++; }
            if (y + S(30) > listTop && y < listBottom) {
                Text(lastCat, g_fTiny, ox + S(4), y + S(10), W - S(90), y + S(30), COL_TEXT_FAINT, 1, TXA_LEFT);
                std::wstring sa = (chk == inCat) ? L"clear" : L"select all";
                Text(sa, g_fTiny, W - S(90), y + S(10), W - S(4), y + S(30), COL_ACCENT, 1, TXA_RIGHT);
                // only clickable when fully inside the list viewport (no click-through)
                if (y + S(6) >= listTop && y + S(30) <= listBottom)
                    g_hits.push_back({ { W - S(96), y + S(6), W - S(4), y + S(30) }, HIT_CATEGORY, (int)i });
            }
            y += S(34);
        }
        float rt = y, rb = y + rowH;
        if (rb > listTop && rt < listBottom) {
            if (it.checked)      FillRR(ox, rt + S(3), W, rb - S(3), S(10), COL_ROW_SEL);
            else if (it.hover > 0.01f) FillRR(ox, rt + S(3), W, rb - S(3), S(10), COL_ROW_HOVER, it.hover * 0.85f);
            DrawCheckbox(ox + S(22), (rt + rb) / 2, it.check, it.hover > 0.3f);
            DrawAppIcon(ox + S(46), (rt + rb) / 2 - S(18), S(36), it);
            Text(it.name, g_fBody, ox + S(98), rt + S(9), W - S(150), rt + S(31), COL_TEXT, 1, TXA_LEFT);
            Text(it.id, g_fSmall, ox + S(98), rt + S(32), W - S(150), rt + S(52), COL_TEXT_FAINT, 1, TXA_LEFT);
            if (it.status != ST_NONE) DrawStatus(W - S(24), (rt + rb) / 2, it.status);
            else if (it.installed) {
                FillRR(W - S(96), (rt + rb) / 2 - S(11), W - S(14), (rt + rb) / 2 + S(11), S(11), COL_GOOD, 0.14f);
                Text(L"installed", g_fTiny, W - S(96), (rt + rb) / 2 - S(11), W - S(14), (rt + rb) / 2 + S(11), COL_GOOD, 1, TXA_CENTER);
            }
            // clamp the click target to the visible list so the action bar below can't be clicked through
            float hitT = std::max(rt, listTop), hitB = std::min(rb, listBottom);
            if (hitB - hitT > S(6))
                g_hits.push_back({ { ox, hitT, W - S(40), hitB }, HIT_INSTALLER_ROW, (int)i });
        }
        y += rowH;
    }
    g_rt->PopAxisAlignedClip();
    float totalBottom = y + g_scroll;
    g_scrollMax = std::max(0.0f, totalBottom - listBottom + S(8));

    // action bar
    float barY = H - S(138);
    FillRR(ox, barY, W, H - S(20), S(14), COL_CARD);
    StrokeRR(ox, barY, W, H - S(20), S(14), COL_BORDER, 1, 1);

    // progress bar (during / after install)
    int tot = g_instTotal.load(), don = g_instDone.load();
    if (tot > 0) {
        float pgT = barY + S(12), pgL = ox + S(16), pgR = W - S(220);
        FillRR(pgL, pgT + S(6), pgR, pgT + S(12), S(3), COL_TRACK);
        float frac = tot ? (float)don / tot : 0;
        if (frac > 0) FillRR(pgL, pgT + S(6), pgL + (pgR - pgL) * frac, pgT + S(12), S(3), g_installing ? COL_ACCENT : COL_GOOD);
        Text(std::to_wstring(don) + L"/" + std::to_wstring(tot), g_fTiny, pgL, pgT - S(4), pgR, pgT + S(4), COL_TEXT_DIM, 1, TXA_LEFT);
    } else {
        Text(std::to_wstring(checked) + L" selected", g_fBody, ox + S(18), barY + S(10), ox + S(200), barY + S(38), COL_TEXT_DIM, 1, TXA_LEFT);
        float sx = ox + S(130);
        bool h1 = false, h2 = false;
        for (auto& h : g_hits) { if (inRect(h.r, g_mouseX, g_mouseY)) { if (h.kind == HIT_SELECTALL) h1 = true; if (h.kind == HIT_CLEAR) h2 = true; } }
        FillRR(sx, barY + S(11), sx + S(88), barY + S(39), S(8), h1 ? COL_ROW_SEL : COL_ROW_HOVER);
        Text(L"Select all", g_fSmall, sx, barY + S(11), sx + S(88), barY + S(39), COL_TEXT, 1, TXA_CENTER);
        g_hits.push_back({ { sx, barY + S(11), sx + S(88), barY + S(39) }, HIT_SELECTALL, 0 });
        float cx2 = sx + S(96);
        FillRR(cx2, barY + S(11), cx2 + S(66), barY + S(39), S(8), h2 ? COL_ROW_SEL : COL_ROW_HOVER);
        Text(L"Clear", g_fSmall, cx2, barY + S(11), cx2 + S(66), barY + S(39), COL_TEXT, 1, TXA_CENTER);
        g_hits.push_back({ { cx2, barY + S(11), cx2 + S(66), barY + S(39) }, HIT_CLEAR, 0 });
    }

    // install button
    float bw = S(196), bh = S(46), bl = W - bw - S(16), bt = barY + S(46);
    bool enabled = checked > 0 && !g_installing;
    bool bhover = false; for (auto& h : g_hits) if (h.kind == HIT_INSTALL_BTN && inRect(h.r, g_mouseX, g_mouseY)) bhover = true;
    FillRR(bl, bt, bl + bw, bt + bh, S(6), enabled ? (bhover ? 0x1A88DE : COL_ACCENT_DK) : COL_TRACK, enabled ? 1 : 0.5f);
    std::wstring label = g_installing ? L"Installing..." : L"Install all checked";
    Text(label, g_fTitle, bl, bt, bl + bw, bt + bh, enabled ? 0xFFFFFF : COL_TEXT_DIM, 1, TXA_CENTER);
    if (enabled) g_hits.push_back({ { bl, bt, bl + bw, bt + bh }, HIT_INSTALL_BTN, 0 });

    Text(GetStatus(), g_fSmall, ox + S(18), barY + S(46), bl - S(20), barY + S(70), COL_TEXT_DIM, 1, TXA_LEFT);
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        float ly = barY + S(70); int shown2 = 0;
        for (auto it = g_log.rbegin(); it != g_log.rend() && shown2 < 3; ++it) {
            if (it->empty()) continue;
            Text(*it, g_fMono, ox + S(18), ly, bl - S(20), ly + S(18), COL_TEXT_FAINT, 1, TXA_LEFT);
            ly += S(17); shown2++;
        }
    }
}

void DrawSettings(float ox, float W, float H) {
    struct CatInfo { const wchar_t* name; const wchar_t* glyph; int accent; };
    static const CatInfo cats[] = {
        { L"Taskbar",             L"\xE7C9", 0x60CDFF },
        { L"File Explorer",       L"\xEC50", 0x4ECB8A },
        { L"Start Menu & Search", L"\xE81D", 0x9B8AFF },
        { L"Appearance",          L"\xE771", 0xF5A623 },
        { L"Window Management",   L"\xE7C4", 0xFF9F6B },
        { L"Privacy",             L"\xE72E", 0xFF6B6B },
        { L"Developer",           L"\xE943", 0xA8CC8C },
        { L"Monitor Settings",    L"\xE7F4", 0x5BD2F0 },
        { L"Mouse Settings",      L"\xE962", 0xCB8CE0 },
    };
    static const int nCats = 9;
    float listTop = S(96), listBottom = H - S(24);

    if (g_settingsCat < 0) {
        // ── Grid view ──────────────────────────────────────────────────────
        DrawHeader(ox, W, L"Windows Settings", L"Choose a category to configure.");
        float gap = S(14), cw = (W - ox - gap) / 2, ch = S(88);
        float startY = listTop + S(4);
        g_rt->PushAxisAlignedClip(D2D1::RectF(ox - S(6), listTop, W + S(24), listBottom), D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < nCats; ++i) {
            int col = i % 2, row = i / 2;
            float l = ox + col * (cw + gap);
            float t = startY + row * (ch + gap) - g_scroll;
            float r = l + cw, b = t + ch;
            if (b < listTop || t > listBottom) continue;
            bool hov = false;
            for (auto& h : g_hits) if (h.kind == HIT_SETTINGS_CAT && h.index == i && inRect(h.r, g_mouseX, g_mouseY)) hov = true;
            FillRR(l, t, r, b, S(12), hov ? COL_CARD_HI : COL_CARD);
            StrokeRR(l, t, r, b, S(12), hov ? COL_BORDER_HI : COL_BORDER, 1, 1);
            float iy = t + (ch - S(40)) / 2;
            FillRRc(l + S(16), iy, l + S(56), iy + S(40), S(10), RGBH(cats[i].accent, 0.16f));
            Text(cats[i].glyph, g_fIconBig, l + S(16), iy, l + S(56), iy + S(40), cats[i].accent, 1, TXA_CENTER);
            int cnt = 0; for (auto& s : g_settings) if (s.category == cats[i].name) cnt++;
            Text(cats[i].name, g_fTitle, l + S(68), t + S(18), r - S(30), t + S(46), COL_TEXT, 1, TXA_LEFT);
            Text(std::to_wstring(cnt) + L" settings", g_fSmall, l + S(68), t + S(46), r - S(30), t + S(70), COL_TEXT_DIM, 1, TXA_LEFT);
            Text(ICON_CHEV, g_fIconSm, r - S(30), t, r, t + ch, COL_TEXT_FAINT, 1, TXA_CENTER);
            g_hits.push_back({ { l, t, r, b }, HIT_SETTINGS_CAT, i });
        }
        g_rt->PopAxisAlignedClip();
        int rows = (nCats + 1) / 2;
        g_scrollMax = std::max(0.0f, startY + rows * (ch + gap) - listBottom + S(8));

    } else {
        // ── Detail view ────────────────────────────────────────────────────
        bool backHov = false;
        for (auto& h : g_hits) if (h.kind == HIT_SETTINGS_BACK && inRect(h.r, g_mouseX, g_mouseY)) backHov = true;
        Text(ICON_CHEV_L, g_fIconSm, ox, S(26), ox + S(22), S(52), backHov ? COL_TEXT : COL_TEXT_DIM, 1, TXA_CENTER);
        Text(L"Windows Settings", g_fSmall, ox + S(22), S(26), ox + S(220), S(52), backHov ? COL_TEXT : COL_TEXT_DIM, 1, TXA_LEFT);
        g_hits.push_back({ { ox, S(26), ox + S(220), S(54) }, HIT_SETTINGS_BACK, 0 });
        Text(cats[g_settingsCat].name, g_fH1, ox, S(54), W, S(84), COL_TEXT, 1, TXA_LEFT);

        g_rt->PushAxisAlignedClip(D2D1::RectF(ox - S(6), listTop, W + S(24), listBottom), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = listTop + S(8) - g_scroll, rowH = S(64);

        // ── Per-monitor hardware cards (Monitor Settings only, cats index 7) ─────────────
        if (g_settingsCat == 7) {
            static const wchar_t* oriLabels[] = { L"Landscape", L"Portrait", L"Landscape (flipped)", L"Portrait (flipped)" };
            static const int mapColors5[] = { 0x5BD2F0, 0x6CCB5F, 0xF5A623, 0xCB8CE0, 0xFF6B6B };

            // ── Improved mini-map ─────────────────────────────────────────────
            if (!g_monitors.empty()) {
                float mapH=S(132), mapPad=S(14), mapAreaW=W-ox;
                if (y+mapH>listTop && y<listBottom) {
                    int minX=INT_MAX,minY=INT_MAX,maxX=INT_MIN,maxY=INT_MIN;
                    for (auto& m:g_monitors) {
                        if (m.posX<minX) minX=m.posX; if (m.posY<minY) minY=m.posY;
                        int rx=m.posX+m.width, ry=m.posY+m.height;
                        if (rx>maxX) maxX=rx; if (ry>maxY) maxY=ry;
                    }
                    int totW=maxX-minX, totH=maxY-minY;
                    if (totW<1) totW=1; if (totH<1) totH=1;
                    float labH=S(24), avW=mapAreaW-mapPad*2, avH=mapH-labH-mapPad*2;
                    float scl=std::min(avW/(float)totW, avH/(float)totH);
                    float offX=ox+mapPad+(avW-totW*scl)*0.5f, offY=y+labH+mapPad+(avH-totH*scl)*0.5f;
                    FillRR(ox,y,W,y+mapH,S(10),0x161616);
                    StrokeRR(ox,y,W,y+mapH,S(10),COL_BORDER,1,1);
                    Text(L"Monitor Layout",g_fTiny,ox+S(14),y+S(7),W,y+S(21),COL_TEXT_FAINT,1,TXA_LEFT);
                    for (int mi2=0;mi2<(int)g_monitors.size();mi2++) {
                        auto& m=g_monitors[mi2]; int col=mapColors5[mi2%5];
                        float rml=offX+(m.posX-minX)*scl, rmt=offY+(m.posY-minY)*scl;
                        float rmr=rml+m.width*scl, rmb=rmt+m.height*scl;
                        float rrw=rmr-rml, rrh=rmb-rmt;
                        // Fill + border (primary gets brighter border)
                        FillRR(rml,rmt,rmr,rmb,S(4),col,0.14f);
                        StrokeRR(rml,rmt,rmr,rmb,S(4),col,m.isPrimary?1.0f:0.65f,m.isPrimary?2.0f:1.0f);
                        // Number centered
                        Text(std::to_wstring(mi2+1),g_fSmall,rml,rmt,rmr,rmb,col,1,TXA_CENTER);
                        // Star for primary (top-right corner)
                        if (m.isPrimary && rrw>S(16) && rrh>S(14))
                            Text(L"\xE735",g_fIconSm,rmr-S(16),rmt+S(1),rmr-S(1),rmt+S(15),0xFFCC44,1,TXA_CENTER);
                        // Resolution hint below number if enough space
                        if (rrh>S(30)) {
                            int mn=std::min(m.width,m.height);
                            std::wstring rs=mn>=2160?L"4K":mn>=1440?L"1440p":mn>=1080?L"1080p":mn>=720?L"720p":(std::to_wstring(mn)+L"p");
                            Text(rs,g_fTiny,rml,rmt+rrh*0.52f,rmr,rmb,col,0.75f,TXA_CENTER);
                        }
                    }
                }
                y += mapH+S(10);
            }

            // ── Section label ───────────────────────────────────────────
            if (y+S(22)>listTop && y<listBottom)
                Text(L"Connected Monitors",g_fTiny,ox+S(4),y+S(4),W,y+S(22),COL_TEXT_FAINT,1,TXA_LEFT);
            y += S(28);

            // ── Monitor cards with dropdown controls ───────────────────────
            float mch=S(134), mgap=S(10), half=ox+(W-ox)*0.5f;
            for (int mi=0; mi<(int)g_monitors.size(); ++mi) {
                auto& mon=g_monitors[mi];
                float t=y, b=t+mch;
                if (b<listTop||t>listBottom) { y+=mch+mgap; continue; }
                int accentCol=mapColors5[mi%5];
                FillRR(ox,t,W,b,S(12),COL_CARD); StrokeRR(ox,t,W,b,S(12),COL_BORDER,1,1);
                // Badge
                FillRRc(ox+S(14),t+S(12),ox+S(38),t+S(36),S(7),RGBH(accentCol,0.18f));
                Text(std::to_wstring(mi+1),g_fSmall,ox+S(14),t+S(12),ox+S(38),t+S(36),accentCol,1,TXA_CENTER);
                // Primary badge
                if (mon.isPrimary) { FillRR(ox+S(46),t+S(16),ox+S(100),t+S(32),S(7),COL_GOOD,0.15f); Text(L"Primary",g_fTiny,ox+S(46),t+S(16),ox+S(100),t+S(32),COL_GOOD,1,TXA_CENTER); }
                // Name
                Text(mon.friendlyName,g_fTitle,mon.isPrimary?ox+S(108):ox+S(46),t+S(12),W-S(14),t+S(36),COL_TEXT,1,TXA_LEFT);
                // Specs
                std::wstring specLine=std::to_wstring(mon.width)+L"\u00D7"+std::to_wstring(mon.height)+L"  \u00B7  "+std::to_wstring(mon.refreshRate)+L" Hz  \u00B7  "+oriLabels[mon.orientation&3];
                Text(specLine,g_fSmall,ox+S(14),t+S(38),W-S(14),t+S(54),COL_TEXT_DIM,1,TXA_LEFT);
                // Separator
                g_brush->SetColor(RGBH(COL_BORDER)); g_rt->DrawLine({ox+S(14),t+S(60)},{W-S(14),t+S(60)},g_brush.Get(),1.0f);

                // Selector row
                float selY1=t+S(66), selY2=selY1+S(26);
                // Res dropdown
                Text(L"Res",g_fSmall,ox+S(14),selY1,ox+S(38),selY2,COL_TEXT_FAINT,1,TXA_LEFT);
                {
                    float ddL=ox+S(42), ddR=half-S(8);
                    bool isOpen=(g_ddOpen==mi*2); bool ddHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_DD_TOGGLE&&h2.index==mi*2&&inRect(h2.r,g_mouseX,g_mouseY)) ddHov=true;
                    FillRR(ddL,selY1,ddR,selY2,S(6),(isOpen||ddHov)?COL_ROW_SEL:COL_ROW_HOVER);
                    StrokeRR(ddL,selY1,ddR,selY2,S(6),isOpen?COL_ACCENT:COL_BORDER,1,1);
                    std::wstring rv=mon.availRes.empty()?std::to_wstring(mon.width)+L"\u00D7"+std::to_wstring(mon.height)
                        :std::to_wstring(mon.availRes[mon.selResIdx].first)+L"\u00D7"+std::to_wstring(mon.availRes[mon.selResIdx].second);
                    Text(rv,g_fSmall,ddL+S(10),selY1,ddR-S(22),selY2,COL_TEXT,1,TXA_LEFT);
                    Text(L"\xE70D",g_fIconSm,ddR-S(22),selY1,ddR-S(2),selY2,isOpen?COL_ACCENT:COL_TEXT_FAINT,1,TXA_CENTER);
                    g_hits.push_back({{ddL,selY1,ddR,selY2},HIT_DD_TOGGLE,mi*2});
                    if (isOpen) { g_ddPopX=ddL; g_ddPopW=ddR-ddL; g_ddBtnT=selY1; g_ddBtnB=selY2; }
                }
                // Divider
                g_brush->SetColor(RGBH(COL_BORDER)); g_rt->DrawLine({half,selY1+S(4)},{half,selY2-S(4)},g_brush.Get(),1.0f);
                // Hz dropdown
                Text(L"Hz",g_fSmall,half+S(10),selY1,half+S(30),selY2,COL_TEXT_FAINT,1,TXA_LEFT);
                {
                    float ddL=half+S(34), ddR=W-S(14);
                    bool isOpen=(g_ddOpen==mi*2+1); bool ddHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_DD_TOGGLE&&h2.index==mi*2+1&&inRect(h2.r,g_mouseX,g_mouseY)) ddHov=true;
                    FillRR(ddL,selY1,ddR,selY2,S(6),(isOpen||ddHov)?COL_ROW_SEL:COL_ROW_HOVER);
                    StrokeRR(ddL,selY1,ddR,selY2,S(6),isOpen?COL_ACCENT:COL_BORDER,1,1);
                    std::wstring hv=mon.availHz.empty()?std::to_wstring(mon.refreshRate)+L" Hz":std::to_wstring(mon.availHz[mon.selHzIdx])+L" Hz";
                    Text(hv,g_fSmall,ddL+S(10),selY1,ddR-S(22),selY2,COL_TEXT,1,TXA_LEFT);
                    Text(L"\xE70D",g_fIconSm,ddR-S(22),selY1,ddR-S(2),selY2,isOpen?COL_ACCENT:COL_TEXT_FAINT,1,TXA_CENTER);
                    g_hits.push_back({{ddL,selY1,ddR,selY2},HIT_DD_TOGGLE,mi*2+1});
                    if (isOpen) { g_ddPopX=ddL; g_ddPopW=ddR-ddL; g_ddBtnT=selY1; g_ddBtnB=selY2; }
                }

                // Action buttons
                float bbY=b-S(40), bh=S(26);
                {   bool apHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_MONITOR_APPLY&&h2.index==mi&&inRect(h2.r,g_mouseX,g_mouseY)) apHov=true;
                    float abl=ox+S(14),abr=abl+S(72);
                    FillRR(abl,bbY,abr,bbY+bh,S(7),COL_ACCENT_DK,apHov?1.0f:0.22f);
                    StrokeRR(abl,bbY,abr,bbY+bh,S(7),apHov?COL_ACCENT:COL_BORDER,1,1);
                    Text(L"Apply",g_fSmall,abl,bbY,abr,bbY+bh,apHov?COL_TEXT:COL_ACCENT,1,TXA_CENTER);
                    g_hits.push_back({{abl,bbY,abr,bbY+bh},HIT_MONITOR_APPLY,mi}); }
                {   bool rHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_MONITOR_ROTATE&&h2.index==mi&&inRect(h2.r,g_mouseX,g_mouseY)) rHov=true;
                    float rbl=ox+S(96),rbr=rbl+S(72);
                    FillRR(rbl,bbY,rbr,bbY+bh,S(7),rHov?COL_ROW_SEL:COL_ROW_HOVER);
                    StrokeRR(rbl,bbY,rbr,bbY+bh,S(7),COL_BORDER,1,1);
                    Text(L"Rotate",g_fSmall,rbl,bbY,rbr,bbY+bh,COL_TEXT,1,TXA_CENTER);
                    g_hits.push_back({{rbl,bbY,rbr,bbY+bh},HIT_MONITOR_ROTATE,mi}); }
                if (!mon.isPrimary) {
                    bool pHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_MONITOR_PRIMARY&&h2.index==mi&&inRect(h2.r,g_mouseX,g_mouseY)) pHov=true;
                    float pbl=ox+S(178),pbr=pbl+S(96);
                    FillRR(pbl,bbY,pbr,bbY+bh,S(7),pHov?COL_ROW_SEL:COL_ROW_HOVER);
                    StrokeRR(pbl,bbY,pbr,bbY+bh,S(7),COL_BORDER,1,1);
                    Text(L"Set Primary",g_fSmall,pbl,bbY,pbr,bbY+bh,COL_TEXT,1,TXA_CENTER);
                    g_hits.push_back({{pbl,bbY,pbr,bbY+bh},HIT_MONITOR_PRIMARY,mi}); }
                y += mch+mgap;
            }
            if (g_monitors.empty()) {
                Text(L"No monitors detected",g_fBody,ox,y+S(10),W,y+S(40),COL_TEXT_FAINT,1,TXA_LEFT);
                y += S(50);
            }
            y += S(6);
            if (y+S(22)>listTop && y<listBottom)
                Text(L"Display Settings",g_fTiny,ox+S(4),y+S(4),W,y+S(22),COL_TEXT_FAINT,1,TXA_LEFT);
            y += S(28);
        }

        // ── Registry toggles ────────────────────────────────────────────────────
        for (size_t i = 0; i < g_settings.size(); ++i) {
            auto& s = g_settings[i];
            if (s.category != cats[g_settingsCat].name) continue;
            float rt = y, rb = y + rowH - S(8);
            if (rb > listTop && rt < listBottom) {
                FillRR(ox, rt, W, rb, S(12), s.hover > 0.01f ? COL_CARD_HI : COL_CARD);
                if (s.hover > 0.01f) StrokeRR(ox, rt, W, rb, S(12), COL_BORDER_HI, s.hover, 1);
                std::wstring title = s.title + (s.adminReq ? L"   · admin" : L"");
                Text(title, g_fTitle, ox + S(18), rt + S(9), W - S(80), rt + S(33), (s.adminReq && !g_isAdmin) ? COL_TEXT_DIM : COL_TEXT, 1, TXA_LEFT);
                Text(s.desc, g_fSmall, ox + S(18), rt + S(32), W - S(80), rt + S(54), COL_TEXT_DIM, 1, TXA_LEFT);
                DrawToggle(W - S(62), rt + (rowH - S(8)) / 2 - S(11), s.toggle);
                if (rt >= listTop - S(2) && rb <= listBottom + S(2))
                    g_hits.push_back({ { ox, rt, W, rb }, HIT_SETTING_ROW, (int)i });
            }
            y += rowH;
        }
        g_rt->PopAxisAlignedClip();
        g_scrollMax = std::max(0.0f, (y + g_scroll) - listBottom + S(8));
        // Dropdown popup overlay – drawn outside scroll clip, floats on top
        if (g_ddOpen >= 0 && g_settingsCat == 7) {
            int monIdx=g_ddOpen/2, ddType=g_ddOpen%2;
            if (monIdx<(int)g_monitors.size() && g_ddBtnB>listTop && g_ddBtnB<listBottom+S(50)) {
                auto& mon=g_monitors[monIdx];
                int nItems=(ddType==0)?(int)mon.availRes.size():(int)mon.availHz.size();
                int selIdx=(ddType==0)?mon.selResIdx:mon.selHzIdx;
                float itemH=S(30), popH=nItems*itemH+S(8);
                float popY=g_ddBtnB+S(3);
                if (popY+popH > H-S(24)) popY=g_ddBtnT-S(4)-popH;
                if (popY < S(32)) popY=S(32);
                FillRR(g_ddPopX,popY,g_ddPopX+g_ddPopW,popY+popH,S(8),0x1C1C1C);
                StrokeRR(g_ddPopX,popY,g_ddPopX+g_ddPopW,popY+popH,S(8),COL_ACCENT,0.65f,1.5f);
                for (int i=0; i<nItems; ++i) {
                    float iy=popY+S(4)+i*itemH, ib=iy+itemH-S(2);
                    bool isSel=(i==selIdx), isHov=false;
                    for (auto& h2:g_hits) if (h2.kind==HIT_DD_ITEM&&h2.index==i&&inRect(h2.r,g_mouseX,g_mouseY)) isHov=true;
                    if (isSel) FillRRc(g_ddPopX+S(5),iy,g_ddPopX+g_ddPopW-S(5),ib,S(5),RGBH(COL_ACCENT,0.14f));
                    else if (isHov) FillRR(g_ddPopX+S(5),iy,g_ddPopX+g_ddPopW-S(5),ib,S(5),COL_ROW_HOVER);
                    std::wstring itxt;
                    if (ddType==0&&i<(int)mon.availRes.size())
                        itxt=std::to_wstring(mon.availRes[i].first)+L"\u00D7"+std::to_wstring(mon.availRes[i].second);
                    else if (ddType==1&&i<(int)mon.availHz.size())
                        itxt=std::to_wstring(mon.availHz[i])+L" Hz";
                    Text(itxt,g_fSmall,g_ddPopX+S(14),iy,g_ddPopX+g_ddPopW-S(28),ib,isSel?COL_ACCENT:COL_TEXT,1,TXA_LEFT);
                    if (isSel) Text(L"\xE73E",g_fIconSm,g_ddPopX+g_ddPopW-S(26),iy,g_ddPopX+g_ddPopW-S(4),ib,COL_ACCENT,1,TXA_CENTER);
                    g_hits.push_back({{g_ddPopX,iy,g_ddPopX+g_ddPopW,iy+itemH},HIT_DD_ITEM,i});
                }
            }
        }
    }
}

void DrawMisc(float ox, float W, float H) {
    DrawHeader(ox, W, L"Misc", L"Quick maintenance tools and shortcuts.");
    float listTop = S(96), listBottom = H - S(24);
    g_rt->PushAxisAlignedClip(D2D1::RectF(ox - S(6), listTop, W + S(24), listBottom), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = listTop + S(8) - g_scroll, rowH = S(70);
    for (size_t i = 0; i < g_misc.size(); ++i) {
        auto& m = g_misc[i];
        float rt = y, rb = y + rowH - S(10);
        if (rb > listTop && rt < listBottom) {
            FillRR(ox, rt, W, rb, S(12), m.hover > 0.01f ? COL_CARD_HI : COL_CARD);
            if (m.hover > 0.01f) StrokeRR(ox, rt, W, rb, S(12), COL_BORDER_HI, m.hover, 1);
            Text(m.title, g_fTitle, ox + S(18), rt + S(11), W - S(140), rt + S(35), COL_TEXT, 1, TXA_LEFT);
            Text(m.desc, g_fSmall, ox + S(18), rt + S(33), W - S(140), rt + S(55), COL_TEXT_DIM, 1, TXA_LEFT);
            float bw = S(84), bh = S(34), bl = W - bw - S(16), bt = rt + (rowH - S(10)) / 2 - bh / 2;
            FillRR(bl, bt, bl + bw, bt + bh, S(9), m.btn > 0.5f ? COL_ROW_SEL : COL_ROW_HOVER);
            StrokeRR(bl, bt, bl + bw, bt + bh, S(9), COL_BORDER, 1, 1);
            Text(L"Run", g_fSmall, bl, bt, bl + bw, bt + bh, COL_TEXT, 1, TXA_CENTER);
            if (bt >= listTop && bt + bh <= listBottom)
                g_hits.push_back({ { bl, bt, bl + bw, bt + bh }, HIT_MISC_BTN, (int)i });
        }
        y += rowH;
    }
    g_rt->PopAxisAlignedClip();
    g_scrollMax = std::max(0.0f, (y + g_scroll) - listBottom + S(8));
}

void OnRender() {
    if (FAILED(CreateDeviceResources())) return;
    g_hits.clear();
    g_rt->BeginDraw();
    g_rt->Clear(RGBH(COL_BG));
    D2D1_SIZE_F sz = g_rt->GetSize();
    float W = sz.width, H = sz.height;
    DrawSidebar(W, H);
    float ox = S(NAVW) + S(32), contentRight = W - S(30);
    float slide = (1.0f - g_pageAnim) * S(22);
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(slide, 0));
    switch (g_page) {
        case PAGE_HOME:       DrawHome(ox, contentRight, H); break;
        case PAGE_INSTALLERS: DrawInstallers(ox, contentRight, H); break;
        case PAGE_SETTINGS:   DrawSettings(ox, contentRight, H); break;
        case PAGE_MISC:       DrawMisc(ox, contentRight, H); break;
        default: break;
    }
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = g_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) DiscardDeviceResources();
}

// ---------------------------------------------------------------------------
void Tick() {
    g_now = NowSeconds();
    for (auto& it : g_installers) it.check = ease(it.check, it.checked ? 1.0f : 0.0f, 0.25f);
    for (auto& s : g_settings)   s.toggle = ease(s.toggle, s.on ? 1.0f : 0.0f, 0.25f);
    g_pageAnim = ease(g_pageAnim, 1.0f, 0.18f);
    g_scrollTarget = clampf(g_scrollTarget, 0, g_scrollMax);
    g_scroll = ease(g_scroll, g_scrollTarget, 0.22f);
#ifdef DEBUG_TITLE
    static wchar_t last[160] = L"";
    size_t logn; { std::lock_guard<std::mutex> lk(g_logMutex); logn = g_log.size(); }
    wchar_t t[160];
    swprintf(t, 160, L"EI [page=%d checked=%d installing=%d done=%d/%d log=%zu search=%s]",
             (int)g_page, CountChecked(), (int)g_installing.load(), g_instDone.load(), g_instTotal.load(),
             logn, g_search.c_str());
    if (wcscmp(t, last) != 0) { wcscpy_s(last, t); SetWindowTextW(g_hwnd, t); }
#endif
}
void UpdateHovers() {
    for (auto& it : g_installers) it.hover = ease(it.hover, 0.0f, 0.2f);
    for (auto& s : g_settings)   s.hover  = ease(s.hover, 0.0f, 0.2f);
    for (auto& m : g_misc)       { m.hover = ease(m.hover, 0.0f, 0.2f); m.btn = ease(m.btn, 0.0f, 0.25f); }
    for (int i = 0; i < PAGE_COUNT; ++i) g_navHover[i] = ease(g_navHover[i], 0.0f, 0.2f);
    g_searchHover = ease(g_searchHover, 0.0f, 0.2f);
    g_cursor = 0;
    for (auto& h : g_hits) {
        if (!inRect(h.r, g_mouseX, g_mouseY)) continue;
        if (h.kind == HIT_SEARCH) { g_searchHover = ease(g_searchHover, 1.0f, 0.4f); g_cursor = 2; }
        else g_cursor = 1;
        switch (h.kind) {
            case HIT_NAV: g_navHover[h.index] = ease(g_navHover[h.index], 1.0f, 0.4f); break;
            case HIT_INSTALLER_ROW: g_installers[h.index].hover = ease(g_installers[h.index].hover, 1.0f, 0.35f); break;
            case HIT_SETTING_ROW: g_settings[h.index].hover = ease(g_settings[h.index].hover, 1.0f, 0.35f); break;
            case HIT_MISC_BTN: g_misc[h.index].btn = ease(g_misc[h.index].btn, 1.0f, 0.4f); break;
        }
    }
}

void GoPage(Page p) { if (p == g_page) return; g_page = p; g_pageAnim = 0.0f; g_scroll = g_scrollTarget = 0.0f; g_searchFocus = false; g_settingsCat = -1; g_ddOpen = -1; }

void OnClick(int x, int y) {
    // Dropdown has highest priority: when open, only DD hits are processed
    if (g_ddOpen >= 0) {
        for (auto& h : g_hits) {
            if (!inRect(h.r, x, y)) continue;
            if (h.kind == HIT_DD_TOGGLE) { g_ddOpen = (g_ddOpen==h.index) ? -1 : h.index; return; }
            if (h.kind == HIT_DD_ITEM) {
                int monIdx=g_ddOpen/2, ddType=g_ddOpen%2;
                if (monIdx<(int)g_monitors.size()) {
                    if (ddType==0) { g_monitors[monIdx].selResIdx=h.index; UpdateMonitorHz(monIdx); }
                    else g_monitors[monIdx].selHzIdx=h.index;
                }
                g_ddOpen=-1; return;
            }
        }
        g_ddOpen=-1; return; // click outside -> close
    }
    bool hitSearch = false;
    for (auto& h : g_hits) {
        if (!inRect(h.r, x, y)) continue;
        switch (h.kind) {
        case HIT_NAV: GoPage((Page)h.index); return;
        case HIT_HOME_CARD: GoPage((Page)h.index); return;
        case HIT_ADMIN_BTN: RelaunchAsAdmin(); return;
        case HIT_SEARCH: g_searchFocus = true; hitSearch = true; return;
        case HIT_INSTALLER_ROW: g_installers[h.index].checked = !g_installers[h.index].checked; g_installers[h.index].status = ST_NONE; return;
        case HIT_CATEGORY: {
            std::wstring cat = g_installers[h.index].category;
            int inCat = 0, chk = 0;
            for (auto& o : g_installers) if (o.category == cat && MatchesSearch(o)) { inCat++; if (o.checked) chk++; }
            bool selectAll = chk != inCat;
            for (auto& o : g_installers) if (o.category == cat && MatchesSearch(o)) o.checked = selectAll;
            return; }
        case HIT_SELECTALL: for (auto& it : g_installers) if (MatchesSearch(it)) it.checked = true; return;
        case HIT_CLEAR: for (auto& it : g_installers) it.checked = false; return;
        case HIT_INSTALL_BTN: {
            if (g_installing) return;
            std::vector<int> idxs;
            for (size_t i = 0; i < g_installers.size(); ++i) if (g_installers[i].checked) idxs.push_back((int)i);
            if (idxs.empty()) return;
            { std::lock_guard<std::mutex> lk(g_logMutex); g_log.clear(); }
            std::thread(InstallThread, idxs).detach();
            return; }
        case HIT_SETTINGS_CAT: g_settingsCat = h.index; g_scroll = g_scrollTarget = 0.0f; g_ddOpen = -1; if (h.index == 7) ScanMonitors(); return;
        case HIT_SETTINGS_BACK: g_settingsCat = -1; g_scroll = g_scrollTarget = 0.0f; g_ddOpen = -1; return;
        case HIT_MONITOR_ROTATE:  SetMonitorOrientation(h.index); return;
        case HIT_MONITOR_PRIMARY: SetMonitorPrimary(h.index); return;
        case HIT_MONITOR_RES_PREV:
            if (h.index < (int)g_monitors.size() && g_monitors[h.index].selResIdx > 0)
                { g_monitors[h.index].selResIdx--; UpdateMonitorHz(h.index); } return;
        case HIT_MONITOR_RES_NEXT:
            if (h.index < (int)g_monitors.size() && g_monitors[h.index].selResIdx+1 < (int)g_monitors[h.index].availRes.size())
                { g_monitors[h.index].selResIdx++; UpdateMonitorHz(h.index); } return;
        case HIT_MONITOR_HZ_PREV:
            if (h.index < (int)g_monitors.size() && g_monitors[h.index].selHzIdx > 0)
                g_monitors[h.index].selHzIdx--; return;
        case HIT_MONITOR_HZ_NEXT:
            if (h.index < (int)g_monitors.size() && g_monitors[h.index].selHzIdx+1 < (int)g_monitors[h.index].availHz.size())
                g_monitors[h.index].selHzIdx++; return;
        case HIT_MONITOR_APPLY: ApplyMonitorMode(h.index); return;
        case HIT_DD_TOGGLE: g_ddOpen = h.index; return;
        case HIT_SETTING_ROW: {
            auto& s = g_settings[h.index];
            if (s.adminReq && !g_isAdmin) { SetStatus(L"That setting needs admin – click \"Run as admin\"."); return; }
            bool target = !s.on; DWORD v = target ? s.onValue : s.offValue; bool ok = true;
            for (auto& n : s.names) ok = RegWriteDword(s.root, s.subkey, n, v) && ok;
            if (ok) { s.on = target; SetStatus(s.title + (target ? L": On" : L": Off")); }
            else SetStatus(L"Couldn't change " + s.title + L" (need admin?)");
            return; }
        case HIT_MISC_BTN: {
            auto& m = g_misc[h.index];
            if (!m.confirm.empty() && MessageBoxW(g_hwnd, m.confirm.c_str(), L"Confirm", MB_YESNO | MB_ICONWARNING) != IDYES) return;
            RunMisc(m); return; }
        }
    }
    if (!hitSearch) g_searchFocus = false;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        int corner = 2 /*DWMWCP_ROUND*/;
        DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
        COLORREF cap = RGB(0x1C, 0x1C, 0x1C);
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
        return 0; }
    case WM_MOUSEMOVE: {
        g_mouseX = GET_X_LPARAM(lp); g_mouseY = GET_Y_LPARAM(lp);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&tme);
        return 0; }
    case WM_MOUSELEAVE: g_mouseX = g_mouseY = -1; return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, g_cursor == 1 ? IDC_HAND : g_cursor == 2 ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: SetFocus(hwnd); OnClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
    case WM_CHAR:
        if (g_searchFocus) {
            wchar_t c = (wchar_t)wp;
            if (c == 8) { if (!g_search.empty()) g_search.pop_back(); }
            else if (c == 27) { g_search.clear(); g_searchFocus = false; }
            else if (c == 13) { g_searchFocus = false; }
            else if (c >= 32) g_search.push_back(c);
        }
        return 0;
    case WM_MOUSEWHEEL:
        g_scrollTarget = clampf(g_scrollTarget - GET_WHEEL_DELTA_WPARAM(wp) * 0.55f, 0, g_scrollMax);
        g_ddOpen = -1;
        return 0;
    case WM_SIZE: if (g_rt) { RECT rc; GetClientRect(hwnd, &rc); g_rt->Resize(D2D1::SizeU(rc.right, rc.bottom)); } return 0;
    case WM_DPICHANGED: {
        g_scale = HIWORD(wp) / 96.0f;
        g_fBig.Reset(); g_fH1.Reset(); g_fTitle.Reset(); g_fBody.Reset(); g_fSmall.Reset();
        g_fMono.Reset(); g_fNav.Reset(); g_fIcon.Reset(); g_fTiny.Reset();
        g_fIconNav.Reset(); g_fIconBig.Reset(); g_fIconSm.Reset();
        CreateTextFormats();
        RECT* pr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0; }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); OnRender(); EndPaint(hwnd, &ps); return 0; }
    case WM_GETMINMAXINFO: { MINMAXINFO* mmi = (MINMAXINFO*)lp; mmi->ptMinTrackSize.x = 900; mmi->ptMinTrackSize.y = 600; return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmd) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_isAdmin = CheckIsAdmin();
    SeedData();
    LoadIconSources();

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2dFactory.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)g_dwrite.GetAddressOf());

    HICON appIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"EasyInstallerWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = nullptr;
    wc.hIcon = appIcon;
    RegisterClassW(&wc);

    g_scale = GetDpiForSystem() / 96.0f;
    CreateTextFormats();

    int w = (int)(1000 * g_scale), h = (int)(700 * g_scale);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Easy Setup", WS_OVERLAPPEDWINDOW, sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (appIcon) { SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon); SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon); }

    ShowWindow(g_hwnd, nCmd); UpdateWindow(g_hwnd);
    ScanMonitors();
    std::thread(ScanInstalledThread).detach();

    timeBeginPeriod(1);  // higher timer/sleep resolution

    // Continuous render loop paced to the display's refresh via DwmFlush().
    // Animation is advanced by real elapsed time (g_dt) so it stays smooth at any
    // refresh rate (60 / 120 / 144 Hz ...) instead of judder from a fixed timer.
    MSG msg; bool running = true;
    double last = NowSeconds();
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!running) break;

        if (IsIconic(g_hwnd)) { Sleep(12); last = NowSeconds(); continue; }

        double now = NowSeconds();
        g_dt = clampf((float)(now - last), 0.0f, 0.1f);
        last = now;

        UpdateHovers();
        Tick();
        OnRender();

        // Block until the compositor's next vertical blank -> vsync-paced, tear-free.
        if (FAILED(DwmFlush())) Sleep(6);
    }

    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
