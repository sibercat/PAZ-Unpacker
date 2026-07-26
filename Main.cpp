#include "Main.h"
#include "Cache.h"
#include <cassert>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <winhttp.h>

namespace fs = std::filesystem;

// ── Forward declarations ──────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
DWORD   WINAPI   FileThread(LPVOID arg);
DWORD   WINAPI   CacheLoadThread(LPVOID arg);
DWORD   WINAPI   ExtractThread(LPVOID arg);
DWORD   WINAPI   AddThread(LPVOID arg);
DWORD   WINAPI   CheckUpdateThread(LPVOID arg);

// ── Update check plumbing ────────────────────────────────────────────────────
// Two callers with different reporting rules share one thread:
//   silent  -- fired once at startup; only ever lights up the footer link
//   loud    -- the Help menu item; reports every outcome in a message box
// The request and the result are each heap-allocated and owned by exactly one
// side, so a startup check and a menu check may safely overlap.
struct UpdateCheckReq {
  HWND hWnd;
  bool bSilent;
};

struct UpdateResult {
  int          nCode;    // 1 = newer available, 0 = up to date, -1 = check failed
  bool         bSilent;
  std::wstring wsTag;    // release tag, only meaningful when nCode == 1
};

void StartUpdateCheck(HWND hWnd, bool bSilent);
void             OpenPazFolder(HWND hWnd, const std::wstring &folderPath);
void             ShowSettingsDialog(HWND hParent);
void             ShowSearchWindow(HWND hParent);
void             InvalidateFileNodeCache();
WNDPROC          g_fnOrigStatusBarProc = nullptr;  // original statusbar WndProc for subclass
WNDPROC          g_fnOrigHeaderProc    = nullptr;  // original ListView header WndProc for subclass
void    UpdatePreview(kukdh1::Tree *pTree);
HBITMAP LoadWICBitmap(const std::wstring &path, int maxW, int maxH);
void    ClearPamPreview();
bool    RenderPamPreview();
void    LoadPamTextures(const kukdh1::PamModel &model,
                        std::vector<kukdh1::PamTexture> &out);
bool    LoadTexturePixels(const std::wstring &path, kukdh1::PamTexture &out);
void    ExportSelectedModel(HWND hWnd, kukdh1::Tree *pTree);
DWORD   WINAPI   ExportThread(LPVOID arg);
static const std::vector<kukdh1::Tree *> &GetCachedFileNodes();

// ── Global ────────────────────────────────────────────────────────────────────
AppData app;

// ─────────────────────────────────────────────────────────────────────────────
// WinMain
// ─────────────────────────────────────────────────────────────────────────────
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdParam, int nCmdShow) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  // ── Dark mode: make menus and common controls use dark theme ─────────────
  // These are undocumented uxtheme.dll exports (ordinals 135, 104, 133).
  // They are stable since Windows 10 1809 and used by many mainstream apps.
  {
    HMODULE hUxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUxtheme) {
      // ordinal 135 = SetPreferredAppMode: 0=default,1=AllowDark,2=ForceDark
      using fnSetPreferredAppMode = HRESULT(WINAPI*)(DWORD);
      auto _SetPreferredAppMode = reinterpret_cast<fnSetPreferredAppMode>(
        GetProcAddress(hUxtheme, MAKEINTRESOURCEA(135)));
      // ordinal 104 = RefreshImmersiveColorPolicyState
      using fnRefresh = void(WINAPI*)();
      auto _Refresh = reinterpret_cast<fnRefresh>(
        GetProcAddress(hUxtheme, MAKEINTRESOURCEA(104)));
      if (_SetPreferredAppMode) _SetPreferredAppMode(2); // ForceDark
      if (_Refresh)             _Refresh();
      // Don't FreeLibrary — keep it loaded so the setting stays active
    }
  }

  HWND  hWnd;
  WNDCLASS wndclass;
  MSG   msg;
  INITCOMMONCONTROLSEX iccex;
  std::wstring lpszClass = app.CSetting.getString(kukdh1::Setting::ID_CAPTION);
  // Append version to window class / caption base
  lpszClass += L" v" APP_VERSION;

  iccex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  iccex.dwICC  = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES;
  if (!InitCommonControlsEx(&iccex)) {
    CoUninitialize();
    return -1;
  }

  // Register preview panel window class
  WNDCLASS previewClass = {};
  previewClass.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  previewClass.lpfnWndProc   = PreviewWndProc;
  previewClass.hInstance     = hInstance;
  previewClass.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
  previewClass.lpszClassName = L"PAZPreview";
  RegisterClass(&previewClass);

  // Register main window class
  wndclass.cbClsExtra    = 0;
  wndclass.cbWndExtra    = 0;
  wndclass.hbrBackground = nullptr;  // dark bg handled via WM_ERASEBKGND
  wndclass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
  wndclass.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
  wndclass.hInstance     = hInstance;
  wndclass.lpfnWndProc   = WndProc;
  wndclass.lpszClassName = lpszClass.c_str();
  wndclass.lpszMenuName  = nullptr;
  wndclass.style         = CS_VREDRAW | CS_HREDRAW;
  RegisterClass(&wndclass);

  hWnd = CreateWindow(lpszClass.c_str(), lpszClass.c_str(), WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
    nullptr, nullptr, hInstance, nullptr);
  ShowWindow(hWnd, nCmdShow);

  // Call AllowDarkModeForWindow AFTER ShowWindow for reliable dark menu bar
  {
    HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
    using fnAllowDark = BOOL(WINAPI*)(HWND, BOOL);
    auto _Allow = reinterpret_cast<fnAllowDark>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
    if (_Allow) { _Allow(hWnd, TRUE); _Allow(app.hStatusBar, TRUE); }

    // SetWindowCompositionAttribute with WCA_USEDARKMODECOLORS=26 — makes Win11
    // render the menu bar with the dark system colour (undocumented user32 export).
    struct WINCOMPATTRIBDATA { DWORD attrib; PVOID pData; SIZE_T cbData; };
    BOOL bDark = TRUE;
    WINCOMPATTRIBDATA wca = { 26, &bDark, sizeof(bDark) };
    using fnSetWCA = BOOL(WINAPI*)(HWND, WINCOMPATTRIBDATA*);
    auto _SetWCA = reinterpret_cast<fnSetWCA>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (_SetWCA) _SetWCA(hWnd, &wca);

    SendMessage(hWnd,           WM_THEMECHANGED, 0, 0);
    SendMessage(app.hStatusBar, WM_THEMECHANGED, 0, 0);
    DrawMenuBar(hWnd);
  }

  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  CoUninitialize();
  return (int)msg.wParam;
}

