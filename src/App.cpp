#include "pch.h"
#include "App.h"
#include "Window.h"
#include "ImageLoader.h"
#include "ImageCache.h"
#include "FolderNavigator.h"

App* App::s_instance = nullptr;

App::App() {
    s_instance = this;
}

App::~App() {
    StopGifAnimation();
    if (m_imageCache) {
        m_imageCache->Shutdown();
    }
    s_instance = nullptr;
}

// Helper to convert rotation degrees to WIC transform option
static WICBitmapTransformOptions GetWICTransformForRotation(int rotation) {
    switch (rotation) {
    case Rotation::CW_90:  return WICBitmapTransformRotate90;
    case Rotation::CW_180: return WICBitmapTransformRotate180;
    case Rotation::CW_270: return WICBitmapTransformRotate270;
    default:               return WICBitmapTransformRotate0;
    }
}

// Crop transformation parameters for coordinate conversion
struct CropTransformParams {
    float originalWidth, originalHeight;
    int cropRectX, cropRectY;
    int cropRectWidth, cropRectHeight;
};

// Check if a normalized point (in original image space) falls inside the crop rectangle
static bool IsPointInCropRect(float normX, float normY, const CropTransformParams& params) {
    float pixelX = normX * params.originalWidth;
    float pixelY = normY * params.originalHeight;
    return pixelX >= params.cropRectX && pixelX <= params.cropRectX + params.cropRectWidth &&
           pixelY >= params.cropRectY && pixelY <= params.cropRectY + params.cropRectHeight;
}

// Transform a normalized point from original image space to cropped image space
// Returns the new normalized coordinates
static D2D1_POINT_2F TransformPointToCroppedSpace(float normX, float normY, const CropTransformParams& params) {
    float pixelX = normX * params.originalWidth;
    float pixelY = normY * params.originalHeight;
    return D2D1::Point2F(
        (pixelX - params.cropRectX) / params.cropRectWidth,
        (pixelY - params.cropRectY) / params.cropRectHeight
    );
}

// Transform markup strokes to cropped coordinate space
static std::vector<App::MarkupStroke> TransformMarkupStrokesForCrop(
    const std::vector<App::MarkupStroke>& strokes,
    const CropTransformParams& params,
    float scaleFactor)
{
    std::vector<App::MarkupStroke> result;
    for (const auto& stroke : strokes) {
        App::MarkupStroke newStroke;
        newStroke.color = stroke.color;
        newStroke.width = stroke.width * scaleFactor;

        for (const auto& pt : stroke.points) {
            if (IsPointInCropRect(pt.x, pt.y, params)) {
                newStroke.points.push_back(TransformPointToCroppedSpace(pt.x, pt.y, params));
            }
        }

        if (newStroke.points.size() >= App::MIN_STROKE_POINTS) {
            result.push_back(newStroke);
        }
    }
    return result;
}

// Transform text overlays to cropped coordinate space
static std::vector<App::TextOverlay> TransformTextOverlaysForCrop(
    const std::vector<App::TextOverlay>& texts,
    const CropTransformParams& params,
    float scaleFactor)
{
    std::vector<App::TextOverlay> result;
    for (const auto& text : texts) {
        if (IsPointInCropRect(text.x, text.y, params)) {
            D2D1_POINT_2F newPos = TransformPointToCroppedSpace(text.x, text.y, params);
            App::TextOverlay newText = text;
            newText.x = newPos.x;
            newText.y = newPos.y;
            newText.fontSize = text.fontSize * scaleFactor;
            result.push_back(newText);
        }
    }
    return result;
}

// Helper to flip buffer from top-down to bottom-up for Windows DIB format
static void FlipBufferVertically(const std::vector<BYTE>& src, std::vector<BYTE>& dst, UINT width, UINT height) {
    UINT stride = App::GetBitmapStride(width);
    dst.resize(src.size());
    for (UINT y = 0; y < height; ++y) {
        memcpy(&dst[y * stride], &src[(height - 1 - y) * stride], stride);
    }
}

// Helper to render markup strokes and text overlays to a D2D render target
static void RenderMarkupAndTextToTarget(
    ID2D1RenderTarget* renderTarget,
    float width, float height,
    const std::vector<App::MarkupStroke>& strokes,
    const std::vector<App::TextOverlay>& texts)
{
    // Draw markup strokes (normalized coords scaled to output size)
    for (const auto& stroke : strokes) {
        if (stroke.points.size() < App::MIN_STROKE_POINTS) continue;
        ComPtr<ID2D1SolidColorBrush> brush;
        renderTarget->CreateSolidColorBrush(stroke.color, &brush);
        if (!brush) continue;

        float strokeWidth = stroke.width * width;
        for (size_t i = 1; i < stroke.points.size(); ++i) {
            D2D1_POINT_2F p1 = { stroke.points[i-1].x * width, stroke.points[i-1].y * height };
            D2D1_POINT_2F p2 = { stroke.points[i].x * width, stroke.points[i].y * height };
            renderTarget->DrawLine(p1, p2, brush.Get(), strokeWidth);
        }
    }

    // Draw text overlays
    ComPtr<IDWriteFactory> dwriteFactory;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));

    if (dwriteFactory) {
        for (const auto& text : texts) {
            ComPtr<IDWriteTextFormat> textFormat;
            float fontSize = text.fontSize * width;
            dwriteFactory->CreateTextFormat(Renderer::DEFAULT_FONT_NAME, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                fontSize, Renderer::DEFAULT_LOCALE, &textFormat);
            if (!textFormat) continue;

            ComPtr<ID2D1SolidColorBrush> brush;
            renderTarget->CreateSolidColorBrush(text.color, &brush);
            if (!brush) continue;

            float x = text.x * width;
            float y = text.y * height;
            renderTarget->DrawText(text.text.c_str(), (UINT32)text.text.length(),
                textFormat.Get(), D2D1::RectF(x, y, width, height), brush.Get());
        }
    }
}

