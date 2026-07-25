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

  BOOL BrowseFolder(HWND hParent, LPCWSTR szTitle, LPCWSTR szStartPath, std::wstring &outFolder) {
    IFileOpenDialog *pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
      return FALSE;

    DWORD dwOptions = 0;
    pfd->GetOptions(&dwOptions);
    pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (szTitle) pfd->SetTitle(szTitle);

    if (szStartPath && szStartPath[0]) {
      IShellItem *psi = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(szStartPath, nullptr, IID_PPV_ARGS(&psi)))) {
        pfd->SetFolder(psi);
        psi->Release();
      }
    }

    HRESULT hr = pfd->Show(hParent);
    if (FAILED(hr)) { pfd->Release(); return FALSE; }

    IShellItem *psiResult = nullptr;
    hr = pfd->GetResult(&psiResult);
    pfd->Release();
    if (FAILED(hr)) return FALSE;

    PWSTR pszPath = nullptr;
    hr = psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    psiResult->Release();
    if (SUCCEEDED(hr)) {
      outFolder = pszPath;
      CoTaskMemFree(pszPath);
    }
    return SUCCEEDED(hr) ? TRUE : FALSE;
  }

  BOOL SaveFileDialog(HWND hParent, LPCWSTR szTitle, LPCWSTR szDefaultName,
                      const COMDLG_FILTERSPEC *pFilters, UINT uFilterCount,
                      LPCWSTR szStartPath, UINT &outFilterIndex,
                      std::wstring &outPath) {
    IFileSaveDialog *pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pfd))))
      return FALSE;

    DWORD dwOptions = 0;
    pfd->GetOptions(&dwOptions);
    pfd->SetOptions(dwOptions | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (szTitle)       pfd->SetTitle(szTitle);
    if (szDefaultName) pfd->SetFileName(szDefaultName);
    if (pFilters && uFilterCount) {
      pfd->SetFileTypes(uFilterCount, pFilters);
      pfd->SetFileTypeIndex(outFilterIndex ? outFilterIndex : 1);
    }

    if (szStartPath && szStartPath[0]) {
      IShellItem *psi = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(szStartPath, nullptr, IID_PPV_ARGS(&psi)))) {
        pfd->SetFolder(psi);
        psi->Release();
      }
    }

    HRESULT hr = pfd->Show(hParent);
    if (FAILED(hr)) { pfd->Release(); return FALSE; }

    UINT idx = 1;
    pfd->GetFileTypeIndex(&idx);

    IShellItem *psiResult = nullptr;
    hr = pfd->GetResult(&psiResult);
    pfd->Release();
    if (FAILED(hr)) return FALSE;

    PWSTR pszPath = nullptr;
    hr = psiResult->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    psiResult->Release();
    if (SUCCEEDED(hr)) {
      outPath = pszPath;
      CoTaskMemFree(pszPath);
      outFilterIndex = idx;
    }
    return SUCCEEDED(hr) ? TRUE : FALSE;
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