// ─────────────────────────────────────────────────────────────────────────────
// Status bar dark-mode subclass
// The STATUSCLASSNAME control ignores WM_CTLCOLOR* messages.
// Subclass it to paint with dark colours ourselves.
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK DarkStatusBarProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; GetClientRect(hWnd, &rc);
    FillRect(hdc, &rc, app.hBrushBg);

    SetTextColor(hdc, CLR_DARK_TEXT2);
    SetBkMode(hdc, TRANSPARENT);
    if (app.hFont) SelectObject(hdc, app.hFont);

    // The update notice is right-aligned inside the last part. Measure it up
    // front so the part's own text can be clipped short of it rather than
    // drawing underneath.
    std::wstring wsNotice;
    int nNoticeW = 0;
    if (app.bUpdateAvailable) {
      // Up arrow written as \u2191 rather than a literal: this file is UTF-8
      // without a BOM and the build does not pass /utf-8.
      wsNotice = L"\u2191 Update available: " + app.wsUpdateTag;
      SIZE sz = {};
      GetTextExtentPoint32W(hdc, wsNotice.c_str(), (int)wsNotice.size(), &sz);
      nNoticeW = sz.cx + 12;
    }
    SetRectEmpty(&app.rcUpdateLink);

    // Draw each part's text
    int nParts = (int)SendMessage(hWnd, SB_GETPARTS, 0, 0);
    for (int i = 0; i < nParts; i++) {
      RECT partRc;
      SendMessage(hWnd, SB_GETRECT, i, (LPARAM)&partRc);
      wchar_t buf[256] = {};
      SendMessage(hWnd, SB_GETTEXT, i, (LPARAM)buf);
      InflateRect(&partRc, -2, 0);

      if (i == nParts - 1 && nNoticeW > 0) {
        // Keep clear of the size grip, which overlays the last part.
        RECT linkRc = partRc;
        linkRc.right -= GetSystemMetrics(SM_CXVSCROLL);
        linkRc.left   = (std::max)(partRc.left, linkRc.right - nNoticeW);
        if (linkRc.right > linkRc.left) {
          partRc.right = linkRc.left;
          app.rcUpdateLink = linkRc;
          SetTextColor(hdc, CLR_DARK_LINK);
          DrawTextW(hdc, wsNotice.c_str(), -1, &linkRc,
                    DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
          SetTextColor(hdc, CLR_DARK_TEXT2);
        }
      }

      DrawTextW(hdc, buf, -1, &partRc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    EndPaint(hWnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;

  // Footer update notice behaves like a link: hand cursor, opens the releases
  // page. rcUpdateLink is empty unless the notice is actually on screen.
  if (msg == WM_LBUTTONUP && !IsRectEmpty(&app.rcUpdateLink)) {
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (PtInRect(&app.rcUpdateLink, pt)) {
      ShellExecuteW(hWnd, L"open", APP_RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL);
      return 0;
    }
  }
  if (msg == WM_SETCURSOR && !IsRectEmpty(&app.rcUpdateLink)) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hWnd, &pt);
    if (PtInRect(&app.rcUpdateLink, pt)) {
      SetCursor(LoadCursor(nullptr, IDC_HAND));
      return TRUE;
    }
  }

  return CallWindowProc(g_fnOrigStatusBarProc, hWnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// ListView header dark-mode subclass
// NM_CUSTOMDRAW cannot override visual-style themed drawing; subclassing WM_PAINT
// gives us full control, same technique used for the status bar.
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK DarkHeaderProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT rc; GetClientRect(hWnd, &rc);
    FillRect(hdc, &rc, app.hBrushInput);

    int count = Header_GetItemCount(hWnd);
    for (int i = 0; i < count; i++) {
      RECT itemRc;
      Header_GetItemRect(hWnd, i, &itemRc);

      // Right divider
      HPEN hPen = CreatePen(PS_SOLID, 1, CLR_DARK_BORDER);
      HPEN hOld = (HPEN)SelectObject(hdc, hPen);
      MoveToEx(hdc, itemRc.right - 1, itemRc.top,    nullptr);
      LineTo   (hdc, itemRc.right - 1, itemRc.bottom);
      SelectObject(hdc, hOld); DeleteObject(hPen);

      // Item text
      wchar_t buf[256] = {};
      HDITEMW hdi = {}; hdi.mask = HDI_TEXT; hdi.pszText = buf; hdi.cchTextMax = _countof(buf);
      Header_GetItem(hWnd, i, &hdi);

      SetTextColor(hdc, CLR_DARK_TEXT);
      SetBkMode(hdc, TRANSPARENT);
      if (app.hFont) SelectObject(hdc, app.hFont);
      RECT textRc = itemRc;
      InflateRect(&textRc, -6, 0);
      DrawTextW(hdc, buf, -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    }

    // Bottom border
    HPEN hPen = CreatePen(PS_SOLID, 1, CLR_DARK_BORDER);
    HPEN hOld = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
    LineTo   (hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, hOld); DeleteObject(hPen);

    EndPaint(hWnd, &ps);
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  return CallWindowProc(g_fnOrigHeaderProc, hWnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Preview panel WndProc
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
  switch (iMessage) {
    case WM_ERASEBKGND:
      return 1;   // handled in WM_PAINT — prevents flicker

    // ── 3D model interaction ────────────────────────────────────────────────
    // Only active while a .pam model is loaded; textures ignore these.
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      if (app.CPamModel) {
        if (iMessage == WM_LBUTTONDOWN) app.bPamOrbiting = true;
        else                            app.bPamPanning  = true;
        app.ptPamDragOrigin.x = GET_X_LPARAM(lParam);
        app.ptPamDragOrigin.y = GET_Y_LPARAM(lParam);
        SetCapture(hWnd);
      }
      return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
      if (app.bPamOrbiting || app.bPamPanning) {
        app.bPamOrbiting = false;
        app.bPamPanning  = false;
        ReleaseCapture();
        // Drag over — redraw at full resolution.
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    // Camera changes only mark the view dirty and invalidate. The actual
    // rasterise happens in WM_PAINT, which Windows synthesises only once the
    // queue is empty — rendering here instead would let mouse-move messages
    // pile up faster than frames complete.
    case WM_MOUSEMOVE:
      if (app.CPamModel && (app.bPamOrbiting || app.bPamPanning)) {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        int dx = x - app.ptPamDragOrigin.x;
        int dy = y - app.ptPamDragOrigin.y;
        app.ptPamDragOrigin.x = x;
        app.ptPamDragOrigin.y = y;

        if (app.bPamOrbiting) {
          app.PamCam.Orbit(dx * 0.010f, dy * 0.010f);
        }
        else {
          RECT rc; GetClientRect(hWnd, &rc);
          int w = rc.right - rc.left, h = rc.bottom - rc.top;
          if (w > 0 && h > 0) {
            app.PamCam.fPanX += (float)dx / w;
            app.PamCam.fPanY += (float)dy / h;
          }
        }
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    case WM_MOUSEWHEEL:
      if (app.CPamModel) {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        app.PamCam.Zoom(delta > 0 ? 1.15f : 1.0f / 1.15f);
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    // Double-click resets the view; middle-click cycles textured/solid/wireframe.
    case WM_LBUTTONDBLCLK:
      if (app.CPamModel) {
        app.PamCam.Reset();
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    case WM_MBUTTONDOWN:
      if (app.CPamModel) {
        // textured -> solid -> wireframe -> textured
        if (app.bPamShowTexture)      { app.bPamShowTexture = false; app.bPamWireframe = false; }
        else if (!app.bPamWireframe)  { app.bPamWireframe = true; }
        else                          { app.bPamShowTexture = true; app.bPamWireframe = false; }
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    case WM_SIZE:
      // Re-render at the new panel size so the model stays sharp.
      if (app.CPamModel) {
        app.bPamDirty = true;
        InvalidateRect(hWnd, nullptr, FALSE);
      }
      return 0;

    case WM_PAINT:
      {
        // Rasterise here rather than in the input handlers so consecutive
        // mouse moves collapse into a single frame.
        if (app.CPamModel && app.bPamDirty) {
          RenderPamPreview();
          app.bPamDirty = false;
        }

        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        if (app.hPreviewBitmap) {
          BITMAP bm;
          GetObject(app.hPreviewBitmap, sizeof(bm), &bm);

          HDC hdcMem = CreateCompatibleDC(hdc);
          HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, app.hPreviewBitmap);

          // A model always fills the panel — the bitmap is the whole frame, at
          // full size or (while dragging) half size. A texture keeps its own
          // size and gets centred.
          bool isModel = (app.CPamModel != nullptr);
          int x = isModel ? 0 : (rc.right  - bm.bmWidth)  / 2;
          int y = isModel ? 0 : (rc.bottom - bm.bmHeight) / 2;
          int dw = isModel ? rc.right  : bm.bmWidth;
          int dh = isModel ? rc.bottom : bm.bmHeight;

          // Fill only the margins around the image. Filling the whole panel and
          // then blitting over it paints every pixel twice, which is visible as
          // a flash while orbiting a model (the bitmap covers the full panel).
          int saved = SaveDC(hdc);
          ExcludeClipRect(hdc, x, y, x + dw, y + dh);
          FillRect(hdc, &rc, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
          RestoreDC(hdc, saved);

          if (bm.bmWidth != dw || bm.bmHeight != dh) {
            SetStretchBltMode(hdc, COLORONCOLOR);   // fast; this frame is transient
            StretchBlt(hdc, x, y, dw, dh, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
          }
          else {
            BitBlt(hdc, x, y, dw, dh, hdcMem, 0, 0, SRCCOPY);
          }

          SelectObject(hdcMem, hOld);
          DeleteDC(hdcMem);
        }
        else {
          FillRect(hdc, &rc, app.hBrushPanel ? app.hBrushPanel : GetSysColorBrush(COLOR_BTNFACE));
          SetTextColor(hdc, CLR_DARK_TEXT2);
          SetBkMode(hdc, TRANSPARENT);
          if (app.hFont) SelectObject(hdc, app.hFont);
          DrawText(hdc, L"No preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Control hint while a model is on screen
        if (app.CPamModel) {
          SetTextColor(hdc, CLR_DARK_TEXT2);
          SetBkMode(hdc, TRANSPARENT);
          if (app.hFont) SelectObject(hdc, app.hFont);
          RECT rcHint = rc;
          rcHint.bottom -= 4;
          DrawText(hdc,
            L"drag: orbit   right-drag: pan   wheel: zoom   dbl-click: reset   mid-click: texture/solid/wire",
            -1, &rcHint, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        }

        EndPaint(hWnd, &ps);
        return 0;
      }
  }
  return DefWindowProc(hWnd, iMessage, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Window message handlers
// ─────────────────────────────────────────────────────────────────────────────
BOOL Cls_OnCreate(HWND hWnd, LPCREATESTRUCT lpCreateStruct) {
  // ── Dark mode title bar ────────────────────────────────────────────────────
  BOOL darkMode = TRUE;
  DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

  // ── Dark mode brushes ──────────────────────────────────────────────────────
  app.hBrushBg    = CreateSolidBrush(CLR_DARK_BG);
  app.hBrushPanel = CreateSolidBrush(CLR_DARK_PANEL);
  app.hBrushInput = CreateSolidBrush(CLR_DARK_INPUT);

  // ── Controls ───────────────────────────────────────────────────────────────
  // Extract button — visible next to Load button; disabled until a file is selected
  app.hButtonExctact  = CreateWindow(WC_BUTTON, L"Extract",
    WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
    0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_EXTRACT, lpCreateStruct->hInstance, nullptr);

  // Load button — visible at top of left panel; loads last saved PAZ folder
  app.hButtonLoad = CreateWindow(WC_BUTTON, L"Load",
    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
    0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_LOAD, lpCreateStruct->hInstance, nullptr);

  app.hTreeFileSystem = CreateWindow(WC_TREEVIEW, nullptr,
    WS_CHILD | WS_VISIBLE | TVS_DISABLEDRAGDROP | TVS_HASBUTTONS | TVS_TRACKSELECT | TVS_LINESATROOT,
    0, 0, 0, 0, hWnd, (HMENU)ID_TREE_FILESYSTEM, lpCreateStruct->hInstance, nullptr);
  app.hStatusBar      = CreateWindow(STATUSCLASSNAME, nullptr,
    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
    0, 0, 0, 0, hWnd, (HMENU)ID_STATUSBAR, lpCreateStruct->hInstance, nullptr);
  app.hStaticInfo     = CreateWindow(WC_STATIC, nullptr,
    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
    0, 0, 0, 0, hWnd, (HMENU)ID_STATIC, lpCreateStruct->hInstance, nullptr);

  // ── Dark mode: allow dark common-control rendering for this window ─────────
  {
    HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
    using fnAllowDark = BOOL(WINAPI*)(HWND, BOOL);
    auto _Allow = reinterpret_cast<fnAllowDark>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
    if (_Allow) {
      _Allow(hWnd, TRUE);
      _Allow(app.hStatusBar, TRUE);
    }
    SendMessage(hWnd, WM_THEMECHANGED, 0, 0);
  }

  // Dark mode theming for controls
  SetWindowTheme(app.hTreeFileSystem, L"DarkMode_Explorer", nullptr);
  TreeView_SetBkColor(app.hTreeFileSystem,   CLR_DARK_INPUT);
  TreeView_SetTextColor(app.hTreeFileSystem, CLR_DARK_TEXT);
  SetWindowTheme(app.hButtonLoad,    L"DarkMode_Explorer", nullptr);
  SetWindowTheme(app.hButtonExctact, L"DarkMode_Explorer", nullptr);
  SetWindowTheme(app.hStatusBar,  L"DarkMode_Explorer", nullptr);
  SetWindowTheme(hWnd,            L"DarkMode_Explorer", nullptr);

  // Subclass status bar for dark custom painting
  g_fnOrigStatusBarProc = (WNDPROC)SetWindowLongPtrW(
    app.hStatusBar, GWLP_WNDPROC, (LONG_PTR)DarkStatusBarProc);

  // ── Texture preview panel ─────────────────────────────────────────────────
  app.hPreviewPanel = CreateWindow(L"PAZPreview", nullptr,
    WS_CHILD | WS_VISIBLE,
    0, 0, 0, 0, hWnd, (HMENU)ID_PREVIEW, lpCreateStruct->hInstance, nullptr);

  // ── Font ──────────────────────────────────────────────────────────────────
  app.hFont = CreateFont(FONT_SIZE, 0, 0, 0, FW_NORMAL, 0, 0, 0,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_ROMAN, FONT_FACE);

  SendMessage(app.hTreeFileSystem, WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStatusBar,      WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStaticInfo,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hButtonLoad,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hButtonExctact,  WM_SETFONT, (WPARAM)app.hFont, TRUE);

  // ── Status bar sections ───────────────────────────────────────────────────
  int parts[STATUSBAR_SECTION_COUNT] = { STATUSBAR_SECTION1, STATUSBAR_SECTION2, -1 };
  SendMessage(app.hStatusBar, SB_SETPARTS, STATUSBAR_SECTION_COUNT, (LPARAM)parts);
  SendMessage(app.hStatusBar, SB_SETTEXT, (WPARAM)0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());

  // ── Progress bar (inside status bar section 1) ────────────────────────────
  RECT rtArea;
  SendMessage(app.hStatusBar, SB_GETRECT, 1, (LPARAM)&rtArea);
  app.hProgressBar = CreateWindow(PROGRESS_CLASS, nullptr,
    WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
    rtArea.left, rtArea.top, rtArea.right - rtArea.left, rtArea.bottom - rtArea.top,
    app.hStatusBar, nullptr, lpCreateStruct->hInstance, nullptr);

  // ── WIC factory ──────────────────────────────────────────────────────────
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
    IID_PPV_ARGS(&app.pWICFactory));

  // ── Menu bar ─────────────────────────────────────────────────────────────
  {
    HMENU hFile  = CreatePopupMenu();
    AppendMenuW(hFile, MF_STRING,    ID_MENU_FILE_OPEN,    L"&Open Folder...");
    AppendMenuW(hFile, MF_STRING,    ID_MENU_FILE_EXTRACT, L"&Extract...");
    AppendMenuW(hFile, MF_STRING,    ID_MENU_FILE_EXPORT,  L"Export &Model / Skeleton...");
    AppendMenuW(hFile, MF_SEPARATOR, 0,                    nullptr);
    AppendMenuW(hFile, MF_STRING,    ID_MENU_FILE_EXIT,    L"E&xit");

    HMENU hCache = CreatePopupMenu();
    AppendMenuW(hCache, MF_STRING, ID_MENU_CACHE_REBUILD,    L"&Rebuild Cache");
    AppendMenuW(hCache, MF_STRING, ID_MENU_CACHE_CLEAR_TEMP, L"Clear &Preview Temp Files");

    // Settings — single item, no submenu needed
    HMENU hSettings = CreatePopupMenu();
    AppendMenuW(hSettings, MF_STRING, ID_MENU_SETTINGS, L"&Configure Paths...");

    HMENU hHelp = CreatePopupMenu();
    AppendMenuW(hHelp, MF_STRING,    ID_MENU_HELP_CHECK_UPDATE, L"Check for &Updates");
    AppendMenuW(hHelp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hHelp, MF_STRING,    ID_MENU_HELP_ABOUT,        L"&About");

    HMENU hMenu = CreateMenu();
    AppendMenuW(hMenu, MF_POPUP,  (UINT_PTR)hFile,         L"&File");
    AppendMenuW(hMenu, MF_POPUP,  (UINT_PTR)hCache,        L"&Cache");
    AppendMenuW(hMenu, MF_STRING, ID_MENU_SEARCH_OPEN,     L"&Search");  // direct action, no submenu
    AppendMenuW(hMenu, MF_POPUP,  (UINT_PTR)hSettings,     L"Se&ttings");
    AppendMenuW(hMenu, MF_POPUP,  (UINT_PTR)hHelp,         L"&Help");
    SetMenu(hWnd, hMenu);
  }

  // ── Background update check ──────────────────────────────────────────────
  // Silent: a newer release adds a clickable notice to the footer, and every
  // other outcome (offline, rate-limited, already current) is ignored.
  StartUpdateCheck(hWnd, true);

  return TRUE;
}

void Cls_OnDestroy(HWND hWnd) {
  if (app.hPreviewBitmap) { DeleteObject(app.hPreviewBitmap); app.hPreviewBitmap = nullptr; }
  if (app.pWICFactory)    { app.pWICFactory->Release();       app.pWICFactory    = nullptr; }
  if (app.hFont)          { DeleteObject(app.hFont);          app.hFont           = nullptr; }
  if (app.hBrushBg)       { DeleteObject(app.hBrushBg);       app.hBrushBg        = nullptr; }
  if (app.hBrushPanel)    { DeleteObject(app.hBrushPanel);    app.hBrushPanel     = nullptr; }
  if (app.hBrushInput)    { DeleteObject(app.hBrushInput);    app.hBrushInput     = nullptr; }
  // Save settings explicitly — ExitProcess skips all C++ destructors so ~Setting() never runs.
  app.CSetting.Save();
  // Exit immediately — letting ~AppData() run would cascade-free ~500k unique_ptr<Tree> nodes,
  // stalling the system heap for several seconds. OS reclaims all memory instantly.
  CoUninitialize();
  ExitProcess(0);
}

// Splitter band occupies [SplitterX(cx), SplitterX(cx) + SPLITTER_WIDTH).
static int SplitterX(int cx) {
  int x = (int)(cx * app.fDivideRatio + 0.5f) - SPLITTER_WIDTH / 2;
  int lo = (int)(cx * DIVIDE_RATIO_MIN);
  int hi = (int)(cx * DIVIDE_RATIO_MAX) - SPLITTER_WIDTH;
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  return x;
}

void Cls_OnSize(HWND hWnd, UINT state, int cx, int cy) {
  int nSplitX     = SplitterX(cx);
  int nTreeWidth  = nSplitX;                              // gap belongs to the splitter
  int nRightX     = nSplitX + SPLITTER_WIDTH;
  int nRightWidth = cx - nRightX;
  if (nRightWidth < 1) nRightWidth = 1;
  int nStatusH    = GetSystemMetrics(SM_CYMENU) + GetSystemMetrics(SM_CYBORDER) * 2;
  int nContentH   = cy - nStatusH - HEADER_HEIGHT;
  if (nContentH < 1) nContentH = 1;
  int nInfoH    = (int)(nContentH * INFO_RATIO);
  int nPreviewH = nContentH - nInfoH;

  // Row 0: Load | Extract buttons side by side across the left panel
  int nHalfW = nTreeWidth / 2;
  MoveWindow(app.hButtonLoad,    0,       0, nHalfW,            LOAD_HEIGHT, TRUE);
  MoveWindow(app.hButtonExctact, nHalfW,  0, nTreeWidth - nHalfW, LOAD_HEIGHT, TRUE);

  // Main content area (directly below Load button)
  MoveWindow(app.hTreeFileSystem, 0,       HEADER_HEIGHT,          nTreeWidth,  nContentH, TRUE);
  MoveWindow(app.hStaticInfo,     nRightX, HEADER_HEIGHT,          nRightWidth, nInfoH,     TRUE);
  MoveWindow(app.hPreviewPanel,   nRightX, HEADER_HEIGHT + nInfoH, nRightWidth, nPreviewH,  TRUE);
  MoveWindow(app.hStatusBar, 0, 0, 0, 0, TRUE);
}

void Cls_OnGetMinMaxInfo(HWND hWnd, LPMINMAXINFO lpMinMaxInfo) {
  lpMinMaxInfo->ptMinTrackSize.x = WINDOW_MIN_WIDTH;
  lpMinMaxInfo->ptMinTrackSize.y = WINDOW_MIN_HEIGHT;
}

void Cls_OnCommand(HWND hWnd, int id, HWND hwndCtl, UINT codeNotify) {
  switch (id) {
    case ID_BUTTON_LOAD:
      // Load from saved PAZ folder path — no browse dialog
      {
        if (app.bBusy) break;
        std::wstring saved;
        app.CSetting.getData(SETTING_LAST_FOLDER, saved, L"");
        if (!saved.empty()) {
          OpenPazFolder(hWnd, saved);
        } else {
          MessageBoxW(hWnd,
            L"No PAZ folder configured.\r\nUse Settings \u2192 Configure Paths to set your PAZ folder.",
            L"Load", MB_OK | MB_ICONINFORMATION);
        }
      }
      break;

    case ID_MENU_FILE_OPEN:
      {
        if (app.bBusy) break;
        std::wstring folderPath;
        std::wstring wsLastPath;
        app.CSetting.getData(SETTING_LAST_FOLDER, wsLastPath, L"C:\\");
        if (kukdh1::BrowseFolder(hWnd, app.CSetting.getString(kukdh1::Setting::ID_SELECT_FOLDER_TO_OPEN).c_str(), wsLastPath.c_str(), folderPath))
          OpenPazFolder(hWnd, folderPath);
      }
      break;

    case ID_BUTTON_EXTRACT:
    case ID_MENU_FILE_EXTRACT:
      {
        if (app.bBusy) break;
        TVITEM tvi = {};
        HTREEITEM hTree = TreeView_GetSelection(app.hTreeFileSystem);
        tvi.hItem = hTree;
        tvi.mask  = TVIF_PARAM;
        TreeView_GetItem(app.hTreeFileSystem, &tvi);
        if (tvi.lParam) {
          // Claim bBusy before spawning — a plain check would let two rapid
          // invocations (button + menu + right-click) start parallel extracts.
          bool expected = false;
          if (app.bBusy.compare_exchange_strong(expected, true)) {
            HANDLE hThread = CreateThread(nullptr, 0, ExtractThread, (LPVOID)tvi.lParam, 0, nullptr);
            CloseHandle(hThread);
          }
        }
      }
      break;

    case ID_MENU_FILE_EXPORT:
      {
        if (app.bBusy) break;
        TVITEM tvi = {};
        tvi.hItem = TreeView_GetSelection(app.hTreeFileSystem);
        tvi.mask  = TVIF_PARAM;
        TreeView_GetItem(app.hTreeFileSystem, &tvi);
        ExportSelectedModel(hWnd, (kukdh1::Tree *)tvi.lParam);
      }
      break;

    case ID_MENU_FILE_EXIT:
      DestroyWindow(hWnd);
      break;

    case ID_MENU_CACHE_REBUILD:
      if (!app.bBusy && app.CTree) {
        // Delete cache so FileThread rebuilds it from scratch
        DeleteFileW(kukdh1::CachePath(app.wsFolderPath).c_str());
        OpenPazFolder(hWnd, app.wsFolderPath);
      }
      break;

    case ID_MENU_CACHE_CLEAR_TEMP:
      {
        int n = kukdh1::ClearPreviewTempFiles();
        WCHAR buf[64];
        swprintf_s(buf, L"Deleted %d preview temp file(s).", n);
        MessageBoxW(hWnd, buf, L"Clear Temp Files", MB_OK | MB_ICONINFORMATION);
      }
      break;

    case ID_MENU_SEARCH_OPEN:
      if (!app.bBusy)
        ShowSearchWindow(hWnd);
      break;

    case ID_MENU_SETTINGS:
      if (!app.bBusy)
        ShowSettingsDialog(hWnd);
      break;

    case ID_MENU_HELP_CHECK_UPDATE:
    {
      // Disable item while checking to prevent double-clicks (Help is index 4)
      HMENU hBar = GetMenu(hWnd);
      HMENU hHelp = GetSubMenu(hBar, 4);
      EnableMenuItem(hHelp, ID_MENU_HELP_CHECK_UPDATE, MF_BYCOMMAND | MF_GRAYED);
      DrawMenuBar(hWnd);
      StartUpdateCheck(hWnd, false);
    }
    break;

    case ID_MENU_HELP_ABOUT:
      MessageBoxW(hWnd,
        L"PAZ Unpacker v" APP_VERSION L"\r\n\r\n"
        L"Community revival fork by sibercat\r\n"
        L"Original tool by kukdh1 (2015)\r\n\r\n"
        L"https://github.com/sibercat/PAZ-Unpacker",
        L"About PAZ Unpacker", MB_OK | MB_ICONINFORMATION);
      break;
  }
}

void Cls_OnDrawItem(HWND hWnd, const DRAWITEMSTRUCT *lpDrawItem) {
  if (lpDrawItem->CtlID == ID_STATIC) {
    // Dark background + text
    FillRect(lpDrawItem->hDC, &lpDrawItem->rcItem, app.hBrushBg);
    SetTextColor(lpDrawItem->hDC, CLR_DARK_TEXT);
    SetBkMode(lpDrawItem->hDC, TRANSPARENT);

    SIZE size;
    uint32_t uiLength = (uint32_t)SendMessage(lpDrawItem->hwndItem, WM_GETTEXTLENGTH, 0, 0);
    std::wstring text(uiLength + 1, L'\0');
    SendMessage(lpDrawItem->hwndItem, WM_GETTEXT, uiLength + 1, (LPARAM)text.data());
    text.resize(uiLength);

    GetTextExtentPoint32(lpDrawItem->hDC, text.c_str(), 1, &size);

    WCHAR *handle  = nullptr;
    WCHAR *pszLine = wcstok_s(text.data(), L"\r\n", &handle);
    for (int i = 0; ; i++) {
      if (pszLine == nullptr) break;
      RECT rtRect;
      SetRect(&rtRect, 4, size.cy * i + 4, lpDrawItem->rcItem.right - 4, size.cy * (i + 1) + 4);
      DrawText(lpDrawItem->hDC, pszLine, (int)wcslen(pszLine), &rtRect, DT_WORD_ELLIPSIS | DT_NOCLIP);
      pszLine = wcstok_s(nullptr, L"\r\n", &handle);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// UAH (undocumented) dark menu-bar messages — stable since Win10 1809
// Windows sends these when SetPreferredAppMode + AllowDarkModeForWindow are set.
// Handling them lets us paint the menu bar and items with our dark palette.
// ─────────────────────────────────────────────────────────────────────────────
#define WM_UAHDRAWMENU     0x0091
#define WM_UAHDRAWMENUITEM 0x0092

struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
struct UAHDRAWMENUITEM {
  DRAWITEMSTRUCT dis;
  struct { MENUITEMINFOW mii; } umi;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main WndProc
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
  switch (iMessage) {
    HANDLE_MSG(hWnd, WM_CREATE,       Cls_OnCreate);
    HANDLE_MSG(hWnd, WM_DESTROY,      Cls_OnDestroy);
    HANDLE_MSG(hWnd, WM_SIZE,         Cls_OnSize);
    HANDLE_MSG(hWnd, WM_GETMINMAXINFO,Cls_OnGetMinMaxInfo);
    HANDLE_MSG(hWnd, WM_COMMAND,      Cls_OnCommand);
    HANDLE_MSG(hWnd, WM_DRAWITEM,     Cls_OnDrawItem);

    case WM_APP_LOAD_FALLBACK:
      // Cache load failed or was stale — run full PAZ scan
      {
        HANDLE hThread = CreateThread(nullptr, 0, FileThread, nullptr, 0, nullptr);
        CloseHandle(hThread);
      }
      return 0;

    case WM_APP_UPDATE_RESULT:
      {
        // lParam = heap UpdateResult from CheckUpdateThread; we own it now.
        std::unique_ptr<UpdateResult> res((UpdateResult *)(LPARAM)lParam);
        if (!res) return 0;

        // A newer release always lights up the footer, however it was found.
        if (res->nCode == 1) {
          app.bUpdateAvailable = true;
          app.wsUpdateTag      = res->wsTag;
          InvalidateRect(app.hStatusBar, nullptr, TRUE);
        }

        // The startup check reports nothing else: no internet, a rate-limited
        // API or an up-to-date build must not interrupt the user.
        if (res->bSilent) return 0;

        // Re-enable the menu item (Help is index 4 — File/Cache/Search/Settings/Help)
        if (HMENU hBar = GetMenu(hWnd)) {
          EnableMenuItem(GetSubMenu(hBar, 4), ID_MENU_HELP_CHECK_UPDATE,
                         MF_BYCOMMAND | MF_ENABLED);
          DrawMenuBar(hWnd);
        }

        if (res->nCode == 1) {
          wchar_t msg[256];
          swprintf_s(msg,
            L"A new version is available!\r\n\r\n"
            L"Current:  v" APP_VERSION L"\r\n"
            L"Latest:   %s\r\n\r\n"
            L"Visit https://github.com/sibercat/PAZ-Unpacker/releases to download.",
            res->wsTag.c_str());
          MessageBoxW(hWnd, msg, L"Update Available", MB_OK | MB_ICONINFORMATION);
        } else if (res->nCode == 0) {
          MessageBoxW(hWnd,
            L"You are running the latest version (v" APP_VERSION L").",
            L"Up to Date", MB_OK | MB_ICONINFORMATION);
        } else {
          MessageBoxW(hWnd,
            L"Could not reach GitHub to check for updates.\r\nPlease check your internet connection.",
            L"Update Check Failed", MB_OK | MB_ICONWARNING);
        }
      }
      return 0;

    case WM_INITMENUPOPUP:
      {
        HMENU hSub = (HMENU)wParam;
        bool hasPaz = (app.CTree != nullptr && !app.bBusy);
        EnableMenuItem(hSub, ID_MENU_FILE_EXTRACT,     MF_BYCOMMAND | (hasPaz ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(hSub, ID_MENU_FILE_EXPORT,      MF_BYCOMMAND | (hasPaz ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(hSub, ID_MENU_CACHE_REBUILD,    MF_BYCOMMAND | (hasPaz ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(hSub, ID_MENU_CACHE_CLEAR_TEMP, MF_BYCOMMAND | (!app.bBusy ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(hSub, ID_MENU_SETTINGS,         MF_BYCOMMAND | (!app.bBusy ? MF_ENABLED : MF_GRAYED));
      }
      return 0;

    // ── Splitter: drag the band between the tree and the preview ─────────────
    case WM_SETCURSOR:
      if ((HWND)wParam == hWnd) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        RECT rc; GetClientRect(hWnd, &rc);
        int sx = SplitterX(rc.right);
        if (pt.y >= HEADER_HEIGHT && pt.x >= sx && pt.x < sx + SPLITTER_WIDTH) {
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      break;

    case WM_LBUTTONDOWN:
      {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        RECT rc; GetClientRect(hWnd, &rc);
        int sx = SplitterX(rc.right);
        if (y >= HEADER_HEIGHT && x >= sx && x < sx + SPLITTER_WIDTH) {
          app.bSplitterDrag = true;
          SetCapture(hWnd);
        }
      }
      return 0;

    case WM_MOUSEMOVE:
      if (app.bSplitterDrag) {
        RECT rc; GetClientRect(hWnd, &rc);
        if (rc.right > 0) {
          float ratio = (float)(GET_X_LPARAM(lParam) + SPLITTER_WIDTH / 2) / rc.right;
          if (ratio < DIVIDE_RATIO_MIN) ratio = DIVIDE_RATIO_MIN;
          if (ratio > DIVIDE_RATIO_MAX) ratio = DIVIDE_RATIO_MAX;
          if (ratio != app.fDivideRatio) {
            app.fDivideRatio = ratio;
            Cls_OnSize(hWnd, 0, rc.right, rc.bottom);
            app.bPamDirty = true;   // preview panel changed size
            InvalidateRect(hWnd, nullptr, FALSE);
          }
        }
      }
      return 0;

    case WM_LBUTTONUP:
      if (app.bSplitterDrag) {
        app.bSplitterDrag = false;
        ReleaseCapture();
      }
      return 0;

    case WM_ERASEBKGND:
      {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, app.hBrushBg);
        return 1;
      }

    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)wParam, CLR_DARK_TEXT);
      SetBkColor((HDC)wParam, CLR_DARK_INPUT);
      return (LRESULT)app.hBrushInput;

    case WM_CTLCOLORSTATIC:
      SetTextColor((HDC)wParam, CLR_DARK_TEXT);
      SetBkColor((HDC)wParam, CLR_DARK_BG);
      return (LRESULT)app.hBrushBg;

    case WM_CTLCOLORBTN:
      SetTextColor((HDC)wParam, CLR_DARK_TEXT);
      SetBkColor((HDC)wParam, CLR_DARK_PANEL);
      return (LRESULT)app.hBrushPanel;

    case WM_NOTIFY:
      {
        LPNMHDR hdr = (LPNMHDR)lParam;

        if (hdr->idFrom == ID_TREE_FILESYSTEM) {
          LPNMTREEVIEW ntv = (LPNMTREEVIEW)lParam;
          kukdh1::Tree *pTree;

          if (hdr->code == TVN_SELCHANGED) {
            pTree = (kukdh1::Tree *)ntv->itemNew.lParam;
            if (pTree != nullptr && !app.bBusy) {
              WCHAR pszBuffer[1024];
              std::wstring capacity;
              switch (pTree->GetType()) {
                case kukdh1::Tree::TREE_TYPE_ROOT:
                  if (app.CMeta != nullptr) {
                    kukdh1::ConvertCapacity(app.CTree->GetCapacity(), capacity);
                    swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_META_FILE_INFO).c_str(),
                      app.CMeta->uiVersion, app.CMeta->uiPAZFileCount, capacity.c_str());
                    SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  }
                  break;
                case kukdh1::Tree::TREE_TYPE_FOLDER:
                  kukdh1::ConvertCapacity(pTree->GetCapacity(), capacity);
                  swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_INTERNAL_FOLDER_INFO).c_str(),
                    pTree->GetName().c_str(), capacity.c_str());
                  SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  break;
                case kukdh1::Tree::TREE_TYPE_FILE:
                  kukdh1::ConvertCapacity(pTree->GetCapacity(), capacity);
                  swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_INTERNAL_FILE_INFO).c_str(),
                    pTree->GetName().c_str(), capacity.c_str(),
                    pTree->GetFileInfo().wsPazFullPath.c_str(),
                    pTree->GetFileInfo().sFullPath.c_str());
                  SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  break;
              }
              UpdatePreview(pTree);
              // Enable Extract for any selected node (file, folder, or root)
              EnableWindow(app.hButtonExctact, TRUE);
            }
          }
          else if (hdr->code == TVN_ITEMEXPANDING) {
            if (ntv->action == TVE_EXPAND) {
              pTree = (kukdh1::Tree *)ntv->itemNew.lParam;
              if (pTree != nullptr) {
                HANDLE hThread = CreateThread(nullptr, 0, AddThread, (LPVOID)pTree, 0, nullptr);
                CloseHandle(hThread);
              }
            }
          }
        }
      }
      return 0;

    case WM_CONTEXTMENU:
      // Right-click on the main TreeView — show an Extract popup
      if ((HWND)wParam == app.hTreeFileSystem) {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        TVHITTESTINFO ht = {};
        ht.pt = pt;
        ScreenToClient(app.hTreeFileSystem, &ht.pt);
        HTREEITEM hHit = TreeView_HitTest(app.hTreeFileSystem, &ht);
        if (hHit && (ht.flags & TVHT_ONITEM)) {
          TreeView_SelectItem(app.hTreeFileSystem, hHit);
          TVITEM tvi = {};
          tvi.hItem = hHit;
          tvi.mask  = TVIF_PARAM;
          TreeView_GetItem(app.hTreeFileSystem, &tvi);
          if (tvi.lParam) {
            kukdh1::Tree *pNode = (kukdh1::Tree *)tvi.lParam;

            // Export is offered on .pam models, .pac character meshes and
            // .pab skeletons only.
            bool isPam = false, isPab = false, isPac = false;
            if (pNode->GetType() == kukdh1::Tree::TREE_TYPE_FILE) {
              const std::string &fp = pNode->GetFileInfo().sFullPath;
              if (fp.size() >= 4) {
                std::string e = fp.substr(fp.size() - 4);
                std::transform(e.begin(), e.end(), e.begin(),
                               [](unsigned char c) { return (char)::tolower(c); });
                isPam = (e == ".pam");
                isPab = (e == ".pab");
                isPac = (e == ".pac");
              }
            }

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING | (app.bBusy ? MF_GRAYED : 0), 1, L"Extract");
            if (isPam)
              AppendMenuW(hMenu, MF_STRING | (app.bBusy ? MF_GRAYED : 0), 2,
                          L"Export Model (OBJ / FBX)...");
            else if (isPab)
              AppendMenuW(hMenu, MF_STRING | (app.bBusy ? MF_GRAYED : 0), 2,
                          L"Export Skeleton (FBX)...");
            else if (isPac)
              AppendMenuW(hMenu, MF_STRING | (app.bBusy ? MF_GRAYED : 0), 2,
                          L"Export Character Mesh + Skeleton (FBX)...");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);

            bool expected = false;
            if (cmd == 1 && app.bBusy.compare_exchange_strong(expected, true)) {
              HANDLE hThread = CreateThread(nullptr, 0, ExtractThread, (LPVOID)tvi.lParam, 0, nullptr);
              CloseHandle(hThread);
            }
            else if (cmd == 2 && !app.bBusy) {
              ExportSelectedModel(hWnd, pNode);
            }
          }
        }
        return 0;
      }
      break;

    // ── UAH dark menu-bar painting ───────────────────────────────────────────
    case WM_UAHDRAWMENU:
    {
      auto *p = reinterpret_cast<UAHMENU*>(lParam);
      MENUBARINFO mbi = { sizeof(mbi) };
      if (GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi)) {
        RECT rcWindow; GetWindowRect(hWnd, &rcWindow);
        RECT rc = mbi.rcBar;
        OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
        FillRect(p->hdc, &rc, app.hBrushBg);
      }
      return 0;
    }

    case WM_UAHDRAWMENUITEM:
    {
      auto *p = reinterpret_cast<UAHDRAWMENUITEM*>(lParam);
      bool sel = (p->dis.itemState & ODS_SELECTED) != 0;
      bool hot = (p->dis.itemState & ODS_HOTLIGHT) != 0;
      HBRUSH hBr = (sel || hot) ? app.hBrushPanel : app.hBrushBg;
      FillRect(p->dis.hDC, &p->dis.rcItem, hBr);

      wchar_t buf[256] = {};
      MENUITEMINFOW mii = {}; mii.cbSize = sizeof(mii);
      mii.fMask = MIIM_STRING; mii.dwTypeData = buf; mii.cch = _countof(buf);
      GetMenuItemInfoW(GetMenu(hWnd), p->dis.itemID, FALSE, &mii);

      SetTextColor(p->dis.hDC, CLR_DARK_TEXT);
      SetBkMode(p->dis.hDC, TRANSPARENT);
      if (app.hFont) SelectObject(p->dis.hDC, app.hFont);
      DrawTextW(p->dis.hDC, buf, -1, &p->dis.rcItem,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER);
      return 0;
    }
  }

  return DefWindowProc(hWnd, iMessage, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Open PAZ folder — shared by Open button, menu, and Rebuild Cache
// ─────────────────────────────────────────────────────────────────────────────
void OpenPazFolder(HWND hWnd, const std::wstring &folderPath) {
  // Close the Search window first — its result list holds raw Tree* pointers
  // into the tree we are about to destroy (its next repaint would read freed memory).
  if (app.hSearchWnd && IsWindow(app.hSearchWnd))
    DestroyWindow(app.hSearchWnd);

  // Reset UI
  TreeView_DeleteAllItems(app.hTreeFileSystem);
  SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)L"");
  if (app.hPreviewBitmap) { DeleteObject(app.hPreviewBitmap); app.hPreviewBitmap = nullptr; }
  InvalidateRect(app.hPreviewPanel, nullptr, TRUE);
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"");

  {
    // app.mtx guards app.CTree against an in-flight SearchThread still walking it
    std::lock_guard<std::mutex> lock(app.mtx);
    InvalidateFileNodeCache();   // holds raw pointers into the tree we're freeing
    app.CTree.reset();
    app.CMeta.reset();
  }
  app.wsFolderPath = folderPath;
  app.CSetting.setData(SETTING_LAST_FOLDER, app.wsFolderPath);

  WCHAR titleBuf[MAX_PATH + 64];
  swprintf_s(titleBuf, app.CSetting.getString(kukdh1::Setting::ID_CAPTION_WITH_PATH).c_str(), app.wsFolderPath.c_str());
  SetWindowText(hWnd, titleBuf);

  try {
    app.CMeta = std::make_unique<kukdh1::Meta>((wchar_t *)app.wsFolderPath.c_str());
    {
      std::lock_guard<std::mutex> lock(app.mtx);
      app.CTree = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_ROOT);
    }

    // Use cache if available and up-to-date, otherwise do full PAZ scan
    if (kukdh1::IsCacheValid(app.wsFolderPath)) {
      HANDLE hThread = CreateThread(nullptr, 0, CacheLoadThread, nullptr, 0, nullptr);
      CloseHandle(hThread);
    } else {
      HANDLE hThread = CreateThread(nullptr, 0, FileThread, nullptr, 0, nullptr);
      CloseHandle(hThread);
    }
  }
  catch (const std::exception &e) {
    std::wstring msg;
    int len = MultiByteToWideChar(CP_ACP, 0, e.what(), -1, nullptr, 0);
    if (len > 0) { msg.resize(len - 1); MultiByteToWideChar(CP_ACP, 0, e.what(), -1, msg.data(), len); }
    if (msg.empty()) msg = app.CSetting.getString(kukdh1::Setting::ID_NO_META_FILE_EXISTS);
    MessageBox(hWnd, msg.c_str(), app.CSetting.getString(kukdh1::Setting::ID_ALERT).c_str(), MB_OK);
    app.CMeta.reset();
    app.wsFolderPath.clear();
    SetWindowText(hWnd, app.CSetting.getString(kukdh1::Setting::ID_CAPTION).c_str());
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache load thread — fast path when paz_cache.bin is current
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI CacheLoadThread(LPVOID) {
  app.bBusy = true;
  EnableWindow(app.hTreeFileSystem, FALSE);
  EnableWindow(app.hButtonLoad,     FALSE);
  EnableWindow(app.hButtonExctact,  FALSE);
  SendMessage(app.hStatusBar, SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_BUSY).c_str());
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Loading from cache...");

  uint32_t pazVersion = 0, pazFileCount = 0;
  bool ok = kukdh1::LoadCache(app.wsFolderPath, app.CTree.get(), pazVersion, pazFileCount);

  if (!ok) {
    // Cache corrupt or stale — fall back to full scan
    DeleteFileW(kukdh1::CachePath(app.wsFolderPath).c_str());
    {
      std::lock_guard<std::mutex> lock(app.mtx);
      InvalidateFileNodeCache();
      app.CTree = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_ROOT);
    }
    EnableWindow(app.hTreeFileSystem, TRUE);
    EnableWindow(app.hButtonLoad,     TRUE);
    EnableWindow(app.hButtonExctact,  FALSE);  // stays disabled until a file node is selected
    app.bBusy = false;
    PostMessage(GetParent(app.hTreeFileSystem), WM_APP_LOAD_FALLBACK, 0, 0);
    return 0;
  }

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Sorting...");
  app.CTree->SortChild();
  app.CTree->UpdateCapacity();

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Building tree...");
  app.CTree->AddToTree(app.hTreeFileSystem);
  app.CTree->AddChildsToTree(app.hTreeFileSystem);

  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
  SendMessage(app.hStatusBar,   SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());
  SendMessage(app.hStatusBar,   SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());

  EnableWindow(app.hButtonLoad,     TRUE);
  EnableWindow(app.hButtonExctact,  FALSE);  // stays disabled until a file node is selected
  EnableWindow(app.hTreeFileSystem, TRUE);

  TreeView_Select(app.hTreeFileSystem, app.CTree->GetHandle(), TVGN_CARET);
  TreeView_Expand(app.hTreeFileSystem, app.CTree->GetHandle(), TVE_EXPAND);

  app.bBusy = false;
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings dialog
// ─────────────────────────────────────────────────────────────────────────────
#define IDC_EDIT_PAZ           101
#define IDC_EDIT_EXTRACT       102
#define IDC_BTN_BROWSE_PAZ     103
#define IDC_BTN_BROWSE_EXTRACT 104
#define IDC_BTN_SAVE           105
#define IDC_BTN_CANCEL         106
#define IDC_LBL_PAZ            107
#define IDC_LBL_EXTRACT        108

// Helper: apply dark colours to a settings-dialog control DC
static LRESULT SettingsDlgColor(HDC hdc, HBRUSH hBrush, COLORREF text, COLORREF bg) {
  SetTextColor(hdc, text);
  SetBkColor(hdc, bg);
  return (LRESULT)hBrush;
}

LRESULT CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
    {
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hDlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

      HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
      // Layout constants — all in client coordinates
      int W = 520, pad = 14, labelH = 18, editH = 26, btnW = 88, btnH = 28;
      int y = pad;

      // PAZ Folder row
      CreateWindow(WC_STATIC, L"PAZ Folder (pad00000.meta location):", WS_CHILD|WS_VISIBLE,
        pad, y, W-2*pad, labelH, hDlg, (HMENU)IDC_LBL_PAZ, hInst, nullptr);
      y += labelH + 4;
      CreateWindow(WC_EDIT, nullptr, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
        pad, y, W-2*pad-btnW-8, editH, hDlg, (HMENU)IDC_EDIT_PAZ, hInst, nullptr);
      CreateWindow(WC_BUTTON, L"Browse", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        W-pad-btnW, y, btnW, editH, hDlg, (HMENU)IDC_BTN_BROWSE_PAZ, hInst, nullptr);
      y += editH + pad;

      // Extract Path row
      CreateWindow(WC_STATIC, L"Default Extract Path:", WS_CHILD|WS_VISIBLE,
        pad, y, W-2*pad, labelH, hDlg, (HMENU)IDC_LBL_EXTRACT, hInst, nullptr);
      y += labelH + 4;
      CreateWindow(WC_EDIT, nullptr, WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
        pad, y, W-2*pad-btnW-8, editH, hDlg, (HMENU)IDC_EDIT_EXTRACT, hInst, nullptr);
      CreateWindow(WC_BUTTON, L"Browse", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        W-pad-btnW, y, btnW, editH, hDlg, (HMENU)IDC_BTN_BROWSE_EXTRACT, hInst, nullptr);
      y += editH + pad;

      // Save / Cancel buttons (right-aligned)
      CreateWindow(WC_BUTTON, L"Save", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
        W - 2*btnW - pad - 8, y, btnW, btnH, hDlg, (HMENU)IDC_BTN_SAVE, hInst, nullptr);
      CreateWindow(WC_BUTTON, L"Cancel", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        W - btnW - pad, y, btnW, btnH, hDlg, (HMENU)IDC_BTN_CANCEL, hInst, nullptr);

      // Set font on all children
      EnumChildWindows(hDlg, [](HWND hChild, LPARAM lp) -> BOOL {
        SendMessage(hChild, WM_SETFONT, lp, TRUE);
        return TRUE;
      }, (LPARAM)app.hFont);

      // Populate saved paths
      std::wstring paz, ext;
      app.CSetting.getData(SETTING_LAST_FOLDER,  paz, L"");
      app.CSetting.getData(SETTING_LAST_EXTRACT, ext, L"");
      SetWindowTextW(GetDlgItem(hDlg, IDC_EDIT_PAZ),     paz.c_str());
      SetWindowTextW(GetDlgItem(hDlg, IDC_EDIT_EXTRACT), ext.c_str());
      return 0;
    }

    case WM_CTLCOLOREDIT:
      return SettingsDlgColor((HDC)wParam, app.hBrushInput, CLR_DARK_TEXT, CLR_DARK_INPUT);
    case WM_CTLCOLORSTATIC:
      return SettingsDlgColor((HDC)wParam, app.hBrushBg, CLR_DARK_TEXT, CLR_DARK_BG);
    case WM_CTLCOLORBTN:
      return SettingsDlgColor((HDC)wParam, app.hBrushPanel, CLR_DARK_TEXT, CLR_DARK_PANEL);

    case WM_ERASEBKGND:
    {
      RECT rc; GetClientRect(hDlg, &rc);
      FillRect((HDC)wParam, &rc, app.hBrushBg);
      return 1;
    }

    case WM_COMMAND:
    {
      int ctrl = LOWORD(wParam);
      if (ctrl == IDC_BTN_BROWSE_PAZ || ctrl == IDC_BTN_BROWSE_EXTRACT) {
        bool isPaz = (ctrl == IDC_BTN_BROWSE_PAZ);
        int editId = isPaz ? IDC_EDIT_PAZ : IDC_EDIT_EXTRACT;
        WCHAR cur[MAX_PATH] = {};
        GetWindowTextW(GetDlgItem(hDlg, editId), cur, MAX_PATH);
        std::wstring chosen;
        const wchar_t *prompt = isPaz
          ? L"Select PAZ folder (containing pad00000.meta)"
          : L"Select default extract folder";
        if (kukdh1::BrowseFolder(hDlg, prompt, cur, chosen))
          SetWindowTextW(GetDlgItem(hDlg, editId), chosen.c_str());
      } else if (ctrl == IDC_BTN_SAVE) {
        WCHAR paz[MAX_PATH] = {}, ext[MAX_PATH] = {};
        GetWindowTextW(GetDlgItem(hDlg, IDC_EDIT_PAZ),     paz, MAX_PATH);
        GetWindowTextW(GetDlgItem(hDlg, IDC_EDIT_EXTRACT), ext, MAX_PATH);
        app.CSetting.setData(SETTING_LAST_FOLDER,  std::wstring(paz));
        app.CSetting.setData(SETTING_LAST_EXTRACT, std::wstring(ext));
        app.CSetting.Save();
        DestroyWindow(hDlg);
      } else if (ctrl == IDC_BTN_CANCEL) {
        DestroyWindow(hDlg);
      }
      return 0;
    }

    case WM_CLOSE:
      DestroyWindow(hDlg);
      return 0;

    case WM_DESTROY:
      EnableWindow(GetWindow(hDlg, GW_OWNER), TRUE);
      SetForegroundWindow(GetWindow(hDlg, GW_OWNER));
      return 0;
  }
  return DefWindowProc(hDlg, msg, wParam, lParam);
}

void ShowSettingsDialog(HWND hParent) {
  // Register class once
  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = SettingsDlgProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // we handle WM_ERASEBKGND
    wc.lpszClassName = L"PAZSettingsDlg";
    RegisterClassW(&wc);
    registered = true;
  }

  // H must account for non-client area (title bar ~30px + borders ~8px on Win11)
  // Client content: 14+18+4+26+14+18+4+26+14+28+14 = ~180px client → 220px window
  int W = 520, H = 225;
  RECT pr; GetWindowRect(hParent, &pr);
  int x = pr.left + (pr.right - pr.left - W) / 2;
  int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

  HWND hDlg = CreateWindowExW(
    WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW,
    L"PAZSettingsDlg", L"Settings",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
    x, y, W, H,
    hParent, nullptr, GetModuleHandleW(nullptr), nullptr);

  if (!hDlg) return;
  EnableWindow(hParent, FALSE);
  ShowWindow(hDlg, SW_SHOW);

  // Modal message loop — runs until hDlg is destroyed
  MSG msg;
  while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
    if (!IsDialogMessage(hDlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Search window
// Non-modal floating window: text input → virtual ListView of matching files.
// Uses LVS_OWNERDATA (virtual mode) + background search thread so the UI never
// freezes regardless of result count.
// ─────────────────────────────────────────────────────────────────────────────

// Results shared between search thread and UI thread.
// Written by search thread; read by LVN_GETDISPINFO on UI thread.
// Protected by g_searchGen: the thread only posts if its gen still matches.
static std::vector<kukdh1::Tree*> g_searchResults;
static std::atomic<DWORD>         g_searchGen { 0 };
static size_t                     g_totalFiles = 0;

struct SearchParams {
  HWND   hWnd;   // search window to post WM_APP_SEARCH_DONE to
  DWORD  gen;    // generation this thread was started with
  std::wstring pat;
};

static DWORD WINAPI SearchThread(LPVOID arg) {
  auto *p = reinterpret_cast<SearchParams*>(arg);
  HWND  hWnd = p->hWnd;
  DWORD gen  = p->gen;
  std::wstring pat = std::move(p->pat);
  delete p;

  auto *results = new std::vector<kukdh1::Tree*>();
  {
    // Hold app.mtx for the whole walk — OpenPazFolder resets app.CTree under
    // the same lock, so nodes can't be freed while we're reading them.
    std::lock_guard<std::mutex> lock(app.mtx);
    if (app.CTree && !pat.empty()) {
      std::vector<kukdh1::Tree*> allFiles;
      app.CTree->GetFileNodeList(allFiles);

      results->reserve(std::min<size_t>(allFiles.size(), 4096));
      for (auto *node : allFiles) {
        if (g_searchGen.load() != gen) { delete results; return 0; }  // cancelled
        const std::string &fp = node->GetFileInfo().sFullPath;
        std::wstring wfp;
        kukdh1::ConvertWidechar(fp, wfp);
        std::transform(wfp.begin(), wfp.end(), wfp.begin(), ::towlower);
        if (wfp.find(pat) != std::wstring::npos)
          results->push_back(node);
      }
    }
  }

  // PostMessage fails if the window was destroyed before we finished — free then.
  if (g_searchGen.load() != gen ||
      !PostMessage(hWnd, WM_APP_SEARCH_DONE, (WPARAM)gen, (LPARAM)results))
    delete results;
  return 0;
}

static void SearchWnd_KickSearch(HWND hWnd) {
  if (app.bBusy) return;  // tree is being rebuilt — don't race the load thread

  HWND hEdit = GetDlgItem(hWnd, ID_SEARCH_EDIT);
  int len = GetWindowTextLengthW(hEdit);
  std::wstring pat(len + 1, L'\0');
  GetWindowTextW(hEdit, pat.data(), len + 1);
  pat.resize(len);
  std::transform(pat.begin(), pat.end(), pat.begin(), ::towlower);

  // Bump generation — any running thread with an old gen will discard its results
  DWORD gen = ++g_searchGen;

  // Show "searching…" immediately
  SetWindowTextW(GetDlgItem(hWnd, ID_SEARCH_COUNT), L"Searching\u2026");

  auto *p  = new SearchParams { hWnd, gen, std::move(pat) };
  HANDLE h = CreateThread(nullptr, 0, SearchThread, p, 0, nullptr);
  CloseHandle(h);
}

// Force-expands the TreeView path to pTarget so it becomes visible and selectable.
// Needed because the TreeView uses lazy loading — deep items have no HTREEITEM until expanded.
static void ForceExpandToNode(HWND hTree, kukdh1::Tree *pTarget) {
  const std::string &fullPath = pTarget->GetFileInfo().sFullPath;
  std::vector<std::string> pathParts;
  kukdh1::ParsePath(fullPath, pathParts);
  if (pathParts.size() < 2) return;

  kukdh1::Tree *ptr = app.CTree.get();
  // Walk each folder level, ensuring children are added to the TreeView widget
  for (size_t k = 0; k + 1 < pathParts.size(); k++) {
    if (!ptr->IsChildAdded()) ptr->AddChildsToTree(hTree);
    kukdh1::Tree *child = ptr->GetChildFolderWithName(pathParts[k]);
    if (!child) return;
    if (!child->IsAdded()) child->AddToTree(hTree);
    ptr = child;
  }
  // Ensure the file node itself is added (children of its parent folder)
  if (!ptr->IsChildAdded()) ptr->AddChildsToTree(hTree);
}

LRESULT CALLBACK SearchWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
    {
      BOOL dark = TRUE;
      DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
      HMODULE hUxtheme = GetModuleHandleW(L"uxtheme.dll");
      using fnAllowDark = BOOL(WINAPI*)(HWND, BOOL);
      auto _Allow = reinterpret_cast<fnAllowDark>(GetProcAddress(hUxtheme, MAKEINTRESOURCEA(133)));
      if (_Allow) _Allow(hWnd, TRUE);
      SetWindowTheme(hWnd, L"DarkMode_Explorer", nullptr);

      HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
      RECT rc; GetClientRect(hWnd, &rc);
      int W = rc.right, pad = 8, editH = 26, countH = 20;

      // Count label (top right)
      CreateWindowW(WC_STATIC, L"",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, pad, W, countH, hWnd, (HMENU)ID_SEARCH_COUNT, hInst, nullptr);

      // Search input
      HWND hEdit = CreateWindowW(WC_EDIT, nullptr,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        pad, pad + countH + 4, W - 2*pad, editH, hWnd, (HMENU)ID_SEARCH_EDIT, hInst, nullptr);
      SendMessage(hEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type to search... (e.g. .dds, ui_texture, ci_ingame)");

      // Virtual ListView — LVS_OWNERDATA means no items are stored in the control;
      // we supply text on demand via LVN_GETDISPINFO from g_searchResults.
      int listY = pad + countH + 4 + editH + 4;
      HWND hList = CreateWindowExW(0, WC_LISTVIEW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, listY, W, rc.bottom - listY,
        hWnd, (HMENU)ID_SEARCH_LIST, hInst, nullptr);
      ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
      SetWindowTheme(hList, L"DarkMode_Explorer", nullptr);
      ListView_SetBkColor(hList,     CLR_DARK_INPUT);
      ListView_SetTextBkColor(hList, CLR_DARK_INPUT);
      ListView_SetTextColor(hList,   CLR_DARK_TEXT);

      // Subclass header for full dark painting
      HWND hHeader = ListView_GetHeader(hList);
      SetWindowTheme(hHeader, L"", L"");
      g_fnOrigHeaderProc = (WNDPROC)SetWindowLongPtrW(
        hHeader, GWLP_WNDPROC, (LONG_PTR)DarkHeaderProc);

      // Columns
      LVCOLUMNW col = {};
      col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
      col.fmt  = LVCFMT_LEFT;
      col.cx   = 580; col.pszText = (LPWSTR)L"Path";     ListView_InsertColumn(hList, 0, &col);
      col.cx   = 90;  col.pszText = (LPWSTR)L"Size";     ListView_InsertColumn(hList, 1, &col);
      col.cx   = 130; col.pszText = (LPWSTR)L"PAZ File"; ListView_InsertColumn(hList, 2, &col);

      SendMessage(GetDlgItem(hWnd, ID_SEARCH_COUNT), WM_SETFONT, (WPARAM)app.hFont, TRUE);
      SendMessage(hEdit,  WM_SETFONT, (WPARAM)app.hFont, TRUE);
      SendMessage(hList,  WM_SETFONT, (WPARAM)app.hFont, TRUE);

      app.hSearchWnd = hWnd;
      g_searchResults.clear();

      if (app.CTree) {
        std::vector<kukdh1::Tree*> tmp;
        app.CTree->GetFileNodeList(tmp);
        g_totalFiles = tmp.size();
        wchar_t buf[64];
        swprintf_s(buf, L"%zu files \u2014 type to search", g_totalFiles);
        SetWindowTextW(GetDlgItem(hWnd, ID_SEARCH_COUNT), buf);
      } else {
        g_totalFiles = 0;
        SetWindowTextW(GetDlgItem(hWnd, ID_SEARCH_COUNT),
          L"No PAZ folder loaded \u2014 use Load button first");
      }
      return 0;
    }

    case WM_SIZE:
    {
      int W = LOWORD(lParam), H = HIWORD(lParam);
      int pad = 8, editH = 26, countH = 20;
      int listY = pad + countH + 4 + editH + 4;
      MoveWindow(GetDlgItem(hWnd, ID_SEARCH_COUNT), 0, pad, W, countH, TRUE);
      MoveWindow(GetDlgItem(hWnd, ID_SEARCH_EDIT), pad, pad + countH + 4, W - 2*pad, editH, TRUE);
      MoveWindow(GetDlgItem(hWnd, ID_SEARCH_LIST), 0, listY, W, H - listY, TRUE);
      return 0;
    }

    case WM_ERASEBKGND:
    {
      RECT rc; GetClientRect(hWnd, &rc);
      FillRect((HDC)wParam, &rc, app.hBrushBg);
      return 1;
    }

    case WM_CTLCOLOREDIT:
      SetTextColor((HDC)wParam, CLR_DARK_TEXT);
      SetBkColor((HDC)wParam, CLR_DARK_INPUT);
      return (LRESULT)app.hBrushInput;

    case WM_CTLCOLORSTATIC:
      SetTextColor((HDC)wParam, CLR_DARK_TEXT2);
      SetBkColor((HDC)wParam, CLR_DARK_BG);
      return (LRESULT)app.hBrushBg;

    case WM_COMMAND:
      if (LOWORD(wParam) == ID_SEARCH_EDIT && HIWORD(wParam) == EN_CHANGE)
        SearchWnd_KickSearch(hWnd);
      return 0;

    case WM_APP_SEARCH_DONE:
    {
      // wParam = generation this result belongs to
      if ((DWORD)wParam != g_searchGen.load()) {
        // Stale result from a cancelled search — discard
        delete reinterpret_cast<std::vector<kukdh1::Tree*>*>(lParam);
        return 0;
      }
      auto *results = reinterpret_cast<std::vector<kukdh1::Tree*>*>(lParam);
      g_searchResults = std::move(*results);
      delete results;

      HWND hList = GetDlgItem(hWnd, ID_SEARCH_LIST);
      // Tell the virtual ListView how many rows there are — no item insertion
      ListView_SetItemCountEx(hList, (int)g_searchResults.size(), LVSICF_NOINVALIDATEALL);
      InvalidateRect(hList, nullptr, TRUE);

      wchar_t buf[64];
      if (g_searchResults.empty() && g_totalFiles > 0) {
        swprintf_s(buf, L"%zu files \u2014 type to search", g_totalFiles);
      } else {
        swprintf_s(buf, L"%zu result(s) of %zu total",
                   g_searchResults.size(), g_totalFiles);
      }
      SetWindowTextW(GetDlgItem(hWnd, ID_SEARCH_COUNT), buf);
      return 0;
    }

    case WM_NOTIFY:
    {
      LPNMHDR hdr = (LPNMHDR)lParam;

      // Virtual ListView: supply item text on demand
      if (hdr->idFrom == ID_SEARCH_LIST && hdr->code == LVN_GETDISPINFOW) {
        auto *pdi = reinterpret_cast<NMLVDISPINFOW*>(lParam);
        int idx = pdi->item.iItem;
        if (idx < 0 || idx >= (int)g_searchResults.size()) return 0;
        kukdh1::Tree *node = g_searchResults[idx];
        const auto &fi = node->GetFileInfo();

        // We use a per-item static buffer via thread-local storage trick:
        // the ListView only reads pszText until the next LVN_GETDISPINFO,
        // so a single static wchar_t buffer per column is safe here.
        static wchar_t s_path[512], s_size[32], s_paz[64];

        if (pdi->item.mask & LVIF_TEXT) {
          switch (pdi->item.iSubItem) {
            case 0:
            {
              std::wstring tmp; kukdh1::ConvertWidechar(fi.sFullPath, tmp);
              wcsncpy_s(s_path, tmp.c_str(), _TRUNCATE);
              pdi->item.pszText = s_path;
              break;
            }
            case 1:
              if (fi.uiOriginalSize >= 1024*1024)
                swprintf_s(s_size, L"%.2f MB", fi.uiOriginalSize / 1048576.0);
              else if (fi.uiOriginalSize >= 1024)
                swprintf_s(s_size, L"%.1f KB", fi.uiOriginalSize / 1024.0);
              else
                swprintf_s(s_size, L"%u B", fi.uiOriginalSize);
              pdi->item.pszText = s_size;
              break;
            case 2:
            {
              std::wstring paz = fi.wsPazFullPath;
              size_t sl = paz.rfind(L'\\');
              if (sl != std::wstring::npos) paz = paz.substr(sl + 1);
              wcsncpy_s(s_paz, paz.c_str(), _TRUNCATE);
              pdi->item.pszText = s_paz;
              break;
            }
          }
        }
        return 0;
      }

      if (hdr->idFrom == ID_SEARCH_LIST && hdr->code == NM_DBLCLK) {
        HWND hList = GetDlgItem(hWnd, ID_SEARCH_LIST);
        int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (sel < 0 || sel >= (int)g_searchResults.size()) return 0;
        kukdh1::Tree *pTree = g_searchResults[sel];
        if (pTree) {
          // Force-expand path if item hasn't been added to the TreeView widget yet
          if (!pTree->GetHandle()) {
            ForceExpandToNode(app.hTreeFileSystem, pTree);
          }
          if (pTree->GetHandle()) {
            HTREEITEM hItem  = pTree->GetHandle();
            HWND      hParent = GetParent(hWnd);
            // Navigate and select BEFORE closing (safe: pTree owned by app.CTree, not g_searchResults)
            TreeView_EnsureVisible(app.hTreeFileSystem, hItem);
            TreeView_SelectItem(app.hTreeFileSystem, hItem);
            SetForegroundWindow(hParent);
            SetFocus(app.hTreeFileSystem);  // highlight requires focus on the TreeView
            DestroyWindow(hWnd);
            return 0;
          }
          DestroyWindow(hWnd);
        }
      }
      return 0;
    }

    case WM_CONTEXTMENU:
    {
      HWND hList = GetDlgItem(hWnd, ID_SEARCH_LIST);
      if ((HWND)wParam != hList) break;
      int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
      if (sel < 0 || sel >= (int)g_searchResults.size()) break;

      HMENU hMenu = CreatePopupMenu();
      AppendMenuW(hMenu, MF_STRING | (app.bBusy ? MF_GRAYED : 0), 1, L"Extract");

      int mx = GET_X_LPARAM(lParam), my = GET_Y_LPARAM(lParam);
      if (mx == -1 && my == -1) {
        // Keyboard invoke — position near selected item
        RECT rc;
        ListView_GetItemRect(hList, sel, &rc, LVIR_BOUNDS);
        POINT pt = { rc.left + 4, rc.bottom };
        ClientToScreen(hList, &pt);
        mx = pt.x; my = pt.y;
      }

      int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, mx, my, 0, hWnd, nullptr);
      DestroyMenu(hMenu);

      if (cmd == 1) {
        kukdh1::Tree *pTree = g_searchResults[sel];
        bool expected = false;
        if (pTree && app.bBusy.compare_exchange_strong(expected, true)) {
          HANDLE h = CreateThread(nullptr, 0, ExtractThread, pTree, 0, nullptr);
          CloseHandle(h);
        }
      }
      return 0;
    }

    case WM_CLOSE:
      DestroyWindow(hWnd);
      return 0;

    case WM_DESTROY:
      ++g_searchGen;  // cancel any running search thread
      g_searchResults.clear();
      app.hSearchWnd = nullptr;
      return 0;
  }
  return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowSearchWindow(HWND hParent) {
  // If already open, bring it to front
  if (app.hSearchWnd && IsWindow(app.hSearchWnd)) {
    SetForegroundWindow(app.hSearchWnd);
    return;
  }

  static bool registered = false;
  if (!registered) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = SearchWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PAZSearchWnd";
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);
    registered = true;
  }

  RECT pr; GetWindowRect(hParent, &pr);
  int W = 900, H = 600;
  int x = pr.left + (pr.right - pr.left - W) / 2;
  int y = pr.top  + (pr.bottom - pr.top  - H) / 2;

  CreateWindowExW(
    WS_EX_APPWINDOW,
    L"PAZSearchWnd", L"Search Files",
    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
    x, y, W, H,
    hParent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Update check thread
// Calls GitHub releases API, compares tag_name with APP_VERSION.
// arg = heap UpdateCheckReq, owned by this thread.
// Posts WM_APP_UPDATE_RESULT with lParam = heap UpdateResult, owned by the
// window proc. wParam mirrors the result code for convenience.
// ─────────────────────────────────────────────────────────────────────────────
static void PostUpdateResult(HWND hWnd, bool bSilent, int nCode,
                             const std::wstring &wsTag = std::wstring()) {
  auto *res = new UpdateResult{ nCode, bSilent, wsTag };
  if (!PostMessage(hWnd, WM_APP_UPDATE_RESULT, (WPARAM)(INT_PTR)nCode, (LPARAM)res))
    delete res;   // window already gone; nobody will take ownership
}

void StartUpdateCheck(HWND hWnd, bool bSilent) {
  auto *req = new UpdateCheckReq{ hWnd, bSilent };
  HANDLE hThread = CreateThread(nullptr, 0, CheckUpdateThread, req, 0, nullptr);
  if (hThread) CloseHandle(hThread);
  else         delete req;
}

DWORD WINAPI CheckUpdateThread(LPVOID arg) {
  std::unique_ptr<UpdateCheckReq> req((UpdateCheckReq *)arg);
  HWND hWnd    = req->hWnd;
  bool bSilent = req->bSilent;

  HINTERNET hSession = WinHttpOpen(L"PAZ-Unpacker/" APP_VERSION,
                                   WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }

  HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com",
                                      INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!hConnect) {
    WinHttpCloseHandle(hSession);
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }

  HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
    L"/repos/sibercat/PAZ-Unpacker/releases/latest",
    nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
    WINHTTP_FLAG_SECURE);
  if (!hRequest) {
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }

  bool sent = WinHttpSendRequest(hRequest,
    WINHTTP_NO_ADDITIONAL_HEADERS, 0,
    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
    WinHttpReceiveResponse(hRequest, nullptr);

  if (!sent) {
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }

  // Read response body
  std::string body;
  DWORD avail = 0;
  while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
    std::string chunk(avail, '\0');
    DWORD read = 0;
    WinHttpReadData(hRequest, chunk.data(), avail, &read);
    body.append(chunk.data(), read);
  }

  WinHttpCloseHandle(hRequest);
  WinHttpCloseHandle(hConnect);
  WinHttpCloseHandle(hSession);

  // Parse "tag_name" from JSON — simple search, no full parser needed
  // GitHub returns: "tag_name":"v2.1.0"
  const std::string key = "\"tag_name\":\"";
  size_t pos = body.find(key);
  if (pos == std::string::npos) {
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }
  pos += key.size();
  size_t end = body.find('"', pos);
  if (end == std::string::npos) {
    PostUpdateResult(hWnd, bSilent, -1);
    return 0;
  }

  std::string tagA = body.substr(pos, end - pos);  // e.g. "v2.1.0"

  // Strip leading 'v' for comparison with APP_VERSION (which has no 'v')
  std::string tagStripped = (!tagA.empty() && tagA[0] == 'v') ? tagA.substr(1) : tagA;
  constexpr char appVerA[] = APP_VERSION_A;

  // Compare numerically — a remote tag older than or equal to this build
  // (e.g. running a dev build ahead of the latest release) is "up to date".
  int rMaj = 0, rMin = 0, rPat = 0, lMaj = 0, lMin = 0, lPat = 0;
  sscanf_s(tagStripped.c_str(), "%d.%d.%d", &rMaj, &rMin, &rPat);
  sscanf_s(appVerA,             "%d.%d.%d", &lMaj, &lMin, &lPat);
  bool newer = (rMaj != lMaj) ? (rMaj > lMaj)
             : (rMin != lMin) ? (rMin > lMin)
             : (rPat > lPat);

  if (!newer) {
    PostUpdateResult(hWnd, bSilent, 0);
  } else {
    // Convert tag to wchar_t for display
    int wlen = MultiByteToWideChar(CP_UTF8, 0, tagA.c_str(), -1, nullptr, 0);
    std::wstring tagW(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1)
      MultiByteToWideChar(CP_UTF8, 0, tagA.c_str(), -1, tagW.data(), wlen);
    PostUpdateResult(hWnd, bSilent, 1, tagW);
  }
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// File loading thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI FileThread(LPVOID arg) {
  app.bBusy = true;
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);
  std::vector<std::string> paths;
  WCHAR buffer[128];

  EnableWindow(app.hButtonLoad, FALSE);
  EnableWindow(app.hTreeFileSystem, FALSE);
  SendMessage(app.hStatusBar, SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_BUSY).c_str());
  SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, app.CMeta->uiPAZFileCount);

  uint32_t i = 0;
  for (const auto &paz_entry : app.CMeta->vPAZs) {
    SendMessage(app.hProgressBar, PBM_SETPOS, i++, 0);
    swprintf_s(buffer, app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READING).c_str(), i, app.CMeta->uiPAZFileCount);
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buffer);

    try {
      kukdh1::PazFile paz((wchar_t *)app.wsFolderPath.c_str(), paz_entry.uiPazFileID, cipher);

      for (auto &file : paz.vFileInfo) {
        kukdh1::Tree *ptr = app.CTree.get();
        kukdh1::ParsePath(file.sFullPath, paths);

        for (auto path = paths.begin(); path != paths.end() - 1; ++path) {
          kukdh1::Tree *temp = ptr->GetChildFolderWithName(*path);
          if (temp) {
            ptr = temp;
          }
          else {
            auto node = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_FOLDER);
            node->SetFolderInfo(ptr, *path);
            ptr = ptr->AddChild(std::move(node));
          }
        }

        auto fileNode = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_FILE);
        fileNode->SetFileInfo(ptr, paths.back(), file);
        ptr->AddChild(std::move(fileNode));
      }
    }
    catch (const std::exception &) {
      // Skip unreadable PAZ files
    }
  }

  SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, 3);
  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_SORTING).c_str());
  app.CTree->SortChild();
  SendMessage(app.hProgressBar, PBM_SETPOS, 1, 0);

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_CAPACITY).c_str());
  app.CTree->UpdateCapacity();
  SendMessage(app.hProgressBar, PBM_SETPOS, 2, 0);

  // Write index cache so future opens skip the PAZ scan
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Writing cache...");
  kukdh1::WriteCache(app.wsFolderPath, app.CTree.get(),
    app.CMeta->uiVersion, app.CMeta->uiPAZFileCount);

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_ADDING).c_str());
  app.CTree->AddToTree(app.hTreeFileSystem);
  app.CTree->AddChildsToTree(app.hTreeFileSystem);
  SendMessage(app.hProgressBar, PBM_SETPOS, 3, 0);

  SendMessage(app.hStatusBar, SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());
  EnableWindow(app.hButtonLoad,     TRUE);
  EnableWindow(app.hButtonExctact,  FALSE);  // stays disabled until a file node is selected
  EnableWindow(app.hTreeFileSystem, TRUE);

  TreeView_Select(app.hTreeFileSystem, app.CTree->GetHandle(), TVGN_CARET);
  TreeView_Expand(app.hTreeFileSystem, app.CTree->GetHandle(), TVE_EXPAND);

  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());

  app.bBusy = false;
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Extraction helpers
// ─────────────────────────────────────────────────────────────────────────────