// Load and decode image from file to WIC bitmap source
ComPtr<IWICBitmapSource> App::LoadAndDecodeImage(IWICImagingFactory* wicFactory, WICPixelFormatGUID targetFormat) {
    if (!m_currentImage || m_currentImage->filePath.empty()) return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(
        m_currentImage->filePath.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    CHECK_HR_RETURN_NULL(hr);

    ComPtr<IWICBitmapFrameDecode> frameDecode;
    hr = decoder->GetFrame(0, &frameDecode);
    CHECK_HR_RETURN_NULL(hr);

    ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    CHECK_HR_RETURN_NULL(hr);

    hr = converter->Initialize(frameDecode.Get(), targetFormat,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    CHECK_HR_RETURN_NULL(hr);

    return converter;
}

// Apply rotation transformation to WIC bitmap source
ComPtr<IWICBitmapSource> App::ApplyWICRotation(IWICImagingFactory* wicFactory, IWICBitmapSource* source) {
    if (m_rotation == Rotation::NONE || !source) return source;

    ComPtr<IWICBitmapFlipRotator> rotator;
    HRESULT hr = wicFactory->CreateBitmapFlipRotator(&rotator);
    CHECK_HR_RETURN_NULL(hr);

    hr = rotator->Initialize(source, GetWICTransformForRotation(m_rotation));
    CHECK_HR_RETURN_NULL(hr);

    return rotator;
}

// Apply crop transformation to WIC bitmap source
ComPtr<IWICBitmapSource> App::ApplyWICCrop(IWICImagingFactory* wicFactory, IWICBitmapSource* source) {
    if (!m_hasCrop || !source) return source;

    ComPtr<IWICBitmapClipper> clipper;
    HRESULT hr = wicFactory->CreateBitmapClipper(&clipper);
    CHECK_HR_RETURN_NULL(hr);

    hr = clipper->Initialize(source, &m_appliedCrop);
    CHECK_HR_RETURN_NULL(hr);

    return clipper;
}

// Create WIC bitmap from source and render markup overlays
ComPtr<IWICBitmap> App::CreateWICBitmapWithOverlays(IWICImagingFactory* wicFactory, ID2D1Factory* d2dFactory, IWICBitmapSource* source) {
    if (!source) return nullptr;

    UINT width, height;
    HRESULT hr = source->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0) return nullptr;

    ComPtr<IWICBitmap> wicBitmap;
    hr = wicFactory->CreateBitmapFromSource(source, WICBitmapCacheOnLoad, &wicBitmap);
    CHECK_HR_RETURN_NULL(hr);

    // Render markup and text overlays
    ComPtr<ID2D1RenderTarget> renderTarget;
    D2D1_RENDER_TARGET_PROPERTIES renderTargetProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    hr = d2dFactory->CreateWicBitmapRenderTarget(wicBitmap.Get(), renderTargetProps, &renderTarget);
    CHECK_HR_RETURN_NULL(hr);

    renderTarget->BeginDraw();
    RenderMarkupAndTextToTarget(renderTarget.Get(), static_cast<float>(width), static_cast<float>(height),
        m_markupStrokes, m_textOverlays);
    hr = renderTarget->EndDraw();
    CHECK_HR_RETURN_NULL(hr);

    return wicBitmap;
}

// Initialize BITMAPINFOHEADER for 32-bit RGBA image (bottom-up format)
static void InitializeDIBHeader(BITMAPINFOHEADER& bi, UINT width, UINT height) {
    bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = height;  // Positive = bottom-up
    bi.biPlanes = App::BITMAP_PLANES;
    bi.biBitCount = App::BITMAP_BITS_PER_PIXEL;
    bi.biCompression = BI_RGB;
}

// Get fully transformed image with all overlays applied (rotation, crop, markup, text)
ComPtr<IWICBitmap> App::GetTransformedImageWithOverlays(IWICImagingFactory* wicFactory, ID2D1Factory* d2dFactory) {
    ComPtr<IWICBitmapSource> source = LoadAndDecodeImage(wicFactory, WIC_PIXEL_FORMAT_PREMULTIPLIED);
    if (!source) return nullptr;

    source = ApplyWICRotation(wicFactory, source.Get());
    if (!source) return nullptr;

    source = ApplyWICCrop(wicFactory, source.Get());
    if (!source) return nullptr;

    return CreateWICBitmapWithOverlays(wicFactory, d2dFactory, source.Get());
}

// Create DIB (Device Independent Bitmap) for clipboard from WIC bitmap
HGLOBAL App::CreateDIBFromBitmap(IWICBitmap* bitmap, UINT width, UINT height) {
    if (!bitmap) return nullptr;

    UINT stride = GetBitmapStride(width);
    std::vector<BYTE> buffer(stride * height);
    WICRect rcCopy = { 0, 0, (INT)width, (INT)height };
    HRESULT hr = bitmap->CopyPixels(&rcCopy, stride, (UINT)buffer.size(), buffer.data());
    if (FAILED(hr)) return nullptr;

    // Flip buffer to bottom-up for Windows clipboard compatibility
    std::vector<BYTE> flippedBuffer;
    FlipBufferVertically(buffer, flippedBuffer, width, height);

    // Create DIB header
    BITMAPINFOHEADER bi;
    InitializeDIBHeader(bi, width, height);

    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + flippedBuffer.size());
    if (!hDib) return nullptr;

    void* pDib = GlobalLock(hDib);
    if (pDib) {
        memcpy(pDib, &bi, sizeof(BITMAPINFOHEADER));
        memcpy(static_cast<BYTE*>(pDib) + sizeof(BITMAPINFOHEADER), flippedBuffer.data(), flippedBuffer.size());
        GlobalUnlock(hDib);
    }

    return hDib;
}

// Encode WIC bitmap to PNG format for clipboard
HGLOBAL App::EncodeBitmapToPNG(IWICImagingFactory* wicFactory, IWICBitmap* bitmap) {
    if (!bitmap) return nullptr;

    ComPtr<IStream> pngStream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &pngStream);
    CHECK_HR_RETURN_NULL(hr);

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    CHECK_HR_RETURN_NULL(hr);

    hr = encoder->Initialize(pngStream.Get(), WICBitmapEncoderNoCache);
    CHECK_HR_RETURN_NULL(hr);

    ComPtr<IWICBitmapFrameEncode> frameEncode;
    hr = encoder->CreateNewFrame(&frameEncode, nullptr);
    CHECK_HR_RETURN_NULL(hr);

    hr = frameEncode->Initialize(nullptr);
    CHECK_HR_RETURN_NULL(hr);

    UINT width, height;
    bitmap->GetSize(&width, &height);
    hr = frameEncode->SetSize(width, height);
    CHECK_HR_RETURN_NULL(hr);

    WICPixelFormatGUID pixelFormat = WIC_PIXEL_FORMAT_STRAIGHT_ALPHA;
    hr = frameEncode->SetPixelFormat(&pixelFormat);
    CHECK_HR_RETURN_NULL(hr);

    hr = frameEncode->WriteSource(bitmap, nullptr);
    CHECK_HR_RETURN_NULL(hr);

    hr = frameEncode->Commit();
    CHECK_HR_RETURN_NULL(hr);

    hr = encoder->Commit();
    CHECK_HR_RETURN_NULL(hr);

    // Get PNG data and copy to new HGLOBAL
    HGLOBAL hPng = nullptr;
    hr = GetHGlobalFromStream(pngStream.Get(), &hPng);
    if (FAILED(hr)) return nullptr;

    STATSTG stat;
    pngStream->Stat(&stat, STATFLAG_NONAME);
    SIZE_T pngSize = static_cast<SIZE_T>(stat.cbSize.QuadPart);

    HGLOBAL hPngCopy = GlobalAlloc(GMEM_MOVEABLE, pngSize);
    if (hPngCopy) {
        void* pPngSrc = GlobalLock(hPng);
        void* pPngDst = GlobalLock(hPngCopy);
        if (pPngSrc && pPngDst) {
            memcpy(pPngDst, pPngSrc, pngSize);
        }
        GlobalUnlock(hPng);
        GlobalUnlock(hPngCopy);
    }

    return hPngCopy;
}

// Create HBITMAP from pixel buffer for Windows clipboard history
HBITMAP App::CreateHBITMAPFromBuffer(const std::vector<BYTE>& buffer, UINT width, UINT height) {
    // First flip the buffer to bottom-up format
    std::vector<BYTE> flippedBuffer;
    FlipBufferVertically(buffer, flippedBuffer, width, height);

    BITMAPINFO bmi = {};
    InitializeDIBHeader(bmi.bmiHeader, width, height);

    HDC hdc = GetDC(nullptr);
    HBITMAP hBitmap = CreateDIBitmap(hdc, &bmi.bmiHeader, CBM_INIT,
        flippedBuffer.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);

    return hBitmap;
}

