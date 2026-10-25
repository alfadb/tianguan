// SPDX-License-Identifier: GPL-3.0-or-later
//
// View geometry for the remote-page presenter (render.cxx).
//
// This header is the ONE source of truth for how a remote framebuffer is laid
// out on the EGL surface. Both the render loop (glViewport) and renderStats()
// call ComputeViewGeometry(), so the geometry ArkTS uses for input mapping can
// never drift from what was actually drawn. It is header-only and depends only
// on the C++ standard library, so a host-side unit test can include it without
// the OHOS EGL/GLES headers.
#pragma once

#include <algorithm>
#include <cmath>

namespace tianguan {

// zoom is a multiple of the contain fit: 1.0 = exactly the old letterboxed
// layout, 4.0 = four times that linear scale. It can never go below 1, so the
// picture always covers at least the old contain rectangle and can never be
// smaller than the window in either axis.
constexpr double kMinViewZoom = 1.0;
constexpr double kMaxViewZoom = 4.0;

struct ViewGeometry {
  double baseScale = 1.0;  // contain scale before zoom
  double scale = 1.0;      // effective scale applied to the framebuffer
  int contentW = 0;        // drawn width in surface pixels
  int contentH = 0;        // drawn height in surface pixels
  double left = 0.0;       // drawn left edge, surface pixels, top-left origin
  double top = 0.0;        // drawn top edge, surface pixels, top-left origin
  double zoom = 1.0;       // clamped zoom actually used
  double panX = 0.0;       // effective pan after clamping (surface pixels)
  double panY = 0.0;
  double contentScale = 1.0;   // contentW / framebuffer width (x mapping)
  double contentScaleY = 1.0;  // contentH / framebuffer height (y mapping)
};

// Clamps a requested zoom into [kMinViewZoom, kMaxViewZoom]; a non-finite
// request (NaN/inf from a broken caller) falls back to the contain layout.
inline double ClampViewZoom(double zoom) {
  if (!std::isfinite(zoom)) return kMinViewZoom;
  if (zoom < kMinViewZoom) return kMinViewZoom;
  if (zoom > kMaxViewZoom) return kMaxViewZoom;
  return zoom;
}

// Rounds a surface-pixel coordinate to the nearest integer for glViewport.
// At zoom == 1 and pan == 0 every input is already integral, so the legacy
// contain layout is reproduced bit for bit.
inline int RoundViewportCoord(double value) {
  return static_cast<int>(std::floor(value + 0.5));
}

// Computes the presented rectangle for a framebuffer of fbWidth x fbHeight on
// a surfWidth x surfHeight surface.
//
//   baseScale = min(surfW/fbW, surfH/fbH)
//   scale     = baseScale * clamp(zoom)
//   contentW  = fbW * scale, contentH = fbH * scale
//   left      = (surfW - contentW) / 2 + panX
//   top       = (surfH - contentH) / 2 + panY
//
// Clamping (after the zoom clamp, before drawing):
//   contentW <= surfW -> horizontally centred, panX ignored
//   contentW >  surfW -> left in [surfW - contentW, 0]   (never shows a gap)
// and the same vertically. The centred term deliberately uses integer
// division, matching the pre-transform renderer exactly at zoom == 1.
inline ViewGeometry ComputeViewGeometry(int fbWidth, int fbHeight, int surfWidth,
                                        int surfHeight, double requestedZoom,
                                        double panX, double panY) {
  ViewGeometry geometry;
  if (fbWidth <= 0 || fbHeight <= 0 || surfWidth <= 0 || surfHeight <= 0) {
    return geometry;
  }
  geometry.zoom = ClampViewZoom(requestedZoom);
  // Float arithmetic on purpose: at zoom == 1 this yields exactly the scale,
  // content size and offsets the contain renderer used before the transform.
  const float base = std::min(static_cast<float>(surfWidth) / static_cast<float>(fbWidth),
                              static_cast<float>(surfHeight) / static_cast<float>(fbHeight));
  const float scale = base * static_cast<float>(geometry.zoom);
  geometry.baseScale = base;
  geometry.scale = scale;
  geometry.contentW = std::max(1, static_cast<int>(fbWidth * scale + 0.5f));
  geometry.contentH = std::max(1, static_cast<int>(fbHeight * scale + 0.5f));

  const int centeredLeft = (surfWidth - geometry.contentW) / 2;
  const int centeredTop = (surfHeight - geometry.contentH) / 2;
  if (geometry.contentW <= surfWidth) {
    geometry.left = centeredLeft;
  } else {
    const double minLeft = surfWidth - geometry.contentW;
    double left = centeredLeft + (std::isfinite(panX) ? panX : 0.0);
    if (left < minLeft) left = minLeft;
    if (left > 0.0) left = 0.0;
    geometry.left = left;
  }
  if (geometry.contentH <= surfHeight) {
    geometry.top = centeredTop;
  } else {
    const double minTop = surfHeight - geometry.contentH;
    double top = centeredTop + (std::isfinite(panY) ? panY : 0.0);
    if (top < minTop) top = minTop;
    if (top > 0.0) top = 0.0;
    geometry.top = top;
  }
  // Effective pan = how far the clamped rectangle is from the centred one.
  geometry.panX = geometry.left - centeredLeft;
  geometry.panY = geometry.top - centeredTop;

  // Ratios of the DRAWN rectangle to the framebuffer, for input mapping:
  // remote = (surface - contentLeft) / contentScale. Using the rounded
  // content size keeps the mapping exact for the pixels on screen.
  geometry.contentScale = static_cast<double>(geometry.contentW) / static_cast<double>(fbWidth);
  geometry.contentScaleY = static_cast<double>(geometry.contentH) / static_cast<double>(fbHeight);
  return geometry;
}

}  // namespace tianguan
