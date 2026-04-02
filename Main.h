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
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ── Version ──────────────────────────────────────────────────────────────────
#define APP_VERSION               L"2.0.0"

// ── Window layout ────────────────────────────────────────────────────────────
#define WINDOW_MIN_WIDTH          1100
#define WINDOW_MIN_HEIGHT         700
#define DIVIDE_RATIO              0.65f
#define STATUSBAR_SECTION_COUNT   3
#define STATUSBAR_SECTION1        70
#define STATUSBAR_SECTION2        530
#define BUTTON_WIDTH              65
#define BUTTON_HEIGHT             26
#define SEARCH_HEIGHT             26
#define FILTER_HEIGHT             26
#define HEADER_HEIGHT             (SEARCH_HEIGHT + FILTER_HEIGHT)
#define INFO_RATIO                0.32f   // fraction of right panel for text info

// ── Font ─────────────────────────────────────────────────────────────────────
#define FONT_SIZE                 17
#define FONT_FACE                 L"Segoe UI"

// ── Control IDs ──────────────────────────────────────────────────────────────
#define ID_BUTTON_OPEN            0
#define ID_BUTTON_EXTRACT         1
#define ID_TREE_FILESYSTEM        10
#define ID_STATIC                 11
#define ID_STATUSBAR              20
#define ID_EDIT_SEARCH            30
#define ID_PREVIEW                50
#define ID_CHECK_BASE             40   // 40 … 49  (one per extension)

// ── Crypto ───────────────────────────────────────────────────────────────────
#define ICE_KEY                   ((uint8_t *)"\x51\xF3\x0F\x11\x04\x24\x6A\x00")
#define ICE_KEY_LEN               8

// ── Extension filter list ────────────────────────────────────────────────────
constexpr int EXT_FILTER_COUNT = 10;
constexpr const wchar_t* EXT_NAMES[EXT_FILTER_COUNT] = {
    L".dds", L".png", L".bmp", L".xml",
    L".pam", L".pcm", L".webm", L".bk2", L".srt", L".txt"
};
// Extensions that have a texture preview
constexpr const wchar_t* PREVIEW_EXTS[] = { L".dds", L".png", L".bmp" };
constexpr int PREVIEW_SIZE_LIMIT = 32 * 1024 * 1024;   // 32 MB max for preview

// ── Application state ────────────────────────────────────────────────────────
typedef struct _AppData {
  // Controls
  HWND hButtonOpen;
  HWND hButtonExctact;
  HWND hTreeFileSystem;
  HWND hStatusBar;
  HWND hStaticInfo;
  HWND hProgressBar;
  HWND hSearchEdit;
  HWND hCheckboxes[EXT_FILTER_COUNT];
  HWND hPreviewPanel;

  // Data
  std::unique_ptr<kukdh1::Tree> CTree;
  std::unique_ptr<kukdh1::Meta> CMeta;
  kukdh1::Setting CSetting;
  std::wstring wsFolderPath;

  // GDI / WIC
  HFONT  hFont;
  HBITMAP hPreviewBitmap;
  IWICImagingFactory *pWICFactory;

  // State flags
  bool bSearchMode;
  std::mutex mtx;
  std::atomic<bool> bBusy;

  _AppData() :
    hButtonOpen(nullptr), hButtonExctact(nullptr),
    hTreeFileSystem(nullptr), hStatusBar(nullptr),
    hStaticInfo(nullptr), hProgressBar(nullptr),
    hSearchEdit(nullptr), hPreviewPanel(nullptr),
    hFont(nullptr), hPreviewBitmap(nullptr), pWICFactory(nullptr),
    bSearchMode(false), bBusy(false)
  {
    memset(hCheckboxes, 0, sizeof(hCheckboxes));
  }
} AppData;

#endif
