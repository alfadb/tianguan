// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 视频渲染器：OHOS AVCodec 硬解（`OH_VideoDecoder`，Buffer 模式）。
//
// 数据流：
//   common-c 视频线程
//     submitDecodeUnit()：把 PDECODE_UNIT 的 bufferList 按顺序拼成一个 AnnexB
//       缓冲（H.264/HEVC 的 common-c buffer 已经各自带起始码，SPS/PPS/VPS 与
//       PICDATA 直接相邻拼接即可，不再补起始码）-> 有解码器交还的输入 buffer 就
//       立刻填好并 PushInputBuffer，否则进 4 深的有界队列。
//   AVCodec 输入回调线程（onNeedInputBuffer）
//     把队列里的解码单元塞进刚交还的输入 buffer 后 Push；队列为空时把这个输入
//     buffer 记进 freeInputs_，留给下一次 submitDecodeUnit 直接下发。
//   AVCodec 输出回调线程（onNewOutputBuffer）
//     取 OH_AVBuffer 地址 + 输出描述（stride / sliceHeight / PIC_WIDTH/PIC_HEIGHT
//     / 像素格式）-> libswscale 把 NV12/NV21 转成 BGRA（成员缓冲，w*h*4）-> 首帧
//     或尺寸变化先 sink.resize(w,h)，再 sink.commit 整帧矩形 -> FreeOutputBuffer。
//
// 线程与锁（这是本文件最需要小心的部分）：
//   * mutex_ 只保护状态、输入队列和 freeInputs_，是短临界区；任何 AVCodec 接口都
//     不在持有它的时候调用。原因是 Destroy/Stop 可能同步等解码器回调线程退出，
//     持锁调用会与回调里的加锁互等。
//   * codecApiMutex_ 只用来串行化 PushInputBuffer：submitDecodeUnit 与输入回调
//     可能来自不同线程，两个 Push 不能并发。FreeOutputBuffer 只在输出回调线程
//     里调用，刻意不参与这把锁，避免「push 等 buffer、free 等锁」的自锁。
//   * 两把锁的获取顺序只有 codecApiMutex_ -> mutex_ 一种（pushInput 持 api 锁时
//     可能走进 logWarnThrottled），没有任何路径先拿 mutex_ 再拿 codecApiMutex_，
//     所以不会出现锁序反转。
//   * activeCalls_ 统计正在使用解码器的 submit/回调（ActiveCall 作用域守卫）。
//     stop() 先把 running_ 置 false（之后新进入的回调直接返回），再等 activeCalls_
//     归零，最后才 Stop/Destroy，因此回调线程永远看不到已经释放的解码器，也不会
//     有人在 Destroy 之后调用 AVCodec。
//   * sws 上下文与 BGRA 缓冲只在输出回调线程使用；stop() 在等 activeCalls_ 归零
//     之后才释放它们。
//   * setup() 在调用 Start() 之前就发布 codec_/running_（解码器可能在 Start 返回
//     的瞬间交还输入 buffer），started_ 标记 Start 是否已经成功：未成功时交还的
//     输入 buffer 只是存进 freeInputs_，不 Push。
//   * 分段耗时统计（g_videoPerf，见下面「5 秒窗口」）只用自己的一把锁，做几次
//     时钟读取和一次直方图自增；任何调用点都不在持有 mutex_/codecApiMutex_ 时写它。
//
// 本次范围：H.264/HEVC、Buffer 模式的 CPU 输出（NV12/NV21 -> BGRA）。Surface
// 模式直出纹理、P010 十比特、AV1（OBU 而非 AnnexB）不在本次实现内，见文件末尾。
#include "sunshine_video.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// FFmpeg 的 swscale 头没有 extern "C" 保护（上游按 C 使用），C++ 里必须自己包一层，
// 否则链接不到 libswscale.a 的 C 符号。
extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_buffer/native_buffer.h>
#include <native_window/external_window.h>

#if defined(__MUSL__)
#include <hilog/log.h>
#endif

namespace tianguan {
namespace {

// 与 sunshine_backend.cxx / rdp_backend.cxx 相同的日志策略：一次性/错误走 WARN，
// 周期统计走 DEBUG；日志只含格式、尺寸、计数，不含任何画面内容或凭据。
void logWarn(const std::string& line) {
#if defined(__MUSL__)
  OH_LOG_Print(LOG_APP, LOG_WARN, 0, "TianguanDiag", "%{public}s", line.c_str());
#else
  std::fprintf(stderr, "W TianguanDiag %s\n", line.c_str());
  std::fflush(stderr);
#endif
}

void logDebug(const std::string& line) {
#if defined(__MUSL__)
  OH_LOG_Print(LOG_APP, LOG_DEBUG, 0, "TianguanDiag", "%{public}s", line.c_str());
#else
  std::fprintf(stderr, "D TianguanDiag %s\n", line.c_str());
  std::fflush(stderr);
#endif
}

std::string formatText(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return std::string(buffer);
}

uint64_t monotonicMillis() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

uint64_t monotonicMicros() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

// ---------------------------------------------------------------------------
// 分段耗时统计（5 秒窗口）
//
// 每帧只做几次 steady_clock 读取和一次直方图自增：avg 用整数和，P95 用 1 ms 一桶
// 的直方图（2000 ms 之后落进溢出桶），不对样本排序，也不保存无界样本。聚合状态
// 是进程级的（common-c 同时只允许一个串流），setup() 时重置；读取方是后端（5 秒
// 汇总日志）和宿主（statusJson.perf），拿到的是算好的快照，不等待解码。
// ---------------------------------------------------------------------------
const size_t kPerfBucketCount = 2001;  // 0..1999 ms 各一桶 + >=2000 ms 溢出桶
const uint64_t kPerfWindowUs = 5000000;  // 5 秒

struct PerfMetricWindow {
  PerfMetricWindow() : count(0), sumUs(0) {
    std::memset(buckets, 0, sizeof(buckets));
  }
  void add(uint64_t us) {
    ++count;
    sumUs += us;
    size_t bucket = static_cast<size_t>(us / 1000);
    if (bucket >= kPerfBucketCount) bucket = kPerfBucketCount - 1;
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
  // 直方图桶下界即毫秒值；桶宽 1 ms，误差 < 1 ms。
  double p95Ms() const {
    if (count == 0) return 0.0;
    const uint64_t target = (count * 95 + 99) / 100;  // ceil(0.95 * count)
    uint64_t running = 0;
    for (size_t i = 0; i < kPerfBucketCount; ++i) {
      running += buckets[i];
      if (running >= target) return static_cast<double>(i);
    }
    return static_cast<double>(kPerfBucketCount - 1);
  }
  uint64_t count;
  uint64_t sumUs;
  uint32_t buckets[kPerfBucketCount];
};

class VideoPerfAggregator {
 public:
  VideoPerfAggregator()
      : windowStartUs_(0), inFrames_(0), decFrames_(0), droppedUnits_(0) {}

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    windowStartUs_ = 0;
    inFrames_ = 0;
    decFrames_ = 0;
    droppedUnits_ = 0;
    netToSubmit_.reset();
    submitToOutput_.reset();
    convert_.reset();
    commit_.reset();
    last_ = SunshineVideoPerf();
  }

