// SPDX-License-Identifier: GPL-3.0-or-later
//
// Remote-page presenter: XComponent(SURFACE) + native EGL/GLES render thread,
// one per session (see render.h). Evolved from the glspike experiment
// (.work/glspike/REPORT.md), which proved this path keeps the application GPU
// allocation flat where Canvas + drawImage leaked one cached texture +
// Vulkan buffer per frame into RenderService.
#include "diagnostics.h"
#include "render.h"
#include "sunshine_backend.h"
#include "view_geometry.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <hilog/log.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* kTag = "TianguanRender";
const unsigned int kDomain = 0x0000;
// Poll cadence while waiting for the next frame. A NONE poll is one mutex +
// a few compares; 250 wakeups/s is negligible next to the 60 fps swap loop
// the spike ran, and catches a new committed version within a few ms.
const int kIdlePollMs = 4;
// Present errors are retried a few times (a resize can transiently fail a
// swap) before the thread gives up; ArkTS re-attaches when that happens.
const int kMaxSwapFailures = 8;
// A surface that reports no usable geometry for this many polls (~1 s at the
// idle cadence) is treated as dead; the renderer fails and ArkTS re-attaches.
const int kMaxSurfaceQueryFailures = 250;
// How often the stats snapshot is refreshed while no frames arrive, so an
// idle renderer still reports a current state/fps (decaying to 0).
const int kStatsPublishMs = 250;

long long NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// 帧延迟分段统计（5 秒窗口）
//
//   lag           取到帧的时刻 - 镜像提交时间戳（FrameGrab::commitTimeUs）：
//                 画面在镜像里等了多久才被渲染线程取走；
//   pickupToSwap  取到帧到 eglSwapBuffers 返回（拷贝 + 上传 + 绘制 + 提交给
//                 合成器），即渲染线程自己这一段；
//   outFps        窗口内真正上屏（swap 成功）的帧率。
//
// 每帧只做两次 steady_clock 读取和一次直方图自增；P95 用 1 ms 一桶的直方图求，
// 不排序、不留无界样本。结果以进程级快照发布（render.h 的 RenderFramePerf），
// 后端 5 秒汇总行和 statusJson.perf 在任意线程读。
// ---------------------------------------------------------------------------
const size_t kFramePerfBucketCount = 2001;  // 0..1999 ms + >=2000 ms 溢出桶

struct FramePerfHistogram {
  FramePerfHistogram() : count(0), sumUs(0) {
    std::memset(buckets, 0, sizeof(buckets));
  }
  void add(long long us) {
    if (us < 0) return;
    ++count;
    sumUs += us;
    size_t bucket = static_cast<size_t>(us / 1000);
    if (bucket >= kFramePerfBucketCount) bucket = kFramePerfBucketCount - 1;
    if (buckets[bucket] != 0xffffffffu) ++buckets[bucket];
  }
  void reset() {
    count = 0;
    sumUs = 0;
    std::memset(buckets, 0, sizeof(buckets));
  }
  double avgMs() const {
    return count != 0 ? static_cast<double>(sumUs) / static_cast<double>(count) / 1000.0
                      : 0.0;
  }
  double p95Ms() const {
    if (count == 0) return 0.0;
    const long long target = (count * 95 + 99) / 100;
    long long running = 0;
    for (size_t i = 0; i < kFramePerfBucketCount; ++i) {
      running += buckets[i];
      if (running >= target) return static_cast<double>(i);
    }
    return static_cast<double>(kFramePerfBucketCount - 1);
  }
  long long count;
  long long sumUs;
  uint32_t buckets[kFramePerfBucketCount];
};

std::mutex g_framePerfMutex;
tianguan::RenderFramePerf g_framePerf;

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void LogLine(const char* line) {
  OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "%{public}s", line);
}

// Diagnostic summaries use DEBUG: they are periodic timings, not errors, and
// must not spam the default log level. One-shot and error lines stay WARN.
void LogDebug(const char* line) {
  OH_LOG_Print(LOG_APP, LOG_DEBUG, kDomain, kTag, "%{public}s", line);
}

long long NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// ---------------------------------------------------------------------------
// Process-wide EGLDisplay. eglInitialize/eglTerminate are NOT per-thread or
// per-consumer: one renderer terminating the default display would kill every
// other renderer's context (several sessions can present at once). The
// display is therefore initialized once for the whole process and never
// terminated; each renderer destroys only its own EGLSurface/EGLContext and
// native window. Renderers may come and go freely underneath it.
// ---------------------------------------------------------------------------
std::mutex g_displayMutex;
EGLDisplay g_display = EGL_NO_DISPLAY;
bool g_displayTried = false;

EGLDisplay AcquireSharedDisplay() {
  std::lock_guard<std::mutex> lock(g_displayMutex);
  if (g_display != EGL_NO_DISPLAY || g_displayTried) return g_display;
  g_displayTried = true;
  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  int major = 0;
  int minor = 0;
  if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE) {
    if (display != EGL_NO_DISPLAY) eglTerminate(display);
    display = eglGetPlatformDisplay(EGL_PLATFORM_OHOS_KHR, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY ||
        eglInitialize(display, &major, &minor) != EGL_TRUE) {
      if (display != EGL_NO_DISPLAY) eglTerminate(display);
      display = EGL_NO_DISPLAY;
    }
  }
  g_display = display;
  return g_display;
}

// --- Zero-copy surface source (Sunshine) -----------------------------------
//
// OH_NativeImage owns a producer OHNativeWindow that the video decoder renders
// into, and the consumer side that turns the newest buffer into an OES
// external texture. Its frame-available callback runs on an internal thread:
// it may only record the arrival time, flip a flag and wake the render thread
// (calling GL or NativeImage from that callback is forbidden). The signal is
// shared_ptr-owned by the Renderer and marked dead before the NativeImage is
// destroyed, so a late callback can never touch a freed renderer.
struct Renderer;  // forward declaration for the weak_ptr below

struct ExternalFrameSignal {
  ExternalFrameSignal() : alive(true), frameAvailable(false), lastFrameUs(0) {}
  std::mutex mutex;               // guards alive + owner
  bool alive;
  std::weak_ptr<Renderer> owner;  // set once the shared_ptr exists
  std::atomic<bool> frameAvailable;
  std::atomic<long long> lastFrameUs;  // NowUs() at the callback
};

// Defined after Renderer (it needs the complete type to notify its wake cv).
void OnExternalFrameAvailable(void* context);

/** One status snapshot, guarded by the renderer's stats mutex. */
struct RenderStats {
  std::string state = "starting";  // starting|running|suspended|stopped|failed
  std::string surfaceId;
  std::string error;
  long long frames = 0;        // presented frames (successful swaps)
  long long swapFailures = 0;
  long long uploads = 0;       // glTexSubImage2D calls
  long long fullUploads = 0;   // uploads that covered the whole texture
  long long skips = 0;         // polls that found nothing new
  double fps = 0.0;
  int width = 0;               // last seen EGL surface size
  int height = 0;
  int texWidth = 0;            // texture (== remote framebuffer) size
  int texHeight = 0;
  // Presented geometry, exactly the rectangle the last draw used. This is the
  // single source of truth ArkTS maps input from (never recomputed there).
  int contentLeft = 0;   // surface pixels, top-left origin
  int contentTop = 0;
  int contentW = 0;
  int contentH = 0;
  double contentScale = 1.0;   // contentW / texWidth
  double contentScaleY = 1.0;  // contentH / texHeight
  double zoom = 1.0;           // clamped zoom actually used
  double panX = 0.0;           // effective pan after clamping (surface pixels)
  double panY = 0.0;
  int frameVersion = -1;       // last consumed mirror version
  long long elapsedMs = 0;
  int lastEglError = 0;
  bool suspended = false;
};

struct Renderer {
  explicit Renderer(const std::string& id, bool startSuspended)
      : sessionId(id),
        suspended(startSuspended),
        frameSignal(new ExternalFrameSignal()) {}
  const std::string sessionId;
  std::thread thread;  // owned by whoever joins; moved out under `mutex`
  // Control block: park/wake plus thread ownership. The loop re-checks these
  // every iteration and parks while suspended. detach() sets stopRequested
  // and notifies; joining is handed to a reaper so UI callbacks never block
  // on a swap that may be stuck in the driver.
  std::mutex mutex;
  std::condition_variable wake;
  bool suspended;
  bool stopRequested = false;
  // View transform: written by the NAPI thread, read once per loop iteration
  // by the render thread. The mutex only ever guards three doubles (never held
  // during uploads, draws or swaps), and viewDirty asks the idle loop to
  // redraw the already-uploaded texture once after a change.
  std::mutex viewMutex;
  double viewZoom = 1.0;
  double viewPanX = 0.0;
  double viewPanY = 0.0;
  std::atomic<bool> viewDirty{false};
  std::mutex statsMutex;
  RenderStats stats;
  // --- Zero-copy surface source (Sunshine) --------------------------------
  // Control commands are executed on the render thread (it owns the GL
  // context); the caller blocks on taskDone. At most one command is in flight.
  std::mutex taskMutex;
  std::condition_variable taskDone;
  int taskKind = 0;  // 0 = none, 1 = create external source, 2 = release it
  bool taskFinished = false;
  bool taskOk = false;
  unsigned int taskTextureId = 0;
  NativeWindow* taskWindow = nullptr;
  std::atomic<bool> loopFinished{false};
  // Frame-arrival signal shared with OH_NativeImage's listener callback.
  std::shared_ptr<ExternalFrameSignal> frameSignal;
  // Video size reported by the decoder (surface mode has no stride/format).
  std::atomic<int> sourceWidth{0};
  std::atomic<int> sourceHeight{0};
  std::atomic<bool> externalSourceActive{false};
};

