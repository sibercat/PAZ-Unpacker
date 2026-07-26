#include "PskExport.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>

namespace kukdh1 {

  namespace {

    // ── Chunk plumbing ───────────────────────────────────────────────────────

    void WriteChunkHeader(std::ostream &f, const char *pName,
                          uint32_t uiTypeFlag, uint32_t uiDataSize,
                          uint32_t uiDataCount) {
      char name[20] = {};
      // The id is a fixed 20-byte field; anything past it is a format error
      // rather than something to truncate silently.
      assert(strlen(pName) < sizeof(name));
      strncpy_s(name, sizeof(name), pName, sizeof(name) - 1);
      f.write(name, sizeof(name));
      f.write(reinterpret_cast<const char *>(&uiTypeFlag),  4);
      f.write(reinterpret_cast<const char *>(&uiDataSize),  4);
      f.write(reinterpret_cast<const char *>(&uiDataCount), 4);
    }

    template <typename T>
    void Put(std::ostream &f, const T &v) {
      f.write(reinterpret_cast<const char *>(&v), sizeof(T));
    }

    void PutName(std::ostream &f, const std::string &s, size_t n) {
      std::vector<char> buf(n, 0);
      const size_t len = (s.size() < n - 1) ? s.size() : n - 1;
      memcpy(buf.data(), s.data(), len);
      f.write(buf.data(), (std::streamsize)n);
    }

    // ── Bone records ─────────────────────────────────────────────────────────

    // REFSKELT and BONENAMES share this 120-byte record.
    void WriteBone(std::ostream &f, const PabBone &b, size_t stChildCount,
                   bool bRoot) {
      PutName(f, b.sName, 64);
      Put<uint32_t>(f, 0);                                  // flags
      Put<int32_t>(f, (int32_t)stChildCount);
      Put<int32_t>(f, b.iParent < 0 ? 0 : b.iParent);       // root points at itself

      // Rotations are stored conjugated for every bone EXCEPT the root.
      //
      // The reader composes rotation as (quat as matrix3), and MaxScript's
      // conversion is itself a conjugation, so a stored quaternion comes out
      // as its own conjugate. ActorX then conjugates the root a second time,
      // which cancels for that one bone. Storing it the other way round sends
      // each bone's local offset off along the wrong axis, and the rig fans out
      // in a straight line instead of following the chain.
      float x = b.fQuat[0], y = b.fQuat[1], z = b.fQuat[2], w = b.fQuat[3];
      const float n = std::sqrt(x*x + y*y + z*z + w*w);
      if (n > 0.0f) { x /= n; y /= n; z /= n; w /= n; }
      if (!bRoot) { x = -x; y = -y; z = -z; }

      Put<float>(f, x); Put<float>(f, y); Put<float>(f, z); Put<float>(f, w);
      Put<float>(f, b.fTrans[0]);
      Put<float>(f, b.fTrans[1]);
      Put<float>(f, b.fTrans[2]);
      Put<float>(f, 0.0f);                                  // length, cosmetic
      Put<float>(f, 1.0f); Put<float>(f, 1.0f); Put<float>(f, 1.0f);   // size
    }

    void WriteBoneChunk(std::ostream &f, const char *pChunk,
                        const PabSkeleton &skel) {
      std::vector<size_t> children(skel.vBones.size(), 0);
      for (const auto &b : skel.vBones)
        if (b.iParent >= 0) children[(size_t)b.iParent]++;

      WriteChunkHeader(f, pChunk, 0, 120, (uint32_t)skel.vBones.size());
      for (size_t i = 0; i < skel.vBones.size(); i++)
        WriteBone(f, skel.vBones[i], children[i], skel.vBones[i].iParent < 0);
    }

  }  // namespace

