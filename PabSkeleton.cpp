#include "PabSkeleton.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <Windows.h>

namespace kukdh1 {

namespace {

  constexpr size_t HEADER_SIZE   = 0x16;
  // The payload proper is 302 bytes; the two that follow it are the head of
  // whatever comes next. Counting them here keeps the separator table below in
  // whole units, and every offset used from the payload sits well inside 302.
  constexpr size_t BONE_PAYLOAD  = 304;
  constexpr uint8_t TYPE_SKELETON = 0x01;

  // Field offsets inside the payload.
  constexpr size_t OFF_PARENT = 0x000;
  constexpr size_t OFF_SCALE  = 0x104;
  constexpr size_t OFF_QUAT   = 0x110;
  constexpr size_t OFF_TRANS  = 0x120;

  // Bytes between the end of one payload and the start of the next record: an
  // optional 24/36/56/60-byte block, then the next bone's 4-byte id, less the
  // two already counted above. The flag bytes cannot tell 24 from 60 apart, so
  // the correct value is found by validating the record that follows.
  constexpr size_t SEPARATORS[] = { 2, 26, 38, 58, 62 };
  // The final bone still carries its block, but no id follows it.
  constexpr size_t TRAILERS[]   = { 0, 24, 36, 56, 60 };

  template <typename T>
  T Read(const uint8_t *p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
  }