bool App::Initialize(HINSTANCE hInstance, int nCmdShow, const std::wstring& initialFile) {
    // Initialize COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        return false;
    }

    // Create components
    m_window = std::make_unique<Window>(this);
    m_renderer = std::make_unique<Renderer>();
    m_imageLoader = std::make_unique<ImageLoader>();
    m_imageCache = std::make_unique<ImageCache>();
    m_navigator = std::make_unique<FolderNavigator>();

    // Create window
    if (!m_window->Create(hInstance, nCmdShow)) {
        return false;
    }

    // Enable drag-drop
    DragAcceptFiles(m_window->GetHwnd(), TRUE);

    // Initialize renderer
    if (!m_renderer->Initialize(m_window->GetHwnd())) {
        return false;
    }

    // Initialize image loader
    m_imageLoader->Initialize(
        m_renderer->GetDeviceContext(),
        m_renderer->GetWICFactory()
    );

    // Initialize cache
    m_imageCache->Initialize(m_imageLoader.get());

    // Open initial file if provided
    if (!initialFile.empty()) {
        OpenFile(initialFile);
    }

    return true;
}

int App::Run() {
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void App::OpenFile(const std::wstring& filePath) {
    if (!ImageLoader::IsSupportedFormat(filePath)) {
        return;
    }

    m_navigator->SetCurrentFile(filePath);
    LoadCurrentImage();
    PrefetchAdjacentImages();
}

void App::LoadCurrentImage() {
    StopGifAnimation();

    // Reset all transformations when loading new image
    ClearEditState();

    std::wstring filePath = m_navigator->GetCurrentFilePath();
    if (filePath.empty()) {
        m_currentImage = nullptr;
        m_renderer->ClearImage();
        UpdateTitle();
        Invalidate();
        return;
    }

    // Try cache first
    m_currentImage = m_imageCache->Get(filePath);

    if (!m_currentImage) {
        // Load synchronously
        m_currentImage = m_imageLoader->LoadImage(filePath);

        // Note: Could add to cache here, but for simplicity we skip it
    }

    if (m_currentImage) {
        m_renderer->SetImage(m_currentImage->bitmap);

        // Start animation if GIF
        if (m_currentImage->isAnimated) {
            StartGifAnimation();
        }
    } else {
        m_renderer->ClearImage();
    }

    UpdateTitle();
    Invalidate();
}

void App::UpdateTitle() {
    std::wstring title = Window::GetDefaultTitle();

    if (m_currentImage && !m_currentImage->filePath.empty()) {
        fs::path path(m_currentImage->filePath);
        title = path.filename().wstring();

        // Add image info
        title += L" - " + std::to_wstring(m_currentImage->width) +
                 L" x " + std::to_wstring(m_currentImage->height);

        // Add position in folder
        title += L" [" + std::to_wstring(m_navigator->GetCurrentIndex() + 1) +
                 L"/" + std::to_wstring(m_navigator->GetTotalCount()) + L"]";

        // Add GIF pause indicator
        if (m_currentImage->isAnimated && m_gifPaused) {
            title += TITLE_SUFFIX_PAUSED;
        }
    }

    // Add edit mode indicator
    switch (m_editMode) {
    case EditMode::Crop:   title += TITLE_SUFFIX_CROP;   break;
    case EditMode::Markup: title += TITLE_SUFFIX_MARKUP; break;
    case EditMode::Text:   title += TITLE_SUFFIX_TEXT;   break;
    case EditMode::Erase:  title += TITLE_SUFFIX_ERASE;  break;
    default: break;
    }

    m_window->SetTitle(title);
}

void App::NavigateNext() {
    if (m_navigator->GoToNext()) {
        LoadCurrentImage();
        PrefetchAdjacentImages();
    }
}

void App::NavigatePrevious() {
    if (m_navigator->GoToPrevious()) {
        LoadCurrentImage();
        PrefetchAdjacentImages();
    }
}

void App::PrefetchAdjacentImages() {
    auto adjacent = m_navigator->GetAdjacentFiles(PREFETCH_ADJACENT_COUNT);
    m_imageCache->Prefetch(adjacent);
}

bool App::TryNavigateWithDelay(std::function<bool()> navigateFn) {
    DWORD now = GetTickCount();
    if (!m_isNavigating || (now - m_lastNavigateTime) >= NAVIGATE_DELAY_MS) {
        navigateFn();
        m_isNavigating = true;
        m_lastNavigateTime = now;
    }
    return true;
}

void App::NavigateFirst() {
    if (m_navigator->GoToFirst()) {
        LoadCurrentImage();
    }
}

void App::NavigateLast() {
    if (m_navigator->GoToLast()) {
        LoadCurrentImage();
    }
}

void App::ToggleFullscreen() {
    m_window->ToggleFullscreen();
}

void App::DeleteCurrentFile() {
    if (m_navigator->DeleteCurrentFile()) {
        LoadCurrentImage();
    }
}

void App::ZoomIn() {
    float zoom = m_renderer->GetZoom();
    m_renderer->SetZoom(zoom * ZOOM_FACTOR);
    Invalidate();
}

void App::ZoomOut() {
    float zoom = m_renderer->GetZoom();
    m_renderer->SetZoom(zoom / ZOOM_FACTOR);
    Invalidate();
}

void App::ResetZoom() {
    m_renderer->ResetView();
    Invalidate();
}

void App::SetActualSizeZoom() {
    m_renderer->SetZoom(CalculateActualSizeZoom());
    Invalidate();
}

float App::CalculateActualSizeZoom() const {
    if (!m_currentImage) return 1.0f;

    float fitScaleX = static_cast<float>(m_window->GetWidth()) / m_currentImage->width;
    float fitScaleY = static_cast<float>(m_window->GetHeight()) / m_currentImage->height;
    float fitScale = std::min(fitScaleX, fitScaleY);

    // Return zoom that counteracts the fit-to-window scale to show actual pixels
    return 1.0f / fitScale;
}

void App::StartGifAnimation() {
    if (!m_currentImage || !m_currentImage->isAnimated) {
        return;
    }

    m_gifPaused = false;
    m_currentImage->currentFrame = 0;

    UINT delay = m_currentImage->frameDelays.empty() ? DEFAULT_GIF_FRAME_DELAY_MS : m_currentImage->frameDelays[0];
    m_gifTimerId = SetTimer(m_window->GetHwnd(), GIF_TIMER_ID, delay, GifTimerProc);
}

void App::StopGifAnimation() {
    if (m_gifTimerId != 0) {
        KillTimer(m_window->GetHwnd(), m_gifTimerId);
        m_gifTimerId = 0;
    }
}

void App::AdvanceGifFrame() {
    if (!m_currentImage || !m_currentImage->isAnimated || m_gifPaused) {
        return;
    }

    // Advance to next frame
    m_currentImage->currentFrame++;
    if (m_currentImage->currentFrame >= m_currentImage->frames.size()) {
        m_currentImage->currentFrame = 0;
    }

    // Update bitmap
    if (m_currentImage->currentFrame < m_currentImage->frames.size()) {
        m_currentImage->bitmap = m_currentImage->frames[m_currentImage->currentFrame];
        m_renderer->SetImage(m_currentImage->bitmap);
        Invalidate();
    }

    // Schedule next frame
    UINT delay = DEFAULT_GIF_FRAME_DELAY_MS;
    if (m_currentImage->currentFrame < m_currentImage->frameDelays.size()) {
        delay = m_currentImage->frameDelays[m_currentImage->currentFrame];
    }

    KillTimer(m_window->GetHwnd(), m_gifTimerId);
    m_gifTimerId = SetTimer(m_window->GetHwnd(), GIF_TIMER_ID, delay, GifTimerProc);
}

void CALLBACK App::GifTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)hwnd; (void)msg; (void)id; (void)time;
    if (s_instance) {
        s_instance->AdvanceGifFrame();
    }
}

