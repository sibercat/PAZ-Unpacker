#pragma once

#ifndef _CACHE_H_
#define _CACHE_H_

#include <cstdint>
#include <string>
#include "Tree.h"

namespace kukdh1 {

constexpr uint32_t CACHE_VERSION = 2;  // v2: cache stored next to exe; folder path in header

// Returns the cache file path for a given PAZ folder.
std::wstring CachePath(const std::wstring &pazFolder);

// Returns true if a valid, up-to-date cache exists for this folder.
bool IsCacheValid(const std::wstring &pazFolder);

// Loads file records from cache into root. Returns false if cache is missing,
// invalid, or stale (caller should fall back to full PAZ scan).
bool LoadCache(const std::wstring &pazFolder, Tree *root,
               uint32_t &outPazVersion, uint32_t &outPazFileCount);

// Serialises the tree to cache. Called after a successful full PAZ scan.
void WriteCache(const std::wstring &pazFolder, Tree *root,
                uint32_t pazVersion, uint32_t pazFileCount);

// Deletes %TEMP%\paz_preview_*.* files left over from previews.
// Returns the number of files deleted.
int ClearPreviewTempFiles();

} // namespace kukdh1

#endif
