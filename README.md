# PAZ Unpacker
Executable can be found here [Releases](https://github.com/sibercat/PAZ-Unpacker/releases)
![unpacker](https://raw.githubusercontent.com/sibercat/PAZ-Unpacker/refs/heads/main/PAZ-Unpacker-master.png)
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
- **3D model viewer** — textured preview of `.pam` models with orbit / pan / zoom
- **Model export** — export any `.pam` to **OBJ** or **FBX**, with textures written alongside as PNG
- **Resizable layout** — drag the splitter between the file tree and the preview
- **Cache** — binary cache (v2) for fast startup; embeds folder path for multi-installation support
- Multi-language support (English, Japanese, Korean)
- Displays file metadata: size, PAZ source, internal path

## What's new in this fork

The original project was last updated in 2015. This fork modernises the codebase while keeping full compatibility with the original PAZ format.

### Foundation
| Area | Change |
|---|---|
| **Platform** | Upgraded from 32-bit to **64-bit (x64)** — handles large archives correctly |
| **Compiler** | VS2015 (v140) → **VS2025 (v145)** with `/std:c++latest` (C++26) |
| **Memory** | Raw pointers / `calloc`/`free` → `std::unique_ptr`, `std::vector` |
| **Strings** | Fixed-size `WCHAR[4096]` buffers → `std::wstring` throughout |
| **Paths** | Manual `CreateDirectory` loop → `std::filesystem::create_directories` |
| **Error handling** | Silent failures → `std::runtime_error` with descriptive messages |
| **Thread safety** | Added `std::atomic<bool>` busy flag guarding all thread spawns |
| **Assertions** | `assert()` precondition checks on all critical parsing functions |
| **Code quality** | `nullptr` throughout, `const`-correct getters, range-based for loops |

### 3D models (v2.4.0)

The `.pam` model format used by BDO was reverse engineered for this fork — see
`PamModel.h` for the full layout. All three format versions found in the archive
are supported.

| Area | Detail |
|---|---|
| **Viewer** | Software rasteriser drawing into the existing preview panel — no GPU dependency, no external libraries |
| **Shading** | Perspective-correct textured rendering with bilinear filtering and alpha cut-out for foliage and decals |
| **Controls** | Drag to orbit, right-drag to pan, wheel to zoom, double-click to reset, middle-click cycles textured / solid / wireframe |
| **Export** | Wavefront OBJ (+ `.mtl`) or binary FBX 7.4; pick the format in the save dialog |
| **Textures** | Referenced textures are pulled from the archive and written next to the model as full-resolution PNG |

Exports are verified to import into **Blender**, **3ds Max** and **Unreal Engine 5**.

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

Modifications made in this fork are licensed under the [MIT License](LICENSE).  
The original source code was published without a license — all original rights belong to **kukdh1**.  
See [CREDITS](CREDITS) for full attribution.
