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

  // A creature or mount motion folder holds 73-178 clips and is read whole;
  // a player class's holds 355-2,078, too many to extract per export.
  constexpr size_t kOwnRigScanAll = 300;

}  // namespace

std::string ModelPrefix(const std::string &sPath) {
  const std::string base = BaseName(sPath);
  size_t us = base.find('_');
  if (us == std::string::npos || us == 0) return std::string();
  return Lower(base.substr(0, us));
}

std::string ItemWord(const std::string &sPacPath) {
  std::string base = Lower(BaseName(sPacPath));
  size_t dot = base.rfind('.');
  if (dot != std::string::npos) base.resize(dot);

  std::vector<std::string> parts;
  for (size_t i = 0;;) {
    size_t us = base.find('_', i);
    parts.push_back(base.substr(i, us == std::string::npos ? std::string::npos : us - i));
    if (us == std::string::npos) break;
    i = us + 1;
  }

  // Skip the class prefix, then a short code after it if there is one.
  size_t k = 1;
  if (k < parts.size() && parts[k].size() <= 2) k++;
  for (; k < parts.size(); k++) {
    const std::string &w = parts[k];
    if (!w.empty() && !std::all_of(w.begin(), w.end(),
                                   [](unsigned char c) { return std::isdigit(c) != 0; }))
      return w;
  }
  return std::string();
}

std::string MotionRoot(const std::string &sPacPath) {
  static const std::string kModel = "character/model/";
  std::string lp = Lower(sPacPath);
  if (lp.compare(0, kModel.size(), kModel) != 0) return std::string();
  size_t a = lp.find('/', kModel.size());
  size_t b = (a == std::string::npos) ? a : lp.find('/', a + 1);
  if (b == std::string::npos) return std::string();
  return "character/motion/" + lp.substr(kModel.size(), b + 1 - kModel.size());
}

std::vector<size_t> OwnRigClipCandidates(const std::string &sPacPath,
                                         const std::vector<std::string> &vClipPaths) {
  std::vector<size_t> out;
  const std::string root = MotionRoot(sPacPath);
  if (root.empty()) return out;

  std::vector<size_t> inRoot;
  for (size_t i = 0; i < vClipPaths.size(); i++) {
    const std::string lp = Lower(vClipPaths[i]);
    if (lp.size() < 5 || lp.compare(lp.size() - 4, 4, ".paa") != 0) continue;
    if (lp.compare(0, root.size(), root) == 0) inRoot.push_back(i);
  }
  if (inRoot.size() <= kOwnRigScanAll) return inRoot;

  const std::string word = ItemWord(sPacPath);
  if (word.empty()) return out;
  const std::string needle = "_" + word + "_";
  for (size_t i : inRoot)
    if (("_" + Lower(BaseName(vClipPaths[i]))).find(needle) != std::string::npos)
      out.push_back(i);
  return out;
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
