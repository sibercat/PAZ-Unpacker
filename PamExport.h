#pragma once

#ifndef _PAM_EXPORT_H_
#define _PAM_EXPORT_H_

#include <string>
#include <vector>

#include "PamModel.h"

namespace kukdh1 {

  enum PamExportFormat {
    PAM_EXPORT_OBJ,   // Wavefront OBJ + companion .mtl
    PAM_EXPORT_FBX    // Autodesk FBX, binary version 7.4
  };

  // One entry per submesh: the texture file written next to the model, or an
  // empty string if the texture could not be resolved. Used to fill in the
  // material definitions.
  typedef std::vector<std::string> PamTextureFileList;

  // Writes the model to wsPath in the requested format. Returns false on any
  // I/O failure. Textures are not written here — the caller extracts those
  // from the archive, since only it can reach the PAZ data.
  bool ExportModel(const PamModel &model, const std::wstring &wsPath,
                   PamExportFormat format, const PamTextureFileList &vTextureFiles,
                   std::wstring &wsError);

  // Default file extension for a format, without the dot.
  const wchar_t *ExportExtension(PamExportFormat format);

}

#endif
