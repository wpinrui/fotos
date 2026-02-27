#include "pch.h"
#include "Renderer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Helper to create render target bitmap properties (used by CreateDeviceResources and Resize)
static D2D1_BITMAP_PROPERTIES1 CreateRenderTargetBitmapProperties() {
    return D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
    );
}

// Helper to clamp a value to bounds [0, max]
static float ClampToBounds(float value, float maxValue) {
    return std::max(0.0f, std::min(value, maxValue));
}

Renderer::~Renderer() {
    DiscardDeviceResources();
}

bool Renderer::Initialize(HWND hwnd) {
    m_hwnd = hwnd;

    RECT rc;
    GetClientRect(hwnd, &rc);
    m_width = rc.right - rc.left;
    m_height = rc.bottom - rc.top;

    // Create WIC factory
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_wicFactory)
    );
    if (FAILED(hr)) return false;

    // Create D2D factory
    D2D1_FACTORY_OPTIONS options = {};
#ifdef _DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        options,
        m_factory.GetAddressOf()
    );
    if (FAILED(hr)) return false;

    // Create DWrite factory for text rendering
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Create cached toolbar text format
    m_dwriteFactory->CreateTextFormat(DEFAULT_FONT_NAME, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        TOOLBAR_FONT_SIZE, DEFAULT_LOCALE, &m_toolbarTextFormat);
    if (m_toolbarTextFormat) {
        m_toolbarTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_toolbarTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Create icon font (Segoe Fluent Icons) with explicit availability check
    ComPtr<IDWriteFontCollection> fontCollection;
    m_dwriteFactory->GetSystemFontCollection(&fontCollection);
    if (fontCollection) {
        UINT32 familyIndex = 0;
        BOOL exists = FALSE;
        fontCollection->FindFamilyName(ICON_FONT_NAME, &familyIndex, &exists);
        if (exists) {
            hr = m_dwriteFactory->CreateTextFormat(ICON_FONT_NAME, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                ICON_FONT_SIZE, DEFAULT_LOCALE, &m_iconTextFormat);
            if (SUCCEEDED(hr) && m_iconTextFormat) {
                m_iconTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_iconTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_hasIconFont = true;
            }
        }
    }

    // Create tooltip text format
    m_dwriteFactory->CreateTextFormat(DEFAULT_FONT_NAME, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        TOOLTIP_FONT_SIZE, DEFAULT_LOCALE, &m_tooltipTextFormat);

    // Create toast text format
    m_dwriteFactory->CreateTextFormat(DEFAULT_FONT_NAME, nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        TOAST_FONT_SIZE, DEFAULT_LOCALE, &m_toastTextFormat);
    if (m_toastTextFormat) {
        m_toastTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_toastTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    try {
        CreateDeviceResources();
    } catch (...) {
        return false;
    }
    return true;
}

void Renderer::CreateDeviceResources() {
    // Create D3D11 device
    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    D3D_FEATURE_LEVEL featureLevel;

    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        creationFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &d3dDevice,
        &featureLevel,
        &d3dContext
    );

    if (FAILED(hr)) {
        // Fall back to WARP
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            creationFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &d3dDevice,
            &featureLevel,
            &d3dContext
        );
    }
    THROW_IF_FAILED(hr);

    // Get DXGI device
    ComPtr<IDXGIDevice1> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    THROW_IF_FAILED(hr);

    // Create D2D device
    hr = m_factory->CreateDevice(dxgiDevice.Get(), &m_device);
    THROW_IF_FAILED(hr);

    // Create D2D device context
    hr = m_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &m_deviceContext
    );
    THROW_IF_FAILED(hr);

    // Get DXGI adapter and factory
    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    THROW_IF_FAILED(hr);

    ComPtr<IDXGIFactory2> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
    THROW_IF_FAILED(hr);

    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width > 0 ? m_width : MIN_DIMENSION;
    swapChainDesc.Height = m_height > 0 ? m_height : MIN_DIMENSION;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = SWAP_CHAIN_BUFFER_COUNT;
    swapChainDesc.Scaling = DXGI_SCALING_NONE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = dxgiFactory->CreateSwapChainForHwnd(
        d3dDevice.Get(),
        m_hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &m_swapChain
    );
    THROW_IF_FAILED(hr);

    // Create render target bitmap from swap chain
    ComPtr<IDXGISurface> dxgiSurface;
    hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiSurface));
    THROW_IF_FAILED(hr);

    D2D1_BITMAP_PROPERTIES1 bitmapProperties = CreateRenderTargetBitmapProperties();

    hr = m_deviceContext->CreateBitmapFromDxgiSurface(
        dxgiSurface.Get(),
        bitmapProperties,
        &m_targetBitmap
    );
    THROW_IF_FAILED(hr);

    m_deviceContext->SetTarget(m_targetBitmap.Get());
}

