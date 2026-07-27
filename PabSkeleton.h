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
  //    0x12  uint32    the FIRST bone's id — see uiBoneId below; every other
  //                    bone's id sits just before its own record
  //
  //  Bone record @0x16, one per bone, in hierarchy order (a parent always
  //  precedes its children, so a single forward pass can compose transforms)
  //    uint8     name length
  //    char[]    name, CP949 (Korean) — converted to UTF-8 on load
  //    302 bytes payload:
  //      +0x000  int32     parent index, -1 for the root
  //      +0x004  float[16] matrix A  ─┐ bind / inverse-bind pairs; not needed
  //      +0x044  float[16] matrix B   │ for export because the SRT triple
  //      +0x084  float[16] matrix C   │ below reproduces the same transform
  //      +0x0C4  float[16] matrix D  ─┘
  //      +0x104  float[3]  scale
  //      +0x110  float[4]  rotation quaternion (x, y, z, w)
  //      +0x120  float[3]  translation
  //      +0x12C  uint8[2]  flags
  //    0/24/36/56/60 bytes  optional block, present per the flags
  //    uint32    the id of the NEXT bone (absent after the last record, which
  //              instead ends the file two bytes past its block)
  //
  //  The flags say a block is there but not how big, so the reader picks the
  //  size by validating the record that follows. The blocks hold no data a
  //  skeleton needs, so they are skipped -- but the id must be read at the far
  //  side of the block, not at a fixed offset from the payload. Reading it at
  //  a fixed offset silently returns padding for exactly the bones that carry
  //  a block, which is how ~10% of rigs lost part of their palette.
  //
  //  PabSkeleton.cpp folds the first two bytes after the payload into its
  //  BONE_PAYLOAD constant, so the separators it searches are these block
  //  sizes plus two.
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
    // palette entries against this field. A .paa animation track names its
    // target bone the same way.
    //
    // An id is stored immediately BEFORE the record it names: bone 0 takes the
    // header field at 0x12, and every later bone takes the four bytes ending
    // where its own record starts. Nothing follows the last record, because
    // there is no further bone to name.
    uint32_t    uiBoneId;
  };

  class PabSkeleton {
    public:
      std::vector<PabBone> vBones;

      uint32_t uiVersion;
      uint32_t uiFirstBoneId;   // header field at 0x12; vBones[0].uiBoneId

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