  void noteInFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    ++inFrames_;
  }
  void noteDecodedFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    ++decFrames_;
  }
  void noteDroppedUnit() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    ++droppedUnits_;
  }
  void noteNetToSubmit(uint64_t us) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    netToSubmit_.add(us);
  }
  void noteSubmitToOutput(uint64_t us) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    submitToOutput_.add(us);
  }
  void noteConvert(uint64_t us) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    convert_.add(us);
  }
  void noteCommit(uint64_t us) {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    commit_.add(us);
  }

  SunshineVideoPerf snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    rotateLocked(monotonicMicros());
    return last_;
  }

 private:
  void rotateLocked(uint64_t nowUs) {
    if (windowStartUs_ == 0) {
      windowStartUs_ = nowUs;
      return;
    }
    if (nowUs - windowStartUs_ < kPerfWindowUs) return;
    const uint64_t windowUs = nowUs - windowStartUs_;
    const double seconds = static_cast<double>(windowUs) / 1000000.0;
    SunshineVideoPerf out;
    out.windowMs = windowUs / 1000;
    out.inFrames = inFrames_;
    out.decFrames = decFrames_;
    out.droppedUnits = droppedUnits_;
    out.inFps = seconds > 0.0 ? static_cast<double>(inFrames_) / seconds : 0.0;
    out.decFps = seconds > 0.0 ? static_cast<double>(decFrames_) / seconds : 0.0;
    out.netToSubmitAvgMs = netToSubmit_.avgMs();
    out.netToSubmitP95Ms = netToSubmit_.p95Ms();
    out.submitToOutputAvgMs = submitToOutput_.avgMs();
    out.submitToOutputP95Ms = submitToOutput_.p95Ms();
    out.convertAvgMs = convert_.avgMs();
    out.convertP95Ms = convert_.p95Ms();
    out.commitAvgMs = commit_.avgMs();
    out.commitP95Ms = commit_.p95Ms();
    last_ = out;
    windowStartUs_ = nowUs;
    inFrames_ = 0;
    decFrames_ = 0;
    droppedUnits_ = 0;
    netToSubmit_.reset();
    submitToOutput_.reset();
    convert_.reset();
    commit_.reset();
  }

  std::mutex mutex_;
  uint64_t windowStartUs_;
  uint64_t inFrames_;
  uint64_t decFrames_;
  uint64_t droppedUnits_;
  PerfMetricWindow netToSubmit_;
  PerfMetricWindow submitToOutput_;
  PerfMetricWindow convert_;
  PerfMetricWindow commit_;
  SunshineVideoPerf last_;
};

VideoPerfAggregator g_videoPerf;

// ---------------------------------------------------------------------------
// 端到端延迟的时间戳快照（见 sunshine_video.h 的 SunshineFrameTiming）
//
// presentationTimeUs 是主机 RTP 时钟（Limelight.h:169-171：epoch 为主机第一帧抓屏），
// 不能和客户端 now 直接相减；这里用「客户端 receiveTimeUs - 主机 presentationTimeUs」
// 的滑动最小值估计两端的固定偏移（最小值对应最快的一帧，即最优路径；因此算出的
// captureToSwap 会偏低约「最小单向延迟」，报告里注明）。样本有界（240 帧）。
// ---------------------------------------------------------------------------
const size_t kFrameClockOffsetSamples = 240;

std::mutex g_frameTimingMutex;
SunshineFrameTiming g_latestFrameTiming;
std::deque<long long> g_frameClockOffsets;  // receiveTimeUs - presentationTimeUs（可负）

void resetFrameTimingLocked() {
  g_frameClockOffsets.clear();
  g_latestFrameTiming = SunshineFrameTiming();
}

long long currentClockOffsetLocked() {
  if (g_frameClockOffsets.empty()) return 0;
  long long best = g_frameClockOffsets.front();
  for (std::deque<long long>::const_iterator it = g_frameClockOffsets.begin();
       it != g_frameClockOffsets.end(); ++it) {
    if (*it < best) best = *it;
  }
  return best;
}

// 一帧解码输出（Surface 模式的 RenderOutputBuffer / Buffer 模式的 commit 之后）。
// presentationTimeUs 来自 DECODE_UNIT，receiveTimeUs 来自同一解码单元的配对记录。
void noteFrameTiming(long long presentationTimeUs, long long receiveTimeUs) {
  SunshineFrameTiming timing;
  timing.presentationTimeUs = presentationTimeUs;
  timing.receiveTimeUs = receiveTimeUs;
  timing.notedAtUs = static_cast<long long>(LiGetMicroseconds());
  std::lock_guard<std::mutex> lock(g_frameTimingMutex);
  if (presentationTimeUs != 0 && receiveTimeUs != 0) {
    const long long candidate = receiveTimeUs - presentationTimeUs;
    g_frameClockOffsets.push_back(candidate);
    while (g_frameClockOffsets.size() > kFrameClockOffsetSamples)
      g_frameClockOffsets.pop_front();
    timing.hostToClientOffsetUs = currentClockOffsetLocked();
    timing.offsetValid = true;
    timing.valid = true;
  } else {
    // 主机 PTS 无效（0）：只留 receiveTimeUs，渲染线程会退化成 receiveToSwap。
    timing.offsetValid = !g_frameClockOffsets.empty();
    if (timing.offsetValid) timing.hostToClientOffsetUs = currentClockOffsetLocked();
    timing.valid = receiveTimeUs != 0;
  }
  g_latestFrameTiming = timing;
}

// 有界输入队列的深度：解码器自己还有若干在途 buffer，超过这个数再攒只会增加
// 端到端延迟；满了就让 common-c 去要一个 IDR（画面会跳一下，但不会无限积压）。
const size_t kMaxQueuedDecodeUnits = 4;

// VIDEO_FORMAT_* -> AVCodec mime。AV1 以后再接（common-c 的 AV1 单元没有
// BUFFER_TYPE_* 分类，语义与 AnnexB 不同，见文件末尾）。
const char* mimeForVideoFormat(int videoFormat) {
  switch (videoFormat) {
    case VIDEO_FORMAT_H264:
      return OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    case VIDEO_FORMAT_H265:
      return OH_AVCODEC_MIMETYPE_VIDEO_HEVC;
    default:
      return nullptr;
  }
}

// 一个待下发的解码单元：AnnexB 字节 + PTS(µs) + 是否同步帧。
struct QueuedDecodeUnit {
  QueuedDecodeUnit() : ptsUs(0), syncFrame(false), receiveTimeUs(0) {}
  std::vector<uint8_t> annexB;
  int64_t ptsUs;
  bool syncFrame;
  uint64_t receiveTimeUs;  // DECODE_UNIT.receiveTimeUs（端到端延迟用）
};

// 解码器交还的一个可用输入 buffer（索引 + 实例，回调里给什么就存什么）。
struct FreeInputBuffer {
  FreeInputBuffer() : index(0), buffer(nullptr) {}
  FreeInputBuffer(uint32_t bufferIndex, OH_AVBuffer* instance)
      : index(bufferIndex), buffer(instance) {}
  uint32_t index;
  OH_AVBuffer* buffer;
};

// 一次输入下发的记录：输出回调拿同样的 PTS 回来配对 submitToOutput 间隔。
// 有界（解码器同时在途的帧远少于这个数），满了丢最旧的。
struct PendingSubmit {
  PendingSubmit() : ptsUs(0), submitUs(0), receiveTimeUs(0) {}
  PendingSubmit(int64_t pts, uint64_t atUs, uint64_t receiveUs)
      : ptsUs(pts), submitUs(atUs), receiveTimeUs(receiveUs) {}
  int64_t ptsUs;
  uint64_t submitUs;
  uint64_t receiveTimeUs;  // DECODE_UNIT.receiveTimeUs（客户端 common-c 时钟）
};
const size_t kMaxPendingSubmits = 240;