// Scan a PAM file's raw bytes for embedded texture filenames (.dds/.png/.bmp).
// Returns deduplicated list of filenames found (e.g. "Trim_Wood_out_01.dds").
static std::vector<std::string> ScanPamTextures(const std::wstring &path) {
  std::vector<std::string> result;
  std::ifstream f(path, std::ios::binary);
  if (!f) return result;
  std::vector<char> data((std::istreambuf_iterator<char>(f)), {});

  std::unordered_set<std::string> seen;
  for (size_t i = 0; i + 4 <= data.size(); ) {
    unsigned char c = (unsigned char)data[i];
    if (c >= 0x20 && c <= 0x7e) {
      size_t j = i;
      while (j < data.size() && (unsigned char)data[j] >= 0x20 && (unsigned char)data[j] <= 0x7e) j++;
      size_t len = j - i;
      if (len >= 5 && len < 260) {
        std::string s(data.begin() + i, data.begin() + j);
        if (s.size() >= 4) {
          std::string ext = s.substr(s.size() - 4);
          std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char x){ return (char)::tolower(x); });
          if ((ext == ".dds" || ext == ".png" || ext == ".bmp") && !seen.count(s)) {
            result.push_back(s);
            seen.insert(s);
          }
        }
      }
      i = j;
    } else {
      i++;
    }
  }
  return result;
}

