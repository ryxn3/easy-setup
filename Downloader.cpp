// Downloader.cpp — small standalone installer-style wizard for Easy Setup.
// Custom borderless window with its own title bar, a multi-page flow
// (Welcome -> Choose location -> Installing -> Done/Error), animated
// gradient accents, and a Program Files default (falls back to asking for
// admin only if that location actually needs it).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>
#include <cmath>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;

static const wchar_t* kDownloadUrl = L"https://ryxn3.github.io/easy-setup/EasySetup.exe";
static const wchar_t* kDefaultName = L"EasySetup.exe";
static const wchar_t* kDefaultSubfolder = L"Easy Setup";
static const int kWinW = 460, kWinH = 340;
static const int kTitleBarH = 42;
static const int kDownloadTimeoutMs = 15000;

// ---- Fluent-dark palette, matching the website ----
static const Color kBg(0xFF, 0x1c, 0x1c, 0x1c);
static const Color kTitleBg(0xFF, 0x14, 0x14, 0x14);
static const Color kSurface(0xFF, 0x2b, 0x2b, 0x2b);
static const Color kSurface3(0xFF, 0x32, 0x32, 0x32);
static const Color kBorder(0xFF, 0x38, 0x38, 0x38);
static const Color kText(0xFF, 0xf3, 0xf3, 0xf3);
static const Color kTextDim(0xFF, 0xa8, 0xa8, 0xa8);
static const Color kAccent(0xFF, 0x60, 0xcd, 0xff);
static const Color kAccentInk(0xFF, 0x0a, 0x1a, 0x22);
static const Color kGood(0xFF, 0x6c, 0xcb, 0x5f);
static const Color kBad(0xFF, 0xff, 0x8a, 0x8a);

static Color HSLtoRGB(float h, float s, float l, BYTE alpha = 255) {
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f);
    float c = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = l - c / 2.0f;
    float r = 0, g = 0, b = 0;
    if (h < 60)      { r = c; g = x; b = 0; }
    else if (h < 120){ r = x; g = c; b = 0; }
    else if (h < 180){ r = 0; g = c; b = x; }
    else if (h < 240){ r = 0; g = x; b = c; }
    else if (h < 300){ r = x; g = 0; b = c; }
    else             { r = c; g = 0; b = x; }
    return Color(alpha, (BYTE)((r + m) * 255), (BYTE)((g + m) * 255), (BYTE)((b + m) * 255));
}

enum Page { PAGE_WELCOME, PAGE_CHOOSE, PAGE_INSTALLING, PAGE_DONE, PAGE_ERROR };

static const UINT WM_APP_DONE  = WM_APP + 2;
static const UINT WM_APP_ERROR = WM_APP + 3;
static const UINT_PTR kAnimTimerId = 1; // ambient color + progress sweep + page transition, all ~16ms while active

static HWND g_hwnd = nullptr;
static Page g_page = PAGE_WELCOME;
static Page g_prevPage = PAGE_WELCOME;
static ULONGLONG g_transitionStart = 0;
static bool g_transitioning = false;
static const int kTransitionMs = 320;

static std::wstring g_destPath;
static std::wstring g_errorText;
static bool g_errorNeedsAdmin = false;
static ULONGLONG g_downloadStartTick = 0;
static volatile LONG g_downloadGeneration = 0;
static float g_colorPhase = 0.0f;      // ambient hue rotation, degrees
static float g_progressPhase = 0.0f;   // 0..1 ping-pong for the installing sweep
static bool g_relaunchedElevated = false;

static RECT g_rcClose{}, g_rcPrimary{}, g_rcSecondary{}, g_rcBrowse{};
static bool g_hoverClose = false, g_hoverPrimary = false, g_hoverSecondary = false, g_hoverBrowse = false;