// AVCodec 硬解渲染器。只有 SunshineVideoSink::create() 会构造它。
class AvCodecVideoSink : public SunshineVideoSink {
 public:
  AvCodecVideoSink()
      : codec_(nullptr),
        running_(false),
        started_(false),
        surfaceMode_(false),
        externalSourceHeld_(false),
        surfaceWidth_(0),
        surfaceHeight_(0),
        activeCalls_(0),
        generation_(0),
        configuredFps_(0),
        lastPtsUs_(0),
        matchedSubmitToOutputSumUs_(0),
        matchedSubmitToOutputCount_(0),
        lastSubmitToOutputUs_(0),
        sws_(nullptr),
        swsWidth_(0),
        swsHeight_(0),
        swsSourceFormat_(AV_PIX_FMT_NONE),
        bgra_(),
        frameWidth_(0),
        frameHeight_(0),
        frameStride_(0),
        frameSliceHeight_(0),
        framePixelFormat_(0),
        geometryDirty_(true),
        committedWidth_(0),
        committedHeight_(0),
        loggedPixelFormat_(false),
        loggedFirstFrame_(false),
        droppedUnits_(0),
        decodeErrors_(0),
        lastWarnMs_(0),
        lastProgressMs_(0) {}

  ~AvCodecVideoSink() override { stop(); }

  void bindFrameSink(const SunshineFrameSink& sink, int generation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    frameSink_ = sink;
    generation_ = generation;
  }

  bool setup(int videoFormat, int width, int height, int fps) override {
    // 同一实例重复 setup（理论上后端每次尝试都新建一个，这里只是不留下旧解码器）。
    stop();
    const char* mime = mimeForVideoFormat(videoFormat);
    if (mime == nullptr) {
      logWarn(formatText("sunshine video: unsupported format 0x%X (%dx%d@%d)",
                         videoFormat, width, height, fps));
      return false;
    }
    if (width <= 0 || height <= 0) {
      logWarn(formatText("sunshine video: invalid %dx%d", width, height));
      return false;
    }

    SunshineFrameSink sink;
    int generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sink = frameSink_;
      generation = generation_;
    }

