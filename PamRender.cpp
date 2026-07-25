#include "PamRender.h"

#include <algorithm>
#include <cmath>

namespace kukdh1 {

  namespace {
    constexpr float PI_F      = 3.14159265358979f;
    constexpr float PITCH_MAX = 1.55334f;   // ~89 degrees
    constexpr float NEAR_EPS  = 1e-4f;

    struct ScreenVertex {
      float sx, sy;      // viewport pixels
      float invW;        // 1/w for perspective-correct interpolation
      float uOverW;      // u/w and v/w, interpolated linearly in screen space
      float vOverW;
      float depth;       // 0..1, smaller is nearer
      float light;       // lambert term, already shaded
    };

    // View-space vertex, kept so triangles crossing the near plane can be
    // clipped before projection. Projecting first would divide by ~0.
    struct ViewVertex {
      float vx, vy, vz;
      float u, v;
      float light;
    };

    struct ProjParams {
      float halfW, halfH, tanHalf, aspect, panX, panY, zNear, zFar;
    };

    inline ScreenVertex ProjectView(const ViewVertex &v, const ProjParams &pp) {
      ScreenVertex o;
      const float invW = 1.0f / v.vz;
      o.invW   = invW;
      o.sx     = pp.halfW + (v.vx / (pp.tanHalf * pp.aspect)) * invW * pp.halfW + pp.panX;
      o.sy     = pp.halfH - (v.vy / pp.tanHalf) * invW * pp.halfH + pp.panY;
      o.uOverW = v.u * invW;
      o.vOverW = v.v * invW;
      float d  = (v.vz - pp.zNear) / (pp.zFar - pp.zNear);
      o.depth  = (d < 0.0f) ? 0.0f : (d > 1.0f ? 1.0f : d);
      o.light  = v.light;
      return o;
    }

    // Sutherland-Hodgman against the single plane vz >= zClip. Returns 0, 3 or
    // 4 vertices; 4 is fanned into two triangles by the caller. Clipping rather
    // than discarding is what keeps a large polygon that merely touches the
    // near plane from vanishing whole.
    inline int ClipNear(const ViewVertex in[3], float zClip, ViewVertex out[4]) {
      int n = 0;
      for (int i = 0; i < 3; i++) {
        const ViewVertex &a = in[i];
        const ViewVertex &b = in[(i + 1) % 3];
        const bool ain = (a.vz >= zClip), bin = (b.vz >= zClip);
        if (ain) out[n++] = a;
        if (ain != bin) {
          const float t = (zClip - a.vz) / (b.vz - a.vz);
          ViewVertex m;
          m.vx    = a.vx + (b.vx - a.vx) * t;
          m.vy    = a.vy + (b.vy - a.vy) * t;
          m.vz    = zClip;
          m.u     = a.u + (b.u - a.u) * t;
          m.v     = a.v + (b.v - a.v) * t;
          m.light = a.light + (b.light - a.light) * t;
          out[n++] = m;
        }
      }
      return n;
    }

