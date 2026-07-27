#ifndef __SKELETONMATCH_H__
#define __SKELETONMATCH_H__

#include <string>
#include <vector>

namespace kukdh1 {

  // A .pac names no skeleton, so the right .pab has to be guessed from the
  // archive's layout. The archive lays them out by convention:
  //
  //   character/model/1_pc/11_pgw/armor/9_upperbody/pgw_00_ub_0001.pac
  //   character/model/1_pc/11_pgw/pgw_01.pab
  //
  // so the rig is usually the .pab sharing the mesh's prefix -- the text up to
  // the first underscore -- preferring <prefix>_01.pab. That is the first
  // candidate returned.
  //
  // The convention is not universal, though. Three ways it breaks, all
  // shipping:
  //
  //   12_cash/c0074_longtailedtit/m0358_longtailedtit_0001.pac -> c0074_0001.pab
  //   12_cash/c0030_itemstampoff_01/c0069_horsestampoff_01.pac -> c0030_01.pab
  //   8_housing/h0004_craftingcooking_0001/t0051_..._0001.pac  -> h0003_01.pab
  //
  // The prefix names a rig that exists elsewhere and is the wrong one, and the
  // right one may be beside the mesh or in a *sibling* folder -- the housing
  // cooking station is rigged by the armour station's skeleton. So the rest of
  // the candidates are ordered by how much of their path they share with the
  // mesh, nearest first.
  //
  // The caller is expected to keep the first candidate whose bone ids cover the
  // mesh's whole palette. Because the conventional pick is returned first, a
  // mesh that resolves today cannot change rigs.
  //
  // Returns indices into vPabPaths, best first, at most 32 of them. Paths are
  // compared case-insensitively and must use '/' separators, as the archive
  // index stores them.
  std::vector<size_t> RankSkeletonCandidates(const std::string &sPacPath,
                                             const std::vector<std::string> &vPabPaths);

  // The text up to the first underscore of a path's file name, lower-cased, or
  // empty when there is none. Animation clips are matched by the same rule --
  // applied to the chosen .pab, since clips belong to the rig rather than the
  // mesh.
  std::string ModelPrefix(const std::string &sPath);

}

#endif
