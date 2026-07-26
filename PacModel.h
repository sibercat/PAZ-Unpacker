#pragma once

#ifndef _PAC_MODEL_H_
#define _PAC_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace kukdh1 {

  // ── PAC ("PAR " type 0x03) skinned character mesh ──────────────────────────
  //
  //  Header
  //    0x00  char[4]   "PAR "
  //    0x04  uint8     0x03 (character mesh)
  //    0x05  uint8     version: 1, 2 or 3
  //    0x06  uint8[10] 00 01 02 03 04 05 06 07 08 09 (constant)
  //    0x10  uint32    unknown
  //    0x14  uint8     bone palette count
  //    0x15  uint32[]  bone palette -- see below
  //          uint8     count of a second small list
  //          uint8[]   that list (purpose unknown, skipped)
  //          uint32    unknown
  //          uint32    unknown
  //          uint16    submesh count
  //
  //  Submesh[submesh count]
  //    uint8     name length
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
  //  Version 1 uses a different header and is rejected. Versions 2 and 3 parse
  //  100% of a 78-file sample, and version 3 covers every character mesh seen.

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

      PacModel();

      bool Load(const std::wstring &wsPath);
      bool LoadFromMemory(const uint8_t *pData, size_t stSize);

      bool   IsEmpty() const;
      size_t TotalVertices() const;
      size_t TotalTriangles() const;

    private:
      void ComputeNormals(PacSubmesh &sm) const;
      void Clear();
  };

}

#endif