void Renderer::DiscardDeviceResources() {
    m_targetBitmap.Reset();
    m_swapChain.Reset();
    m_deviceContext.Reset();
    m_device.Reset();
}

void Renderer::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == m_width && height == m_height) return;
    if (!m_swapChain || !m_deviceContext) return;

    m_width = width;
    m_height = height;

    // Clear target before resizing
    m_deviceContext->SetTarget(nullptr);
    m_targetBitmap.Reset();

    // Resize swap chain
    HRESULT hr = m_swapChain->ResizeBuffers(
        0, width, height,
        DXGI_FORMAT_UNKNOWN, 0
    );

    if (SUCCEEDED(hr)) {
        // Recreate render target
        ComPtr<IDXGISurface> dxgiSurface;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiSurface));
        THROW_IF_FAILED(hr);

        D2D1_BITMAP_PROPERTIES1 bitmapProperties = CreateRenderTargetBitmapProperties();

        hr = m_deviceContext->CreateBitmapFromDxgiSurface(
            dxgiSurface.Get(),
            bitmapProperties,
            &m_targetBitmap
        );
        THROW_IF_FAILED(hr);

        m_deviceContext->SetTarget(m_targetBitmap.Get());
    }
}

void Renderer::SetImage(ComPtr<ID2D1Bitmap> bitmap) {
    m_currentImage = bitmap;
    ResetView();
}

void Renderer::ClearImage() {
    m_currentImage.Reset();
}

void Renderer::SetZoom(float zoom) {
    m_zoom = std::clamp(zoom, MIN_ZOOM, MAX_ZOOM);
}

void Renderer::SetPan(float panX, float panY) {
    m_panX = panX;
    m_panY = panY;
}

void Renderer::AddPan(float dx, float dy) {
    m_panX += dx;
    m_panY += dy;
}

void Renderer::ResetView() {
    m_zoom = 1.0f;
    m_panX = 0.0f;
    m_panY = 0.0f;
}

void Renderer::SetRotation(int degrees) {
    m_rotation = degrees % Rotation::FULL_ROTATION;
}

void Renderer::SetCropMode(bool enabled) {
    m_cropMode = enabled;
    if (!enabled) {
        m_cropRect = { 0, 0, 0, 0 };
    }

    // Create brushes for crop overlay if needed
    if (enabled && !m_cropBrush && m_deviceContext) {
        m_deviceContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::White), &m_cropBrush);
        m_deviceContext->CreateSolidColorBrush(
            D2D1::ColorF(0, 0, 0, CROP_DIM_OPACITY), &m_cropDimBrush);
    }
}

void Renderer::SetCropRect(D2D1_RECT_F rect) {
    m_cropRect = rect;
}

void Renderer::SetMarkupStrokes(const std::vector<MarkupStroke>& strokes) {
    m_markupStrokes = strokes;
}

void Renderer::SetTextOverlays(const std::vector<TextOverlay>& overlays) {
    m_textOverlays = overlays;
}

D2D1_RECT_F Renderer::GetScreenImageRect() const {
    return CalculateImageRect();
}

