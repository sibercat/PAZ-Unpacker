#include "Main.h"
#include <cassert>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// ── Forward declarations ──────────────────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
DWORD   WINAPI   FileThread(LPVOID arg);
DWORD   WINAPI   ExtractThread(LPVOID arg);
DWORD   WINAPI   AddThread(LPVOID arg);
void    ApplySearchFilter();
void    RestoreNormalTree();
void    UpdatePreview(kukdh1::Tree *pTree);
HBITMAP LoadWICBitmap(const std::wstring &path, int maxW, int maxH);

// ── Global ────────────────────────────────────────────────────────────────────
AppData app;

// ─────────────────────────────────────────────────────────────────────────────
// WinMain
// ─────────────────────────────────────────────────────────────────────────────
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdParam, int nCmdShow) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  HWND  hWnd;
  WNDCLASS wndclass;
  MSG   msg;
  INITCOMMONCONTROLSEX iccex;
  std::wstring lpszClass = app.CSetting.getString(kukdh1::Setting::ID_CAPTION);
  // Append version to window class / caption base
  lpszClass += L" v" APP_VERSION;

  iccex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  iccex.dwICC  = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES;
  if (!InitCommonControlsEx(&iccex)) {
    CoUninitialize();
    return -1;
  }

  // Register preview panel window class
  WNDCLASS previewClass = {};
  previewClass.style         = CS_HREDRAW | CS_VREDRAW;
  previewClass.lpfnWndProc   = PreviewWndProc;
  previewClass.hInstance     = hInstance;
  previewClass.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
  previewClass.lpszClassName = L"PAZPreview";
  RegisterClass(&previewClass);

  // Register main window class
  wndclass.cbClsExtra    = 0;
  wndclass.cbWndExtra    = 0;
  wndclass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
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

  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  CoUninitialize();
  return (int)msg.wParam;
}