  bool ExportPsk(const PacModel &model, const PabSkeleton &skel,
                 const std::wstring &wsPath, std::wstring &wsError) {
    wsError.clear();
    if (model.IsEmpty())  { wsError = L"The model has no geometry to export."; return false; }
    if (skel.IsEmpty())   { wsError = L"The skeleton has no bones."; return false; }

    // Resolve the palette first: binding to the wrong bones is worse than
    // refusing, and the caller can explain a mismatch.
    std::vector<int> paletteToBone(model.vBonePalette.size(), -1);
    size_t unresolved = 0;
    for (size_t i = 0; i < model.vBonePalette.size(); i++) {
      paletteToBone[i] = skel.FindBoneById(model.vBonePalette[i]);
      if (paletteToBone[i] < 0) unresolved++;
    }
    if (unresolved) {
      wchar_t buf[192];
      swprintf_s(buf,
        L"%zu of %zu bone palette entries are not in this skeleton.\r\n"
        L"The mesh and skeleton probably belong to different characters.",
        unresolved, model.vBonePalette.size());
      wsError = buf;
      return false;
    }

    std::ofstream f(wsPath, std::ios::binary);
    if (!f) { wsError = L"Could not open the .psk file for writing."; return false; }

    // A wedge is a per-corner vertex: position index plus UV plus material. The
    // .pac already stores vertices that way, so submeshes concatenate directly
    // and each wedge maps one-to-one onto a source vertex.
    struct Wedge { uint32_t point; float u, v; uint8_t mat; };
    std::vector<float>    points;      // xyz triples
    std::vector<Wedge>    wedges;
    std::vector<uint32_t> faces;       // wedge triples
    std::vector<uint8_t>  faceMat;

    // Weights, gathered per point rather than per wedge.
    struct Influence { float weight; uint32_t point, bone; };
    std::vector<Influence> influences;

    uint32_t base = 0;
    for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
      const PacSubmesh &sm = model.vSubmeshes[s];

      for (const auto &v : sm.vVertices) {
        const uint32_t point = (uint32_t)(points.size() / 3);
        points.push_back(v.x); points.push_back(v.y); points.push_back(v.z);

        Wedge w;
        w.point = point;
        w.u     = v.u;
        w.v     = v.v;
        w.mat   = (uint8_t)s;
        wedges.push_back(w);

        // Quantised weights sum to about 255; normalise so the total is 1.
        float total = 0.0f;
        for (int k = 0; k < 4; k++) total += (float)v.weight[k];
        if (total <= 0.0f) total = 1.0f;
        for (int k = 0; k < 4; k++) {
          if (!v.weight[k]) continue;
          const int slot = v.bone[k];
          if (slot < 0 || (size_t)slot >= paletteToBone.size()) continue;
          Influence inf;
          inf.weight = (float)v.weight[k] / total;
          inf.point  = point;
          inf.bone   = (uint32_t)paletteToBone[slot];
          influences.push_back(inf);
        }
      }

      for (size_t i = 0; i + 2 < sm.vIndices.size(); i += 3) {
        // Reverse the winding: the .pac's order renders inside-out once the
        // importer has applied its own handedness.
        faces.push_back(base + sm.vIndices[i + 2]);
        faces.push_back(base + sm.vIndices[i + 1]);
        faces.push_back(base + sm.vIndices[i + 0]);
        faceMat.push_back((uint8_t)s);
      }
      base += (uint32_t)sm.vVertices.size();
    }

    const size_t nPoints = points.size() / 3;
    const size_t nWedges = wedges.size();
    const size_t nFaces  = faceMat.size();

    WriteChunkHeader(f, "ACTRHEAD", 1999801, 0, 0);

    WriteChunkHeader(f, "PNTS0000", 0, 12, (uint32_t)nPoints);
    for (size_t i = 0; i < nPoints; i++) {
      Put<float>(f, points[i * 3 + 0]);
      Put<float>(f, points[i * 3 + 1]);
      Put<float>(f, points[i * 3 + 2]);
    }

    // Past 65536 wedges the importer switches to 32-bit point indices, keyed
    // off the wedge count alone, so the record must match.
    const bool bWide = nWedges > 65536;
    WriteChunkHeader(f, "VTXW0000", 0, bWide ? 16 : 16, (uint32_t)nWedges);
    for (const auto &w : wedges) {
      if (bWide) {
        Put<uint32_t>(f, w.point);
      } else {
        Put<uint16_t>(f, (uint16_t)w.point);
        Put<uint16_t>(f, 0);
      }
      Put<float>(f, w.u);
      Put<float>(f, w.v);
      Put<uint8_t>(f, w.mat);
      Put<uint8_t>(f, 0);
      Put<uint16_t>(f, 0);
    }

    // Wedge indices are 16-bit in FACE0000; anything larger needs FACE3200.
    const bool bFace32 = nWedges > 65536;
    WriteChunkHeader(f, bFace32 ? "FACE3200" : "FACE0000", 0,
                     bFace32 ? 18 : 12, (uint32_t)nFaces);
    for (size_t i = 0; i < nFaces; i++) {
      if (bFace32) {
        Put<uint32_t>(f, faces[i * 3 + 0]);
        Put<uint32_t>(f, faces[i * 3 + 1]);
        Put<uint32_t>(f, faces[i * 3 + 2]);
      } else {
        Put<uint16_t>(f, (uint16_t)faces[i * 3 + 0]);
        Put<uint16_t>(f, (uint16_t)faces[i * 3 + 1]);
        Put<uint16_t>(f, (uint16_t)faces[i * 3 + 2]);
      }
      Put<uint8_t>(f, faceMat[i]);
      Put<uint8_t>(f, 0);
      Put<uint32_t>(f, 1);                                  // smoothing groups
    }