D2D1_RECT_F Renderer::GetCropRectInImageCoords() const {
    if (!m_currentImage) return { 0, 0, 0, 0 };

    // Get the image rect in screen coordinates
    D2D1_RECT_F imageRect = CalculateImageRect();

    // Convert screen crop rect to image coordinates
    float scaleX = m_currentImage->GetSize().width / (imageRect.right - imageRect.left);
    float scaleY = m_currentImage->GetSize().height / (imageRect.bottom - imageRect.top);

    D2D1_RECT_F result;
    result.left = (m_cropRect.left - imageRect.left) * scaleX;
    result.top = (m_cropRect.top - imageRect.top) * scaleY;
    result.right = (m_cropRect.right - imageRect.left) * scaleX;
    result.bottom = (m_cropRect.bottom - imageRect.top) * scaleY;

    // Clamp to image bounds
    float imgWidth = m_currentImage->GetSize().width;
    float imgHeight = m_currentImage->GetSize().height;
    result.left = ClampToBounds(result.left, imgWidth);
    result.top = ClampToBounds(result.top, imgHeight);
    result.right = ClampToBounds(result.right, imgWidth);
    result.bottom = ClampToBounds(result.bottom, imgHeight);

    return result;
}

D2D1_RECT_F Renderer::CalculateImageRect() const {
    if (!m_currentImage) {
        return D2D1::RectF(0, 0, 0, 0);
    }

    auto imageSize = m_currentImage->GetSize();
    float imageWidth = imageSize.width;
    float imageHeight = imageSize.height;

    // Swap dimensions for 90/270 degree rotations
    if (m_rotation == Rotation::CW_90 || m_rotation == Rotation::CW_270) {
        std::swap(imageWidth, imageHeight);
    }

    // Calculate scale to fit image in window while preserving aspect ratio
    float scaleX = static_cast<float>(m_width) / imageWidth;
    float scaleY = static_cast<float>(m_height) / imageHeight;
    float fitScale = std::min(scaleX, scaleY);

    // Apply user zoom
    float finalScale = fitScale * m_zoom;

    // Calculate centered position
    float scaledWidth = imageWidth * finalScale;
    float scaledHeight = imageHeight * finalScale;
    float x = (m_width - scaledWidth) / 2.0f + m_panX;
    float y = (m_height - scaledHeight) / 2.0f + m_panY;

    return D2D1::RectF(x, y, x + scaledWidth, y + scaledHeight);
}

void Renderer::RenderMarkupStrokes(const D2D1_RECT_F& screenRect) {
    float screenW = screenRect.right - screenRect.left;
    float screenH = screenRect.bottom - screenRect.top;

    for (const auto& stroke : m_markupStrokes) {
        if (stroke.points.size() < 2) continue;

        ComPtr<ID2D1SolidColorBrush> brush;
        m_deviceContext->CreateSolidColorBrush(stroke.color, &brush);
        if (!brush) continue;

        float screenStrokeWidth = stroke.width * screenW;

        for (size_t i = 1; i < stroke.points.size(); ++i) {
            D2D1_POINT_2F p1 = {
                screenRect.left + stroke.points[i - 1].x * screenW,
                screenRect.top + stroke.points[i - 1].y * screenH
            };
            D2D1_POINT_2F p2 = {
                screenRect.left + stroke.points[i].x * screenW,
                screenRect.top + stroke.points[i].y * screenH
            };
            m_deviceContext->DrawLine(p1, p2, brush.Get(), screenStrokeWidth);
        }
    }
}

void Renderer::RenderTextOverlays(const D2D1_RECT_F& screenRect) {
    if (!m_dwriteFactory) return;

    float screenW = screenRect.right - screenRect.left;
    float screenH = screenRect.bottom - screenRect.top;

    for (const auto& text : m_textOverlays) {
        ComPtr<IDWriteTextFormat> textFormat;
        float screenFontSize = text.fontSize * screenW;
        m_dwriteFactory->CreateTextFormat(DEFAULT_FONT_NAME, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            screenFontSize, DEFAULT_LOCALE, &textFormat);
        if (!textFormat) continue;

        ComPtr<ID2D1SolidColorBrush> brush;
        m_deviceContext->CreateSolidColorBrush(text.color, &brush);
        if (!brush) continue;

        float screenX = screenRect.left + text.x * screenW;
        float screenY = screenRect.top + text.y * screenH;
        m_deviceContext->DrawText(text.text.c_str(), (UINT32)text.text.length(),
            textFormat.Get(), D2D1::RectF(screenX, screenY, screenX + TEXT_DRAW_MAX_WIDTH, screenY + TEXT_DRAW_MAX_HEIGHT), brush.Get());
    }
}