bool CheckEncrypt(const std::string &filename, uint32_t size) {
  assert(!filename.empty());
  if (filename.length() < 5) return false;
  return filename.compare(filename.length() - 5, 5, ".dbss") == 0;
}

bool ExtractFile(const std::wstring &path, const kukdh1::FileInfo &file, kukdh1::Crypt &cipher) {
  assert(!path.empty());

  // Runtime checks, not asserts — asserts compile out in Release and a corrupt
  // record would otherwise hit UB below.
  if (file.uiCompressedSize == 0 || file.uiOriginalSize == 0) return false;

  bool bCompressed = (file.uiOriginalSize > file.uiCompressedSize);
  bool bEncrypted  = !CheckEncrypt(file.sFullPath, file.uiCompressedSize);

  std::fstream pazfile(file.wsPazFullPath, std::ios::in | std::ios::binary);
  if (!pazfile.is_open()) return false;

  std::fstream savefile(path, std::ios::out | std::ios::binary);
  if (!savefile.is_open()) return false;

  uint32_t length = file.uiCompressedSize;
  std::vector<uint8_t> encrypted(length);
  pazfile.seekg(file.uiOffset);
  pazfile.read(reinterpret_cast<char *>(encrypted.data()), length);
  pazfile.close();

  auto free_deleter = [](uint8_t *p) { if (p) free(p); };
  std::unique_ptr<uint8_t, decltype(free_deleter)> owned_decrypt(nullptr, free_deleter);
  uint8_t *raw_decrypted  = nullptr;
  uint32_t decrypted_len  = length;

  if (bEncrypted) {
    try {
      cipher.decrypt(encrypted.data(), length, &raw_decrypted, &decrypted_len);
    }
    catch (...) {
      return false;  // corrupt record — length not a cipher-block multiple, etc.
    }
    owned_decrypt.reset(raw_decrypted);
  }
  else {
    raw_decrypted = encrypted.data();
    decrypted_len = length;
  }

  // Read the payload's compression-header length if one can be present.
  // Header layout: byte0 flags (0x01 = packed; 0x02 = 32-bit length at +5,
  // otherwise 8-bit length at +2).
  uint32_t embeddedLen = 0;
  bool haveHeader = false;
  if (decrypted_len >= 3) {
    if (raw_decrypted[0] & 0x02) {
      if (decrypted_len >= 9) {
        embeddedLen = *reinterpret_cast<const uint32_t *>(raw_decrypted + 5);
        haveHeader = true;
      }
    } else {
      embeddedLen = raw_decrypted[2];
      haveHeader = true;
    }
  }

  // Unpack when the index says the file shrank, or when an unshrunken payload
  // carries the stored-block wrapper (0x6E) with a length that matches the
  // index — a plain file merely starting with 'n' won't satisfy both.
  bool bUnpack = haveHeader &&
    (bCompressed || (raw_decrypted[0] == 0x6E && embeddedLen == file.uiOriginalSize));

  if (bUnpack) {
    // Never let decompress() write past the buffer the index sized for us.
    if (embeddedLen > file.uiOriginalSize) return false;
    std::vector<uint8_t> decompressed(file.uiOriginalSize);
    kukdh1::decompress(raw_decrypted, decompressed.data());
    owned_decrypt.reset();
    savefile.write(reinterpret_cast<const char *>(decompressed.data()), file.uiOriginalSize);
  }
  else if (bCompressed) {
    return false;  // index says compressed but payload is too short for a header
  }
  else {
    savefile.write(reinterpret_cast<const char *>(raw_decrypted),
                   std::min<uint32_t>(decrypted_len, file.uiOriginalSize));
  }

  savefile.close();
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Extraction thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI ExtractThread(LPVOID arg) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  app.bBusy = true;
  kukdh1::Tree     *CTree = (kukdh1::Tree *)arg;
  kukdh1::CryptICE  cipher(ICE_KEY, ICE_KEY_LEN);
  WCHAR buffer[128];
  std::wstring sFolderPath;
  std::wstring sLastExtractPath;
  int totalTexExtracted = 0;

  app.CSetting.getData(SETTING_LAST_EXTRACT, sLastExtractPath, L"");
  EnableWindow(app.hButtonExctact, FALSE);

  // Use saved extract path directly; only browse if none is configured yet.
  if (!sLastExtractPath.empty()) {
    sFolderPath = sLastExtractPath;
  } else if (!kukdh1::BrowseFolder(nullptr,
      app.CSetting.getString(kukdh1::Setting::ID_SELECT_FOLDER_TO_SAVE).c_str(),
      app.wsFolderPath.c_str(), sFolderPath)) {
    // User cancelled — bail out
    EnableWindow(app.hButtonExctact, TRUE);
    app.bBusy = false;
    CoUninitialize();
    return 0;
  } else {
    app.CSetting.setData(SETTING_LAST_EXTRACT, sFolderPath);
  }

  {
    std::vector<kukdh1::FileInfo> vFileList;
    CTree->GetFileList(vFileList);
    uint32_t uiFiles = (uint32_t)vFileList.size();

    if (!sFolderPath.empty() && sFolderPath.back() != L'\\') sFolderPath.push_back(L'\\');

    SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, uiFiles);
    uint32_t i = 1;

    // Tracks filenames already extracted (from file list or texture scan) to avoid duplicates.
    std::unordered_set<std::string> extractedNames;
    // Lazily-built index: lowercase filename → first matching Tree node (for texture lookup).
    std::unordered_map<std::string, kukdh1::Tree*> nameIdx;

    for (auto &info : vFileList) {
      std::vector<std::string> paths;
      kukdh1::ParsePath(info.sFullPath, paths);

      std::wstring dirPath = sFolderPath;
      for (auto path = paths.begin(); path != paths.end() - 1; ++path) {
        std::wstring folder;
        kukdh1::ConvertWidechar(*path, folder);
        dirPath.append(folder).append(L"\\");
      }

      std::error_code ec;
      fs::create_directories(dirPath, ec);
      if (ec) {
        MessageBox(nullptr,
          app.CSetting.getString(kukdh1::Setting::ID_DIRECTORY_CREATE_FAILED).c_str(),
          app.CSetting.getString(kukdh1::Setting::ID_ERROR).c_str(),
          MB_OK | MB_ICONERROR);
        break;
      }

      std::wstring fileName;
      kukdh1::ConvertWidechar(paths.back(), fileName);
      std::wstring savePath = dirPath + fileName;

      swprintf_s(buffer, app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_EXTRACT).c_str(), i, uiFiles);
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buffer);
      ExtractFile(savePath, info, cipher);
      SendMessage(app.hProgressBar, PBM_SETPOS, i++, 0);

      extractedNames.insert(paths.back());

      // If this is a PAM model file, extract its referenced textures alongside it.
      std::string baseName = paths.back();
      std::string baseExt  = baseName.size() >= 4 ? baseName.substr(baseName.size() - 4) : "";
      std::transform(baseExt.begin(), baseExt.end(), baseExt.begin(), [](unsigned char x){ return (char)::tolower(x); });
      if (baseExt == ".pam") {
        // Build the full-tree filename index once per extraction operation.
        if (nameIdx.empty() && app.CTree) {
          SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Building texture index...");
          std::vector<kukdh1::Tree*> allNodes;
          app.CTree->GetFileNodeList(allNodes);
          for (auto *n : allNodes) {
            std::string nm  = n->GetName();
            std::string nml = nm;
            std::transform(nml.begin(), nml.end(), nml.begin(), [](unsigned char x){ return (char)::tolower(x); });
            if (!nameIdx.count(nml)) nameIdx[nml] = n;
          }
        }

        auto texNames = ScanPamTextures(savePath);
        int pamTexExtracted = 0;
        for (auto &texName : texNames) {
          if (extractedNames.count(texName)) continue;
          std::string texLower = texName;
          std::transform(texLower.begin(), texLower.end(), texLower.begin(), [](unsigned char x){ return (char)::tolower(x); });
          auto it = nameIdx.find(texLower);
          if (it != nameIdx.end()) {
            std::wstring wTexName;
            kukdh1::ConvertWidechar(texName, wTexName);

            // Ensure the output directory exists (texture may be in a different PAZ folder)
            std::error_code ec2;
            fs::create_directories(dirPath, ec2);

            if (ExtractFile(dirPath + wTexName, it->second->GetFileInfo(), cipher)) {
              extractedNames.insert(texName);
              pamTexExtracted++;
              totalTexExtracted++;
            }
          }
        }

        // Show diagnostic: how many textures were found/extracted for this PAM
        swprintf_s(buffer, L"PAM: %d/%d textures extracted",
                   pamTexExtracted, (int)texNames.size());
        SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buffer);

      }
    }
  }

  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
  if (totalTexExtracted > 0) {
    swprintf_s(buffer, L"Done (+ %d textures)", totalTexExtracted);
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buffer);
  } else {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());
  }
  EnableWindow(app.hButtonExctact, TRUE);
  app.bBusy = false;
  CoUninitialize();
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Model export (OBJ / FBX)
// ─────────────────────────────────────────────────────────────────────────────

