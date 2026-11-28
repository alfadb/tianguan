// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
namespace tianguan {

// Remote-page presenter: one native EGL/GLES render thread per session,
// attached to the OHNativeWindow behind that session's XComponent(SURFACE).
//
// The Canvas + drawImage path handed every frame to ArkUI, so RenderService
// cached a texture plus a Vulkan buffer per picture version until the 4096 MB
// application GPU quota killed the process. This renderer owns ONE fixed
// texture, re-uploads only what changed and presents with eglSwapBuffers, so
// its GPU allocation is independent of the number of frames (verified by the
// glspike experiment, .work/glspike/REPORT.md).
//
// Frame source: the renderer is the session's frame consumer. It pulls the
// BGRA mirror through takeFrameStruct() (same lock and delivery baseline as
// the ArkTS takeFrame), packs changed rows into a staging buffer and uploads
// them with glTexSubImage2D. The ArkTS page must therefore NOT call
// takeFrame() while this renderer is attached.
//
// Layout: contain scaling with black letterbox bars, recomputed from the
// actual EGL surface size every frame, so a window resize re-fits the picture
// by itself. On top of that fit sits a per-session view transform: zoom
// (1.0 = the contain fit, up to kMaxViewZoom) and pan in surface pixels.
// Zooming out below the contain fit is impossible; pan is ignored in an axis
// where the content is still smaller than the surface (centred), and clamped
// to the content edges otherwise, so no blank strip can ever be dragged in.
// Geometry (content rectangle, scale) is the single source of truth shared by
// the viewport and renderStats(), see view_geometry.h.
//
// Lifecycle: attach on XComponent surface creation, detach on surface/page
// destruction. detach() only requests a stop and returns immediately — a
// reaper thread joins the render thread, so UI callbacks never wait on a
// swap that may be stuck in the driver. setRenderSurfaceSuspended() parks
// the thread while the window is hidden so nothing swaps into an invisible
// surface; attach() takes the initial suspend state (a surface created while
// the window is hidden starts parked).
//
// EGL display: one process-wide EGLDisplay, initialized once and never
// terminated (eglInitialize is not per-consumer; one renderer terminating it
// would kill every other renderer's context). Each renderer destroys only
// its own EGLSurface, EGLContext and native window. SharedEGLDisplay()
// exposes that display to the spike experiment for the same reason; it
// returns void* so headers stay free of EGL includes.
std::string attachRenderSurface(const std::string& sessionId, const std::string& surfaceId,
                                bool startSuspended = false);
std::string detachRenderSurface(const std::string& sessionId);
// JSON snapshot for diagnostics: state, fps, uploads, skipped polls, surface
// and texture size, EGL error, and the presented geometry (contentLeft/
// contentTop/contentScale/contentScaleY/contentW/contentH/zoom/panX/panY/
// maxZoom) ArkTS maps input from. "ERROR:NO_RENDERER" when not attached.
std::string renderStats(const std::string& sessionId);

// 渲染线程侧的分段耗时快照：最近一个有帧的 5 秒窗口，进程级只读（同一时刻只有
// 一个会话在串流，多个渲染器时以最后发布的为准；没有渲染器时全 0）。
//   outFps         实际上屏帧率（成功 eglSwapBuffers 的次数 / 窗口长度）；
//   lagAvgMs/P95Ms 取到帧的时刻减镜像提交时间戳（FrameGrab::commitTimeUs，
//                  steady_clock 微秒），即「画面提交后多久才被取走」；
//   pickupToSwap   取到帧到 eglSwapBuffers 返回（含拷贝/上传/绘制）。
struct RenderFramePerf {
  RenderFramePerf()
      : outFps(0.0),
        lagAvgMs(0.0),
        lagP95Ms(0.0),
        pickupToSwapAvgMs(0.0),
        pickupToSwapP95Ms(0.0),
        windowMs(0) {}
  double outFps;
  double lagAvgMs;
  double lagP95Ms;
  double pickupToSwapAvgMs;
  double pickupToSwapP95Ms;
  unsigned long long windowMs;  // 0 = 还没有发布过
};
RenderFramePerf renderFramePerf();
// Sets this session's view transform. zoom is clamped to [1, kMaxViewZoom];
// panX/panY are surface-pixel offsets from the centred position and are
// clamped against the live surface size while drawing. Returns "OK" or
// "ERROR:NO_RENDERER". The idle render loop redraws the current texture once
// even if no new frame arrives.
std::string setViewTransform(const std::string& sessionId, double zoom, double panX,
                             double panY);
// true = park the render thread (window hidden), false = resume (window
// shown). Returns OK even when no renderer is attached; that state has no
// thread to park.
std::string setRenderSurfaceSuspended(const std::string& sessionId, bool suspended);
// The process-wide EGLDisplay (EGL_NO_DISPLAY as nullptr on failure).
void* SharedEGLDisplay();

}  // namespace tianguan
