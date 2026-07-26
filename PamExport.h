#pragma once

#ifndef _PAM_EXPORT_H_
#define _PAM_EXPORT_H_

#include <string>
#include <vector>

#include "PaaAnimation.h"
#include "PabSkeleton.h"
#include "PacModel.h"
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

  // Writes a .pab skeleton as a binary FBX bone hierarchy (LimbNode chain with
  // local translation / rotation / scale). FBX only — OBJ cannot represent a
  // node hierarchy. Returns false on any I/O failure.
  bool ExportSkeleton(const PabSkeleton &skel, const std::wstring &wsPath,
                      std::wstring &wsError);

  // Writes a .pab skeleton with a .paa clip applied: the bone hierarchy plus
  // FBX animation curves. Tracks are matched to bones by id, and a clip that
  // drives none of this skeleton's bones is refused rather than exported as a
  // silent rest pose. FBX only.
  bool ExportAnimation(const PabSkeleton &skel, const PaaAnimation &anim,
                       const std::wstring &wsPath, std::wstring &wsError);

  // One animation clip to carry alongside a mesh, as its own FBX take. The
  // caller owns the PaaAnimation.
  struct PamAnimClip {
    std::string         sName;    // take name, normally the .paa file name
    const PaaAnimation *pAnim;
  };

  // Writes a .pac character mesh bound to its .pab skeleton: geometry plus a
  // bone hierarchy, skin clusters and a bind pose. The mesh's bone palette is
  // resolved against the skeleton by bone id, so the two files must belong
  // together; wsError explains it if they do not. FBX only.
  //
  // Any clips given are written into the same file, one AnimationStack each.
  // That is not a convenience: Unreal produces nothing at all from an
  // animation-only FBX -- verified against UE 5.8, and equally true of one
  // Blender itself exported -- whereas a file carrying the mesh and its takes
  // together imports as a SkeletalMesh plus one AnimSequence per take.
  bool ExportSkinnedModel(const PacModel &model, const PabSkeleton &skel,
                          const PamTextureFileList &vTextureFiles,
                          const std::wstring &wsPath, std::wstring &wsError,
                          const std::vector<PamAnimClip> &vClips = {});

}

#endif
