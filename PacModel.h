#pragma once

#ifndef _PAC_MODEL_H_
#define _PAC_MODEL_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "PamModel.h"
#include "PabSkeleton.h"

namespace kukdh1 {

  // ── PAC ("PAR " type 0x03) skinned character mesh ──────────────────────────
  //
  //  Header
  //    0x00  char[4]   "PAR "
  //    0x04  uint8     0x03 (character mesh)
  //    0x05  uint8     version: 1, 2 or 3
  //    0x06  uint8[10] 00 01 02 03 04 05 06 07 08 09 (constant)
  //
  //    All three versions share this shape; only where the palette starts
  //    differs. Version 1 puts the count at 0x10 and the palette at 0x11,
  //    unaligned. Versions 2 and 3 put the count at 0x14, with a uint32 at
  //    0x10 that is normally 0.
  //
  //    When that uint32 is 1, the mesh carries its own bones and the palette
  //    count moves past them:
  //      0x14  uint32    bone count
  //      0x18  record[bone count], 306 bytes each:
  //              uint32    bone id (same id space as PabBone::uiBoneId)
  //              int32     parent index, -1 for the root
  //              float[4][4] x 4  local, inverse local, bind, inverse bind
  //              float[10] scale, rotation quaternion, translation
  //              uint8     unknown
  //              uint8     1 if a further 36 bytes follow the record
  //    Seen on 144 meshes, all ending on the usual 25-byte trailer: every
  //    class's _0033 weapon, the smallnom/smallpet backpack pets, some
  //    monster and mount wings, and pkow_tail_01. No archive .pab covers
  //    them; the table is their rig -- see PacEmbeddedSkeleton().
  //
  //          uint8     bone palette count
  //          uint32[]  bone palette -- see below
  //          uint8     count of a second small list
  //          uint8[]   that list (purpose unknown, skipped)
  //          uint32    unknown
  //          uint32    unknown
  //          uint16    submesh count
  //
  //  A submesh count of 0 is a placeholder: 493 files (pgw_10_hel_0031_haircut
  //  and the like) hold an empty bounding box and nothing else.
  //
  //  Submesh[submesh count]
  //    uint8     name length, occasionally 0
  //    char[]    name
  //    uint16    unknown
  //    3 x LOD, highest detail first:
  //      uint16  vertex count
  //      vertex[vertex count], stride 32:
  //        +0x00 float[3]  position, centimetres
  //        +0x0C uint8[4]  packed normal / tangent (not decoded)
  //        +0x10 half[2]   texture coordinates
  //        +0x14 uint8[4]  unknown (tangent or a second UV set)
  //        +0x18 uint8[4]  bone indices -- into the palette, NOT the skeleton
  //        +0x1C uint8[4]  bone weights, quantised so they sum to ~255
  //      uint32  index count, always a multiple of 3
  //      uint16  index[index count], submesh-local
  //
  //  A trailer of up to a few hundred bytes follows the last submesh on some
  //  character meshes; it holds nothing needed for geometry and is ignored.
  //
  //  The bone palette is the link to the skeleton. A vertex stores palette
  //  offsets, not skeleton indices, so resolving a skin means mapping each
  //  palette entry onto the .pab bone whose PabBone::uiBoneId matches. This is
  //  what lets one mesh bind to several skeleton variants.
  //
  //  Only LOD 0 is kept -- the other two are reduced copies of the same mesh.
  //
  //  All three versions are supported. Version 1 carries the player character
  //  head, hair, eye and body meshes, so it is not a legacy corner.

  struct PacVertex {
    float   x, y, z;
    float   u, v;
    float   nx, ny, nz;      // computed from geometry, as for PamModel
    uint8_t bone[4];         // palette offsets
    uint8_t weight[4];       // sum ~255
  };

  struct PacSubmesh {
    std::string             sName;
    std::vector<PacVertex>  vVertices;
    std::vector<uint32_t>   vIndices;   // submesh-local
  };

  class PacModel {
    public:
      std::vector<uint32_t>   vBonePalette;   // palette slot -> PabBone::uiBoneId
      std::vector<PacSubmesh> vSubmeshes;
      uint32_t                uiVersion;
      bool                    bPlaceholder;     // parsed, zero submeshes;
                                                // Load still returns false

      // The mesh's own bones, when it carries them (see above). Names are
      // empty: the file stores none.
      std::vector<PabBone>    vEmbeddedBones;

      PacModel();

      bool Load(const std::wstring &wsPath);
      bool LoadFromMemory(const uint8_t *pData, size_t stSize);

      bool   IsEmpty() const;
      size_t TotalVertices() const;
      size_t TotalTriangles() const;

      // The palette ids some vertex actually leans on, with a non-zero weight.
      //
      // Meshes carry dead slots: pnw_10_hand_0027 has four no vertex touches,
      // and one of those names a bone that appears in none of the archive's
      // skeletons. Since nothing is weighted to it, it cannot move a vertex --
      // so demanding it resolve would refuse an export over a bone that does
      // not exist and does not matter. Bind against these, not the raw palette.
      std::vector<uint32_t> UsedBoneIds() const;

    private:
      void ComputeNormals(PacSubmesh &sm) const;
      void Clear();
  };

  // Flattens a character mesh into the static-model shape the rasteriser and
  // the OBJ/FBX writers already understand: submesh vertices concatenated,
  // indices rebased, bounding box computed.
  //
  // Skin weights are dropped -- this is for previewing and for paths that do
  // not care about the rig. Use PacModel plus PabSkeleton directly when the
  // binding matters.
  //
  // Each submesh's texture is set to "<submesh name>.dds", which is the
  // archive's naming convention (see PacModel above), so the existing
  // by-name texture lookup resolves a character mesh unchanged.
  bool PacToPamModel(const PacModel &src, PamModel &dst);

  // Turns a mesh's own bone table into a skeleton the exporters can bind to.
  //
  // A mesh that carries one is bound to it, not to a .pab: no archive rig
  // covers these meshes, and where a .pab shares a bone with one its bind pose
  // can sit a metre away (the weapon is posed for its own rig, not the
  // class's). The file stores no names, so each bone takes the name vNames
  // gives its id -- borrowed from nearby .pab files -- or "Bone_<index>".
  // Names stay unique; a borrowed one already taken falls back too.
  //
  // Returns false when the mesh carries no table.
  bool PacEmbeddedSkeleton(const PacModel &src,
                           const std::unordered_map<uint32_t, std::string> &vNames,
                           PabSkeleton &dst);

}

#endif