void Renderer::RenderCropOverlay() {
    if (!m_cropMode || !m_cropBrush || !m_cropDimBrush) return;
    if (m_cropRect.right <= m_cropRect.left || m_cropRect.bottom <= m_cropRect.top) return;

    float width = static_cast<float>(m_width);
    float height = static_cast<float>(m_height);

    // Dim area outside crop rect (top, bottom, left, right regions)
    m_deviceContext->FillRectangle(
        D2D1::RectF(0, 0, width, m_cropRect.top), m_cropDimBrush.Get());
    m_deviceContext->FillRectangle(
        D2D1::RectF(0, m_cropRect.bottom, width, height), m_cropDimBrush.Get());
    m_deviceContext->FillRectangle(
        D2D1::RectF(0, m_cropRect.top, m_cropRect.left, m_cropRect.bottom), m_cropDimBrush.Get());
    m_deviceContext->FillRectangle(
        D2D1::RectF(m_cropRect.right, m_cropRect.top, width, m_cropRect.bottom), m_cropDimBrush.Get());

    // Draw crop border
    m_deviceContext->DrawRectangle(m_cropRect, m_cropBrush.Get(), CROP_BORDER_WIDTH);
}

void Renderer::Render() {
    if (!m_deviceContext || !m_targetBitmap) return;

    m_deviceContext->BeginDraw();
    m_deviceContext->Clear(m_backgroundColor);

    if (m_currentImage) {
        D2D1_RECT_F destRect = CalculateImageRect();

        // Apply rotation transform
        if (m_rotation != Rotation::NONE) {
            float centerX = (destRect.left + destRect.right) / 2.0f;
            float centerY = (destRect.top + destRect.bottom) / 2.0f;
            m_deviceContext->SetTransform(
                D2D1::Matrix3x2F::Rotation(static_cast<float>(m_rotation),
                    D2D1::Point2F(centerX, centerY))
            );

            // Adjust destRect for rotated image
            if (m_rotation == Rotation::CW_90 || m_rotation == Rotation::CW_270) {
                float w = destRect.right - destRect.left;
                float h = destRect.bottom - destRect.top;
                destRect = D2D1::RectF(
                    centerX - h / 2, centerY - w / 2,
                    centerX + h / 2, centerY + w / 2
                );
            }
        }

        // Use high quality interpolation for better image quality
        m_deviceContext->DrawBitmap(
            m_currentImage.Get(),
            destRect,
            1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
        );

        // Reset transform
        m_deviceContext->SetTransform(D2D1::Matrix3x2F::Identity());

        // Render overlays using extracted helper methods
        D2D1_RECT_F screenRect = CalculateImageRect();
        RenderMarkupStrokes(screenRect);
        RenderTextOverlays(screenRect);
        RenderCropOverlay();
    }

    // Toolbar draws on top of everything, even without an image
    RenderToolbar();
    RenderTooltip();
    RenderToast();

    HRESULT hr = m_deviceContext->EndDraw();

    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
        CreateDeviceResources();
    }

    // Present
    DXGI_PRESENT_PARAMETERS presentParams = {};
    m_swapChain->Present1(1, 0, &presentParams);
}

void Renderer::SetToolbar(const std::vector<ToolbarButton>& buttons, D2D1_RECT_F bounds, float opacity) {
    m_toolbarButtons = buttons;
    m_toolbarBounds = bounds;
    m_toolbarOpacity = opacity;
    m_toolbarVisible = true;
}

void Renderer::ClearToolbar() {
    m_toolbarVisible = false;
}

