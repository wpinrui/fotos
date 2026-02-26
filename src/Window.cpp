#include "pch.h"
#include "Window.h"
#include "App.h"

// Dark mode attribute for Windows 10/11
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

Window::Window(App* app) : m_app(app) {}

Window::~Window() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
}

bool Window::Create(HINSTANCE hInstance, int nCmdShow) {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // Get DPI for initial window sizing
    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(nullptr, hdc);
    m_dpiScale = dpi / BASE_DPI;

    // Calculate initial window size
    int initialWidth = static_cast<int>(INITIAL_WIDTH * m_dpiScale);
    int initialHeight = static_cast<int>(INITIAL_HEIGHT * m_dpiScale);

    // Create window
    m_hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        initialWidth, initialHeight,
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) {
        return false;
    }

    // Apply dark mode to title bar
    ApplyDarkMode();

    // Get actual client size
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_width = rc.right - rc.left;
    m_height = rc.bottom - rc.top;

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);

    return true;
}

void Window::SetTitle(const std::wstring& title) {
    SetWindowTextW(m_hwnd, title.c_str());
}

void Window::ToggleFullscreen() {
    DWORD style = GetWindowLong(m_hwnd, GWL_STYLE);

    if (!m_isFullscreen) {
        // Save current window placement
        GetWindowPlacement(m_hwnd, &m_windowPlacement);

        // Get monitor info
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);

        // Remove title bar and borders
        SetWindowLong(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);

        // Resize to fill monitor
        SetWindowPos(m_hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        m_isFullscreen = true;
    } else {
        // Restore title bar and borders
        SetWindowLong(m_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);

        // Restore window placement
        SetWindowPlacement(m_hwnd, &m_windowPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        m_isFullscreen = false;
    }
}

void Window::ApplyDarkMode() {
    // Check if system is in dark mode
    HKEY hKey;
    DWORD useLightTheme = 1;
    DWORD dataSize = sizeof(DWORD);

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_THEME_PATH,
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, REGISTRY_LIGHT_THEME_KEY, nullptr, nullptr,
            (LPBYTE)&useLightTheme, &dataSize);
        RegCloseKey(hKey);
    }

    BOOL darkMode = (useLightTheme == 0) ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* window = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        window = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
        window->m_hwnd = hwnd;
    } else {
        window = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(msg, wParam, lParam);
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT Window::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_DPICHANGED:
        OnDpiChanged(HIWORD(wParam), reinterpret_cast<RECT*>(lParam));
        return 0;

    case WM_KEYDOWN:
        if (m_app) {
            m_app->OnKeyDown(static_cast<UINT>(wParam));
        }
        return 0;

    case WM_KEYUP:
        if (m_app) {
            m_app->OnKeyUp(static_cast<UINT>(wParam));
        }
        return 0;

    case WM_CHAR:
        if (m_app) {
            m_app->OnChar(static_cast<wchar_t>(wParam));
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (m_app) {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            m_app->OnMouseWheel(delta);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (m_app) {
            m_app->OnMouseDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;

    case WM_LBUTTONUP:
        if (m_app) {
            m_app->OnMouseUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;

    case WM_MOUSEMOVE:
        if (m_app) {
            m_app->OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (m_app) {
            m_app->ShowContextMenu(m_hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;

    case WM_COMMAND:
        if (m_app) {
            m_app->OnContextMenuCommand(LOWORD(wParam));
        }
        return 0;

    case WM_PAINT:
        if (m_app) {
            m_app->Render();
        }
        ValidateRect(m_hwnd, nullptr);
        return 0;

    case WM_DROPFILES:
        if (m_app) {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            wchar_t filePath[MAX_PATH];
            if (DragQueryFileW(hDrop, 0, filePath, MAX_PATH)) {
                m_app->OpenFile(filePath);
            }
            DragFinish(hDrop);
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        return 1; // Prevent flicker - we handle all drawing

    default:
        return DefWindowProc(m_hwnd, msg, wParam, lParam);
    }
}

void Window::OnResize(int width, int height) {
    m_width = width;
    m_height = height;
    if (m_app && width > 0 && height > 0) {
        m_app->OnResize(width, height);
    }
}

void Window::OnDpiChanged(UINT dpi, const RECT* newRect) {
    m_dpiScale = dpi / BASE_DPI;

    SetWindowPos(m_hwnd, nullptr,
        newRect->left, newRect->top,
        newRect->right - newRect->left,
        newRect->bottom - newRect->top,
        SWP_NOZORDER | SWP_NOACTIVATE);
}
