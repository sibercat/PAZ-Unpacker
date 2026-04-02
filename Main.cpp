#include "Main.h"
#include <cassert>
#include <filesystem>

namespace fs = std::filesystem;

LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
DWORD WINAPI FileThread(LPVOID arg);
DWORD WINAPI ExtractThread(LPVOID arg);
DWORD WINAPI AddThread(LPVOID arg);

AppData app;

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdParam, int nCmdShow) {
  HWND hWnd;
  WNDCLASS wndclass;
  MSG msg;
  INITCOMMONCONTROLSEX iccex;
  std::wstring lpszClass = app.CSetting.getString(kukdh1::Setting::ID_CAPTION);

  iccex.dwSize = sizeof(INITCOMMONCONTROLSEX);
  iccex.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_TREEVIEW_CLASSES;

  if (!InitCommonControlsEx(&iccex)) {
    return -1;
  }

  wndclass.cbClsExtra = 0;
  wndclass.cbWndExtra = 0;
  wndclass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wndclass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wndclass.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wndclass.hInstance = hInstance;
  wndclass.lpfnWndProc = WndProc;
  wndclass.lpszClassName = lpszClass.c_str();
  wndclass.lpszMenuName = nullptr;
  wndclass.style = CS_VREDRAW | CS_HREDRAW;

  RegisterClass(&wndclass);

  hWnd = CreateWindow(lpszClass.c_str(), lpszClass.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, hInstance, nullptr);
  ShowWindow(hWnd, nCmdShow);

  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  return (int)msg.wParam;
}

BOOL Cls_OnCreate(HWND hWnd, LPCREATESTRUCT lpCreateStruct) {
  app.hButtonOpen     = CreateWindow(WC_BUTTON, app.CSetting.getString(kukdh1::Setting::ID_OPEN).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_OPEN, lpCreateStruct->hInstance, nullptr);
  app.hButtonExctact  = CreateWindow(WC_BUTTON, app.CSetting.getString(kukdh1::Setting::ID_EXTRACT).c_str(), WS_CHILD | WS_VISIBLE | WS_DISABLED | BS_PUSHBUTTON, 0, 0, 0, 0, hWnd, (HMENU)ID_BUTTON_EXTRACT, lpCreateStruct->hInstance, nullptr);
  app.hTreeFileSystem = CreateWindow(WC_TREEVIEW, nullptr, WS_CHILD | WS_VISIBLE | TVS_DISABLEDRAGDROP | TVS_HASBUTTONS | TVS_TRACKSELECT | TVS_LINESATROOT, 0, 0, 0, 0, hWnd, (HMENU)ID_TREE_FILESYSTEM, lpCreateStruct->hInstance, nullptr);
  app.hStatusBar      = CreateWindow(STATUSCLASSNAME, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hWnd, (HMENU)ID_STATUSBAR, lpCreateStruct->hInstance, nullptr);
  app.hStaticInfo     = CreateWindow(WC_STATIC, nullptr, WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, hWnd, (HMENU)ID_STATIC, lpCreateStruct->hInstance, nullptr);

  SetWindowTheme(app.hTreeFileSystem, L"Explorer", nullptr);

  app.hFont = CreateFont(FONT_SIZE, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_ROMAN, FONT_FACE);

  SendMessage(app.hButtonOpen,     WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hButtonExctact,  WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hTreeFileSystem, WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStatusBar,      WM_SETFONT, (WPARAM)app.hFont, TRUE);
  SendMessage(app.hStaticInfo,     WM_SETFONT, (WPARAM)app.hFont, TRUE);

  int parts[STATUSBAR_SECTION_COUNT] = { STATUSBAR_SECTION1, STATUSBAR_SECTION2, -1 };
  SendMessage(app.hStatusBar, SB_SETPARTS, STATUSBAR_SECTION_COUNT, (LPARAM)parts);
  SendMessage(app.hStatusBar, SB_SETTEXT, 0, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_STATUS_IDLE).c_str());

  RECT rtArea;
  SendMessage(app.hStatusBar, SB_GETRECT, 1, (LPARAM)&rtArea);
  app.hProgressBar = CreateWindow(PROGRESS_CLASS, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, rtArea.left, rtArea.top, rtArea.right - rtArea.left, rtArea.bottom - rtArea.top, app.hStatusBar, nullptr, lpCreateStruct->hInstance, nullptr);

  return TRUE;
}

void Cls_OnDestroy(HWND hWnd) {
  app.CTree.reset();
  app.CMeta.reset();
  app.wsFolderPath.clear();
  DeleteObject(app.hFont);
  PostQuitMessage(0);
}

