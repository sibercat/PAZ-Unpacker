#include "PazFile.h"
#include <cassert>
#include <memory>

namespace kukdh1 {
	FileInfo::FileInfo(uint8_t *buffer) {
		assert(buffer != nullptr);
		memcpy(&uiCRC, buffer + 0, 4);
		memcpy(&uiFolderID, buffer + 4, 4);
		memcpy(&uiFileID, buffer + 8, 4);
		memcpy(&uiOffset, buffer + 12, 4);
		memcpy(&uiCompressedSize, buffer + 16, 4);
		memcpy(&uiOriginalSize, buffer + 20, 4);
	}

  FileInfo::FileInfo() {
    // DO NOTHING
  }

	PazFile::PazFile(wchar_t *wpszPazFolder, uint32_t uiPazIndex, CryptICE &cipher) {
		assert(wpszPazFolder != nullptr);
		std::wstring path(wpszPazFolder);
		std::wstringstream wss(path, std::ios_base::out | std::ios_base::in | std::ios_base::ate);

		wss << L"\\PAD" << std::setw(5) << std::setfill(L'0') << uiPazIndex << L".PAZ";

		path = wss.str();

		std::fstream file(path, std::ios::binary | std::ios::in);
		if (!file.is_open()) {
			throw std::runtime_error("Failed to open PAZ file");
		}

		uint8_t buffer[64];
		uint32_t uiFileCount;
		uint32_t uiPathLength;

		// Read Header
		file.read((char *)buffer, 12);
		memcpy(&uiCRC, buffer + 0, 4);
		memcpy(&uiFileCount, buffer + 4, 4);
		memcpy(&uiPathLength, buffer + 8, 4);

		// Read file info
		std::vector<uint8_t> infos(uiFileCount * 24);
		file.read(reinterpret_cast<char *>(infos.data()), infos.size());

		for (uint32_t i = 0; i < uiFileCount; i++) {
			vFileInfo.push_back(FileInfo(infos.data() + i * 24));
			vFileInfo.back().wsPazFullPath = path;
		}

		// Read encrypted file names
		std::vector<uint8_t> encrypted(uiPathLength);
		file.read(reinterpret_cast<char *>(encrypted.data()), uiPathLength);

		// Decrypt — output is malloc'd by cipher, wrap in unique_ptr
		uint8_t *decrypted_raw = nullptr;
		cipher.decrypt(encrypted.data(), uiPathLength, &decrypted_raw, &uiPathLength);
		auto decrypted = std::unique_ptr<uint8_t, decltype(&free)>(decrypted_raw, free);

		// Parse null-separated path strings
		std::vector<std::string> paths;
		const char *ptr = reinterpret_cast<const char *>(decrypted.get());
		const char *end = ptr + uiPathLength;
		for (; ptr < end; ) {
			paths.push_back(ptr);
			ptr += paths.back().length() + 1;
		}

		for (auto &info : vFileInfo) {
			info.sFullPath = paths.at(info.uiFolderID);
			info.sFullPath.append(paths.at(info.uiFileID));
		}
	}
}
