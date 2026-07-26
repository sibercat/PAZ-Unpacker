#pragma once

#ifndef _PAA_ANIMATION_H_
#define _PAA_ANIMATION_H_

#include <cstdint>
#include <string>
#include <vector>

namespace kukdh1 {

  // ── PAA ("PAR " type 0x02) animation clip ──────────────────────────────────
  //
  //  Header, 0x1A bytes
  //    0x00  char[4]   "PAR "
  //    0x04  uint8     0x02 (animation)
  //    0x05  uint8     version, 2 observed throughout
  //    0x06  uint8[10] 00 01 02 03 04 05 06 07 08 09 (constant)
  //    0x10  uint16    animated bone count
  //    0x12  uint16    unknown
  //    0x14  half      unknown; NOT the clip length -- it does not track the
  //                    key times at all, so duration is taken from the keys
  //    0x16  uint32    total size of the key data
  //
  //  Then one block per bone, laid out consecutively:
  //    uint32  bone id -- matches PabBone::uiBoneId
  //    uint16  scale key count;  each key: uint16 time + half[3]   (8 bytes)
  //    uint16  rotation key count; each key: uint16 time + half[4] (10 bytes)
  //    uint16  position key count; each key: uint16 time + half[3] (8 bytes)
  //
  //  The fixed 4+2+2+2 = 10 bytes per bone are exactly the 10*count term in
  //  the identity filesize == 0x1A + 10*boneCount + keyDataSize, which holds
  //  on every file sampled.
  //
  //  Times are milliseconds. Key spacing is overwhelmingly 33 or 34 ms, i.e.
  //  30 fps, and clip lengths land on round values like 3333 and 10000 ms.
  //
  //  Validated on 391 clips: every one walks its bone blocks to an exact
  //  end-of-file, and all 2,053,038 rotation keys decode to unit quaternions.

  struct PaaVecKey {
    uint16_t uiTimeMs;
    float    v[3];
  };

  struct PaaQuatKey {
    uint16_t uiTimeMs;
    float    q[4];        // x, y, z, w
  };

  struct PaaTrack {
    uint32_t                uiBoneId;    // matches PabBone::uiBoneId
    std::vector<PaaVecKey>  vScale;
    std::vector<PaaQuatKey> vRotation;
    std::vector<PaaVecKey>  vPosition;
  };

  class PaaAnimation {
    public:
      std::vector<PaaTrack> vTracks;
      uint32_t              uiVersion;

      PaaAnimation();

      bool Load(const std::wstring &wsPath);
      bool LoadFromMemory(const uint8_t *pData, size_t stSize);

      bool     IsEmpty() const;
      // Longest key time in the clip, in milliseconds.
      uint32_t DurationMs() const;
      size_t   TotalKeys() const;

    private:
      void Clear();
  };

}

#endif