void Cls_OnSize(HWND hWnd, UINT state, int cx, int cy) {
  int nTreeWidth = (int)(cx * DIVIDE_RATIO + 0.5f);
  int nStaticWidth = cx - nTreeWidth;
  int nStatusbarHeight = GetSystemMetrics(SM_CYMENU) + GetSystemMetrics(SM_CYBORDER) * 2;

  MoveWindow(app.hTreeFileSystem, 0, 0, nTreeWidth, cy - nStatusbarHeight, TRUE);
  MoveWindow(app.hStaticInfo, nTreeWidth, 0, nStaticWidth - BUTTON_WIDTH, cy - nStatusbarHeight, TRUE);
  MoveWindow(app.hStatusBar, 0, 0, 0, 0, TRUE);
  MoveWindow(app.hButtonOpen, cx - BUTTON_WIDTH, 0, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE);
  MoveWindow(app.hButtonExctact, cx - BUTTON_WIDTH, BUTTON_HEIGHT, BUTTON_WIDTH, BUTTON_HEIGHT, TRUE);
}

void Cls_OnGetMinMaxInfo(HWND hWnd, LPMINMAXINFO lpMinMaxInfo) {
  lpMinMaxInfo->ptMinTrackSize.x = WINDOW_MIN_WIDTH;
  lpMinMaxInfo->ptMinTrackSize.y = WINDOW_MIN_HEIGHT;
}

void Cls_OnCommand(HWND hWnd, int id, HWND hwndCtl, UINT codeNotify) {
  switch (id) {
    case ID_BUTTON_OPEN:
      {
        if (app.bBusy) break;

        std::wstring folderPath;
        std::wstring wsLastPath;
        app.CSetting.getData(SETTING_LAST_FOLDER, wsLastPath, L"C:\\");

        if (kukdh1::BrowseFolder(hWnd, app.CSetting.getString(kukdh1::Setting::ID_SELECT_FOLDER_TO_OPEN).c_str(), wsLastPath.c_str(), folderPath)) {
          TreeView_DeleteAllItems(app.hTreeFileSystem);
          SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)L"");
          app.CTree.reset();
          app.CMeta.reset();
          app.wsFolderPath = folderPath;

          app.CSetting.setData(SETTING_LAST_FOLDER, app.wsFolderPath);

          WCHAR titleBuf[MAX_PATH + 64];
          swprintf_s(titleBuf, app.CSetting.getString(kukdh1::Setting::ID_CAPTION_WITH_PATH).c_str(), app.wsFolderPath.c_str());
          SetWindowText(hWnd, titleBuf);

          try {
            app.CMeta = std::make_unique<kukdh1::Meta>((wchar_t *)app.wsFolderPath.c_str());
            app.CTree = std::make_unique<kukdh1::Tree>(kukdh1::Tree::TREE_TYPE_ROOT);

            HANDLE hThread = CreateThread(nullptr, 0, FileThread, nullptr, 0, nullptr);
            CloseHandle(hThread);
          }
          catch (const std::exception &e) {
            std::wstring msg;
            int len = MultiByteToWideChar(CP_ACP, 0, e.what(), -1, nullptr, 0);
            if (len > 0) {
              msg.resize(len - 1);
              MultiByteToWideChar(CP_ACP, 0, e.what(), -1, msg.data(), len);
            }
            if (msg.empty()) {
              msg = app.CSetting.getString(kukdh1::Setting::ID_NO_META_FILE_EXISTS);
            }
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
        tvi.mask = TVIF_PARAM;
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

    WCHAR *handle = nullptr;
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
  switch (iMessage) {
    HANDLE_MSG(hWnd, WM_CREATE, Cls_OnCreate);
    HANDLE_MSG(hWnd, WM_DESTROY, Cls_OnDestroy);
    HANDLE_MSG(hWnd, WM_SIZE, Cls_OnSize);
    HANDLE_MSG(hWnd, WM_GETMINMAXINFO, Cls_OnGetMinMaxInfo);
    HANDLE_MSG(hWnd, WM_COMMAND, Cls_OnCommand);
    HANDLE_MSG(hWnd, WM_DRAWITEM, Cls_OnDrawItem);

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
                    kukdh1::ConvertCapacity((app.CTree->GetCapacity()), capacity);
                    swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_META_FILE_INFO).c_str(), app.CMeta->uiVersion, app.CMeta->uiPAZFileCount, capacity.c_str());
                    SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  }
                  break;
                case kukdh1::Tree::TREE_TYPE_FOLDER:
                  kukdh1::ConvertCapacity((pTree->GetCapacity()), capacity);
                  swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_INTERNAL_FOLDER_INFO).c_str(), pTree->GetName().c_str(), capacity.c_str());
                  SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  break;
                case kukdh1::Tree::TREE_TYPE_FILE:
                  kukdh1::ConvertCapacity((pTree->GetCapacity()), capacity);
                  swprintf_s(pszBuffer, app.CSetting.getString(kukdh1::Setting::ID_INTERNAL_FILE_INFO).c_str(), pTree->GetName().c_str(), capacity.c_str(), pTree->GetFileInfo().wsPazFullPath.c_str(), pTree->GetFileInfo().sFullPath.c_str());
                  SendMessage(app.hStaticInfo, WM_SETTEXT, 0, (LPARAM)pszBuffer);
                  break;
              }
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
  }

  return DefWindowProc(hWnd, iMessage, wParam, lParam);
}

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
      // Skip PAZ files that fail to open; continue with the rest
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
  EnableWindow(app.hButtonOpen, TRUE);
  EnableWindow(app.hButtonExctact, TRUE);
  EnableWindow(app.hTreeFileSystem, TRUE);

  TreeView_Select(app.hTreeFileSystem, app.CTree->GetHandle(), TVGN_CARET);
  TreeView_Expand(app.hTreeFileSystem, app.CTree->GetHandle(), TVE_EXPAND);

  SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
  SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());

  app.bBusy = false;
  return 0;
}

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

  // Decrypt into a malloc'd buffer (cipher API allocates it), wrap with unique_ptr
  uint8_t *raw_decrypted = nullptr;
  uint32_t decrypted_len = length;
  auto free_deleter = [](uint8_t *p) { if (p) free(p); };
  std::unique_ptr<uint8_t, decltype(free_deleter)> owned_decrypt(nullptr, free_deleter);

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

