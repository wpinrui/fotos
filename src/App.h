#pragma once
#include "pch.h"
#include "Renderer.h"
#include "ImageLoader.h"

// Forward declarations
class Window;
class ImageCache;
class FolderNavigator;

class App {
public:
    App();
    ~App();

    bool Initialize(HINSTANCE hInstance, int nCmdShow, const std::wstring& initialFile = L"");
    int Run();

    // Event handlers (called by Window)
    void OnKeyDown(UINT key);
    void OnKeyUp(UINT key);
    void OnChar(wchar_t ch);
    void OnMouseWheel(int delta);
    void OnMouseDown(int x, int y);
    void OnMouseUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnResize(int width, int height);
    void Render();
    void ShowContextMenu(int screenX, int screenY);
    bool OnContextMenuCommand(UINT commandId);
    bool IsCursorHidden() const { return m_cursorHidden; }

    // File operations
    void OpenFile(const std::wstring& filePath);

    // Use Renderer's types to avoid duplication
    using MarkupStroke = Renderer::MarkupStroke;
    using TextOverlay = Renderer::TextOverlay;

    // Image format constants (public for helper function access)
    static constexpr UINT RGBA_BYTES_PER_PIXEL = 4;
    static constexpr size_t MIN_STROKE_POINTS = 2;  // Minimum points to form a valid stroke
    static constexpr WORD BITMAP_BITS_PER_PIXEL = 32;
    static constexpr WORD BITMAP_PLANES = 1;

    // Helper to calculate bitmap stride (width * bytes per pixel)
    static UINT GetBitmapStride(UINT width) { return width * RGBA_BYTES_PER_PIXEL; }

    // EditState struct (public because used in public-accessible helper methods)
    struct EditState {
        std::vector<MarkupStroke> strokes;
        std::vector<TextOverlay> texts;
        bool hasCrop;
        WICRect appliedCrop;
    };

private:
    // Edit modes (declared early for use in method signatures)
    enum class EditMode { None, Crop, Markup, Text, Erase };

    // Context menu command IDs
    enum ContextMenuCommand : UINT {
        CMD_OPEN_IMAGE = 1001,
        CMD_OPEN_FOLDER,
        CMD_SAVE,
        CMD_SAVE_AS,
        CMD_COPY_CLIPBOARD,
        CMD_SET_WALLPAPER,
        CMD_ROTATE_CW,
        CMD_ROTATE_CCW,
        CMD_FIT_TO_WINDOW,
        CMD_ACTUAL_SIZE,
        CMD_FULLSCREEN,
        CMD_DELETE,
        CMD_NAVIGATE_PREV = 1020,
        CMD_NAVIGATE_NEXT,
    };

    // Toolbar button enable conditions
    enum class EnableFlag { AlwaysEnabled, NeedsImage, NeedsImageNoEdit };

    struct ToolbarButtonDef {
        std::wstring label;
        UINT commandId;
        EnableFlag enableFlag;
        bool isSeparator = false;
    };

    void LoadCurrentImage();
    void UpdateTitle();
    void NavigateNext();
    void NavigatePrevious();
    void PrefetchAdjacentImages();
    bool TryNavigateWithDelay(std::function<bool()> navigateFn);
    void NavigateFirst();
    void NavigateLast();
    void ToggleFullscreen();
    void DeleteCurrentFile();
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();
    void SetActualSizeZoom();
    float CalculateActualSizeZoom() const;

    // Phase 2 features
    void CopyToClipboard();
    void SetAsWallpaper();
    void OpenFileDialog();
    void OpenFolderDialog();
    void SaveImage();
    void SaveImageAs();
    bool PromptSaveEditedImageDialog(bool& saveCopy);
    void SaveImageAsCopy(const fs::path& origPath);
    void SaveImageOverwrite(const fs::path& origPath, const std::wstring& savedFilePath);
    void RotateCW();
    void RotateCCW();
    void RotateAndSaveImage(int rotationDelta);  // Shared rotation logic
    void ToggleEditMode(EditMode mode);  // Unified edit mode toggle
    void CancelCurrentMode();
    void ApplyCrop();

    // Image saving helper
    bool SaveImageToFile(const std::wstring& filePath);

    // WIC transformation helpers (shared by clipboard and save operations)
    ComPtr<IWICBitmapSource> LoadAndDecodeImage(IWICImagingFactory* wicFactory, WICPixelFormatGUID targetFormat);
    ComPtr<IWICBitmapSource> ApplyWICRotation(IWICImagingFactory* wicFactory, IWICBitmapSource* source);
    ComPtr<IWICBitmapSource> ApplyWICCrop(IWICImagingFactory* wicFactory, IWICBitmapSource* source);
    ComPtr<IWICBitmap> CreateWICBitmapWithOverlays(IWICImagingFactory* wicFactory, ID2D1Factory* d2dFactory, IWICBitmapSource* source);
    ComPtr<IWICBitmap> GetTransformedImageWithOverlays(IWICImagingFactory* wicFactory, ID2D1Factory* d2dFactory);