    // 1) 先试零拷贝 Surface 路径：解码器直接渲染进宿主给的 OH_NativeImage 窗口，
    //    由渲染线程把同一块画面当 OES 外部纹理采样。这样就没有 NV12->BGRA 的 CPU
    //    转换、没有整帧镜像拷贝、也没有每帧 glTexSubImage2D 上传。
    void* externalWindow = nullptr;
    unsigned int externalTexture = 0;
    bool surfaceMode = false;
    if (sink.acquireExternalSource != nullptr) {
      if (sink.acquireExternalSource(sink.user, generation, &externalTexture,
                                     &externalWindow) &&
          externalWindow != nullptr) {
        surfaceMode = true;
      } else {
        logWarn("sunshine video: surface source unavailable (renderer not attached or "
                "timed out), using buffer mode");
      }
    }
    OH_AVCodec* codec =
        surfaceMode ? startDecoder(mime, width, height, fps, externalWindow) : nullptr;
    if (surfaceMode && codec == nullptr) {
      // Surface 路径起不来（Configure/SetSurface/Prepare 失败）就回退，行为与原来
      // 的 Buffer 模式完全一致；外部纹理源由宿主销毁。
      logWarn("sunshine video: surface-mode decoder failed to start, falling back to "
              "buffer mode");
      if (sink.releaseExternalSource != nullptr)
        sink.releaseExternalSource(sink.user, generation);
      surfaceMode = false;
    }
    if (codec == nullptr) codec = startDecoder(mime, width, height, fps, nullptr);
    if (codec == nullptr) return false;
    // Prepare 之后注册的回调线程可能已经就绪，Start 一返回就可能交还输入 buffer。
    // 所以在 Start 之前先把 codec_/running_ 发布出去（否则回调会看到 running_ 为
    // false 而丢掉这些 buffer，解码器永远等不到它们），再用 started_ 区分「Start
    // 还没成功」：那段时间交还的输入 buffer 只进 freeInputs_，等 setup 返回后由
    // 第一次 submitDecodeUnit 直接下发。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stats_ = SunshineVideoStats();
      stats_.videoFormat = videoFormat;
      stats_.width = width;
      stats_.height = height;
      stats_.fps = fps;
      configuredFps_ = fps;
      codec_ = codec;
      running_ = true;
      started_ = false;
      surfaceMode_ = surfaceMode;
      externalSourceHeld_ = surfaceMode;
      surfaceWidth_ = 0;
      surfaceHeight_ = 0;
      queue_.clear();
      freeInputs_.clear();
      geometryDirty_ = true;
      frameWidth_ = 0;
      frameHeight_ = 0;
      frameStride_ = 0;
      frameSliceHeight_ = 0;
      framePixelFormat_ = 0;
      committedWidth_ = 0;
      committedHeight_ = 0;
      loggedPixelFormat_ = false;
      loggedFirstFrame_ = false;
      droppedUnits_ = 0;
      decodeErrors_ = 0;
      lastPtsUs_ = 0;
      lastWarnMs_ = 0;
      lastProgressMs_ = 0;
      pendingSubmits_.clear();
      matchedSubmitToOutputSumUs_ = 0;
      matchedSubmitToOutputCount_ = 0;
      lastSubmitToOutputUs_ = 0;
    }
    // 新的连接从空的 5 秒统计窗口开始（进程级聚合器，见文件头的统计说明）。
    g_videoPerf.reset();
    // 新连接 = 新的 RTP 时间基，主机/客户端时钟偏移要重新估计。
    {
      std::lock_guard<std::mutex> lock(g_frameTimingMutex);
      resetFrameTimingLocked();
    }
    OH_AVErrCode result = OH_VideoDecoder_Start(codec);
    if (result != AV_ERR_OK) {
      logWarn(formatText("sunshine video: Start failed: %d", static_cast<int>(result)));
      // stop() 会等在途回调退出并销毁这个解码器（codec_ 已经发布出去）。
      stop();
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      started_ = true;
    }
    logWarn(formatText(
        "sunshine video: OH_VideoDecoder(%s) started, %s, %dx%d@%d, low-latency=1",
        mime, surfaceMode ? "surface mode (zero-copy)" : "buffer mode NV12", width,
        height, fps));
    return true;
  }

  int submitDecodeUnit(PDECODE_UNIT decodeUnit) override {
    if (decodeUnit == nullptr || decodeUnit->bufferList == nullptr) return DR_OK;

    // 分段耗时 1：common-c 的 enqueueTimeUs 用 PltGetMicroseconds() 取值，那是它
    // 自己的单调时钟（CLOCK_MONOTONIC_RAW 减启动偏移），不能和 steady_clock 直接
    // 相减；LiGetMicroseconds() 是同一时钟，用它取「现在」才得到真实间隔。
    const uint64_t commonNowUs = static_cast<uint64_t>(LiGetMicroseconds());
    if (decodeUnit->enqueueTimeUs != 0 && commonNowUs > decodeUnit->enqueueTimeUs)
      g_videoPerf.noteNetToSubmit(commonNowUs - decodeUnit->enqueueTimeUs);
    g_videoPerf.noteInFrame();

    // 1. 拼 AnnexB（不持锁：buffer 链归调用线程所有，拼的过程只读）。
    QueuedDecodeUnit unit;
    size_t total = 0;
    for (PLENTRY entry = decodeUnit->bufferList; entry != nullptr;
         entry = entry->next) {
      if (entry->data != nullptr && entry->length > 0)
        total += static_cast<size_t>(entry->length);
    }
    if (total == 0) return DR_OK;
    unit.annexB.reserve(total);
    for (PLENTRY entry = decodeUnit->bufferList; entry != nullptr;
         entry = entry->next) {
      if (entry->data == nullptr || entry->length <= 0) continue;
      const uint8_t* data = reinterpret_cast<const uint8_t*>(entry->data);
      unit.annexB.insert(unit.annexB.end(), data, data + entry->length);
    }
    unit.syncFrame = decodeUnit->frameType == FRAME_TYPE_IDR;
    unit.receiveTimeUs = decodeUnit->receiveTimeUs;

    // 2. 入队 / 直接下发。freeInputs_ 非空说明解码器有空闲输入 buffer，立刻填
    //    并 Push（在锁外做）；否则进有界队列。队列满则丢弃这一帧并要 IDR。
    ActiveCall call(this);
    OH_AVCodec* codec = call.codec();
    if (codec == nullptr) return DR_OK;
    FreeInputBuffer input;
    bool hasInput = false;
    int result = DR_OK;
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // 主机给的 presentationTimeUs 以第一个采集帧为 0，单位 µs，正好是
      // OH_AVCodecBufferAttr::pts 的单位；缺失时按 fps 递推。
      unit.ptsUs = decodeUnit->presentationTimeUs > 0
                       ? static_cast<int64_t>(decodeUnit->presentationTimeUs)
                       : lastPtsUs_;
      lastPtsUs_ = unit.ptsUs + (configuredFps_ > 0
                                     ? static_cast<int64_t>(1000000 / configuredFps_)
                                     : 16000);
      if (!freeInputs_.empty()) {
        input = freeInputs_.front();
        freeInputs_.pop_front();
        hasInput = true;
      } else if (queue_.size() < kMaxQueuedDecodeUnits) {
        queue_.push_back(std::move(unit));
      } else {
        ++droppedUnits_;
        dropped = true;
        result = DR_NEED_IDR;
      }
      stats_.bytes += static_cast<unsigned long long>(total);
    }
    if (hasInput) pushInput(codec, input, unit);
    if (dropped) {
      g_videoPerf.noteDroppedUnit();
      logWarnThrottled("sunshine video: input queue full, dropped a decode unit");
    }
    return result;
  }

  void stop() override {
    OH_AVCodec* codec = nullptr;
    SunshineFrameSink sink;
    int generation = 0;
    bool releaseExternal = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      running_ = false;
      started_ = false;
      codec = codec_;
      codec_ = nullptr;
      releaseExternal = externalSourceHeld_;
      externalSourceHeld_ = false;
      surfaceMode_ = false;
      surfaceWidth_ = 0;
      surfaceHeight_ = 0;
      sink = frameSink_;
      generation = generation_;
      queue_.clear();
      freeInputs_.clear();
      pendingSubmits_.clear();
      // 在途的 submit/回调退出前会把 activeCalls_ 减回去；它们看到 running_ 为
      // false 后不会再碰解码器，所以下面的 Stop/Destroy 不会与它们并发。
      if (!idleCond_.wait_for(lock, std::chrono::seconds(3),
                              [this]() { return activeCalls_ == 0; })) {
        logWarn(formatText("sunshine video: %d codec call(s) still running in stop()",
                           activeCalls_));
      }
    }
    if (codec != nullptr) {
      // 不在持锁时调 AVCodec：Destroy 可能同步等解码器回调线程退出。
      std::lock_guard<std::mutex> apiLock(codecApiMutex_);
      OH_VideoDecoder_Stop(codec);  // 已经 EOS/stopped 时返回错误，忽略即可。
      OH_VideoDecoder_Destroy(codec);
      logDebug("sunshine video: OH_VideoDecoder destroyed");
    }
    // 解码器已经停掉，才能归还 Surface 模式的 OHNativeImage 窗口（否则解码器可能
    // 还在往一个正在销毁的 surface 提交帧）。
    if (releaseExternal && sink.releaseExternalSource != nullptr) {
      sink.releaseExternalSource(sink.user, generation);
      logWarn("sunshine video: surface source released");
    }
    // sws 上下文与 BGRA 缓冲只在输出回调线程里用；activeCalls_ 归零之后不会再有
    // 线程使用它们。
    if (sws_ != nullptr) {
      sws_freeContext(sws_);
      sws_ = nullptr;
    }
    swsWidth_ = 0;
    swsHeight_ = 0;
    swsSourceFormat_ = AV_PIX_FMT_NONE;
    std::vector<uint8_t>().swap(bgra_);
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = SunshineVideoStats();
  }

  SunshineVideoStats stats() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

 private:
  // 在途调用守卫：作用域内保证 stop() 不会销毁正在使用的解码器。构造时若
  // running_ 为 false（正在停止）则不进入，codec() 返回 nullptr。
  class ActiveCall {
   public:
    explicit ActiveCall(AvCodecVideoSink* sink) : sink_(sink), codec_(nullptr) {
      std::lock_guard<std::mutex> lock(sink_->mutex_);
      if (sink_->running_ && sink_->codec_ != nullptr) {
        ++sink_->activeCalls_;
        codec_ = sink_->codec_;
      }
    }
    ~ActiveCall() {
      if (codec_ == nullptr) return;
      std::lock_guard<std::mutex> lock(sink_->mutex_);
      if (sink_->activeCalls_ > 0) --sink_->activeCalls_;
      if (sink_->activeCalls_ == 0) sink_->idleCond_.notify_all();
    }
    bool entered() const { return codec_ != nullptr; }
    OH_AVCodec* codec() const { return codec_; }

   private:
    AvCodecVideoSink* sink_;
    OH_AVCodec* codec_;  // stop() 会等本作用域结束才 Destroy
  };

  // ---- AVCodec 回调入口（static -> 实例）------------------------------------

  static void onErrorCallback(OH_AVCodec* codec, int32_t errorCode, void* userData) {
    (void)codec;
    static_cast<AvCodecVideoSink*>(userData)->handleError(errorCode);
  }

  static void onStreamChangedCallback(OH_AVCodec* codec, OH_AVFormat* format,
                                      void* userData) {
    (void)codec;
    static_cast<AvCodecVideoSink*>(userData)->handleStreamChanged(format);
  }

  static void onNeedInputBufferCallback(OH_AVCodec* codec, uint32_t index,
                                        OH_AVBuffer* buffer, void* userData) {
    (void)codec;
    static_cast<AvCodecVideoSink*>(userData)->handleInputBuffer(index, buffer);
  }

  static void onNewOutputBufferCallback(OH_AVCodec* codec, uint32_t index,
                                        OH_AVBuffer* buffer, void* userData) {
    static_cast<AvCodecVideoSink*>(userData)->handleOutputBuffer(codec, index, buffer);
  }

  // ---- 输入路径 -------------------------------------------------------------

  // 创建并 Prepare 解码器。surfaceWindow 非空 = Surface 模式：像素格式用
  // AV_PIXEL_FORMAT_SURFACE_FORMAT，并在 Prepare 之前 OH_VideoDecoder_SetSurface
  // 把 OH_NativeImage 的窗口交给解码器（帧直接渲染进 OES 外部纹理，没有 CPU 可读
  // 缓冲）；为空 = 原来的 Buffer 模式（NV12 输出，由 swscale 转 BGRA）。失败返回
  // nullptr（已打日志并销毁实例）。
  OH_AVCodec* startDecoder(const char* mime, int width, int height, int fps,
                           void* surfaceWindow) {
    OH_AVCodec* codec = OH_VideoDecoder_CreateByMime(mime);
    if (codec == nullptr) {
      logWarn(formatText("sunshine video: CreateByMime(%s) failed (%dx%d@%d)", mime,
                         width, height, fps));
      return nullptr;
    }
    OH_AVFormat* format = OH_AVFormat_Create();
    if (format == nullptr) {
      OH_VideoDecoder_Destroy(codec);
      logWarn("sunshine video: OH_AVFormat_Create failed");
      return nullptr;
    }
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_WIDTH, width);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_HEIGHT, height);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT,
                            surfaceWindow != nullptr ? AV_PIXEL_FORMAT_SURFACE_FORMAT
                                                     : AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, fps);
    // 串流是实时的：低延迟优先，宁可少缓冲也不要增加端到端延迟。平台不支持该
    // 键时 SDK 不报错、退回普通解码流程（见头文件说明）。
    OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
    OH_AVErrCode result = OH_VideoDecoder_Configure(codec, format);
    OH_AVFormat_Destroy(format);
    if (result != AV_ERR_OK) {
      logWarn(formatText("sunshine video: Configure(%s %dx%d) failed: %d", mime,
                         width, height, static_cast<int>(result)));
      OH_VideoDecoder_Destroy(codec);
      return nullptr;
    }
    if (surfaceWindow != nullptr) {
      result = OH_VideoDecoder_SetSurface(
          codec, reinterpret_cast<OHNativeWindow*>(surfaceWindow));
      if (result != AV_ERR_OK) {
        logWarn(formatText("sunshine video: SetSurface failed: %d",
                           static_cast<int>(result)));
        OH_VideoDecoder_Destroy(codec);
        return nullptr;
      }
    }

    // 新接口（API 11+）：Buffer 模式的 onNeedInputBuffer/onNewOutputBuffer 直接给
    // OH_AVBuffer，比旧的 SetCallback（OH_AVMemory + index）少一次索引查表，也才能
    // 用 OH_AVBuffer_SetBufferAttr 设置 pts/flags。
    OH_AVCodecCallback callbacks;
    std::memset(&callbacks, 0, sizeof(callbacks));
    callbacks.onError = onErrorCallback;
    callbacks.onStreamChanged = onStreamChangedCallback;
    callbacks.onNeedInputBuffer = onNeedInputBufferCallback;
    callbacks.onNewOutputBuffer = onNewOutputBufferCallback;
    result = OH_VideoDecoder_RegisterCallback(codec, callbacks, this);
    if (result != AV_ERR_OK) {
      logWarn(formatText("sunshine video: RegisterCallback failed: %d",
                         static_cast<int>(result)));
      OH_VideoDecoder_Destroy(codec);
      return nullptr;
    }
    result = OH_VideoDecoder_Prepare(codec);
    if (result != AV_ERR_OK) {
      logWarn(formatText("sunshine video: Prepare failed: %d",
                         static_cast<int>(result)));
      OH_VideoDecoder_Destroy(codec);
      return nullptr;
    }
    return codec;
  }

  // 把单元填进解码器交还的输入 buffer 并下发。调用方持有 ActiveCall，因此这里
  // 可以放心使用 codec（stop() 会等这一轮结束）。
  bool pushInput(OH_AVCodec* codec, const FreeInputBuffer& input,
                 const QueuedDecodeUnit& unit) {
    uint8_t* addr = OH_AVBuffer_GetAddr(input.buffer);
    const int32_t capacity = OH_AVBuffer_GetCapacity(input.buffer);
    if (addr == nullptr || capacity <= 0 ||
        static_cast<size_t>(capacity) < unit.annexB.size()) {
      logWarnThrottled(formatText(
          "sunshine video: input buffer too small (%d < %zu bytes)",
          static_cast<int>(capacity), unit.annexB.size()));
      return false;
    }
    std::memcpy(addr, unit.annexB.data(), unit.annexB.size());
    OH_AVCodecBufferAttr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.pts = unit.ptsUs;
    attr.size = static_cast<int32_t>(unit.annexB.size());
    attr.offset = 0;
    // 同步帧标志只是给解码器/统计的提示；SPS/PPS 已经和图像数据在同一个 buffer
    // 里，不需要额外的 CODEC_DATA 单元。
    attr.flags = unit.syncFrame ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME
                                : AVCODEC_BUFFER_FLAGS_NONE;
    if (OH_AVBuffer_SetBufferAttr(input.buffer, &attr) != AV_ERR_OK) {
      logWarnThrottled("sunshine video: SetBufferAttr failed");
      return false;
    }
    const uint64_t submitUs = monotonicMicros();
    std::lock_guard<std::mutex> apiLock(codecApiMutex_);
    const OH_AVErrCode result = OH_VideoDecoder_PushInputBuffer(codec, input.index);
    if (result != AV_ERR_OK) {
      logWarnThrottled(formatText("sunshine video: PushInputBuffer failed: %d",
                                  static_cast<int>(result)));
      return false;
    }
    // 分段耗时 2：记下「送进解码器」的时刻，输出回调用同一个 PTS 配对。
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pendingSubmits_.push_back(PendingSubmit(unit.ptsUs, submitUs, unit.receiveTimeUs));
      while (pendingSubmits_.size() > kMaxPendingSubmits) pendingSubmits_.pop_front();
    }
    return true;
  }

  void handleInputBuffer(uint32_t index, OH_AVBuffer* buffer) {
    if (buffer == nullptr) return;
    ActiveCall call(this);
    OH_AVCodec* codec = call.codec();
    if (codec == nullptr) return;
    QueuedDecodeUnit unit;
    bool hasUnit = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        // Start 还没返回：这时交还的输入 buffer 只存起来，等 setup 返回后的第一次
        // submitDecodeUnit 直接下发（此时 Push 会返回 INVALID_STATE，不能现在推）。
        freeInputs_.push_back(FreeInputBuffer(index, buffer));
        return;
      }
      if (!queue_.empty()) {
        unit = std::move(queue_.front());
        queue_.pop_front();
        hasUnit = true;
      } else {
        // 队列空：这个 buffer 先存着，等下一次 submitDecodeUnit 直接下发。解码器
        // 一次会交还它缓冲池里的若干个输入 buffer，所以这里按列表累积，不能只留
        // 最后一个（否则剩下的输入 buffer 永远压在手里，解码器会卡住）。
        freeInputs_.push_back(FreeInputBuffer(index, buffer));
      }
    }
    if (hasUnit) pushInput(codec, FreeInputBuffer(index, buffer), unit);
  }

  // ---- 输出路径 -------------------------------------------------------------

  void handleError(int32_t errorCode) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++decodeErrors_;
    }
    logWarnThrottled(
        formatText("sunshine video: AVCodec error %d", static_cast<int>(errorCode)));
  }

  void handleStreamChanged(OH_AVFormat* format) {
    int32_t width = 0;
    int32_t height = 0;
    if (format != nullptr) {
      if (!OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_WIDTH, &width))
        OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &width);
      if (!OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_HEIGHT, &height))
        OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &height);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      geometryDirty_ = true;
    }
    logWarn(formatText("sunshine video: stream changed -> %dx%d", width, height));
  }

  // Surface 模式的有效画面尺寸：输出描述里的 PIC_WIDTH/PIC_HEIGHT（裁剪后的可见
  // 尺寸）；取不到就退回 Configure 的协商宽高。只在首帧或 onStreamChanged 之后查
  // 一次，然后交给宿主（镜像/状态尺寸 + 渲染线程的 contain 几何）。Surface 模式
  // 没有像素写进镜像（镜像是黑的），所以这里不 commit。
  void updateSurfaceGeometry(OH_AVCodec* codec) {
    int fallbackWidth = 0;
    int fallbackHeight = 0;
    bool need = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      need = geometryDirty_ || surfaceWidth_ <= 0;
      fallbackWidth = stats_.width;
      fallbackHeight = stats_.height;
    }
    if (!need) return;
    int32_t picWidth = 0;
    int32_t picHeight = 0;
    OH_AVFormat* description = OH_VideoDecoder_GetOutputDescription(codec);
    if (description != nullptr) {
      if (!OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_PIC_WIDTH, &picWidth))
        OH_AVFormat_GetIntValue(description, OH_MD_KEY_WIDTH, &picWidth);
      if (!OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_PIC_HEIGHT, &picHeight))
        OH_AVFormat_GetIntValue(description, OH_MD_KEY_HEIGHT, &picHeight);
      OH_AVFormat_Destroy(description);
    }
    if (picWidth <= 0) picWidth = fallbackWidth;
    if (picHeight <= 0) picHeight = fallbackHeight;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      geometryDirty_ = false;
    }
    if (picWidth <= 0 || picHeight <= 0) return;
    SunshineFrameSink sink;
    int generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (surfaceWidth_ == picWidth && surfaceHeight_ == picHeight) return;
      surfaceWidth_ = picWidth;
      surfaceHeight_ = picHeight;
      sink = frameSink_;
      generation = generation_;
    }
    if (sink.setSourceSize != nullptr)
      sink.setSourceSize(sink.user, generation, picWidth, picHeight);
    logWarn(formatText("sunshine video: surface source %dx%d", picWidth, picHeight));
  }

  // 读解码器输出描述（stride / sliceHeight / 可见宽高 / 像素格式）。输出描述是
  // SDK 明确推荐的取法；PIC_WIDTH/PIC_HEIGHT 是裁剪后的可见尺寸，stride 是补齐
  // 后的行字节数，sliceHeight 是单个平面实际分配的行数（UV 平面从
  // Y + sliceHeight*stride 开始）。取不到的字段用 NativeBuffer 的 config 兜底。
  bool refreshOutputGeometry(OH_AVCodec* codec, OH_AVBuffer* buffer) {
    int32_t picWidth = 0;
    int32_t picHeight = 0;
    int32_t stride = 0;
    int32_t sliceHeight = 0;
    int32_t pixelFormat = 0;
    bool havePixelFormat = false;
    OH_AVFormat* description = OH_VideoDecoder_GetOutputDescription(codec);
    if (description != nullptr) {
      if (!OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_PIC_WIDTH, &picWidth))
        OH_AVFormat_GetIntValue(description, OH_MD_KEY_WIDTH, &picWidth);
      if (!OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_PIC_HEIGHT, &picHeight))
        OH_AVFormat_GetIntValue(description, OH_MD_KEY_HEIGHT, &picHeight);
      OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_STRIDE, &stride);
      OH_AVFormat_GetIntValue(description, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &sliceHeight);
      havePixelFormat =
          OH_AVFormat_GetIntValue(description, OH_MD_KEY_PIXEL_FORMAT, &pixelFormat);
      OH_AVFormat_Destroy(description);
    }
    if (stride <= 0 || picWidth <= 0 || picHeight <= 0 || sliceHeight <= 0) {
      OH_NativeBuffer* nativeBuffer = OH_AVBuffer_GetNativeBuffer(buffer);
      if (nativeBuffer != nullptr) {
        OH_NativeBuffer_Config config;
        std::memset(&config, 0, sizeof(config));
        OH_NativeBuffer_GetConfig(nativeBuffer, &config);
        OH_NativeBuffer_Unreference(nativeBuffer);
        if (stride <= 0) stride = config.stride;
        if (picWidth <= 0) picWidth = config.width;
        if (picHeight <= 0) picHeight = config.height;
        if (sliceHeight <= 0) sliceHeight = config.height;
      }
    }
    if (!havePixelFormat || pixelFormat <= 0) {
      // 拿不到描述时按 Configure 的约定当 NV12 处理（格式错误的日志由
      // handleOutputBuffer 用 loggedPixelFormat_ 控制只打一次）。
      pixelFormat = AV_PIXEL_FORMAT_NV12;
    }
    if (stride <= 0) stride = picWidth;
    if (sliceHeight <= 0) sliceHeight = picHeight;
    if (picWidth <= 0 || picHeight <= 0 || stride <= 0) {
      logWarnThrottled("sunshine video: decoder output geometry unavailable");
      return false;
    }
    if (stride < picWidth || sliceHeight < picHeight) {
      // stride/sliceHeight 不可能小于可见宽高；不满足说明描述读错了，继续用会读越界。
      logWarnThrottled(formatText(
          "sunshine video: inconsistent output geometry %dx%d stride=%d "
          "sliceHeight=%d",
          picWidth, picHeight, stride, sliceHeight));
      return false;
    }
    frameWidth_ = picWidth;
    frameHeight_ = picHeight;
    frameStride_ = stride;
    frameSliceHeight_ = sliceHeight;
    framePixelFormat_ = pixelFormat;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      geometryDirty_ = false;
    }
    // 宽高变了必须换 sws 上下文（1:1 转换，像素格式相同但尺寸不同）。
    if (sws_ != nullptr &&
        (frameWidth_ != swsWidth_ || frameHeight_ != swsHeight_)) {
      sws_freeContext(sws_);
      sws_ = nullptr;
      swsWidth_ = 0;
      swsHeight_ = 0;
      swsSourceFormat_ = AV_PIX_FMT_NONE;
    }
    return true;
  }

  void handleOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer) {
    if (buffer == nullptr) return;
    ActiveCall call(this);
    if (!call.entered()) return;  // 正在停止：buffer 随 Destroy 一起回收
    OH_AVCodecBufferAttr attr;
    std::memset(&attr, 0, sizeof(attr));
    if (OH_AVBuffer_GetBufferAttr(buffer, &attr) != AV_ERR_OK) {
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      return;
    }
    if ((attr.flags & AVCODEC_BUFFER_FLAGS_EOS) != 0) {
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      logDebug("sunshine video: decoder reported EOS");
      return;
    }
    // 分段耗时 2：按 PTS 配对「送进解码器 -> 输出回调」的间隔。解码器会把输入
    // 的 pts 原样带回；配不上（pts 被改写/为 0）时用最近已配对样本的运行均值，
    // 这样统计不会因为个别帧配不上就断档（见文件末尾的说明）。
    const uint64_t outputUs = monotonicMicros();
    uint64_t matchedReceiveTimeUs = 0;
    {
      uint64_t submitToOutputUs = 0;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::deque<PendingSubmit>::iterator it = pendingSubmits_.begin();
             it != pendingSubmits_.end(); ++it) {
          if (it->ptsUs != attr.pts) continue;
          if (outputUs > it->submitUs) submitToOutputUs = outputUs - it->submitUs;
          matchedReceiveTimeUs = it->receiveTimeUs;
          // 运行均值，配对不上时的兜底就是这个均值。
          matchedSubmitToOutputSumUs_ += submitToOutputUs;
          ++matchedSubmitToOutputCount_;
          lastSubmitToOutputUs_ = static_cast<uint64_t>(
              matchedSubmitToOutputSumUs_ / matchedSubmitToOutputCount_);
          pendingSubmits_.erase(it);
          break;
        }
        if (submitToOutputUs == 0) submitToOutputUs = lastSubmitToOutputUs_;
      }
      if (submitToOutputUs != 0) g_videoPerf.noteSubmitToOutput(submitToOutputUs);
    }
    if (surfaceMode_) {
      // Surface 模式（零拷贝）：解码器已经把帧渲染进 OH_NativeImage 的窗口，这里
      // 不再有 CPU 可读缓冲（没有 NV12/BGRA、没有 stride）。只做三件事：
      //   1) 计解码输出帧数（convert/commit 在这个模式下不存在，保持 0）；
      //   2) 首帧/onStreamChanged 之后从输出描述更新有效画面尺寸并交给宿主；
      //   3) 把 buffer 交还解码器：RenderOutputBuffer 同时触发向 surface 的呈现，
      //      失败时退回 FreeOutputBuffer，避免解码器因为 buffer 不回收而卡住。
      g_videoPerf.noteDecodedFrame();
      updateSurfaceGeometry(codec);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.frames;
        if ((attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0) ++stats_.idrFrames;
      }
      if (OH_VideoDecoder_RenderOutputBuffer(codec, index) != AV_ERR_OK) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
      }
      // 端到端延迟：这一帧紧接着就会被渲染线程取走并呈现，记下它的主机抓屏时刻与
      // 首包接收时刻（渲染线程在 swap 返回时用最近一帧算 captureToSwap）。
      noteFrameTiming(static_cast<long long>(attr.pts),
                      static_cast<long long>(matchedReceiveTimeUs));
      return;
    }
    uint8_t* addr = OH_AVBuffer_GetAddr(buffer);
    const int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    if (addr == nullptr || capacity <= 0) {
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      return;
    }

    // 输出描述只在首帧、onStreamChanged 之后重新读取，避免每帧都做一次 AVCodec
    // 查询；几何未知时也读一次。
    bool needGeometry = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      needGeometry = geometryDirty_ || frameStride_ <= 0;
    }
    if (needGeometry || sws_ == nullptr) {
      if (!refreshOutputGeometry(codec, buffer)) {
        OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
      }
    }

    // 本次只支持 8 比特 NV12/NV21（P010 等十比特格式不在范围内）：格式不符时记
    // 一次错误日志并跳过该帧，避免每帧刷屏。
    const bool nv12 = framePixelFormat_ == AV_PIXEL_FORMAT_NV12;
    const bool nv21 = framePixelFormat_ == AV_PIXEL_FORMAT_NV21;
    if (!nv12 && !nv21) {
      if (!loggedPixelFormat_) {
        loggedPixelFormat_ = true;
        logWarn(formatText(
            "sunshine video: unsupported decoder output pixel format %d (only "
            "NV12/NV21 8-bit supported), dropping frames",
            framePixelFormat_));
      }
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      return;
    }
    const size_t needed = static_cast<size_t>(frameStride_) *
                          static_cast<size_t>(frameSliceHeight_) * 3 / 2;
    if (static_cast<size_t>(capacity) < needed) {
      logWarnThrottled(formatText(
          "sunshine video: output buffer too small (%d < %zu), dropping frame",
          static_cast<int>(capacity), needed));
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      return;
    }
    if (!ensureConverter(nv12 ? AV_PIX_FMT_NV12 : AV_PIX_FMT_NV21)) {
      OH_VideoDecoder_FreeOutputBuffer(codec, index);
      return;
    }
    g_videoPerf.noteDecodedFrame();

    // NV12/NV21：Y 平面从 buffer 起始，UV 交错平面在 Y + sliceHeight*stride。
    const uint8_t* source[3];
    source[0] = addr;
    source[1] = addr + static_cast<size_t>(frameSliceHeight_) *
                           static_cast<size_t>(frameStride_);
    source[2] = nullptr;
    int sourceStride[3] = {frameStride_, frameStride_, 0};
    uint8_t* destination[3] = {bgra_.data(), nullptr, nullptr};
    int destinationStride[3] = {frameWidth_ * 4, 0, 0};
    // 分段耗时 3：NV12/NV21 -> BGRA 的 sws 转换本身。
    const uint64_t convertStartUs = monotonicMicros();
    sws_scale(sws_, source, sourceStride, 0, frameHeight_, destination,
              destinationStride);
    g_videoPerf.noteConvert(monotonicMicros() - convertStartUs);

    // swscale 的 BGRA 输出通常已经把 alpha 写成 255，这里显式补齐（与 RDP 路径
    // commit 前强制 alpha 的做法一致，不依赖 sws 的行为）。
    for (size_t i = 3; i < bgra_.size(); i += 4) bgra_[i] = 255;

    // 画面全黑检测：稀疏采样这一帧，把「黑 / 非黑」连同帧时刻交给后端；连续
    // 全黑时长和 3 秒阈值由状态读取方换算（见 sunshine_backend.h）。采样只做
    // 整数加法、不逐像素遍历：1080p 每帧约 68*120 = 8160 个点。
    sunshineNoteVideoLuma(monotonicMillis(), frameIsBlack());

    // 帧提交：只有本回调线程会走到这里，所以同一时刻只有一个提交者。sink 与
    // generation 在锁内取快照，宿主自己还会核对 generation。
    SunshineFrameSink sink;
    int generation = 0;
    unsigned long long frames = 0;
    unsigned long long idrFrames = 0;
    unsigned long long decodeErrors = 0;
    unsigned long long dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sink = frameSink_;
      generation = generation_;
      ++stats_.frames;
      if ((attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) != 0) ++stats_.idrFrames;
      frames = stats_.frames;
      idrFrames = stats_.idrFrames;
      decodeErrors = decodeErrors_;
      dropped = droppedUnits_;
    }
    if (sink.resize != nullptr &&
        (frameWidth_ != committedWidth_ || frameHeight_ != committedHeight_)) {
      sink.resize(sink.user, generation, frameWidth_, frameHeight_);
      committedWidth_ = frameWidth_;
      committedHeight_ = frameHeight_;
      if (loggedFirstFrame_)
        logWarn(formatText("sunshine video: decoded size changed -> %dx%d",
                           frameWidth_, frameHeight_));
    }
    if (sink.commit != nullptr) {
      SunshineRect rect;
      rect.x = 0;
      rect.y = 0;
      rect.w = frameWidth_;
      rect.h = frameHeight_;
      rect.stride = frameWidth_ * 4;
      rect.pixels = bgra_.data();
      // 分段耗时 4：宿主 commit（含镜像拷贝，宿主在 pix 锁内做完才返回）。
      const uint64_t commitStartUs = monotonicMicros();
      sink.commit(sink.user, generation, &rect, 1, frameWidth_, frameHeight_);
      g_videoPerf.noteCommit(monotonicMicros() - commitStartUs);
    }
    // 端到端延迟（Buffer 回退路径同样可用）：这一帧刚提交进宿主镜像，紧接着会被
    // 渲染线程取走并呈现。
    noteFrameTiming(static_cast<long long>(attr.pts),
                    static_cast<long long>(matchedReceiveTimeUs));
    OH_VideoDecoder_FreeOutputBuffer(codec, index);

    if (!loggedFirstFrame_) {
      loggedFirstFrame_ = true;
      logWarn(formatText(
          "sunshine video: first frame %dx%d (stride=%d sliceHeight=%d, %s)",
          frameWidth_, frameHeight_, frameStride_, frameSliceHeight_,
          nv12 ? "NV12" : "NV21"));
    }
    // 进度日志每秒最多一条，不刷屏。
    const uint64_t now = monotonicMillis();
    if (now - lastProgressMs_ >= 1000) {
      lastProgressMs_ = now;
      logDebug(formatText(
          "sunshine video: frames=%llu (idr %llu), %dx%d, errors=%llu, dropped=%llu",
          frames, idrFrames, frameWidth_, frameHeight_, decodeErrors, dropped));
    }
  }

  // 稀疏亮度采样（只读 bgra_，只在输出回调线程里调用）：每 16 行取一行、每
  // 16 像素取一个点，取 R/G/B 平均并忽略 alpha。平均亮度 < 4 判为黑帧；整数
  // 比较 sum < 4*3*count 等价且免去除法。4K 每帧也只有约 2.7 万个采样点。
  bool frameIsBlack() const {
    const int width = frameWidth_;
    const int height = frameHeight_;
    if (width <= 0 || height <= 0) return true;
    const size_t stride = static_cast<size_t>(width) * 4;
    unsigned long long sum = 0;
    size_t count = 0;
    for (int y = 0; y < height; y += 16) {
      const uint8_t* row = bgra_.data() + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; x += 16) {
        const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
        sum += static_cast<unsigned long long>(pixel[0]) + pixel[1] + pixel[2];
        ++count;
      }
    }
    return count == 0 || sum < 12ULL * count;
  }

  // 建/复用 sws 上下文与 BGRA 输出缓冲（只在输出回调线程里调用）。
  bool ensureConverter(AVPixelFormat sourceFormat) {
    if (sws_ != nullptr && swsWidth_ == frameWidth_ && swsHeight_ == frameHeight_ &&
        swsSourceFormat_ == sourceFormat) {
      return true;
    }
    if (sws_ != nullptr) {
      sws_freeContext(sws_);
      sws_ = nullptr;
    }
    sws_ = sws_getContext(frameWidth_, frameHeight_, sourceFormat, frameWidth_,
                          frameHeight_, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                          nullptr, nullptr);
    if (sws_ == nullptr) {
      logWarnThrottled(formatText("sunshine video: sws_getContext(%dx%d) failed",
                                  frameWidth_, frameHeight_));
      return false;
    }
    swsWidth_ = frameWidth_;
    swsHeight_ = frameHeight_;
    swsSourceFormat_ = sourceFormat;
    bgra_.assign(static_cast<size_t>(frameWidth_) * frameHeight_ * 4, 0);
    return true;
  }

  void logWarnThrottled(const std::string& line) {
    const uint64_t now = monotonicMillis();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (lastWarnMs_ != 0 && now - lastWarnMs_ < 1000) return;
      lastWarnMs_ = now;
    }
    logWarn(line);
  }

  // ---- 状态 ---------------------------------------------------------------
  mutable std::mutex mutex_;          // 状态/队列/统计（短临界区，内部不调 AVCodec）
  std::condition_variable idleCond_;  // stop() 等在途调用归零
  std::mutex codecApiMutex_;          // 串行化 PushInputBuffer
  OH_AVCodec* codec_;                 // 受 mutex_ 保护
  bool running_;                      // 受 mutex_ 保护（Start 之前就发布）
  bool started_;                      // 受 mutex_ 保护（Start 已成功）
  // 本次 setup 走的是零拷贝 Surface 路径（受 mutex_ 保护）。externalSourceHeld_ 表示
  // 宿主的外部纹理源还在我们手里，必须在 Stop/Destroy 之后再归还。
  bool surfaceMode_;
  bool externalSourceHeld_;
  int surfaceWidth_;   // 受 mutex_ 保护：Surface 模式的有效画面尺寸
  int surfaceHeight_;
  int activeCalls_;                   // 受 mutex_ 保护
  SunshineFrameSink frameSink_;       // 受 mutex_ 保护
  int generation_;                    // 受 mutex_ 保护
  SunshineVideoStats stats_;          // 受 mutex_ 保护
  std::deque<QueuedDecodeUnit> queue_;      // 受 mutex_ 保护
  std::deque<FreeInputBuffer> freeInputs_;  // 受 mutex_ 保护
  int configuredFps_;  // 受 mutex_ 保护（submit 里读）
  int64_t lastPtsUs_;  // 受 mutex_ 保护（PTS 递推）
  // 分段耗时 2 的配对表：push 时记 {pts, 时刻}，输出回调按 pts 取回（受 mutex_）。
  std::deque<PendingSubmit> pendingSubmits_;
  uint64_t matchedSubmitToOutputSumUs_;  // 受 mutex_ 保护（已配对样本的运行和）
  uint64_t matchedSubmitToOutputCount_;  // 受 mutex_ 保护
  uint64_t lastSubmitToOutputUs_;        // 受 mutex_ 保护（运行均值，配对失败兜底）

  // 解码输出与转换状态：只在 AVCodec 输出回调线程里读写；sws_ / bgra_ 的释放在
  // stop() 里，且发生在 activeCalls_ 归零之后。
  SwsContext* sws_;
  int swsWidth_;
  int swsHeight_;
  AVPixelFormat swsSourceFormat_;
  std::vector<uint8_t> bgra_;
  int frameWidth_;
  int frameHeight_;
  int frameStride_;
  int frameSliceHeight_;
  int framePixelFormat_;  // OH_AVPixelFormat
  bool geometryDirty_;    // 受 mutex_ 保护（onStreamChanged 会置位）
  int committedWidth_;    // 已提交给宿主的尺寸（输出回调线程私有）
  int committedHeight_;
  bool loggedPixelFormat_;
  bool loggedFirstFrame_;
  unsigned long long droppedUnits_;  // 受 mutex_ 保护（计数用）
  unsigned long long decodeErrors_;  // 受 mutex_ 保护
  uint64_t lastWarnMs_;              // 受 mutex_ 保护
  uint64_t lastProgressMs_;          // 输出回调线程私有
};

}  // namespace

