// Downloader.cpp — tiny bootstrapper. Downloads the latest EasySetup.exe from the
// project website and launches it, so the button on the site can stay a small,
// fast download instead of shipping the full app binary every time.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <urlmon.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "kernel32.lib")

static const wchar_t* kDownloadUrl = L"https://ryxn3.github.io/easy-setup/EasySetup.exe";
static const wchar_t* kFallbackUrl = L"https://ryxn3.github.io/easy-setup/";
static const wchar_t* kWindowTitle = L"Easy Setup";
static const UINT WM_APP_STATUS = WM_APP + 1;

static HWND g_hwnd = nullptr;
static std::wstring g_status = L"Downloading Easy Setup…";
static bool g_failed = false;

static void PostStatus(const std::wstring& text, bool failed) {
    auto* payload = new std::wstring(text);
    PostMessageW(g_hwnd, WM_APP_STATUS, (WPARAM)failed, (LPARAM)payload);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_STATUS: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        g_status = *text;
        g_failed = wp != 0;
        delete text;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, g_failed ? RGB(0xff, 0x8a, 0x8a) : RGB(0xf3, 0xf3, 0xf3));

        HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hdc, font);

        RECT textRc = rc;
        textRc.left += 24; textRc.right -= 24;
        DrawTextW(hdc, g_status.c_str(), -1, &textRc,
            DT_CENTER | DT_VCENTER | DT_WORDBREAK);

        SelectObject(hdc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI DownloadThread(LPVOID) {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring dest = std::wstring(tempPath) + L"EasySetup.exe";

    HRESULT hr = URLDownloadToFileW(nullptr, kDownloadUrl, dest.c_str(), 0, nullptr);

    if (SUCCEEDED(hr)) {
        PostStatus(L"Done — launching Easy Setup…", false);
        Sleep(400);
        HINSTANCE r = ShellExecuteW(nullptr, L"open", dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) {
            PostStatus(L"Downloaded, but couldn't launch it automatically.\nFind it in your Temp folder, or run it as administrator.", true);
            Sleep(2500);
        }
    } else {
        PostStatus(L"Couldn't reach the download server.\nOpening the site instead…", true);
        Sleep(1800);
        ShellExecuteW(nullptr, L"open", kFallbackUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }

    Sleep(300);
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"EasySetupDownloaderWnd";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    RegisterClassW(&wc);

    int w = 380, h = 130;
    int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);

    g_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, kWindowTitle,
        (WS_POPUP | WS_CAPTION | WS_SYSMENU) & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
        (sx - w) / 2, (sy - h) / 2, w, h, nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    HANDLE thread = CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
