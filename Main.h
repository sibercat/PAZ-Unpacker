#pragma once

#ifndef _MAIN_H_
#define _MAIN_H_

#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <shellapi.h>
#include <wincodec.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <string>
#include <vector>

#include "Version.h"
#include "Helper.h"
#include "Tree.h"
#include "MetaFile.h"
#include "PamExport.h"
#include "PskExport.h"
#include "PamModel.h"
#include "PamRender.h"
#include "PazFile.h"
#include "Setting.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// APP_VERSION / APP_VERSION_A come from Version.h (shared with the .rc).

// Where the footer update link sends the user.
#define APP_RELEASES_URL          L"https://github.com/sibercat/PAZ-Unpacker/releases"

// ── Window layout ────────────────────────────────────────────────────────────
#define WINDOW_MIN_WIDTH          1100
#define WINDOW_MIN_HEIGHT         700
#define DIVIDE_RATIO              0.65f   // initial split; app.fDivideRatio tracks it at runtime
#define SPLITTER_WIDTH            6       // draggable band between tree and preview
#define DIVIDE_RATIO_MIN          0.15f
#define DIVIDE_RATIO_MAX          0.85f
#define STATUSBAR_SECTION_COUNT   3
#define STATUSBAR_SECTION1        70
#define STATUSBAR_SECTION2        530
#define BUTTON_WIDTH              65
#define BUTTON_HEIGHT             26
#define LOAD_HEIGHT               30
#define HEADER_HEIGHT             LOAD_HEIGHT
#define INFO_RATIO                0.32f   // fraction of right panel for text info

// ── Dark mode palette ─────────────────────────────────────────────────────────
#define CLR_DARK_BG     RGB( 30,  30,  30)   // main window background
#define CLR_DARK_PANEL  RGB( 45,  45,  48)   // panel / control background
#define CLR_DARK_INPUT  RGB( 37,  37,  38)   // edit / tree background
#define CLR_DARK_TEXT   RGB(212, 212, 212)   // primary text
#define CLR_DARK_TEXT2  RGB(128, 128, 128)   // secondary / dim text
#define CLR_DARK_BORDER RGB( 63,  63,  70)   // border / separator
#define CLR_DARK_LINK   RGB( 96, 165, 250)   // clickable text (footer update notice)

// ── Font ─────────────────────────────────────────────────────────────────────
#define FONT_SIZE                 17
#define FONT_FACE                 L"Segoe UI"

// ── Control IDs ──────────────────────────────────────────────────────────────
#define ID_BUTTON_EXTRACT         1
#define ID_BUTTON_LOAD            2
#define ID_TREE_FILESYSTEM        10
#define ID_STATIC                 11
#define ID_STATUSBAR              20
#define ID_PREVIEW                50
#define ID_SEARCH_EDIT            60   // search window input
#define ID_SEARCH_LIST            61   // search window ListView
#define ID_SEARCH_COUNT           62   // search window count label

// ── Custom window messages ────────────────────────────────────────────────────
#define WM_APP_LOAD_FALLBACK     (WM_APP + 0)  // cache load failed → launch FileThread
#define WM_APP_UPDATE_RESULT     (WM_APP + 1)  // update check done; wParam=1 update available, 0 up-to-date, -1 error
#define WM_APP_SEARCH_DONE       (WM_APP + 2)  // search thread done; lParam = new std::vector<kukdh1::Tree*>*

// ── Menu IDs ─────────────────────────────────────────────────────────────────
#define ID_MENU_FILE_OPEN        200
#define ID_MENU_FILE_EXTRACT     201
#define ID_MENU_FILE_EXPORT      203
#define ID_MENU_FILE_EXIT        202
#define ID_MENU_CACHE_REBUILD    210
#define ID_MENU_CACHE_CLEAR_TEMP 211
#define ID_MENU_SEARCH_OPEN      240
#define ID_MENU_SETTINGS         230
#define ID_MENU_HELP_CHECK_UPDATE 221
#define ID_MENU_HELP_ABOUT       220