// Phase 2 feature implementations

void App::CopyToClipboard() {
    if (!m_currentImage || m_currentImage->filePath.empty()) return;

    auto wicFactory = m_renderer->GetWICFactory();
    auto d2dFactory = m_renderer->GetFactory();
    if (!wicFactory || !d2dFactory) return;

    // Get transformed image with all overlays applied
    ComPtr<IWICBitmap> wicBitmap = GetTransformedImageWithOverlays(wicFactory, d2dFactory);
    if (!wicBitmap) return;

    UINT width, height;
    wicBitmap->GetSize(&width, &height);

    // Create clipboard formats
    HGLOBAL hDib = CreateDIBFromBitmap(wicBitmap.Get(), width, height);
    HGLOBAL hPng = EncodeBitmapToPNG(wicFactory, wicBitmap.Get());

    // Get pixel buffer for HBITMAP creation
    UINT stride = GetBitmapStride(width);
    std::vector<BYTE> buffer(stride * height);
    WICRect rcCopy = { 0, 0, (INT)width, (INT)height };
    wicBitmap->CopyPixels(&rcCopy, stride, (UINT)buffer.size(), buffer.data());
    HBITMAP hBitmap = CreateHBITMAPFromBuffer(buffer, width, height);

    // Set clipboard with multiple formats
    if (OpenClipboard(m_window->GetHwnd())) {
        EmptyClipboard();

        // PNG format for browsers and modern apps
        UINT pngFormat = RegisterClipboardFormatW(PNG_CLIPBOARD_FORMAT);
        if (pngFormat && hPng) {
            SetClipboardData(pngFormat, hPng);
            hPng = nullptr;
        }

        // BITMAP format for Windows clipboard history
        if (hBitmap) {
            SetClipboardData(CF_BITMAP, hBitmap);
            hBitmap = nullptr;
        }

        // DIB format for traditional apps
        if (hDib) {
            SetClipboardData(CF_DIB, hDib);
            hDib = nullptr;
        }

        CloseClipboard();
    }

    // Clean up any handles not taken by clipboard
    if (hDib) GlobalFree(hDib);
    if (hPng) GlobalFree(hPng);
    if (hBitmap) DeleteObject(hBitmap);
}

// Helper to show file dialog and get the selected path
// Returns empty string if cancelled or failed
static std::wstring ShowFileOpenDialog(HWND hwnd, bool pickFolder,
    const COMDLG_FILTERSPEC* filters = nullptr, UINT filterCount = 0)
{
    ComPtr<IFileOpenDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return L"";

    if (pickFolder) {
        DWORD options;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS);
    } else if (filters && filterCount > 0) {
        dialog->SetFileTypes(filterCount, filters);
    }

    hr = dialog->Show(hwnd);
    if (FAILED(hr)) return L"";

    ComPtr<IShellItem> item;
    hr = dialog->GetResult(&item);
    if (FAILED(hr)) return L"";

    PWSTR path;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    if (FAILED(hr)) return L"";

    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

void App::SetAsWallpaper() {
    if (!m_currentImage || m_currentImage->filePath.empty()) return;

    // Copy original file to temp location (Windows wallpaper API works best with BMP/JPG)
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);

    fs::path srcPath(m_currentImage->filePath);
    std::wstring ext = srcPath.extension().wstring();
    std::wstring wallpaperPath = std::wstring(tempPath) + WALLPAPER_TEMP_PREFIX + ext;

    try {
        fs::copy_file(m_currentImage->filePath, wallpaperPath, fs::copy_options::overwrite_existing);
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
            const_cast<wchar_t*>(wallpaperPath.c_str()),
            SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    } catch (...) {
        // Copy failed
    }
}

void App::OpenFileDialog() {
    std::wstring filePath = ShowFileOpenDialog(m_window->GetHwnd(), false,
        OPEN_FILE_FILTERS, ARRAYSIZE(OPEN_FILE_FILTERS));
    if (!filePath.empty()) {
        OpenFile(filePath);
    }
}

void App::OpenFolderDialog() {
    std::wstring folderPath = ShowFileOpenDialog(m_window->GetHwnd(), true);
    if (folderPath.empty()) return;

    // Find first image in folder
    for (const auto& entry : fs::directory_iterator(folderPath)) {
        if (ImageLoader::IsSupportedFormat(entry.path().wstring())) {
            OpenFile(entry.path().wstring());
            break;
        }
    }
}

bool App::PromptSaveEditedImageDialog(bool& saveCopy) {
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = m_window->GetHwnd();
    config.dwFlags = TDF_USE_COMMAND_LINKS;
    config.pszWindowTitle = L"Save Image";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"The image has markups or crop applied.";
    config.pszContent = L"How would you like to save?";

    TASKDIALOG_BUTTON buttons[] = {
        { DIALOG_BUTTON_SAVE_COPY, L"Save Copy\nOriginal file preserved" },
        { DIALOG_BUTTON_OVERWRITE, L"Overwrite\nReplace original file" },
        { DIALOG_BUTTON_CANCEL, L"Cancel\nDon't save" }
    };
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);

    int clicked = 0;
    if (FAILED(TaskDialogIndirect(&config, &clicked, nullptr, nullptr))) return false;
    if (clicked == DIALOG_BUTTON_CANCEL) return false;

    saveCopy = (clicked == DIALOG_BUTTON_SAVE_COPY);
    return true;
}

void App::SaveImageAsCopy(const fs::path& origPath) {
    // Generate copy filename: image.jpg -> image_edited.jpg
    fs::path copyPath = origPath.parent_path() /
        (origPath.stem().wstring() + EDITED_FILE_SUFFIX + origPath.extension().wstring());

    // If file exists, add number: image_edited_2.jpg
    int counter = EDITED_FILE_COUNTER_START;
    while (fs::exists(copyPath)) {
        copyPath = origPath.parent_path() /
            (origPath.stem().wstring() + EDITED_FILE_SUFFIX + L"_" + std::to_wstring(counter++) + origPath.extension().wstring());
    }

    if (!SaveImageToFile(copyPath.wstring())) return;

    // Clear edits since they're now saved (keep rotation since it wasn't saved)
    ClearEditState(false);
    UpdateRendererMarkup();
    UpdateRendererText();
    Invalidate();

    FlashWindow(m_window->GetHwnd(), TRUE);
}

