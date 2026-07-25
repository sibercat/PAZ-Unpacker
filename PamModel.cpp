#include "PamModel.h"

#include <cmath>
#include <cstring>
#include <fstream>

namespace kukdh1 {

  namespace {
    constexpr uint32_t MAT_BASE     = 0x410;   // material array offset
    constexpr uint32_t MAX_MATERIAL = 4096;    // sanity bound

    // Layout differences between the three versions seen in the archive.
    // Returns false for an unknown version.
    bool GetLayout(uint8_t version, uint32_t &matStride, uint32_t &nameOff,
                   uint32_t &vertexStride) {
      switch (version) {
        case 4: matStride = 0x110; nameOff = 0x10; vertexStride = 28; return true;
        case 5: matStride = 0x114; nameOff = 0x14; vertexStride = 28; return true;
        case 6: matStride = 0x110; nameOff = 0x10; vertexStride = 32; return true;
        default: return false;
      }
    }

    uint32_t ReadU32(const uint8_t *p) {
      uint32_t v;
      memcpy(&v, p, 4);
      return v;
    }

    float ReadF32(const uint8_t *p) {
      float v;
      memcpy(&v, p, 4);
      return v;
    }

    // IEEE 754 half → float. MSVC has no portable std::float16_t yet.
    float ReadF16(const uint8_t *p) {
      uint16_t h;
      memcpy(&h, p, 2);

      uint32_t sign = (uint32_t)(h & 0x8000) << 16;
      uint32_t exp  = (h >> 10) & 0x1F;
      uint32_t mant = h & 0x03FF;
      uint32_t bits;

      if (exp == 0) {
        if (mant == 0) {
          bits = sign;                       // +/- zero
        }
        else {
          // Subnormal — normalise it.
          exp = 127 - 15 + 1;
          while ((mant & 0x0400) == 0) {
            mant <<= 1;
            exp--;
          }
          mant &= 0x03FF;
          bits = sign | (exp << 23) | (mant << 13);
        }
      }
      else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (mant << 13);   // inf / NaN
      }
      else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
      }

