// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-side unit test for app/native/view_geometry.h.
//
// Standard library only: this binary must build and run on the development
// host without the OHOS EGL/GLES headers or any device. It is wired into CTest
// by app/native/CMakeLists.txt in the host (non-OHOS) branch only.

#include <cmath>
#include <cstdio>

#include "../view_geometry.h"

namespace {

int gChecks = 0;
int gFailures = 0;

void Check(bool ok, const char* expression, int line) {
  ++gChecks;
  if (!ok) {
    ++gFailures;
    std::printf("FAIL: %s (line %d)\n", expression, line);
  }
}

void CheckNear(double actual, double expected, double tolerance, const char* expression,
               int line) {
  ++gChecks;
  if (!(std::fabs(actual - expected) <= tolerance)) {
    ++gFailures;
    std::printf("FAIL: %s (line %d): actual %.9f expected %.9f tolerance %.9f\n",
                expression, line, actual, expected, tolerance);
  }
}

}  // namespace

#define CHECK(cond) Check((cond), #cond, __LINE__)
#define CHECK_NEAR(actual, expected, tolerance) \
  CheckNear((actual), (expected), (tolerance), #actual " ~= " #expected, __LINE__)

int main() {
  using tianguan::ClampViewZoom;
  using tianguan::ComputeViewGeometry;
  using tianguan::kMaxViewZoom;
  using tianguan::kMinViewZoom;
  using tianguan::RoundViewportCoord;
  using tianguan::ViewGeometry;

  // --- zoom == 1 with a non-zero pan: the contain layout is still centred ---
  {
    const ViewGeometry g = ComputeViewGeometry(800, 600, 1000, 1000, 1.0, 30.0, -20.0);
    CHECK_NEAR(g.zoom, 1.0, 1e-12);
    CHECK_NEAR(g.baseScale, 1.25, 1e-6);
    CHECK_NEAR(g.scale, 1.25, 1e-6);
    CHECK(g.contentW == 1000);
    CHECK(g.contentH == 750);
    CHECK_NEAR(g.left, 0.0, 0.0);   // (1000 - 1000) / 2, pan ignored
    CHECK_NEAR(g.top, 125.0, 0.0);  // (1000 - 750) / 2, pan ignored
    CHECK_NEAR(g.panX, 0.0, 0.0);
    CHECK_NEAR(g.panY, 0.0, 0.0);
    CHECK_NEAR(g.contentScale, 1.25, 1e-12);   // contentW / fbWidth
    CHECK_NEAR(g.contentScaleY, 1.25, 1e-12);  // contentH / fbHeight
  }

  // --- zoom > 1, content overflows: pan is clamped to [surf - content, 0] ---
  {
    // fb 800x600 on a 1000x1000 surface, zoom 2: content 2000x1500,
    // centred offsets left=-500 top=-250.
    const ViewGeometry centred = ComputeViewGeometry(800, 600, 1000, 1000, 2.0, 0.0, 0.0);
    CHECK(centred.contentW == 2000);
    CHECK(centred.contentH == 1500);
    CHECK_NEAR(centred.left, -500.0, 0.0);
    CHECK_NEAR(centred.top, -250.0, 0.0);

    const ViewGeometry moved = ComputeViewGeometry(800, 600, 1000, 1000, 2.0, 100.0, 100.0);
    CHECK_NEAR(moved.left, -400.0, 0.0);
    CHECK_NEAR(moved.top, -150.0, 0.0);
    CHECK_NEAR(moved.panX, 100.0, 0.0);
    CHECK_NEAR(moved.panY, 100.0, 0.0);

    // Far past both limits: clamped to the low edge, never showing a gap.
    const ViewGeometry low = ComputeViewGeometry(800, 600, 1000, 1000, 2.0, -10000.0, -10000.0);
    CHECK_NEAR(low.left, -1000.0, 0.0);  // surfW - contentW
    CHECK_NEAR(low.top, -500.0, 0.0);    // surfH - contentH
    CHECK(low.left <= 0.0 && low.left >= 1000.0 - 2000.0);
    CHECK(low.top <= 0.0 && low.top >= 1000.0 - 1500.0);

    // Far past the high limit: pinned to 0 (the leading edge).
    const ViewGeometry high = ComputeViewGeometry(800, 600, 1000, 1000, 2.0, 10000.0, 10000.0);
    CHECK_NEAR(high.left, 0.0, 0.0);
    CHECK_NEAR(high.top, 0.0, 0.0);
    CHECK_NEAR(high.panX, 500.0, 0.0);   // 0 - centredLeft
    CHECK_NEAR(high.panY, 250.0, 0.0);   // 0 - centredTop
    CHECK(high.left <= 0.0 && high.left >= 1000.0 - 2000.0);
    CHECK(high.top <= 0.0 && high.top >= 1000.0 - 1500.0);
  }

  // --- content smaller than the surface: forced centring, pan has no effect ---
  {
    // fb 100x100 on a 1000x800 surface with zoom 1: base 8, content 800x800.
    const ViewGeometry g = ComputeViewGeometry(100, 100, 1000, 800, 1.0, 50.0, 50.0);
    CHECK(g.contentW == 800);
    CHECK(g.contentH == 800);
    CHECK_NEAR(g.left, 100.0, 0.0);  // (1000 - 800) / 2
    CHECK_NEAR(g.top, 0.0, 0.0);     // (800 - 800) / 2
    CHECK_NEAR(g.panX, 0.0, 0.0);
    CHECK_NEAR(g.panY, 0.0, 0.0);
  }

  // --- zoom clamp: [1, 4]; NaN/inf fall back to 1 ---
  {
    CHECK_NEAR(ClampViewZoom(1.0), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(1.5), 1.5, 0.0);
    CHECK_NEAR(ClampViewZoom(2.5), 2.5, 0.0);
    CHECK_NEAR(ClampViewZoom(4.0), 4.0, 0.0);
    CHECK_NEAR(ClampViewZoom(kMinViewZoom), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(kMaxViewZoom), 4.0, 0.0);
    CHECK_NEAR(ClampViewZoom(0.5), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(-3.0), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(5.0), 4.0, 0.0);
    CHECK_NEAR(ClampViewZoom(1.0e9), 4.0, 0.0);
    CHECK_NEAR(ClampViewZoom(std::nan("")), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(INFINITY), 1.0, 0.0);
    CHECK_NEAR(ClampViewZoom(-INFINITY), 1.0, 0.0);
  }

  // --- NaN zoom must produce the plain contain layout ---
  {
    const ViewGeometry broken = ComputeViewGeometry(800, 600, 1000, 1000, std::nan(""), 0.0, 0.0);
    const ViewGeometry contain = ComputeViewGeometry(800, 600, 1000, 1000, 1.0, 0.0, 0.0);
    CHECK_NEAR(broken.zoom, 1.0, 0.0);
    CHECK_NEAR(broken.scale, contain.scale, 0.0);
    CHECK(broken.contentW == contain.contentW);
    CHECK(broken.contentH == contain.contentH);
    CHECK_NEAR(broken.left, contain.left, 0.0);
    CHECK_NEAR(broken.top, contain.top, 0.0);

    const ViewGeometry infiniteZoom = ComputeViewGeometry(800, 600, 1000, 1000, INFINITY, 0.0, 0.0);
    CHECK_NEAR(infiniteZoom.zoom, 1.0, 0.0);
    CHECK_NEAR(infiniteZoom.left, 0.0, 0.0);
  }

  // --- NaN pan is treated as 0 (only meaningful when content overflows) ---
  {
    const ViewGeometry g = ComputeViewGeometry(800, 600, 1000, 1000, 2.0, std::nan(""), std::nan(""));
    CHECK_NEAR(g.left, -500.0, 0.0);  // centred, as if pan were 0
    CHECK_NEAR(g.top, -250.0, 0.0);
    CHECK_NEAR(g.panX, 0.0, 0.0);
    CHECK_NEAR(g.panY, 0.0, 0.0);
  }

  // --- rotation: surface width/height swapped, geometry recomputed ---
  {
    // fb 800x600 on 1000x600 (landscape): base 1.0, content 800x600.
    const ViewGeometry landscape = ComputeViewGeometry(800, 600, 1000, 600, 1.0, 25.0, 25.0);
    CHECK_NEAR(landscape.baseScale, 1.0, 1e-6);
    CHECK(landscape.contentW == 800);
    CHECK(landscape.contentH == 600);
    CHECK_NEAR(landscape.left, 100.0, 0.0);
    CHECK_NEAR(landscape.top, 0.0, 0.0);

    // Same framebuffer on 600x1000 (portrait): base 0.75, content 600x450.
    const ViewGeometry portrait = ComputeViewGeometry(800, 600, 600, 1000, 1.0, 25.0, 25.0);
    CHECK_NEAR(portrait.baseScale, 0.75, 1e-6);
    CHECK(portrait.contentW == 600);
    CHECK(portrait.contentH == 450);
    CHECK_NEAR(portrait.left, 0.0, 0.0);
    CHECK_NEAR(portrait.top, 275.0, 0.0);  // (1000 - 450) / 2, pan ignored
    CHECK_NEAR(portrait.panX, 0.0, 0.0);
    CHECK_NEAR(portrait.panY, 0.0, 0.0);

    // Portrait with zoom 2: width overflows (600 -> 1200) but height does not
    // (450 -> 900 <= 1000), so only the horizontal pan applies.
    const ViewGeometry portraitZoom = ComputeViewGeometry(800, 600, 600, 1000, 2.0, 100.0, 1000.0);
    CHECK(portraitZoom.contentW == 1200);
    CHECK(portraitZoom.contentH == 900);
    CHECK_NEAR(portraitZoom.left, -200.0, 0.0);  // -300 + 100
    CHECK_NEAR(portraitZoom.top, 50.0, 0.0);     // centred; vertical pan ignored
  }

  // --- renderStats input mapping: surface pixel -> remote pixel ---
  {
    // fb 800x600 on 1600x1200 at zoom 1.5: base 2, scale 3, content 2400x1800,
    // left -400, top -300.
    const ViewGeometry g = ComputeViewGeometry(800, 600, 1600, 1200, 1.5, 0.0, 0.0);
    CHECK_NEAR(g.scale, 3.0, 1e-6);
    CHECK(g.contentW == 2400);
    CHECK(g.contentH == 1800);
    CHECK_NEAR(g.left, -400.0, 0.0);
    CHECK_NEAR(g.top, -300.0, 0.0);
    CHECK_NEAR(g.contentScale, 3.0, 1e-12);
    CHECK_NEAR(g.contentScaleY, 3.0, 1e-12);

    // The ArkTS mapping is remote = (surface - contentLeft) / contentScale.
    const auto remoteX = [](const ViewGeometry& v, double x) {
      return (x - v.left) / v.contentScale;
    };
    const auto remoteY = [](const ViewGeometry& v, double y) {
      return (y - v.top) / v.contentScaleY;
    };
    CHECK_NEAR(remoteX(g, 700.0), 1100.0 / 3.0, 1e-12);
    CHECK_NEAR(remoteY(g, 300.0), 200.0, 1e-12);
    // Content corners map exactly to framebuffer corners.
    CHECK_NEAR(remoteX(g, g.left), 0.0, 1e-12);
    CHECK_NEAR(remoteY(g, g.top), 0.0, 1e-12);
    CHECK_NEAR(remoteX(g, g.left + g.contentW), 800.0, 1e-9);
    CHECK_NEAR(remoteY(g, g.top + g.contentH), 600.0, 1e-9);
  }

  // --- RoundViewportCoord matches glViewport's integer grid ---
  {
    CHECK(RoundViewportCoord(0.0) == 0);
    CHECK(RoundViewportCoord(2.4) == 2);
    CHECK(RoundViewportCoord(2.5) == 3);
    CHECK(RoundViewportCoord(3.5) == 4);
    CHECK(RoundViewportCoord(-0.5) == 0);
    CHECK(RoundViewportCoord(-1.5) == -1);
    CHECK(RoundViewportCoord(-0.51) == -1);
  }

  // --- invalid dimensions return the default (contain) geometry ---
  {
    const ViewGeometry zero = ComputeViewGeometry(0, 600, 1000, 1000, 2.0, 10.0, 10.0);
    CHECK_NEAR(zero.zoom, 1.0, 0.0);
    CHECK(zero.contentW == 0);
    CHECK(zero.contentH == 0);
    CHECK_NEAR(zero.left, 0.0, 0.0);
    CHECK_NEAR(zero.top, 0.0, 0.0);
    CHECK_NEAR(zero.scale, 1.0, 0.0);

    const ViewGeometry noSurface = ComputeViewGeometry(800, 600, 0, 0, 2.0, 10.0, 10.0);
    CHECK(noSurface.contentW == 0);
    CHECK(noSurface.contentH == 0);
  }

  if (gFailures == 0) {
    std::printf("view_geometry_test: %d checks passed\n", gChecks);
    return 0;
  }
  std::printf("view_geometry_test: %d/%d checks FAILED\n", gFailures, gChecks);
  return 1;
}