void Renderer::RenderToolbar() {
    if (!m_toolbarVisible || !m_deviceContext || !m_toolbarTextFormat) return;
    if (m_toolbarButtons.empty()) return;

    // Draw rounded background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    D2D1_COLOR_F bgColor = Colors::TOOLBAR_BG;
    bgColor.a *= m_toolbarOpacity;
    m_deviceContext->CreateSolidColorBrush(bgColor, &bgBrush);
    if (!bgBrush) return;

    D2D1_ROUNDED_RECT roundedBg = {
        m_toolbarBounds,
        TOOLBAR_CORNER_RADIUS,
        TOOLBAR_CORNER_RADIUS
    };
    m_deviceContext->FillRoundedRectangle(roundedBg, bgBrush.Get());

    // Brushes for buttons
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> disabledBrush;
    ComPtr<ID2D1SolidColorBrush> hoverBrush;
    ComPtr<ID2D1SolidColorBrush> separatorBrush;

    D2D1_COLOR_F whiteColor = Colors::WHITE;
    whiteColor.a *= m_toolbarOpacity;
    m_deviceContext->CreateSolidColorBrush(whiteColor, &textBrush);

    D2D1_COLOR_F disabledColor = Colors::TOOLBAR_DISABLED;
    disabledColor.a *= m_toolbarOpacity;
    m_deviceContext->CreateSolidColorBrush(disabledColor, &disabledBrush);

    D2D1_COLOR_F hoverColor = Colors::TOOLBAR_HOVER;
    hoverColor.a *= m_toolbarOpacity;
    m_deviceContext->CreateSolidColorBrush(hoverColor, &hoverBrush);

    D2D1_COLOR_F sepColor = Colors::TOOLBAR_SEPARATOR;
    sepColor.a *= m_toolbarOpacity;
    m_deviceContext->CreateSolidColorBrush(sepColor, &separatorBrush);

    if (!textBrush || !disabledBrush || !hoverBrush || !separatorBrush) return;

    for (const auto& btn : m_toolbarButtons) {
        if (btn.isSeparator) {
            // Draw thin vertical separator line
            float centerX = (btn.rect.left + btn.rect.right) / 2.0f;
            float topY = btn.rect.top + 4.0f;
            float bottomY = btn.rect.bottom - 4.0f;
            m_deviceContext->DrawLine(
                D2D1::Point2F(centerX, topY),
                D2D1::Point2F(centerX, bottomY),
                separatorBrush.Get(), TOOLBAR_SEPARATOR_LINE_WIDTH);
            continue;
        }

        // Draw hover highlight
        if (btn.hovered && btn.enabled) {
            D2D1_ROUNDED_RECT hoverRect = {
                btn.rect,
                TOOLBAR_CORNER_RADIUS / 2.0f,
                TOOLBAR_CORNER_RADIUS / 2.0f
            };
            m_deviceContext->FillRoundedRectangle(hoverRect, hoverBrush.Get());
        }

        // Draw icon or text label
        auto* brush = btn.enabled ? textBrush.Get() : disabledBrush.Get();
        if (btn.useIcon && btn.iconCodepoint != 0 && m_hasIconFont && m_iconTextFormat) {
            wchar_t glyphStr[2] = { btn.iconCodepoint, 0 };
            if (btn.mirrorIcon) {
                float centerX = (btn.rect.left + btn.rect.right) / 2.0f;
                float centerY = (btn.rect.top + btn.rect.bottom) / 2.0f;
                auto mirror = D2D1::Matrix3x2F::Translation(-centerX, -centerY)
                    * D2D1::Matrix3x2F::Scale(-1.0f, 1.0f)
                    * D2D1::Matrix3x2F::Translation(centerX, centerY);
                m_deviceContext->SetTransform(mirror);
            }
            m_deviceContext->DrawText(glyphStr, 1, m_iconTextFormat.Get(), btn.rect, brush);
            if (btn.mirrorIcon) {
                m_deviceContext->SetTransform(D2D1::Matrix3x2F::Identity());
            }
        } else {
            m_deviceContext->DrawText(
                btn.label.c_str(),
                static_cast<UINT32>(btn.label.length()),
                m_toolbarTextFormat.Get(),
                btn.rect,
                brush);
        }
    }
}

void Renderer::SetTooltip(const TooltipData& tooltip) { m_tooltip = tooltip; }
void Renderer::ClearTooltip() { m_tooltip.visible = false; }

void Renderer::SetToast(const std::wstring& message, float opacity) {
    m_toastMessage = message;
    m_toastOpacity = opacity;
}

void Renderer::ClearToast() {
    m_toastMessage.clear();
    m_toastOpacity = 0.0f;
}