void App::SaveImageOverwrite(const fs::path& origPath, const std::wstring& savedFilePath) {
    std::wstring tempPath = GenerateTempPath(m_currentImage->filePath);

    // Save to temp file
    if (!SaveImageToFile(tempPath)) return;

    // Release current image so original file isn't locked
    m_currentImage->bitmap.Reset();
    m_currentImage = nullptr;
    m_renderer->ClearImage();

    // Replace original with temp
    try {
        fs::remove(origPath);
        fs::rename(tempPath, origPath);
    } catch (...) {
        try { fs::remove(tempPath); } catch (...) {}
    }

    // Reset transformations since they're now baked in
    ClearEditState();

    // Reload the image
    m_navigator->SetCurrentFile(savedFilePath);
    LoadCurrentImage();

    FlashWindow(m_window->GetHwnd(), TRUE);
}

void App::SaveImage() {
    if (!m_currentImage || m_currentImage->filePath.empty()) return;

    fs::path origPath(m_currentImage->filePath);
    std::wstring savedFilePath = m_currentImage->filePath;
    bool saveCopy = false;

    if (HasPendingEdits()) {
        if (!PromptSaveEditedImageDialog(saveCopy)) return;
    }

    if (saveCopy) {
        SaveImageAsCopy(origPath);
    } else {
        SaveImageOverwrite(origPath, savedFilePath);
    }
}

void App::SaveImageAs() {
    if (!m_currentImage || m_currentImage->filePath.empty()) return;

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&dialog));
    if (FAILED(hr)) return;

    // Get original extension
    fs::path srcPath(m_currentImage->filePath);
    std::wstring srcExt = srcPath.extension().wstring();

    dialog->SetFileTypes(ARRAYSIZE(SAVE_FILE_FILTERS), SAVE_FILE_FILTERS);

    // Set default filter based on original extension
    dialog->SetFileTypeIndex(GetSaveFilterIndexForExtension(srcExt));

    dialog->SetDefaultExtension(srcExt.empty() ? L"png" : srcExt.c_str() + 1);
    dialog->SetFileName(srcPath.stem().wstring().c_str());

    hr = dialog->Show(m_window->GetHwnd());
    if (SUCCEEDED(hr)) {
        ComPtr<IShellItem> item;
        hr = dialog->GetResult(&item);
        if (SUCCEEDED(hr)) {
            PWSTR filePath;
            hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
            if (SUCCEEDED(hr)) {
                // Save with transformations applied
                SaveImageToFile(filePath);
                CoTaskMemFree(filePath);
            }
        }
    }
}

// Get container format GUID from file extension
GUID App::GetContainerFormatForExtension(const std::wstring& ext) {
    std::wstring extLower = ToLowerCase(ext);

    if (extLower == L".jpg" || extLower == L".jpeg") {
        return GUID_ContainerFormatJpeg;
    } else if (extLower == L".bmp") {
        return GUID_ContainerFormatBmp;
    }
    return GUID_ContainerFormatPng;
}

// Get save dialog filter index from file extension
UINT App::GetSaveFilterIndexForExtension(const std::wstring& ext) {
    std::wstring extLower = ToLowerCase(ext);

    if (extLower == L".jpg" || extLower == L".jpeg") {
        return SAVE_FILTER_JPEG_INDEX;
    } else if (extLower == L".bmp") {
        return SAVE_FILTER_BMP_INDEX;
    }
    return SAVE_FILTER_PNG_INDEX;
}

// Generate temp file path for save operations
std::wstring App::GenerateTempPath(const std::wstring& originalPath) {
    fs::path origPath(originalPath);
    return (origPath.parent_path() / (TEMP_FILE_PREFIX + origPath.filename().wstring())).wstring();
}

