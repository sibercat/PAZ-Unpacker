# PAZ Unpacker

A Windows GUI tool for unpacking and extracting files from **Black Desert Online** (BDO) `.PAZ` archive files.

> **This is a community-maintained fork of [kukdh1/PAZ-Unpacker](https://github.com/kukdh1/PAZ-Unpacker).**  
> Full credit for the original work goes to **kukdh1**. See [CREDITS](CREDITS) for details.

---

## Features

- Browse the virtual filesystem inside BDO PAZ archives
- Extract individual files, folders, or entire archives
- **Dark mode UI** — full dark theme across all controls and windows
- **Settings dialog** — configure your PAZ folder and extract path once; no dialogs after that
- **Load button** — one-click load from your saved PAZ folder path
- **Extract button** — extracts directly to your saved path, no browse dialog if already configured
- **Search window** — fast non-blocking search across all 800k+ files with live filtering
- **Check for Updates** — checks GitHub releases for new versions
- **File preview** — inline DDS/PNG/BMP texture preview panel
- **Cache** — binary cache (v2) for fast startup; embeds folder path for multi-installation support
- Multi-language support (English, Japanese, Korean)
- Displays file metadata: size, PAZ source, internal path

## What's new in this fork

The original project was last updated in 2015. This fork modernises the codebase while keeping full compatibility with the original PAZ format.

### v2.2.0
| Area | Change |
|---|---|
| **Dark mode** | Full Win32 dark theme — title bar, menu bar, status bar, tree, buttons, all dialogs |
| **Settings** | New Settings dialog to configure PAZ folder path and extract output path |
| **Load button** | One-click load from saved PAZ path — no browse dialog needed |
| **Extract button** | Moved to toolbar next to Load; uses saved extract path directly |
| **Search window** | Non-modal floating window with virtual ListView + background search thread (no UI freeze on 800k+ files) |
| **Cache v2** | Cache header now stores the PAZ folder path — supports multiple BDO installations |
| **Check for Updates** | Help menu item checks GitHub releases API for newer versions |
| **Settings persistence** | Explicit save on exit — fixes settings loss caused by `ExitProcess` skipping destructors |

### Foundation (v2.0.0)
| Area | Change |
|---|---|
| **Platform** | Upgraded from 32-bit to **64-bit (x64)** — handles large archives correctly |
| **Compiler** | VS2015 (v140) → **VS2025 (v145)** with `/std:c++latest` (C++26) |
| **Memory** | Raw pointers / `calloc`/`free` → `std::unique_ptr`, `std::vector` |
| **Strings** | Fixed-size `WCHAR[4096]` buffers → `std::wstring` throughout |
| **Paths** | Manual `CreateDirectory` loop → `std::filesystem::create_directories` |
| **Error handling** | Silent failures → `std::runtime_error` with descriptive messages |
| **Thread safety** | Added `std::atomic<bool>` busy flag guarding all thread spawns |
| **Bug fix** | Dangling pointer in `AddThread` (temporary `.c_str()` passed to callback) |
| **Bug fix** | `ConvertWidechar` undefined behaviour (`(wchar_t*)out.c_str()` write) |
| **Bug fix** | `BrowseFolder` undefined behaviour (writing through `wstring::c_str()`) |
| **Bug fix** | `catch(std::exception e)` by value → `catch(const std::exception &e)` |
| **Assertions** | `assert()` precondition checks on all critical parsing functions |
| **Code quality** | `nullptr` throughout, `const`-correct getters, range-based for loops |

## Requirements

- Windows 10 or 11 (x64)
- [Visual Studio 2025](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
- Windows SDK 10.0

## Build

1. Open `PAZ-Unpacker.sln` in Visual Studio 2025
2. Select **Release | x64**
3. Build → Build Solution (`Ctrl+Shift+B`)
4. Copy the four `Setting*.xml` files alongside the built `.exe`

## Usage

1. Launch `PAZ-Unpacker.exe`
2. Go to **Settings → Configure Paths** and set your PAZ folder (the folder containing `pad00000.meta`) and your preferred extract output folder
3. Click **Load** to load the PAZ archive — subsequent launches just press Load
4. Browse the file tree on the left
5. Select a file, folder, or the root node, then click **Extract**
6. Use **Search** in the menu bar to search across all files by name or extension (e.g. `.dds`, `ui_texture`)

## Format notes (from original README)

- Tested on KR client. The meta file format changed on KR client since 2016.05 (decrypt key changed).
- If an extracted file begins with `0x6E`, the first 9 bytes are a compression header:
  ```
  0x6E       : Header magic
  DWORD      : Original file size
  DWORD      : Compressed file size
  ```
- BDO client (and XIGNCODE3) does not check CRC codes on packed files.

## License

The original source code has no license — all original rights belong to **kukdh1**.  
Modifications made in this fork are © the contributors of this repository.  
See [CREDITS](CREDITS) for full attribution.
