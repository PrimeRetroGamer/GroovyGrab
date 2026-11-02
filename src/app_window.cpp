#include "app_window.h"

#include <windowsx.h>
#include <dbt.h>
#include <dwmapi.h>

#include "imgui_impl_win32.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace app_window {

namespace {

constexpr UINT_PTR kResizePaintTimer = 1;
constexpr int kAppIcon = 1; // resource id in groovygrab.rc
constexpr LONG kMinWidth = 800;
constexpr LONG kMinHeight = 560; // the whole panel still fits; at 100% display scale, like the others
constexpr int kDefaultWidth = 1024;
constexpr int kDefaultHeight = 640;

// Windows 11 DWM attributes, by number so older SDKs still build.
constexpr DWORD kDwmUseDarkMode = 20;
constexpr DWORD kDwmCornerPreference = 33;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmCornerDoNotRound = 1;

app_gui::Frame* g_gui = nullptr;
WINDOWPLACEMENT g_placement{sizeof(g_placement)};
bool g_imgui_ready = false;
bool g_sizing = false;
std::function<void()> g_paint;
std::function<void()> g_power_down;

HICON load_icon(HINSTANCE instance, int width_metric, int height_metric) {
    return static_cast<HICON>(LoadImage(instance, MAKEINTRESOURCE(kAppIcon), IMAGE_ICON,
                                        GetSystemMetrics(width_metric), GetSystemMetrics(height_metric),
                                        LR_DEFAULTCOLOR));
}

void paint() {
    if (g_paint) g_paint();
}

void toggle_fullscreen(HWND hwnd) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
    if (!g_gui->fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (GetWindowPlacement(hwnd, &g_placement) &&
            GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtr(hwnd, GWL_STYLE, style & ~WS_THICKFRAME);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_gui->fullscreen = true;
        }
    } else {
        g_gui->fullscreen = false;
        SetWindowLongPtr(hwnd, GWL_STYLE, style | WS_THICKFRAME);
        // A window still marked maximized would keep the monitor's size, over the taskbar.
        if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetWindowPlacement(hwnd, &g_placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

LRESULT hit_test(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    if (g_gui->fullscreen) return HTCLIENT;
    const LRESULT hit = DefWindowProc(hwnd, WM_NCHITTEST, wparam, lparam);
    if (hit != HTCLIENT) return hit;
    RECT window{};
    GetWindowRect(hwnd, &window);
    const int x = GET_X_LPARAM(lparam) - window.left;
    const int y = GET_Y_LPARAM(lparam) - window.top;
    const int edge = static_cast<int>(app_gui::kHit * g_gui->scale);
    const bool left = x < edge;
    const bool right = x >= window.right - window.left - edge;
    const bool top = y < edge;
    const bool bottom = y >= window.bottom - window.top - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    const bool on_caption = x >= static_cast<int>(g_gui->caption_min.x) &&
                            x < static_cast<int>(g_gui->caption_max.x) &&
                            y >= static_cast<int>(g_gui->caption_min.y) &&
                            y < static_cast<int>(g_gui->caption_max.y);
    const bool widget_active =
        g_imgui_ready && ImGui::GetIO().WantCaptureMouse && ImGui::IsAnyItemActive();
    return on_caption && !widget_active ? HTCAPTION : HTCLIENT;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (g_imgui_ready && ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam)) {
        return true;
    }
    switch (message) {
    case WM_NCCALCSIZE:
        if (wparam) return 0;
        break;
    case WM_NCHITTEST:
        return hit_test(hwnd, wparam, lparam);
    case WM_KEYDOWN:
        if (g_imgui_ready && ImGui::GetIO().WantCaptureKeyboard) break;
        if (wparam == VK_ESCAPE) {
            if (g_gui->fullscreen) toggle_fullscreen(hwnd);
            return 0;
        }
        if (wparam == VK_F11) {
            toggle_fullscreen(hwnd);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        limits->ptMinTrackSize = {static_cast<LONG>(kMinWidth * g_gui->scale), static_cast<LONG>(kMinHeight * g_gui->scale)};
        // Borderless windows would otherwise maximize over the taskbar.
        MONITORINFO mi{sizeof(mi)};
        if (!g_gui->fullscreen && GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            limits->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top};
            limits->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
        }
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        g_sizing = true;
        SetTimer(hwnd, kResizePaintTimer, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        g_sizing = false;
        KillTimer(hwnd, kResizePaintTimer);
        paint();
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) paint();
        return 0;
    case WM_TIMER:
        if (wparam == kResizePaintTimer) paint();
        return 0;
    case WM_DPICHANGED: {
        g_gui->scale = static_cast<float>(HIWORD(wparam)) / USER_DEFAULT_SCREEN_DPI;
        g_gui->scale_changed = true;
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED) g_gui->devices_changed = true;
        return TRUE;
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND && g_power_down) g_power_down();
        return TRUE;
    case WM_ENDSESSION:
        if (wparam && g_power_down) g_power_down();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, message, wparam, lparam);
}

} // namespace

HWND create(HINSTANCE instance, app_gui::Frame& gui, int width, int height) {
    g_gui = &gui;
    WNDCLASSEX wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    const HICON big_icon = load_icon(instance, SM_CXICON, SM_CYICON);
    const HICON small_icon = load_icon(instance, SM_CXSMICON, SM_CYSMICON);
    wc.hIcon = big_icon;
    wc.hIconSm = small_icon;
    wc.lpszClassName = L"GroovyGrabWindow";
    if (!RegisterClassEx(&wc)) throw std::runtime_error("RegisterClassEx failed");

    POINT cursor{};
    GetCursorPos(&cursor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfo(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    // Hidden until its display's scale is known, which sets its size.
    HWND hwnd = CreateWindowEx(WS_EX_APPWINDOW, wc.lpszClassName, L"GroovyGrab",
                               WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU,
                               work.left, work.top, kMinWidth, kMinHeight, nullptr, nullptr, instance, nullptr);
    if (!hwnd) throw std::runtime_error("CreateWindowEx failed");
    gui.scale = static_cast<float>(GetDpiForWindow(hwnd)) / USER_DEFAULT_SCREEN_DPI;
    const auto scaled = [&](int size) { return static_cast<int>(size * gui.scale); };
    width = std::min(std::max(width > 0 ? width : scaled(kDefaultWidth), scaled(kMinWidth)), work_w);
    height = std::min(std::max(height > 0 ? height : scaled(kDefaultHeight), scaled(kMinHeight)), work_h);
    SetWindowPos(hwnd, nullptr, work.left + (work_w - width) / 2, work.top + (work_h - height) / 2, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));

    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kDwmUseDarkMode, &dark, sizeof(dark));
    const DWORD square = kDwmCornerDoNotRound;
    DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &square, sizeof(square));
    const COLORREF border = RGB(168, 85, 247);
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &border, sizeof(border));
    const COLORREF caption = RGB(22, 12, 36);
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof(caption));
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

bool normal_size(HWND hwnd, int& width, int& height) {
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (g_gui->fullscreen || !GetWindowPlacement(hwnd, &placement)) return false;
    width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
    height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    return true;
}

void set_paint_callback(std::function<void()> callback) {
    g_paint = std::move(callback);
}

void set_power_down_callback(std::function<void()> callback) {
    g_power_down = std::move(callback);
}

void set_imgui_ready(bool ready) {
    g_imgui_ready = ready;
}

bool sizing() {
    return g_sizing;
}

} // namespace app_window
