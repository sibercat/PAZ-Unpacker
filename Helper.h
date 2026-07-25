#pragma once

#ifndef _HELPER_H_
#define _HELPER_H_

#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <vector>
#include <string>
#include <sstream>

namespace kukdh1 {
  HTREEITEM AddTreeItem(HWND hTree, HTREEITEM hParent, HTREEITEM hInsertAfter, LPWSTR pszText, LPARAM lParam);
  BOOL BrowseFolder(HWND hParent, LPCWSTR szTitle, LPCWSTR szStartPath, std::wstring &outFolder);

  // Save-as dialog. The chosen file type doubles as the format selector:
  // outFilterIndex is 1-based into pFilters. szDefaultExt is appended when the
  // user types a name without one.
  BOOL SaveFileDialog(HWND hParent, LPCWSTR szTitle, LPCWSTR szDefaultName,
                      const COMDLG_FILTERSPEC *pFilters, UINT uFilterCount,
                      LPCWSTR szStartPath, UINT &outFilterIndex,
                      std::wstring &outPath);
  void ParsePath(std::string path, std::vector<std::string> &folders);
  void ConvertWidechar(const std::string &in, std::wstring &out);
  void ConvertCapacity(const LARGE_INTEGER &value, std::wstring &out);
}

#endif