// Re-encodes an image to PNG at its native resolution. The preview decoder
// caps textures at 512px, which is fine on screen but would throw away detail
// in an export.
static bool SaveTextureAsPng(const std::wstring &srcPath, const std::wstring &dstPath) {
  if (!app.pWICFactory) return false;

  IWICBitmapDecoder     *decoder = nullptr;
  IWICBitmapFrameDecode *frame   = nullptr;
  IWICFormatConverter   *conv    = nullptr;
  IWICBitmapEncoder     *encoder = nullptr;
  IWICBitmapFrameEncode *outFrame = nullptr;
  IWICStream            *stream  = nullptr;
  bool ok = false;

  auto cleanup = [&]() {
    if (outFrame) outFrame->Release();
    if (encoder)  encoder->Release();
    if (stream)   stream->Release();
    if (conv)     conv->Release();
    if (frame)    frame->Release();
    if (decoder)  decoder->Release();
  };

  if (FAILED(app.pWICFactory->CreateDecoderFromFilename(
        srcPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) {
    cleanup(); return false;
  }
  if (FAILED(decoder->GetFrame(0, &frame))) { cleanup(); return false; }

  if (FAILED(app.pWICFactory->CreateFormatConverter(&conv))) { cleanup(); return false; }
  WICPixelFormatGUID srcFmt = {};
  frame->GetPixelFormat(&srcFmt);
  BOOL can = FALSE;
  conv->CanConvert(srcFmt, GUID_WICPixelFormat32bppBGRA, &can);
  if (!can) { cleanup(); return false; }
  if (FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    cleanup(); return false;
  }

  if (FAILED(app.pWICFactory->CreateStream(&stream))) { cleanup(); return false; }
  if (FAILED(stream->InitializeFromFilename(dstPath.c_str(), GENERIC_WRITE))) {
    cleanup(); return false;
  }
  if (FAILED(app.pWICFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) {
    cleanup(); return false;
  }
  if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) { cleanup(); return false; }
  if (FAILED(encoder->CreateNewFrame(&outFrame, nullptr)))          { cleanup(); return false; }
  if (FAILED(outFrame->Initialize(nullptr)))                        { cleanup(); return false; }

  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  outFrame->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  outFrame->SetPixelFormat(&fmt);

  if (SUCCEEDED(outFrame->WriteSource(conv, nullptr)) &&
      SUCCEEDED(outFrame->Commit()) &&
      SUCCEEDED(encoder->Commit())) {
    ok = true;
  }

  cleanup();
  return ok;
}

// Extracts every texture the model references into outDir as PNG. Fills
// vOutFiles with the file name written per submesh (empty when unresolved).
static void ExportModelTextures(const kukdh1::PamModel &model,
                                const std::wstring &wsOutDir,
                                kukdh1::PamTextureFileList &vOutFiles) {
  vOutFiles.assign(model.vSubmeshes.size(), std::string());
  if (!app.CTree) return;

  WCHAR tempDir[MAX_PATH];
  GetTempPath(MAX_PATH, tempDir);
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);

  // name -> written png file (so a texture shared by several submeshes is
  // extracted and converted only once)
  std::unordered_map<std::string, std::string> done;

  for (size_t i = 0; i < model.vSubmeshes.size(); i++) {
    const std::string &texName = model.vSubmeshes[i].sTexture;
    if (texName.empty()) continue;

    std::string key = texName;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });

    auto it = done.find(key);
    if (it != done.end()) { vOutFiles[i] = it->second; continue; }

    // Locate the texture anywhere in the archive by file name.
    kukdh1::Tree *node = nullptr;
    for (auto *n : GetCachedFileNodes()) {
      const std::string &nm = n->GetName();
      if (nm.size() == texName.size() && _stricmp(nm.c_str(), texName.c_str()) == 0) {
        node = n;
        break;
      }
    }
    if (!node) { done[key] = std::string(); continue; }

    std::wstring wTexName;
    kukdh1::ConvertWidechar(texName, wTexName);
    std::wstring tempPath = std::wstring(tempDir) + L"paz_export_" + wTexName;

    std::string pngName;
    if (ExtractFile(tempPath, node->GetFileInfo(), cipher)) {
      std::wstring wPng = wTexName;
      size_t dot = wPng.find_last_of(L'.');
      if (dot != std::wstring::npos) wPng = wPng.substr(0, dot);
      wPng += L".png";

      if (SaveTextureAsPng(tempPath, wsOutDir + L"\\" + wPng)) {
        std::string narrow;
        narrow.reserve(wPng.size());
        for (wchar_t c : wPng) narrow.push_back((c < 128) ? (char)c : '_');
        pngName = narrow;
      }
      DeleteFile(tempPath.c_str());
    }

    done[key]   = pngName;
    vOutFiles[i] = pngName;
  }
}