static bool PtInRect2(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static void Repaint() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

static void EnsureAnimTimer() { SetTimer(g_hwnd, kAnimTimerId, 16, nullptr); }

// ============================================================
// Paths
// ============================================================
static std::wstring JoinPath(const std::wstring& folder, const std::wstring& name) {
    if (!folder.empty() && folder.back() != L'\\') return folder + L'\\' + name;
    return folder + name;
}

static std::wstring ProgramFilesDefaultPath() {
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path))) {
        folder = path;
        CoTaskMemFree(path);
    } else {
        wchar_t buf[MAX_PATH];
        GetTempPathW(MAX_PATH, buf);
        folder = buf;
    }
    return JoinPath(JoinPath(folder, kDefaultSubfolder), kDefaultName);
}

// Creates the parent directory (if needed) and confirms we can actually write
// there, without touching the network. This lets us show the "needs admin"
// page instantly instead of only discovering it after a failed download.
static bool EnsureDirectoryWritable(const std::wstring& dir) {
    int result = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS) {
        return false;
    }
    std::wstring probe = JoinPath(dir, L".es_write_test");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static std::wstring DirOf(const std::wstring& filePath) {
    size_t slash = filePath.find_last_of(L'\\');
    return slash != std::wstring::npos ? filePath.substr(0, slash) : filePath;
}

// ============================================================
// Download (kept exactly as the proven-correct version: the download
// thread gets its own COM apartment, no custom IBindStatusCallback, a
// watchdog timeout, and a generation guard so a stale/timed-out attempt
// can never clobber a newer one).
// ============================================================
static DWORD WINAPI DownloadThread(LPVOID param) {
    LONG myGeneration = (LONG)(LONG_PTR)param;

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = URLDownloadToFileW(nullptr, kDownloadUrl, g_destPath.c_str(), 0, nullptr);
    if (SUCCEEDED(coHr)) CoUninitialize();

    if (InterlockedCompareExchange(&g_downloadGeneration, 0, 0) != myGeneration) return 0;

    if (SUCCEEDED(hr)) {
        PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)myGeneration, 0);
    } else {
        auto* msg = new std::wstring(L"Couldn't reach the download server. Check your connection and try again.");
        PostMessageW(g_hwnd, WM_APP_ERROR, (WPARAM)myGeneration, (LPARAM)msg);
    }
    return 0;
}

static void GoToPage(Page next);

static void StartDownload() {
    if (!EnsureDirectoryWritable(DirOf(g_destPath))) {
        g_errorNeedsAdmin = true;
        g_errorText = L"Administrator permission is needed to install here.";
        GoToPage(PAGE_ERROR);
        return;
    }
    g_errorNeedsAdmin = false;
    g_downloadStartTick = GetTickCount64();
    LONG generation = InterlockedIncrement(&g_downloadGeneration);
    EnsureAnimTimer();
    GoToPage(PAGE_INSTALLING);
    HANDLE t = CreateThread(nullptr, 0, DownloadThread, (LPVOID)(LONG_PTR)generation, 0, nullptr);
    if (t) CloseHandle(t);
}

static void RelaunchElevated() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring args = L"\"" + g_destPath + L"\"";
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", exePath, args.c_str(), nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)r > 32) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    // If the user cancels the UAC prompt, ShellExecuteW just returns a low
    // value (commonly ERROR_CANCELLED) and we simply stay on the error page.
}

static void BrowseForPath() {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;

    COMDLG_FILTERSPEC filters[] = { { L"Application", L"*.exe" } };
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(L"exe");

    std::wstring folder = DirOf(g_destPath);
    std::wstring name = g_destPath.substr(folder.size() + (folder.empty() ? 0 : 1));
    dlg->SetFileName(name.c_str());

    IShellItem* folderItem = nullptr;
    if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&folderItem)))) {
        dlg->SetFolder(folderItem);
        folderItem->Release();
    }

    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* result = nullptr;
        if (SUCCEEDED(dlg->GetResult(&result))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                g_destPath = path;
                CoTaskMemFree(path);
                Repaint();
            }
            result->Release();
        }
    }
    dlg->Release();
}

