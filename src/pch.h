#pragma once

// Windows headers
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

// DXGI and Direct3D
#include <dxgi1_2.h>
#include <d3d11.h>

// Direct2D and DirectWrite
#include <d2d1_1.h>
#include <dwrite.h>
#include <wincodec.h>

// COM smart pointers
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// C++ Standard Library
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <cfloat>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <unordered_map>
#include <functional>
#include <optional>

namespace fs = std::filesystem;

// Helper macro for COM error checking - throws on failure
#ifndef THROW_IF_FAILED
#define THROW_IF_FAILED(hr) if (FAILED(hr)) throw std::runtime_error("COM operation failed")
#endif

// Helper macro for COM error checking - returns nullptr on failure
#ifndef CHECK_HR_RETURN_NULL
#define CHECK_HR_RETURN_NULL(hr) if (FAILED(hr)) return nullptr
#endif

// Shared string utility - convert wide string to lowercase
inline std::wstring ToLowerCase(const std::wstring& str) {
    std::wstring result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::towlower);
    return result;
}

// WIC pixel format constants (inline for single definition across translation units)
inline const GUID& WIC_PIXEL_FORMAT_PREMULTIPLIED = GUID_WICPixelFormat32bppPBGRA;
inline const GUID& WIC_PIXEL_FORMAT_STRAIGHT_ALPHA = GUID_WICPixelFormat32bppBGRA;

// Rotation degree constants
namespace Rotation {
    constexpr int NONE = 0;
    constexpr int CW_90 = 90;
    constexpr int CW_180 = 180;
    constexpr int CW_270 = 270;
    constexpr int FULL_ROTATION = 360;
}

// Standard UI colors (D2D1 ColorF values)
namespace Colors {
    constexpr D2D1_COLOR_F WHITE = { 1.0f, 1.0f, 1.0f, 1.0f };
    constexpr D2D1_COLOR_F RED = { 1.0f, 0.0f, 0.0f, 1.0f };
    constexpr D2D1_COLOR_F BLACK = { 0.0f, 0.0f, 0.0f, 1.0f };
    constexpr D2D1_COLOR_F DARK_GRAY = { 0.1f, 0.1f, 0.1f, 1.0f };
    constexpr D2D1_COLOR_F TOOLBAR_BG = { 0.15f, 0.15f, 0.15f, 0.85f };
    constexpr D2D1_COLOR_F TOOLBAR_HOVER = { 1.0f, 1.0f, 1.0f, 0.2f };
    constexpr D2D1_COLOR_F TOOLBAR_DISABLED = { 0.5f, 0.5f, 0.5f, 1.0f };
    constexpr D2D1_COLOR_F TOOLBAR_SEPARATOR = { 0.4f, 0.4f, 0.4f, 0.6f };
    constexpr D2D1_COLOR_F TOAST_BG = { 0.12f, 0.12f, 0.12f, 0.90f };
    constexpr D2D1_COLOR_F TOOLTIP_BG = { 0.10f, 0.10f, 0.10f, 0.95f };
}