void Renderer::RenderTooltip() {
    if (!m_tooltip.visible || m_tooltip.text.empty() || !m_tooltipTextFormat || !m_dwriteFactory) return;

    // Measure text to size the tooltip
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(m_tooltip.text.c_str(),
        static_cast<UINT32>(m_tooltip.text.length()),
        m_tooltipTextFormat.Get(), TOOLTIP_MAX_WIDTH, 50.0f, &layout);
    if (!layout) return;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);

    float tooltipW = metrics.width + TOOLTIP_PADDING_H * 2;
    float tooltipH = metrics.height + TOOLTIP_PADDING_V * 2;

    // Position below the anchor button, centered
    float anchorCenterX = (m_tooltip.anchorRect.left + m_tooltip.anchorRect.right) / 2.0f;
    float tooltipLeft = anchorCenterX - tooltipW / 2.0f;
    float tooltipTop = m_tooltip.anchorRect.bottom + TOOLTIP_OFFSET_Y;

    // Clamp to window bounds
    float margin = 4.0f;
    tooltipLeft = std::max(margin, std::min(tooltipLeft, static_cast<float>(m_width) - tooltipW - margin));

    D2D1_RECT_F tipRect = D2D1::RectF(tooltipLeft, tooltipTop, tooltipLeft + tooltipW, tooltipTop + tooltipH);

    // Draw background
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    m_deviceContext->CreateSolidColorBrush(Colors::TOOLTIP_BG, &bgBrush);
    if (!bgBrush) return;
    D2D1_ROUNDED_RECT roundedRect = { tipRect, TOOLTIP_CORNER_RADIUS, TOOLTIP_CORNER_RADIUS };
    m_deviceContext->FillRoundedRectangle(roundedRect, bgBrush.Get());

    // Draw text
    ComPtr<ID2D1SolidColorBrush> textBrush;
    m_deviceContext->CreateSolidColorBrush(Colors::WHITE, &textBrush);
    if (!textBrush) return;
    D2D1_RECT_F textRect = D2D1::RectF(
        tipRect.left + TOOLTIP_PADDING_H, tipRect.top + TOOLTIP_PADDING_V,
        tipRect.right - TOOLTIP_PADDING_H, tipRect.bottom - TOOLTIP_PADDING_V);
    m_deviceContext->DrawText(m_tooltip.text.c_str(),
        static_cast<UINT32>(m_tooltip.text.length()),
        m_tooltipTextFormat.Get(), textRect, textBrush.Get());
}

void Renderer::RenderToast() {
    if (m_toastMessage.empty() || m_toastOpacity <= 0.0f || !m_toastTextFormat || !m_dwriteFactory) return;

    // Measure text
    ComPtr<IDWriteTextLayout> layout;
    m_dwriteFactory->CreateTextLayout(m_toastMessage.c_str(),
        static_cast<UINT32>(m_toastMessage.length()),
        m_toastTextFormat.Get(), TOAST_MAX_WIDTH, 50.0f, &layout);
    if (!layout) return;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);

    float toastW = metrics.width + TOAST_PADDING_H * 2;
    float toastH = metrics.height + TOAST_PADDING_V * 2;

    // Bottom center
    float toastLeft = (static_cast<float>(m_width) - toastW) / 2.0f;
    float toastTop = static_cast<float>(m_height) - TOAST_BOTTOM_MARGIN - toastH;

    D2D1_RECT_F toastRect = D2D1::RectF(toastLeft, toastTop, toastLeft + toastW, toastTop + toastH);

    // Draw background with opacity
    ComPtr<ID2D1SolidColorBrush> bgBrush;
    D2D1_COLOR_F bg = Colors::TOAST_BG;
    bg.a *= m_toastOpacity;
    m_deviceContext->CreateSolidColorBrush(bg, &bgBrush);
    if (!bgBrush) return;
    D2D1_ROUNDED_RECT rounded = { toastRect, TOAST_CORNER_RADIUS, TOAST_CORNER_RADIUS };
    m_deviceContext->FillRoundedRectangle(rounded, bgBrush.Get());

    // Draw text with opacity
    ComPtr<ID2D1SolidColorBrush> textBrush;
    D2D1_COLOR_F textColor = Colors::WHITE;
    textColor.a *= m_toastOpacity;
    m_deviceContext->CreateSolidColorBrush(textColor, &textBrush);
    if (!textBrush) return;
    m_deviceContext->DrawText(m_toastMessage.c_str(),
        static_cast<UINT32>(m_toastMessage.length()),
        m_toastTextFormat.Get(), toastRect, textBrush.Get());
}
