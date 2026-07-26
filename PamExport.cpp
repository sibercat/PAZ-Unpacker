#include "PamExport.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace kukdh1 {

  namespace {

    // Strips any directory part and the extension, leaving a bare name usable
    // as an object / material identifier.
    std::string StemOf(const std::wstring &wsPath) {
      std::filesystem::path p(wsPath);
      std::string s = p.stem().string();
      if (s.empty()) s = "model";
      // Material names must not contain whitespace in OBJ.
      for (char &c : s) if (c == ' ' || c == '\t') c = '_';
      return s;
    }

    std::string SanitiseMaterialName(const std::string &tex, size_t index) {
      std::string base = tex;
      size_t slash = base.find_last_of("/\\");
      if (slash != std::string::npos) base = base.substr(slash + 1);
      size_t dot = base.find_last_of('.');
      if (dot != std::string::npos) base = base.substr(0, dot);
      for (char &c : base) if (c == ' ' || c == '\t' || c == '#') c = '_';
      if (base.empty()) base = "material_" + std::to_string(index);
      return base;
    }

    // ── Wavefront OBJ ────────────────────────────────────────────────────────
    //
    // Vertex positions are written unchanged: the archive already stores Y-up,
    // which is what OBJ consumers expect. The V texture coordinate is flipped
    // because OBJ places the origin at the bottom-left while the source images
    // are top-left.
    bool WriteObj(const PamModel &model, const std::wstring &wsPath,
                  const PamTextureFileList &vTextureFiles, std::wstring &wsError) {
      std::filesystem::path objPath(wsPath);
      std::filesystem::path mtlPath = objPath;
      mtlPath.replace_extension(L".mtl");

      std::ofstream f(objPath, std::ios::binary);
      if (!f) { wsError = L"Could not open the .obj file for writing."; return false; }

      const std::string stem = StemOf(wsPath);

      f << "# Exported by PAZ Unpacker\n";
      f << "# Source: Black Desert Online .pam (PAR v" << model.uiVersion << ")\n";
      f << "# " << model.vVertices.size() << " vertices, "
        << (model.vIndices.size() / 3) << " triangles, "
        << model.vSubmeshes.size() << " material(s)\n";
      f << "mtllib " << mtlPath.filename().string() << "\n";
      f << "o " << stem << "\n";

      f.setf(std::ios::fixed);
      f.precision(6);

      for (const auto &v : model.vVertices)
        f << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
      for (const auto &v : model.vVertices)
        f << "vt " << v.u << ' ' << (1.0f - v.v) << '\n';
      for (const auto &v : model.vVertices)
        f << "vn " << v.nx << ' ' << v.ny << ' ' << v.nz << '\n';

      for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
        const PamSubmesh &sm = model.vSubmeshes[s];
        const std::string mat = SanitiseMaterialName(sm.sTexture, s);

        f << "g " << stem << "_" << s << "\n";
        f << "usemtl " << mat << "\n";

        const uint32_t last = sm.uiBaseIndex + sm.uiIndexCount;
        for (uint32_t i = sm.uiBaseIndex; i + 2 < last; i += 3) {
          // OBJ indices are 1-based and shared across v / vt / vn.
          const uint32_t a = model.vIndices[i + 0] + 1;
          const uint32_t b = model.vIndices[i + 1] + 1;
          const uint32_t c = model.vIndices[i + 2] + 1;
          f << "f " << a << '/' << a << '/' << a
            << ' '  << b << '/' << b << '/' << b
            << ' '  << c << '/' << c << '/' << c << '\n';
        }
      }
      if (!f) { wsError = L"Failed while writing the .obj file."; return false; }
      f.close();

      // ── Companion material library ─────────────────────────────────────────
      std::ofstream m(mtlPath, std::ios::binary);
      if (!m) { wsError = L"Could not open the .mtl file for writing."; return false; }

      m << "# Exported by PAZ Unpacker\n";
      m.setf(std::ios::fixed);
      m.precision(4);

      for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
        const std::string mat = SanitiseMaterialName(model.vSubmeshes[s].sTexture, s);
        m << "\nnewmtl " << mat << "\n";
        m << "Ka 1.0000 1.0000 1.0000\n";
        m << "Kd 1.0000 1.0000 1.0000\n";
        m << "Ks 0.0000 0.0000 0.0000\n";
        m << "d 1.0000\n";
        m << "illum 2\n";
        if (s < vTextureFiles.size() && !vTextureFiles[s].empty()) {
          m << "map_Kd " << vTextureFiles[s] << "\n";
          // Same image supplies the cutout mask for foliage and decals.
          m << "map_d "  << vTextureFiles[s] << "\n";
        }
      }
      if (!m) { wsError = L"Failed while writing the .mtl file."; return false; }
      return true;
    }

    // ── Binary FBX 7.4 ───────────────────────────────────────────────────────
    //
    // FBX has no public specification; this follows the community-documented
    // binary layout, which is what Blender's importer reads (it rejects the
    // ASCII variant, so text is not an option).
    //
    // The file is a tree of nodes:
    //   uint32 endOffset       absolute file offset just past this node
    //   uint32 numProperties
    //   uint32 propertyListLen
    //   uint8  nameLen, char[] name
    //   properties...
    //   child nodes... followed by a 13-byte null record, but ONLY when the
    //                  node actually has children
    //
    // The tree is built in memory first so every node's size — and therefore
    // every absolute offset — is known before anything is written.

    struct FbxNode {
      std::string name;
      std::string props;        // already-serialised property bytes
      uint32_t    propCount = 0;
      std::vector<FbxNode> children;

      explicit FbxNode(std::string n = std::string()) : name(std::move(n)) {}

      FbxNode &Child(const std::string &n) {
        children.emplace_back(n);
        return children.back();
      }

      // ── Property writers ───────────────────────────────────────────────────
      void Put(const void *p, size_t n) {
        props.append(reinterpret_cast<const char *>(p), n);
      }
      void I16(int16_t v)  { props.push_back('Y'); Put(&v, 2); propCount++; }
      void Bool(bool v)    { props.push_back('C'); char c = v ? 1 : 0; Put(&c, 1); propCount++; }
      void I32(int32_t v)  { props.push_back('I'); Put(&v, 4); propCount++; }
      void F32(float v)    { props.push_back('F'); Put(&v, 4); propCount++; }
      void F64(double v)   { props.push_back('D'); Put(&v, 8); propCount++; }
      void I64(int64_t v)  { props.push_back('L'); Put(&v, 8); propCount++; }

      void Str(const std::string &s) {
        props.push_back('S');
        uint32_t n = (uint32_t)s.size();
        Put(&n, 4);
        props.append(s);
        propCount++;
      }
      // Object names are "Name\0\x01Class" in the binary form.
      void NameClass(const std::string &n, const std::string &cls) {
        std::string s = n;
        s.push_back('\0');
        s.push_back('\x01');
        s += cls;
        Str(s);
      }

      template <typename T>
      void Array(char code, const std::vector<T> &v) {
        props.push_back(code);
        uint32_t count    = (uint32_t)v.size();
        uint32_t encoding = 0;                 // 0 = uncompressed
        uint32_t bytes    = count * (uint32_t)sizeof(T);
        Put(&count, 4);
        Put(&encoding, 4);
        Put(&bytes, 4);
        if (count) props.append(reinterpret_cast<const char *>(v.data()), bytes);
        propCount++;
      }
      void ArrF64(const std::vector<double> &v)  { Array('d', v); }
      void ArrI32(const std::vector<int32_t> &v) { Array('i', v); }
      void ArrF32(const std::vector<float> &v)   { Array('f', v); }
      void ArrI64(const std::vector<int64_t> &v) { Array('l', v); }

      // A Properties70 entry: P: name, type, subtype, flags, value(s)
      void P(const std::string &n, const std::string &type,
             const std::string &sub, const std::string &flags) {
        Str(n); Str(type); Str(sub); Str(flags);
      }

      // ── Sizing and emission ────────────────────────────────────────────────
      size_t Size() const {
        size_t total = 4 + 4 + 4 + 1 + name.size() + props.size();
        for (const auto &c : children) total += c.Size();
        if (!children.empty()) total += 13;    // null record ends the child list
        return total;
      }

      void Write(std::ofstream &f, size_t offset) const {
        const uint32_t endOffset = (uint32_t)(offset + Size());
        const uint32_t plen      = (uint32_t)props.size();
        const uint8_t  nlen      = (uint8_t)name.size();

        f.write(reinterpret_cast<const char *>(&endOffset), 4);
        f.write(reinterpret_cast<const char *>(&propCount), 4);
        f.write(reinterpret_cast<const char *>(&plen), 4);
        f.write(reinterpret_cast<const char *>(&nlen), 1);
        f.write(name.data(), nlen);
        f.write(props.data(), plen);

        size_t childOffset = offset + 4 + 4 + 4 + 1 + name.size() + props.size();
        for (const auto &c : children) {
          c.Write(f, childOffset);
          childOffset += c.Size();
        }
        if (!children.empty()) {
          char nullRec[13] = {};
          f.write(nullRec, 13);
        }
      }
    };

    // ── Shared FBX emit ──────────────────────────────────────────────────
    // Writes the magic, the node tree and the footer. The footer id is a
    // checksum partner of FileId and CreationTime in BuildFbxPreamble, so
    // both functions must be used together or the Autodesk SDK rejects the
    // file with "Cannot open FBX file".
    void EmitFbxFile(std::ofstream &f, const std::vector<FbxNode> &roots) {

      // ── Emit ─────────────────────────────────────────────────────────────
      const char magic[23] = {
        'K','a','y','d','a','r','a',' ','F','B','X',' ','B','i','n','a','r','y',
        ' ',' ', '\0', '\x1A', '\0'
      };
      f.write(magic, 23);
      uint32_t version = 7400;
      f.write(reinterpret_cast<const char *>(&version), 4);

      size_t offset = 27;
      for (const auto &r : roots) {
        r.Write(f, offset);
        offset += r.Size();
      }

      // Null record closes the top-level list.
      char nullRec[13] = {};
      f.write(nullRec, 13);
      offset += 13;

      // ── Footer ───────────────────────────────────────────────────────────
      // Exact layout, verified byte-for-byte against a file the Autodesk SDK
      // accepts:
      //   16 bytes footer id, zero padding up to a 16-byte boundary,
      //   uint32 version, 120 zero bytes, 16-byte magic.
      // There is NO extra padding word before the version — adding one shifts
      // the whole footer and the SDK reports "Cannot open FBX file".
      //
      // The footer id is not arbitrary: it is derived from FileId, so the two
      // constants below are a matched pair and must be changed together.
      static const unsigned char kFooterId[16] = {
        0xFA, 0xBC, 0xAB, 0x09, 0xD0, 0xC8, 0xD4, 0x66,
        0xB1, 0x76, 0xFB, 0x83, 0x1C, 0xF7, 0x26, 0x7E
      };
      f.write(reinterpret_cast<const char *>(kFooterId), 16);
      offset += 16;

      char zero[16] = {};
      size_t pad = (16 - (offset % 16)) % 16;
      if (pad) { f.write(zero, pad); offset += pad; }

      f.write(reinterpret_cast<const char *>(&version), 4);
      for (int i = 0; i < 120; i++) f.put('\0');

      static const unsigned char kFooterMagic[16] = {
        0xF8, 0x5A, 0x8C, 0x6A, 0xDE, 0xF5, 0xD9, 0x7E,
        0xEC, 0xE9, 0x0C, 0xE3, 0x75, 0x8F, 0x29, 0x0B
      };
      f.write(reinterpret_cast<const char *>(kFooterMagic), 16);
    }

    // ── Shared FBX preamble ──────────────────────────────────────────────
    // Every section here is required by the Autodesk FBX SDK (3ds Max and
    // Unreal both use it); Blender is happy with far less. Kept in one place
    // so the mesh and skeleton exports cannot drift apart -- in particular
    // the FileId / CreationTime / footer-id checksum triple, which only
    // validates as a matched set.
    // FBX measures time in KTime units; this is the documented tick rate. Used
    // by the preamble's time span as well as the animation writers.
    constexpr int64_t kKTimePerSecond = 46186158000LL;

    // pActiveAnimStack names the scene's active AnimationStack, or is empty for
    // a file with no animation. A reader that honours it will otherwise look
    // for a stack called "" and play nothing.
    void BuildFbxPreamble(std::vector<FbxNode> &roots,
                          const char *pActiveAnimStack = "") {
      const int64_t idDocument = 100000;
      const char *kCreator = "PAZ Unpacker FBX Export";

      // Part of the FileId / footer-id / CreationTime checksum triple below.
      // Do not change without changing the other two.
      const char *kCreationTime = "1970-01-01 10:00:00:000";

      std::time_t now = std::time(nullptr);
      std::tm tmNow = {};
#if defined(_MSC_VER)
      localtime_s(&tmNow, &now);
#else
      tmNow = *std::localtime(&now);
#endif
      char timeStamp[64];
      std::snprintf(timeStamp, sizeof(timeStamp), "%04d-%02d-%02d %02d:%02d:%02d:000",
                    tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                    tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec);

      {
        FbxNode hdr("FBXHeaderExtension");
        hdr.Child("FBXHeaderVersion").I32(1003);
        hdr.Child("FBXVersion").I32(7400);
        hdr.Child("EncryptionType").I32(0);
        {
          FbxNode &ts = hdr.Child("CreationTimeStamp");
          ts.Child("Version").I32(1000);
          ts.Child("Year").I32(tmNow.tm_year + 1900);
          ts.Child("Month").I32(tmNow.tm_mon + 1);
          ts.Child("Day").I32(tmNow.tm_mday);
          ts.Child("Hour").I32(tmNow.tm_hour);
          ts.Child("Minute").I32(tmNow.tm_min);
          ts.Child("Second").I32(tmNow.tm_sec);
          ts.Child("Millisecond").I32(0);
        }
        hdr.Child("Creator").Str(kCreator);
        {
          FbxNode &si = hdr.Child("SceneInfo");
          si.NameClass("GlobalInfo", "SceneInfo");
          si.Str("UserData");
          si.Child("Type").Str("UserData");
          si.Child("Version").I32(100);
          {
            FbxNode &md = si.Child("MetaData");
            md.Child("Version").I32(100);
            md.Child("Title").Str("");
            md.Child("Subject").Str("");
            md.Child("Author").Str("");
            md.Child("Keywords").Str("");
            md.Child("Revision").Str("");
            md.Child("Comment").Str("");
          }
          FbxNode &p70 = si.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("DocumentUrl", "KString", "Url", ""); p.Str("/dummy.fbx"); }
          { FbxNode &p = p70.Child("P"); p.P("SrcDocumentUrl", "KString", "Url", ""); p.Str("/dummy.fbx"); }
          { FbxNode &p = p70.Child("P"); p.P("Original", "Compound", "", ""); }
          { FbxNode &p = p70.Child("P"); p.P("Original|ApplicationVendor", "KString", "", ""); p.Str("sibercat"); }
          { FbxNode &p = p70.Child("P"); p.P("Original|ApplicationName", "KString", "", ""); p.Str(kCreator); }
          { FbxNode &p = p70.Child("P"); p.P("Original|ApplicationVersion", "KString", "", ""); p.Str("1.0"); }
          { FbxNode &p = p70.Child("P"); p.P("Original|DateTime_GMT", "DateTime", "", ""); p.Str(timeStamp); }
          { FbxNode &p = p70.Child("P"); p.P("Original|FileName", "KString", "", ""); p.Str("/dummy.fbx"); }
          { FbxNode &p = p70.Child("P"); p.P("LastSaved", "Compound", "", ""); }
          { FbxNode &p = p70.Child("P"); p.P("LastSaved|ApplicationVendor", "KString", "", ""); p.Str("sibercat"); }
          { FbxNode &p = p70.Child("P"); p.P("LastSaved|ApplicationName", "KString", "", ""); p.Str(kCreator); }
          { FbxNode &p = p70.Child("P"); p.P("LastSaved|ApplicationVersion", "KString", "", ""); p.Str("1.0"); }
          { FbxNode &p = p70.Child("P"); p.P("LastSaved|DateTime_GMT", "DateTime", "", ""); p.Str(timeStamp); }
        }
        roots.push_back(std::move(hdr));
      }
      {
        // 16 arbitrary but stable bytes; importers only check that it is present.
        FbxNode fid("FileId");
        static const unsigned char kFileId[16] = {
          0x28, 0xB3, 0x2A, 0xEB, 0xB6, 0x24, 0xCC, 0xC2,
          0xBF, 0xC8, 0xB0, 0x2A, 0xA9, 0x2B, 0xFC, 0xF1
        };
        fid.props.push_back('R');
        uint32_t n = 16;
        fid.Put(&n, 4);
        fid.props.append(reinterpret_cast<const char *>(kFileId), 16);
        fid.propCount++;
        roots.push_back(std::move(fid));
      }
      // The footer id is a checksum over FileId AND this CreationTime string,
      // so the three values are a matched set. Writing a real timestamp here
      // invalidates the footer and the Autodesk SDK refuses the file outright
      // ("Cannot open FBX file") — Blender hardcodes this same 1970 stamp for
      // exactly this reason. The genuine export time is still recorded in
      // CreationTimeStamp and SceneInfo above, which are not part of the sum.
      { FbxNode n("CreationTime"); n.Str(kCreationTime); roots.push_back(std::move(n)); }
      { FbxNode n("Creator");      n.Str(kCreator);      roots.push_back(std::move(n)); }
      {
        FbxNode gs("GlobalSettings");
        gs.Child("Version").I32(1000);
        FbxNode &p70 = gs.Child("Properties70");
        // Y-up, Z-forward, X-right: matches how the archive stores coordinates.
        { FbxNode &p = p70.Child("P"); p.P("UpAxis", "int", "Integer", ""); p.I32(1); }
        { FbxNode &p = p70.Child("P"); p.P("UpAxisSign", "int", "Integer", ""); p.I32(1); }
        { FbxNode &p = p70.Child("P"); p.P("FrontAxis", "int", "Integer", ""); p.I32(2); }
        { FbxNode &p = p70.Child("P"); p.P("FrontAxisSign", "int", "Integer", ""); p.I32(1); }
        { FbxNode &p = p70.Child("P"); p.P("CoordAxis", "int", "Integer", ""); p.I32(0); }
        { FbxNode &p = p70.Child("P"); p.P("CoordAxisSign", "int", "Integer", ""); p.I32(1); }
        { FbxNode &p = p70.Child("P"); p.P("UnitScaleFactor", "double", "Number", ""); p.F64(1.0); }
        { FbxNode &p = p70.Child("P"); p.P("OriginalUnitScaleFactor", "double", "Number", ""); p.F64(1.0); }
        // Time settings. Without these a reader has no scene frame rate or time
        // span to work from: UE reports "There was nothing to import" for an
        // animation-only file, because it derives the import range from here.
        // 10 is ePAL (25 fps), which matches the millisecond key times closely
        // enough; the real clip length travels on the AnimationStack anyway.
        { FbxNode &p = p70.Child("P"); p.P("TimeMode", "enum", "", ""); p.I32(10); }
        { FbxNode &p = p70.Child("P"); p.P("TimeSpanStart", "KTime", "Time", ""); p.I64(0); }
        { FbxNode &p = p70.Child("P"); p.P("TimeSpanStop", "KTime", "Time", ""); p.I64(kKTimePerSecond); }
        { FbxNode &p = p70.Child("P"); p.P("CustomFrameRate", "double", "Number", ""); p.F64(25.0); }
        roots.push_back(std::move(gs));
      }
      {
        // The SDK cross-checks these against what actually appears in Objects.
        FbxNode doc("Documents");
        doc.Child("Count").I32(1);
        FbxNode &d = doc.Child("Document");
        d.I64(idDocument);
        d.Str("Scene");
        d.Str("Scene");
        {
          FbxNode &p70 = d.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("SourceObject", "object", "", ""); }
          { FbxNode &p = p70.Child("P"); p.P("ActiveAnimStackName", "KString", "", "");
            p.Str(pActiveAnimStack); }
        }
        d.Child("RootNode").I64(0);
        roots.push_back(std::move(doc));
      }
      { roots.push_back(FbxNode("References")); }
    }

    bool WriteFbx(const PamModel &model, const std::wstring &wsPath,
                  const PamTextureFileList &vTextureFiles, std::wstring &wsError) {
      std::ofstream f(wsPath, std::ios::binary);
      if (!f) { wsError = L"Could not open the .fbx file for writing."; return false; }

      const std::string stem = StemOf(wsPath);
      const size_t vcount = model.vVertices.size();
      const size_t tcount = model.vIndices.size() / 3;

      // Unique object ids. Any distinct 64-bit values will do.
      const int64_t idDocument = 100000;
      const int64_t idGeometry = 1000000;
      const int64_t idModel    = 2000000;
      const int64_t idMatBase  = 3000000;
      const int64_t idTexBase  = 4000000;
      const int64_t idVidBase  = 5000000;

      // Which submeshes have a texture file to hook up.
      std::vector<size_t> texSlots;
      for (size_t s = 0; s < model.vSubmeshes.size(); s++)
        if (s < vTextureFiles.size() && !vTextureFiles[s].empty())
          texSlots.push_back(s);

      // ── Geometry arrays ──────────────────────────────────────────────────
      std::vector<double> verts;
      verts.reserve(vcount * 3);
      for (const auto &v : model.vVertices) {
        verts.push_back(v.x); verts.push_back(v.y); verts.push_back(v.z);
      }

      std::vector<double> normals;
      normals.reserve(vcount * 3);
      for (const auto &v : model.vVertices) {
        normals.push_back(v.nx); normals.push_back(v.ny); normals.push_back(v.nz);
      }

      std::vector<double> uvs;
      uvs.reserve(vcount * 2);
      for (const auto &v : model.vVertices) {
        uvs.push_back(v.u);
        uvs.push_back(1.0 - v.v);      // FBX UV origin is bottom-left
      }

      // Polygon vertex indices: the final index of each polygon is stored as
      // its bitwise complement to mark the end of the face.
      std::vector<int32_t> polyIdx;
      std::vector<int32_t> uvIdx;
      polyIdx.reserve(model.vIndices.size());
      uvIdx.reserve(model.vIndices.size());
      for (size_t i = 0; i + 2 < model.vIndices.size(); i += 3) {
        const int32_t a = (int32_t)model.vIndices[i + 0];
        const int32_t b = (int32_t)model.vIndices[i + 1];
        const int32_t c = (int32_t)model.vIndices[i + 2];
        polyIdx.push_back(a); polyIdx.push_back(b); polyIdx.push_back(~c);
        uvIdx.push_back(a);   uvIdx.push_back(b);   uvIdx.push_back(c);
      }

      // One material index per triangle.
      std::vector<int32_t> matPerPoly;
      matPerPoly.reserve(tcount);
      for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
        const uint32_t tris = model.vSubmeshes[s].uiIndexCount / 3;
        for (uint32_t t = 0; t < tris; t++) matPerPoly.push_back((int32_t)s);
      }

      // ── Document tree ────────────────────────────────────────────────────
      std::vector<FbxNode> roots;
      BuildFbxPreamble(roots);

      {
        FbxNode defs("Definitions");
        defs.Child("Version").I32(100);
        // Must equal the number of objects actually emitted, counting the
        // Texture and Video pairs: GlobalSettings + Geometry + Model +
        // materials + (texture + video) per textured submesh.
        defs.Child("Count").I32(3 + (int32_t)model.vSubmeshes.size()
                                  + 2 * (int32_t)texSlots.size());
        { FbxNode &o = defs.Child("ObjectType"); o.Str("GlobalSettings"); o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Geometry"); o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Model");    o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Material");
          o.Child("Count").I32((int32_t)model.vSubmeshes.size()); }
        if (!texSlots.empty()) {
          { FbxNode &o = defs.Child("ObjectType"); o.Str("Texture");
            o.Child("Count").I32((int32_t)texSlots.size()); }
          { FbxNode &o = defs.Child("ObjectType"); o.Str("Video");
            o.Child("Count").I32((int32_t)texSlots.size()); }
        }
        roots.push_back(std::move(defs));
      }

      {
        FbxNode objs("Objects");

        // Geometry
        FbxNode &geo = objs.Child("Geometry");
        geo.I64(idGeometry);
        // FBX 7.x expects the bare name here; the "Geometry::" style prefix is
        // a version 6 convention and importers surface it verbatim.
        geo.NameClass(stem, "Geometry");
        geo.Str("Mesh");
        geo.Child("Properties70");
        geo.Child("GeometryVersion").I32(124);
        geo.Child("Vertices").ArrF64(verts);
        geo.Child("PolygonVertexIndex").ArrI32(polyIdx);

        {
          FbxNode &n = geo.Child("LayerElementNormal");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("");
          n.Child("MappingInformationType").Str("ByVertice");
          n.Child("ReferenceInformationType").Str("Direct");
          n.Child("Normals").ArrF64(normals);
        }
        {
          FbxNode &n = geo.Child("LayerElementUV");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("UVMap");
          n.Child("MappingInformationType").Str("ByPolygonVertex");
          n.Child("ReferenceInformationType").Str("IndexToDirect");
          n.Child("UV").ArrF64(uvs);
          n.Child("UVIndex").ArrI32(uvIdx);
        }
        {
          FbxNode &n = geo.Child("LayerElementMaterial");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("");
          n.Child("MappingInformationType").Str("ByPolygon");
          n.Child("ReferenceInformationType").Str("IndexToDirect");
          n.Child("Materials").ArrI32(matPerPoly);
        }
        {
          FbxNode &layer = geo.Child("Layer");
          layer.I32(0);
          layer.Child("Version").I32(100);
          { FbxNode &e = layer.Child("LayerElement");
            e.Child("Type").Str("LayerElementNormal");   e.Child("TypedIndex").I32(0); }
          { FbxNode &e = layer.Child("LayerElement");
            e.Child("Type").Str("LayerElementUV");       e.Child("TypedIndex").I32(0); }
          { FbxNode &e = layer.Child("LayerElement");
            e.Child("Type").Str("LayerElementMaterial"); e.Child("TypedIndex").I32(0); }
        }

        // Model
        FbxNode &mdl = objs.Child("Model");
        mdl.I64(idModel);
        mdl.NameClass(stem, "Model");
        mdl.Str("Mesh");
        mdl.Child("Version").I32(232);
        {
          FbxNode &p70 = mdl.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("Lcl Translation", "Lcl Translation", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Lcl Rotation", "Lcl Rotation", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Lcl Scaling", "Lcl Scaling", "", "A");
            p.F64(1.0); p.F64(1.0); p.F64(1.0); }
          { FbxNode &p = p70.Child("P"); p.P("DefaultAttributeIndex", "int", "Integer", "");
            p.I32(0); }
          { FbxNode &p = p70.Child("P"); p.P("InheritType", "enum", "", ""); p.I32(1); }
        }
        mdl.Child("MultiLayer").I32(0);
        mdl.Child("MultiTake").I32(0);
        mdl.Child("Shading").Bool(true);
        mdl.Child("Culling").Str("CullingOff");

        // Materials
        for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
          const std::string mname = SanitiseMaterialName(model.vSubmeshes[s].sTexture, s);
          FbxNode &mat = objs.Child("Material");
          mat.I64(idMatBase + (int64_t)s);
          mat.NameClass(mname, "Material");
          mat.Str("");
          mat.Child("Version").I32(102);
          mat.Child("ShadingModel").Str("Phong");
          mat.Child("MultiLayer").I32(0);
          FbxNode &p70 = mat.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("ShadingModel", "KString", "", ""); p.Str("Phong"); }
          { FbxNode &p = p70.Child("P"); p.P("DiffuseColor", "Color", "", "A");
            p.F64(0.8); p.F64(0.8); p.F64(0.8); }
          { FbxNode &p = p70.Child("P"); p.P("DiffuseFactor", "Number", "", "A"); p.F64(1.0); }
          { FbxNode &p = p70.Child("P"); p.P("AmbientColor", "Color", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("SpecularColor", "Color", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("SpecularFactor", "Number", "", "A"); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Shininess", "Number", "", "A"); p.F64(20.0); }
          { FbxNode &p = p70.Child("P"); p.P("ShininessExponent", "Number", "", "A"); p.F64(20.0); }
          { FbxNode &p = p70.Child("P"); p.P("Opacity", "Number", "", "A"); p.F64(1.0); }
          { FbxNode &p = p70.Child("P"); p.P("Reflectivity", "Number", "", "A"); p.F64(0.0); }
        }

        // Texture + Video pairs. FBX models an image as a Video "clip" that a
        // Texture node points at; the Texture is then linked to a material
        // property. Paths are relative so the .fbx and .png stay portable.
        for (size_t k = 0; k < texSlots.size(); k++) {
          const size_t s = texSlots[k];
          const std::string &file = vTextureFiles[s];
          const std::string tname = SanitiseMaterialName(model.vSubmeshes[s].sTexture, s);

          FbxNode &tex = objs.Child("Texture");
          tex.I64(idTexBase + (int64_t)k);
          tex.NameClass(tname + "_tex", "Texture");
          tex.Str("");
          tex.Child("Type").Str("TextureVideoClip");
          tex.Child("Version").I32(202);
          tex.Child("TextureName").NameClass(tname + "_tex", "Texture");
          tex.Child("Media").NameClass(file, "Video");
          tex.Child("FileName").Str(file);
          tex.Child("RelativeFilename").Str(file);
          // Deliberately no ModelUVTranslation / ModelUVScaling: those are
          // vector properties (two doubles each), and emitting a scalar makes
          // strict readers choke. Nothing here needs them.
          {
            FbxNode &p70 = tex.Child("Properties70");
            FbxNode &p = p70.Child("P");
            p.P("UseMaterial", "bool", "", "");
            p.I32(1);
          }

          FbxNode &vid = objs.Child("Video");
          vid.I64(idVidBase + (int64_t)k);
          vid.NameClass(file, "Video");
          vid.Str("Clip");
          vid.Child("Type").Str("Clip");
          {
            FbxNode &p70 = vid.Child("Properties70");
            FbxNode &p = p70.Child("P");
            p.P("Path", "KString", "XRefUrl", "");
            p.Str(file);
          }
          vid.Child("UseMipMap").I32(0);
          vid.Child("Filename").Str(file);
          vid.Child("RelativeFilename").Str(file);
        }

        roots.push_back(std::move(objs));
      }

      {
        FbxNode conn("Connections");
        // Geometry -> Model, Model -> scene root, Material -> Model
        { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idGeometry); c.I64(idModel); }
        { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idModel);    c.I64(0); }
        for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
          FbxNode &c = conn.Child("C");
          c.Str("OO"); c.I64(idMatBase + (int64_t)s); c.I64(idModel);
        }
        // Texture -> material property, and the image clip -> texture.
        for (size_t k = 0; k < texSlots.size(); k++) {
          const size_t s = texSlots[k];
          { FbxNode &c = conn.Child("C"); c.Str("OP");
            c.I64(idTexBase + (int64_t)k); c.I64(idMatBase + (int64_t)s);
            c.Str("DiffuseColor"); }
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idVidBase + (int64_t)k); c.I64(idTexBase + (int64_t)k); }
        }
        roots.push_back(std::move(conn));
      }
      {
        // No animation, but the section must exist for the SDK.
        FbxNode takes("Takes");
        takes.Child("Current").Str("");
        roots.push_back(std::move(takes));
      }

      EmitFbxFile(f, roots);

      if (!f) { wsError = L"Failed while writing the .fbx file."; return false; }
      (void)vTextureFiles;   // textures are referenced by the sidecar files
      return true;
    }

    // ── Skeleton export ──────────────────────────────────────────────────────

    // FBX stores node rotation as Euler angles in degrees, applied in the order
    // named by RotationOrder (0 = XYZ, the default we rely on). For that order
    // the composed matrix is Rz*Ry*Rx, which gives the extraction below.
    void QuatToEulerXYZDeg(const float q[4], double out[3]) {
      double x = q[0], y = q[1], z = q[2], w = q[3];
      const double n = std::sqrt(x*x + y*y + z*z + w*w);
      if (n > 0.0) { x /= n; y /= n; z /= n; w /= n; }

      const double m00 = 1 - 2*(y*y + z*z), m01 =     2*(x*y - z*w), m02 =     2*(x*z + y*w);
      const double m10 =     2*(x*y + z*w), m11 = 1 - 2*(x*x + z*z), m12 =     2*(y*z - x*w);
      const double m20 =     2*(x*z - y*w), m21 =     2*(y*z + x*w), m22 = 1 - 2*(x*x + y*y);
      (void)m01; (void)m11;

      double sy = -m20;
      if (sy >  1.0) sy =  1.0;
      if (sy < -1.0) sy = -1.0;

      double rx, ry, rz;
      if (std::fabs(sy) > 0.999999) {
        // Gimbal lock: X and Z become degenerate, so fold everything into X.
        ry = std::asin(sy);
        rx = std::atan2(-m12, m11);
        rz = 0.0;
      } else {
        ry = std::asin(sy);
        rx = std::atan2(m21, m22);
        rz = std::atan2(m10, m00);
      }

      const double kRadToDeg = 57.295779513082320876798154814105;
      out[0] = rx * kRadToDeg;
      out[1] = ry * kRadToDeg;
      out[2] = rz * kRadToDeg;
    }

    // ── Animation helpers ────────────────────────────────────────────────────

    inline int64_t MsToKTime(uint32_t ms) {
      return (int64_t)ms * (kKTimePerSecond / 1000);
    }

    // Converts a quaternion track to Euler XYZ degrees, keeping each key
    // continuous with the one before it. Converting keys independently is
    // correct per key but produces 360 degree jumps between them, which reads
    // as a limb snapping round mid-clip.
    // Output is flat: three doubles per key.
    void QuatTrackToEuler(const std::vector<PaaQuatKey> &keys,
                          std::vector<double> &out) {
      out.clear();
      out.reserve(keys.size() * 3);
      double prev[3] = { 0.0, 0.0, 0.0 };
      for (size_t i = 0; i < keys.size(); i++) {
        double e[3];
        QuatToEulerXYZDeg(keys[i].q, e);
        if (i) {
          for (int c = 0; c < 3; c++) {
            // Snap onto the branch nearest the previous key.
            while (e[c] - prev[c] >  180.0) e[c] -= 360.0;
            while (e[c] - prev[c] < -180.0) e[c] += 360.0;
          }
        }
        for (int c = 0; c < 3; c++) { out.push_back(e[c]); prev[c] = e[c]; }
      }
    }

    // One FBX AnimationCurve. Tangent data is left at zero and the flags are
    // the value Blender and Max both emit for auto-clamped cubic keys.
    void EmitCurve(FbxNode &objs, int64_t id, const std::vector<int64_t> &times,
                   const std::vector<float> &values) {
      FbxNode &c = objs.Child("AnimationCurve");
      c.I64(id);
      c.NameClass("", "AnimCurve");
      c.Str("");
      c.Child("Default").F64(values.empty() ? 0.0 : values[0]);
      c.Child("KeyVer").I32(4008);
      c.Child("KeyTime").ArrI64(times);
      c.Child("KeyValueFloat").ArrF32(values);
      { std::vector<int32_t> f{ 24836 };            c.Child("KeyAttrFlags").ArrI32(f); }
      { std::vector<float>   d{ 0.f, 0.f, 0.f, 0.f }; c.Child("KeyAttrDataFloat").ArrF32(d); }
      { std::vector<int32_t> r{ (int32_t)times.size() }; c.Child("KeyAttrRefCount").ArrI32(r); }
    }

    bool WriteSkeletonFbx(const PabSkeleton &skel, const PaaAnimation *pAnim,
                          const std::wstring &wsPath, std::wstring &wsError) {
      std::ofstream f(wsPath, std::ios::binary);
      if (!f) { wsError = L"Could not open the .fbx file for writing."; return false; }

      const size_t bones = skel.vBones.size();

      // Ids must not collide with the mesh export's ranges, so that a combined
      // mesh + skeleton file can reuse both blocks unchanged later.
      const int64_t idModelBase = 6000000;
      const int64_t idAttrBase  = 7000000;
      const int64_t idAnimStack = 9000000;
      const int64_t idAnimLayer = 9000001;
      const int64_t idNodeBase  = 10000000;
      const int64_t idCurveBase = 11000000;

      // Work out which channels actually animate before emitting anything, so
      // the Definitions count matches and constant channels cost nothing. A
      // single key is a constant value the bone's Lcl property already holds.
      struct AnimChan {
        size_t   bone;
        int      kind;                 // 0 = translation, 1 = rotation, 2 = scale
        std::vector<int64_t> times;
        std::vector<float>   v[3];
      };
      std::vector<AnimChan> chans;

      if (pAnim) {
        for (const auto &tr : pAnim->vTracks) {
          const int b = skel.FindBoneById(tr.uiBoneId);
          if (b < 0) continue;          // clip drives a bone this rig lacks

          if (tr.vPosition.size() >= 2) {
            AnimChan c; c.bone = (size_t)b; c.kind = 0;
            for (const auto &k : tr.vPosition) {
              c.times.push_back(MsToKTime(k.uiTimeMs));
              for (int a = 0; a < 3; a++) c.v[a].push_back(k.v[a]);
            }
            chans.push_back(std::move(c));
          }
          if (tr.vRotation.size() >= 2) {
            std::vector<double> eul;
            QuatTrackToEuler(tr.vRotation, eul);
            AnimChan c; c.bone = (size_t)b; c.kind = 1;
            for (size_t k = 0; k < tr.vRotation.size(); k++) {
              c.times.push_back(MsToKTime(tr.vRotation[k].uiTimeMs));
              for (int a = 0; a < 3; a++) c.v[a].push_back((float)eul[k * 3 + a]);
            }
            chans.push_back(std::move(c));
          }
          if (tr.vScale.size() >= 2) {
            AnimChan c; c.bone = (size_t)b; c.kind = 2;
            for (const auto &k : tr.vScale) {
              c.times.push_back(MsToKTime(k.uiTimeMs));
              for (int a = 0; a < 3; a++) c.v[a].push_back(k.v[a]);
            }
            chans.push_back(std::move(c));
          }
        }
      }
      const size_t nChan = chans.size();

      std::vector<FbxNode> roots;
      BuildFbxPreamble(roots, nChan ? "Take 001" : "");

      {
        FbxNode defs("Definitions");
        defs.Child("Version").I32(100);
        defs.Child("Count").I32((int32_t)(1 + bones * 2 +
                                          (nChan ? 2 + nChan + nChan * 3 : 0)));
        { FbxNode &o = defs.Child("ObjectType"); o.Str("GlobalSettings"); o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Model");
          o.Child("Count").I32((int32_t)bones); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("NodeAttribute");
          o.Child("Count").I32((int32_t)bones); }
        if (nChan) {
          { FbxNode &o = defs.Child("ObjectType"); o.Str("AnimationStack"); o.Child("Count").I32(1); }
          { FbxNode &o = defs.Child("ObjectType"); o.Str("AnimationLayer"); o.Child("Count").I32(1); }
          { FbxNode &o = defs.Child("ObjectType"); o.Str("AnimationCurveNode");
            o.Child("Count").I32((int32_t)nChan); }
          { FbxNode &o = defs.Child("ObjectType"); o.Str("AnimationCurve");
            o.Child("Count").I32((int32_t)(nChan * 3)); }
        }
        roots.push_back(std::move(defs));
      }

      {
        FbxNode objs("Objects");

        for (size_t i = 0; i < bones; i++) {
          const PabBone &b = skel.vBones[i];

          // The attribute is what makes the node draw and behave as a bone
          // rather than an empty transform.
          FbxNode &attr = objs.Child("NodeAttribute");
          attr.I64(idAttrBase + (int64_t)i);
          attr.NameClass(b.sName, "NodeAttribute");
          attr.Str("LimbNode");
          {
            FbxNode &p70 = attr.Child("Properties70");
            FbxNode &p = p70.Child("P");
            p.P("Size", "double", "Number", ""); p.F64(1.0);
          }
          attr.Child("TypeFlags").Str("Skeleton");

          double euler[3];
          QuatToEulerXYZDeg(b.fQuat, euler);

          FbxNode &mdl = objs.Child("Model");
          mdl.I64(idModelBase + (int64_t)i);
          mdl.NameClass(b.sName, "Model");
          mdl.Str("LimbNode");
          mdl.Child("Version").I32(232);
          {
            FbxNode &p70 = mdl.Child("Properties70");
            { FbxNode &p = p70.Child("P"); p.P("RotationActive", "bool", "", ""); p.I32(1); }
            { FbxNode &p = p70.Child("P"); p.P("InheritType", "enum", "", ""); p.I32(1); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Translation", "Lcl Translation", "", "A");
              p.F64(b.fTrans[0]); p.F64(b.fTrans[1]); p.F64(b.fTrans[2]); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Rotation", "Lcl Rotation", "", "A");
              p.F64(euler[0]); p.F64(euler[1]); p.F64(euler[2]); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Scaling", "Lcl Scaling", "", "A");
              p.F64(b.fScale[0]); p.F64(b.fScale[1]); p.F64(b.fScale[2]); }
            // Binds the LimbNode attribute to this node; without it Max builds
            // a Dummy helper instead of a bone.
            { FbxNode &p = p70.Child("P"); p.P("DefaultAttributeIndex", "int", "Integer", "");
              p.I32(0); }
          }
          mdl.Child("Shading").Bool(true);
          mdl.Child("Culling").Str("CullingOff");
        }

        if (nChan) {
          const int64_t stop = MsToKTime(pAnim->DurationMs());
          {
            FbxNode &st = objs.Child("AnimationStack");
            st.I64(idAnimStack);
            st.NameClass("Take 001", "AnimStack");
            st.Str("");
            FbxNode &p70 = st.Child("Properties70");
            { FbxNode &p = p70.Child("P"); p.P("LocalStop", "KTime", "Time", ""); p.I64(stop); }
            { FbxNode &p = p70.Child("P"); p.P("ReferenceStop", "KTime", "Time", ""); p.I64(stop); }
          }
          {
            FbxNode &ly = objs.Child("AnimationLayer");
            ly.I64(idAnimLayer);
            ly.NameClass("Base Layer", "AnimLayer");
            ly.Str("");
          }
          static const char *kAxis[3] = { "d|X", "d|Y", "d|Z" };
          for (size_t k = 0; k < nChan; k++) {
            const AnimChan &c = chans[k];
            FbxNode &cn = objs.Child("AnimationCurveNode");
            cn.I64(idNodeBase + (int64_t)k);
            cn.NameClass(c.kind == 0 ? "T" : c.kind == 1 ? "R" : "S", "AnimCurveNode");
            cn.Str("");
            FbxNode &p70 = cn.Child("Properties70");
            for (int a = 0; a < 3; a++) {
              FbxNode &p = p70.Child("P");
              p.P(kAxis[a], "Number", "", "A");
              p.F64(c.v[a].empty() ? 0.0 : c.v[a][0]);
            }
            for (int a = 0; a < 3; a++)
              EmitCurve(objs, idCurveBase + (int64_t)(k * 3 + a), c.times, c.v[a]);
          }
        }

        roots.push_back(std::move(objs));
      }

      {
        FbxNode conn("Connections");
        for (size_t i = 0; i < bones; i++) {
          // Attribute -> its own model.
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idAttrBase + (int64_t)i); c.I64(idModelBase + (int64_t)i); }
          // Model -> parent model, or the scene root for the skeleton root.
          const int32_t par = skel.vBones[i].iParent;
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idModelBase + (int64_t)i);
            c.I64(par < 0 ? 0 : idModelBase + (int64_t)par); }
        }
        if (nChan) {
          static const char *kProp[3] = { "Lcl Translation", "Lcl Rotation", "Lcl Scaling" };
          static const char *kAxis[3] = { "d|X", "d|Y", "d|Z" };
          { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idAnimStack); c.I64(0); }
          { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idAnimLayer); c.I64(idAnimStack); }
          for (size_t k = 0; k < nChan; k++) {
            const AnimChan &ch = chans[k];
            { FbxNode &c = conn.Child("C"); c.Str("OO");
              c.I64(idNodeBase + (int64_t)k); c.I64(idAnimLayer); }
            // The curve node drives one property of one bone.
            { FbxNode &c = conn.Child("C"); c.Str("OP");
              c.I64(idNodeBase + (int64_t)k);
              c.I64(idModelBase + (int64_t)ch.bone);
              c.Str(kProp[ch.kind]); }
            for (int a = 0; a < 3; a++) {
              FbxNode &c = conn.Child("C"); c.Str("OP");
              c.I64(idCurveBase + (int64_t)(k * 3 + a));
              c.I64(idNodeBase + (int64_t)k);
              c.Str(kAxis[a]);
            }
          }
        }
        roots.push_back(std::move(conn));
      }

      {
        // Current on its own names a take that does not exist, so a reader
        // looking the take up finds nothing -- which is exactly how UE reports
        // "There was nothing to import". The Take entry has to be here too,
        // carrying the same span as the AnimationStack.
        FbxNode takes("Takes");
        takes.Child("Current").Str(nChan ? "Take 001" : "");
        if (nChan) {
          const int64_t stop = MsToKTime(pAnim->DurationMs());
          FbxNode &tk = takes.Child("Take");
          tk.Str("Take 001");
          tk.Child("FileName").Str("Take 001.tak");
          { FbxNode &t = tk.Child("LocalTime");     t.I64(0); t.I64(stop); }
          { FbxNode &t = tk.Child("ReferenceTime"); t.I64(0); t.I64(stop); }
        }
        roots.push_back(std::move(takes));
      }

      EmitFbxFile(f, roots);

      if (!f) { wsError = L"Failed while writing the .fbx file."; return false; }
      return true;
    }

    // ── Skinned mesh export ──────────────────────────────────────────────────

    // Inverse of a rigid transform stored the way FBX lays one out (basis rows,
    // translation at 12..14). Bones carry rotation and translation only, so the
    // rotation part transposes and the translation negates through it. Using a
    // general inverse here would be slower and less numerically clean.
    void InvertRigid(const float *m, double *out) {
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          out[r * 4 + c] = m[c * 4 + r];
      for (int c = 0; c < 3; c++) {
        double t = 0.0;
        for (int k = 0; k < 3; k++) t += (double)m[12 + k] * m[k * 4 + c];
        out[12 + c] = -t;
      }
      out[3] = out[7] = out[11] = 0.0;
      out[15] = 1.0;
    }

    bool WriteSkinnedFbx(const PacModel &model, const PabSkeleton &skel,
                         const PamTextureFileList &vTextureFiles,
                         const std::wstring &wsPath, std::wstring &wsError) {
      if (skel.IsEmpty()) { wsError = L"The skeleton has no bones."; return false; }

      // Resolve the mesh's bone palette onto the skeleton by bone id. If this
      // fails the two files do not belong together, and binding anyway would
      // silently produce a mangled skin.
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
      if (!f) { wsError = L"Could not open the .fbx file for writing."; return false; }

      // Flatten every submesh into one mesh; FBX handles per-face materials,
      // and a single skin keeps the cluster bookkeeping manageable.
      std::vector<double>  verts, normals, uvs;
      std::vector<int32_t> polyIdx, uvIdx, matPerPoly;
      // Per bone: the vertices it influences and by how much.
      std::vector<std::vector<int32_t>> clusterIdx(skel.vBones.size());
      std::vector<std::vector<double>>  clusterWgt(skel.vBones.size());

      int32_t base = 0;
      int32_t smIndex = 0;
      for (const auto &sm : model.vSubmeshes) {
        for (const auto &v : sm.vVertices) {
          verts.push_back(v.x);  verts.push_back(v.y);  verts.push_back(v.z);
          normals.push_back(v.nx); normals.push_back(v.ny); normals.push_back(v.nz);
          uvs.push_back(v.u);    uvs.push_back(1.0 - v.v);   // FBX UV origin is bottom-left
        }
        for (size_t i = 0; i + 2 < sm.vIndices.size(); i += 3) {
          const int32_t a = base + (int32_t)sm.vIndices[i + 0];
          const int32_t b = base + (int32_t)sm.vIndices[i + 1];
          const int32_t c = base + (int32_t)sm.vIndices[i + 2];
          polyIdx.push_back(a); polyIdx.push_back(b); polyIdx.push_back(~c);
          uvIdx.push_back(a);   uvIdx.push_back(b);   uvIdx.push_back(c);
          matPerPoly.push_back(smIndex);
        }
        for (size_t i = 0; i < sm.vVertices.size(); i++) {
          const auto &v = sm.vVertices[i];
          // Weights are quantised to 8 bits and sum to about 255, so normalise
          // against the actual total rather than assuming exactly 255.
          int total = 0;
          for (int k = 0; k < 4; k++) total += v.weight[k];
          if (!total) continue;
          for (int k = 0; k < 4; k++) {
            if (!v.weight[k]) continue;
            const int bone = paletteToBone[v.bone[k]];
            clusterIdx[bone].push_back(base + (int32_t)i);
            clusterWgt[bone].push_back((double)v.weight[k] / (double)total);
          }
        }
        base += (int32_t)sm.vVertices.size();
        smIndex++;
      }

      // Submeshes whose texture was resolved get a Texture + Video pair.
      std::vector<size_t> texSlots;
      for (size_t s = 0; s < model.vSubmeshes.size(); s++)
        if (s < vTextureFiles.size() && !vTextureFiles[s].empty()) texSlots.push_back(s);

      std::vector<float> world;
      skel.ComputeWorldMatrices(world);

      // Only bones that actually influence something need a cluster.
      std::vector<size_t> active;
      for (size_t b = 0; b < skel.vBones.size(); b++)
        if (!clusterIdx[b].empty()) active.push_back(b);

      const std::string stem = StemOf(wsPath);

      const int64_t idGeometry  = 1000000;
      const int64_t idModel     = 2000000;
      const int64_t idSkin      = 3000000;
      const int64_t idPose      = 4000000;
      const int64_t idMatBase   = 5000000;
      const int64_t idModelBase = 6000000;
      const int64_t idAttrBase  = 7000000;
      const int64_t idClusBase  = 8000000;
      const int64_t idTexBase   = 12000000;
      const int64_t idVidBase   = 13000000;
      const size_t  nMat        = model.vSubmeshes.size();

      std::vector<FbxNode> roots;
      BuildFbxPreamble(roots);

      {
        FbxNode defs("Definitions");
        defs.Child("Version").I32(100);
        defs.Child("Count").I32((int32_t)(3 + skel.vBones.size() * 2 + active.size() + 1
                                          + nMat + 2 * texSlots.size()));
        { FbxNode &o = defs.Child("ObjectType"); o.Str("GlobalSettings"); o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Geometry");  o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Model");
          o.Child("Count").I32((int32_t)(1 + skel.vBones.size())); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("NodeAttribute");
          o.Child("Count").I32((int32_t)skel.vBones.size()); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Deformer");
          o.Child("Count").I32((int32_t)(1 + active.size())); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Pose"); o.Child("Count").I32(1); }
        { FbxNode &o = defs.Child("ObjectType"); o.Str("Material");
          o.Child("Count").I32((int32_t)nMat); }
        if (!texSlots.empty()) {
          { FbxNode &o = defs.Child("ObjectType"); o.Str("Texture");
            o.Child("Count").I32((int32_t)texSlots.size()); }
          { FbxNode &o = defs.Child("ObjectType"); o.Str("Video");
            o.Child("Count").I32((int32_t)texSlots.size()); }
        }
        roots.push_back(std::move(defs));
      }

      {
        FbxNode objs("Objects");

        FbxNode &geo = objs.Child("Geometry");
        geo.I64(idGeometry);
        geo.NameClass(stem, "Geometry");
        geo.Str("Mesh");
        geo.Child("Properties70");
        geo.Child("GeometryVersion").I32(124);
        geo.Child("Vertices").ArrF64(verts);
        geo.Child("PolygonVertexIndex").ArrI32(polyIdx);
        {
          FbxNode &n = geo.Child("LayerElementNormal");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("");
          n.Child("MappingInformationType").Str("ByVertice");
          n.Child("ReferenceInformationType").Str("Direct");
          n.Child("Normals").ArrF64(normals);
        }
        {
          FbxNode &n = geo.Child("LayerElementUV");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("UVMap");
          n.Child("MappingInformationType").Str("ByPolygonVertex");
          n.Child("ReferenceInformationType").Str("IndexToDirect");
          n.Child("UV").ArrF64(uvs);
          n.Child("UVIndex").ArrI32(uvIdx);
        }
        {
          // One material per submesh, assigned per triangle, so a flattened
          // mesh still shows the right texture on each part.
          FbxNode &n = geo.Child("LayerElementMaterial");
          n.I32(0);
          n.Child("Version").I32(101);
          n.Child("Name").Str("");
          n.Child("MappingInformationType").Str("ByPolygon");
          n.Child("ReferenceInformationType").Str("IndexToDirect");
          n.Child("Materials").ArrI32(matPerPoly);
        }
        {
          FbxNode &lay = geo.Child("Layer");
          lay.I32(0);
          lay.Child("Version").I32(100);
          { FbxNode &e = lay.Child("LayerElement");
            e.Child("Type").Str("LayerElementNormal"); e.Child("TypedIndex").I32(0); }
          { FbxNode &e = lay.Child("LayerElement");
            e.Child("Type").Str("LayerElementUV"); e.Child("TypedIndex").I32(0); }
          { FbxNode &e = lay.Child("LayerElement");
            e.Child("Type").Str("LayerElementMaterial"); e.Child("TypedIndex").I32(0); }
        }

        FbxNode &mdl = objs.Child("Model");
        mdl.I64(idModel);
        mdl.NameClass(stem, "Model");
        mdl.Str("Mesh");
        mdl.Child("Version").I32(232);
        {
          // DefaultAttributeIndex is what binds the node's attribute -- here
          // the geometry -- to the node. Without it 3ds Max builds the node as
          // a Dummy helper and drops the mesh entirely, while Blender and UE
          // infer the link from the connection and show nothing wrong.
          FbxNode &p70 = mdl.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("Lcl Translation", "Lcl Translation", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Lcl Rotation", "Lcl Rotation", "", "A");
            p.F64(0.0); p.F64(0.0); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Lcl Scaling", "Lcl Scaling", "", "A");
            p.F64(1.0); p.F64(1.0); p.F64(1.0); }
          { FbxNode &p = p70.Child("P"); p.P("DefaultAttributeIndex", "int", "Integer", "");
            p.I32(0); }
          { FbxNode &p = p70.Child("P"); p.P("InheritType", "enum", "", ""); p.I32(1); }
        }
        mdl.Child("MultiLayer").I32(0);
        mdl.Child("MultiTake").I32(0);
        mdl.Child("Shading").Bool(true);
        mdl.Child("Culling").Str("CullingOff");

        for (size_t i = 0; i < skel.vBones.size(); i++) {
          const PabBone &b = skel.vBones[i];

          FbxNode &attr = objs.Child("NodeAttribute");
          attr.I64(idAttrBase + (int64_t)i);
          attr.NameClass(b.sName, "NodeAttribute");
          attr.Str("LimbNode");
          { FbxNode &p70 = attr.Child("Properties70");
            FbxNode &p = p70.Child("P"); p.P("Size", "double", "Number", ""); p.F64(1.0); }
          attr.Child("TypeFlags").Str("Skeleton");

          double euler[3];
          QuatToEulerXYZDeg(b.fQuat, euler);

          FbxNode &bm = objs.Child("Model");
          bm.I64(idModelBase + (int64_t)i);
          bm.NameClass(b.sName, "Model");
          bm.Str("LimbNode");
          bm.Child("Version").I32(232);
          {
            FbxNode &p70 = bm.Child("Properties70");
            { FbxNode &p = p70.Child("P"); p.P("RotationActive", "bool", "", ""); p.I32(1); }
            { FbxNode &p = p70.Child("P"); p.P("InheritType", "enum", "", ""); p.I32(1); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Translation", "Lcl Translation", "", "A");
              p.F64(b.fTrans[0]); p.F64(b.fTrans[1]); p.F64(b.fTrans[2]); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Rotation", "Lcl Rotation", "", "A");
              p.F64(euler[0]); p.F64(euler[1]); p.F64(euler[2]); }
            { FbxNode &p = p70.Child("P"); p.P("Lcl Scaling", "Lcl Scaling", "", "A");
              p.F64(b.fScale[0]); p.F64(b.fScale[1]); p.F64(b.fScale[2]); }
            // Binds the LimbNode attribute to this node; without it Max builds
            // a Dummy helper instead of a bone.
            { FbxNode &p = p70.Child("P"); p.P("DefaultAttributeIndex", "int", "Integer", "");
              p.I32(0); }
          }
          bm.Child("Shading").Bool(true);
          bm.Child("Culling").Str("CullingOff");
        }

        for (size_t s = 0; s < nMat; s++) {
          const std::string mname = SanitiseMaterialName(model.vSubmeshes[s].sName, s);
          FbxNode &mat = objs.Child("Material");
          mat.I64(idMatBase + (int64_t)s);
          mat.NameClass(mname, "Material");
          mat.Str("");
          mat.Child("Version").I32(102);
          mat.Child("ShadingModel").Str("Phong");
          mat.Child("MultiLayer").I32(0);
          FbxNode &p70 = mat.Child("Properties70");
          { FbxNode &p = p70.Child("P"); p.P("ShadingModel", "KString", "", ""); p.Str("Phong"); }
          { FbxNode &p = p70.Child("P"); p.P("DiffuseColor", "Color", "", "A");
            p.F64(0.8); p.F64(0.8); p.F64(0.8); }
          { FbxNode &p = p70.Child("P"); p.P("DiffuseFactor", "Number", "", "A"); p.F64(1.0); }
          { FbxNode &p = p70.Child("P"); p.P("SpecularFactor", "Number", "", "A"); p.F64(0.0); }
          { FbxNode &p = p70.Child("P"); p.P("Opacity", "Number", "", "A"); p.F64(1.0); }
        }

        for (size_t k = 0; k < texSlots.size(); k++) {
          const size_t s = texSlots[k];
          const std::string &file = vTextureFiles[s];
          const std::string tname = SanitiseMaterialName(model.vSubmeshes[s].sName, s);

          FbxNode &tex = objs.Child("Texture");
          tex.I64(idTexBase + (int64_t)k);
          tex.NameClass(tname + "_tex", "Texture");
          tex.Str("");
          tex.Child("Type").Str("TextureVideoClip");
          tex.Child("Version").I32(202);
          tex.Child("TextureName").Str(tname + "_tex" + std::string("\0\x01""Texture", 9));
          tex.Child("Properties70");
          tex.Child("Media").Str(tname + "_vid" + std::string("\0\x01""Video", 7));
          tex.Child("FileName").Str(file);
          tex.Child("RelativeFilename").Str(file);
          tex.Child("ModelUVTranslation").F64(0.0), tex.Child("ModelUVScaling").F64(1.0);
          tex.Child("Texture_Alpha_Source").Str("None");

          FbxNode &vid = objs.Child("Video");
          vid.I64(idVidBase + (int64_t)k);
          vid.NameClass(tname + "_vid", "Video");
          vid.Str("Clip");
          vid.Child("Type").Str("Clip");
          vid.Child("Properties70");
          vid.Child("UseMipMap").I32(0);
          vid.Child("Filename").Str(file);
          vid.Child("RelativeFilename").Str(file);
        }

        {
          FbxNode &skin = objs.Child("Deformer");
          skin.I64(idSkin);
          skin.NameClass(stem + "_skin", "Deformer");
          skin.Str("Skin");
          skin.Child("Version").I32(101);
          skin.Child("Link_DeformAcceptance").F64(0.0);
          skin.Child("SkinningType").Str("Linear");
        }

        for (size_t k = 0; k < active.size(); k++) {
          const size_t b = active[k];
          FbxNode &cl = objs.Child("Deformer");
          cl.I64(idClusBase + (int64_t)k);
          cl.NameClass(skel.vBones[b].sName + "_cluster", "SubDeformer");
          cl.Str("Cluster");
          cl.Child("Version").I32(100);
          { FbxNode &u = cl.Child("UserData"); u.Str(""); u.Str(""); }
          cl.Child("Indexes").ArrI32(clusterIdx[b]);
          cl.Child("Weights").ArrF64(clusterWgt[b]);

          // TransformLink is the bone's world transform at bind time, and
          // Transform is the mesh expressed in that bone's space: a reader
          // recovers the mesh's world matrix as TransformLink * Transform
          // (this is literally what Blender's importer computes). The mesh
          // sits at the origin, so Transform is simply the inverse.
          const float *W = &world[b * 16];
          double inv[16];
          InvertRigid(W, inv);
          std::vector<double> tr(inv, inv + 16), lk(16);
          for (int i = 0; i < 16; i++) lk[i] = W[i];
          cl.Child("Transform").ArrF64(tr);
          cl.Child("TransformLink").ArrF64(lk);
        }

        {
          FbxNode &pose = objs.Child("Pose");
          pose.I64(idPose);
          pose.NameClass(stem + "_bindpose", "Pose");
          pose.Str("BindPose");
          pose.Child("Type").Str("BindPose");
          pose.Child("Version").I32(100);
          pose.Child("NbPoseNodes").I32((int32_t)(1 + skel.vBones.size()));
          {
            FbxNode &pn = pose.Child("PoseNode");
            pn.Child("Node").I64(idModel);
            std::vector<double> ident(16, 0.0);
            ident[0] = ident[5] = ident[10] = ident[15] = 1.0;
            pn.Child("Matrix").ArrF64(ident);
          }
          for (size_t i = 0; i < skel.vBones.size(); i++) {
            FbxNode &pn = pose.Child("PoseNode");
            pn.Child("Node").I64(idModelBase + (int64_t)i);
            std::vector<double> m(16);
            for (int k = 0; k < 16; k++) m[k] = world[i * 16 + k];
            pn.Child("Matrix").ArrF64(m);
          }
        }

        roots.push_back(std::move(objs));
      }

      {
        FbxNode conn("Connections");
        { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idGeometry); c.I64(idModel); }
        { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idModel);    c.I64(0); }
        for (size_t i = 0; i < skel.vBones.size(); i++) {
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idAttrBase + (int64_t)i); c.I64(idModelBase + (int64_t)i); }
          const int32_t par = skel.vBones[i].iParent;
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idModelBase + (int64_t)i);
            c.I64(par < 0 ? 0 : idModelBase + (int64_t)par); }
        }
        for (size_t s = 0; s < nMat; s++) {
          FbxNode &c = conn.Child("C");
          c.Str("OO"); c.I64(idMatBase + (int64_t)s); c.I64(idModel);
        }
        for (size_t k = 0; k < texSlots.size(); k++) {
          const size_t s = texSlots[k];
          { FbxNode &c = conn.Child("C"); c.Str("OP");
            c.I64(idTexBase + (int64_t)k); c.I64(idMatBase + (int64_t)s);
            c.Str("DiffuseColor"); }
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idVidBase + (int64_t)k); c.I64(idTexBase + (int64_t)k); }
        }
        // Skin deforms the geometry; each cluster belongs to the skin and is
        // driven by one bone.
        { FbxNode &c = conn.Child("C"); c.Str("OO"); c.I64(idSkin); c.I64(idGeometry); }
        for (size_t k = 0; k < active.size(); k++) {
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idClusBase + (int64_t)k); c.I64(idSkin); }
          { FbxNode &c = conn.Child("C"); c.Str("OO");
            c.I64(idModelBase + (int64_t)active[k]); c.I64(idClusBase + (int64_t)k); }
        }
        roots.push_back(std::move(conn));
      }

      { FbxNode takes("Takes"); takes.Child("Current").Str(""); roots.push_back(std::move(takes)); }

      EmitFbxFile(f, roots);

      if (!f) { wsError = L"Failed while writing the .fbx file."; return false; }
      return true;
    }

  } // namespace

  const wchar_t *ExportExtension(PamExportFormat format) {
    switch (format) {
      case PAM_EXPORT_FBX: return L"fbx";
      case PAM_EXPORT_OBJ:
      default:             return L"obj";
    }
  }

  bool ExportModel(const PamModel &model, const std::wstring &wsPath,
                   PamExportFormat format, const PamTextureFileList &vTextureFiles,
                   std::wstring &wsError) {
    wsError.clear();
    if (model.IsEmpty()) {
      wsError = L"The model has no geometry to export.";
      return false;
    }

    switch (format) {
      case PAM_EXPORT_OBJ:
        return WriteObj(model, wsPath, vTextureFiles, wsError);
      case PAM_EXPORT_FBX:
        return WriteFbx(model, wsPath, vTextureFiles, wsError);
    }
    wsError = L"Unknown export format.";
    return false;
  }

  bool ExportSkeleton(const PabSkeleton &skel, const std::wstring &wsPath,
                      std::wstring &wsError) {
    wsError.clear();
    if (skel.IsEmpty()) {
      wsError = L"The skeleton has no bones to export.";
      return false;
    }
    // OBJ has no concept of a node hierarchy, so FBX is the only option here.
    return WriteSkeletonFbx(skel, nullptr, wsPath, wsError);
  }

  bool ExportAnimation(const PabSkeleton &skel, const PaaAnimation &anim,
                       const std::wstring &wsPath, std::wstring &wsError) {
    wsError.clear();
    if (skel.IsEmpty()) { wsError = L"The skeleton has no bones."; return false; }
    if (anim.IsEmpty()) { wsError = L"The clip has no keyframes."; return false; }

    // A clip that drives no bone of this skeleton would export as a silent
    // rest pose, which looks like a broken export rather than a mismatch.
    size_t hit = 0;
    for (const auto &t : anim.vTracks)
      if (skel.FindBoneById(t.uiBoneId) >= 0) hit++;
    if (!hit) {
      wsError = L"None of this clip's tracks match the skeleton.\r\n\r\n"
                L"The clip and skeleton belong to different characters.";
      return false;
    }
    return WriteSkeletonFbx(skel, &anim, wsPath, wsError);
  }

  bool ExportSkinnedModel(const PacModel &model, const PabSkeleton &skel,
                          const PamTextureFileList &vTextureFiles,
                          const std::wstring &wsPath, std::wstring &wsError) {
    wsError.clear();
    if (model.IsEmpty()) {
      wsError = L"The model has no geometry to export.";
      return false;
    }
    return WriteSkinnedFbx(model, skel, vTextureFiles, wsPath, wsError);
  }

}