    // Clipboard helpers
    HGLOBAL CreateDIBFromBitmap(IWICBitmap* bitmap, UINT width, UINT height);
    HGLOBAL EncodeBitmapToPNG(IWICImagingFactory* wicFactory, IWICBitmap* bitmap);
    HBITMAP CreateHBITMAPFromBuffer(const std::vector<BYTE>& buffer, UINT width, UINT height);

    // File encoding helpers
    static GUID GetContainerFormatForExtension(const std::wstring& ext);
    static UINT GetSaveFilterIndexForExtension(const std::wstring& ext);
    static std::wstring GenerateTempPath(const std::wstring& originalPath);
    bool EncodeAndSaveToFile(IWICImagingFactory* wicFactory, IWICBitmap* bitmap,
                             const std::wstring& filePath, GUID containerFormat);

    // Update renderer with current markup/text
    void UpdateRendererMarkup();
    void UpdateRendererText();
    void EraseAtPoint(int x, int y);

    // Coordinate transformation helper
    bool ScreenToNormalizedImageCoords(int screenX, int screenY, float& normX, float& normY) const;

    // Keyboard state helper
    static bool IsKeyPressed(int vk) { return (GetKeyState(vk) & KEY_DOWN_BIT) != 0; }

    // Window invalidation helper (DRY for repeated InvalidateRect calls)
    void Invalidate();

    // Edit state management
    void ClearEditState(bool clearRotation = true);
    void PushUndoState();
    void Undo();
    bool HasPendingEdits() const;
    EditState SaveCurrentEditState() const;
    void RestoreEditState(const EditState& state);

    // GIF animation
    void StartGifAnimation();
    void StopGifAnimation();
    void AdvanceGifFrame();
    static void CALLBACK GifTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

    // Keyboard handlers (extracted from OnKeyDown for clarity)
    bool HandleTextEditingKey(UINT key);
    bool HandleNavigationKey(UINT key, bool ctrl, bool shift);
    bool HandleZoomKey(UINT key, bool ctrl);
    bool HandleEditModeKey(UINT key, bool ctrl, bool shift);
    bool HandleFileOperationKey(UINT key, bool ctrl, bool shift);

    // Mouse handlers by mode (extracted from OnMouseDown/OnMouseMove for clarity)
    void HandleCropMouseDown(int x, int y);
    void HandleMarkupMouseDown(int x, int y);
    void HandleTextMouseDown(int x, int y);
    void HandleEraseMouseDown(int x, int y);
    void HandlePanMouseDown(int x, int y);
    void HandleCropMouseMove(int x, int y);
    void HandleMarkupMouseMove(int x, int y);
    void HandleEraseMouseMove(int x, int y);
    void HandlePanMouseMove(int x, int y);

    // Toolbar
    void InitToolbarButtons();
    void UpdateToolbarRenderData();
    int HitTestToolbar(int x, int y) const;
    void ShowToolbar();
    void HideToolbar();
    void ResetToolbarTimer();
    static void CALLBACK ToolbarTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<ImageLoader> m_imageLoader;
    std::unique_ptr<ImageCache> m_imageCache;
    std::unique_ptr<FolderNavigator> m_navigator;

    // Current image
    std::shared_ptr<ImageData> m_currentImage;

    // GIF animation
    UINT_PTR m_gifTimerId = 0;
    bool m_gifPaused = false;
    static App* s_instance; // For timer callback

    // Mouse state for panning
    bool m_isPanning = false;
    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
    float m_panStartX = 0.0f;
    float m_panStartY = 0.0f;

    // Navigation key repeat handling
    bool m_isNavigating = false;
    DWORD m_lastNavigateTime = 0;
    static const DWORD NAVIGATE_DELAY_MS = 50;  // Fast navigation when holding key

    // Image prefetch settings
    static constexpr int PREFETCH_ADJACENT_COUNT = 3;

    // GIF animation constants
    static constexpr UINT_PTR GIF_TIMER_ID = 1;
    static constexpr UINT DEFAULT_GIF_FRAME_DELAY_MS = 100;
    static constexpr float DEFAULT_TEXT_FONT_SIZE = 24.0f;
    static constexpr float ERASE_HIT_RADIUS_PIXELS = 30.0f;
    static constexpr float ZOOM_FACTOR = 1.25f;
    static constexpr float MARKUP_STROKE_WIDTH_PIXELS = 3.0f;
    static constexpr float TEXT_HIT_BOX_WIDTH = 0.2f;
    static constexpr float JPEG_SAVE_QUALITY = 0.9f;

