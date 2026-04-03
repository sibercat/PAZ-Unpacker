#pragma once

#ifndef _MAIN_H_
#define _MAIN_H_

#include <Windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
#include <Uxtheme.h>
#include <wincodec.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>

#include "Helper.h"
#include "Tree.h"
#include "MetaFile.h"
#include "PazFile.h"
#include "Setting.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Version ──────────────────────────────────────────────────────────────────
#define APP_VERSION               L"2.2.0"

// ── Window layout ────────────────────────────────────────────────────────────
#define WINDOW_MIN_WIDTH          1100
#define WINDOW_MIN_HEIGHT         700
#define DIVIDE_RATIO              0.65f
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

// ── Font ─────────────────────────────────────────────────────────────────────
#define FONT_SIZE                 17
#define FONT_FACE                 L"Segoe UI"

// ── Control IDs ──────────────────────────────────────────────────────────────
#define ID_BUTTON_OPEN            0
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

// ── Application state ────────────────────────────────────────────────────────
typedef struct _AppData {
  // Controls
  HWND hButtonOpen;
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

  // Dark mode brushes
  HBRUSH hBrushBg;     // CLR_DARK_BG   — main window background
  HBRUSH hBrushPanel;  // CLR_DARK_PANEL — control / button background
  HBRUSH hBrushInput;  // CLR_DARK_INPUT — edit / tree background

  // State flags
  std::mutex mtx;
  std::atomic<bool> bBusy;

  _AppData() :
    hButtonOpen(nullptr), hButtonExctact(nullptr), hButtonLoad(nullptr),
    hTreeFileSystem(nullptr), hStatusBar(nullptr),
    hStaticInfo(nullptr), hProgressBar(nullptr),
    hPreviewPanel(nullptr), hSearchWnd(nullptr),
    hFont(nullptr), hPreviewBitmap(nullptr), pWICFactory(nullptr),
    hBrushBg(nullptr), hBrushPanel(nullptr), hBrushInput(nullptr),
    bBusy(false)
  {}

} AppData;

#endif