std::mutex g_registryMutex;
std::map<std::string, std::shared_ptr<Renderer>> g_renderers;

void OnExternalFrameAvailable(void* context) {
  ExternalFrameSignal* signal = static_cast<ExternalFrameSignal*>(context);
  if (signal == nullptr) return;
  signal->lastFrameUs.store(NowUs());
  signal->frameAvailable.store(true);
  std::shared_ptr<Renderer> owner;
  {
    std::lock_guard<std::mutex> lock(signal->mutex);
    if (!signal->alive) return;
    owner = signal->owner.lock();
  }
  // notify_all does not need the renderer mutex; the loop re-checks the flag.
  if (owner) owner->wake.notify_all();
}

std::string EglErrorText(const char* stage, EGLint error) {
  char buffer[256];
  std::snprintf(buffer, sizeof(buffer), "%s failed (egl=0x%04x)", stage,
                static_cast<unsigned int>(error));
  return std::string(buffer);
}

// Requests a stop and hands the join to a detached reaper thread, so callers
// on the UI thread (detachRenderSurface / attach replacing a previous
// renderer) never wait for a render thread that may be blocked inside
// eglSwapBuffers on a dying surface. The renderer object stays alive through
// the shared_ptr until the reaper is done with it.
void StopRendererAsync(const std::shared_ptr<Renderer>& renderer, const char* reason) {
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    renderer->stopRequested = true;
  }
  renderer->wake.notify_all();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    worker = std::move(renderer->thread);
  }
  if (worker.joinable()) {
    try {
      // C++11 has no init-capture, so the movable thread travels through a
      // shared_ptr; the reaper joins it and the last reference dies with it.
      std::shared_ptr<std::thread> holder(new std::thread(std::move(worker)));
      std::thread reaper([holder]() { holder->join(); });
      reaper.detach();
    } catch (const std::exception& error) {
      // Could not spawn a reaper (thread exhaustion): join here as the last
      // resort so the thread is never left unjoined-and-running forever.
      char line[256];
      std::snprintf(line, sizeof(line), "session=%s reaper spawn failed (%s), joining inline",
                    renderer->sessionId.c_str(), error.what());
      LogLine(line);
      worker.join();
    }
  }
  char line[192];
  std::snprintf(line, sizeof(line), "session=%s stop requested (%s)", renderer->sessionId.c_str(),
                reason);
  LogLine(line);
}

GLuint CompileShader(GLenum type, const char* source, std::string& error) {
  GLuint shader = glCreateShader(type);
  if (shader == 0) {
    error = "glCreateShader returned 0";
    return 0;
  }
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled != GL_TRUE) {
    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength > 1 ? logLength : 1), '\0');
    if (logLength > 1) glGetShaderInfoLog(shader, logLength, nullptr, &log[0]);
    error = std::string("shader compile failed: ") + log.c_str();
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

// The mirror is BGRA; uploading those bytes as GL_RGBA swaps red and blue, so
// the fragment shader swizzles them back (.bgr). Forcing alpha to 1 keeps a
// stale alpha byte from dimming the picture.
GLuint BuildProgram(std::string& error) {
  static const char* kVertexSource =
      "attribute vec2 aPosition;\n"
      "attribute vec2 aTexCoord;\n"
      "varying vec2 vTexCoord;\n"
      "void main() {\n"
      "  vTexCoord = aTexCoord;\n"
      "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
      "}\n";
  static const char* kFragmentSource =
      "precision mediump float;\n"
      "varying vec2 vTexCoord;\n"
      "uniform sampler2D uTexture;\n"
      "void main() {\n"
      "  vec4 texel = texture2D(uTexture, vTexCoord);\n"
      "  gl_FragColor = vec4(texel.bgr, 1.0);\n"
      "}\n";
  GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexSource, error);
  if (vertex == 0) return 0;
  GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentSource, error);
  if (fragment == 0) {
    glDeleteShader(vertex);
    return 0;
  }
  GLuint program = glCreateProgram();
  if (program == 0) {
    error = "glCreateProgram returned 0";
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "aPosition");
  glBindAttribLocation(program, 1, "aTexCoord");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength > 1 ? logLength : 1), '\0');
    if (logLength > 1) glGetProgramInfoLog(program, logLength, nullptr, &log[0]);
    error = std::string("program link failed: ") + log.c_str();
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

// 零拷贝路径的着色器：解码器通过 OH_NativeImage 把帧写进 OES 外部纹理（内容已是
// RGB，vendor 完成色彩转换），所以不做 BGR swizzle。采样坐标必须乘
// OH_NativeImage_GetTransformMatrixV2 的 4x4 矩阵：native_image.h 说明它是
// "transform matrix of the texture image by producer transform type"，包含裁剪
// 区域与翻转/旋转（生产者缓冲自上而下，矩阵里通常就是 scaleY=-1 + 平移 1）。矩阵
// 按列主序原样上传（glUniformMatrix4fv 的 transpose=GL_FALSE），乘在
// vec4(vTexCoord, 0, 1) 左侧；alpha 强制 1。
GLuint BuildExternalProgram(std::string& error) {
  static const char* kVertexSource =
      "attribute vec2 aPosition;\n"
      "attribute vec2 aTexCoord;\n"
      "varying vec2 vTexCoord;\n"
      "void main() {\n"
      "  vTexCoord = aTexCoord;\n"
      "  gl_Position = vec4(aPosition, 0.0, 1.0);\n"
      "}\n";
  static const char* kFragmentSource =
      "#extension GL_OES_EGL_image_external : require\n"
      "precision mediump float;\n"
      "varying vec2 vTexCoord;\n"
      "uniform samplerExternalOES uTexture;\n"
      "uniform mat4 uTexMatrix;\n"
      "void main() {\n"
      "  vec2 uv = (uTexMatrix * vec4(vTexCoord, 0.0, 1.0)).xy;\n"
      "  vec4 texel = texture2D(uTexture, uv);\n"
      "  gl_FragColor = vec4(texel.rgb, 1.0);\n"
      "}\n";
  GLuint vertex = CompileShader(GL_VERTEX_SHADER, kVertexSource, error);
  if (vertex == 0) return 0;
  GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, kFragmentSource, error);
  if (fragment == 0) {
    glDeleteShader(vertex);
    return 0;
  }
  GLuint program = glCreateProgram();
  if (program == 0) {
    error = "glCreateProgram returned 0";
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    return 0;
  }
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "aPosition");
  glBindAttribLocation(program, 1, "aTexCoord");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint logLength = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<size_t>(logLength > 1 ? logLength : 1), '\0');
    if (logLength > 1) glGetProgramInfoLog(program, logLength, nullptr, &log[0]);
    error = std::string("external program link failed: ") + log.c_str();
    glDeleteProgram(program);
    return 0;
  }
  return program;
}