// ── Crypto ───────────────────────────────────────────────────────────────────
#define ICE_KEY                   ((uint8_t *)"\x51\xF3\x0F\x11\x04\x24\x6A\x00")
#define ICE_KEY_LEN               8

// Extensions that have a texture preview
constexpr const wchar_t* PREVIEW_EXTS[] = { L".dds", L".png", L".bmp" };
constexpr int PREVIEW_SIZE_LIMIT = 32 * 1024 * 1024;   // 32 MB max for preview

// .pam models get the 3D preview instead
constexpr int MODEL_SIZE_LIMIT = 64 * 1024 * 1024;     // 64 MB max for a model

// ── Application state ────────────────────────────────────────────────────────
typedef struct _AppData {
  // Controls
  HWND hButtonExctact;
  HWND hButtonLoad;
  HWND hTreeFileSystem;
  HWND hStatusBar;
  HWND hStaticInfo;
  HWND hProgressBar;
  HWND hPreviewPanel;
  HWND hSearchWnd;    // floating search window (nullptr when closed)

  // Data
  std::unique_ptr<kukdh1::Tree> CTree;
  std::unique_ptr<kukdh1::Meta> CMeta;
  kukdh1::Setting CSetting;
  std::wstring wsFolderPath;

  // GDI / WIC
  HFONT  hFont;
  HBITMAP hPreviewBitmap;
  IWICImagingFactory *pWICFactory;

  // 3D model preview (.pam)
  std::unique_ptr<kukdh1::PamModel>  CPamModel;
  kukdh1::PamCamera                  PamCam;
  kukdh1::PamTarget                  PamTarget;   // kept alive so the depth buffer persists
  std::vector<kukdh1::PamTexture>    vPamTextures;
  void  *pPamPixels;      // DIB bits of hPreviewBitmap while a model is shown
  int    nPamWidth;       // bitmap size == render size (halved while dragging)
  int    nPamHeight;
  bool   bPamOrbiting;
  bool   bPamPanning;
  POINT  ptPamDragOrigin;
  bool   bPamWireframe;
  bool   bPamShowTexture;
  bool   bPamDirty;       // camera moved; re-rasterise on the next WM_PAINT

  // Dark mode brushes
  HBRUSH hBrushBg;     // CLR_DARK_BG   — main window background
  HBRUSH hBrushPanel;  // CLR_DARK_PANEL — control / button background
  HBRUSH hBrushInput;  // CLR_DARK_INPUT — edit / tree background

  // Splitter between the tree and the right-hand panel
  float fDivideRatio;
  bool  bSplitterDrag;

  // Footer update notice. rcUpdateLink is written by the status bar paint
  // handler and read by its hit test, so the clickable area always matches
  // exactly what was drawn (it is empty while no update is showing).
  std::wstring wsUpdateTag;
  bool         bUpdateAvailable;
  RECT         rcUpdateLink;

  // State flags
  std::mutex mtx;
  std::atomic<bool> bBusy;

  _AppData() :
    hButtonExctact(nullptr), hButtonLoad(nullptr),
    hTreeFileSystem(nullptr), hStatusBar(nullptr),
    hStaticInfo(nullptr), hProgressBar(nullptr),
    hPreviewPanel(nullptr), hSearchWnd(nullptr),
    hFont(nullptr), hPreviewBitmap(nullptr), pWICFactory(nullptr),
    pPamPixels(nullptr), nPamWidth(0), nPamHeight(0),
    bPamOrbiting(false), bPamPanning(false), bPamWireframe(false),
    bPamShowTexture(true), bPamDirty(false),
    hBrushBg(nullptr), hBrushPanel(nullptr), hBrushInput(nullptr),
    fDivideRatio(DIVIDE_RATIO), bSplitterDrag(false),
    bUpdateAvailable(false),
    bBusy(false)
  {
    ptPamDragOrigin.x = ptPamDragOrigin.y = 0;
    SetRectEmpty(&rcUpdateLink);
  }

} AppData;

#endif
