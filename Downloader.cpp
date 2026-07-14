// Downloader.cpp — small standalone downloader for Easy Setup.
// Lets the user pick where to save it and performs a plain, standard file
// download (no silent auto-run) — you choose to open it afterwards.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>

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

// ---- Fluent-dark palette, matching the website ----
static const Color kBg(0xFF, 0x20, 0x20, 0x20);
static const Color kSurface(0xFF, 0x2b, 0x2b, 0x2b);
static const Color kSurface3(0xFF, 0x32, 0x32, 0x32);
static const Color kBorder(0xFF, 0x3a, 0x3a, 0x3a);
static const Color kText(0xFF, 0xf3, 0xf3, 0xf3);
static const Color kTextDim(0xFF, 0xa8, 0xa8, 0xa8);
static const Color kAccent(0xFF, 0x60, 0xcd, 0xff);
static const Color kAccentInk(0xFF, 0x0a, 0x1a, 0x22);
static const Color kGood(0xFF, 0x6c, 0xcb, 0x5f);
static const Color kBad(0xFF, 0xff, 0x8a, 0x8a);

enum AppState { STATE_CHOOSE, STATE_DOWNLOADING, STATE_DONE, STATE_ERROR };

static const UINT WM_APP_DONE  = WM_APP + 2;
static const UINT WM_APP_ERROR = WM_APP + 3;
static const UINT_PTR kProgressTimerId = 1;

static const int kDownloadTimeoutMs = 15000;

static HWND g_hwnd = nullptr;
static AppState g_state = STATE_CHOOSE;
static std::wstring g_destPath;
static std::wstring g_errorText;
static int g_progressAnimPhase = 0; // drives the indeterminate progress sweep
static ULONGLONG g_downloadStartTick = 0;
static volatile LONG g_downloadGeneration = 0; // bumped on every retry so a stale
                                                // watchdog/thread can't clobber a newer state

static RECT g_rcBrowse{}, g_rcPrimary{}, g_rcSecondary{};
static bool g_hoverBrowse = false, g_hoverPrimary = false, g_hoverSecondary = false;

static std::wstring DefaultDownloadsPath() {
    PWSTR path = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path))) {
        folder = path;
        CoTaskMemFree(path);
    } else {
        wchar_t buf[MAX_PATH];
        GetTempPathW(MAX_PATH, buf);
        folder = buf;
    }
    if (!folder.empty() && folder.back() != L'\\') folder += L'\\';
    return folder + kDefaultName;
}

static void Repaint() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, TRUE); }

// URLDownloadToFileW is called with no status callback: a previous version wired up a
// custom IBindStatusCallback for exact byte progress, but it introduced a COM lifetime
// bug (the download would finish fine, then the window would silently die on the very
// next allocation, e.g. clicking a button). Not worth the risk for a small, fast file —
// an indeterminate sweep (driven by kProgressTimerId) covers the UX just as well.
static DWORD WINAPI DownloadThread(LPVOID param) {
    LONG myGeneration = (LONG)(LONG_PTR)param;

    // URLDownloadToFileW uses COM internally; this thread needs its own apartment
    // rather than relying on the main thread's (a bare CreateThread()'d thread has
    // none, which made the call hang unpredictably instead of failing fast).
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = URLDownloadToFileW(nullptr, kDownloadUrl, g_destPath.c_str(), 0, nullptr);
    if (SUCCEEDED(coHr)) CoUninitialize();

    // If the UI already gave up on this attempt (watchdog timeout / user retried),
    // don't clobber whatever state it has moved on to.
    if (InterlockedCompareExchange(&g_downloadGeneration, 0, 0) != myGeneration) return 0;

    if (SUCCEEDED(hr)) {
        PostMessageW(g_hwnd, WM_APP_DONE, (WPARAM)myGeneration, 0);
    } else {
        auto* msg = new std::wstring(L"Couldn't reach the download server. Check your connection and try again.");
        PostMessageW(g_hwnd, WM_APP_ERROR, (WPARAM)myGeneration, (LPARAM)msg);
    }
    return 0;
}