void RenderLoop(const std::shared_ptr<Renderer> renderer, uint64_t surfaceId,
                const std::string surfaceIdText) {
  OHNativeWindow* window = nullptr;
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLSurface eglSurface = EGL_NO_SURFACE;
  EGLContext context = EGL_NO_CONTEXT;
  EGLConfig config = nullptr;
  GLuint texture = 0;
  GLuint program = 0;
  GLuint vertexBuffer = 0;
  // --- Zero-copy surface source (Sunshine) --------------------------------
  // Render-thread-local: the create/release tasks run on this thread with the
  // GL context current, so nothing here is touched from another thread. The
  // BGRA path below is untouched and still used by VNC/RDP and by the Sunshine
  // Buffer-mode fallback.
  bool externalActive = false;
  OH_NativeImage* nativeImage = nullptr;
  uint32_t externalTexture = 0;
  // 外部纹理专用的顶点缓冲：UV 方向与 BGRA 路径相反（v=0 在画面底部），见下面
  // 创建处的注释。绝不能复用 BGRA 的 vertexBuffer，否则 SDK 变换矩阵里的 Y 翻转
  // 会被预先抵消一次，画面上下颠倒。
  GLuint externalVertexBuffer = 0;
  GLuint externalProgram = 0;
  GLint externalTextureLoc = -1;
  GLint externalMatrixLoc = -1;
  float externalTransform[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                                 0.0f, 1.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 1.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f, 1.0f};
  // 变换矩阵诊断：第一帧一定打一次，之后只有矩阵变化且距上次 >= 1s 才再打。
  bool loggedTransform = false;
  float lastLoggedTransform[16] = {0.0f};
  long long lastTransformLogMs = 0;
  GLuint lumaFbo = 0;
  GLuint lumaRenderbuffer = 0;
  bool lumaProbeBroken = false;
  long long lastLumaProbeMs = 0;
  long long lastSurfaceFailLogMs = 0;
  long long lastExternalLogMs = 0;
  long long externalPerfWindowStartUs = NowUs();
  long long externalPerfSwaps = 0;
  const std::shared_ptr<ExternalFrameSignal> frameSignal = renderer->frameSignal;
  FramePerfHistogram externalLag;          // 帧可用 -> UpdateSurfaceImage 完成
  FramePerfHistogram externalPickupSwap;   // UpdateSurfaceImage 完成 -> swap 返回
  FramePerfHistogram externalFrameToSwap;  // 帧可用 -> swap 返回
  std::string failure;
  EGLint lastEglError = EGL_SUCCESS;
  int clientVersion = 3;
  int surfaceWidth = 0;
  int surfaceHeight = 0;
  int texWidth = 0;
  int texHeight = 0;
  // Snapshot of the renderer's view transform, refreshed at the top of every
  // loop iteration (a 3-double copy under the view mutex, ~4 ms apart at most
  // while idle) so a draw never holds that lock.
  double viewZoom = 1.0;
  double viewPanX = 0.0;
  double viewPanY = 0.0;
  int afterVersion = -1;
  bool forceFull = true;
  // geometryDirty: the layout (surface size or texture size) changed, so the
  // next iteration redraws even without new pixels. presentPending: pixels
  // were uploaded but the last swap failed — retry the present, never the
  // upload (the texture already holds that version).
  bool geometryDirty = true;
  bool presentPending = false;
  bool presented = false;  // at least one frame reached the surface
  int surfaceQueryFailures = 0;
  std::vector<uint8_t> staging;
  long long frames = 0;
  long long swapFailures = 0;
  long long uploads = 0;
  long long fullUploads = 0;
  long long skips = 0;
  double fps = 0.0;
  const long long startedAt = NowMs();
  auto fpsWindowStart = std::chrono::steady_clock::now();
  long long fpsWindowFrames = 0;
  long long lastLogMs = startedAt;
  long long lastStatsMs = 0;
  // --- Per-second diagnostic aggregation (debug "TianguanRender" line) -----
  // All values are microseconds and reset every window. lockWait covers every
  // takeFrameStruct call (including NONE polls), copy/upload/swap cover the
  // delivered path, maxFlushGap is the largest interval between two successful
  // eglSwapBuffers in the window. swap times eglSwapBuffers itself, which is
  // where the buffer flush to the compositor happens.
  long long diagWindowStartUs = NowUs();
  long long diagLockWaitSumUs = 0;
  long long diagLockWaitMaxUs = 0;
  long long diagLockWaitN = 0;
  long long diagCopySumUs = 0;
  long long diagCopyMaxUs = 0;
  long long diagCopyN = 0;
  long long diagUploadSumUs = 0;
  long long diagUploadMaxUs = 0;
  long long diagUploadN = 0;
  long long diagSwapSumUs = 0;
  long long diagSwapMaxUs = 0;
  long long diagSwapN = 0;
  long long diagWindowFrames = 0;
  long long diagLastPresentUs = 0;
  long long diagMaxFlushGapUs = 0;
  // --- Frame latency aggregation (5 s window, see kFramePerf*) -------------
  // lag and pickupToSwap are only sampled on the delivered path; outFps counts
  // successful swaps in the window (idle retries are included, they are rare).
  long long framePerfWindowStartUs = NowUs();
  FramePerfHistogram framePerfLag;
  FramePerfHistogram framePerfPickupToSwap;
  long long framePerfSwaps = 0;

  auto setFailure = [&](const std::string& error) {
    failure = error;
    char line[320];
    std::snprintf(line, sizeof(line), "session=%s FAIL %s", renderer->sessionId.c_str(),
                  error.c_str());
    LogLine(line);
  };
  auto querySurfaceSize = [&]() -> bool {
    EGLint w = 0;
    EGLint h = 0;
    if (eglQuerySurface(display, eglSurface, EGL_WIDTH, &w) != EGL_TRUE ||
        eglQuerySurface(display, eglSurface, EGL_HEIGHT, &h) != EGL_TRUE ||
        w <= 0 || h <= 0) {
      return false;
    }
    if (w != surfaceWidth || h != surfaceHeight) {
      surfaceWidth = w;
      surfaceHeight = h;
      geometryDirty = true;  // letterbox must be recomputed and redrawn
      char line[192];
      std::snprintf(line, sizeof(line), "session=%s surface %dx%d",
                    renderer->sessionId.c_str(), w, h);
      LogLine(line);
    }
    return true;
  };
  // Present layout is derived from the TEXTURE's own size — the aspect of the
  // pixels actually stored — never from a not-yet-uploaded mirror geometry.
  auto drawAndSwap = [&]() -> bool {
    if (texture == 0 || texWidth <= 0 || texHeight <= 0) return false;
    // Contain * zoom, panned and clamped, computed once here and reused for
    // the stats report: the picture ArkTS maps input against is exactly the
    // rectangle drawn below. zoom == 1 / pan == 0 reproduces the old contain
    // layout pixel for pixel (including its integer centring).
    const tianguan::ViewGeometry geometry = tianguan::ComputeViewGeometry(
        texWidth, texHeight, surfaceWidth, surfaceHeight, viewZoom, viewPanX, viewPanY);
    const int viewportLeft = tianguan::RoundViewportCoord(geometry.left);
    const int viewportTop = tianguan::RoundViewportCoord(geometry.top);
    glViewport(viewportLeft, surfaceHeight - viewportTop - geometry.contentH, geometry.contentW,
               geometry.contentH);
    {
      // Publish before presenting so a stats read right after a redraw sees
      // the rectangle that was just drawn. Short lock, no GL calls inside.
      std::lock_guard<std::mutex> lock(renderer->statsMutex);
      renderer->stats.contentLeft = viewportLeft;
      renderer->stats.contentTop = viewportTop;
      renderer->stats.contentW = geometry.contentW;
      renderer->stats.contentH = geometry.contentH;
      renderer->stats.contentScale = geometry.contentScale;
      renderer->stats.contentScaleY = geometry.contentScaleY;
      renderer->stats.zoom = geometry.zoom;
      renderer->stats.panX = geometry.panX;
      renderer->stats.panY = geometry.panY;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(glGetUniformLocation(program, "uTexture"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const long long swapStartUs = NowUs();
    const EGLBoolean swapped = eglSwapBuffers(display, eglSurface);
    const long long swapUs = NowUs() - swapStartUs;
    diagSwapSumUs += swapUs;
    diagSwapN += 1;
    if (swapUs > diagSwapMaxUs) diagSwapMaxUs = swapUs;
    if (swapped != EGL_TRUE) {
      lastEglError = eglGetError();
      ++swapFailures;
      char line[192];
      std::snprintf(line, sizeof(line),
                    "session=%s eglSwapBuffers failed (count=%lld egl=0x%04x)",
                    renderer->sessionId.c_str(), swapFailures,
                    static_cast<unsigned int>(lastEglError));
      LogLine(line);
      return false;
    }
    ++frames;
    ++diagWindowFrames;
    ++framePerfSwaps;
    const long long presentedAtUs = NowUs();
    if (diagLastPresentUs != 0 && presentedAtUs - diagLastPresentUs > diagMaxFlushGapUs) {
      diagMaxFlushGapUs = presentedAtUs - diagLastPresentUs;
    }
    diagLastPresentUs = presentedAtUs;
    presented = true;
    presentPending = false;
    return true;
  };
  // Refreshes the fps window and publishes a stats snapshot. Called at a low
  // cadence from EVERY path (frames, skips, resume), so an idle renderer
  // still reports a current state and an fps that decays to 0.
  auto publishStatsIfDue = [&](const char* state) {
    const long long nowMs = NowMs();
    if (lastStatsMs != 0 && nowMs - lastStatsMs < kStatsPublishMs) return;
    lastStatsMs = nowMs;
    const auto now = std::chrono::steady_clock::now();
    const double windowMs =
        std::chrono::duration_cast<std::chrono::microseconds>(now - fpsWindowStart).count() /
        1000.0;
    if (windowMs >= 1000.0) {
      fps = (frames - fpsWindowFrames) * 1000.0 / windowMs;
      fpsWindowFrames = frames;
      fpsWindowStart = now;
    }
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    renderer->stats.state = state;
    renderer->stats.frames = frames;
    renderer->stats.swapFailures = swapFailures;
    renderer->stats.uploads = uploads;
    renderer->stats.fullUploads = fullUploads;
    renderer->stats.skips = skips;
    renderer->stats.fps = fps;
    renderer->stats.width = surfaceWidth;
    renderer->stats.height = surfaceHeight;
    renderer->stats.texWidth = texWidth;
    renderer->stats.texHeight = texHeight;
    renderer->stats.frameVersion = afterVersion;
    renderer->stats.elapsedMs = nowMs - startedAt;
    renderer->stats.lastEglError = static_cast<int>(lastEglError);
  };
  // Clears the per-second diagnostic window (also used after a suspension, so
  // the parked interval never shows up as a flush gap or as low fps).
  auto resetDiagWindow = [&]() {
    diagWindowStartUs = NowUs();
    diagLockWaitSumUs = 0;
    diagLockWaitMaxUs = 0;
    diagLockWaitN = 0;
    diagCopySumUs = 0;
    diagCopyMaxUs = 0;
    diagCopyN = 0;
    diagUploadSumUs = 0;
    diagUploadMaxUs = 0;
    diagUploadN = 0;
    diagSwapSumUs = 0;
    diagSwapMaxUs = 0;
    diagSwapN = 0;
    diagWindowFrames = 0;
    diagLastPresentUs = 0;
    diagMaxFlushGapUs = 0;
  };
  // One debug line per second (hidden at the default log level). fps is
  // computed on this window's presents; frames/uploads are cumulative.
  // Averages divide by the calls seen in the window (max is the worst single
  // call).
  auto logDiagIfDue = [&]() {
    const long long nowUs = NowUs();
    const long long elapsedUs = nowUs - diagWindowStartUs;
    if (elapsedUs < 1000000) return;
    const double windowSec = static_cast<double>(elapsedUs) / 1000000.0;
    const double windowFps = static_cast<double>(diagWindowFrames) / windowSec;
    const double lockWaitAvg =
        diagLockWaitN != 0 ? static_cast<double>(diagLockWaitSumUs) / diagLockWaitN / 1000.0 : 0.0;
    const double copyAvg =
        diagCopyN != 0 ? static_cast<double>(diagCopySumUs) / diagCopyN / 1000.0 : 0.0;
    const double uploadAvg =
        diagUploadN != 0 ? static_cast<double>(diagUploadSumUs) / diagUploadN / 1000.0 : 0.0;
    const double swapAvg =
        diagSwapN != 0 ? static_cast<double>(diagSwapSumUs) / diagSwapN / 1000.0 : 0.0;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "TianguanRender fps=%.1f frames=%lld uploads=%lld "
                  "lockWaitAvg/Max=%.2f/%.2fms copy=%.2f/%.2fms upload=%.2f/%.2fms "
                  "swap=%.2f/%.2fms maxFlushGap=%.1fms",
                  windowFps, frames, uploads,
                  lockWaitAvg, static_cast<double>(diagLockWaitMaxUs) / 1000.0,
                  copyAvg, static_cast<double>(diagCopyMaxUs) / 1000.0,
                  uploadAvg, static_cast<double>(diagUploadMaxUs) / 1000.0,
                  swapAvg, static_cast<double>(diagSwapMaxUs) / 1000.0,
                  static_cast<double>(diagMaxFlushGapUs) / 1000.0);
    LogDebug(line);
    resetDiagWindow();
  };
  // 零拷贝路径：把当前 OES 外部纹理按 contain 几何画满并提交，统计口径与
  // drawAndSwap 相同（frames/diag/perf 窗口）。与原 BGRA 路径完全独立：原函数
  // 一行未改，VNC/RDP 永远走原路径。
  auto drawExternalAndSwap = [&]() -> bool {
    if (externalTexture == 0 || externalProgram == 0 || texWidth <= 0 || texHeight <= 0)
      return false;
    const tianguan::ViewGeometry geometry = tianguan::ComputeViewGeometry(
        texWidth, texHeight, surfaceWidth, surfaceHeight, viewZoom, viewPanX, viewPanY);
    const int viewportLeft = tianguan::RoundViewportCoord(geometry.left);
    const int viewportTop = tianguan::RoundViewportCoord(geometry.top);
    glViewport(viewportLeft, surfaceHeight - viewportTop - geometry.contentH,
               geometry.contentW, geometry.contentH);
    {
      std::lock_guard<std::mutex> lock(renderer->statsMutex);
      renderer->stats.contentLeft = viewportLeft;
      renderer->stats.contentTop = viewportTop;
      renderer->stats.contentW = geometry.contentW;
      renderer->stats.contentH = geometry.contentH;
      renderer->stats.contentScale = geometry.contentScale;
      renderer->stats.contentScaleY = geometry.contentScaleY;
      renderer->stats.zoom = geometry.zoom;
      renderer->stats.panX = geometry.panX;
      renderer->stats.panY = geometry.panY;
    }
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(externalProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
    glUniform1i(externalTextureLoc, 0);
    // transpose=GL_FALSE：按列主序原样上传 GetTransformMatrixV2 的矩阵（含裁剪与
    // Y 翻转），着色器里乘在 vec4(vTexCoord,0,1) 左侧。
    glUniformMatrix4fv(externalMatrixLoc, 1, GL_FALSE, externalTransform);
    glBindBuffer(GL_ARRAY_BUFFER, externalVertexBuffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const long long swapStartUs = NowUs();
    const EGLBoolean swapped = eglSwapBuffers(display, eglSurface);
    const long long swapUs = NowUs() - swapStartUs;
    diagSwapSumUs += swapUs;
    diagSwapN += 1;
    if (swapUs > diagSwapMaxUs) diagSwapMaxUs = swapUs;
    if (swapped != EGL_TRUE) {
      lastEglError = eglGetError();
      ++swapFailures;
      char line[192];
      std::snprintf(line, sizeof(line),
                    "session=%s surface eglSwapBuffers failed (count=%lld egl=0x%04x)",
                    renderer->sessionId.c_str(), swapFailures,
                    static_cast<unsigned int>(lastEglError));
      LogLine(line);
      return false;
    }
    ++frames;
    ++diagWindowFrames;
    ++framePerfSwaps;
    const long long presentedAtUs = NowUs();
    if (diagLastPresentUs != 0 && presentedAtUs - diagLastPresentUs > diagMaxFlushGapUs) {
      diagMaxFlushGapUs = presentedAtUs - diagLastPresentUs;
    }
    diagLastPresentUs = presentedAtUs;
    presented = true;
    presentPending = false;
    return true;
  };
  // 黑屏检测（Surface 模式没有可采样的 BGRA 镜像）：把外部纹理画进 32x32 的 FBO
  // 再 glReadPixels，按平均亮度判定，结果交给 sunshineNoteVideoLuma，与 Buffer 模式
  // 的采样阈值一致（平均亮度 < 4 为黑）。每 500ms 最多一次，且在正常 swap 之后做，
  // 不进入正常呈现路径。FBO 建不起来就永久停用这个探针（只记一次）。
  auto probeExternalLuma = [&]() {
    if (lumaProbeBroken || externalTexture == 0 || externalProgram == 0) return;
    if (lumaFbo == 0) {
      glGenFramebuffers(1, &lumaFbo);
      glGenRenderbuffers(1, &lumaRenderbuffer);
      glBindRenderbuffer(GL_RENDERBUFFER, lumaRenderbuffer);
      // RGBA4 是 ES2 核心格式，32x32 足够判黑，且读回仍是 8 位。
      glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 32, 32);
      glBindFramebuffer(GL_FRAMEBUFFER, lumaFbo);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                lumaRenderbuffer);
      const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      if (status != GL_FRAMEBUFFER_COMPLETE) {
        lumaProbeBroken = true;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "session=%s black probe unavailable (fbo=0x%04x)",
                      renderer->sessionId.c_str(), static_cast<unsigned int>(status));
        LogLine(line);
        return;
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, lumaFbo);
    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(externalProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
    glUniform1i(externalTextureLoc, 0);
    glUniformMatrix4fv(externalMatrixLoc, 1, GL_FALSE, externalTransform);
    glBindBuffer(GL_ARRAY_BUFFER, externalVertexBuffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    unsigned char pixels[32 * 32 * 4];
    glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    unsigned long long sum = 0;
    for (size_t i = 0; i < sizeof(pixels); i += 4) {
      sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
    }
    const bool black = sum < 4ull * 3 * (sizeof(pixels) / 4);
    tianguan::sunshineNoteVideoLuma(static_cast<unsigned long long>(NowMs()), black);
  };
  // Surface 模式的 5 秒窗口：发布 RenderFramePerf（后端 5 秒汇总行与 statusJson.perf
  // 读它），并打一条带 src=surface 的统计行。
  auto logExternalStatsIfDue = [&]() {
    const long long nowMs = NowMs();
    if (lastExternalLogMs != 0 && nowMs - lastExternalLogMs < 5000) return;
    lastExternalLogMs = nowMs;
    const long long windowUs = NowUs() - externalPerfWindowStartUs;
    tianguan::RenderFramePerf perf;
    perf.windowMs =
        windowUs > 0 ? static_cast<unsigned long long>(windowUs / 1000) : 0;
    perf.outFps = windowUs > 0
                      ? static_cast<double>(externalPerfSwaps) * 1000000.0 /
                            static_cast<double>(windowUs)
                      : 0.0;
    perf.lagAvgMs = externalLag.avgMs();
    perf.lagP95Ms = externalLag.p95Ms();
    perf.pickupToSwapAvgMs = externalPickupSwap.avgMs();
    perf.pickupToSwapP95Ms = externalPickupSwap.p95Ms();
    perf.frameAvailableToSwapAvgMs = externalFrameToSwap.avgMs();
    perf.frameAvailableToSwapP95Ms = externalFrameToSwap.p95Ms();
    {
      std::lock_guard<std::mutex> lock(g_framePerfMutex);
      g_framePerf = perf;
    }
    externalLag.reset();
    externalPickupSwap.reset();
    externalFrameToSwap.reset();
    externalPerfSwaps = 0;
    externalPerfWindowStartUs = NowUs();
    char line[640];
    std::snprintf(line, sizeof(line),
                  "session=%s src=surface frames=%lld fps=%.1f size=%dx%d tex=%dx%d "
                  "lag=%.1f/%.1fms pickupSwap=%.1f/%.1fms frameToSwap=%.1f/%.1fms",
                  renderer->sessionId.c_str(), frames, perf.outFps, surfaceWidth,
                  surfaceHeight, texWidth, texHeight, perf.lagAvgMs, perf.lagP95Ms,
                  perf.pickupToSwapAvgMs, perf.pickupToSwapP95Ms,
                  perf.frameAvailableToSwapAvgMs, perf.frameAvailableToSwapP95Ms);
    LogDebug(line);
  };
  auto hasPendingTask = [&]() -> bool {
    std::lock_guard<std::mutex> lock(renderer->taskMutex);
    return renderer->taskKind != 0;
  };
  // 在渲染线程执行宿主投递的控制命令（建/销毁外部纹理源）。GL 上下文在这里是当前
  // 的；完成后唤醒阻塞在 taskDone 上的调用方。
  auto processPendingTask = [&]() {
    int kind = 0;
    {
      std::lock_guard<std::mutex> lock(renderer->taskMutex);
      kind = renderer->taskKind;
    }
    if (kind == 0) return;
    bool ok = false;
    if (kind == 1) {
      std::string error;
      if (externalProgram == 0) externalProgram = BuildExternalProgram(error);
      if (externalProgram == 0) {
        char line[320];
        std::snprintf(line, sizeof(line), "session=%s external shader failed: %s",
                      renderer->sessionId.c_str(), error.c_str());
        LogLine(line);
      } else {
        externalTextureLoc = glGetUniformLocation(externalProgram, "uTexture");
        externalMatrixLoc = glGetUniformLocation(externalProgram, "uTexMatrix");
        // 外部纹理的顶点缓冲用 GL 原生 UV 方向（左下角 v=0、左上角 v=1），与
        // OpenHarmony 官方 NativeImage 示例（applications_app_samples 的
        // NdkNativeImage ImageRender::SetupVertexAttributes）一致。原因：
        // OH_NativeImage_GetTransformMatrixV2 返回的 4x4 矩阵已经包含把生产者
        // 自上而下缓冲翻正的 Y 翻转（以及裁剪/旋转），见 native_image.h 里
        // "transform matrix of the texture image by producer transform type"。
        // BGRA 路径的 vertexBuffer 是 v=0 在上（补偿 glTexSubImage2D 的自上而下
        // 上传），若拿来画外部纹理就会把这次翻转预先抵消，画面上下颠倒。
        static const GLfloat kExternalVertices[] = {
            -1.0f, -1.0f, 0.0f, 0.0f,  // 左下 -> uv(0,0)
            1.0f,  -1.0f, 1.0f, 0.0f,  // 右下 -> uv(1,0)
            -1.0f, 1.0f,  0.0f, 1.0f,  // 左上 -> uv(0,1)
            1.0f,  1.0f,  1.0f, 1.0f   // 右上 -> uv(1,1)
        };
        if (externalVertexBuffer == 0) {
          glGenBuffers(1, &externalVertexBuffer);
          glBindBuffer(GL_ARRAY_BUFFER, externalVertexBuffer);
          glBufferData(GL_ARRAY_BUFFER, sizeof(kExternalVertices), kExternalVertices,
                       GL_STATIC_DRAW);
        }
        glGenTextures(1, &externalTexture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, externalTexture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        nativeImage = OH_NativeImage_Create(externalTexture, GL_TEXTURE_EXTERNAL_OES);
        if (nativeImage == nullptr) {
          char line[192];
          std::snprintf(line, sizeof(line), "session=%s OH_NativeImage_Create failed",
                        renderer->sessionId.c_str());
          LogLine(line);
        } else {
          {
            std::lock_guard<std::mutex> lock(frameSignal->mutex);
            frameSignal->alive = true;
            frameSignal->owner = renderer;
            frameSignal->frameAvailable.store(false);
          }
          OH_OnFrameAvailableListener listener;
          listener.context = frameSignal.get();
          listener.onFrameAvailable = OnExternalFrameAvailable;
          const int32_t listenerResult =
              OH_NativeImage_SetOnFrameAvailableListener(nativeImage, listener);
          NativeWindow* sourceWindow =
              listenerResult == 0 ? OH_NativeImage_AcquireNativeWindow(nativeImage) : nullptr;
          if (listenerResult != 0 || sourceWindow == nullptr) {
            char line[192];
            std::snprintf(line, sizeof(line),
                          "session=%s surface source window failed (listener=%d)",
                          renderer->sessionId.c_str(), listenerResult);
            LogLine(line);
          } else {
            externalActive = true;
            renderer->externalSourceActive.store(true);
            loggedTransform = false;  // 新源头：第一帧重新打一次矩阵
            {
              std::lock_guard<std::mutex> lock(renderer->taskMutex);
              renderer->taskTextureId = externalTexture;
              renderer->taskWindow = sourceWindow;
            }
            ok = true;
            char line[224];
            std::snprintf(line, sizeof(line),
                          "session=%s surface source created (texture=%u)",
                          renderer->sessionId.c_str(), externalTexture);
            LogLine(line);
          }
        }
        if (!ok) {
          // 建到一半失败：把已经建好的东西收干净，避免泄漏。
          {
            std::lock_guard<std::mutex> lock(frameSignal->mutex);
            frameSignal->alive = false;
            frameSignal->owner.reset();
          }
          if (nativeImage != nullptr) {
            OH_NativeImage_UnsetOnFrameAvailableListener(nativeImage);
            OH_NativeImage_Destroy(&nativeImage);
          }
          if (externalTexture != 0) {
            glDeleteTextures(1, &externalTexture);
            externalTexture = 0;
          }
          if (externalVertexBuffer != 0) {
            glDeleteBuffers(1, &externalVertexBuffer);
            externalVertexBuffer = 0;
          }
          if (externalProgram != 0) {
            glDeleteProgram(externalProgram);
            externalProgram = 0;
          }
        }
      }
    } else if (kind == 2) {
      // 先让监听回调失效，再销毁 NativeImage/纹理；调用方保证解码器已经停止。
      {
        std::lock_guard<std::mutex> lock(frameSignal->mutex);
        frameSignal->alive = false;
        frameSignal->owner.reset();
        frameSignal->frameAvailable.store(false);
      }
      if (nativeImage != nullptr) {
        OH_NativeImage_UnsetOnFrameAvailableListener(nativeImage);
        OH_NativeImage_Destroy(&nativeImage);
      }
      if (externalTexture != 0) {
        glDeleteTextures(1, &externalTexture);
        externalTexture = 0;
      }
      if (externalVertexBuffer != 0) {
        glDeleteBuffers(1, &externalVertexBuffer);
        externalVertexBuffer = 0;
      }
      if (externalProgram != 0) {
        glDeleteProgram(externalProgram);
        externalProgram = 0;
      }
      externalActive = false;
      renderer->externalSourceActive.store(false);
      texWidth = 0;
      texHeight = 0;
      ok = true;
      char line[192];
      std::snprintf(line, sizeof(line), "session=%s surface source released",
                    renderer->sessionId.c_str());
      LogLine(line);
    }
    {
      std::lock_guard<std::mutex> lock(renderer->taskMutex);
      renderer->taskOk = ok;
      renderer->taskKind = 0;
      renderer->taskFinished = true;
    }
    renderer->taskDone.notify_all();
  };

  do {
    int32_t result = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (result != 0 || window == nullptr) {
      char buffer[256];
      std::snprintf(buffer, sizeof(buffer),
                    "OH_NativeWindow_CreateNativeWindowFromSurfaceId failed (rc=%d)", result);
      setFailure(buffer);
      break;
    }
    display = AcquireSharedDisplay();
    if (display == EGL_NO_DISPLAY) {
      setFailure("shared EGL display unavailable");
      break;
    }
    EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE};
    EGLint configCount = 0;
    EGLConfig chosen = nullptr;
    if (eglChooseConfig(display, configAttributes, &chosen, 1, &configCount) != EGL_TRUE ||
        configCount < 1) {
      configAttributes[3] = EGL_OPENGL_ES2_BIT;
      clientVersion = 2;
      configCount = 0;
      if (eglChooseConfig(display, configAttributes, &chosen, 1, &configCount) != EGL_TRUE ||
          configCount < 1) {
        setFailure(EglErrorText("eglChooseConfig", eglGetError()));
        break;
      }
    }
    config = chosen;
    eglSurface = eglCreateWindowSurface(
        display, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
    if (eglSurface == EGL_NO_SURFACE) {
      setFailure(EglErrorText("eglCreateWindowSurface", eglGetError()));
      break;
    }
    EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, clientVersion, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
      setFailure(EglErrorText("eglCreateContext", eglGetError()));
      break;
    }
    if (eglMakeCurrent(display, eglSurface, eglSurface, context) != EGL_TRUE) {
      setFailure(EglErrorText("eglMakeCurrent", eglGetError()));
      break;
    }
    // Present at the surface's vsync: swaps throttle themselves and the idle
    // poll below never swaps at all, so the CPU cost follows the content.
    eglSwapInterval(display, 1);
    querySurfaceSize();
    {
      char line[256];
      std::snprintf(line, sizeof(line),
                    "session=%s surface=%s ES=%d size=%dx%d renderer=%s",
                    renderer->sessionId.c_str(), surfaceIdText.c_str(), clientVersion,
                    surfaceWidth, surfaceHeight,
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
      LogLine(line);
    }
    std::string programError;
    program = BuildProgram(programError);
    if (program == 0) {
      setFailure(programError);
      break;
    }
    static const GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 0.0f,
        1.0f,  1.0f,  1.0f, 0.0f};
    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
    publishStatsIfDue("running");

    while (true) {
      // 宿主投递的零拷贝控制命令（建/销毁外部纹理源）先执行：GL 上下文在这条
      // 线程上是当前的。没有命令时是一次空检查。
      processPendingTask();
      // Consume a view transform change first: it must redraw even when no new
      // frame arrives (pan/zoom over a static desktop). Then copy the current
      // transform for this iteration; the lock is released before any query,
      // upload, draw or swap.
      if (renderer->viewDirty.exchange(false)) geometryDirty = true;
      {
        std::lock_guard<std::mutex> lock(renderer->viewMutex);
        viewZoom = renderer->viewZoom;
        viewPanX = renderer->viewPanX;
        viewPanY = renderer->viewPanY;
      }
      logDiagIfDue();
      {
        std::unique_lock<std::mutex> lock(renderer->mutex);
        if (renderer->stopRequested) break;
        if (renderer->suspended) {
          {
            std::lock_guard<std::mutex> statsLock(renderer->statsMutex);
            renderer->stats.state = "suspended";
            renderer->stats.suspended = true;
          }
          renderer->wake.wait(lock, [&renderer]() {
            return renderer->stopRequested || !renderer->suspended;
          });
          if (renderer->stopRequested) break;
          // The surface may have changed size while nobody swapped.
          geometryDirty = true;
          // The parked interval is not a content/flush problem: start a fresh
          // diagnostic window instead of reporting it as a huge flush gap.
          resetDiagWindow();
        }
      }
      const bool haveSurface = querySurfaceSize();
      if (!haveSurface) {
        // No usable surface right now (being destroyed, or not yet sized).
        // Never grab frames in that state: pixels taken from the delivery
        // layer could not be presented and would be lost. Surface events or
        // detach stop this loop; a persistent failure gives up on its own and
        // ArkTS re-attaches.
        ++skips;
        if (++surfaceQueryFailures >= kMaxSurfaceQueryFailures) {
          setFailure("surface unusable (eglQuerySurface failed repeatedly)");
          break;
        }
        publishStatsIfDue("running");
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
        continue;
      }
      surfaceQueryFailures = 0;
      // 零拷贝（Surface 模式）会话：画面来自 OH_NativeImage 的外部纹理，完全不
      // 读 BGRA 镜像（takeFrameStruct 一次都不调）。本分支与下面的 BGRA 路径互斥，
      // 原路径（VNC/RDP/Sunshine Buffer 回退）逐行未改。
      if (externalActive) {
        const int requestedWidth = renderer->sourceWidth.load();
        const int requestedHeight = renderer->sourceHeight.load();
        if (requestedWidth > 0 && requestedHeight > 0 &&
            (requestedWidth != texWidth || requestedHeight != texHeight)) {
          texWidth = requestedWidth;
          texHeight = requestedHeight;
          geometryDirty = true;
          char line[192];
          std::snprintf(line, sizeof(line), "session=%s surface source %dx%d",
                        renderer->sessionId.c_str(), texWidth, texHeight);
          LogLine(line);
        }
        if (frameSignal->frameAvailable.exchange(false) && nativeImage != nullptr) {
          // 帧可用回调只置标志并唤醒；UpdateSurfaceImage / 查询矩阵 / 绘制都必须
          // 在这条线程（GL 上下文当前）做。
          const long long arrivedUs = frameSignal->lastFrameUs.load();
          const int32_t updateResult = OH_NativeImage_UpdateSurfaceImage(nativeImage);
          if (updateResult == 0) {
            const long long pickedUpUs = NowUs();
            if (arrivedUs > 0 && pickedUpUs >= arrivedUs)
              externalLag.add(pickedUpUs - arrivedUs);
            const int32_t matrixResult =
                OH_NativeImage_GetTransformMatrixV2(nativeImage, externalTransform);
            bool matrixChanged = false;
            if (matrixResult != 0) {
              // 拿不到矩阵时退化为单位矩阵（不自己猜翻转），日志里会明确记一条。
              static const float kIdentityMatrix[16] = {
                  1.0f, 0.0f, 0.0f, 0.0f,
                  0.0f, 1.0f, 0.0f, 0.0f,
                  0.0f, 0.0f, 1.0f, 0.0f,
                  0.0f, 0.0f, 0.0f, 1.0f};
              std::memcpy(externalTransform, kIdentityMatrix, sizeof(externalTransform));
              loggedTransform = false;  // 下一帧仍拿不到就再记一次
            } else {
              matrixChanged = std::memcmp(externalTransform, lastLoggedTransform,
                                          sizeof(externalTransform)) != 0;
            }
            // 诊断（一次性 + 变化时）：矩阵语义是「裁剪区域 + 翻转/旋转」，来自
            // native_image.h 的 "transform matrix of the texture image by producer
            // transform type"。典型值应含 scaleY=-1 或平移 1（列主序）。
            const long long matrixNowMs = NowMs();
            if (!loggedTransform ||
                (matrixChanged && matrixNowMs - lastTransformLogMs >= 1000)) {
              loggedTransform = true;
              lastTransformLogMs = matrixNowMs;
              std::memcpy(lastLoggedTransform, externalTransform,
                          sizeof(lastLoggedTransform));
              char line[400];
              std::snprintf(
                  line, sizeof(line),
                  "session=%s texture transform: %.3f %.3f %.3f %.3f | %.3f %.3f "
                  "%.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
                  renderer->sessionId.c_str(), externalTransform[0],
                  externalTransform[1], externalTransform[2], externalTransform[3],
                  externalTransform[4], externalTransform[5], externalTransform[6],
                  externalTransform[7], externalTransform[8], externalTransform[9],
                  externalTransform[10], externalTransform[11], externalTransform[12],
                  externalTransform[13], externalTransform[14], externalTransform[15]);
              LogLine(line);
            }
            if (drawExternalAndSwap()) {
              geometryDirty = false;
              ++externalPerfSwaps;
              if (pickedUpUs != 0) externalPickupSwap.add(NowUs() - pickedUpUs);
              if (arrivedUs > 0 && NowUs() >= arrivedUs)
                externalFrameToSwap.add(NowUs() - arrivedUs);
              const long long nowMs = NowMs();
              if (lastLumaProbeMs == 0 || nowMs - lastLumaProbeMs >= 500) {
                lastLumaProbeMs = nowMs;
                probeExternalLuma();
              }
              logExternalStatsIfDue();
            } else if (swapFailures >= kMaxSwapFailures) {
              setFailure(EglErrorText("eglSwapBuffers (repeated)", lastEglError));
              break;
            }
          } else if (lastSurfaceFailLogMs == 0 ||
                     NowMs() - lastSurfaceFailLogMs >= 1000) {
            lastSurfaceFailLogMs = NowMs();
            // 窗口失效（surface 被销毁后重建）时 UpdateSurfaceImage 会失败。限频
            // 记一条日志并继续按 4ms 轮询；真正恢复需要重新建立串流（见 render.h
            // 里对 renderReleaseExternalSource 的说明）。
            char line[224];
            std::snprintf(line, sizeof(line),
                          "session=%s UpdateSurfaceImage failed (%d); waiting for a new "
                          "stream to re-acquire the source",
                          renderer->sessionId.c_str(), updateResult);
            LogLine(line);
          }
          publishStatsIfDue("running");
        } else if (geometryDirty && presented && externalTexture != 0) {
          // 尺寸/布局变化或缩放平移：用已经解码好的外部纹理重画一次。
          if (drawExternalAndSwap()) geometryDirty = false;
          publishStatsIfDue("running");
        } else {
          publishStatsIfDue("running");
          // 等帧：帧可用回调唤醒，4ms 兜底轮询（与 BGRA 路径的空闲节奏一致）。
          std::unique_lock<std::mutex> lock(renderer->mutex);
          renderer->wake.wait_for(lock, std::chrono::milliseconds(kIdlePollMs), [&]() {
            return renderer->stopRequested || renderer->suspended ||
                   frameSignal->frameAvailable.load() || hasPendingTask();
          });
        }
        continue;
      }
      // 本次轮询的取帧时刻（0 = 本轮没取到帧），pickupToSwap 的起点。
      long long pickupUs = 0;
      tianguan::FrameGrab grab;
      tianguan::FrameGrabTiming grabTiming;
      const tianguan::FrameGrabStatus status = tianguan::takeFrameStruct(
          renderer->sessionId, staging.empty() ? nullptr : staging.data(),
          staging.size(), afterVersion, forceFull, grab, &grabTiming);
      diagLockWaitSumUs += grabTiming.lockWaitUs;
      diagLockWaitN += 1;
      if (grabTiming.lockWaitUs > diagLockWaitMaxUs) diagLockWaitMaxUs = grabTiming.lockWaitUs;
      if (status == tianguan::FrameGrabDelivered) {
        diagCopySumUs += grabTiming.copyUs;
        diagCopyN += 1;
        if (grabTiming.copyUs > diagCopyMaxUs) diagCopyMaxUs = grabTiming.copyUs;
        // 帧延迟 1：镜像提交时刻（producer 打的 commitTimeUs）到本次取到帧。
        // 只有 Sunshine 提交路径会打这个时间戳，其它协议为 0，不计样本。
        pickupUs = NowUs();
        if (grab.commitTimeUs > 0 && pickupUs >= grab.commitTimeUs) {
          framePerfLag.add(pickupUs - grab.commitTimeUs);
        }
      }
      if (status == tianguan::FrameGrabInvalidSession) {
        // The session is gone (window closed / disconnect): stop cleanly.
        setFailure("session closed");
        break;
      }
      if (status == tianguan::FrameGrabTooSmall) {
        // Grow the staging buffer to the full mirror; the next grab is forced
        // full so the resized buffer starts from a complete frame.
        if (grab.fbWidth > 0 && grab.fbHeight > 0 &&
            grab.fbWidth <= 4096 && grab.fbHeight <= 4096) {
          staging.assign(static_cast<size_t>(grab.fbWidth) * grab.fbHeight * 4, 0);
        }
        forceFull = true;
        continue;
      }
      if (status != tianguan::FrameGrabDelivered) {
        // Nothing new to upload. Present only what is already in the texture:
        // a frame whose swap failed (presentPending) or a changed layout
        // (geometryDirty). Otherwise stay idle — no re-upload, no swap.
        ++skips;
        if ((presentPending || (geometryDirty && presented)) && texture != 0) {
          if (drawAndSwap()) {
            geometryDirty = false;
          } else if (swapFailures >= kMaxSwapFailures) {
            setFailure(EglErrorText("eglSwapBuffers (repeated)", lastEglError));
            break;
          }
        }
        publishStatsIfDue("running");
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
        continue;
      }
      // Delivered. A partial region is only handed to us with a known
      // baseline, but the texture may not exist yet (first frame after the
      // ArkTS path ran, or after a renderer restart): demand a full frame.
      if (!grab.full && texture == 0) {
        forceFull = true;
        continue;
      }
      if (texture == 0 || (grab.full && (grab.w != texWidth || grab.h != texHeight))) {
        // First frame or remote resolution change: (re)allocate the texture
        // with the full mirror's geometry. texture == 0 implies grab.full (a
        // partial grab is refused above until the texture exists).
        if (texture == 0) glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, grab.w, grab.h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        const GLenum allocError = glGetError();
        if (allocError != GL_NO_ERROR) {
          char buffer[192];
          std::snprintf(buffer, sizeof(buffer), "glTexImage2D failed (gl=0x%04x)",
                        static_cast<unsigned int>(allocError));
          setFailure(buffer);
          break;
        }
        texWidth = grab.w;
        texHeight = grab.h;
        geometryDirty = true;
      }
      // Upload what changed. staging rows are packed (w*4) exactly as
      // takeFrameStruct copies them, so no GL_UNPACK_ROW_LENGTH is needed.
      glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
      const long long uploadStartUs = NowUs();
      glTexSubImage2D(GL_TEXTURE_2D, 0, grab.x, grab.y, grab.w, grab.h, GL_RGBA,
                      GL_UNSIGNED_BYTE, staging.data());
      const GLenum uploadError = glGetError();
      const long long uploadUs = NowUs() - uploadStartUs;
      diagUploadSumUs += uploadUs;
      diagUploadN += 1;
      if (uploadUs > diagUploadMaxUs) diagUploadMaxUs = uploadUs;
      if (uploadError != GL_NO_ERROR) {
        char buffer[192];
        std::snprintf(buffer, sizeof(buffer), "glTexSubImage2D failed (gl=0x%04x)",
                      static_cast<unsigned int>(uploadError));
        setFailure(buffer);
        break;
      }
      ++uploads;
      if (grab.full) ++fullUploads;
      afterVersion = grab.version;
      forceFull = false;
      // The pixels are in the texture now; only the present can still fail,
      // and presentPending makes the idle path retry it without re-uploading.
      presentPending = true;
      if (drawAndSwap()) {
        geometryDirty = false;
        // 帧延迟 2：取到这一帧到 eglSwapBuffers 返回（拷贝/上传/绘制都在内）。
        if (pickupUs != 0) framePerfPickupToSwap.add(NowUs() - pickupUs);
      } else if (swapFailures >= kMaxSwapFailures) {
        setFailure(EglErrorText("eglSwapBuffers (repeated)", lastEglError));
        break;
      }

      const long long nowMs = NowMs();
      if (nowMs - lastLogMs >= 5000) {
        lastLogMs = nowMs;
        // 结算 5 秒帧延迟窗口并发布快照（render.h 的 RenderFramePerf）：
        // 后端 5 秒汇总行与 statusJson.perf 都读这份快照。
        const long long perfWindowUs = NowUs() - framePerfWindowStartUs;
        tianguan::RenderFramePerf framePerf;
        framePerf.windowMs =
            perfWindowUs > 0 ? static_cast<unsigned long long>(perfWindowUs / 1000) : 0;
        framePerf.outFps =
            perfWindowUs > 0
                ? static_cast<double>(framePerfSwaps) * 1000000.0 /
                      static_cast<double>(perfWindowUs)
                : 0.0;
        framePerf.lagAvgMs = framePerfLag.avgMs();
        framePerf.lagP95Ms = framePerfLag.p95Ms();
        framePerf.pickupToSwapAvgMs = framePerfPickupToSwap.avgMs();
        framePerf.pickupToSwapP95Ms = framePerfPickupToSwap.p95Ms();
        {
          std::lock_guard<std::mutex> lock(g_framePerfMutex);
          g_framePerf = framePerf;
        }
        framePerfLag.reset();
        framePerfPickupToSwap.reset();
        framePerfSwaps = 0;
        framePerfWindowStartUs = NowUs();
        char line[640];
        std::snprintf(line, sizeof(line),
                      "session=%s frames=%lld fps=%.1f uploads=%lld full=%lld skips=%lld "
                      "swapFail=%lld size=%dx%d tex=%dx%d ver=%d elapsed=%lldms "
                      "lag=%.1f/%.1fms pickupSwap=%.1f/%.1fms",
                      renderer->sessionId.c_str(), frames, fps, uploads, fullUploads, skips,
                      swapFailures, surfaceWidth, surfaceHeight, texWidth, texHeight,
                      afterVersion, nowMs - startedAt, framePerf.lagAvgMs,
                      framePerf.lagP95Ms, framePerf.pickupToSwapAvgMs,
                      framePerf.pickupToSwapP95Ms);
        LogDebug(line);
      }
      publishStatsIfDue("running");
    }
  } while (false);

  // 零拷贝源的收尾：先让帧可用回调失效（late callback 只碰 shared_ptr 信号），
  // 再销毁 NativeImage 与 OES 纹理。仍在本线程、GL 上下文尚未销毁之前。
  {
    std::lock_guard<std::mutex> lock(frameSignal->mutex);
    frameSignal->alive = false;
    frameSignal->owner.reset();
    frameSignal->frameAvailable.store(false);
  }
  if (nativeImage != nullptr) {
    OH_NativeImage_UnsetOnFrameAvailableListener(nativeImage);
    OH_NativeImage_Destroy(&nativeImage);
  }
  if (lumaFbo != 0) glDeleteFramebuffers(1, &lumaFbo);
  if (lumaRenderbuffer != 0) glDeleteRenderbuffers(1, &lumaRenderbuffer);
  if (externalTexture != 0) glDeleteTextures(1, &externalTexture);
  if (externalVertexBuffer != 0) glDeleteBuffers(1, &externalVertexBuffer);
  if (externalProgram != 0) glDeleteProgram(externalProgram);
  externalActive = false;
  renderer->externalSourceActive.store(false);
  // 放开还阻塞在 taskDone 上的控制命令调用方（线程已经结束，不会再有结果）。
  {
    std::lock_guard<std::mutex> lock(renderer->taskMutex);
    renderer->loopFinished.store(true);
    renderer->taskKind = 0;
    renderer->taskOk = false;
    renderer->taskFinished = true;
  }
  renderer->taskDone.notify_all();

  // Tear everything this thread created down, in reverse order. GL objects
  // first (the context is still current), then its EGL surface and context,
  // then the native window. The shared display is intentionally NOT
  // terminated: other renderers (and the spike) keep using it.
  if (texture != 0) glDeleteTextures(1, &texture);
  if (vertexBuffer != 0) glDeleteBuffers(1, &vertexBuffer);
  if (program != 0) glDeleteProgram(program);
  if (display != EGL_NO_DISPLAY) {
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(display, eglSurface);
    if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
  }
  if (window != nullptr) OH_NativeWindow_DestroyNativeWindow(window);

  {
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    renderer->stats.state = failure.empty() ? "stopped" : "failed";
    renderer->stats.error = failure;
    renderer->stats.frames = frames;
    renderer->stats.swapFailures = swapFailures;
    renderer->stats.uploads = uploads;
    renderer->stats.fullUploads = fullUploads;
    renderer->stats.skips = skips;
    renderer->stats.fps = fps;
    renderer->stats.width = surfaceWidth;
    renderer->stats.height = surfaceHeight;
    renderer->stats.texWidth = texWidth;
    renderer->stats.texHeight = texHeight;
    renderer->stats.frameVersion = afterVersion;
    renderer->stats.elapsedMs = NowMs() - startedAt;
    renderer->stats.lastEglError = static_cast<int>(lastEglError);
    renderer->stats.suspended = false;
  }
  char line[384];
  std::snprintf(line, sizeof(line),
                "session=%s render thread stopped: state=%s frames=%lld uploads=%lld "
                "skips=%lld elapsed=%lldms surface=%s",
                renderer->sessionId.c_str(), failure.empty() ? "stopped" : "failed",
                frames, uploads, skips, NowMs() - startedAt, surfaceIdText.c_str());
  LogLine(line);
}

}  // namespace

void* tianguan::SharedEGLDisplay() {
  return AcquireSharedDisplay();
}

// 最近一个有帧的 5 秒窗口的帧延迟快照（见 render.h）。读的是算好的值，锁只保护
// 一个 40 字节的结构；没有渲染器时全 0（windowMs == 0）。
tianguan::RenderFramePerf tianguan::renderFramePerf() {
  std::lock_guard<std::mutex> lock(g_framePerfMutex);
  return g_framePerf;
}

namespace tianguan {

std::string attachRenderSurface(const std::string& sessionId, const std::string& surfaceId,
                                bool startSuspended) {
  if (sessionId.empty()) return "ERROR:INVALID_SESSION";
  uint64_t parsed = 0;
  if (surfaceId.empty() || surfaceId.size() > 32) return "ERROR:INVALID_SURFACE_ID";
  for (size_t i = 0; i < surfaceId.size(); ++i) {
    if (surfaceId[i] < '0' || surfaceId[i] > '9') return "ERROR:INVALID_SURFACE_ID";
  }
  parsed = std::strtoull(surfaceId.c_str(), nullptr, 10);
  if (parsed == 0) return "ERROR:INVALID_SURFACE_ID";

  std::shared_ptr<Renderer> renderer(new Renderer(sessionId, startSuspended));
  // The frame-available callback reaches the renderer through this weak_ptr, so
  // a callback that races with renderer teardown cannot resurrect or touch a
  // freed object.
  renderer->frameSignal->owner = renderer;
  {
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    renderer->stats.state = "starting";
    renderer->stats.surfaceId = surfaceId;
    renderer->stats.suspended = startSuspended;
  }
  // Spawn the thread BEFORE publishing the renderer, so a concurrent detach
  // can always see (and hand off) a fully constructed thread.
  try {
    renderer->thread = std::thread(RenderLoop, renderer, parsed, surfaceId);
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    renderer->stats.state = "failed";
    renderer->stats.error = std::string("std::thread: ") + error.what();
    return "ERROR:THREAD_START";
  }
  // Replace any previous renderer for this session (a dead one from a failed
  // surface, or a live one when the surface was recreated in one step).
  std::shared_ptr<Renderer> previous;
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto found = g_renderers.find(sessionId);
    if (found != g_renderers.end()) {
      previous = found->second;
      g_renderers.erase(found);
    }
    g_renderers[sessionId] = renderer;
  }
  if (previous) StopRendererAsync(previous, "replaced by new attach");
  char line[192];
  std::snprintf(line, sizeof(line), "session=%s attach surface=%s suspended=%d",
                sessionId.c_str(), surfaceId.c_str(), startSuspended ? 1 : 0);
  LogLine(line);
  return "OK:ATTACHED";
}

std::string detachRenderSurface(const std::string& sessionId) {
  std::shared_ptr<Renderer> renderer;
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto found = g_renderers.find(sessionId);
    if (found == g_renderers.end()) return "OK:NOT_ATTACHED";
    renderer = found->second;
    g_renderers.erase(found);
  }
  // Joining happens on a reaper thread; this call returns immediately so UI
  // callbacks (surface destroyed, page teardown) never wait on the driver.
  StopRendererAsync(renderer, "detach");
  return "OK:DETACHED";
}

std::string renderStats(const std::string& sessionId) {
  std::shared_ptr<Renderer> renderer;
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto found = g_renderers.find(sessionId);
    if (found == g_renderers.end()) return "ERROR:NO_RENDERER";
    renderer = found->second;
  }
  RenderStats snapshot;
  {
    // statsMutex first, then the control mutex — the same order everywhere.
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    snapshot = renderer->stats;
  }
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    snapshot.suspended = renderer->suspended;
  }
  char buffer[1536];
  std::snprintf(
      buffer, sizeof(buffer),
      "{\"state\":\"%s\",\"surfaceId\":\"%s\",\"error\":\"%s\",\"frames\":%lld,"
      "\"swapFailures\":%lld,\"uploads\":%lld,\"fullUploads\":%lld,\"skips\":%lld,"
      "\"fps\":%.1f,\"width\":%d,\"height\":%d,\"texWidth\":%d,\"texHeight\":%d,"
      "\"frameVersion\":%d,\"elapsedMs\":%lld,\"lastEglError\":%d,\"suspended\":%s,"
      "\"contentLeft\":%d,\"contentTop\":%d,\"contentScale\":%.6f,"
      "\"contentScaleY\":%.6f,\"contentW\":%d,\"contentH\":%d,"
      "\"zoom\":%.4f,\"panX\":%.3f,\"panY\":%.3f,\"maxZoom\":%.1f}",
      JsonEscape(snapshot.state).c_str(), JsonEscape(snapshot.surfaceId).c_str(),
      JsonEscape(snapshot.error).c_str(), snapshot.frames, snapshot.swapFailures,
      snapshot.uploads, snapshot.fullUploads, snapshot.skips, snapshot.fps,
      snapshot.width, snapshot.height, snapshot.texWidth, snapshot.texHeight,
      snapshot.frameVersion, snapshot.elapsedMs, snapshot.lastEglError,
      snapshot.suspended ? "true" : "false", snapshot.contentLeft, snapshot.contentTop,
      snapshot.contentScale, snapshot.contentScaleY, snapshot.contentW, snapshot.contentH,
      snapshot.zoom, snapshot.panX, snapshot.panY, kMaxViewZoom);
  return std::string(buffer);
}