    WriteChunkHeader(f, "MATT0000", 0, 88, (uint32_t)model.vSubmeshes.size());
    for (const auto &sm : model.vSubmeshes) {
      PutName(f, sm.sName, 64);
      Put<int32_t>(f, 0); Put<int32_t>(f, 0); Put<int32_t>(f, 0);
      Put<int32_t>(f, 0); Put<int32_t>(f, 0); Put<int32_t>(f, 0);
    }

    WriteBoneChunk(f, "REFSKELT", skel);

    WriteChunkHeader(f, "RAWWEIGHTS", 0, 12, (uint32_t)influences.size());
    for (const auto &inf : influences) {
      Put<float>(f, inf.weight);
      Put<uint32_t>(f, inf.point);
      Put<uint32_t>(f, inf.bone);
    }

    if (!f) { wsError = L"Failed while writing the .psk file."; return false; }
    return true;
  }

  namespace {

    // Linear sample of a vector track at a time in milliseconds. The keys are
    // sorted, and clips are short, so a scan is cheaper than anything cleverer.
    void SampleVec(const std::vector<PaaVecKey> &keys, uint32_t ms,
                   const float fallback[3], float out[3]) {
      if (keys.empty()) { for (int i = 0; i < 3; i++) out[i] = fallback[i]; return; }
      if (ms <= keys.front().uiTimeMs) { for (int i = 0; i < 3; i++) out[i] = keys.front().v[i]; return; }
      if (ms >= keys.back().uiTimeMs)  { for (int i = 0; i < 3; i++) out[i] = keys.back().v[i];  return; }

      for (size_t i = 0; i + 1 < keys.size(); i++) {
        const PaaVecKey &a = keys[i], &b = keys[i + 1];
        if (ms < a.uiTimeMs || ms > b.uiTimeMs) continue;
        const uint32_t span = b.uiTimeMs - a.uiTimeMs;
        const float t = span ? (float)(ms - a.uiTimeMs) / (float)span : 0.0f;
        for (int k = 0; k < 3; k++) out[k] = a.v[k] + (b.v[k] - a.v[k]) * t;
        return;
      }
      for (int i = 0; i < 3; i++) out[i] = keys.back().v[i];
    }

    // Same for rotations, along the shorter arc.
    void SampleQuat(const std::vector<PaaQuatKey> &keys, uint32_t ms,
                    const float fallback[4], float out[4]) {
      auto copy = [&](const float *q) { for (int i = 0; i < 4; i++) out[i] = q[i]; };
      if (keys.empty()) { copy(fallback); return; }
      if (ms <= keys.front().uiTimeMs) { copy(keys.front().q); return; }
      if (ms >= keys.back().uiTimeMs)  { copy(keys.back().q);  return; }

      for (size_t i = 0; i + 1 < keys.size(); i++) {
        const PaaQuatKey &a = keys[i], &b = keys[i + 1];
        if (ms < a.uiTimeMs || ms > b.uiTimeMs) continue;
        const uint32_t span = b.uiTimeMs - a.uiTimeMs;
        const float t = span ? (float)(ms - a.uiTimeMs) / (float)span : 0.0f;

        float dot = 0.0f;
        for (int k = 0; k < 4; k++) dot += a.q[k] * b.q[k];
        const float sign = (dot < 0.0f) ? -1.0f : 1.0f;   // shorter arc
        float len = 0.0f;
        for (int k = 0; k < 4; k++) {
          out[k] = a.q[k] + (b.q[k] * sign - a.q[k]) * t;
          len += out[k] * out[k];
        }
        len = std::sqrt(len);
        if (len > 0.0f) for (int k = 0; k < 4; k++) out[k] /= len;
        return;
      }
      copy(keys.back().q);
    }

  }  // namespace

  bool ExportPsa(const PabSkeleton &skel, const std::vector<PskAnimClip> &vClips,
                 const std::wstring &wsPath, std::wstring &wsError) {
    wsError.clear();
    if (skel.IsEmpty()) { wsError = L"The skeleton has no bones."; return false; }

    // Only clips that drive this rig are worth writing; one that matches
    // nothing would export as a silent rest pose.
    struct Clip { const PskAnimClip *pClip; uint32_t uiFrames, uiMs; };
    std::vector<Clip> keep;
    const float kRate = 30.0f;

    for (const auto &c : vClips) {
      if (!c.pAnim || c.pAnim->IsEmpty()) continue;
      size_t hit = 0;
      for (const auto &t : c.pAnim->vTracks)
        if (skel.FindBoneById(t.uiBoneId) >= 0) hit++;
      if (!hit) continue;

      const uint32_t ms = c.pAnim->DurationMs();
      uint32_t frames = (uint32_t)(((double)ms / 1000.0) * kRate + 0.5) + 1;
      if (frames < 2) frames = 2;
      keep.push_back({ &c, frames, ms });
    }
    if (keep.empty()) {
      wsError = L"None of these clips drive this skeleton.";
      return false;
    }

    std::ofstream f(wsPath, std::ios::binary);
    if (!f) { wsError = L"Could not open the .psa file for writing."; return false; }

    WriteChunkHeader(f, "ANIMHEAD", 1999801, 0, 0);
    WriteBoneChunk(f, "BONENAMES", skel);

    const size_t nBones = skel.vBones.size();
    uint32_t firstFrame = 0;
    WriteChunkHeader(f, "ANIMINFO", 0, 160, (uint32_t)keep.size());
    for (const auto &c : keep) {
      PutName(f, c.pClip->sName, 64);
      PutName(f, "None", 64);                               // group
      Put<int32_t>(f, (int32_t)nBones);
      Put<int32_t>(f, 0);                                   // root include
      Put<int32_t>(f, 0);                                   // key compression
      Put<int32_t>(f, 0);                                   // key quotum
      Put<float>(f, 0.0f);                                  // key reduction
      Put<float>(f, (float)c.uiMs / 1000.0f);               // track time
      Put<float>(f, kRate);
      Put<int32_t>(f, 0);                                   // start bone
      Put<int32_t>(f, (int32_t)firstFrame);
      Put<int32_t>(f, (int32_t)c.uiFrames);
      firstFrame += c.uiFrames;
    }

    // Keys are stored frame by frame, bones in skeleton order inside each.
    size_t totalKeys = 0;
    for (const auto &c : keep) totalKeys += (size_t)c.uiFrames * nBones;
    WriteChunkHeader(f, "ANIMKEYS", 0, 32, (uint32_t)totalKeys);

    for (const auto &c : keep) {
      // Bone index -> track, so each frame is a lookup rather than a search.
      std::vector<const PaaTrack *> track(nBones, nullptr);
      for (const auto &t : c.pClip->pAnim->vTracks) {
        const int b = skel.FindBoneById(t.uiBoneId);
        if (b >= 0) track[(size_t)b] = &t;
      }

      for (uint32_t frame = 0; frame < c.uiFrames; frame++) {
        const uint32_t ms = (c.uiFrames > 1)
          ? (uint32_t)((double)c.uiMs * frame / (c.uiFrames - 1)) : 0;

        for (size_t b = 0; b < nBones; b++) {
          const PabBone &bone = skel.vBones[b];
          float pos[3], quat[4];

          if (track[b]) {
            SampleVec(track[b]->vPosition, ms, bone.fTrans, pos);
            SampleQuat(track[b]->vRotation, ms, bone.fQuat, quat);
          } else {
            for (int k = 0; k < 3; k++) pos[k] = bone.fTrans[k];
            for (int k = 0; k < 4; k++) quat[k] = bone.fQuat[k];
          }

          float len = 0.0f;
          for (int k = 0; k < 4; k++) len += quat[k] * quat[k];
          len = std::sqrt(len);
          if (len > 0.0f) for (int k = 0; k < 4; k++) quat[k] /= len;
          // Same convention as the bone table: conjugated except at the root.
          if (bone.iParent >= 0) { quat[0] = -quat[0]; quat[1] = -quat[1]; quat[2] = -quat[2]; }

          Put<float>(f, pos[0]); Put<float>(f, pos[1]); Put<float>(f, pos[2]);
          Put<float>(f, quat[0]); Put<float>(f, quat[1]);
          Put<float>(f, quat[2]); Put<float>(f, quat[3]);
          Put<float>(f, 1.0f / kRate);                      // time, in frames
        }
      }
    }

    if (!f) { wsError = L"Failed while writing the .psa file."; return false; }
    return true;
  }

}