static void StartDownload() {
    g_state = STATE_DOWNLOADING;
    g_progressAnimPhase = 0;
    g_downloadStartTick = GetTickCount64();
    LONG generation = InterlockedIncrement(&g_downloadGeneration);
    // Deliberately coarse (not 30ms): a tight repaint timer on the main thread was
    // strongly correlated with URLDownloadToFileW hanging on the download thread —
    // likely message-pump contention with urlmon's internal binding notifications.
    SetTimer(g_hwnd, kProgressTimerId, 200, nullptr);
    Repaint();
    HANDLE t = CreateThread(nullptr, 0, DownloadThread, (LPVOID)(LONG_PTR)generation, 0, nullptr);
    if (t) CloseHandle(t);
}

static void BrowseForPath() {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;

    COMDLG_FILTERSPEC filters[] = { { L"Application", L"*.exe" } };
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(L"exe");

    size_t slash = g_destPath.find_last_of(L'\\');
    std::wstring folder = slash != std::wstring::npos ? g_destPath.substr(0, slash) : L"";
    std::wstring name = slash != std::wstring::npos ? g_destPath.substr(slash + 1) : g_destPath;
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

// ---- drawing helpers ----
static void FillRoundRect(Graphics& g, const RectF& r, float radius, const Brush& brush) {
    GraphicsPath path;
    float d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);
}