std::string setViewTransform(const std::string& sessionId, double zoom, double panX,
                             double panY) {
  std::shared_ptr<Renderer> renderer;
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto found = g_renderers.find(sessionId);
    if (found == g_renderers.end()) return "ERROR:NO_RENDERER";
    renderer = found->second;
  }
  // Zoom is clamped here (contract: [1, kMaxViewZoom]); pan is stored as
  // requested and clamped against the live surface size while drawing, so a
  // rotation/resize automatically re-clamps it instead of leaving a gap.
  const double clampedZoom = ClampViewZoom(zoom);
  const double safePanX = std::isfinite(panX) ? panX : 0.0;
  const double safePanY = std::isfinite(panY) ? panY : 0.0;
  {
    std::lock_guard<std::mutex> lock(renderer->viewMutex);
    renderer->viewZoom = clampedZoom;
    renderer->viewPanX = safePanX;
    renderer->viewPanY = safePanY;
  }
  // Redraw the already-uploaded texture on the next loop iteration even when
  // the session is idle (no new frames arriving).
  renderer->viewDirty.store(true);
  return "OK";
}

std::string setRenderSurfaceSuspended(const std::string& sessionId, bool suspended) {
  std::shared_ptr<Renderer> renderer;
  {
    std::lock_guard<std::mutex> lock(g_registryMutex);
    const auto found = g_renderers.find(sessionId);
    if (found == g_renderers.end()) return "OK:NO_RENDERER";
    renderer = found->second;
  }
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    renderer->suspended = suspended;
  }
  renderer->wake.notify_all();
  {
    std::lock_guard<std::mutex> lock(renderer->statsMutex);
    renderer->stats.suspended = suspended;
    if (suspended && renderer->stats.state == "running") {
      renderer->stats.state = "suspended";
    } else if (!suspended && renderer->stats.state == "suspended") {
      // The loop republishes "running" on its next iteration; set it here so
      // an immediate stats read after resume is not stale.
      renderer->stats.state = "running";
    }
  }
  char line[192];
  std::snprintf(line, sizeof(line), "session=%s render %s", sessionId.c_str(),
                suspended ? "suspended" : "resumed");
  LogLine(line);
  return suspended ? "OK:SUSPENDED" : "OK:RESUMED";
}