struct ExportParams {
  kukdh1::Tree           *pTree;
  std::wstring            wsPath;
  kukdh1::PamExportFormat format;
};

// A .pac names no skeleton, but the archive lays them out by convention:
//   character/model/1_pc/11_pgw/armor/9_upperbody/pgw_00_ub_0001.pac
//   character/model/1_pc/11_pgw/pgw_01.pab
// so the rig is the .pab whose name starts with the mesh's prefix -- the text
// up to the first underscore. Prefer <prefix>_01.pab, which is the base rig.
static kukdh1::Tree *FindSkeletonFor(const std::string &sPacPath) {
  size_t slash = sPacPath.rfind('/');
  std::string base = (slash != std::string::npos) ? sPacPath.substr(slash + 1) : sPacPath;
  size_t us = base.find('_');
  if (us == std::string::npos || us == 0) return nullptr;

  std::string prefix = base.substr(0, us);
  std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                 [](unsigned char c) { return (char)::tolower(c); });

  kukdh1::Tree *best = nullptr;
  for (kukdh1::Tree *n : GetCachedFileNodes()) {
    const std::string &p = n->GetFileInfo().sFullPath;
    if (p.size() < 5 || p.compare(p.size() - 4, 4, ".pab") != 0) continue;
    size_t s = p.rfind('/');
    std::string b = (s != std::string::npos) ? p.substr(s + 1) : p;
    std::transform(b.begin(), b.end(), b.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    if (b.compare(0, prefix.size(), prefix) != 0) continue;
    if (b.size() <= prefix.size() || b[prefix.size()] != '_') continue;
    if (b == prefix + "_01.pab") return n;      // the base rig, done
    if (!best) best = n;
  }
  return best;
}

