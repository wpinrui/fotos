#pragma once
#include "pch.h"

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    bool Initialize(HWND hwnd);
    void Resize(int width, int height);
    void Render();

    // Set the current image to display
    void SetImage(ComPtr<ID2D1Bitmap> bitmap);
    void ClearImage();

    // Zoom and pan
    void SetZoom(float zoom);
    void SetPan(float panX, float panY);
    void AddPan(float dx, float dy);
    void ResetView();
    float GetZoom() const { return m_zoom; }
    float GetPanX() const { return m_panX; }
    float GetPanY() const { return m_panY; }

    // Rotation (0, 90, 180, 270)
    void SetRotation(int degrees);
    int GetRotation() const { return m_rotation; }

    // Crop mode
    void SetCropMode(bool enabled);
    void SetCropRect(D2D1_RECT_F rect);
    D2D1_RECT_F GetCropRectInImageCoords() const;

    // Markup strokes
    struct MarkupStroke {
        std::vector<D2D1_POINT_2F> points;
        D2D1_COLOR_F color;
        float width;
    };
    void SetMarkupStrokes(const std::vector<MarkupStroke>& strokes);

    // Text overlays
    struct TextOverlay {
        std::wstring text;
        float x, y;  // Normalized 0-1 coords
        D2D1_COLOR_F color;
        float fontSize;  // Normalized
    };
    void SetTextOverlays(const std::vector<TextOverlay>& overlays);

    // Toolbar overlay
    struct ToolbarButton {
        D2D1_RECT_F rect;       // Screen-space bounds
        std::wstring label;
        wchar_t iconCodepoint = 0;
        bool useIcon = false;
        bool enabled = true;
        bool hovered = false;
        bool isSeparator = false;
    };
    void SetToolbar(const std::vector<ToolbarButton>& buttons, D2D1_RECT_F bounds, float opacity);
    void ClearToolbar();
    bool HasIconFont() const { return m_hasIconFont; }

    // Tooltip overlay
    struct TooltipData {
        D2D1_RECT_F anchorRect;
        std::wstring text;
        bool visible = false;
    };
    void SetTooltip(const TooltipData& tooltip);
    void ClearTooltip();

    // Toast notification
    void SetToast(const std::wstring& message, float opacity);
    void ClearToast();

    // Get image rect in screen coordinates (for coordinate transforms)
    D2D1_RECT_F GetScreenImageRect() const;

    // Get Direct2D factory (for creating bitmaps)
    ID2D1Factory1* GetFactory() const { return m_factory.Get(); }
    ID2D1DeviceContext* GetDeviceContext() const { return m_deviceContext.Get(); }
    IWICImagingFactory* GetWICFactory() const { return m_wicFactory.Get(); }

    // Text rendering constants (public for shared use)
    static constexpr wchar_t DEFAULT_FONT_NAME[] = L"Segoe UI";
    static constexpr wchar_t DEFAULT_LOCALE[] = L"en-us";

private:
    void CreateDeviceResources();
    void DiscardDeviceResources();
    D2D1_RECT_F CalculateImageRect() const;

    // Rendering sub-routines (extracted from Render for clarity)
    void RenderMarkupStrokes(const D2D1_RECT_F& screenRect);
    void RenderTextOverlays(const D2D1_RECT_F& screenRect);
    void RenderCropOverlay();
    void RenderToolbar();
    void RenderTooltip();
    void RenderToast();

    HWND m_hwnd = nullptr;
    int m_width = 0;
    int m_height = 0;

    // Direct2D resources
    ComPtr<ID2D1Factory1> m_factory;
    ComPtr<ID2D1Device> m_device;
    ComPtr<ID2D1DeviceContext> m_deviceContext;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID2D1Bitmap1> m_targetBitmap;

    // WIC
    ComPtr<IWICImagingFactory> m_wicFactory;

    // Current image
    ComPtr<ID2D1Bitmap> m_currentImage;
    float m_zoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    int m_rotation = 0;

    // Crop mode
    bool m_cropMode = false;
    D2D1_RECT_F m_cropRect = { 0, 0, 0, 0 };
    ComPtr<ID2D1SolidColorBrush> m_cropBrush;
    ComPtr<ID2D1SolidColorBrush> m_cropDimBrush;

    // Markup strokes
    std::vector<MarkupStroke> m_markupStrokes;
    ComPtr<ID2D1SolidColorBrush> m_markupBrush;

    // Text overlays
    std::vector<TextOverlay> m_textOverlays;
    ComPtr<IDWriteFactory> m_dwriteFactory;

    // Toolbar overlay
    std::vector<ToolbarButton> m_toolbarButtons;
    D2D1_RECT_F m_toolbarBounds = {};
    float m_toolbarOpacity = 0.0f;
    bool m_toolbarVisible = false;
    ComPtr<IDWriteTextFormat> m_toolbarTextFormat;

    // Icon font
    ComPtr<IDWriteTextFormat> m_iconTextFormat;
    bool m_hasIconFont = false;

    // Tooltip
    TooltipData m_tooltip;
    ComPtr<IDWriteTextFormat> m_tooltipTextFormat;

    // Toast
    std::wstring m_toastMessage;
    float m_toastOpacity = 0.0f;
    ComPtr<IDWriteTextFormat> m_toastTextFormat;

    // Background color (dark)
    D2D1_COLOR_F m_backgroundColor = Colors::DARK_GRAY;

    // Zoom limits
    static constexpr float MIN_ZOOM = 0.1f;
    static constexpr float MAX_ZOOM = 10.0f;

    // Swap chain constants
    static constexpr UINT SWAP_CHAIN_BUFFER_COUNT = 2;
    static constexpr UINT MIN_DIMENSION = 1;

    // Text rendering constants
    static constexpr float TEXT_DRAW_MAX_WIDTH = 1000.0f;
    static constexpr float TEXT_DRAW_MAX_HEIGHT = 200.0f;

    // Crop overlay constants
    static constexpr float CROP_DIM_OPACITY = 0.5f;
    static constexpr float CROP_BORDER_WIDTH = 2.0f;

    // Toolbar rendering constants
    static constexpr float TOOLBAR_FONT_SIZE = 12.0f;
    static constexpr float TOOLBAR_CORNER_RADIUS = 6.0f;
    static constexpr float TOOLBAR_HOVER_OPACITY = 0.3f;
    static constexpr float TOOLBAR_SEPARATOR_LINE_WIDTH = 1.0f;

    // Icon font constants
    static constexpr float ICON_FONT_SIZE = 16.0f;
    static constexpr wchar_t ICON_FONT_NAME[] = L"Segoe Fluent Icons";

    // Tooltip rendering constants
    static constexpr float TOOLTIP_FONT_SIZE = 11.0f;
    static constexpr float TOOLTIP_PADDING_H = 8.0f;
    static constexpr float TOOLTIP_PADDING_V = 4.0f;
    static constexpr float TOOLTIP_CORNER_RADIUS = 4.0f;
    static constexpr float TOOLTIP_OFFSET_Y = 4.0f;
    static constexpr float TOOLTIP_MAX_WIDTH = 500.0f;

    // Toast rendering constants
    static constexpr float TOAST_FONT_SIZE = 13.0f;
    static constexpr float TOAST_PADDING_H = 16.0f;
    static constexpr float TOAST_PADDING_V = 10.0f;
    static constexpr float TOAST_CORNER_RADIUS = 8.0f;
    static constexpr float TOAST_BOTTOM_MARGIN = 40.0f;
    static constexpr float TOAST_MAX_WIDTH = 600.0f;
};
