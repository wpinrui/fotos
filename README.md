# fotos

A fast, minimal photo viewer for Windows. Built to fix the pain points of the default Photos app.

## Features

- **Fast** - Hardware-accelerated rendering with Direct2D
- **Auto-fit** - Images automatically fit to window size
- **Rapid browsing** - Hold arrow keys to quickly scan through photos
- **Animated GIF** - Full playback with pause/play support
- **Native look** - Dark mode support, follows system theme

## Keyboard Shortcuts

### Navigation
| Key | Action |
|-----|--------|
| Left/Right | Previous/Next image |
| Home/End | First/Last image |
| Space | Pause/play GIF |

### View
| Key | Action |
|-----|--------|
| F | Fit to window |
| 1 | Actual size (100%) |
| +/- or Scroll | Zoom in/out |
| F11 | Toggle fullscreen |

### File
| Key | Action |
|-----|--------|
| Ctrl+O | Open image |
| Ctrl+F | Open folder |
| Ctrl+S | Save image |
| Ctrl+Shift+S | Save as |
| Ctrl+W | Close image |
| Ctrl+Q | Quit |
| Delete | Delete file (recycle bin) |

### Edit
| Key | Action |
|-----|--------|
| Ctrl+C | Copy image to clipboard |
| Ctrl+B | Set as wallpaper |
| R | Rotate 90° clockwise |
| Shift+R | Rotate 90° counter-clockwise |
| C | Crop mode (Enter to apply) |
| M | Markup (draw) mode |
| T | Text overlay mode |
| E | Erase mode (remove markup/text) |
| Ctrl+Z | Undo |
| Esc | Exit edit mode / Close |

## Building

Requires:
- Visual Studio 2022+ with C++ workload
- CMake 3.20+
- Windows SDK

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build\Release\fotos.exe`

## Usage

```powershell
fotos.exe "C:\path\to\image.jpg"
```

Or drag-drop an image onto the executable.

## Tech Stack

- C++20
- Win32 API
- Direct2D / Direct3D 11
- Windows Imaging Component (WIC)

## Roadmap

### Phase 1: Core Viewer (MVP) - Done
- [x] Win32 window with DPI awareness and dark title bar
- [x] Direct2D hardware-accelerated rendering
- [x] Auto-fit images to window
- [x] Folder navigation (arrow keys, Home/End)
- [x] Fast browsing with image pre-caching
- [x] Animated GIF playback with pause/play
- [x] Zoom/pan with mouse wheel and drag
- [x] Delete to recycle bin

### Phase 2: Core Features - Done
- [x] Set as wallpaper (Ctrl+B)
- [x] Rotate 90° CW/CCW (R/Shift+R)
- [x] Crop with drag selection (C to toggle, Enter to apply)
- [x] Markup/drawing mode (M)
- [x] Text overlay (T)
- [x] Copy to clipboard (Ctrl+C)
- [x] Open file/folder dialogs (Ctrl+O, Ctrl+F)
- [x] Save/Save As (Ctrl+S, Ctrl+Shift+S)

### Phase 3: Polish & Distribution
- [ ] Floating toolbar (auto-hide)
- [ ] Right-click context menu
- [ ] Performance optimization (<100ms startup)
- [ ] MSI installer with file associations
- [ ] "Open with" context menu integration

## Supported Formats

JPEG, PNG, BMP, GIF (animated), TIFF, WebP, HEIC

## License

MIT
