#pragma once

#ifndef _PAB_SKELETON_H_
#define _PAB_SKELETON_H_

#include <cstdint>
#include <string>
#include <vector>

namespace kukdh1 {

  // ── PAB ("PAR " type 0x01) skeleton format ─────────────────────────────────
  //
  // Same container family as .pam; byte 0x04 is the type code (0x06 mesh,
  // 0x03 character mesh, 0x02 animation, 0x01 skeleton) and 0x05 the version.
  //
  //  Header
  //    0x00  char[4]   "PAR "
  //    0x04  uint8     0x01 (skeleton)
  //    0x05  uint8     version: 3 (player) or 4 (monster) — same layout
  //    0x06  uint8[10] 00 01 02 03 04 05 06 07 08 09 (constant descriptor)
  //    0x10  uint16    bone count
  //    0x12  uint32    skeleton id — the same value appears in the .paa
  //                    animation clips that drive this rig
  //
  //  Bone record @0x16, one per bone, in hierarchy order (a parent always
  //  precedes its children, so a single forward pass can compose transforms)
  //    uint8     name length
  //    char[]    name, CP949 (Korean) — converted to UTF-8 on load
  //    304 bytes payload:
  //      +0x000  int32     parent index, -1 for the root
  //      +0x004  float[16] matrix A  ─┐ bind / inverse-bind pairs; not needed
  //      +0x044  float[16] matrix B   │ for export because the SRT triple
  //      +0x084  float[16] matrix C   │ below reproduces the same transform
  //      +0x0C4  float[16] matrix D  ─┘
  //      +0x104  float[3]  scale
  //      +0x110  float[4]  rotation quaternion (x, y, z, w)
  //      +0x120  float[3]  translation
  //      +0x12C  uint8[4]  flags
  //
  //  Records are separated by 2 bytes, and a bone may carry one of several
  //  optional trailing blocks (24, 36, 56 or 60 bytes). The flag bytes at
  //  +0x12C signal that a block is present but do not distinguish 24 from 60,
  //  so the reader picks the separator by validating the following record.
  //  The blocks hold no data needed for a skeleton, so they are skipped.
  //
  //  The SRT triple is LOCAL — parent-relative. Verified by composing it down
  //  the hierarchy of a player rig: feet land at y≈15, pelvis y≈99 and head
  //  y≈151, i.e. a correctly proportioned human in centimetres, Y-up.
  //
  //  Validated against 271 skeletons spanning players, monsters and objects:
  //  270 parse to an exact end-of-file. Anything that does not is rejected
  //  rather than guessed at.

  struct PabBone {
    std::string sName;       // UTF-8
    int32_t     iParent;     // index into vBones, or -1 for the root
    float       fScale[3];
    float       fQuat[4];    // x, y, z, w
    float       fTrans[3];

    // Stable per-bone id. A .pac mesh does not store global bone indices --
    // its per-file palette lists these ids, and a vertex's 4 bone indices are
    // offsets into that palette. Resolving a skin therefore means matching
    // palette entries against this field.
    //
    // Sits at payload+302, so its last two bytes fall in what would otherwise
    // be the inter-record gap. The final bone in a file is stored two bytes
    // short, so its id is unavailable; bHasId says whether it was read.
    uint32_t    uiBoneId;
    bool        bHasId;
  };

  class PabSkeleton {
    public:
      std::vector<PabBone> vBones;

      uint32_t uiVersion;
      uint32_t uiSkeletonId;

      PabSkeleton();

      // Parses a decrypted .pab. Returns false if the file is not a recognised
      // PAR skeleton or fails any structural check.
      bool Load(const std::wstring &wsPath);
      bool LoadFromMemory(const uint8_t *pData, size_t stSize);

      bool IsEmpty() const;

      // Composes the local SRT chain into a world-space position per bone.
      // Sized to vBones; safe to call on an empty skeleton.
      void ComputeWorldPositions(std::vector<float> &vOutXYZ) const;

      // World transform per bone as 16 floats, laid out the way FBX stores a
      // matrix: rows of basis vectors with the translation at 12, 13, 14.
      // This is what a skin cluster's TransformLink needs.
      void ComputeWorldMatrices(std::vector<float> &vOut16) const;

      // Index of the bone carrying uiBoneId, or -1. Used to resolve a .pac
      // bone palette onto this skeleton.
      int FindBoneById(uint32_t uiId) const;

    private:
      void Clear();
  };

}

#endif