SunshineVideoSink* SunshineVideoSink::create() {
  return new AvCodecVideoSink();
}

// 进程级分段耗时快照（见 sunshine_video.h）。读取方在任意线程调用；聚合器内部
// 只用自己的一把锁，且从不在持有它的时候再拿别的锁，所以不会和上面的解码锁序
// 交叉。
SunshineVideoPerf sunshineVideoPerf() {
  return g_videoPerf.snapshot();
}

SunshineFrameTiming sunshineLatestFrameTiming() {
  std::lock_guard<std::mutex> lock(g_frameTimingMutex);
  return g_latestFrameTiming;
}

}  // namespace tianguan

// 遗留/后续工作（不在本次范围）：
//   * Surface 模式直出纹理：Buffer 模式要把整帧从 CPU 读出来再 sws 转 RGBA 再
//     拷进镜像，1080p 每帧约 4~8MB 内存流量；后续可让解码器输出到 OHNativeWindow，
//     用 EGL 的 OHNativeWindow 纹理直接上屏，省掉这两次拷贝。
//   * AV1：moonlight-common-c 给 AV1 的单元没有 BUFFER_TYPE_* 分类、也不保证
//     AnnexB 起始码（OBU 直通），需要单独的 OBU 解析/封装路径，本次未接。
//   * P010/HDR：十比特输出格式被显式跳过（只记一次日志）。
