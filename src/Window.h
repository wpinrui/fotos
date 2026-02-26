#pragma once
#include "pch.h"

class App;

class Window {
public:
    Window(App* app);
    ~Window();

    bool Create(HINSTANCE hInstance, int nCmdShow);
    HWND GetHwnd() const { return m_hwnd; }

    void SetTitle(const std::wstring& title);
    void ToggleFullscreen();

    // Get default window title (static for use by other classes)
    static std::wstring GetDefaultTitle() { return WINDOW_TITLE; }
    bool IsFullscreen() const { return m_isFullscreen; }

    // Get client area size in pixels
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    float GetDpiScale() const { return m_dpiScale; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnResize(int width, int height);
    void OnDpiChanged(UINT dpi, const RECT* newRect);
    void ApplyDarkMode();

    // Window constants
    static constexpr wchar_t WINDOW_CLASS_NAME[] = L"FotosWindow";
    static constexpr wchar_t WINDOW_TITLE[] = L"fotos";
    static constexpr int INITIAL_WIDTH = 800;
    static constexpr int INITIAL_HEIGHT = 600;
    static constexpr float BASE_DPI = 96.0f;

    // Registry constants for dark mode detection
    static constexpr wchar_t REGISTRY_THEME_PATH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr wchar_t REGISTRY_LIGHT_THEME_KEY[] = L"AppsUseLightTheme";

    App* m_app;
    HWND m_hwnd = nullptr;
    int m_width = INITIAL_WIDTH;
    int m_height = INITIAL_HEIGHT;
    float m_dpiScale = 1.0f;
    bool m_isFullscreen = false;
    WINDOWPLACEMENT m_windowPlacement = { sizeof(WINDOWPLACEMENT) };
};
