#include "MetaFile.h"
#include <cassert>
#include <stdexcept>

namespace kukdh1 {
	PAZTable::PAZTable(uint8_t *buffer) {
		memcpy(&uiPazFileID, buffer + 0, 4);
		memcpy(&uiCRC, buffer + 4, 4);
		memcpy(&uiSize, buffer + 8, 4);
	}

  Meta::Meta(wchar_t *wpszPazFolder) {
    assert(wpszPazFolder != nullptr);
    std::fstream file;
		std::wstring path(wpszPazFolder);

		path.append(L"\\pad00000.meta");

    file.open(path, std::ios::in | std::ios::binary);
    if (file.is_open()) {
      uint8_t buffer[64];

      // Read Header
      file.read((char *)buffer, 8);
      if (file.gcount() != 8) {
        throw std::runtime_error("pad00000.meta is corrupt: truncated header");
      }
			memcpy(&uiVersion, buffer + 0, 4);
			memcpy(&uiPAZFileCount, buffer + 4, 4);

      // Sanity bound — a foreign or corrupt meta file would otherwise loop
      // over garbage (BDO ships ~11k PAZ files).
      if (uiPAZFileCount == 0 || uiPAZFileCount > 100000) {
        throw std::runtime_error("pad00000.meta is invalid: unreasonable PAZ file count");
      }

      // Read PAZ File information
      vPAZs.reserve(uiPAZFileCount);
      for (uint32_t idx = 0; idx < uiPAZFileCount; idx++) {
        file.read((char *)buffer, 12);
        if (file.gcount() != 12) {
          throw std::runtime_error("pad00000.meta is corrupt: truncated PAZ table");
        }

        vPAZs.push_back(PAZTable(buffer));
      }

      file.close();
    }
    else {
      throw std::runtime_error("Cannot open meta file: pad00000.meta not found in selected folder");
    }
  }
}