    // File naming constants
    static constexpr wchar_t WALLPAPER_TEMP_PREFIX[] = L"fotos_wallpaper";
    static constexpr wchar_t PNG_CLIPBOARD_FORMAT[] = L"PNG";
    static constexpr wchar_t EDITED_FILE_SUFFIX[] = L"_edited";
    static constexpr wchar_t TEMP_FILE_PREFIX[] = L"~temp_";
    static constexpr int EDITED_FILE_COUNTER_START = 2;

    // UI constants
    static constexpr wchar_t MIN_PRINTABLE_CHAR = 32;

    // Keyboard state constants
    static constexpr WORD KEY_DOWN_BIT = 0x8000;  // High bit indicates key is pressed

    // Dialog button IDs
    static constexpr int DIALOG_BUTTON_SAVE_COPY = 100;
    static constexpr int DIALOG_BUTTON_OVERWRITE = 101;
    static constexpr int DIALOG_BUTTON_CANCEL = 102;

    // Dialog filter specifications
    static inline const COMDLG_FILTERSPEC OPEN_FILE_FILTERS[] = {
        { L"Image Files", L"*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tiff;*.tif;*.webp;*.heic;*.heif" },
        { L"All Files", L"*.*" }
    };
    static inline const COMDLG_FILTERSPEC SAVE_FILE_FILTERS[] = {
        { L"PNG Image", L"*.png" },
        { L"JPEG Image", L"*.jpg;*.jpeg" },
        { L"BMP Image", L"*.bmp" },
        { L"All Files", L"*.*" }
    };

    // Save dialog filter indices (1-indexed for SetFileTypeIndex)
    static constexpr UINT SAVE_FILTER_PNG_INDEX = 1;
    static constexpr UINT SAVE_FILTER_JPEG_INDEX = 2;
    static constexpr UINT SAVE_FILTER_BMP_INDEX = 3;

    // Edit mode title suffixes (shown in window title when mode is active)
    static constexpr wchar_t TITLE_SUFFIX_CROP[] = L" [CROP - drag to select, Enter to apply, Esc to cancel]";
    static constexpr wchar_t TITLE_SUFFIX_MARKUP[] = L" [MARKUP - drag to draw, Esc to exit]";
    static constexpr wchar_t TITLE_SUFFIX_TEXT[] = L" [TEXT - click to add text, Esc to exit]";
    static constexpr wchar_t TITLE_SUFFIX_ERASE[] = L" [ERASE - click on markup/text to delete, Esc to exit]";
    static constexpr wchar_t TITLE_SUFFIX_PAUSED[] = L" (paused)";

    // Rotation state (0, 90, 180, 270 degrees)
    int m_rotation = 0;

    // Current edit mode
    EditMode m_editMode = EditMode::None;

    // Crop selection
    bool m_isCropDragging = false;
    int m_cropStartX = 0;
    int m_cropStartY = 0;
    int m_cropEndX = 0;
    int m_cropEndY = 0;

    // Applied crop (in original image coordinates, before rotation)
    bool m_hasCrop = false;
    WICRect m_appliedCrop = {};

    // Markup drawing
    std::vector<MarkupStroke> m_markupStrokes;
    bool m_isDrawing = false;
    bool m_isErasing = false;

    // Text overlays
    std::vector<TextOverlay> m_textOverlays;

    // Text editing state
    bool m_isEditingText = false;
    std::wstring m_editingText;
    float m_editingTextX = 0;
    float m_editingTextY = 0;

    // Undo stack
    std::vector<EditState> m_undoStack;
    static constexpr size_t MAX_UNDO_LEVELS = 50;

    // Toolbar state
    std::vector<ToolbarButtonDef> m_toolbarDefs;
    std::vector<D2D1_RECT_F> m_toolbarButtonRects;
    D2D1_RECT_F m_toolbarBounds = {};
    bool m_toolbarVisible = true;
    int m_toolbarHoverIndex = -1;
    UINT_PTR m_toolbarHideTimerId = 0;
    bool m_cursorHidden = false;

    // Toolbar constants
    static constexpr UINT_PTR TOOLBAR_HIDE_TIMER_ID = 2;
    static constexpr DWORD TOOLBAR_HIDE_DELAY_MS = 2000;
    static constexpr float TOOLBAR_BTN_WIDTH = 52.0f;
    static constexpr float TOOLBAR_BTN_HEIGHT = 28.0f;
    static constexpr float TOOLBAR_PADDING = 4.0f;
    static constexpr float TOOLBAR_TOP_MARGIN = 8.0f;
    static constexpr float TOOLBAR_SEPARATOR_WIDTH = 12.0f;
};
