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
  void ParsePath(std::string path, std::vector<std::string> &folders);
  void ConvertWidechar(const std::string &in, std::wstring &out);
  void ConvertCapacity(const LARGE_INTEGER &value, std::wstring &out);
}

#endif
