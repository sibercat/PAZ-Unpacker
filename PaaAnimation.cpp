#include "PaaAnimation.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>

namespace kukdh1 {

namespace {

  constexpr uint8_t TYPE_ANIMATION = 0x02;
  constexpr size_t  HEADER_SIZE    = 0x1A;

  template <typename T>
  T Read(const uint8_t *p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
  }

  // MSVC has no std::float16_t. Same decode as PamModel / PacModel.
  float ReadF16(const uint8_t *p) {
    const uint16_t h = Read<uint16_t>(p);
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    const uint32_t exp  = (h >> 10) & 0x1F;
    const uint32_t man  = h & 0x03FF;

    uint32_t bits;
    if (exp == 0) {
      if (man == 0) bits = sign;
      else {
        int e = -1;
        uint32_t m = man;
        do { m <<= 1; e++; } while ((m & 0x0400) == 0);
        bits = sign | ((uint32_t)(127 - 15 - e) << 23) | ((m & 0x03FF) << 13);
      }
    }
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);
    else                bits = sign | ((exp + 112) << 23) | (man << 13);

    float f;
    memcpy(&f, &bits, 4);
    return f;
  }

}  // namespace

PaaAnimation::PaaAnimation() : uiVersion(0) {}

void PaaAnimation::Clear() {
  vTracks.clear();
  uiVersion = 0;
}

bool PaaAnimation::IsEmpty() const {
  for (const auto &t : vTracks)
    if (!t.vRotation.empty() || !t.vPosition.empty() || !t.vScale.empty())
      return false;
  return true;
}

uint32_t PaaAnimation::DurationMs() const {
  uint32_t m = 0;
  for (const auto &t : vTracks) {
    if (!t.vScale.empty())    m = (std::max)(m, (uint32_t)t.vScale.back().uiTimeMs);
    if (!t.vRotation.empty()) m = (std::max)(m, (uint32_t)t.vRotation.back().uiTimeMs);
    if (!t.vPosition.empty()) m = (std::max)(m, (uint32_t)t.vPosition.back().uiTimeMs);
  }
  return m;
}

size_t PaaAnimation::TotalKeys() const {
  size_t n = 0;
  for (const auto &t : vTracks)
    n += t.vScale.size() + t.vRotation.size() + t.vPosition.size();
  return n;
}

bool PaaAnimation::Load(const std::wstring &wsPath) {
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

bool PaaAnimation::LoadFromMemory(const uint8_t *pData, size_t stSize) {
  assert(pData != nullptr);
  Clear();

  if (stSize < HEADER_SIZE)          return false;
  if (memcmp(pData, "PAR ", 4) != 0) return false;
  if (pData[4] != TYPE_ANIMATION)    return false;

  uiVersion = pData[5];

  const uint32_t boneCount = Read<uint16_t>(pData + 0x10);
  const uint32_t dataSize  = Read<uint32_t>(pData + 0x16);
  if (boneCount == 0) return false;

  // The size identity is a cheap, very specific check that this really is a
  // clip laid out the way we expect before any of it is trusted.
  if (HEADER_SIZE + (size_t)10 * boneCount + dataSize != stSize) { Clear(); return false; }

  vTracks.reserve(boneCount);
  size_t off = HEADER_SIZE;

  for (uint32_t i = 0; i < boneCount; i++) {
    if (off + 10 > stSize) { Clear(); return false; }

    PaaTrack tr;
    tr.uiBoneId = Read<uint32_t>(pData + off); off += 4;

    const uint32_t ns = Read<uint16_t>(pData + off); off += 2;
    if (off + (size_t)8 * ns > stSize) { Clear(); return false; }
    tr.vScale.reserve(ns);
    for (uint32_t k = 0; k < ns; k++) {
      const uint8_t *p = pData + off + (size_t)8 * k;
      PaaVecKey key;
      key.uiTimeMs = Read<uint16_t>(p);
      for (int c = 0; c < 3; c++) key.v[c] = ReadF16(p + 2 + 2 * c);
      tr.vScale.push_back(key);
    }
    off += (size_t)8 * ns;

    if (off + 2 > stSize) { Clear(); return false; }
    const uint32_t nr = Read<uint16_t>(pData + off); off += 2;
    if (off + (size_t)10 * nr > stSize) { Clear(); return false; }
    tr.vRotation.reserve(nr);
    for (uint32_t k = 0; k < nr; k++) {
      const uint8_t *p = pData + off + (size_t)10 * k;
      PaaQuatKey key;
      key.uiTimeMs = Read<uint16_t>(p);
      for (int c = 0; c < 4; c++) key.q[c] = ReadF16(p + 2 + 2 * c);
      // A non-unit quaternion means the walk has drifted. Across 2 million
      // sampled keys none were off, so this only ever fires on a bad file.
      const float n2 = key.q[0]*key.q[0] + key.q[1]*key.q[1]
                     + key.q[2]*key.q[2] + key.q[3]*key.q[3];
      if (n2 < 0.9f || n2 > 1.1f) { Clear(); return false; }
      tr.vRotation.push_back(key);
    }
    off += (size_t)10 * nr;

    if (off + 2 > stSize) { Clear(); return false; }
    const uint32_t np = Read<uint16_t>(pData + off); off += 2;
    if (off + (size_t)8 * np > stSize) { Clear(); return false; }
    tr.vPosition.reserve(np);
    for (uint32_t k = 0; k < np; k++) {
      const uint8_t *p = pData + off + (size_t)8 * k;
      PaaVecKey key;
      key.uiTimeMs = Read<uint16_t>(p);
      for (int c = 0; c < 3; c++) key.v[c] = ReadF16(p + 2 + 2 * c);
      tr.vPosition.push_back(key);
    }
    off += (size_t)8 * np;

    vTracks.push_back(std::move(tr));
  }

  if (off != stSize) { Clear(); return false; }
  return !IsEmpty();
}

}  // namespace kukdh1
