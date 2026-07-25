#pragma once

#ifndef _PAM_MODEL_H_
#define _PAM_MODEL_H_

#include <cstdint>
#include <string>
#include <vector>

namespace kukdh1 {

  // ── PAM ("PAR ") model format ───────────────────────────────────────────────
  //
  //  Header (0x410 bytes)
  //    0x00  char[4]   "PAR "
  //    0x04  uint8     0x06 (constant)
  //    0x05  uint8     version: 4, 5 or 6
  //    0x06  uint8[2]  00 01
  //    0x08  uint8[8]  vertex-element descriptor
  //    0x10  uint32    material count
  //    0x14  float[3]  bounding box min
  //    0x20  float[3]  bounding box max
  //    0x2C..0x40F     reserved (zero)
  //
  //  Material[count] @ 0x410 — one submesh each
  //    v4/v6: stride 0x110, name at +0x10
  //    v5:    stride 0x114, name at +0x14 (an extra uint32 sits at +0x10)
  //      +0x00 uint32   vertex count
  //      +0x04 uint32   index count
  //      +0x08 uint32   base vertex (cumulative)
  //      +0x0C uint32   base index  (cumulative)
  //      +nameOff char[] texture file name, null terminated
  //
  //  Vertex buffer — stride 28 (v4/v5) or 32 (v6)
  //      +0x00 float[3] position
  //      +0x0C float    1.0 (constant)
  //      +0x10 half[2]  texture coordinates
  //      +0x14 uint8[4] packed normal/tangent (encoding not decoded)
  //      +0x18 uint8[4] vertex colour (RGBA, almost always white)
  //      +0x1C uint8[4] v6 only — extra channel, usually zero
  //
  //  Index buffer — uint16 triangle list, indices are submesh-local
  //  (add the submesh base vertex to get a global index).
  //
  //  Verified against 385 files sampled across the whole archive: every file
  //  parses exactly and the decoded positions reproduce the declared bounding
  //  box with zero error.

  struct PamVertex {
    float x, y, z;
    float u, v;
    float nx, ny, nz;   // computed from geometry — the packed normal is not decoded
  };

  struct PamSubmesh {
    uint32_t    uiBaseVertex;
    uint32_t    uiVertexCount;
    uint32_t    uiBaseIndex;
    uint32_t    uiIndexCount;
    std::string sTexture;
  };

  class PamModel {
    public:
      std::vector<PamVertex>  vVertices;
      std::vector<uint32_t>   vIndices;     // global (base vertex already applied)
      std::vector<PamSubmesh> vSubmeshes;

      float    fBBoxMin[3];
      float    fBBoxMax[3];
      uint32_t uiVersion;
      uint32_t uiVertexStride;

      PamModel();

      // Parses a decrypted .pam file. Returns false if the file is not a
      // recognised PAR model or fails any structural check.
      bool Load(const std::wstring &wsPath);
      bool LoadFromMemory(const uint8_t *pData, size_t stSize);

      bool   IsEmpty() const;
      void   GetCenter(float out[3]) const;
      float  GetRadius() const;

    private:
      void ComputeNormals();
      void Clear();
  };

}

#endif
