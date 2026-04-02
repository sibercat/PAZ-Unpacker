#include "Helper.h"

namespace kukdh1 {
  HTREEITEM AddTreeItem(HWND hTree, HTREEITEM hParent, HTREEITEM hInsertAfter, LPWSTR pszText, LPARAM lParam) {
    TVINSERTSTRUCT tvis;

    tvis.hParent = hParent;
    tvis.hInsertAfter = hInsertAfter;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvis.item.pszText = pszText;
    tvis.item.lParam = lParam;

    return TreeView_InsertItem(hTree, &tvis);
  }

  int CALLBACK BrowseCallbackProc(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
    switch (uMsg) {
      case BFFM_INITIALIZED:
        if (lpData != NULL) {
          SendMessage(hwnd, BFFM_SETSELECTION, TRUE, (LPARAM)lpData);
        }
        break;
     }

    return 0;
  }
  
  BOOL BrowseFolder(HWND hParent, LPCWSTR szTitle, LPCWSTR szStartPath, std::wstring &outFolder) {
    LPMALLOC pMalloc;
    LPITEMIDLIST pidl;
    BROWSEINFO bi = {};
    WCHAR szBuffer[MAX_PATH] = {};

    bi.hwndOwner = hParent;
    bi.pidlRoot = nullptr;
    bi.pszDisplayName = nullptr;
    bi.lpszTitle = szTitle;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    bi.lpfn = BrowseCallbackProc;
    bi.lParam = (LPARAM)szStartPath;

    pidl = SHBrowseForFolder(&bi);

    if (pidl == nullptr) {
      return FALSE;
    }

    SHGetPathFromIDListEx(pidl, szBuffer, MAX_PATH, GPFIDL_DEFAULT);
    outFolder = szBuffer;

    if (SHGetMalloc(&pMalloc) != NOERROR) {
      return FALSE;
    }

    pMalloc->Free(pidl);
    pMalloc->Release();

    return TRUE;
  }

  void ParsePath(std::string path, std::vector<std::string> &folders) {
    std::stringstream ss(path);

    folders.clear();

    while (!ss.eof()) {
      std::string token;

      std::getline(ss, token, '/');
      folders.push_back(token);
    }
  }

  void ConvertWidechar(const std::string &in, std::wstring &out) {
    int len = MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.length()), nullptr, 0);
    out.resize(len);
    MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.length()), out.data(), len);
  }

  void ConvertCapacity(const LARGE_INTEGER &values, std::wstring &out) {
    LONGLONG qpart = values.QuadPart;
    std::wstringstream wss;
    int unit = 0;

    while (true) {
      qpart >>= 10;

      if (qpart > 0) {
        unit++;
      }
      else
        break;
    }

    switch (unit) {
      case 1:
        wss << std::to_wstring(values.QuadPart / 1024.0) << L" KB (" << std::to_wstring(values.QuadPart) << " bytes)";
        break;
      case 2:
        wss << std::to_wstring(values.QuadPart / 1024.0 / 1024.0) << L" MB (" << std::to_wstring(values.QuadPart) << " bytes)";
        break;
      case 3:
        wss << std::to_wstring(values.QuadPart / 1024.0 / 1024.0 / 1024.0) << L" GB (" << std::to_wstring(values.QuadPart) << " bytes)";
        break;
      default:
        wss << std::to_wstring(values.QuadPart) << " bytes";
        break;
    }

    out = wss.str();
  }
}
