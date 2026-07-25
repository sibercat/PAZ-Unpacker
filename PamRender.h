#pragma once

#ifndef _PAM_RENDER_H_
#define _PAM_RENDER_H_

#include <cstdint>
#include <vector>

#include "PamModel.h"

namespace kukdh1 {

  // Orbit camera around the model's bounding-box centre.
  struct PamCamera {
    float fYaw;        // radians
    float fPitch;      // radians, clamped to +/- ~89 degrees
    float fZoom;       // multiplier on the fit distance (1.0 = whole model visible)
    float fPanX;       // screen-space pan, in fractions of the viewport
    float fPanY;

    PamCamera() : fYaw(0.7f), fPitch(0.45f), fZoom(1.0f), fPanX(0.0f), fPanY(0.0f) {}

    void Reset() { *this = PamCamera(); }
    void Orbit(float dYaw, float dPitch);
    void Zoom(float factor);
  };

  // Decoded texture for a submesh. Pixels are 0xAARRGGBB, top-down.
  // Dimensions are capped at PAM_TEXTURE_MAX by the loader — this is a preview,
  // and small textures keep the sampler cache-friendly.
  constexpr int PAM_TEXTURE_MAX = 512;

  struct PamTexture {
    int                   nWidth;
    int                   nHeight;
    std::vector<uint32_t> vPixels;
    bool                  bHasAlpha;   // true if the alpha channel is a real cutout mask
    bool                  bPow2;       // both dimensions are powers of two -> mask wrapping

    PamTexture() : nWidth(0), nHeight(0), bHasAlpha(false), bPow2(false) {}
    bool IsValid() const {
      return nWidth > 0 && nHeight > 0 &&
             vPixels.size() == (size_t)nWidth * nHeight;
    }
  };

  // Texels below this alpha are discarded when a texture has a cutout mask.
  constexpr int PAM_ALPHA_CUTOFF = 128;

  // Finalises a freshly decoded texture: sets bHasAlpha (repairing textures
  // whose alpha decoded as uniformly zero, which would otherwise erase the
  // whole submesh) and bPow2. Must be called once after filling vPixels.
  void PrepareTexture(PamTexture &tex);

  enum PamShadeMode {
    PAM_SHADE_SOLID,      // lit, per-submesh tint
    PAM_SHADE_TEXTURED,   // lit, sampled from the submesh texture where available
    PAM_SHADE_WIREFRAME   // lit solid + wireframe overlay
  };

  // 32-bit BGRA render target. pPixels points at a top-down DIB section owned
  // by the caller (stride is assumed to be nWidth * 4). Keep the same instance
  // alive across frames so the depth buffer is not reallocated every time.
  struct PamTarget {
    uint32_t *pPixels;
    int       nWidth;
    int       nHeight;
    std::vector<float> vDepth;

    PamTarget() : pPixels(nullptr), nWidth(0), nHeight(0) {}
  };

  // Rasterises the model into the target, clearing to clrBackground first.
  // pTextures, when non-null, must hold one entry per submesh; invalid entries
  // fall back to the flat tint. Returns the number of triangles drawn.
  uint32_t RenderModel(const PamModel &model, const PamCamera &cam,
                       PamTarget &target, uint32_t clrBackground,
                       PamShadeMode mode,
                       const std::vector<PamTexture> *pTextures = nullptr);

}

#endif
