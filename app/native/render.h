// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
// native_window/external_window.h 的 `typedef struct NativeWindow OHNativeWindow;`
// 的全局前置声明：本头文件不引入 OHOS 头，签名里用 NativeWindow*。
struct NativeWindow;
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
//                  steady_clock 微秒），即「画面提交后多久才被取走」；Surface
//                  （零拷贝）源下它是「帧可用回调 -> 完成 UpdateSurfaceImage」；
//   pickupToSwap   取到帧到 eglSwapBuffers 返回（含拷贝/上传/绘制）；Surface 源
//                  下是 UpdateSurfaceImage 完成到 swap 返回；
//   frameAvailableToSwapAvgMs/P95Ms  仅 Surface 源有意义：帧可用回调到 swap 返回
//                  （上面两段之和，含 eglSwapInterval(1) 等待的 vsync）。
//   captureToSwap   端到端延迟：swap 返回时刻 - 最近一帧的主机抓屏时刻（把
//                  presentationTimeUs 用客户端估计的主机/客户端时钟偏移换算到
//                  客户端时钟）；主机 PTS 无效或结果异常时退化为 receiveToSwap
//                  （swap 返回 - 首包接收时刻），latencySource 标明用的哪一个。
struct RenderFramePerf {
  RenderFramePerf()
      : outFps(0.0),
        lagAvgMs(0.0),
        lagP95Ms(0.0),
        pickupToSwapAvgMs(0.0),
        pickupToSwapP95Ms(0.0),
        frameAvailableToSwapAvgMs(0.0),
        frameAvailableToSwapP95Ms(0.0),
        captureToSwapAvgMs(0.0),
        captureToSwapP95Ms(0.0),
        receiveToSwapAvgMs(0.0),
        receiveToSwapP95Ms(0.0),
        latencySource(0),
        windowMs(0) {}
  double outFps;
  double lagAvgMs;
  double lagP95Ms;
  double pickupToSwapAvgMs;
  double pickupToSwapP95Ms;
  double frameAvailableToSwapAvgMs;
  double frameAvailableToSwapP95Ms;
  double captureToSwapAvgMs;
  double captureToSwapP95Ms;
  double receiveToSwapAvgMs;
  double receiveToSwapP95Ms;
  int latencySource;           // 0 = 无样本, 1 = presentationTimeUs, 2 = receiveTimeUs
  unsigned long long windowMs;  // 0 = 还没有发布过
};
RenderFramePerf renderFramePerf();

// 零拷贝视频源（Sunshine 的 AVCodec Surface 模式）。解码器直接渲染进
// OH_NativeImage 的 OHNativeWindow，渲染线程把同一块画面当 OES 外部纹理解码出来：
// 没有 NV12->BGRA 转换、没有整帧镜像拷贝、没有 glTexSubImage2D 上传。
//
// renderCreateExternalSource: 等到本会话的渲染 surface attach（最多 10 秒；解码
// 线程调用，会阻塞），然后在渲染线程（GL 上下文当前）建 OES 纹理与 OH_NativeImage，
// 返回纹理 id 与给 OH_VideoDecoder_SetSurface 用的窗口。失败返回 false，调用方必须
// 回退到 Buffer 模式。成功后该会话的渲染线程不再读 BGRA 镜像。
// renderSetExternalSourceSize: 视频路径报告有效画面尺寸，作为 contain 几何的纹理
// 尺寸（同时发布 texWidth/texHeight，坐标映射继续走 renderStats）。可在任意线程调。
// renderReleaseExternalSource: 在渲染线程销毁 NativeImage 与纹理；调用方必须已经
// 停止解码器（否则解码器会向已销毁的窗口提交帧）。会话没有渲染器时是空操作。
bool renderCreateExternalSource(const std::string& sessionId, unsigned int* textureId,
                                NativeWindow** nativeWindow);
void renderSetExternalSourceSize(const std::string& sessionId, int width, int height);
void renderReleaseExternalSource(const std::string& sessionId);

// 按会话设置 EGL swap interval（0 = 不等垂直同步，1 = 等 vsync，默认 1）。只对
// 该会话生效，在渲染线程（GL 上下文当前）执行；其它会话的渲染器不受影响。
// Sunshine 的 sunshineVsync=false 会在这里传 0；释放外部纹理源时恢复为 1，
// 保证 Buffer 回退路径与 VNC/RDP 仍是 vsync 节流。
void renderSetSwapInterval(const std::string& sessionId, int interval);
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