DWORD WINAPI ExportThread(LPVOID arg) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::unique_ptr<ExportParams> p(reinterpret_cast<ExportParams *>(arg));

  SendMessage(app.hStatusBar, SB_SETTEXT, 0,
    (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_BUSY).c_str());
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Exporting model...");

  std::wstring msg;
  bool ok = false;

  // Extract and parse the .pam fresh: the preview may be showing a different
  // file, or nothing at all.
  const kukdh1::FileInfo &info = p->pTree->GetFileInfo();
  size_t slash = info.sFullPath.rfind('/');
  std::string name = (slash != std::string::npos)
    ? info.sFullPath.substr(slash + 1) : info.sFullPath;
  std::wstring wName;
  kukdh1::ConvertWidechar(name, wName);

  WCHAR tempDir[MAX_PATH];
  GetTempPath(MAX_PATH, tempDir);
  std::wstring tempPath = std::wstring(tempDir) + L"paz_export_" + wName;

  std::string lowerPath = info.sFullPath;
  std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                 [](unsigned char c) { return (char)::tolower(c); });
  const bool isSkeleton = lowerPath.size() >= 4 &&
                          lowerPath.compare(lowerPath.size() - 4, 4, ".pab") == 0;
  const bool isSkinned  = lowerPath.size() >= 4 &&
                          lowerPath.compare(lowerPath.size() - 4, 4, ".pac") == 0;

  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);
  if (!ExtractFile(tempPath, info, cipher)) {
    msg = L"Could not extract the model from the archive.";
  }
  else if (isSkinned) {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Locating skeleton...");
    kukdh1::PacModel mesh;
    bool loaded = mesh.Load(tempPath);
    DeleteFile(tempPath.c_str());

    if (!loaded || mesh.IsEmpty()) {
      msg = L"The character mesh could not be parsed.\r\n\r\n"
            L"Version 1 .pac files are not supported yet.";
    }
    else if (kukdh1::Tree *pSkelNode = FindSkeletonFor(info.sFullPath)) {
      const kukdh1::FileInfo &sinfo = pSkelNode->GetFileInfo();
      std::wstring skelTemp = std::wstring(tempDir) + L"paz_export_skel.pab";
      kukdh1::PabSkeleton skel;
      bool sok = ExtractFile(skelTemp, sinfo, cipher) && skel.Load(skelTemp);
      DeleteFile(skelTemp.c_str());

      if (!sok || skel.IsEmpty()) {
        msg = L"Found a skeleton for this mesh but could not parse it.";
      }
      else {
        SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Writing skinned mesh...");
        std::wstring err;
        if (kukdh1::ExportSkinnedModel(mesh, skel, p->wsPath, err)) {
          ok = true;
          WCHAR buf[320];
          swprintf_s(buf,
            L"Exported %zu verts, %zu tris, %zu submesh(es), skinned to "
            L"%zu bones.", mesh.TotalVertices(), mesh.TotalTriangles(),
            mesh.vSubmeshes.size(), skel.vBones.size());
          msg = buf;
        } else {
          msg = err;
        }
      }
    }
    else {
      msg = L"No matching .pab skeleton was found in the archive for this "
            L"mesh, so it cannot be skinned.";
    }
  }
  else if (isSkeleton) {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Writing skeleton...");
    kukdh1::PabSkeleton skel;
    bool loaded = skel.Load(tempPath);
    DeleteFile(tempPath.c_str());

    if (!loaded || skel.IsEmpty()) {
      msg = L"The skeleton could not be parsed.";
    }
    else {
      std::wstring err;
      if (kukdh1::ExportSkeleton(skel, p->wsPath, err)) {
        ok = true;
        WCHAR buf[256];
        int depth = 0;
        std::vector<int> d(skel.vBones.size(), 0);
        for (size_t i = 0; i < skel.vBones.size(); i++) {
          const int32_t par = skel.vBones[i].iParent;
          d[i] = (par < 0) ? 0 : d[par] + 1;
          if (d[i] > depth) depth = d[i];
        }
        swprintf_s(buf, L"Exported skeleton: %zu bones, depth %d.",
                   skel.vBones.size(), depth);
        msg = buf;
      } else {
        msg = err;
      }
    }
  }
  else {
    kukdh1::PamModel model;
    bool loaded = model.Load(tempPath);
    DeleteFile(tempPath.c_str());

    if (!loaded || model.IsEmpty()) {
      msg = L"The model could not be parsed.";
    }
    else {
      std::wstring dir = std::filesystem::path(p->wsPath).parent_path().wstring();

      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Exporting textures...");
      kukdh1::PamTextureFileList texFiles;
      ExportModelTextures(model, dir, texFiles);

      size_t texOk = 0;
      for (const auto &t : texFiles) if (!t.empty()) texOk++;

      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Writing model file...");
      std::wstring err;
      if (kukdh1::ExportModel(model, p->wsPath, p->format, texFiles, err)) {
        ok = true;
        WCHAR buf[256];
        swprintf_s(buf,
          L"Exported %zu verts, %zu tris, %zu material(s), %zu/%zu textures.",
          model.vVertices.size(), model.vIndices.size() / 3,
          model.vSubmeshes.size(), texOk, texFiles.size());
        msg = buf;
      }
      else {
        msg = err.empty() ? L"The export failed." : err;
      }
    }
  }

  SendMessage(app.hStatusBar, SB_SETTEXT, 0,
    (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)(ok ? L"Export complete" : L"Export failed"));

  MessageBoxW(nullptr, msg.c_str(), ok ? L"Export Complete" : L"Export Failed",
              MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));

  app.bBusy = false;
  CoUninitialize();
  return 0;
}

// Validates the selection, asks for a destination, then hands off to the
// worker thread. The file type chosen in the dialog selects the format.
void ExportSelectedModel(HWND hWnd, kukdh1::Tree *pTree) {
  if (!pTree || pTree->GetType() != kukdh1::Tree::TREE_TYPE_FILE) {
    MessageBoxW(hWnd, L"Select a .pam model file in the tree first.",
                L"Export Model", MB_OK | MB_ICONINFORMATION);
    return;
  }

  const std::string &full = pTree->GetFileInfo().sFullPath;
  std::string ext = (full.size() >= 4) ? full.substr(full.size() - 4) : "";
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char c) { return (char)::tolower(c); });

  const bool isSkeleton = (ext == ".pab");
  const bool isSkinned  = (ext == ".pac");
  if (ext != ".pam" && !isSkeleton && !isSkinned) {
    MessageBoxW(hWnd,
      L"Only .pam models, .pac character meshes and .pab skeletons can be "
      L"exported.\r\n\r\nSelect one in the tree and try again.",
      L"Export Model", MB_OK | MB_ICONINFORMATION);
    return;
  }

  // Suggest the file's own name, minus its extension.
  size_t slash = full.rfind('/');
  std::string base = (slash != std::string::npos) ? full.substr(slash + 1) : full;
  if (base.size() > 4) base = base.substr(0, base.size() - 4);
  std::wstring wBase;
  kukdh1::ConvertWidechar(base, wBase);

  // Neither a bone hierarchy nor a skin survives OBJ, so those offer FBX only.
  const bool fbxOnly = isSkeleton || isSkinned;
  const COMDLG_FILTERSPEC meshFilters[] = {
    { L"Wavefront OBJ (*.obj)",  L"*.obj" },
    { L"Autodesk FBX (*.fbx)",   L"*.fbx" },
  };
  const COMDLG_FILTERSPEC fbxFilters[] = {
    { L"Autodesk FBX (*.fbx)",   L"*.fbx" },
  };

  std::wstring startDir;
  app.CSetting.getData(SETTING_LAST_EXPORT, startDir, L"");

  const wchar_t *title = isSkeleton ? L"Export Skeleton"
                       : isSkinned  ? L"Export Character Mesh"
                                    : L"Export Model";

  UINT filterIndex = 1;
  std::wstring outPath;
  if (!kukdh1::SaveFileDialog(hWnd, title,
                              (wBase + (fbxOnly ? L".fbx" : L".obj")).c_str(),
                              fbxOnly ? fbxFilters : meshFilters,
                              fbxOnly ? ARRAYSIZE(fbxFilters) : ARRAYSIZE(meshFilters),
                              startDir.c_str(), filterIndex, outPath))
    return;

  kukdh1::PamExportFormat fmt = (fbxOnly || filterIndex == 2)
                                  ? kukdh1::PAM_EXPORT_FBX
                                  : kukdh1::PAM_EXPORT_OBJ;

  // The dialog does not always append the extension when the name already
  // contains a dot, so make sure it matches the chosen type.
  std::filesystem::path outFs(outPath);
  std::wstring want = std::wstring(L".") + kukdh1::ExportExtension(fmt);
  std::wstring have = outFs.extension().wstring();
  std::transform(have.begin(), have.end(), have.begin(), ::towlower);
  if (have != want) {
    outFs.replace_extension(want);
    outPath = outFs.wstring();
  }

  app.CSetting.setData(SETTING_LAST_EXPORT, outFs.parent_path().wstring());
  app.CSetting.Save();

  bool expected = false;
  if (!app.bBusy.compare_exchange_strong(expected, true)) return;

  auto *p = new ExportParams{ pTree, outPath, fmt };
  HANDLE h = CreateThread(nullptr, 0, ExportThread, p, 0, nullptr);
  if (h) CloseHandle(h);
  else { delete p; app.bBusy = false; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lazy-load tree expansion thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI AddThread(LPVOID arg) {
  kukdh1::Tree *pTree = (kukdh1::Tree *)arg;

  if (!pTree->IsGrandchildAdded()) {
    app.bBusy = true;
    EnableWindow(app.hTreeFileSystem, FALSE);
    EnableWindow(app.hButtonExctact, FALSE);

    std::wstring statusMsg = app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_NEW_ADDING);
    pTree->AddGrandchildsToTree(app.hTreeFileSystem, (LPVOID)statusMsg.c_str(),
      [&](LPVOID arg, size_t i, size_t count) {
        WCHAR *pStatusMsg = (WCHAR *)arg;
        WCHAR  buf[128];
        if (i == 0) SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, count);
        swprintf_s(buf, pStatusMsg, i, count);
        SendMessage(app.hProgressBar, PBM_SETPOS,  i, 0);
        SendMessage(app.hStatusBar,   SB_SETTEXT, 2, (LPARAM)buf);
      });

    SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());

    EnableWindow(app.hButtonExctact, TRUE);
    EnableWindow(app.hTreeFileSystem, TRUE);
    app.bBusy = false;
  }

  return 0;
}


// ─────────────────────────────────────────────────────────────────────────────
// Texture preview
// ─────────────────────────────────────────────────────────────────────────────
// Fallback for uncompressed (legacy-header) DDS that the WIC DDS codec rejects
// (e.g. dwMipMapCount=0, non-standard caps flags, etc.).
// Handles 32-bit BGRA/RGBA uncompressed DDS only.
static HBITMAP LoadRawDDS(const std::wstring &path, int maxW, int maxH) {
  if (!app.pWICFactory || maxW < 1 || maxH < 1) return nullptr;

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return nullptr;
  auto fileBytes = static_cast<size_t>(f.tellg());
  if (fileBytes < 128) return nullptr;
  f.seekg(0);

  std::vector<uint8_t> data(fileBytes);
  f.read(reinterpret_cast<char *>(data.data()), fileBytes);
  f.close();

  if (memcmp(data.data(), "DDS ", 4) != 0) return nullptr;

  auto ru32 = [&](size_t off) {
    return *reinterpret_cast<const uint32_t *>(data.data() + off);
  };

  uint32_t height  = ru32(12);
  uint32_t width   = ru32(16);
  uint32_t pfFlags = ru32(80);
  uint32_t fourCC  = ru32(84);
  uint32_t bitCnt  = ru32(88);
  uint32_t rMask   = ru32(92);
  uint32_t gMask   = ru32(96);
  uint32_t bMask   = ru32(100);
  uint32_t aMask   = ru32(104);

  constexpr uint32_t DDPF_ALPHAPIXELS = 0x01;
  constexpr uint32_t DDPF_FOURCC      = 0x04;
  constexpr uint32_t DDPF_RGB         = 0x40;

  // Must be uncompressed RGB(A) with no FourCC, exactly 32 bpp
  if (!(pfFlags & DDPF_RGB) || (pfFlags & DDPF_FOURCC) || fourCC != 0) return nullptr;
  if (bitCnt != 32 || width == 0 || height == 0) return nullptr;

  // Determine WIC pixel format from bit masks
  WICPixelFormatGUID srcFmt;
  if (rMask == 0x00FF0000 && gMask == 0x0000FF00 && bMask == 0x000000FF) {
    srcFmt = (aMask != 0) ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat32bppBGR;
  } else if (rMask == 0x000000FF && gMask == 0x0000FF00 && bMask == 0x00FF0000) {
    srcFmt = (aMask != 0) ? GUID_WICPixelFormat32bppRGBA : GUID_WICPixelFormat32bppRGB;
  } else {
    return nullptr;  // Unknown channel layout
  }

  size_t pixelOffset = 128;  // No DX10 extension header (FourCC would be "DX10")
  size_t pixelBytes  = static_cast<size_t>(width) * height * 4;
  if (data.size() < pixelOffset + pixelBytes) return nullptr;

  // Create a WIC bitmap from the raw pixel data
  IWICBitmap *pBitmap = nullptr;
  HRESULT hr = app.pWICFactory->CreateBitmapFromMemory(
    width, height, srcFmt,
    width * 4, static_cast<UINT>(pixelBytes),
    data.data() + pixelOffset, &pBitmap);
  if (FAILED(hr) || !pBitmap) return nullptr;

  // Scale to fit, never upscale
  float scale = min(static_cast<float>(maxW) / width, static_cast<float>(maxH) / height);
  if (scale > 1.0f) scale = 1.0f;
  UINT dstW = max(1u, static_cast<UINT>(width  * scale));
  UINT dstH = max(1u, static_cast<UINT>(height * scale));

  IWICBitmapScaler    *scaler = nullptr;
  IWICFormatConverter *conv   = nullptr;
  HBITMAP              hResult = nullptr;

  auto cleanup = [&]() {
    if (conv)    { conv->Release();    conv    = nullptr; }
    if (scaler)  { scaler->Release();  scaler  = nullptr; }
    if (pBitmap) { pBitmap->Release(); pBitmap = nullptr; }
  };

  hr = app.pWICFactory->CreateBitmapScaler(&scaler);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  hr = scaler->Initialize(pBitmap, dstW, dstH, WICBitmapInterpolationModeCubic);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  hr = app.pWICFactory->CreateFormatConverter(&conv);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  BOOL canConvert = FALSE;
  conv->CanConvert(srcFmt, GUID_WICPixelFormat32bppBGRA, &canConvert);
  if (!canConvert) { cleanup(); return nullptr; }

  hr = conv->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  BITMAPINFO bmi              = {};
  bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth       = static_cast<LONG>(dstW);
  bmi.bmiHeader.biHeight      = -static_cast<LONG>(dstH);
  bmi.bmiHeader.biPlanes      = 1;
  bmi.bmiHeader.biBitCount    = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *pvBits = nullptr;
  HDC   hdc    = GetDC(nullptr);
  hResult      = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
  ReleaseDC(nullptr, hdc);

  if (hResult && pvBits) {
    WICRect rc = { 0, 0, static_cast<INT>(dstW), static_cast<INT>(dstH) };
    if (FAILED(conv->CopyPixels(&rc, dstW * 4, dstW * dstH * 4, static_cast<BYTE *>(pvBits)))) {
      DeleteObject(hResult);
      hResult = nullptr;
    }
  }

  cleanup();
  return hResult;
}