// ─────────────────────────────────────────────────────────────────────────────
// Preview panel WndProc
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK PreviewWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
  switch (iMessage) {
    case WM_ERASEBKGND:
      return 1;   // handled in WM_PAINT — prevents flicker

    case WM_PAINT:
      {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        if (app.hPreviewBitmap) {
          BITMAP bm;
          GetObject(app.hPreviewBitmap, sizeof(bm), &bm);

          HDC hdcMem = CreateCompatibleDC(hdc);
          HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, app.hPreviewBitmap);

          // Dark grey background, image centred
          FillRect(hdc, &rc, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
          int x = (rc.right  - bm.bmWidth)  / 2;
          int y = (rc.bottom - bm.bmHeight) / 2;
          BitBlt(hdc, x, y, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, SRCCOPY);

          SelectObject(hdcMem, hOld);
          DeleteDC(hdcMem);
        }
        else {
          FillRect(hdc, &rc, GetSysColorBrush(COLOR_BTNFACE));
          SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
          SetBkMode(hdc, TRANSPARENT);
          if (app.hFont) SelectObject(hdc, app.hFont);
          DrawText(hdc, L"No preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  // ── Existing controls ──────────────────────────────────────────────────────
  app.hButtonOpen     = CreateWindow(WC_BUTTON, app.CSetting.getString(kukdh1::Setting::ID_OPEN).c_str(),
    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
    0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_OPEN, lpCreateStruct->hInstance, nullptr);
  app.hButtonExctact  = CreateWindow(WC_BUTTON, app.CSetting.getString(kukdh1::Setting::ID_EXTRACT).c_str(),
    WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON,
    0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_EXTRACT, lpCreateStruct->hInstance, nullptr);
  app.hTreeFileSystem = CreateWindow(WC_TREEVIEW, nullptr,
    WS_CHILD | WS_VISIBLE | TVS_DISABLEDRAGDROP | TVS_HASBUTTONS | TVS_TRACKSELECT | TVS_LINESATROOT,
    0, 0, 0, 0, hWnd, (HMENU)ID_TREE_FILESYSTEM, lpCreateStruct->hInstance, nullptr);
  app.hStatusBar      = CreateWindow(STATUSCLASSNAME, nullptr,
    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
    0, 0, 0, 0, hWnd, (HMENU)ID_STATUSBAR, lpCreateStruct->hInstance, nullptr);
  app.hStaticInfo     = CreateWindow(WC_STATIC, nullptr,
    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
    0, 0, 0, 0, hWnd, (HMENU)ID_STATIC, lpCreateStruct->hInstance, nullptr);

  SetWindowTheme(app.hTreeFileSystem, L"Explorer", nullptr);

  // ── Search edit ────────────────────────────────────────────────────────────
  app.hSearchEdit = CreateWindow(WC_EDIT, nullptr,
    WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
    0, 0, 0, 0, hWnd, (HMENU)ID_EDIT_SEARCH, lpCreateStruct->hInstance, nullptr);
  // Placeholder-style hint text (grey, cleared on focus via EN_SETFOCUS if desired)
  SetWindowText(app.hSearchEdit, L"Search...");

  // ── Extension filter checkboxes ────────────────────────────────────────────
  for (int i = 0; i < EXT_FILTER_COUNT; i++) {
    app.hCheckboxes[i] = CreateWindow(WC_BUTTON, EXT_NAMES[i],
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      0, 0, 0, 0, hWnd, (HMENU)(ID_CHECK_BASE + i), lpCreateStruct->hInstance, nullptr);
  }

  // ── Texture preview panel ─────────────────────────────────────────────────
  app.hPreviewPanel = CreateWindow(L"PAZPreview", nullptr,
    WS_CHILD | WS_VISIBLE,
    0, 0, 0, 0, hWnd, (HMENU)ID_PREVIEW, lpCreateStruct->hInstance, nullptr);

  // ── Font ──────────────────────────────────────────────────────────────────
  app.hFont = CreateFont(FONT_SIZE, 0, 0, 0, FW_NORMAL, 0, 0, 0,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_ROMAN, FONT_FACE);

  SendMessage(app.hButtonOpen,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hButtonExctact,  WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hTreeFileSystem, WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStatusBar,      WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStaticInfo,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hSearchEdit,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  for (int i = 0; i < EXT_FILTER_COUNT; i++)
    SendMessage(app.hCheckboxes[i], WM_SETFONT, (WPARAM)app.hFont, TRUE);

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

  return TRUE;
}

void Cls_OnDestroy(HWND hWnd) {
  if (app.hPreviewBitmap) { DeleteObject(app.hPreviewBitmap); app.hPreviewBitmap = nullptr; }
  if (app.pWICFactory)    { app.pWICFactory->Release();       app.pWICFactory    = nullptr; }

  app.CTree.reset();
  app.CMeta.reset();
  app.wsFolderPath.clear();
  DeleteObject(app.hFont);
  PostQuitMessage(0);
}

void Cls_OnSize(HWND hWnd, UINT state, int cx, int cy) {
  int nTreeWidth      = (int)(cx * DIVIDE_RATIO + 0.5f);
  int nRightWidth     = cx - nTreeWidth;
  int nStatusH        = GetSystemMetrics(SM_CYMENU) + GetSystemMetrics(SM_CYBORDER) * 2;
  int nContentH       = cy - nStatusH - HEADER_HEIGHT;
  if (nContentH < 1) nContentH = 1;
  int nInfoH          = (int)(nContentH * INFO_RATIO);
  int nPreviewH       = nContentH - nInfoH;
  int nCheckW         = nTreeWidth / EXT_FILTER_COUNT;

  // Row 0: search box + buttons
  MoveWindow(app.hSearchEdit,    0, 0, cx - BUTTON_WIDTH, SEARCH_HEIGHT, TRUE);
  MoveWindow(app.hButtonOpen,    cx - BUTTON_WIDTH, 0,            BUTTON_WIDTH, BUTTON_HEIGHT, TRUE);
  MoveWindow(app.hButtonExctact, cx - BUTTON_WIDTH, BUTTON_HEIGHT, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE);

  // Row 1: extension filter checkboxes (span tree width)
  for (int i = 0; i < EXT_FILTER_COUNT; i++) {
    MoveWindow(app.hCheckboxes[i], i * nCheckW, SEARCH_HEIGHT, nCheckW, FILTER_HEIGHT, TRUE);
  }

  // Main content area
  MoveWindow(app.hTreeFileSystem, 0,          HEADER_HEIGHT,              nTreeWidth,  nContentH,  TRUE);
  MoveWindow(app.hStaticInfo,     nTreeWidth, HEADER_HEIGHT,              nRightWidth, nInfoH,      TRUE);
  MoveWindow(app.hPreviewPanel,   nTreeWidth, HEADER_HEIGHT + nInfoH,     nRightWidth, nPreviewH,   TRUE);
  MoveWindow(app.hStatusBar, 0, 0, 0, 0, TRUE);
}

void Cls_OnGetMinMaxInfo(HWND hWnd, LPMINMAXINFO lpMinMaxInfo) {
  lpMinMaxInfo->ptMinTrackSize.x = WINDOW_MIN_WIDTH;
  lpMinMaxInfo->ptMinTrackSize.y = WINDOW_MIN_HEIGHT;
}

void Cls_OnCommand(HWND hWnd, int id, HWND hwndCtl, UINT codeNotify) {
  // Extension filter checkboxes
  if (id >= ID_CHECK_BASE && id < ID_CHECK_BASE + EXT_FILTER_COUNT) {
    if (!app.bBusy) ApplySearchFilter();
    return;
  }

  switch (id) {
    case ID_EDIT_SEARCH:
      if (codeNotify == EN_CHANGE && !app.bBusy) ApplySearchFilter();
      break;

    case ID_BUTTON_OPEN:
      {
        if (app.bBusy) break;

        std::wstring folderPath;
        std::wstring wsLastPath;
        app.CSetting.getData(SETTING_LAST_FOLDER, wsLastPath, L"C:\\");

        if (kukdh1::BrowseFolder(hWnd, app.CSetting.getString(kukdh1::Setting::ID_SELECT_FOLDER_TO_OPEN).c_str(), wsLastPath.c_str(), folderPath)) {
          TreeView_DeleteAllItems(app.hTreeFileSystem);
          SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)L"");

          // Clear preview
          if (app.hPreviewBitmap) { DeleteObject(app.hPreviewBitmap); app.hPreviewBitmap = nullptr; }
          InvalidateRect(app.hPreviewPanel, nullptr, TRUE);

          // Reset search state
          app.bSearchMode = false;
          SetWindowText(app.hSearchEdit, L"Search...");
          for (int i = 0; i < EXT_FILTER_COUNT; i++)
            SendMessage(app.hCheckboxes[i], BM_SETCHECK, BST_UNCHECKED, 0);

          app.CTree.reset();
          app.CMeta.reset();
          app.wsFolderPath = folderPath;
          app.CSetting.setData(SETTING_LAST_FOLDER, app.wsFolderPath);

          WCHAR titleBuf[MAX_PATH + 64];
          swprintf_s(titleBuf, app.CSetting.getString(kukdh1::Setting::ID_CAPTION_WITH_PATH).c_str(), app.wsFolderPath.c_str());
          SetWindowText(hWnd, titleBuf);

          try {
            app.CMeta  = std::make_unique<kukdh1::Meta>((wchar_t *)app.wsFolderPath.c_str());
            app.CTree  = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_ROOT);
            HANDLE hThread = CreateThread(nullptr, 0, FileThread, nullptr, 0, nullptr);
            CloseHandle(hThread);
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
      }
      break;

    case ID_BUTTON_EXTRACT:
      {
        if (app.bBusy) break;

        TVITEM tvi = {};
        HTREEITEM hTree = TreeView_GetSelection(app.hTreeFileSystem);
        tvi.hItem = hTree;
        tvi.mask  = TVIF_PARAM;
        TreeView_GetItem(app.hTreeFileSystem, &tvi);

        if (tvi.lParam) {
          HANDLE hThread = CreateThread(nullptr, 0, ExtractThread, (LPVOID)tvi.lParam, 0, nullptr);
          CloseHandle(hThread);
        }
      }
      break;
  }
}

void Cls_OnDrawItem(HWND hWnd, const DRAWITEMSTRUCT *lpDrawItem) {
  if (lpDrawItem->CtlID == ID_STATIC) {
    FillRect(lpDrawItem->hDC, &lpDrawItem->rcItem, GetSysColorBrush(COLOR_BTNFACE));

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
      SetRect(&rtRect, 0, size.cy * i, lpDrawItem->rcItem.right, size.cy * (i + 1));
      DrawText(lpDrawItem->hDC, pszLine, (int)wcslen(pszLine), &rtRect, DT_WORD_ELLIPSIS | DT_NOCLIP);
      pszLine = wcstok_s(nullptr, L"\r\n", &handle);
    }
  }
}

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

    case WM_NOTIFY:
      {
        LPNMHDR hdr = (LPNMHDR)lParam;

        if (hdr->idFrom == ID_TREE_FILESYSTEM) {
          LPNMTREEVIEW ntv = (LPNMTREEVIEW)lParam;
          kukdh1::Tree *pTree;

          if (hdr->code == TVN_SELCHANGED) {
            WCHAR pszBuffer[1024];
            std::wstring capacity;

            pTree = (kukdh1::Tree *)ntv->itemNew.lParam;
            if (pTree != nullptr) {
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
            }
          }
          else if (hdr->code == TVN_ITEMEXPANDING) {
            // Don't lazy-load in search mode — items are leaf nodes
            if (!app.bSearchMode && ntv->action == TVE_EXPAND) {
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
  }

  return DefWindowProc(hWnd, iMessage, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// File loading thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI FileThread(LPVOID arg) {
  app.bBusy = true;
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);
  std::vector<std::string> paths;
  WCHAR buffer[128];

  EnableWindow(app.hButtonOpen, FALSE);
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

  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_ADDING).c_str());
  app.CTree->AddToTree(app.hTreeFileSystem);
  app.CTree->AddChildsToTree(app.hTreeFileSystem);
  SendMessage(app.hProgressBar, PBM_SETPOS, 3, 0);

  SendMessage(app.hStatusBar, SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());
  EnableWindow(app.hButtonOpen,   TRUE);
  EnableWindow(app.hButtonExctact,TRUE);
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
bool CheckEncrypt(const std::string &filename, uint32_t size) {
  assert(!filename.empty());
  if (filename.length() < 5) return false;
  return filename.compare(filename.length() - 5, 5, ".dbss") == 0;
}

bool ExtractFile(const std::wstring &path, const kukdh1::FileInfo &file, kukdh1::Crypt &cipher) {
  assert(!path.empty());
  assert(file.uiCompressedSize > 0);
  assert(file.uiOriginalSize > 0);

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
    cipher.decrypt(encrypted.data(), length, &raw_decrypted, &decrypted_len);
    owned_decrypt.reset(raw_decrypted);
  }
  else {
    raw_decrypted = encrypted.data();
    decrypted_len = length;
  }

  if (bCompressed || raw_decrypted[0] == 0x6E) {
    std::vector<uint8_t> decompressed(file.uiOriginalSize);
    kukdh1::decompress(raw_decrypted, decompressed.data());
    owned_decrypt.reset();
    savefile.write(reinterpret_cast<const char *>(decompressed.data()), file.uiOriginalSize);
  }
  else {
    savefile.write(reinterpret_cast<const char *>(raw_decrypted), file.uiOriginalSize);
  }

  savefile.close();
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Extraction thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI ExtractThread(LPVOID arg) {
  app.bBusy = true;
  kukdh1::Tree     *CTree = (kukdh1::Tree *)arg;
  kukdh1::CryptICE  cipher(ICE_KEY, ICE_KEY_LEN);
  WCHAR buffer[128];
  std::wstring sFolderPath;
  std::wstring sLastExtractPath;

  app.CSetting.getData(SETTING_LAST_EXTRACT, sLastExtractPath, app.wsFolderPath);
  EnableWindow(app.hButtonExctact, FALSE);

  if (kukdh1::BrowseFolder(nullptr, app.CSetting.getString(kukdh1::Setting::ID_SELECT_FOLDER_TO_SAVE).c_str(), sLastExtractPath.c_str(), sFolderPath)) {
    std::vector<kukdh1::FileInfo> vFileList;
    CTree->GetFileList(vFileList);
    uint32_t uiFiles = (uint32_t)vFileList.size();

    app.CSetting.setData(SETTING_LAST_EXTRACT, sFolderPath);
    if (!sFolderPath.empty() && sFolderPath.back() != L'\\') sFolderPath.push_back(L'\\');

    SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, uiFiles);
    uint32_t i = 1;

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
    }
  }

  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());
  EnableWindow(app.hButtonExctact, TRUE);
  app.bBusy = false;
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lazy-load tree expansion thread
// ─────────────────────────────────────────────────────────────────────────────
DWORD WINAPI AddThread(LPVOID arg) {
  kukdh1::Tree *pTree = (kukdh1::Tree *)arg;

  if (!pTree->IsGrandchildAdded()) {
    app.bBusy = true;
    EnableWindow(app.hTreeFileSystem, FALSE);
    EnableWindow(app.hButtonOpen,    FALSE);
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
    EnableWindow(app.hButtonOpen,    TRUE);
    EnableWindow(app.hTreeFileSystem, TRUE);
    app.bBusy = false;
  }

  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Search / filter
// ─────────────────────────────────────────────────────────────────────────────
void RestoreNormalTree() {
  app.bSearchMode = false;
  TreeView_DeleteAllItems(app.hTreeFileSystem);
  app.CTree->ResetAdded();
  app.CTree->AddToTree(app.hTreeFileSystem);
  app.CTree->AddChildsToTree(app.hTreeFileSystem);
  TreeView_Select(app.hTreeFileSystem, app.CTree->GetHandle(), TVGN_CARET);
  TreeView_Expand(app.hTreeFileSystem, app.CTree->GetHandle(), TVE_EXPAND);
  SendMessage(app.hStatusBar, SB_SETTEXT, 2,
    (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());
}

void ApplySearchFilter() {
  if (!app.CTree) return;

  // ── Gather search text (skip placeholder) ─────────────────────────────────
  int len = GetWindowTextLength(app.hSearchEdit);
  std::wstring searchText(len + 1, L'\0');
  GetWindowText(app.hSearchEdit, searchText.data(), len + 1);
  searchText.resize(len);
  if (searchText == L"Search...") searchText.clear();
  std::transform(searchText.begin(), searchText.end(), searchText.begin(), ::towlower);

  // Narrow version for fast comparison against narrow sFullPath
  std::string narrowSearch;
  for (wchar_t c : searchText) narrowSearch += (char)c;

  // ── Gather checked extensions ─────────────────────────────────────────────
  std::vector<std::string> checkedExts;
  for (int i = 0; i < EXT_FILTER_COUNT; i++) {
    if (SendMessage(app.hCheckboxes[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
      std::string ext;
      for (const wchar_t *p = EXT_NAMES[i]; *p; p++) ext += (char)*p;
      checkedExts.push_back(ext);
    }
  }

  bool isFiltering = !searchText.empty() || !checkedExts.empty();

  if (!isFiltering) {
    if (app.bSearchMode) RestoreNormalTree();
    return;
  }

  // ── Collect all file nodes ────────────────────────────────────────────────
  std::vector<kukdh1::Tree*> allFiles;
  app.CTree->GetFileNodeList(allFiles);

  // ── Filter ────────────────────────────────────────────────────────────────
  std::vector<kukdh1::Tree*> matches;
  matches.reserve(512);

  for (auto *node : allFiles) {
    const std::string &fullPath = node->GetFileInfo().sFullPath;

    // Extension filter
    if (!checkedExts.empty()) {
      size_t dot = fullPath.rfind('.');
      std::string ext = (dot != std::string::npos) ? fullPath.substr(dot) : "";
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (std::find(checkedExts.begin(), checkedExts.end(), ext) == checkedExts.end()) continue;
    }

    // Text search (narrow, case-insensitive)
    if (!narrowSearch.empty()) {
      std::string lowerPath = fullPath;
      std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
      if (lowerPath.find(narrowSearch) == std::string::npos) continue;
    }

    matches.push_back(node);
  }

  // ── Rebuild TreeView as flat list ─────────────────────────────────────────
  app.bSearchMode = true;
  SendMessage(app.hTreeFileSystem, WM_SETREDRAW, FALSE, 0);
  TreeView_DeleteAllItems(app.hTreeFileSystem);

  for (auto *node : matches) {
    std::wstring widePath;
    kukdh1::ConvertWidechar(node->GetFileInfo().sFullPath, widePath);
    wchar_t label[512];
    wcsncpy_s(label, widePath.c_str(), _countof(label) - 1);
    kukdh1::AddTreeItem(app.hTreeFileSystem, nullptr, TVI_LAST, label, (LPARAM)node);
  }

  SendMessage(app.hTreeFileSystem, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(app.hTreeFileSystem, nullptr, TRUE);

  WCHAR buf[64];
  swprintf_s(buf, L"%zu result(s)", matches.size());
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// Texture preview
// ─────────────────────────────────────────────────────────────────────────────
HBITMAP LoadWICBitmap(const std::wstring &path, int maxW, int maxH) {
  if (!app.pWICFactory || maxW < 1 || maxH < 1) return nullptr;

  IWICBitmapDecoder    *decoder   = nullptr;
  IWICBitmapFrameDecode *frame    = nullptr;
  IWICFormatConverter  *converter = nullptr;
  IWICBitmapScaler     *scaler    = nullptr;
  HBITMAP               hResult  = nullptr;

  auto cleanup = [&]() {
    if (scaler)    scaler->Release();
    if (converter) converter->Release();
    if (frame)     frame->Release();
    if (decoder)   decoder->Release();
  };

  HRESULT hr = app.pWICFactory->CreateDecoderFromFilename(
    path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) { cleanup(); return nullptr; }

  hr = app.pWICFactory->CreateFormatConverter(&converter);
  if (FAILED(hr)) { cleanup(); return nullptr; }

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

void UpdatePreview(kukdh1::Tree *pTree) {
  // Clear previous preview
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
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

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

  if (ok) {
    RECT rc;
    GetClientRect(app.hPreviewPanel, &rc);
    app.hPreviewBitmap = LoadWICBitmap(tempPath, rc.right - rc.left, rc.bottom - rc.top);
  }

  DeleteFile(tempPath.c_str());
  InvalidateRect(app.hPreviewPanel, nullptr, TRUE);
}
