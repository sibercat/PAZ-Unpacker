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

      // Blender's importer is happy with a bare minimum, but the Autodesk FBX
      // SDK — which 3ds Max and Unreal both use — additionally requires the
      // header timestamp, SceneInfo, FileId, Documents, References and Takes
      // sections. Omitting Documents in particular leaves the SDK with no
      // scene to populate and the import silently yields nothing.
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
          { FbxNode &p = p70.Child("P"); p.P("ActiveAnimStackName", "KString", "", ""); p.Str(""); }
        }
        d.Child("RootNode").I64(0);
        roots.push_back(std::move(doc));
      }
      { roots.push_back(FbxNode("References")); }
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

      if (!f) { wsError = L"Failed while writing the .fbx file."; return false; }
      (void)vTextureFiles;   // textures are referenced by the sidecar files
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

}