      float f;
      memcpy(&f, &bits, 4);
      return f;
    }
  }

  PamModel::PamModel() : uiVersion(0), uiVertexStride(0) {
    Clear();
  }

  void PamModel::Clear() {
    vVertices.clear();
    vIndices.clear();
    vSubmeshes.clear();
    uiVersion      = 0;
    uiVertexStride = 0;
    for (int i = 0; i < 3; i++) {
      fBBoxMin[i] = 0.0f;
      fBBoxMax[i] = 0.0f;
    }
  }

  bool PamModel::IsEmpty() const {
    return vVertices.empty() || vIndices.empty();
  }

  bool PamModel::Load(const std::wstring &wsPath) {
    std::ifstream f(wsPath, std::ios::binary | std::ios::ate);
    if (!f) return false;

    std::streamoff len = f.tellg();
    if (len < (std::streamoff)MAT_BASE) return false;
    f.seekg(0);

    std::vector<uint8_t> data((size_t)len);
    f.read(reinterpret_cast<char *>(data.data()), len);
    if (!f) return false;

    return LoadFromMemory(data.data(), data.size());
  }

  bool PamModel::LoadFromMemory(const uint8_t *pData, size_t stSize) {
    Clear();

    if (pData == nullptr || stSize < MAT_BASE) return false;
    if (memcmp(pData, "PAR ", 4) != 0) return false;

    uint8_t  version = pData[5];
    uint32_t matStride = 0, nameOff = 0, vertexStride = 0;
    if (!GetLayout(version, matStride, nameOff, vertexStride)) return false;

    uint32_t matCount = ReadU32(pData + 0x10);
    if (matCount == 0 || matCount > MAX_MATERIAL) return false;

    // Material array must fit inside the file.
    uint64_t matEnd = (uint64_t)MAT_BASE + (uint64_t)matCount * matStride;
    if (matEnd > stSize) return false;

    uiVersion      = version;
    uiVertexStride = vertexStride;

    for (int i = 0; i < 3; i++) {
      fBBoxMin[i] = ReadF32(pData + 0x14 + i * 4);
      fBBoxMax[i] = ReadF32(pData + 0x20 + i * 4);
    }

    // ── Materials / submeshes ────────────────────────────────────────────────
    uint64_t totalVerts = 0, totalIndices = 0;
    vSubmeshes.reserve(matCount);

    for (uint32_t i = 0; i < matCount; i++) {
      const uint8_t *rec = pData + MAT_BASE + (size_t)i * matStride;

      PamSubmesh sm;
      sm.uiVertexCount = ReadU32(rec + 0x00);
      sm.uiIndexCount  = ReadU32(rec + 0x04);
      sm.uiBaseVertex  = ReadU32(rec + 0x08);
      sm.uiBaseIndex   = ReadU32(rec + 0x0C);

      // The bases are cumulative; anything else means we misread the layout.
      if (sm.uiBaseVertex != totalVerts || sm.uiBaseIndex != totalIndices)
        return false;
      if (sm.uiIndexCount % 3 != 0) return false;

      const char *name = reinterpret_cast<const char *>(rec + nameOff);
      size_t maxName = matStride - nameOff;
      size_t nameLen = 0;
      while (nameLen < maxName && name[nameLen] != '\0') nameLen++;
      sm.sTexture.assign(name, nameLen);

      totalVerts   += sm.uiVertexCount;
      totalIndices += sm.uiIndexCount;
      vSubmeshes.push_back(std::move(sm));
    }

    if (totalVerts == 0 || totalIndices == 0) return false;
    if (totalVerts > 0xFFFFFFFFull) return false;

    // ── Buffer bounds — the file size must match exactly ─────────────────────
    uint64_t vbOff = matEnd;
    uint64_t ibOff = vbOff + totalVerts * vertexStride;
    uint64_t end   = ibOff + totalIndices * 2;
    if (end != stSize) return false;

    // ── Vertices ─────────────────────────────────────────────────────────────
    vVertices.resize((size_t)totalVerts);
    for (size_t i = 0; i < vVertices.size(); i++) {
      const uint8_t *v = pData + vbOff + i * vertexStride;
      PamVertex &out = vVertices[i];
      out.x = ReadF32(v + 0x00);
      out.y = ReadF32(v + 0x04);
      out.z = ReadF32(v + 0x08);
      out.u = ReadF16(v + 0x10);
      out.v = ReadF16(v + 0x12);
      out.nx = out.ny = out.nz = 0.0f;
    }

    // ── Indices (submesh-local → global) ─────────────────────────────────────
    vIndices.resize((size_t)totalIndices);
    const uint8_t *ib = pData + ibOff;

    for (const auto &sm : vSubmeshes) {
      for (uint32_t k = 0; k < sm.uiIndexCount; k++) {
        uint16_t local;
        memcpy(&local, ib + ((size_t)sm.uiBaseIndex + k) * 2, 2);
        if (local >= sm.uiVertexCount) return false;   // corrupt submesh
        vIndices[sm.uiBaseIndex + k] = sm.uiBaseVertex + local;
      }
    }

    ComputeNormals();
    return true;
  }

  // Smooth normals from area-weighted face normals. The packed normal in the
  // vertex is not decoded, and averaging gives a good result for preview.
  void PamModel::ComputeNormals() {
    for (size_t i = 0; i + 2 < vIndices.size(); i += 3) {
      PamVertex &a = vVertices[vIndices[i + 0]];
      PamVertex &b = vVertices[vIndices[i + 1]];
      PamVertex &c = vVertices[vIndices[i + 2]];

      float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
      float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;

      // Cross product magnitude is twice the triangle area, so this weights
      // large faces more heavily — no normalisation needed here.
      float nx = uy * vz - uz * vy;
      float ny = uz * vx - ux * vz;
      float nz = ux * vy - uy * vx;

      a.nx += nx; a.ny += ny; a.nz += nz;
      b.nx += nx; b.ny += ny; b.nz += nz;
      c.nx += nx; c.ny += ny; c.nz += nz;
    }

    for (auto &v : vVertices) {
      float len = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
      if (len > 1e-12f) {
        v.nx /= len; v.ny /= len; v.nz /= len;
      }
      else {
        v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f;
      }
    }
  }

  void PamModel::GetCenter(float out[3]) const {
    out[0] = (fBBoxMin[0] + fBBoxMax[0]) * 0.5f;
    out[1] = (fBBoxMin[1] + fBBoxMax[1]) * 0.5f;
    out[2] = (fBBoxMin[2] + fBBoxMax[2]) * 0.5f;
  }

  float PamModel::GetRadius() const {
    float dx = fBBoxMax[0] - fBBoxMin[0];
    float dy = fBBoxMax[1] - fBBoxMin[1];
    float dz = fBBoxMax[2] - fBBoxMin[2];
    float r  = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    return (r > 1e-6f) ? r : 1.0f;
  }

}
