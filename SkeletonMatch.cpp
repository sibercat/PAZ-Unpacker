#include "SkeletonMatch.h"

#include <algorithm>
#include <cctype>

namespace kukdh1 {

namespace {

  std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
  }

  std::string BaseName(const std::string &s) {
    size_t slash = s.rfind('/');
    return (slash != std::string::npos) ? s.substr(slash + 1) : s;
  }

  // Leading path components two files share. The mesh's own folder scores
  // highest, then siblings under the same group.
  size_t SharedComponents(const std::string &a, const std::string &b) {
    size_t n = 0, i = 0, j = 0;
    for (;;) {
      size_t p = a.find('/', i), q = b.find('/', j);
      if (p == std::string::npos || q == std::string::npos) break;
      if (a.compare(i, p - i, b, j, q - j) != 0) break;
      n++;
      i = p + 1;
      j = q + 1;
    }
    return n;
  }

  // Two components means only "character/model" in common, which is no
  // relation; ranking the whole archive on that would be guessing. The cap
  // bounds how many skeletons a mesh that resolves nowhere makes the caller
  // extract.
  constexpr size_t kMinShared     = 3;
  constexpr size_t kMaxCandidates = 32;

}  // namespace

std::string ModelPrefix(const std::string &sPath) {
  const std::string base = BaseName(sPath);
  size_t us = base.find('_');
  if (us == std::string::npos || us == 0) return std::string();
  return Lower(base.substr(0, us));
}

std::vector<size_t> RankSkeletonCandidates(const std::string &sPacPath,
                                           const std::vector<std::string> &vPabPaths) {
  std::vector<size_t> out;
  auto Add = [&out](size_t i) {
    if (std::find(out.begin(), out.end(), i) == out.end()) out.push_back(i);
  };

  const std::string prefix = ModelPrefix(sPacPath);
  const std::string lp     = Lower(sPacPath);

  std::vector<std::string> lowerBase(vPabPaths.size());
  for (size_t i = 0; i < vPabPaths.size(); i++)
    lowerBase[i] = Lower(BaseName(vPabPaths[i]));

  // The conventional pick, which is right for the overwhelming majority and so
  // is tried first: a mesh that resolves today keeps the rig it already has.
  if (!prefix.empty()) {
    size_t best = vPabPaths.size();
    for (size_t i = 0; i < vPabPaths.size(); i++) {
      const std::string &b = lowerBase[i];
      if (b.compare(0, prefix.size(), prefix) != 0) continue;
      if (b.size() <= prefix.size() || b[prefix.size()] != '_') continue;
      if (b == prefix + "_01.pab") { best = i; break; }   // the base rig
      if (best == vPabPaths.size()) best = i;
    }
    if (best != vPabPaths.size()) Add(best);
  }

  std::vector<std::pair<size_t, size_t>> ranked;   // shared components, index
  for (size_t i = 0; i < vPabPaths.size(); i++) {
    size_t s = SharedComponents(lp, Lower(vPabPaths[i]));
    if (s >= kMinShared) ranked.emplace_back(s, i);
  }
  std::stable_sort(ranked.begin(), ranked.end(),
                   [](const std::pair<size_t, size_t> &a,
                      const std::pair<size_t, size_t> &b) { return a.first > b.first; });

  // Within the nearest folders the prefix still breaks ties.
  if (!prefix.empty()) {
    for (const auto &kv : ranked)
      if (lowerBase[kv.second] == prefix + "_01.pab") { Add(kv.second); break; }
  }

  for (const auto &kv : ranked) {
    if (out.size() >= kMaxCandidates) break;
    Add(kv.second);
  }
  return out;
}

}  // namespace kukdh1