namespace {

std::shared_ptr<Renderer> FindRenderer(const std::string& sessionId) {
  std::lock_guard<std::mutex> lock(g_registryMutex);
  const auto found = g_renderers.find(sessionId);
  return found == g_renderers.end() ? std::shared_ptr<Renderer>() : found->second;
}

void RenderLogLine(const std::string& line) { LogLine(line.c_str()); }

}  // namespace

bool renderCreateExternalSource(const std::string& sessionId, unsigned int* textureId,
                                NativeWindow** nativeWindow) {
  if (sessionId.empty() || textureId == nullptr || nativeWindow == nullptr) return false;
  // 解码线程在第一次 setup 时调用：ArkTS 的 XComponent surface 可能还没 attach，
  // 这里最多等 10 秒（每 20ms 查一次注册表）。等不到就让调用方回退 Buffer 模式。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::shared_ptr<Renderer> renderer;
  while (std::chrono::steady_clock::now() < deadline) {
    renderer = FindRenderer(sessionId);
    if (renderer) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!renderer) {
    RenderLogLine("render surface not attached within 10s; surface source unavailable");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(renderer->mutex);
    if (renderer->stopRequested) return false;
    // 窗口被隐藏时渲染线程停在 park 里，控制命令不会被处理；直接失败让视频路径
    // 回退 Buffer 模式，避免白等 10 秒。
    if (renderer->suspended) {
      RenderLogLine("render surface suspended; surface source unavailable");
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> lock(renderer->taskMutex);
    if (renderer->taskKind != 0 || renderer->loopFinished.load()) return false;
    renderer->taskKind = 1;
    renderer->taskOk = false;
    renderer->taskFinished = false;
    renderer->taskTextureId = 0;
    renderer->taskWindow = nullptr;
  }
  renderer->wake.notify_all();
  {
    std::unique_lock<std::mutex> lock(renderer->taskMutex);
    if (!renderer->taskDone.wait_until(lock, deadline, [&]() {
          return renderer->taskFinished || renderer->loopFinished.load();
        })) {
      RenderLogLine("surface source creation timed out on the render thread");
      return false;
    }
    if (!renderer->taskOk) return false;
    *textureId = renderer->taskTextureId;
    *nativeWindow = renderer->taskWindow;
  }
  return true;
}

void renderSetExternalSourceSize(const std::string& sessionId, int width, int height) {
  if (width <= 0 || height <= 0) return;
  const std::shared_ptr<Renderer> renderer = FindRenderer(sessionId);
  if (!renderer) return;
  // 只记录尺寸并唤醒渲染线程：下一帧（或这一次的空闲轮询）会把 contain 几何按新
  // 尺寸重算，再经 drawExternalAndSwap/publishStatsIfDue 发布到 renderStats。
  renderer->sourceWidth.store(width);
  renderer->sourceHeight.store(height);
  renderer->wake.notify_all();
}

void renderReleaseExternalSource(const std::string& sessionId) {
  const std::shared_ptr<Renderer> renderer = FindRenderer(sessionId);
  if (!renderer) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  {
    std::unique_lock<std::mutex> lock(renderer->taskMutex);
    // 等上一个命令结束（正常不会有）；渲染线程已经结束就什么都不用做。
    renderer->taskDone.wait_until(lock, deadline, [&]() {
      return renderer->taskKind == 0 || renderer->loopFinished.load();
    });
    if (renderer->loopFinished.load()) return;
    renderer->taskKind = 2;
    renderer->taskOk = false;
    renderer->taskFinished = false;
  }
  renderer->wake.notify_all();
  {
    std::unique_lock<std::mutex> lock(renderer->taskMutex);
    renderer->taskDone.wait_until(lock, deadline, [&]() {
      return renderer->taskFinished || renderer->loopFinished.load();
    });
  }
}

}  // namespace tianguan