HBITMAP LoadWICBitmap(const std::wstring &path, int maxW, int maxH) {
  if (!app.pWICFactory || maxW < 1 || maxH < 1) return nullptr;

  IWICBitmapDecoder     *decoder   = nullptr;
  IWICBitmapFrameDecode *frame     = nullptr;
  IWICFormatConverter   *converter = nullptr;
  IWICBitmapScaler      *scaler    = nullptr;
  HBITMAP                hResult  = nullptr;

  auto cleanup = [&]() {
    if (scaler)    { scaler->Release();    scaler    = nullptr; }
    if (converter) { converter->Release(); converter = nullptr; }
    if (frame)     { frame->Release();     frame     = nullptr; }
    if (decoder)   { decoder->Release();   decoder   = nullptr; }
  };

  HRESULT hr = app.pWICFactory->CreateDecoderFromFilename(
    path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr)) { cleanup(); return LoadRawDDS(path, maxW, maxH); }

  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) { cleanup(); return LoadRawDDS(path, maxW, maxH); }

  hr = app.pWICFactory->CreateFormatConverter(&converter);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  // Try converting to 32bpp BGRA. Some DDS formats (BC6H float, etc.)
  // can't convert directly — fall back to checking canConvert first.
  BOOL canConvert = FALSE;
  WICPixelFormatGUID srcFmt = {};
  frame->GetPixelFormat(&srcFmt);
  converter->CanConvert(srcFmt, GUID_WICPixelFormat32bppBGRA, &canConvert);
  if (!canConvert) { cleanup(); return LoadRawDDS(path, maxW, maxH); }

  hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
    WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  UINT srcW = 0, srcH = 0;
  converter->GetSize(&srcW, &srcH);
  if (srcW == 0 || srcH == 0) { cleanup(); return nullptr; }

  // Scale to fit, never upscale
  float scale = min((float)maxW / srcW, (float)maxH / srcH);
  if (scale > 1.0f) scale = 1.0f;
  UINT dstW = max(1u, (UINT)(srcW * scale));
  UINT dstH = max(1u, (UINT)(srcH * scale));

  hr = app.pWICFactory->CreateBitmapScaler(&scaler);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  hr = scaler->Initialize(converter, dstW, dstH, WICBitmapInterpolationModeCubic);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  BITMAPINFO bmi        = {};
  bmi.bmiHeader.biSize  = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth  = (LONG)dstW;
  bmi.bmiHeader.biHeight = -(LONG)dstH;   // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void *pvBits = nullptr;
  HDC hdc = GetDC(nullptr);
  hResult = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pvBits, nullptr, 0);
  ReleaseDC(nullptr, hdc);

  if (hResult && pvBits) {
    WICRect rc = { 0, 0, (INT)dstW, (INT)dstH };
    if (FAILED(scaler->CopyPixels(&rc, dstW * 4, dstW * dstH * 4, (BYTE*)pvBits))) {
      DeleteObject(hResult);
      hResult = nullptr;
    }
  }

  cleanup();
  return hResult;
}

// ─────────────────────────────────────────────────────────────────────────────
// 3D model preview (.pam)
// Renders with the software rasteriser straight into a DIB section, which the
// preview panel then blits — same path the texture preview already uses.
// ─────────────────────────────────────────────────────────────────────────────
void ClearPamPreview() {
  app.CPamModel.reset();
  app.vPamTextures.clear();
  app.PamTarget.pPixels = nullptr;
  app.PamTarget.nWidth  = 0;
  app.PamTarget.nHeight = 0;
  app.pPamPixels   = nullptr;
  app.nPamWidth    = 0;
  app.nPamHeight   = 0;
  app.bPamOrbiting = false;
  app.bPamPanning  = false;
  app.bPamDirty    = false;
}

// Decodes an image file to raw top-down 0xAARRGGBB pixels, downscaled so the
// longest edge is at most PAM_TEXTURE_MAX. Uses the same WIC path as the
// texture preview, so every DDS variant the preview handles works here too.
bool LoadTexturePixels(const std::wstring &path, kukdh1::PamTexture &out) {
  out.nWidth = out.nHeight = 0;
  out.vPixels.clear();
  if (!app.pWICFactory) return false;

  IWICBitmapDecoder     *decoder   = nullptr;
  IWICBitmapFrameDecode *frame     = nullptr;
  IWICFormatConverter   *converter = nullptr;
  IWICBitmapScaler      *scaler    = nullptr;
  bool ok = false;

  auto cleanup = [&]() {
    if (scaler)    scaler->Release();
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
  };

  if (FAILED(app.pWICFactory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) {
    cleanup();
    return false;
  }
  if (FAILED(decoder->GetFrame(0, &frame))) { cleanup(); return false; }

  UINT srcW = 0, srcH = 0;
  if (FAILED(frame->GetSize(&srcW, &srcH)) || srcW == 0 || srcH == 0) {
    cleanup();
    return false;
  }

  // Fit inside the preview texture budget, never upscale.
  float scale = (std::min)((float)kukdh1::PAM_TEXTURE_MAX / srcW,
                           (float)kukdh1::PAM_TEXTURE_MAX / srcH);
  if (scale > 1.0f) scale = 1.0f;
  UINT dstW = (std::max)(1u, (UINT)(srcW * scale));
  UINT dstH = (std::max)(1u, (UINT)(srcH * scale));

  if (FAILED(app.pWICFactory->CreateFormatConverter(&converter))) { cleanup(); return false; }

  WICPixelFormatGUID srcFmt = {};
  frame->GetPixelFormat(&srcFmt);
  BOOL canConvert = FALSE;
  converter->CanConvert(srcFmt, GUID_WICPixelFormat32bppBGRA, &canConvert);
  if (!canConvert) { cleanup(); return false; }

  if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    cleanup();
    return false;
  }

  if (FAILED(app.pWICFactory->CreateBitmapScaler(&scaler))) { cleanup(); return false; }
  if (FAILED(scaler->Initialize(converter, dstW, dstH, WICBitmapInterpolationModeFant))) {
    cleanup();
    return false;
  }

  out.nWidth  = (int)dstW;
  out.nHeight = (int)dstH;
  out.vPixels.resize((size_t)dstW * dstH);

  WICRect rc = { 0, 0, (INT)dstW, (INT)dstH };
  if (SUCCEEDED(scaler->CopyPixels(&rc, dstW * 4, (UINT)(out.vPixels.size() * 4),
                                   reinterpret_cast<BYTE *>(out.vPixels.data())))) {
    ok = true;
    kukdh1::PrepareTexture(out);
  }
  else {
    out.nWidth = out.nHeight = 0;
    out.vPixels.clear();
  }

  cleanup();
  return ok;
}

// Flattened file-node list, cached because walking 800k+ tree nodes on every
// model selection is far too slow. Holds raw pointers into app.CTree, so it
// MUST be cleared whenever that tree is destroyed (see OpenPazFolder).
static std::vector<kukdh1::Tree *> g_allFileNodes;

void InvalidateFileNodeCache() {
  g_allFileNodes.clear();
  g_allFileNodes.shrink_to_fit();
}

static const std::vector<kukdh1::Tree *> &GetCachedFileNodes() {
  if (g_allFileNodes.empty() && app.CTree)
    app.CTree->GetFileNodeList(g_allFileNodes);
  return g_allFileNodes;
}

// Resolves each submesh's texture name against the loaded tree, extracts it to
// %TEMP% and decodes it. Textures live in a different folder than the model, so
// they are matched on file name only — the same rule PAM co-extraction uses.
void LoadPamTextures(const kukdh1::PamModel &model,
                     std::vector<kukdh1::PamTexture> &out) {
  out.clear();
  out.resize(model.vSubmeshes.size());
  if (!app.CTree) return;

  // Distinct texture names this model needs (usually 1-6, at most ~34).
  std::vector<std::string> wanted;
  for (const auto &sm : model.vSubmeshes) {
    if (sm.sTexture.empty()) continue;
    bool dup = false;
    for (const auto &w : wanted)
      if (_stricmp(w.c_str(), sm.sTexture.c_str()) == 0) { dup = true; break; }
    if (!dup) wanted.push_back(sm.sTexture);
  }
  if (wanted.empty()) return;

  // One pass over the cached node list. The length check rejects almost every
  // node before the string compare, so this stays cheap even at 800k nodes.
  std::vector<kukdh1::Tree *> hits(wanted.size(), nullptr);
  size_t remaining = wanted.size();

  for (auto *n : GetCachedFileNodes()) {
    const std::string &nm = n->GetName();
    for (size_t k = 0; k < wanted.size(); k++) {
      if (hits[k] || nm.size() != wanted[k].size()) continue;
      if (_stricmp(nm.c_str(), wanted[k].c_str()) == 0) {
        hits[k] = n;
        if (--remaining == 0) break;
      }
    }
    if (remaining == 0) break;
  }

  WCHAR tempDir[MAX_PATH];
  GetTempPath(MAX_PATH, tempDir);
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);

  // Decode each distinct texture once, then share it across submeshes.
  std::vector<kukdh1::PamTexture> decoded(wanted.size());
  for (size_t k = 0; k < wanted.size(); k++) {
    if (!hits[k]) continue;   // texture not present in this archive

    std::wstring wName;
    kukdh1::ConvertWidechar(wanted[k], wName);
    std::wstring tempPath = std::wstring(tempDir) + L"paz_preview_tex_" + wName;

    if (ExtractFile(tempPath, hits[k]->GetFileInfo(), cipher)) {
      LoadTexturePixels(tempPath, decoded[k]);
      DeleteFile(tempPath.c_str());
    }
  }

  for (size_t i = 0; i < model.vSubmeshes.size(); i++) {
    const std::string &texName = model.vSubmeshes[i].sTexture;
    if (texName.empty()) continue;
    for (size_t k = 0; k < wanted.size(); k++) {
      if (_stricmp(wanted[k].c_str(), texName.c_str()) == 0) {
        if (decoded[k].IsValid()) out[i] = decoded[k];
        break;
      }
    }
  }
}

// (Re)creates the DIB if the panel size changed, then rasterises the model.
// Returns false if there is nothing to draw.
bool RenderPamPreview() {
  if (!app.CPamModel || app.CPamModel->IsEmpty()) return false;

  RECT rc;
  GetClientRect(app.hPreviewPanel, &rc);
  int panelW = rc.right - rc.left;
  int panelH = rc.bottom - rc.top;
  if (panelW < 2 || panelH < 2) return false;

  // While the camera is being dragged, rasterise at half resolution and let
  // WM_PAINT stretch the result up. Quartering the pixel count keeps large
  // panels interactive; the full-resolution frame is drawn as soon as the drag
  // ends. The bitmap is allocated at the render size rather than the panel
  // size — a smaller render inside a larger bitmap would leave the previous
  // frame visible around it.
  int w = panelW, h = panelH;
  if (app.bPamOrbiting || app.bPamPanning) {
    w = (panelW + 1) / 2;
    h = (panelH + 1) / 2;
  }
  if (w < 2) w = 2;
  if (h < 2) h = 2;

  // Recreate the backing bitmap when the render size changes.
  if (app.hPreviewBitmap == nullptr || w != app.nPamWidth || h != app.nPamHeight) {
    if (app.hPreviewBitmap) {
      DeleteObject(app.hPreviewBitmap);
      app.hPreviewBitmap = nullptr;
      app.pPamPixels     = nullptr;
    }

    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   // top-down, matches the rasteriser
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC hdc = GetDC(nullptr);
    app.hPreviewBitmap = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);

    if (!app.hPreviewBitmap || !bits) {
      app.hPreviewBitmap = nullptr;
      app.pPamPixels     = nullptr;
      return false;
    }
    app.pPamPixels = bits;
    app.nPamWidth  = w;
    app.nPamHeight = h;
  }

  // Reuse the persistent target so the depth buffer is not reallocated per frame.
  app.PamTarget.pPixels = static_cast<uint32_t *>(app.pPamPixels);
  app.PamTarget.nWidth  = app.nPamWidth;
  app.PamTarget.nHeight = app.nPamHeight;

  // COLORREF packs as 0x00BBGGRR; the DIB wants 0xAARRGGBB. Swizzle with masks
  // rather than GetRValue()/GetBValue(), whose BYTE casts trip C4310 here.
  const uint32_t clrBg = (uint32_t)CLR_DARK_INPUT;
  const uint32_t bg = 0xFF000000u
                    | ((clrBg & 0x000000FFu) << 16)   // R
                    |  (clrBg & 0x0000FF00u)          // G
                    | ((clrBg & 0x00FF0000u) >> 16);  // B

  kukdh1::PamShadeMode mode = kukdh1::PAM_SHADE_SOLID;
  if (app.bPamWireframe)        mode = kukdh1::PAM_SHADE_WIREFRAME;
  else if (app.bPamShowTexture) mode = kukdh1::PAM_SHADE_TEXTURED;

  kukdh1::RenderModel(*app.CPamModel, app.PamCam, app.PamTarget, bg, mode,
                      &app.vPamTextures);
  return true;
}

void UpdatePreview(kukdh1::Tree *pTree) {
  // Don't attempt preview while a background thread (search/load) is running —
  // TVN_SELCHANGED fires spuriously during TreeView_DeleteAllItems/insert.
  if (app.bBusy) return;

  // Clear previous preview
  ClearPamPreview();
  if (app.hPreviewBitmap) {
    DeleteObject(app.hPreviewBitmap);
    app.hPreviewBitmap = nullptr;
  }
  InvalidateRect(app.hPreviewPanel, nullptr, TRUE);

  if (!pTree || pTree->GetType() != kukdh1::Tree::TREE_TYPE_FILE) return;

  const kukdh1::FileInfo &info = pTree->GetFileInfo();

  // Check extension is a previewable image type
  size_t dot = info.sFullPath.rfind('.');
  if (dot == std::string::npos) return;
  std::string ext = info.sFullPath.substr(dot);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)::tolower(c); });

  // ── 3D model preview ──────────────────────────────────────────────────────
  if (ext == ".pam") {
    if (info.uiOriginalSize > (uint32_t)MODEL_SIZE_LIMIT) {
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Model too large to preview");
      return;
    }

    size_t mslash = info.sFullPath.rfind('/');
    std::string mname = (mslash != std::string::npos)
      ? info.sFullPath.substr(mslash + 1) : info.sFullPath;
    std::wstring wMName;
    kukdh1::ConvertWidechar(mname, wMName);

    WCHAR mTempDir[MAX_PATH];
    GetTempPath(MAX_PATH, mTempDir);
    std::wstring mTempPath = std::wstring(mTempDir) + L"paz_preview_" + wMName;

    kukdh1::CryptICE mCipher(ICE_KEY, ICE_KEY_LEN);
    if (!ExtractFile(mTempPath, info, mCipher)) {
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Preview: extraction failed");
      return;
    }

    auto model = std::make_unique<kukdh1::PamModel>();
    bool loaded = model->Load(mTempPath);
    DeleteFile(mTempPath.c_str());

    if (!loaded || model->IsEmpty()) {
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Preview: unsupported model format");
      return;
    }

    app.CPamModel = std::move(model);
    app.PamCam.Reset();
    LoadPamTextures(*app.CPamModel, app.vPamTextures);

    size_t texOk = 0;
    for (const auto &t : app.vPamTextures)
      if (t.IsValid()) texOk++;

    if (RenderPamPreview()) {
      // Always spell non-ASCII as a \u escape, never as a literal character:
      // this source is UTF-8 with no BOM, so MSVC decodes raw multi-byte
      // characters using the ANSI codepage and the status bar shows mojibake.
      WCHAR msg[160];
      swprintf_s(msg,
        L"Model v%u \u2014 %zu verts, %zu tris, %zu material(s), %zu/%zu textures",
        app.CPamModel->uiVersion,
        app.CPamModel->vVertices.size(),
        app.CPamModel->vIndices.size() / 3,
        app.CPamModel->vSubmeshes.size(),
        texOk, app.vPamTextures.size());
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)msg);
    }
    InvalidateRect(app.hPreviewPanel, nullptr, TRUE);
    return;
  }

  bool isImage = false;
  for (auto *e : PREVIEW_EXTS) {
    std::string ne; for (const wchar_t *p = e; *p; p++) ne += (char)*p;
    if (ext == ne) { isImage = true; break; }
  }
  if (!isImage) return;

  // Size guard
  if (info.uiOriginalSize > (uint32_t)PREVIEW_SIZE_LIMIT) return;

  // Build temp file path
  size_t slash = info.sFullPath.rfind('/');
  std::string filename = (slash != std::string::npos) ? info.sFullPath.substr(slash + 1) : info.sFullPath;
  std::wstring wFilename;
  kukdh1::ConvertWidechar(filename, wFilename);

  WCHAR tempDir[MAX_PATH];
  GetTempPath(MAX_PATH, tempDir);
  std::wstring tempPath = std::wstring(tempDir) + L"paz_preview_" + wFilename;

  // Extract to temp
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);
  bool ok = ExtractFile(tempPath, info, cipher);

  if (!ok) {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Preview: extraction failed");
    InvalidateRect(app.hPreviewPanel, nullptr, TRUE);
    return;
  }

  RECT rc;
  GetClientRect(app.hPreviewPanel, &rc);
  app.hPreviewBitmap = LoadWICBitmap(tempPath, rc.right - rc.left, rc.bottom - rc.top);
  DeleteFile(tempPath.c_str());

  if (!app.hPreviewBitmap) {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"Preview: unsupported texture format");
  }
  else {
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)L"");
  }
  InvalidateRect(app.hPreviewPanel, nullptr, TRUE);
}