// ============================================================
// Drawing helpers
// ============================================================
static void FillRoundRect(Graphics& g, const RectF& r, float radius, Brush& brush) {
    GraphicsPath path;
    float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static GraphicsPath* RoundRectPath(const RectF& r, float radius) {
    auto* path = new GraphicsPath();
    float d = radius * 2;
    path->AddArc(r.X, r.Y, d, d, 180, 90);
    path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path->CloseFigure();
    return path;
}

static void DrawButton(Graphics& g, const RECT& rc, const std::wstring& label, bool primary, bool hover, Font& font) {
    RectF r((float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    if (primary) {
        Color c1 = HSLtoRGB(200 + g_colorPhase * 0.4f, 0.85f, hover ? 0.68f : 0.62f);
        Color c2 = HSLtoRGB(260 + g_colorPhase * 0.4f, 0.75f, hover ? 0.66f : 0.58f);
        LinearGradientBrush grad(RectF(r.X, r.Y, r.Width, r.Height), c1, c2, LinearGradientModeHorizontal);
        FillRoundRect(g, r, 8.0f, grad);
    } else {
        SolidBrush brush(hover ? Color(0xFF, 0x3c, 0x3c, 0x3c) : (Color)kSurface3);
        FillRoundRect(g, r, 8.0f, brush);
        Pen pen(kBorder, 1.0f);
        RectF br(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1);
        GraphicsPath* path = RoundRectPath(br, 8.0f);
        g.DrawPath(&pen, path);
        delete path;
    }
    SolidBrush textBrush(primary ? kAccentInk : kText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label.c_str(), -1, &font, r, &fmt, &textBrush);
}

// ============================================================
// Custom title bar
// ============================================================
static void DrawTitleBar(Graphics& g, Font& titleFont, FontFamily& fam) {
    SolidBrush bg(kTitleBg);
    g.FillRectangle(&bg, 0, 0, kWinW, kTitleBarH);

    SolidBrush accentBrush(HSLtoRGB(200 + g_colorPhase, 0.85f, 0.62f));
    FillRoundRect(g, RectF(14, 9, 24, 24), 6.0f, accentBrush);
    SolidBrush inkBrush(kAccentInk);
    StringFormat centerFmt; centerFmt.SetAlignment(StringAlignmentCenter); centerFmt.SetLineAlignment(StringAlignmentCenter);
    Font glyphFont(&fam, 12, FontStyleBold, UnitPixel);
    g.DrawString(L"E", 1, &glyphFont, RectF(14, 9, 24, 24), &centerFmt, &inkBrush);

    SolidBrush textBrush(kText);
    g.DrawString(L"Easy Setup", -1, &titleFont, PointF(48, 12), &textBrush);

    g_rcClose = { kWinW - kTitleBarH, 0, kWinW, kTitleBarH };
    if (g_hoverClose) {
        SolidBrush closeBg(Color(0xFF, 0xc4, 0x2b, 0x1c));
        g.FillRectangle(&closeBg, (float)g_rcClose.left, (float)g_rcClose.top,
            (float)(g_rcClose.right - g_rcClose.left), (float)(g_rcClose.bottom - g_rcClose.top));
    }
    Pen xPen(g_hoverClose ? Color(255,255,255,255) : (Color)kTextDim, 1.4f);
    float cx = (g_rcClose.left + g_rcClose.right) / 2.0f, cy = kTitleBarH / 2.0f;
    g.DrawLine(&xPen, cx - 5, cy - 5, cx + 5, cy + 5);
    g.DrawLine(&xPen, cx + 5, cy - 5, cx - 5, cy + 5);

    Pen divider(kBorder, 1.0f);
    g.DrawLine(&divider, 0.0f, (float)kTitleBarH, (float)kWinW, (float)kTitleBarH);
}

// ============================================================
// Page content (rendered into a content-local bitmap so pages can slide)
// ============================================================
static const int kContentW = kWinW;
static const int kContentH = kWinH - kTitleBarH;
static const int kPad = 26;

static void LayoutAndDrawWelcome(Graphics& g, FontFamily& fam) {
    Font bigTitle(&fam, 19, FontStyleBold, UnitPixel);
    Font sub(&fam, 12, FontStyleRegular, UnitPixel);
    Font btnFont(&fam, 13, FontStyleBold, UnitPixel);

    LinearGradientBrush glow(RectF(kContentW/2.0f - 70, 20, 140, 90),
        HSLtoRGB(200 + g_colorPhase, 0.9f, 0.6f, 90), HSLtoRGB(280 + g_colorPhase, 0.8f, 0.6f, 0), LinearGradientModeVertical);
    g.FillEllipse(&glow, kContentW/2.0f - 70, 20.0f, 140.0f, 90.0f);

    LinearGradientBrush iconGrad(RectF(kContentW/2.0f - 30, 26, 60, 60),
        HSLtoRGB(200 + g_colorPhase, 0.85f, 0.62f), HSLtoRGB(260 + g_colorPhase, 0.75f, 0.58f), LinearGradientModeForwardDiagonal);
    FillRoundRect(g, RectF(kContentW/2.0f - 30, 26, 60, 60), 16.0f, iconGrad);
    SolidBrush ink(kAccentInk);
    StringFormat cf; cf.SetAlignment(StringAlignmentCenter); cf.SetLineAlignment(StringAlignmentCenter);
    Font glyphFont(&fam, 22, FontStyleBold, UnitPixel);
    g.DrawString(L"E", 1, &glyphFont, RectF(kContentW/2.0f - 30, 26, 60, 60), &cf, &ink);

    SolidBrush textBrush(kText), dimBrush(kTextDim);
    RectF titleRc(0, 98, (float)kContentW, 28);
    g.DrawString(L"Welcome to Easy Setup", -1, &bigTitle, titleRc, &cf, &textBrush);
    RectF subRc((float)kPad, 130, (float)(kContentW - kPad * 2), 40);
    StringFormat wrapCenter; wrapCenter.SetAlignment(StringAlignmentCenter);
    g.DrawString(L"Installs the apps you want, flips the Windows settings\nyou always change, and cleans up the rest.", -1, &sub, subRc, &wrapCenter, &dimBrush);

    g_rcPrimary = { kPad, kContentH - kPad - 46, kContentW - kPad, kContentH - kPad };
    DrawButton(g, g_rcPrimary, L"Get Started", true, g_hoverPrimary, btnFont);
}

static void LayoutAndDrawChoose(Graphics& g, FontFamily& fam) {
    Font title(&fam, 16, FontStyleBold, UnitPixel);
    Font label(&fam, 11, FontStyleRegular, UnitPixel);
    Font pathFont(&fam, 12, FontStyleRegular, UnitPixel);
    Font btnFont(&fam, 13, FontStyleBold, UnitPixel);

    SolidBrush textBrush(kText), dimBrush(kTextDim);
    g.DrawString(L"Choose install location", -1, &title, PointF((float)kPad, 22), &textBrush);
    g.DrawString(L"Program Files keeps it available for every user on this PC.", -1, &label, PointF((float)kPad, 50), &dimBrush);

    g.DrawString(L"Install to", -1, &label, PointF((float)kPad, 82), &dimBrush);
    RectF pathBox((float)kPad, 100, (float)(kContentW - kPad * 2 - 96), 38);
    SolidBrush pathBg(kSurface);
    FillRoundRect(g, pathBox, 6.0f, pathBg);
    Pen pathBorder(kBorder, 1.0f);
    g.DrawRectangle(&pathBorder, pathBox);
    RectF pathTextRc(pathBox.X + 12, pathBox.Y, pathBox.Width - 24, pathBox.Height);
    StringFormat pathFmt; pathFmt.SetLineAlignment(StringAlignmentCenter);
    pathFmt.SetTrimming(StringTrimmingEllipsisPath);
    pathFmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(g_destPath.c_str(), -1, &pathFont, pathTextRc, &pathFmt, &textBrush);

    g_rcBrowse = { kContentW - kPad - 80, 100, kContentW - kPad, 138 };
    DrawButton(g, g_rcBrowse, L"Browse…", false, g_hoverBrowse, btnFont);

    int actionTop = kContentH - kPad - 46;
    int half = (kContentW - kPad * 2 - 12) / 2;
    g_rcSecondary = { kPad, actionTop, kPad + half, actionTop + 46 };
    g_rcPrimary = { kPad + half + 12, actionTop, kContentW - kPad, actionTop + 46 };
    DrawButton(g, g_rcSecondary, L"Back", false, g_hoverSecondary, btnFont);
    DrawButton(g, g_rcPrimary, L"Install", true, g_hoverPrimary, btnFont);
}

static void LayoutAndDrawInstalling(Graphics& g, FontFamily& fam) {
    Font title(&fam, 16, FontStyleBold, UnitPixel);
    Font sub(&fam, 12, FontStyleRegular, UnitPixel);
    SolidBrush textBrush(kText), dimBrush(kTextDim);

    g.DrawString(L"Installing Easy Setup…", -1, &title, PointF((float)kPad, 40), &textBrush);
    RectF pathRc((float)kPad, 70, (float)(kContentW - kPad * 2), 20);
    StringFormat trimFmt; trimFmt.SetTrimming(StringTrimmingEllipsisPath); trimFmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(g_destPath.c_str(), -1, &sub, pathRc, &trimFmt, &dimBrush);

    RectF barBg((float)kPad, 110, (float)(kContentW - kPad * 2), 10);
    SolidBrush barBgBrush(kSurface3);
    FillRoundRect(g, barBg, 5.0f, barBgBrush);

    float t = g_progressPhase;
    float eased = t < 0.5f ? (t * 2) : (2 - t * 2);
    float segW = barBg.Width * 0.36f;
    float segX = barBg.X + (barBg.Width - segW) * eased;
    Color c1 = HSLtoRGB(g_colorPhase * 1.6f, 0.85f, 0.62f);
    Color c2 = HSLtoRGB(g_colorPhase * 1.6f + 70, 0.85f, 0.62f);
    LinearGradientBrush segBrush(RectF(segX, barBg.Y, segW, barBg.Height), c1, c2, LinearGradientModeHorizontal);
    FillRoundRect(g, RectF(segX, barBg.Y, segW, barBg.Height), 5.0f, segBrush);

    g.DrawString(L"Fetching the latest build from the site…", -1, &sub, PointF((float)kPad, 134), &dimBrush);
}

static void LayoutAndDrawDone(Graphics& g, FontFamily& fam) {
    Font title(&fam, 17, FontStyleBold, UnitPixel);
    Font sub(&fam, 12, FontStyleRegular, UnitPixel);
    Font btnFont(&fam, 13, FontStyleBold, UnitPixel);
    StringFormat cf; cf.SetAlignment(StringAlignmentCenter); cf.SetLineAlignment(StringAlignmentCenter);

    SolidBrush goodBrush(kGood);
    FillRoundRect(g, RectF(kContentW/2.0f - 26, 20, 52, 52), 26.0f, goodBrush);
    Pen checkPen(kAccentInk, 2.6f);
    float cx = kContentW/2.0f, cy = 46;
    g.DrawLine(&checkPen, cx - 12, cy, cx - 4, cy + 9);
    g.DrawLine(&checkPen, cx - 4, cy + 9, cx + 13, cy - 10);

    SolidBrush textBrush(kText), dimBrush(kTextDim);
    g.DrawString(L"All set!", -1, &title, RectF(0, 84, (float)kContentW, 26), &cf, &textBrush);
    RectF pathRc((float)kPad, 114, (float)(kContentW - kPad * 2), 20);
    StringFormat trimFmt; trimFmt.SetAlignment(StringAlignmentCenter); trimFmt.SetTrimming(StringTrimmingEllipsisPath); trimFmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(g_destPath.c_str(), -1, &sub, pathRc, &trimFmt, &dimBrush);

    int actionTop = kContentH - kPad - 46;
    int half = (kContentW - kPad * 2 - 12) / 2;
    g_rcPrimary = { kPad, actionTop, kPad + half, actionTop + 46 };
    g_rcSecondary = { kPad + half + 12, actionTop, kContentW - kPad, actionTop + 46 };
    DrawButton(g, g_rcPrimary, L"Open", true, g_hoverPrimary, btnFont);
    DrawButton(g, g_rcSecondary, L"Show in folder", false, g_hoverSecondary, btnFont);
}

static void LayoutAndDrawError(Graphics& g, FontFamily& fam) {
    Font title(&fam, 16, FontStyleBold, UnitPixel);
    Font sub(&fam, 12, FontStyleRegular, UnitPixel);
    Font btnFont(&fam, 13, FontStyleBold, UnitPixel);
    StringFormat cf; cf.SetAlignment(StringAlignmentCenter); cf.SetLineAlignment(StringAlignmentCenter);

    SolidBrush badBrush(kBad);
    FillRoundRect(g, RectF(kContentW/2.0f - 24, 20, 48, 48), 24.0f, badBrush);
    Font bangFont(&fam, 20, FontStyleBold, UnitPixel);
    SolidBrush ink(kBg);
    g.DrawString(L"!", 1, &bangFont, RectF(kContentW/2.0f - 24, 18, 48, 48), &cf, &ink);

    SolidBrush textBrush(kText), dimBrush(kTextDim);
    g.DrawString(g_errorNeedsAdmin ? L"Admin permission needed" : L"Couldn't install", -1, &title, RectF(0, 78, (float)kContentW, 24), &cf, &textBrush);
    RectF msgRc((float)kPad, 106, (float)(kContentW - kPad * 2), 40);
    StringFormat wrapCenter; wrapCenter.SetAlignment(StringAlignmentCenter);
    g.DrawString(g_errorText.c_str(), -1, &sub, msgRc, &wrapCenter, &dimBrush);

    int actionTop = kContentH - kPad - 46;
    int half = (kContentW - kPad * 2 - 12) / 2;
    g_rcSecondary = { kPad, actionTop, kPad + half, actionTop + 46 };
    g_rcPrimary = { kPad + half + 12, actionTop, kContentW - kPad, actionTop + 46 };
    DrawButton(g, g_rcSecondary, g_errorNeedsAdmin ? L"Choose different folder" : L"Back", false, g_hoverSecondary, btnFont);
    DrawButton(g, g_rcPrimary, g_errorNeedsAdmin ? L"Continue as Administrator" : L"Try Again", true, g_hoverPrimary, btnFont);
}

static void DrawPageContent(Graphics& g, Page page, FontFamily& fam) {
    switch (page) {
        case PAGE_WELCOME:    LayoutAndDrawWelcome(g, fam); break;
        case PAGE_CHOOSE:     LayoutAndDrawChoose(g, fam); break;
        case PAGE_INSTALLING: LayoutAndDrawInstalling(g, fam); break;
        case PAGE_DONE:       LayoutAndDrawDone(g, fam); break;
        case PAGE_ERROR:      LayoutAndDrawError(g, fam); break;
    }
}

static void GoToPage(Page next) {
    g_prevPage = g_page;
    g_page = next;
    g_transitioning = true;
    g_transitionStart = GetTickCount64();
    EnsureAnimTimer();
    Repaint();
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    Bitmap buffer(kWinW, kWinH);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush bgBrush(kBg);
    g.FillRectangle(&bgBrush, 0, 0, kWinW, kWinH);

    FontFamily fam(L"Segoe UI");
    Font baseFont(&fam, 12, FontStyleRegular, UnitPixel);

    DrawTitleBar(g, baseFont, fam);

    float t = 1.0f;
    if (g_transitioning) {
        ULONGLONG elapsed = GetTickCount64() - g_transitionStart;
        if (elapsed >= (ULONGLONG)kTransitionMs) {
            t = 1.0f;
            g_transitioning = false; // must clear here: once t==1 we take the "else" branch below and never reach it again
        } else {
            t = (float)elapsed / kTransitionMs;
        }
    }
    // ease-out-cubic
    float eased = 1.0f - powf(1.0f - t, 3.0f);

    if (t < 1.0f) {
        Bitmap fromBmp(kContentW, kContentH, PixelFormat32bppPARGB);
        Bitmap toBmp(kContentW, kContentH, PixelFormat32bppPARGB);
        Graphics gFrom(&fromBmp), gTo(&toBmp);
        gFrom.SetSmoothingMode(SmoothingModeAntiAlias); gFrom.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gTo.SetSmoothingMode(SmoothingModeAntiAlias); gTo.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        SolidBrush pageBg(kBg);
        gFrom.FillRectangle(&pageBg, 0, 0, kContentW, kContentH);
        gTo.FillRectangle(&pageBg, 0, 0, kContentW, kContentH);
        DrawPageContent(gFrom, g_prevPage, fam);
        DrawPageContent(gTo, g_page, fam);

        ColorMatrix cmFrom = {{ {1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,1,0},{0,0,0,0,1} }};
        cmFrom.m[3][3] = 1.0f - eased;
        ImageAttributes iaFrom; iaFrom.SetColorMatrix(&cmFrom);
        ColorMatrix cmTo = {{ {1,0,0,0,0},{0,1,0,0,0},{0,0,1,0,0},{0,0,0,1,0},{0,0,0,0,1} }};
        cmTo.m[3][3] = eased;
        ImageAttributes iaTo; iaTo.SetColorMatrix(&cmTo);

        Rect destFrom((int)(-eased * 40), kTitleBarH, kContentW, kContentH);
        Rect destTo((int)((1.0f - eased) * 40), kTitleBarH, kContentW, kContentH);
        g.DrawImage(&fromBmp, destFrom, 0, 0, kContentW, kContentH, UnitPixel, &iaFrom);
        g.DrawImage(&toBmp, destTo, 0, 0, kContentW, kContentH, UnitPixel, &iaTo);
    } else {
        Graphics gOffset(&buffer);
        gOffset.SetSmoothingMode(SmoothingModeAntiAlias);
        gOffset.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gOffset.TranslateTransform(0, (float)kTitleBarH);
        DrawPageContent(gOffset, g_page, fam);
    }

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

// ============================================================
// Window procedure
// ============================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_DONE:
        if ((LONG)wp != g_downloadGeneration) return 0;
        GoToPage(PAGE_DONE);
        return 0;
    case WM_APP_ERROR: {
        auto* msg2 = reinterpret_cast<std::wstring*>(lp);
        std::wstring text = *msg2;
        delete msg2;
        if ((LONG)wp != g_downloadGeneration) return 0;
        g_errorNeedsAdmin = false;
        g_errorText = text;
        GoToPage(PAGE_ERROR);
        return 0;
    }
    case WM_TIMER:
        if (wp == kAnimTimerId) {
            g_colorPhase += 0.5f;
            g_progressPhase = fmodf(g_progressPhase + 0.012f, 1.0f);
            if (g_page == PAGE_INSTALLING && !g_transitioning &&
                GetTickCount64() - g_downloadStartTick > (ULONGLONG)kDownloadTimeoutMs) {
                InterlockedIncrement(&g_downloadGeneration); // invalidate the stuck attempt
                g_errorNeedsAdmin = false;
                g_errorText = L"This is taking too long. Check your connection and try again.";
                GoToPage(PAGE_ERROR);
            }
            Repaint();
        }
        return 0;
    case WM_NCHITTEST: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        if (PtInRect2(g_rcClose, pt.x, pt.y)) return HTCLIENT;
        if (pt.y >= 0 && pt.y < kTitleBarH) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        bool bClose = PtInRect2(g_rcClose, pt.x, pt.y);
        bool bPrimary = PtInRect2(g_rcPrimary, pt.x, pt.y - kTitleBarH);
        bool bSecondary = PtInRect2(g_rcSecondary, pt.x, pt.y - kTitleBarH);
        bool bBrowse = PtInRect2(g_rcBrowse, pt.x, pt.y - kTitleBarH);
        if (bClose != g_hoverClose || bPrimary != g_hoverPrimary || bSecondary != g_hoverSecondary || bBrowse != g_hoverBrowse) {
            g_hoverClose = bClose; g_hoverPrimary = bPrimary; g_hoverSecondary = bSecondary; g_hoverBrowse = bBrowse;
            Repaint();
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverClose = g_hoverPrimary = g_hoverSecondary = g_hoverBrowse = false;
        Repaint();
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_transitioning) return 0;
        int x = LOWORD(lp), y = HIWORD(lp);
        if (PtInRect2(g_rcClose, x, y)) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        int cy = y - kTitleBarH;
        switch (g_page) {
        case PAGE_WELCOME:
            if (PtInRect2(g_rcPrimary, x, cy)) GoToPage(PAGE_CHOOSE);
            break;
        case PAGE_CHOOSE:
            if (PtInRect2(g_rcBrowse, x, cy)) BrowseForPath();
            else if (PtInRect2(g_rcSecondary, x, cy)) GoToPage(PAGE_WELCOME);
            else if (PtInRect2(g_rcPrimary, x, cy)) StartDownload();
            break;
        case PAGE_DONE:
            if (PtInRect2(g_rcPrimary, x, cy)) {
                HINSTANCE r = ShellExecuteW(nullptr, L"open", g_destPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if ((INT_PTR)r <= 32) {
                    std::wstring arg = L"/select,\"" + g_destPath + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                }
            } else if (PtInRect2(g_rcSecondary, x, cy)) {
                std::wstring arg = L"/select,\"" + g_destPath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
            break;
        case PAGE_ERROR:
            if (PtInRect2(g_rcPrimary, x, cy)) {
                if (g_errorNeedsAdmin) RelaunchElevated();
                else GoToPage(PAGE_CHOOSE);
            } else if (PtInRect2(g_rcSecondary, x, cy)) {
                GoToPage(PAGE_CHOOSE);
            }
            break;
        default: break;
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        bool onBtn = PtInRect2(g_rcClose, pt.x, pt.y) || PtInRect2(g_rcPrimary, pt.x, pt.y - kTitleBarH) ||
                     PtInRect2(g_rcSecondary, pt.x, pt.y - kTitleBarH) || PtInRect2(g_rcBrowse, pt.x, pt.y - kTitleBarH);
        SetCursor(LoadCursorW(nullptr, onBtn ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdiplusToken;
    GdiplusStartupInput gdiplusInput;
    GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1 && argv[1][0] != L'\0') {
        g_destPath = argv[1];
        g_relaunchedElevated = true;
    }
    if (argv) LocalFree(argv);

    if (g_relaunchedElevated) {
        g_page = PAGE_INSTALLING;
        g_prevPage = PAGE_INSTALLING;
    } else {
        g_destPath = ProgramFilesDefaultPath();
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"EasySetupDownloaderWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    RegisterClassW(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Easy Setup",
        WS_POPUP,
        (sx - kWinW) / 2, (sy - kWinH) / 2, kWinW, kWinH, nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    EnsureAnimTimer();

    if (g_relaunchedElevated) {
        StartDownload();
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return 0;
}