    inline uint32_t PackRGB(int r, int g, int b) {
      r = (r < 0) ? 0 : (r > 255 ? 255 : r);
      g = (g < 0) ? 0 : (g > 255 ? 255 : g);
      b = (b < 0) ? 0 : (b > 255 ? 255 : b);
      return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    // Distinct but muted tints so adjacent untextured submeshes stay readable.
    const uint8_t kSubmeshTint[8][3] = {
      { 205, 205, 210 }, { 200, 175, 145 }, { 160, 195, 200 }, { 195, 180, 200 },
      { 175, 200, 165 }, { 210, 195, 155 }, { 165, 175, 205 }, { 200, 165, 165 }
    };

    // Wrap a texture coordinate into [0, n). BDO models tile heavily, so UVs
    // well outside 0..1 are normal.
    inline int WrapCoord(int v, int n) {
      if (n <= 0) return 0;
      v %= n;
      return (v < 0) ? v + n : v;
    }

    // Bilinear sample; u/v are unnormalised texel coordinates.
    inline void SampleBilinear(const PamTexture &tex, float u, float v,
                               int &outR, int &outG, int &outB, int &outA) {
      float fx = u - 0.5f, fy = v - 0.5f;
      int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
      float tx = fx - x0, ty = fy - y0;

      int x1, y1;
      if (tex.bPow2) {
        // Nearly every game texture is power-of-two, and masking replaces four
        // integer modulos per sample — the single biggest cost in this loop.
        // Two's complement makes this correct for negatives too (-1 & 511 = 511).
        const int mx = tex.nWidth - 1, my = tex.nHeight - 1;
        x1 = (x0 + 1) & mx;  y1 = (y0 + 1) & my;
        x0 &= mx;            y0 &= my;
      }
      else {
        x1 = WrapCoord(x0 + 1, tex.nWidth);
        y1 = WrapCoord(y0 + 1, tex.nHeight);
        x0 = WrapCoord(x0, tex.nWidth);
        y0 = WrapCoord(y0, tex.nHeight);
      }

      const uint32_t *p = tex.vPixels.data();
      uint32_t c00 = p[(size_t)y0 * tex.nWidth + x0];
      uint32_t c10 = p[(size_t)y0 * tex.nWidth + x1];
      uint32_t c01 = p[(size_t)y1 * tex.nWidth + x0];
      uint32_t c11 = p[(size_t)y1 * tex.nWidth + x1];

      float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
      float w01 = (1 - tx) * ty,       w11 = tx * ty;

      outR = (int)(((c00 >> 16) & 0xFF) * w00 + ((c10 >> 16) & 0xFF) * w10 +
                   ((c01 >> 16) & 0xFF) * w01 + ((c11 >> 16) & 0xFF) * w11);
      outG = (int)(((c00 >> 8) & 0xFF) * w00 + ((c10 >> 8) & 0xFF) * w10 +
                   ((c01 >> 8) & 0xFF) * w01 + ((c11 >> 8) & 0xFF) * w11);
      outB = (int)((( c00 & 0xFF)) * w00 + (( c10 & 0xFF)) * w10 +
                   ((c01 & 0xFF)) * w01 + ((c11 & 0xFF)) * w11);
      outA = (int)(((c00 >> 24) & 0xFF) * w00 + ((c10 >> 24) & 0xFF) * w10 +
                   ((c01 >> 24) & 0xFF) * w01 + ((c11 >> 24) & 0xFF) * w11);
    }

    void DrawLine(PamTarget &t, float x0, float y0, float x1, float y1, uint32_t clr) {
      float dx = x1 - x0, dy = y1 - y0;
      int steps = (int)(std::max)(std::fabs(dx), std::fabs(dy));
      if (steps <= 0) return;
      if (steps > 4096) steps = 4096;

      float ix = dx / steps, iy = dy / steps;
      float x = x0, y = y0;
      for (int i = 0; i <= steps; i++) {
        int px = (int)(x + 0.5f), py = (int)(y + 0.5f);
        if (px >= 0 && px < t.nWidth && py >= 0 && py < t.nHeight)
          t.pPixels[(size_t)py * t.nWidth + px] = clr;
        x += ix; y += iy;
      }
    }
  }

  void PrepareTexture(PamTexture &tex) {
    tex.bHasAlpha = false;
    tex.bPow2     = false;
    if (!tex.IsValid()) return;

    auto isPow2 = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    tex.bPow2 = isPow2(tex.nWidth) && isPow2(tex.nHeight);

    uint32_t maxA = 0;
    size_t   cutout = 0;
    for (uint32_t p : tex.vPixels) {
      uint32_t a = (p >> 24) & 0xFF;
      if (a > maxA) maxA = a;
      if (a < (uint32_t)PAM_ALPHA_CUTOFF) cutout++;
    }

    if (maxA == 0) {
      // Alpha decoded as uniformly zero — the source has no usable alpha (or
      // the codec dropped it). Force opaque; alpha-testing would erase the
      // entire submesh.
      for (uint32_t &p : tex.vPixels) p |= 0xFF000000u;
      return;
    }

    // Enable cutout unless essentially everything would be discarded.
    tex.bHasAlpha = (cutout > 0) && (cutout < tex.vPixels.size() * 95 / 100);
  }

  void PamCamera::Orbit(float dYaw, float dPitch) {
    fYaw += dYaw;
    fPitch += dPitch;
    if (fPitch >  PITCH_MAX) fPitch =  PITCH_MAX;
    if (fPitch < -PITCH_MAX) fPitch = -PITCH_MAX;
    // keep yaw in a sane range so it never loses precision after long drags
    if (fYaw >  PI_F * 2.0f) fYaw -= PI_F * 2.0f;
    if (fYaw < -PI_F * 2.0f) fYaw += PI_F * 2.0f;
  }

