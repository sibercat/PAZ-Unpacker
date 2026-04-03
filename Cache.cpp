#include "Cache.h"
#include "Helper.h"
#include "PazFile.h"

#include <filesystem>
#include <fstream>
#include <vector>
#include <Windows.h>

namespace kukdh1 {

// ── Helpers ───────────────────────────────────────────────────────────────────

// Cache lives next to the exe — never inside the game folder (anti-cheat risk).
static std::wstring ExeDir() {
  WCHAR buf[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path().wstring();
}

std::wstring CachePath(const std::wstring & /*pazFolder*/) {
  return ExeDir() + L"\\paz_cache.bin";
}

static std::wstring MetaPath(const std::wstring &pazFolder) {
  return pazFolder + L"\\pad00000.meta";
}

// Returns FILETIME as uint64 and file size for pad00000.meta.
static bool GetMetaStat(const std::wstring &pazFolder, uint64_t &mtime, uint64_t &size) {
  WIN32_FILE_ATTRIBUTE_DATA fa = {};
  if (!GetFileAttributesExW(MetaPath(pazFolder).c_str(), GetFileExInfoStandard, &fa))
    return false;

  ULARGE_INTEGER t;
  t.LowPart  = fa.ftLastWriteTime.dwLowDateTime;
  t.HighPart = fa.ftLastWriteTime.dwHighDateTime;
  ULARGE_INTEGER s;
  s.LowPart  = fa.nFileSizeLow;
  s.HighPart = fa.nFileSizeHigh;

  mtime = t.QuadPart;
  size  = s.QuadPart;
  return true;
}

// Extract numeric PAZ index from a full wsPazFullPath (e.g. "...\\PAD03471.PAZ" → 3471).
static uint32_t PazIdxFromPath(const std::wstring &fullPath) {
  size_t slash = fullPath.rfind(L'\\');
  std::wstring fname = (slash != std::wstring::npos) ? fullPath.substr(slash + 1) : fullPath;
  // fname = "PAD%05u.PAZ"
  if (fname.size() < 12) return 0;
  return static_cast<uint32_t>(std::stoul(fname.substr(3, 5)));
}

// ── Binary format ─────────────────────────────────────────────────────────────
//
//  Header (variable):
//    char[4]   magic          "PAZC"
//    uint32_t  version        CACHE_VERSION
//    uint64_t  meta_mtime     pad00000.meta FILETIME as uint64
//    uint64_t  meta_size      pad00000.meta byte size
//    uint32_t  paz_version    Meta::uiVersion
//    uint32_t  paz_file_count Meta::uiPAZFileCount
//    uint32_t  record_count   total file nodes
//    uint16_t  folder_len     wchar_t count of paz folder path
//    wchar_t[] folder_path    paz folder (UTF-16LE, no null) — invalidates if folder changes
//
//  Per record (variable):
//    uint32_t  paz_idx        PAZ file index → reconstructs wsPazFullPath
//    uint32_t  offset         FileInfo::uiOffset
//    uint32_t  comp_size      FileInfo::uiCompressedSize
//    uint32_t  orig_size      FileInfo::uiOriginalSize
//    uint16_t  path_len       byte length of sFullPath (no null terminator)
//    char[]    path           sFullPath (ASCII/UTF-8)

static const char CACHE_MAGIC[4] = {'P','A','Z','C'};

// ── Public API ────────────────────────────────────────────────────────────────

// Reads and validates the fixed + folder parts of the header.
// Leaves f positioned at the start of per-record data on success.
static bool ReadAndValidateHeader(std::ifstream &f, const std::wstring &pazFolder,
                                  uint64_t curMtime, uint64_t curSize,
                                  uint32_t &outPazVersion, uint32_t &outPazFileCount,
                                  uint32_t &outRecordCount) {
  char     magic[4]      = {};
  uint32_t version       = 0;
  uint64_t cachedMtime   = 0;
  uint64_t cachedSize    = 0;
  uint32_t pazVersion    = 0;
  uint32_t pazFileCount  = 0;
  uint32_t recordCount   = 0;
  uint16_t folderLen     = 0;

  f.read(magic,                                  4);
  f.read(reinterpret_cast<char*>(&version),      4);
  f.read(reinterpret_cast<char*>(&cachedMtime),  8);
  f.read(reinterpret_cast<char*>(&cachedSize),   8);
  f.read(reinterpret_cast<char*>(&pazVersion),   4);
  f.read(reinterpret_cast<char*>(&pazFileCount), 4);
  f.read(reinterpret_cast<char*>(&recordCount),  4);
  f.read(reinterpret_cast<char*>(&folderLen),    2);

  if (!f)                                               return false;
  if (memcmp(magic, CACHE_MAGIC, 4) != 0)               return false;
  if (version     != CACHE_VERSION)                     return false;
  if (cachedMtime != curMtime || cachedSize != curSize) return false;
  if (folderLen   == 0)                                 return false;

  std::wstring cachedFolder(folderLen, L'\0');
  f.read(reinterpret_cast<char*>(cachedFolder.data()), folderLen * sizeof(wchar_t));
  if (!f) return false;

  // Normalise both paths to lowercase for comparison
  std::wstring a = pazFolder,  b = cachedFolder;
  std::transform(a.begin(), a.end(), a.begin(), ::towlower);
  std::transform(b.begin(), b.end(), b.begin(), ::towlower);
  if (a != b) return false;

  outPazVersion   = pazVersion;
  outPazFileCount = pazFileCount;
  outRecordCount  = recordCount;
  return true;
}

bool IsCacheValid(const std::wstring &pazFolder) {
  uint64_t curMtime = 0, curSize = 0;
  if (!GetMetaStat(pazFolder, curMtime, curSize)) return false;

  std::ifstream f(CachePath(pazFolder), std::ios::binary);
  if (!f) return false;

  uint32_t dummy1, dummy2, dummy3;
  return ReadAndValidateHeader(f, pazFolder, curMtime, curSize, dummy1, dummy2, dummy3);
}

bool LoadCache(const std::wstring &pazFolder, Tree *root,
               uint32_t &outPazVersion, uint32_t &outPazFileCount) {
  uint64_t curMtime = 0, curSize = 0;
  if (!GetMetaStat(pazFolder, curMtime, curSize)) return false;

  std::ifstream f(CachePath(pazFolder), std::ios::binary);
  if (!f) return false;

  // ── Header ────────────────────────────────────────────────────────────────
  uint32_t recordCount = 0;
  if (!ReadAndValidateHeader(f, pazFolder, curMtime, curSize,
                             outPazVersion, outPazFileCount, recordCount))
    return false;

  // ── Records ───────────────────────────────────────────────────────────────
  std::vector<std::string> pathParts;
  pathParts.reserve(8);

  for (uint32_t i = 0; i < recordCount; i++) {
    uint32_t pazIdx  = 0;
    uint32_t offset  = 0;
    uint32_t compSz  = 0;
    uint32_t origSz  = 0;
    uint16_t pathLen = 0;

    f.read(reinterpret_cast<char*>(&pazIdx),  4);
    f.read(reinterpret_cast<char*>(&offset),  4);
    f.read(reinterpret_cast<char*>(&compSz),  4);
    f.read(reinterpret_cast<char*>(&origSz),  4);
    f.read(reinterpret_cast<char*>(&pathLen), 2);
    if (!f || pathLen == 0) return false;

    std::string sPath(pathLen, '\0');
    f.read(sPath.data(), pathLen);
    if (!f) return false;

    // Reconstruct wsPazFullPath from folder + PAZ index
    wchar_t pazName[32];
    swprintf_s(pazName, L"PAD%05u.PAZ", pazIdx);
    std::wstring wsPazPath = pazFolder + L"\\" + pazName;

    // Build FileInfo
    FileInfo fi;
    fi.uiOffset         = offset;
    fi.uiCompressedSize = compSz;
    fi.uiOriginalSize   = origSz;
    fi.sFullPath        = sPath;
    fi.wsPazFullPath    = wsPazPath;

    // Insert into tree — same logic as FileThread
    ParsePath(sPath, pathParts);
    if (pathParts.empty()) continue;

    Tree *ptr = root;
    for (auto it = pathParts.begin(); it != pathParts.end() - 1; ++it) {
      Tree *found = ptr->GetChildFolderWithName(*it);
      if (found) {
        ptr = found;
      } else {
        auto node = std::make_unique<Tree>(Tree::TREE_TYPE_FOLDER);
        node->SetFolderInfo(ptr, *it);
        ptr = ptr->AddChild(std::move(node));
      }
    }
    auto fileNode = std::make_unique<Tree>(Tree::TREE_TYPE_FILE);
    fileNode->SetFileInfo(ptr, pathParts.back(), fi);
    ptr->AddChild(std::move(fileNode));
  }

  return f.good() || f.eof();
}

void WriteCache(const std::wstring &pazFolder, Tree *root,
                uint32_t pazVersion, uint32_t pazFileCount) {
  uint64_t mtime = 0, size = 0;
  if (!GetMetaStat(pazFolder, mtime, size)) return;

  // Collect all file nodes
  std::vector<Tree*> files;
  root->GetFileNodeList(files);

  std::ofstream f(CachePath(pazFolder), std::ios::binary | std::ios::trunc);
  if (!f) return;

  // ── Header ────────────────────────────────────────────────────────────────
  uint32_t version     = CACHE_VERSION;
  uint32_t recordCount = static_cast<uint32_t>(files.size());

  f.write(CACHE_MAGIC,                                          4);
  f.write(reinterpret_cast<const char*>(&version),             4);
  f.write(reinterpret_cast<const char*>(&mtime),               8);
  f.write(reinterpret_cast<const char*>(&size),                8);
  f.write(reinterpret_cast<const char*>(&pazVersion),          4);
  f.write(reinterpret_cast<const char*>(&pazFileCount),        4);
  f.write(reinterpret_cast<const char*>(&recordCount),         4);

  uint16_t folderLen = static_cast<uint16_t>(pazFolder.size());
  f.write(reinterpret_cast<const char*>(&folderLen),           2);
  f.write(reinterpret_cast<const char*>(pazFolder.data()),     folderLen * sizeof(wchar_t));

  // ── Records ───────────────────────────────────────────────────────────────
  for (const Tree *node : files) {
    const FileInfo &fi = node->GetFileInfo();

    uint32_t pazIdx  = PazIdxFromPath(fi.wsPazFullPath);
    uint32_t offset  = fi.uiOffset;
    uint32_t compSz  = fi.uiCompressedSize;
    uint32_t origSz  = fi.uiOriginalSize;
    uint16_t pathLen = static_cast<uint16_t>(fi.sFullPath.size());

    f.write(reinterpret_cast<const char*>(&pazIdx),  4);
    f.write(reinterpret_cast<const char*>(&offset),  4);
    f.write(reinterpret_cast<const char*>(&compSz),  4);
    f.write(reinterpret_cast<const char*>(&origSz),  4);
    f.write(reinterpret_cast<const char*>(&pathLen), 2);
    f.write(fi.sFullPath.data(), pathLen);
  }
  // f closes on destruction
}

int ClearPreviewTempFiles() {
  WCHAR tempDir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tempDir);

  std::wstring pattern = std::wstring(tempDir) + L"paz_preview_*.*";

  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return 0;

  int count = 0;
  do {
    std::wstring fullPath = std::wstring(tempDir) + fd.cFileName;
    if (DeleteFileW(fullPath.c_str())) ++count;
  } while (FindNextFileW(h, &fd));

  FindClose(h);
  return count;
}

} // namespace kukdh1