static void DrawButton(Graphics& g, const RECT& rc, const std::wstring& label, bool primary, bool hover, Font& font) {
    RectF r((float)rc.left, (float)rc.top, (float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    Color base = primary ? kAccent : kSurface3;
    if (hover) base = primary ? Color(0xFF, 0x7a, 0xd6, 0xff) : Color(0xFF, 0x3c, 0x3c, 0x3c);
    SolidBrush brush(base);
    FillRoundRect(g, r, 6.0f, brush);
    if (!primary) {
        Pen pen(kBorder, 1.0f);
        RectF br(r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1);
        GraphicsPath path; float d = 12.0f;
        path.AddArc(br.X, br.Y, d, d, 180, 90);
        path.AddArc(br.X + br.Width - d, br.Y, d, d, 270, 90);
        path.AddArc(br.X + br.Width - d, br.Y + br.Height - d, d, d, 0, 90);
        path.AddArc(br.X, br.Y + br.Height - d, d, d, 90, 90);
        path.CloseFigure();
        g.DrawPath(&pen, &path);
    }
    SolidBrush textBrush(primary ? kAccentInk : kText);
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(label.c_str(), -1, &font, r, &fmt, &textBrush);
}

static void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc; GetClientRect(hwnd, &rc);

    Bitmap buffer(rc.right, rc.bottom);
    Graphics g(&buffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    SolidBrush bgBrush(kBg);
    g.FillRectangle(&bgBrush, 0, 0, rc.right, rc.bottom);

    FontFamily fam(L"Segoe UI");
    Font titleFont(&fam, 15, FontStyleBold, UnitPixel);
    Font subFont(&fam, 12, FontStyleRegular, UnitPixel);
    Font labelFont(&fam, 11, FontStyleRegular, UnitPixel);
    Font btnFont(&fam, 12, FontStyleBold, UnitPixel);
    Font pathFont(&fam, 12, FontStyleRegular, UnitPixel);

    int pad = 24;

    // ---- header: icon glyph + title ----
    SolidBrush accentBrush(kAccent);
    FillRoundRect(g, RectF((float)pad, (float)pad, 30, 30), 7.0f, accentBrush);
    SolidBrush inkBrush(kAccentInk);
    StringFormat centerFmt; centerFmt.SetAlignment(StringAlignmentCenter); centerFmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(L"E", 1, &titleFont, RectF((float)pad, (float)pad, 30, 30), &centerFmt, &inkBrush);

    SolidBrush textBrush(kText);
    SolidBrush dimBrush(kTextDim);
    g.DrawString(L"Easy Setup", -1, &titleFont, PointF((float)(pad + 42), (float)pad + 1), &textBrush);
    g.DrawString(L"Downloads the latest build straight from the site", -1, &subFont, PointF((float)(pad + 42), (float)pad + 20), &dimBrush);

    int contentTop = pad + 62;
    int contentW = rc.right - pad * 2;

    if (g_state == STATE_CHOOSE || g_state == STATE_DOWNLOADING) {
        g.DrawString(L"Save to", -1, &labelFont, PointF((float)pad, (float)contentTop), &dimBrush);

        RectF pathBox((float)pad, (float)contentTop + 18, (float)(contentW - 96), 38);
        SolidBrush pathBg(kSurface);
        FillRoundRect(g, pathBox, 6.0f, pathBg);
        Pen pathBorder(kBorder, 1.0f);
        g.DrawRectangle(&pathBorder, pathBox);
        RectF pathTextRc(pathBox.X + 12, pathBox.Y, pathBox.Width - 24, pathBox.Height);
        StringFormat pathFmt; pathFmt.SetLineAlignment(StringAlignmentCenter);
        pathFmt.SetTrimming(StringTrimmingEllipsisPath);
        pathFmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(g_destPath.c_str(), -1, &pathFont, pathTextRc, &pathFmt, &textBrush);

        g_rcBrowse = { rc.right - pad - 80, contentTop + 18, rc.right - pad, contentTop + 18 + 38 };
        DrawButton(g, g_rcBrowse, L"Browse…", false, g_hoverBrowse, btnFont);

        int actionTop = contentTop + 18 + 38 + 18;
        if (g_state == STATE_CHOOSE) {
            g_rcPrimary = { pad, actionTop, rc.right - pad, actionTop + 44 };
            DrawButton(g, g_rcPrimary, L"Download", true, g_hoverPrimary, btnFont);
        } else {
            // indeterminate progress sweep — a segment slides back and forth across the track
            RectF barBg((float)pad, (float)actionTop + 10, (float)contentW, 8);
            SolidBrush barBgBrush(kSurface3);
            FillRoundRect(g, barBg, 4.0f, barBgBrush);

            float t = (g_progressAnimPhase % 16) / 16.0f; // 0..1 sawtooth, ~3.2s per cycle at 200ms/tick
            float eased = t < 0.5f ? (t * 2) : (2 - t * 2); // ping-pong 0..1..0
            float segW = barBg.Width * 0.32f;
            float segX = barBg.X + (barBg.Width - segW) * eased;
            RectF barFg(segX, barBg.Y, segW, barBg.Height);
            FillRoundRect(g, barFg, 4.0f, accentBrush);

            g.DrawString(L"Downloading…", -1, &subFont, PointF((float)pad, barBg.Y + 20), &dimBrush);
        }
    } else if (g_state == STATE_DONE) {
        SolidBrush goodBrush(kGood);
        g.DrawString(L"Done", -1, &titleFont, PointF((float)pad, (float)contentTop), &goodBrush);
        RectF pathTextRc((float)pad, (float)contentTop + 26, (float)contentW, 20);
        StringFormat pathFmt; pathFmt.SetTrimming(StringTrimmingEllipsisPath); pathFmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(g_destPath.c_str(), -1, &subFont, pathTextRc, &pathFmt, &dimBrush);

        int actionTop = contentTop + 60;
        int half = (contentW - 12) / 2;
        g_rcPrimary = { pad, actionTop, pad + half, actionTop + 44 };
        g_rcSecondary = { pad + half + 12, actionTop, rc.right - pad, actionTop + 44 };
        DrawButton(g, g_rcPrimary, L"Open", true, g_hoverPrimary, btnFont);
        DrawButton(g, g_rcSecondary, L"Show in folder", false, g_hoverSecondary, btnFont);
    } else if (g_state == STATE_ERROR) {
        SolidBrush badBrush(kBad);
        RectF errRc((float)pad, (float)contentTop, (float)contentW, 44);
        StringFormat wrapFmt; wrapFmt.SetFormatFlags(StringFormatFlagsNoClip);
        g.DrawString(g_errorText.c_str(), -1, &subFont, errRc, &wrapFmt, &badBrush);

        int actionTop = contentTop + 56;
        g_rcPrimary = { pad, actionTop, rc.right - pad, actionTop + 44 };
        DrawButton(g, g_rcPrimary, L"Try again", true, g_hoverPrimary, btnFont);
    }

    Graphics screen(hdc);
    screen.DrawImage(&buffer, 0, 0);
    EndPaint(hwnd, &ps);
}

static bool PtInRect2(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_DONE:
        if ((LONG)wp != g_downloadGeneration) return 0; // stale, already timed out / retried
        KillTimer(hwnd, kProgressTimerId);
        g_state = STATE_DONE;
        Repaint();
        return 0;
    case WM_APP_ERROR: {
        auto* msg2 = reinterpret_cast<std::wstring*>(lp);
        std::wstring text = *msg2;
        delete msg2;
        if ((LONG)wp != g_downloadGeneration) return 0; // stale
        g_errorText = text;
        KillTimer(hwnd, kProgressTimerId);
        g_state = STATE_ERROR;
        Repaint();
        return 0;
    }
    case WM_TIMER:
        if (wp == kProgressTimerId) {
            if (g_state == STATE_DOWNLOADING && GetTickCount64() - g_downloadStartTick > kDownloadTimeoutMs) {
                InterlockedIncrement(&g_downloadGeneration); // invalidate the stuck attempt
                KillTimer(hwnd, kProgressTimerId);
                g_errorText = L"This is taking too long. Check your connection and try again.";
                g_state = STATE_ERROR;
                Repaint();
                return 0;
            }
            g_progressAnimPhase++;
            Repaint();
        }
        return 0;
    case WM_MOUSEMOVE: {
        int x = LOWORD(lp), y = HIWORD(lp);
        bool b1 = PtInRect2(g_rcBrowse, x, y), b2 = PtInRect2(g_rcPrimary, x, y), b3 = PtInRect2(g_rcSecondary, x, y);
        if (b1 != g_hoverBrowse || b2 != g_hoverPrimary || b3 != g_hoverSecondary) {
            g_hoverBrowse = b1; g_hoverPrimary = b2; g_hoverSecondary = b3;
            Repaint();
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hoverBrowse = g_hoverPrimary = g_hoverSecondary = false;
        Repaint();
        return 0;
    case WM_LBUTTONDOWN: {
        int x = LOWORD(lp), y = HIWORD(lp);
        if ((g_state == STATE_CHOOSE) && PtInRect2(g_rcBrowse, x, y)) {
            BrowseForPath();
        } else if (g_state == STATE_CHOOSE && PtInRect2(g_rcPrimary, x, y)) {
            StartDownload();
        } else if (g_state == STATE_ERROR && PtInRect2(g_rcPrimary, x, y)) {
            g_state = STATE_CHOOSE;
            Repaint();
        } else if (g_state == STATE_DONE && PtInRect2(g_rcPrimary, x, y)) {
            HINSTANCE r = ShellExecuteW(nullptr, L"open", g_destPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)r <= 32) {
                std::wstring arg = L"/select,\"" + g_destPath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        } else if (g_state == STATE_DONE && PtInRect2(g_rcSecondary, x, y)) {
            std::wstring arg = L"/select,\"" + g_destPath + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
        }
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        bool onBtn = PtInRect2(g_rcBrowse, pt.x, pt.y) || PtInRect2(g_rcPrimary, pt.x, pt.y) || PtInRect2(g_rcSecondary, pt.x, pt.y);
        SetCursor(LoadCursorW(nullptr, onBtn ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
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

    g_destPath = DefaultDownloadsPath();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"EasySetupDownloaderWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    int w = 440, h = 260;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Easy Setup — Download",
        (WS_POPUP | WS_CAPTION | WS_SYSMENU) & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
        (sx - w) / 2, (sy - h) / 2, w, h, nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return 0;
}