// Encode WIC bitmap and save to file
bool App::EncodeAndSaveToFile(IWICImagingFactory* wicFactory, IWICBitmap* bitmap,
                               const std::wstring& filePath, GUID containerFormat) {
    if (!bitmap) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    HRESULT hr = wicFactory->CreateEncoder(containerFormat, nullptr, &encoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    hr = wicFactory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return false;

    // Set JPEG quality
    if (containerFormat == GUID_ContainerFormatJpeg && props) {
        PROPBAG2 option = {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = JPEG_SAVE_QUALITY;
        props->Write(1, &option, &value);
    }

    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) return false;

    UINT width, height;
    bitmap->GetSize(&width, &height);
    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID pixelFormat;
    bitmap->GetPixelFormat(&pixelFormat);
    hr = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(hr)) return false;

    hr = frame->WriteSource(bitmap, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

bool App::SaveImageToFile(const std::wstring& filePath) {
    if (!m_currentImage || m_currentImage->filePath.empty()) return false;

    auto wicFactory = m_renderer->GetWICFactory();
    auto d2dFactory = m_renderer->GetFactory();
    if (!wicFactory || !d2dFactory) return false;

    // Determine output format from file extension
    fs::path path(filePath);
    GUID containerFormat = GetContainerFormatForExtension(path.extension().wstring());

    // Get transformed image with all overlays applied
    ComPtr<IWICBitmap> wicBitmap = GetTransformedImageWithOverlays(wicFactory, d2dFactory);
    if (!wicBitmap) return false;

    return EncodeAndSaveToFile(wicFactory, wicBitmap.Get(), filePath, containerFormat);
}

void App::RotateCW() {
    RotateAndSaveImage(Rotation::CW_90);
}

void App::RotateCCW() {
    RotateAndSaveImage(Rotation::CW_270);
}

void App::RotateAndSaveImage(int rotationDelta) {
    if (!m_currentImage || m_currentImage->filePath.empty()) return;

    m_rotation = (m_rotation + rotationDelta) % Rotation::FULL_ROTATION;
    m_renderer->SetRotation(m_rotation);
    Invalidate();

    fs::path origPath(m_currentImage->filePath);
    std::wstring tempPath = GenerateTempPath(m_currentImage->filePath);
    std::wstring savedFilePath = m_currentImage->filePath;

    // Save current edit state (rotation saves without markups)
    EditState preRotationState = SaveCurrentEditState();
    m_markupStrokes.clear();
    m_textOverlays.clear();
    m_hasCrop = false;
    m_appliedCrop = {};

    if (SaveImageToFile(tempPath)) {
        m_currentImage->bitmap.Reset();
        m_currentImage = nullptr;
        m_renderer->ClearImage();

        try {
            fs::remove(origPath);
            fs::rename(tempPath, origPath);
        } catch (...) {
            try { fs::remove(tempPath); } catch (...) {}
        }

        m_rotation = Rotation::NONE;
        m_renderer->SetRotation(Rotation::NONE);
        m_navigator->SetCurrentFile(savedFilePath);
        LoadCurrentImage();
    }

    // Restore edit state
    RestoreEditState(preRotationState);
    UpdateRendererMarkup();
    UpdateRendererText();
}

void App::ToggleEditMode(EditMode mode) {
    m_editMode = (m_editMode == mode) ? EditMode::None : mode;

    // Mode-specific initialization
    if (mode == EditMode::Crop) {
        m_isCropDragging = false;
        m_renderer->SetCropMode(m_editMode == EditMode::Crop);
    }

    UpdateTitle();
    Invalidate();
}

void App::CancelCurrentMode() {
    m_editMode = EditMode::None;
    m_isCropDragging = false;
    m_renderer->SetCropMode(false);
    m_renderer->SetCropRect(D2D1::RectF(0, 0, 0, 0));
    UpdateTitle();
    Invalidate();
}

void App::UpdateRendererMarkup() {
    m_renderer->SetMarkupStrokes(m_markupStrokes);
}

void App::UpdateRendererText() {
    // Start with existing overlays
    std::vector<TextOverlay> overlays = m_textOverlays;

    // Add editing text with cursor
    if (m_isEditingText) {
        D2D1_RECT_F imageRect = m_renderer->GetScreenImageRect();
        float imageW = imageRect.right - imageRect.left;

        TextOverlay cursorOverlay;
        cursorOverlay.text = m_editingText + L"|";
        cursorOverlay.x = m_editingTextX;
        cursorOverlay.y = m_editingTextY;
        cursorOverlay.color = Colors::WHITE;
        cursorOverlay.fontSize = DEFAULT_TEXT_FONT_SIZE / imageW;
        overlays.push_back(cursorOverlay);
    }

    m_renderer->SetTextOverlays(overlays);
}

bool App::ScreenToNormalizedImageCoords(int screenX, int screenY, float& normX, float& normY) const {
    D2D1_RECT_F imageRect = m_renderer->GetScreenImageRect();
    float imageW = imageRect.right - imageRect.left;
    float imageH = imageRect.bottom - imageRect.top;

    if (imageW <= 0 || imageH <= 0) return false;

    normX = (static_cast<float>(screenX) - imageRect.left) / imageW;
    normY = (static_cast<float>(screenY) - imageRect.top) / imageH;
    return true;
}

// Check if a point is within hit radius of any point in a stroke
static bool IsPointNearStroke(const App::MarkupStroke& stroke, float normX, float normY, float hitRadius) {
    float hitRadiusSq = hitRadius * hitRadius;
    for (const auto& pt : stroke.points) {
        float dx = pt.x - normX;
        float dy = pt.y - normY;
        if (dx * dx + dy * dy < hitRadiusSq) {
            return true;
        }
    }
    return false;
}

// Check if a point is within the text overlay's hit box
static bool IsPointNearText(const App::TextOverlay& text, float normX, float normY,
                            float hitRadius, float textHitBoxWidth) {
    float dx = text.x - normX;
    float dy = text.y - normY;
    return dx > -hitRadius && dx < textHitBoxWidth && dy > -hitRadius && dy < hitRadius * 2;
}

void App::EraseAtPoint(int x, int y) {
    float normX, normY;
    if (!ScreenToNormalizedImageCoords(x, y, normX, normY)) return;

    D2D1_RECT_F imageRect = m_renderer->GetScreenImageRect();
    float imageW = imageRect.right - imageRect.left;
    float imageH = imageRect.bottom - imageRect.top;
    float hitRadius = ERASE_HIT_RADIUS_PIXELS / std::min(imageW, imageH);

    bool erased = false;

    // Erase strokes that intersect with click point
    for (auto it = m_markupStrokes.begin(); it != m_markupStrokes.end(); ) {
        if (IsPointNearStroke(*it, normX, normY, hitRadius)) {
            it = m_markupStrokes.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }

    // Erase text overlays that intersect with click point
    for (auto it = m_textOverlays.begin(); it != m_textOverlays.end(); ) {
        if (IsPointNearText(*it, normX, normY, hitRadius, TEXT_HIT_BOX_WIDTH)) {
            it = m_textOverlays.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }

    if (erased) {
        UpdateRendererMarkup();
        UpdateRendererText();
        Invalidate();
    }
}

void App::PushUndoState() {
    m_undoStack.push_back(SaveCurrentEditState());

    // Limit undo stack size
    if (m_undoStack.size() > MAX_UNDO_LEVELS) {
        m_undoStack.erase(m_undoStack.begin());
    }
}

void App::Undo() {
    if (m_undoStack.empty()) return;

    EditState state = m_undoStack.back();
    m_undoStack.pop_back();

    // Check if we're undoing a crop (need to reload image)
    bool wasCropped = m_hasCrop;
    bool willBeCropped = state.hasCrop;

    RestoreEditState(state);

    // If undoing a crop, reload the original image
    if (wasCropped && !willBeCropped && m_currentImage) {
        std::wstring filePath = m_currentImage->filePath;
        m_currentImage = m_imageLoader->LoadImage(filePath);
        if (m_currentImage) {
            m_renderer->SetImage(m_currentImage->bitmap);
        }
    }

    UpdateRendererMarkup();
    UpdateRendererText();
    Invalidate();
}

bool App::HasPendingEdits() const {
    return !m_markupStrokes.empty() || !m_textOverlays.empty() || m_hasCrop;
}

App::EditState App::SaveCurrentEditState() const {
    EditState state;
    state.strokes = m_markupStrokes;
    state.texts = m_textOverlays;
    state.hasCrop = m_hasCrop;
    state.appliedCrop = m_appliedCrop;
    return state;
}

void App::RestoreEditState(const EditState& state) {
    m_markupStrokes = state.strokes;
    m_textOverlays = state.texts;
    m_hasCrop = state.hasCrop;
    m_appliedCrop = state.appliedCrop;
}

void App::ClearEditState(bool clearRotation) {
    if (clearRotation) {
        m_rotation = Rotation::NONE;
        m_renderer->SetRotation(Rotation::NONE);
    }
    m_hasCrop = false;
    m_appliedCrop = {};
    m_markupStrokes.clear();
    m_textOverlays.clear();
    m_undoStack.clear();
}

void App::Invalidate() {
    InvalidateRect(m_window->GetHwnd(), nullptr, FALSE);
}

void App::ApplyCrop() {
    if (m_editMode != EditMode::Crop || !m_currentImage) return;

    // Get crop rect in image coordinates
    D2D1_RECT_F cropRect = m_renderer->GetCropRectInImageCoords();
    if (cropRect.right <= cropRect.left || cropRect.bottom <= cropRect.top) return;

    // Set up crop transformation parameters
    CropTransformParams params;
    params.originalWidth = static_cast<float>(m_currentImage->width);
    params.originalHeight = static_cast<float>(m_currentImage->height);
    params.cropRectX = static_cast<int>(cropRect.left);
    params.cropRectY = static_cast<int>(cropRect.top);
    params.cropRectWidth = static_cast<int>(cropRect.right - cropRect.left);
    params.cropRectHeight = static_cast<int>(cropRect.bottom - cropRect.top);

    if (params.cropRectWidth <= 0 || params.cropRectHeight <= 0) return;

    float scaleFactor = params.originalWidth / params.cropRectWidth;

    // Transform markup and text to new cropped coordinate space
    m_markupStrokes = TransformMarkupStrokesForCrop(m_markupStrokes, params, scaleFactor);
    m_textOverlays = TransformTextOverlaysForCrop(m_textOverlays, params, scaleFactor);

    // Store crop for saving
    m_hasCrop = true;
    m_appliedCrop = { params.cropRectX, params.cropRectY, params.cropRectWidth, params.cropRectHeight };

    auto deviceContext = m_renderer->GetDeviceContext();

    // Create cropped bitmap for display
    D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    ComPtr<ID2D1Bitmap1> croppedBitmap;
    HRESULT hr = deviceContext->CreateBitmap(D2D1::SizeU(params.cropRectWidth, params.cropRectHeight), nullptr, 0, bitmapProps, &croppedBitmap);
    if (FAILED(hr)) return;

    D2D1_POINT_2U destPoint = {0, 0};
    D2D1_RECT_U srcRect = {(UINT32)params.cropRectX, (UINT32)params.cropRectY,
                           (UINT32)(params.cropRectX + params.cropRectWidth), (UINT32)(params.cropRectY + params.cropRectHeight)};
    hr = croppedBitmap->CopyFromBitmap(&destPoint, m_currentImage->bitmap.Get(), &srcRect);
    if (FAILED(hr)) return;

    // Update current image
    m_currentImage->bitmap = croppedBitmap;
    m_currentImage->width = params.cropRectWidth;
    m_currentImage->height = params.cropRectHeight;

    m_renderer->SetImage(m_currentImage->bitmap);
    UpdateRendererMarkup();
    UpdateRendererText();
    CancelCurrentMode();
}

bool App::HandleTextEditingKey(UINT key) {
    if (!m_isEditingText) return false;

    switch (key) {
    case VK_ESCAPE:
        m_isEditingText = false;
        m_editingText.clear();
        UpdateRendererText();
        Invalidate();
        return true;

    case VK_RETURN:
        if (!m_editingText.empty()) {
            PushUndoState();
            D2D1_RECT_F imageRect = m_renderer->GetScreenImageRect();
            float imageW = imageRect.right - imageRect.left;

            TextOverlay text;
            text.x = m_editingTextX;
            text.y = m_editingTextY;
            text.text = m_editingText;
            text.color = Colors::WHITE;
            text.fontSize = DEFAULT_TEXT_FONT_SIZE / imageW;
            m_textOverlays.push_back(text);
        }
        m_isEditingText = false;
        m_editingText.clear();
        UpdateRendererText();
        Invalidate();
        return true;

    case VK_BACK:
        if (!m_editingText.empty()) {
            m_editingText.pop_back();
            UpdateRendererText();
            Invalidate();
        }
        return true;

    default:
        return true;  // Block other keys during text editing
    }
}

bool App::HandleNavigationKey(UINT key, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;

    switch (key) {
    case VK_RIGHT:
        return TryNavigateWithDelay([this]() { NavigateNext(); return true; });

    case VK_LEFT:
        return TryNavigateWithDelay([this]() { NavigatePrevious(); return true; });

    case VK_HOME:
        NavigateFirst();
        return true;

    case VK_END:
        NavigateLast();
        return true;

    case VK_SPACE:
        if (m_currentImage && m_currentImage->isAnimated) {
            m_gifPaused = !m_gifPaused;
            UpdateTitle();
        }
        return true;

    case VK_DELETE:
        DeleteCurrentFile();
        return true;

    default:
        return false;
    }
}

bool App::HandleZoomKey(UINT key, bool ctrl) {
    switch (key) {
    case VK_OEM_PLUS:
    case VK_ADD:
        ZoomIn();
        return true;

    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        ZoomOut();
        return true;

    case 'F':
        if (ctrl) {
            OpenFolderDialog();
        } else {
            ResetZoom();
        }
        return true;

    case '1':
        SetActualSizeZoom();
        return true;

    default:
        return false;
    }
}

bool App::HandleEditModeKey(UINT key, bool ctrl, bool shift) {
    (void)shift;
    switch (key) {
    case VK_ESCAPE:
        if (m_editMode != EditMode::None) {
            CancelCurrentMode();
            return true;
        }
        if (m_window->IsFullscreen()) {
            ToggleFullscreen();
            return true;
        }
        PostQuitMessage(0);
        return true;

    case VK_RETURN:
        if (m_editMode == EditMode::Crop) {
            PushUndoState();
            ApplyCrop();
            return true;
        }
        return false;

    case VK_F11:
        ToggleFullscreen();
        return true;

    case 'C':
        if (ctrl) {
            CopyToClipboard();
        } else {
            ToggleEditMode(EditMode::Crop);
        }
        return true;

    case 'M':
        ToggleEditMode(EditMode::Markup);
        return true;

    case 'T':
        ToggleEditMode(EditMode::Text);
        return true;

    case 'E':
        ToggleEditMode(EditMode::Erase);
        return true;

    case 'R':
        if (shift) {
            RotateCCW();
        } else {
            RotateCW();
        }
        return true;

    case 'Z':
        if (ctrl) {
            Undo();
            return true;
        }
        return false;

    default:
        return false;
    }
}

bool App::HandleFileOperationKey(UINT key, bool ctrl, bool shift) {
    switch (key) {
    case 'B':
        if (ctrl) {
            SetAsWallpaper();
            return true;
        }
        return false;

    case 'O':
        if (ctrl) {
            OpenFileDialog();
            return true;
        }
        return false;

    case 'S':
        if (ctrl && shift) {
            SaveImageAs();
        } else if (ctrl) {
            SaveImage();
        } else {
            return false;
        }
        return true;

    case 'Q':
        if (ctrl) {
            PostQuitMessage(0);
            return true;
        }
        return false;

    case 'W':
        if (ctrl) {
            m_currentImage = nullptr;
            m_renderer->ClearImage();
            m_navigator->Clear();
            UpdateTitle();
            Invalidate();
            return true;
        }
        return false;

    default:
        return false;
    }
}

void App::OnKeyDown(UINT key) {
    // Text editing mode takes priority
    if (m_isEditingText && HandleTextEditingKey(key)) {
        return;
    }

    bool ctrl = IsKeyPressed(VK_CONTROL);
    bool shift = IsKeyPressed(VK_SHIFT);

    // Try each handler in order of priority
    if (HandleNavigationKey(key, ctrl, shift)) return;
    if (HandleZoomKey(key, ctrl)) return;
    if (HandleEditModeKey(key, ctrl, shift)) return;
    if (HandleFileOperationKey(key, ctrl, shift)) return;
}

void App::OnKeyUp(UINT key) {
    switch (key) {
    case VK_RIGHT:
    case VK_LEFT:
        m_isNavigating = false;
        break;
    }
}

void App::OnChar(wchar_t ch) {
    if (m_isEditingText) {
        // Ignore control characters (handled by OnKeyDown)
        if (ch >= MIN_PRINTABLE_CHAR) {
            m_editingText += ch;
            UpdateRendererText();
            Invalidate();
        }
    }
}

void App::OnMouseWheel(int delta) {
    if (delta > 0) {
        ZoomIn();
    } else {
        ZoomOut();
    }
}

// Mouse down handlers by mode
void App::HandleCropMouseDown(int x, int y) {
    m_isCropDragging = true;
    m_cropStartX = x;
    m_cropStartY = y;
    m_cropEndX = x;
    m_cropEndY = y;
    SetCapture(m_window->GetHwnd());
}

void App::HandleMarkupMouseDown(int x, int y) {
    float normX, normY;
    if (!ScreenToNormalizedImageCoords(x, y, normX, normY)) return;

    D2D1_RECT_F imageRect = m_renderer->GetScreenImageRect();
    float imageW = imageRect.right - imageRect.left;

    PushUndoState();
    m_isDrawing = true;
    MarkupStroke stroke;
    stroke.color = Colors::RED;
    stroke.width = MARKUP_STROKE_WIDTH_PIXELS / imageW;
    stroke.points.push_back(D2D1::Point2F(normX, normY));
    m_markupStrokes.push_back(stroke);
    UpdateRendererMarkup();
    SetCapture(m_window->GetHwnd());
}

void App::HandleTextMouseDown(int x, int y) {
    float normX, normY;
    if (!ScreenToNormalizedImageCoords(x, y, normX, normY)) return;

    m_isEditingText = true;
    m_editingText.clear();
    m_editingTextX = normX;
    m_editingTextY = normY;
    UpdateRendererText();
    Invalidate();
}

void App::HandleEraseMouseDown(int x, int y) {
    PushUndoState();
    m_isErasing = true;
    SetCapture(m_window->GetHwnd());
    EraseAtPoint(x, y);
}

void App::HandlePanMouseDown(int x, int y) {
    m_isPanning = true;
    m_lastMouseX = x;
    m_lastMouseY = y;
    SetCapture(m_window->GetHwnd());
}

void App::OnMouseDown(int x, int y) {
    switch (m_editMode) {
    case EditMode::Crop:   HandleCropMouseDown(x, y);   break;
    case EditMode::Markup: HandleMarkupMouseDown(x, y); break;
    case EditMode::Text:   HandleTextMouseDown(x, y);   break;
    case EditMode::Erase:  HandleEraseMouseDown(x, y);  break;
    default:               HandlePanMouseDown(x, y);    break;
    }
}

void App::OnMouseUp(int x, int y) {
    (void)x; (void)y;
    if (m_isCropDragging) {
        m_isCropDragging = false;
        ReleaseCapture();
    } else if (m_isDrawing) {
        m_isDrawing = false;
        ReleaseCapture();
    } else if (m_isErasing) {
        m_isErasing = false;
        ReleaseCapture();
    } else {
        m_isPanning = false;
        ReleaseCapture();
    }
}

// Mouse move handlers by mode
void App::HandleCropMouseMove(int x, int y) {
    m_cropEndX = x;
    m_cropEndY = y;
    float left = static_cast<float>(std::min(m_cropStartX, m_cropEndX));
    float top = static_cast<float>(std::min(m_cropStartY, m_cropEndY));
    float right = static_cast<float>(std::max(m_cropStartX, m_cropEndX));
    float bottom = static_cast<float>(std::max(m_cropStartY, m_cropEndY));
    m_renderer->SetCropRect(D2D1::RectF(left, top, right, bottom));
    Invalidate();
}

void App::HandleMarkupMouseMove(int x, int y) {
    if (m_markupStrokes.empty()) return;

    float normX, normY;
    if (!ScreenToNormalizedImageCoords(x, y, normX, normY)) return;

    m_markupStrokes.back().points.push_back(D2D1::Point2F(normX, normY));
    UpdateRendererMarkup();
    Invalidate();
}

void App::HandleEraseMouseMove(int x, int y) {
    EraseAtPoint(x, y);
}

void App::HandlePanMouseMove(int x, int y) {
    float dx = static_cast<float>(x - m_lastMouseX);
    float dy = static_cast<float>(y - m_lastMouseY);
    m_renderer->AddPan(dx, dy);
    m_lastMouseX = x;
    m_lastMouseY = y;
    Invalidate();
}

void App::OnMouseMove(int x, int y) {
    if (m_isCropDragging) {
        HandleCropMouseMove(x, y);
    } else if (m_isDrawing) {
        HandleMarkupMouseMove(x, y);
    } else if (m_isErasing) {
        HandleEraseMouseMove(x, y);
    } else if (m_isPanning) {
        HandlePanMouseMove(x, y);
    }
}

void App::OnResize(int width, int height) {
    if (m_renderer) {
        m_renderer->Resize(width, height);
        Invalidate();
    }
}

void App::Render() {
    if (m_renderer) {
        m_renderer->Render();
    }
}

void App::ShowContextMenu(int screenX, int screenY) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    bool hasImage = (m_currentImage != nullptr);
    bool inEditMode = (m_editMode != EditMode::None);
    UINT imageFlag = hasImage ? MF_ENABLED : MF_GRAYED;
    UINT editSafeFlag = (hasImage && !inEditMode) ? MF_ENABLED : MF_GRAYED;

    // File section
    AppendMenuW(menu, MF_STRING, CMD_OPEN_IMAGE, L"Open Image...\tCtrl+O");
    AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, L"Open Folder...\tCtrl+F");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Save section
    AppendMenuW(menu, MF_STRING | imageFlag, CMD_SAVE, L"Save\tCtrl+S");
    AppendMenuW(menu, MF_STRING | imageFlag, CMD_SAVE_AS, L"Save As...\tCtrl+Shift+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Clipboard section
    AppendMenuW(menu, MF_STRING | imageFlag, CMD_COPY_CLIPBOARD, L"Copy to Clipboard\tCtrl+C");
    AppendMenuW(menu, MF_STRING | editSafeFlag, CMD_SET_WALLPAPER, L"Set as Wallpaper\tCtrl+B");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Transform section
    AppendMenuW(menu, MF_STRING | editSafeFlag, CMD_ROTATE_CW, L"Rotate CW\tR");
    AppendMenuW(menu, MF_STRING | editSafeFlag, CMD_ROTATE_CCW, L"Rotate CCW\tShift+R");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // View section
    AppendMenuW(menu, MF_STRING | imageFlag, CMD_FIT_TO_WINDOW, L"Fit to Window\tF");
    AppendMenuW(menu, MF_STRING | imageFlag, CMD_ACTUAL_SIZE, L"Actual Size\t1");
    AppendMenuW(menu, MF_STRING, CMD_FULLSCREEN, L"Fullscreen\tF11");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Delete
    AppendMenuW(menu, MF_STRING | editSafeFlag, CMD_DELETE, L"Delete\tDel");

    // Handle keyboard invocation (Shift+F10 sends -1, -1)
    if (screenX == -1 && screenY == -1) {
        POINT pt;
        GetCursorPos(&pt);
        screenX = pt.x;
        screenY = pt.y;
    }

    HWND hwnd = m_window->GetHwnd();
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void App::OnContextMenuCommand(UINT commandId) {
    switch (commandId) {
    case CMD_OPEN_IMAGE:    OpenFileDialog(); break;
    case CMD_OPEN_FOLDER:   OpenFolderDialog(); break;
    case CMD_SAVE:          SaveImage(); break;
    case CMD_SAVE_AS:       SaveImageAs(); break;
    case CMD_COPY_CLIPBOARD: CopyToClipboard(); break;
    case CMD_SET_WALLPAPER: SetAsWallpaper(); break;
    case CMD_ROTATE_CW:     RotateCW(); break;
    case CMD_ROTATE_CCW:    RotateCCW(); break;
    case CMD_FIT_TO_WINDOW: ResetZoom(); break;
    case CMD_ACTUAL_SIZE:   SetActualSizeZoom(); break;
    case CMD_FULLSCREEN:    ToggleFullscreen(); break;
    case CMD_DELETE:        DeleteCurrentFile(); break;
    }
}