DWORD WINAPI ExtractThread(LPVOID arg) {
  app.bBusy = true;
  kukdh1::Tree *CTree = (kukdh1::Tree *)arg;
  kukdh1::CryptICE cipher(ICE_KEY, ICE_KEY_LEN);
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

    if (!sFolderPath.empty() && sFolderPath.back() != L'\\') {
      sFolderPath.push_back(L'\\');
    }

    SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, uiFiles);
    uint32_t i = 1;

    for (auto &info : vFileList) {
      std::vector<std::string> paths;
      kukdh1::ParsePath(info.sFullPath, paths);

      // Build the output directory path from all components except the filename
      std::wstring dirPath = sFolderPath;
      for (auto path = paths.begin(); path != paths.end() - 1; ++path) {
        std::wstring folder;
        kukdh1::ConvertWidechar(*path, folder);
        dirPath.append(folder).append(L"\\");
      }

      // Create the full directory hierarchy in one call
      std::error_code ec;
      fs::create_directories(dirPath, ec);
      if (ec) {
        MessageBox(nullptr, app.CSetting.getString(kukdh1::Setting::ID_DIRECTORY_CREATE_FAILED).c_str(), app.CSetting.getString(kukdh1::Setting::ID_ERROR).c_str(), MB_OK | MB_ICONERROR);
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

DWORD WINAPI AddThread(LPVOID arg) {
  kukdh1::Tree *pTree = (kukdh1::Tree *)arg;

  if (!pTree->IsGrandchildAdded()) {
    app.bBusy = true;
    EnableWindow(app.hTreeFileSystem, FALSE);
    EnableWindow(app.hButtonOpen, FALSE);
    EnableWindow(app.hButtonExctact, FALSE);

    // Store the string locally to avoid dangling pointer from temporary
    std::wstring statusMsg = app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_NEW_ADDING);
    pTree->AddGrandchildsToTree(app.hTreeFileSystem, (LPVOID)statusMsg.c_str(), [&](LPVOID arg, size_t i, size_t count) {
      WCHAR *pStatusMsg = (WCHAR *)arg;
      WCHAR buffer[128];

      if (i == 0) {
        SendMessage(app.hProgressBar, PBM_SETRANGE32, 0, count);
      }
      swprintf_s(buffer, pStatusMsg, i, count);
      SendMessage(app.hProgressBar, PBM_SETPOS, i, 0);
      SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)buffer);
    });

    SendMessage(app.hProgressBar, PBM_SETPOS, 0, 0);
    SendMessage(app.hStatusBar, SB_SETTEXT, 2, (LPARAM)app.CSetting.getString(kukdh1::Setting::ID_PROGRESS_READY).c_str());

    EnableWindow(app.hButtonExctact, TRUE);
    EnableWindow(app.hButtonOpen, TRUE);
    EnableWindow(app.hTreeFileSystem, TRUE);
    app.bBusy = false;
  }

  return 0;
}
