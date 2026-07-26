#include "PacModel.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>

namespace kukdh1 {

namespace {

  constexpr uint8_t TYPE_CHARACTER_MESH = 0x03;
  constexpr size_t  VERTEX_STRIDE       = 32;
  constexpr int     LOD_COUNT           = 3;

  constexpr size_t V_POS    = 0x00;
  constexpr size_t V_UV     = 0x10;
  constexpr size_t V_BONE   = 0x18;
  constexpr size_t V_WEIGHT = 0x1C;

  template <typename T>
  T Read(const uint8_t *p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
  }

  // MSVC has no std::float16_t, so decode the half manually. Same routine as
  // PamModel uses; kept local rather than shared because the two parsers are
  // otherwise independent.
  float ReadF16(const uint8_t *p) {
    const uint16_t h = Read<uint16_t>(p);
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    const uint32_t exp  = (h >> 10) & 0x1F;
    const uint32_t man  = h & 0x03FF;

    uint32_t bits;
    if (exp == 0) {
      if (man == 0) bits = sign;                       // +/- zero
      else {
        // Subnormal: renormalise into a float exponent.
        int e = -1;
        uint32_t m = man;
        do { m <<= 1; e++; } while ((m & 0x0400) == 0);
        bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x03FF) << 13);
      }
    }
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);   // inf / NaN
    else                bits = sign | ((exp + 112) << 23) | (man << 13);

    float f;
    memcpy(&f, &bits, 4);
    return f;
  }

}  // namespace

PacModel::PacModel() : uiVersion(0) {}

void PacModel::Clear() {
  vBonePalette.clear();
  vSubmeshes.clear();
  uiVersion = 0;
}

bool PacModel::IsEmpty() const {
  if (vSubmeshes.empty()) return true;
  for (const auto &s : vSubmeshes)
    if (!s.vVertices.empty() && !s.vIndices.empty()) return false;
  return true;
}

size_t PacModel::TotalVertices() const {
  size_t n = 0;
  for (const auto &s : vSubmeshes) n += s.vVertices.size();
  return n;
}

size_t PacModel::TotalTriangles() const {
  size_t n = 0;
  for (const auto &s : vSubmeshes) n += s.vIndices.size() / 3;
  return n;
}

bool PacModel::Load(const std::wstring &wsPath) {
  std::ifstream f(wsPath, std::ios::binary | std::ios::ate);
  if (!f) return false;
  std::streamoff len = f.tellg();
  if (len <= 0) return false;
  f.seekg(0);
  std::vector<uint8_t> buf((size_t)len);
  f.read(reinterpret_cast<char *>(buf.data()), len);
  if (!f) return false;
  return LoadFromMemory(buf.data(), buf.size());
}