  // Bone names are CP949 (Korean). FBX and the rest of the app expect UTF-8.
  std::string Cp949ToUtf8(const uint8_t *p, size_t n) {
    if (n == 0) return std::string();

    bool ascii = true;
    for (size_t i = 0; i < n; i++)
      if (p[i] >= 0x80) { ascii = false; break; }
    if (ascii) return std::string(reinterpret_cast<const char *>(p), n);

    int wlen = MultiByteToWideChar(949, 0, reinterpret_cast<const char *>(p), (int)n, nullptr, 0);
    if (wlen <= 0) return std::string(reinterpret_cast<const char *>(p), n);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(949, 0, reinterpret_cast<const char *>(p), (int)n, w.data(), wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return std::string(reinterpret_cast<const char *>(p), n);
    std::string u(ulen, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wlen, u.data(), ulen, nullptr, nullptr);
    return u;
  }

  // A record start is plausible when the length byte, the name bytes and the
  // parent index are all in range. This is what disambiguates the separator.
  bool PlausibleRecord(const uint8_t *pData, size_t stSize, size_t off, uint32_t uiBoneCount) {
    if (off >= stSize) return false;
    size_t n = pData[off];
    if (n == 0 || n > 64) return false;
    if (off + 1 + n + 4 > stSize) return false;
    for (size_t i = 0; i < n; i++)
      if (pData[off + 1 + i] < 0x20) return false;   // control byte: not a name
    int32_t parent = Read<int32_t>(pData + off + 1 + n);
    return parent >= -1 && parent < (int32_t)uiBoneCount;
  }

  size_t RecordEnd(const uint8_t *pData, size_t off) {
    return off + 1 + pData[off] + BONE_PAYLOAD;
  }

}  // namespace

PabSkeleton::PabSkeleton() : uiVersion(0), uiFirstBoneId(0) {}

void PabSkeleton::Clear() {
  vBones.clear();
  uiVersion     = 0;
  uiFirstBoneId = 0;
}

bool PabSkeleton::IsEmpty() const {
  return vBones.empty();
}

bool PabSkeleton::Load(const std::wstring &wsPath) {
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

bool PabSkeleton::LoadFromMemory(const uint8_t *pData, size_t stSize) {
  assert(pData != nullptr);
  Clear();

  if (stSize < HEADER_SIZE) return false;
  if (memcmp(pData, "PAR ", 4) != 0) return false;
  if (pData[4] != TYPE_SKELETON)     return false;

  uiVersion = pData[5];
  if (uiVersion != 3 && uiVersion != 4) return false;

  uint32_t uiBoneCount = Read<uint16_t>(pData + 0x10);
  uiFirstBoneId        = Read<uint32_t>(pData + 0x12);
  if (uiBoneCount == 0) return false;

  // Guard against a corrupt count demanding an implausible allocation.
  if ((size_t)uiBoneCount * (1 + BONE_PAYLOAD) > stSize) return false;

  vBones.reserve(uiBoneCount);

  // A bone's id is the 4 bytes immediately preceding its record, so bone 0
  // takes the header field and every later bone takes the tail of the gap that
  // separates it from the bone before. Carrying it forward is what keeps the
  // id with the bone it names.
  uint32_t uiPendingId = uiFirstBoneId;

  size_t off = HEADER_SIZE;
  for (uint32_t i = 0; i < uiBoneCount; i++) {
    if (!PlausibleRecord(pData, stSize, off, uiBoneCount)) { Clear(); return false; }

    size_t nameLen = pData[off];
    size_t payload = off + 1 + nameLen;
    if (payload + BONE_PAYLOAD > stSize) { Clear(); return false; }

    PabBone b;
    b.sName   = Cp949ToUtf8(pData + off + 1, nameLen);
    b.iParent = Read<int32_t>(pData + payload + OFF_PARENT);

    // Parents always precede their children, so this also rejects cycles.
    if (b.iParent >= (int32_t)i) { Clear(); return false; }
    if (i == 0 && b.iParent != -1) { Clear(); return false; }

    for (int k = 0; k < 3; k++) b.fScale[k] = Read<float>(pData + payload + OFF_SCALE + 4 * k);
    for (int k = 0; k < 4; k++) b.fQuat[k]  = Read<float>(pData + payload + OFF_QUAT  + 4 * k);
    for (int k = 0; k < 3; k++) b.fTrans[k] = Read<float>(pData + payload + OFF_TRANS + 4 * k);

    b.uiBoneId = uiPendingId;

    // A non-normal quaternion means the offsets have drifted; that is the
    // cheapest reliable check that this record was read at the right place.
    // The tolerance is deliberately loose: some shipped bones are up to ~1%
    // off (B_Eye_L_01 in m0113_00 has |q|^2 = 0.977) and are perfectly
    // usable once normalised, whereas genuine drift lands nowhere near 1.
    float q2 = b.fQuat[0]*b.fQuat[0] + b.fQuat[1]*b.fQuat[1]
             + b.fQuat[2]*b.fQuat[2] + b.fQuat[3]*b.fQuat[3];
    if (q2 < 0.9f || q2 > 1.1f) { Clear(); return false; }

    vBones.push_back(std::move(b));

    size_t end = RecordEnd(pData, off);

    if (i + 1 == uiBoneCount) {
      // Last bone: whatever remains must be exactly one of the known blocks.
      size_t tail = stSize - end;
      bool valid = false;
      for (size_t t : TRAILERS) if (tail == t) { valid = true; break; }
      if (!valid) { Clear(); return false; }
      off = stSize;
      break;
    }

    size_t chosen = 0;
    for (size_t sep : SEPARATORS) {
      if (!PlausibleRecord(pData, stSize, end + sep, uiBoneCount)) continue;
      // Look one record further so a coincidental match cannot win.
      size_t nxt = RecordEnd(pData, end + sep);
      bool good = (nxt >= stSize);
      if (!good)
        for (size_t s2 : SEPARATORS)
          if (PlausibleRecord(pData, stSize, nxt + s2, uiBoneCount)) { good = true; break; }
      if (!good)
        for (size_t t : TRAILERS)
          if (stSize - nxt == t) { good = true; break; }
      if (good) { chosen = sep; break; }
    }
    if (chosen == 0) { Clear(); return false; }
    off = end + chosen;

    // The id of the bone that starts here. The gap is the optional block
    // followed by these 4 bytes, so reading at a fixed offset inside the
    // payload only lands on the id when no block is present -- which is why
    // rigs that mix the two used to lose exactly their blocked bones.
    uiPendingId = Read<uint32_t>(pData + off - 4);
  }

  if (off != stSize)               { Clear(); return false; }
  if (vBones.size() != uiBoneCount) { Clear(); return false; }
  return true;
}

int PabSkeleton::FindBoneById(uint32_t uiId) const {
  for (size_t i = 0; i < vBones.size(); i++)
    if (vBones[i].uiBoneId == uiId) return (int)i;
  return -1;
}

void PabSkeleton::ComputeWorldMatrices(std::vector<float> &vOut16) const {
  vOut16.assign(vBones.size() * 16, 0.0f);
  if (vBones.empty()) return;

  for (size_t i = 0; i < vBones.size(); i++) {
    const PabBone &b = vBones[i];

    float x = b.fQuat[0], y = b.fQuat[1], z = b.fQuat[2], w = b.fQuat[3];
    const float n = std::sqrt(x*x + y*y + z*z + w*w);
    if (n > 0.0f) { x /= n; y /= n; z /= n; w /= n; }

    // R is the quaternion's rotation for column vectors (v' = R*v), the same
    // form ComputeWorldPositions uses. FBX lays a matrix out for ROW vectors
    // instead -- basis vectors as rows, translation at 12..14 -- so row r must
    // hold the image of axis r, which is column r of R. Transposing here is
    // what keeps this in step with ComputeWorldPositions; dropping it makes
    // every rotated bone carry its own inverse.
    const float R[9] = {
      1 - 2*(y*y + z*z),     2*(x*y - z*w),     2*(x*z + y*w),
          2*(x*y + z*w), 1 - 2*(x*x + z*z),     2*(y*z - x*w),
          2*(x*z - y*w),     2*(y*z + x*w), 1 - 2*(x*x + y*y),
    };
    float L[16] = {};
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        L[r * 4 + c] = R[c * 3 + r] * b.fScale[r];
    L[12] = b.fTrans[0]; L[13] = b.fTrans[1]; L[14] = b.fTrans[2]; L[15] = 1.0f;

    float *W = &vOut16[i * 16];
    if (b.iParent < 0) { memcpy(W, L, sizeof(L)); continue; }

    // W = L * parentW, with row vectors, so the translation row picks up the
    // parent's rotation and offset the same way the basis rows do.
    const float *P = &vOut16[(size_t)b.iParent * 16];
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        float s = 0.0f;
        for (int k = 0; k < 4; k++) s += L[r * 4 + k] * P[k * 4 + c];
        W[r * 4 + c] = s;
      }
    }
  }
}

void PabSkeleton::ComputeWorldPositions(std::vector<float> &vOutXYZ) const {
  vOutXYZ.assign(vBones.size() * 3, 0.0f);
  if (vBones.empty()) return;

  // Rotation matrix per bone, accumulated down the chain. Parents precede
  // children, so one forward pass is enough.
  std::vector<float> rot(vBones.size() * 9, 0.0f);

  for (size_t i = 0; i < vBones.size(); i++) {
    const PabBone &b = vBones[i];

    float x = b.fQuat[0], y = b.fQuat[1], z = b.fQuat[2], w = b.fQuat[3];
    float n = std::sqrt(x*x + y*y + z*z + w*w);
    if (n > 0.0f) { x /= n; y /= n; z /= n; w /= n; }

    const float local[9] = {
      1 - 2*(y*y + z*z),     2*(x*y - z*w),     2*(x*z + y*w),
          2*(x*y + z*w), 1 - 2*(x*x + z*z),     2*(y*z - x*w),
          2*(x*z - y*w),     2*(y*z + x*w), 1 - 2*(x*x + y*y),
    };

    if (b.iParent < 0) {
      memcpy(&rot[i * 9], local, sizeof(local));
      for (int k = 0; k < 3; k++) vOutXYZ[i * 3 + k] = b.fTrans[k];
      continue;
    }

    const float *pr = &rot[(size_t)b.iParent * 9];
    float *cr = &rot[i * 9];
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        cr[r * 3 + c] = pr[r * 3 + 0] * local[0 * 3 + c]
                      + pr[r * 3 + 1] * local[1 * 3 + c]
                      + pr[r * 3 + 2] * local[2 * 3 + c];

    for (int r = 0; r < 3; r++) {
      float v = pr[r * 3 + 0] * b.fTrans[0]
              + pr[r * 3 + 1] * b.fTrans[1]
              + pr[r * 3 + 2] * b.fTrans[2];
      vOutXYZ[i * 3 + r] = vOutXYZ[(size_t)b.iParent * 3 + r] + v;
    }
  }
}

}  // namespace kukdh1