  void PamCamera::Zoom(float factor) {
    fZoom *= factor;
    if (fZoom < 0.05f) fZoom = 0.05f;
    if (fZoom > 20.0f) fZoom = 20.0f;
  }

  uint32_t RenderModel(const PamModel &model, const PamCamera &cam,
                       PamTarget &target, uint32_t clrBackground,
                       PamShadeMode mode,
                       const std::vector<PamTexture> *pTextures) {
    if (target.pPixels == nullptr || target.nWidth < 2 || target.nHeight < 2)
      return 0;

    const int W = target.nWidth, H = target.nHeight;
    const size_t pixelCount = (size_t)W * H;

    std::fill_n(target.pPixels, pixelCount, clrBackground);
    // resize() keeps the allocation across frames; fill separately.
    target.vDepth.resize(pixelCount);
    std::fill_n(target.vDepth.data(), pixelCount, 1.0f);

    if (model.IsEmpty()) return 0;

    const bool wantTexture = (mode == PAM_SHADE_TEXTURED) && pTextures &&
                             pTextures->size() == model.vSubmeshes.size();

    // ── Camera basis ─────────────────────────────────────────────────────────
    float center[3];
    model.GetCenter(center);
    const float radius = model.GetRadius();

    const float cy = std::cos(cam.fYaw),   sy = std::sin(cam.fYaw);
    const float cp = std::cos(cam.fPitch), sp = std::sin(cam.fPitch);

    // Forward points from the eye toward the model centre.
    const float fwd[3] = { -cp * sy, -sp, -cp * cy };
    const float right[3] = { cy, 0.0f, -sy };
    const float up[3] = {
      right[1] * fwd[2] - right[2] * fwd[1],
      right[2] * fwd[0] - right[0] * fwd[2],
      right[0] * fwd[1] - right[1] * fwd[0]
    };

    const float fovY = 45.0f * PI_F / 180.0f;
    const float aspect = (float)W / (float)H;
    // Distance that fits the bounding sphere, then apply zoom.
    const float fitDist = radius / std::tan(fovY * 0.5f) * 1.15f;
    const float dist = fitDist / cam.fZoom;

    const float eye[3] = {
      center[0] - fwd[0] * dist,
      center[1] - fwd[1] * dist,
      center[2] - fwd[2] * dist
    };

    const float tanHalf = std::tan(fovY * 0.5f);
    // Scales with how close the camera is: roomy while the model is framed
    // (which keeps the linear depth range tight), and shrinking to almost
    // nothing once the camera is inside the bounding sphere, so zooming in
    // clips as little as possible. Triangles that do cross it are clipped, not
    // discarded, so the cut is a clean plane.
    const float zNear = (std::max)(radius * 0.001f, dist - radius * 2.0f) * 0.5f;
    const float zClip = (std::max)(zNear, 1e-6f);
    const float zFar  = dist + radius * 4.0f;

    // Pan shifts the projected image directly, in viewport fractions.
    const float panPxX = cam.fPanX * W;
    const float panPxY = cam.fPanY * H;

    // Headlight, slightly offset so curvature reads.
    float lightDir[3] = {
      fwd[0] * 0.6f + right[0] * 0.5f + up[0] * 0.35f,
      fwd[1] * 0.6f + right[1] * 0.5f + up[1] * 0.35f,
      fwd[2] * 0.6f + right[2] * 0.5f + up[2] * 0.35f
    };
    {
      float l = std::sqrt(lightDir[0] * lightDir[0] + lightDir[1] * lightDir[1] +
                          lightDir[2] * lightDir[2]);
      if (l > 1e-6f) { lightDir[0] /= l; lightDir[1] /= l; lightDir[2] /= l; }
    }

    // ── Transform every vertex once ──────────────────────────────────────────
    const size_t vcount = model.vVertices.size();
    static std::vector<ScreenVertex> sv;   // reused across frames
    static std::vector<ViewVertex>   vv;
    sv.resize(vcount);
    vv.resize(vcount);

    const float halfW = W * 0.5f, halfH = H * 0.5f;

    ProjParams pp;
    pp.halfW = halfW; pp.halfH = halfH;
    pp.tanHalf = tanHalf; pp.aspect = aspect;
    pp.panX = panPxX; pp.panY = panPxY;
    pp.zNear = zNear; pp.zFar = zFar;

    for (size_t i = 0; i < vcount; i++) {
      const PamVertex &v = model.vVertices[i];
      float rx = v.x - eye[0], ry = v.y - eye[1], rz = v.z - eye[2];

      ViewVertex &vo = vv[i];
      // View space (right-handed, camera looks down +fwd)
      vo.vx = rx * right[0] + ry * right[1] + rz * right[2];
      vo.vy = rx * up[0]    + ry * up[1]    + rz * up[2];
      vo.vz = rx * fwd[0]   + ry * fwd[1]   + rz * fwd[2];
      vo.u  = v.u;
      vo.v  = v.v;

      // Raw two-sided lambert term; the ambient/diffuse curve is applied at
      // shading time because textured and flat shading want different ones.
      float d = v.nx * lightDir[0] + v.ny * lightDir[1] + v.nz * lightDir[2];
      vo.light = (d < 0.0f) ? -d : d;

      // Only project what is in front of the near plane; anything behind is
      // handled per triangle by the clipper.
      if (vo.vz >= zClip) sv[i] = ProjectView(vo, pp);
    }

    // ── Rasterise ────────────────────────────────────────────────────────────
    uint32_t drawn = 0;
    float *depth = target.vDepth.data();
    uint32_t *px = target.pPixels;

    // Rasterises one screen-space triangle. Shared by the common all-in-front
    // path and by the fragments produced when a triangle is clipped.
    const PamTexture *tex = nullptr;
    float texW = 0.0f, texH = 0.0f;
    const uint8_t *tint = kSubmeshTint[0];

    auto rasterTri = [&](const ScreenVertex &A, const ScreenVertex &B,
                         const ScreenVertex &C) -> bool {
      {
        float area = (B.sx - A.sx) * (C.sy - A.sy) - (B.sy - A.sy) * (C.sx - A.sx);
        if (area == 0.0f) return false;

        // Render both facings — BDO meshes mix winding, and dropping one side
        // leaves holes in flat props.
        const ScreenVertex *p0 = &A, *p1 = &B, *p2 = &C;
        if (area < 0.0f) { p1 = &C; p2 = &B; area = -area; }

        int minX = (int)std::floor((std::min)({ p0->sx, p1->sx, p2->sx }));
        int maxX = (int)std::ceil ((std::max)({ p0->sx, p1->sx, p2->sx }));
        int minY = (int)std::floor((std::min)({ p0->sy, p1->sy, p2->sy }));
        int maxY = (int)std::ceil ((std::max)({ p0->sy, p1->sy, p2->sy }));

        if (minX < 0) minX = 0;
        if (minY < 0) minY = 0;
        if (maxX > W - 1) maxX = W - 1;
        if (maxY > H - 1) maxY = H - 1;
        if (minX > maxX || minY > maxY) return false;

        const float invArea = 1.0f / area;

        // Per-pixel x deltas of the three barycentric weights. These are small
        // and well conditioned, so stepping by them is safe; the *starting*
        // value of each scanline is still computed with the difference form
        // below, because the algebraically equivalent A*x + B*y + C form loses
        // most of its precision (screen coords are large, the result is ~0).
        const float A0 = -(p1->sy - p0->sy) * invArea;   // weight of p2
        const float A1 = -(p2->sy - p1->sy) * invArea;   // weight of p0
        const float A2 = -(p0->sy - p2->sy) * invArea;   // weight of p1

        // Anything interpolated from those weights shares their x delta.
        // (depth has no step: it is evaluated exactly, see the loop below)
        const float Al = A1 * p0->light  + A2 * p1->light  + A0 * p2->light;
        const float Ai = A1 * p0->invW   + A2 * p1->invW   + A0 * p2->invW;
        const float Au = A1 * p0->uOverW + A2 * p1->uOverW + A0 * p2->uOverW;
        const float Av = A1 * p0->vOverW + A2 * p1->vOverW + A0 * p2->vOverW;

        // Narrowing each scanline costs three divides, which only pays off once
        // a row is wide enough. Dense meshes are mostly sub-pixel triangles, so
        // below this width just walk the bounding box.
        const bool wideEnough = (maxX - minX) >= 8;

        for (int y = minY; y <= maxY; y++) {
          const float fy  = y + 0.5f;
          const float fx0 = minX + 0.5f;

          int xs = minX, xe = maxX;

          if (wideEnough) {
            // Accurate evaluation at the left edge of the bounding box.
            float e0 = ((p1->sx - p0->sx) * (fy - p0->sy) - (p1->sy - p0->sy) * (fx0 - p0->sx)) * invArea;
            float e1 = ((p2->sx - p1->sx) * (fy - p1->sy) - (p2->sy - p1->sy) * (fx0 - p1->sx)) * invArea;
            float e2 = ((p0->sx - p2->sx) * (fy - p2->sy) - (p0->sy - p2->sy) * (fx0 - p2->sx)) * invArea;

            // Narrow the scanline to where all three edges are non-negative.
            // Without this the loop walks the whole bounding box, which for a
            // typical triangle is about twice its actual area. Widened by an
            // epsilon so the explicit coverage test below stays authoritative.
            bool empty = false;
            const double kEps = 1e-3;
            const float edgeA[3] = { A0, A1, A2 };
            const float edgeW[3] = { e0, e1, e2 };
            for (int e = 0; e < 3 && !empty; e++) {
              if (edgeA[e] > 1e-9f || edgeA[e] < -1e-9f) {
                // Solve w + A*(x - minX) >= 0. Done in double and compared
                // before the int cast: a near-horizontal edge makes A tiny and
                // the quotient enormous, which would overflow the conversion.
                double bound = (double)minX - (double)edgeW[e] / (double)edgeA[e];
                if (edgeA[e] > 0.0f) {
                  double lof = std::ceil(bound - kEps);
                  if (lof > (double)xe)      empty = true;
                  else if (lof > (double)xs) xs = (int)lof;
                }
                else {
                  double hif = std::floor(bound + kEps);
                  if (hif < (double)xs)      empty = true;
                  else if (hif < (double)xe) xe = (int)hif;
                }
              }
              else if (edgeW[e] < 0.0f) {
                empty = true;   // edge is horizontal and this row is outside it
              }
            }
            if (empty || xs > xe) continue;
          }

          // Evaluate exactly at the span start, then step the cheap channels.
          const float fxs = xs + 0.5f;
          float w0 = ((p1->sx - p0->sx) * (fy - p0->sy) - (p1->sy - p0->sy) * (fxs - p0->sx)) * invArea;
          float w1 = ((p2->sx - p1->sx) * (fy - p1->sy) - (p2->sy - p1->sy) * (fxs - p1->sx)) * invArea;
          float w2 = ((p0->sx - p2->sx) * (fy - p2->sy) - (p0->sy - p2->sy) * (fxs - p2->sx)) * invArea;

          float z  = w1 * p0->depth  + w2 * p1->depth  + w0 * p2->depth;
          float lm = w1 * p0->light  + w2 * p1->light  + w0 * p2->light;
          float iw = w1 * p0->invW   + w2 * p1->invW   + w0 * p2->invW;
          float uw = w1 * p0->uOverW + w2 * p1->uOverW + w0 * p2->uOverW;
          float vw = w1 * p0->vOverW + w2 * p1->vOverW + w0 * p2->vOverW;

          uint32_t *rowPx = px + (size_t)y * W;
          float    *rowZ  = depth + (size_t)y * W;

          for (int x = xs; x <= xe; x++,
               lm += Al, iw += Ai, uw += Au, vw += Av) {
            // Coverage and depth are evaluated exactly, not stepped. Models in
            // this archive lay decals coplanar on terrain, so the depth test is
            // a near-tie there and incremental drift visibly flips which
            // surface wins. Lighting and texture coordinates are stepped —
            // their drift is far below one texel over a scanline.
            const float fx = x + 0.5f;
            w0 = ((p1->sx - p0->sx) * (fy - p0->sy) - (p1->sy - p0->sy) * (fx - p0->sx)) * invArea;
            w1 = ((p2->sx - p1->sx) * (fy - p1->sy) - (p2->sy - p1->sy) * (fx - p1->sx)) * invArea;
            w2 = ((p0->sx - p2->sx) * (fy - p2->sy) - (p0->sy - p2->sy) * (fx - p2->sx)) * invArea;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            z = w1 * p0->depth + w2 * p1->depth + w0 * p2->depth;
            if (z >= rowZ[x]) continue;

            // Flat tints are already bright, so they take a punchier curve.
            // Game textures are mostly dark, so lift the ambient floor and add
            // gain — otherwise iron and stone read as near-black.
            const float lit = tex ? (0.55f + 0.45f * lm) * 1.35f
                                  : (0.25f + 0.75f * lm);

            if (tex) {
              if (iw > 1e-12f) {
                // Perspective-correct: u/w and 1/w are linear in screen space.
                const float rw = 1.0f / iw;   // one divide, not two
                float u = uw * rw;
                float v = vw * rw;
                int r, g, b, a;
                SampleBilinear(*tex, u * texW, v * texH, r, g, b, a);

                // Alpha cutout (foliage, grates, decals). Discard before the
                // depth write, otherwise invisible texels still occlude what
                // is behind them and the cutout renders as black.
                if (tex->bHasAlpha && a < PAM_ALPHA_CUTOFF) continue;

                rowZ[x] = z;
                rowPx[x] = PackRGB((int)(r * lit), (int)(g * lit), (int)(b * lit));
                continue;
              }
            }
            rowZ[x] = z;
            rowPx[x] = PackRGB((int)(tint[0] * lit), (int)(tint[1] * lit),
                               (int)(tint[2] * lit));
          }
        }
      }
      return true;
    };

    // ── Walk the submeshes, clipping triangles that cross the near plane ─────
    for (size_t s = 0; s < model.vSubmeshes.size(); s++) {
      const PamSubmesh &sm = model.vSubmeshes[s];
      tint = kSubmeshTint[s % 8];

      tex  = nullptr;
      if (wantTexture && (*pTextures)[s].IsValid()) tex = &(*pTextures)[s];
      texW = tex ? (float)tex->nWidth  : 0.0f;
      texH = tex ? (float)tex->nHeight : 0.0f;

      const uint32_t last = sm.uiBaseIndex + sm.uiIndexCount;
      for (uint32_t i = sm.uiBaseIndex; i + 2 < last; i += 3) {
        const uint32_t i0 = model.vIndices[i + 0];
        const uint32_t i1 = model.vIndices[i + 1];
        const uint32_t i2 = model.vIndices[i + 2];

        const int inFront = (vv[i0].vz >= zClip) + (vv[i1].vz >= zClip) +
                            (vv[i2].vz >= zClip);
        if (inFront == 0) continue;               // wholly behind the camera

        if (inFront == 3) {                       // common case, already projected
          if (rasterTri(sv[i0], sv[i1], sv[i2])) drawn++;
          continue;
        }

        // Straddles the near plane: clip, then project the fragment. Dropping
        // it instead would make whole polygons vanish as the camera moves in.
        const ViewVertex tri[3] = { vv[i0], vv[i1], vv[i2] };
        ViewVertex poly[4];
        const int n = ClipNear(tri, zClip, poly);
        if (n < 3) continue;

        ScreenVertex clipped[4];
        for (int k = 0; k < n; k++) clipped[k] = ProjectView(poly[k], pp);

        if (rasterTri(clipped[0], clipped[1], clipped[2])) drawn++;
        if (n == 4 && rasterTri(clipped[0], clipped[2], clipped[3])) drawn++;
      }
    }

    // ── Optional wireframe overlay ───────────────────────────────────────────
    if (mode == PAM_SHADE_WIREFRAME) {
      const uint32_t wire = PackRGB(40, 40, 44);
      for (size_t i = 0; i + 2 < model.vIndices.size(); i += 3) {
        const uint32_t i0 = model.vIndices[i + 0];
        const uint32_t i1 = model.vIndices[i + 1];
        const uint32_t i2 = model.vIndices[i + 2];
        // Wireframe is a debug overlay; skip anything crossing the near plane
        // rather than clipping it a second time.
        if (vv[i0].vz < zClip || vv[i1].vz < zClip || vv[i2].vz < zClip) continue;
        const ScreenVertex &A = sv[i0];
        const ScreenVertex &B = sv[i1];
        const ScreenVertex &C = sv[i2];
        DrawLine(target, A.sx, A.sy, B.sx, B.sy, wire);
        DrawLine(target, B.sx, B.sy, C.sx, C.sy, wire);
        DrawLine(target, C.sx, C.sy, A.sx, A.sy, wire);
      }
    }

    return drawn;
  }

}