bool PacModel::LoadFromMemory(const uint8_t *pData, size_t stSize) {
  assert(pData != nullptr);
  Clear();

  if (stSize < 0x20) return false;
  if (memcmp(pData, "PAR ", 4) != 0)        return false;
  if (pData[4] != TYPE_CHARACTER_MESH)      return false;

  uiVersion = pData[5];
  // Version 1 lays its header out differently and is not supported.
  if (uiVersion != 2 && uiVersion != 3) { Clear(); return false; }

  size_t off = 0x14;
  const size_t paletteCount = pData[off++];
  if (off + 4 * paletteCount > stSize) { Clear(); return false; }
  vBonePalette.reserve(paletteCount);
  for (size_t i = 0; i < paletteCount; i++)
    vBonePalette.push_back(Read<uint32_t>(pData + off + 4 * i));
  off += 4 * paletteCount;

  if (off >= stSize) { Clear(); return false; }
  const size_t listCount = pData[off++];
  off += listCount;                       // purpose unknown, not needed here

  if (off + 10 > stSize) { Clear(); return false; }
  off += 8;                               // two unknown uint32s
  const size_t submeshCount = Read<uint16_t>(pData + off);
  off += 2;
  if (submeshCount == 0) { Clear(); return false; }

  vSubmeshes.reserve(submeshCount);
  for (size_t s = 0; s < submeshCount; s++) {
    if (off >= stSize) { Clear(); return false; }
    const size_t nameLen = pData[off];
    if (nameLen == 0 || nameLen > 64)             { Clear(); return false; }
    if (off + 1 + nameLen + 2 > stSize)           { Clear(); return false; }
    for (size_t i = 0; i < nameLen; i++)
      if (pData[off + 1 + i] < 0x20)              { Clear(); return false; }

    PacSubmesh sm;
    sm.sName.assign(reinterpret_cast<const char *>(pData + off + 1), nameLen);
    off += 1 + nameLen + 2;

    for (int lod = 0; lod < LOD_COUNT; lod++) {
      if (off + 2 > stSize) { Clear(); return false; }
      const size_t vcount = Read<uint16_t>(pData + off);
      const size_t vstart = off + 2;
      const size_t vend   = vstart + vcount * VERTEX_STRIDE;
      if (vend + 4 > stSize) { Clear(); return false; }

      const uint32_t icount = Read<uint32_t>(pData + vend);
      if (icount % 3 != 0)  { Clear(); return false; }
      const size_t iend = vend + 4 + (size_t)icount * 2;
      if (iend > stSize)    { Clear(); return false; }

      // Only LOD 0 is kept; the rest are decimated copies of the same mesh.
      if (lod == 0) {
        sm.vVertices.reserve(vcount);
        for (size_t i = 0; i < vcount; i++) {
          const uint8_t *v = pData + vstart + i * VERTEX_STRIDE;
          PacVertex pv = {};
          pv.x = Read<float>(v + V_POS + 0);
          pv.y = Read<float>(v + V_POS + 4);
          pv.z = Read<float>(v + V_POS + 8);
          pv.u = ReadF16(v + V_UV + 0);
          pv.v = ReadF16(v + V_UV + 2);
          for (int k = 0; k < 4; k++) {
            pv.bone[k]   = v[V_BONE + k];
            pv.weight[k] = v[V_WEIGHT + k];
          }
          sm.vVertices.push_back(pv);
        }

        sm.vIndices.reserve(icount);
        for (uint32_t i = 0; i < icount; i++) {
          const uint16_t idx = Read<uint16_t>(pData + vend + 4 + (size_t)i * 2);
          if (idx >= vcount) { Clear(); return false; }
          sm.vIndices.push_back(idx);
        }
      }
      off = iend;
    }

    // A palette offset that does not exist would produce a silently wrong
    // skin, so reject rather than clamp.
    for (const auto &pv : sm.vVertices)
      for (int k = 0; k < 4; k++)
        if (pv.weight[k] && pv.bone[k] >= paletteCount) { Clear(); return false; }

    ComputeNormals(sm);
    vSubmeshes.push_back(std::move(sm));
  }

  // A trailer may follow; it carries nothing needed for geometry.
  return !IsEmpty();
}

void PacModel::ComputeNormals(PacSubmesh &sm) const {
  for (auto &v : sm.vVertices) { v.nx = v.ny = v.nz = 0.0f; }

  // Area-weighted face normals accumulated per vertex, matching PamModel.
  for (size_t i = 0; i + 2 < sm.vIndices.size(); i += 3) {
    PacVertex &a = sm.vVertices[sm.vIndices[i + 0]];
    PacVertex &b = sm.vVertices[sm.vIndices[i + 1]];
    PacVertex &c = sm.vVertices[sm.vIndices[i + 2]];

    const float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const float nx = uy * vz - uz * vy;
    const float ny = uz * vx - ux * vz;
    const float nz = ux * vy - uy * vx;

    a.nx += nx; a.ny += ny; a.nz += nz;
    b.nx += nx; b.ny += ny; b.nz += nz;
    c.nx += nx; c.ny += ny; c.nz += nz;
  }

  for (auto &v : sm.vVertices) {
    const float len = std::sqrt(v.nx*v.nx + v.ny*v.ny + v.nz*v.nz);
    if (len > 1e-12f) { v.nx /= len; v.ny /= len; v.nz /= len; }
    else              { v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f; }
  }
}

}  // namespace kukdh1
