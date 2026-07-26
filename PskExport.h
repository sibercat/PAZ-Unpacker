#pragma once

#ifndef _PSK_EXPORT_H_
#define _PSK_EXPORT_H_

#include <string>
#include <vector>

#include "PaaAnimation.h"
#include "PabSkeleton.h"
#include "PacModel.h"

namespace kukdh1 {

  // ── ActorX .psk / .psa export ──────────────────────────────────────────────
  //
  // Unreal's ActorX interchange format, as read by Gildor's ActorX Importer.
  // Chosen over FBX for rigs because it is unambiguous: a chunk id, a record
  // size and a count, then flat records. None of the conventions that make FBX
  // painful -- row versus column matrix layout, what a skin cluster's Transform
  // means, which importer honours which optional field -- exist here.
  //
  // Every chunk is preceded by:
  //     char[20] ChunkID       name, zero padded
  //     uint32   TypeFlag      1 for the header, 0 elsewhere
  //     uint32   DataSize      bytes per record
  //     uint32   DataCount     number of records
  //
  //  .psk, opening with a bare "ACTRHEAD" chunk:
  //     PNTS0000    12 B  float[3] position, one per unique point
  //     VTXW0000    16 B  wedge: uint16 point, uint16 pad, float u, float v,
  //                       uint8 material, uint8 reserved, uint16 pad
  //     FACE0000    12 B  uint16 wedge[3], uint8 material, uint8 auxMaterial,
  //                       uint32 smoothingGroups
  //                       (FACE3200 is the same with uint32 wedge indices, for
  //                        meshes past 65536 wedges)
  //     MATT0000    88 B  char[64] name + 6 int32
  //     REFSKELT   120 B  char[64] name, uint32 flags, int32 childCount,
  //                       int32 parent, float[4] quat, float[3] position,
  //                       float length, float[3] size
  //     RAWWEIGHTS  12 B  float weight, uint32 point, uint32 bone
  //
  //  .psa, opening with a bare "ANIMHEAD" chunk:
  //     BONENAMES  120 B  same record as REFSKELT
  //     ANIMINFO   160 B  char[64] name, char[64] group, int32 totalBones,
  //                       rootInclude, keyCompressionStyle, keyQuotum,
  //                       float keyReduction, trackTime, animRate,
  //                       int32 startBone, firstRawFrame, numRawFrames
  //     ANIMKEYS    32 B  float[3] position, float[4] quat, float time
  //
  // ANIMKEYS is uncompressed: one key per bone per frame, in bone order within
  // a frame. A .paa's sparse keys are therefore resampled onto a fixed rate.

  // Writes the mesh, its skeleton and the skin weights. Returns false on any
  // I/O failure; wsError explains a refusal.
  bool ExportPsk(const PacModel &model, const PabSkeleton &skel,
                 const std::wstring &wsPath, std::wstring &wsError);

  // One clip for the .psa writer. The caller owns the animation.
  struct PskAnimClip {
    std::string         sName;
    const PaaAnimation *pAnim;
  };

  // Writes one .psa holding every clip as a separate sequence, which is what
  // the format is for -- the bone table is stored once.
  bool ExportPsa(const PabSkeleton &skel, const std::vector<PskAnimClip> &vClips,
                 const std::wstring &wsPath, std::wstring &wsError);

}

#endif
