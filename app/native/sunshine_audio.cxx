// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 音频渲染器：libopus 多流解码 + OH_AudioRenderer 播放。
//
// 数据通路与 rdp_backend.cxx 的 rdpsnd 路径一致：
//   common-c 音频线程：decodeAndPlaySample -> opus_multistream_decode -> 交织
//                      S16 PCM -> 环形缓冲（满时丢最旧，保持实时）；
//   OHOS 音频线程：拉模式写回调从环形缓冲取 PCM 填 callbackBuffer，不足部分
//                  补静音，返回 AUDIO_DATA_CALLBACK_RESULT_VALID；
//   会话线程：init 建解码器与渲染器，stop 停渲染器、销毁解码器、清空缓冲。
// 音频是旁路：解码失败只计数并按 5 秒限频打日志，绝不让音频线程崩溃或影响串流。
//
// 锁与所有权：
//   lifecycle_    串行化 init/stop 的渲染器创建与销毁（会话线程）；
//   decoder_lock_ 保护解码器与 pcm_ 暂存区：decode 与 stop 的销毁互斥；
//   lock_         保护环形缓冲与 renderer_/started_：写回调与 push/stop 互斥；
//   stats_mutex_  保护只读计数（stats() 可能被后端的输入线程调用）。
// 锁序固定为 decoder_lock_ -> lock_，stop 不会持 lock_ 调 Stop()/Release()。
#include "sunshine_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// libopus 的多流解码入口：Moonlight 的音频流是 Opus multistream
// （每个立体声流单独编码后按 mapping 组装），单流 opus_decode 解不了。
#include <opus/opus_multistream.h>

// OHOS AudioRenderer NDK（libohaudio.so，平台系统库，由 CMakeLists 链接）。
#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostreambuilder.h>
#include <ohaudio/native_audiostream_base.h>

#if defined(__MUSL__)
#include <hilog/log.h>
#endif

namespace tianguan {
namespace {

// 环形缓冲容量 = 约 0.5 秒 PCM，钳制在 32..256 KiB（与 RDP 音频路径相同的
// 上下限），并按 4 字节对齐（S16 立体声的一帧）。
const size_t kRingMinBytes = 32 * 1024;
const size_t kRingMaxBytes = 256 * 1024;
// 预缓冲目标 = 约 80 ms PCM（60..100 ms 是常见取值）。Opus 包按 5 ms 到达，
// 网络抖动会成串迟到；先攒够一小段再让渲染器出声，避免每个回调都在「半截
// 数据 + 静音」之间切换（那是周期性咔哒声的来源）。播放中一旦取不满一个
// 回调就退回预缓冲，而不是每回调补一次静音。
const size_t kPrebufferMs = 80;
// 运行期统计行：每 5 秒最多一条，每条连接最多 10 条（stop 时再补最后一条）。
const uint64_t kStatsIntervalMs = 5000;
const int kStatsMaxLines = 10;
// 解码失败日志的限频窗口：坏流不能刷满 hilog。
const uint64_t kDecodeLogIntervalMs = 5000;

// 与 sunshine_backend.cxx、rdp_backend.cxx 相同的日志策略：一次性/错误信息走
// WARN，周期统计走 DEBUG。日志不含任何媒体内容。
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

class OpusAudioSink;

// OH_AudioRenderer 的拉模式写回调。userData 是 OpusAudioSink 本身；对象由后端
// 持有到 LiStopConnection 之后，stop() 里 Release() 结束回调后才允许释放。
OH_AudioData_Callback_Result audioWriteData(OH_AudioRenderer* renderer,
                                            void* userData, void* audioData,
                                            int32_t audioDataSize);

// 一次连接内的音频渲染器：一个 Opus 多流解码器 + 一个 S16LE 环形缓冲 + 一个
// OH_AudioRenderer。参数在 init 时固定，整个会话不再变化（Moonlight 不会在
// 中途换 Opus 布局）。
class OpusAudioSink : public SunshineAudioSink {
 public:
  OpusAudioSink()
      : decoder_(nullptr),
        decodeRate_(0),
        decodeChannels_(0),
        decodeSamplesPerFrame_(0),
        readPos_(0),
        writePos_(0),
        used_(0),
        capacity_(0),
        rate_(0),
        channels_(0),
        samplesPerFrame_(0),
        renderer_(nullptr),
        started_(false),
        primed_(false),
        prebufferBytes_(0),
        playbackLogged_(false),
        bytesPlayed_(0),
        underruns_(0),
        dropped_(0),
        lastStatsMs_(0),
        lastStatsBytes_(0),
        statsLines_(0),
        decodeFailures_(0),
        discarded_(0),
        lastDecodeLogMs_(0) {}

  ~OpusAudioSink() override {
    // 后端通常已经在 stop 回调里停过；这里兜底 init 失败/未走 stop 的路径。
    stop();
  }

  int init(int audioConfiguration,
           const POPUS_MULTISTREAM_CONFIGURATION opusConfig) override;
  void decodeAndPlaySample(char* sampleData, int sampleLength) override;
  void stop() override;
  SunshineAudioStats stats() const override;

  // 写回调的实现：从环形缓冲取 size 字节填 out，不足补静音，返回实际 PCM 字节数。
  size_t fill(uint8_t* out, size_t size);

 private:
  // 建 OH_AudioRenderer 并 Start。返回 false 时音频不可用（init 随之失败）。
  bool startRenderer();
  // 停渲染器、销毁解码器、清空缓冲。调用方必须持有 lifecycle_。
  void stopLocked();
  // 把交织 S16 PCM 追加进环形缓冲，满时丢最旧。调用方持 decoder_lock_。
  void pushPcm(const uint8_t* data, size_t size);
  // 周期统计日志（DEBUG），force 时绕过节流并在无播放时省略。
  void logStats(bool force);

  // 串行化 init/stop 的渲染器创建与销毁（会话线程）。
  std::mutex lifecycle_;
  // 保护解码器与 pcm_ 暂存区：decodeAndPlaySample 与 stop 的销毁互斥。
  std::mutex decoder_lock_;
  OpusMSDecoder* decoder_;
  std::vector<int16_t> pcm_;  // samplesPerFrame * channels 个交织样本
  // 解码侧参数快照（受 decoder_lock_ 保护）：解码线程不能直接读环缓冲的
  // rate_/channels_/samplesPerFrame_，否则与 init 的写入构成数据竞争。
  int decodeRate_;
  int decodeChannels_;
  int decodeSamplesPerFrame_;
  // 保护环形缓冲与 renderer_/started_：写回调与 push/stop 互斥。
  std::mutex lock_;
  std::vector<uint8_t> ring_;
  size_t readPos_;
  size_t writePos_;
  size_t used_;
  size_t capacity_;
  int rate_;
  int channels_;
  int samplesPerFrame_;
  OH_AudioRenderer* renderer_;
  bool started_;
  // 抗抖动状态（受 lock_ 保护）：primed_ 为 false 表示还在预缓冲/刚欠载，
  // 写回调只吐静音；积够 prebufferBytes_ 才转到播放态。
  bool primed_;
  size_t prebufferBytes_;
  bool playbackLogged_;  // 首次进入播放态的一行日志已经打过
  std::atomic<uint64_t> bytesPlayed_;
  std::atomic<uint64_t> underruns_;
  std::atomic<uint64_t> dropped_;
  uint64_t lastStatsMs_;
  uint64_t lastStatsBytes_;
  int statsLines_;
  std::atomic<uint64_t> decodeFailures_;
  std::atomic<uint64_t> discarded_;  // 没有渲染器/解码器时丢掉的 payload 字节
  uint64_t lastDecodeLogMs_;         // 由 decoder_lock_ 保护
  // 保护只读计数（stats() 由后端的输入线程调用，与解码线程并发）。
  mutable std::mutex stats_mutex_;
  SunshineAudioStats stats_;
};

OH_AudioData_Callback_Result audioWriteData(OH_AudioRenderer* renderer,
                                            void* userData, void* audioData,
                                            int32_t audioDataSize) {
  (void)renderer;
  if (userData == nullptr || audioData == nullptr || audioDataSize <= 0)
    return AUDIO_DATA_CALLBACK_RESULT_INVALID;
  static_cast<OpusAudioSink*>(userData)->fill(
      static_cast<uint8_t*>(audioData), static_cast<size_t>(audioDataSize));
  // 静音也是有效数据：绝不返回 INVALID，否则框架会跳过缓冲区并自己报错。
  return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

int OpusAudioSink::init(int audioConfiguration,
                        const POPUS_MULTISTREAM_CONFIGURATION opusConfig) {
  std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
  // 重连/重入时先清掉上一次的资源（stop 幂等）。
  stopLocked();
  if (opusConfig == nullptr) {
    logWarn("audio init: opusConfig is null");
    return -1;
  }
  const int sampleRate = opusConfig->sampleRate;
  const int channelCount = opusConfig->channelCount;
  const int streams = opusConfig->streams;
  const int coupledStreams = opusConfig->coupledStreams;
  const int samplesPerFrame = opusConfig->samplesPerFrame;
  // 解码参数自检：Opus 只接受 8/12/16/24/48kHz，声道数上限是 Limelight 的 8；
  // samplesPerFrame 是每个包每声道的样本数（正常 240 = 5ms@48kHz，可能因
  // CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION 而不同），必须 > 0。
  if (sampleRate <= 0 || channelCount <= 0 ||
      channelCount > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT || streams <= 0 ||
      coupledStreams < 0 || coupledStreams > streams || samplesPerFrame <= 0) {
    logWarn(formatText(
        "audio init: invalid opus config %dHz %dch, streams=%d coupled=%d spf=%d",
        sampleRate, channelCount, streams, coupledStreams, samplesPerFrame));
    return -1;
  }
  int opusError = OPUS_OK;
  OpusMSDecoder* decoder = opus_multistream_decoder_create(
      sampleRate, channelCount, streams, coupledStreams, opusConfig->mapping,
      &opusError);
  if (decoder == nullptr || opusError != OPUS_OK) {
    // 失败只影响声音：打印完整参数，便于判断是布局还是采样率不被支持。
    logWarn(formatText(
        "audio init: opus_multistream_decoder_create failed [%d], %dHz %dch, "
        "streams=%d coupled=%d, mapping=%u,%u,%u,%u,%u,%u,%u,%u",
        opusError, sampleRate, channelCount, streams, coupledStreams,
        static_cast<unsigned>(opusConfig->mapping[0]),
        static_cast<unsigned>(opusConfig->mapping[1]),
        static_cast<unsigned>(opusConfig->mapping[2]),
        static_cast<unsigned>(opusConfig->mapping[3]),
        static_cast<unsigned>(opusConfig->mapping[4]),
        static_cast<unsigned>(opusConfig->mapping[5]),
        static_cast<unsigned>(opusConfig->mapping[6]),
        static_cast<unsigned>(opusConfig->mapping[7])));
    if (decoder != nullptr) opus_multistream_decoder_destroy(decoder);
    return -1;
  }
  std::vector<int16_t> pcm(
      static_cast<size_t>(samplesPerFrame) * static_cast<size_t>(channelCount),
      0);
  {
    std::lock_guard<std::mutex> guard(decoder_lock_);
    decoder_ = decoder;
    decodeRate_ = sampleRate;
    decodeChannels_ = channelCount;
    decodeSamplesPerFrame_ = samplesPerFrame;
    pcm_.swap(pcm);
  }
  // 0.5 秒的 PCM：sampleRate * channels * 2 字节 / 2，钳制后按帧对齐。
  size_t ringBytes = static_cast<size_t>(sampleRate) *
                     static_cast<size_t>(channelCount) * 2u / 2u;
  ringBytes = std::max(kRingMinBytes, std::min(kRingMaxBytes, ringBytes));
  ringBytes &= ~static_cast<size_t>(3);
  {
    std::lock_guard<std::mutex> guard(lock_);
    rate_ = sampleRate;
    channels_ = channelCount;
    samplesPerFrame_ = samplesPerFrame;
    capacity_ = ringBytes;
    ring_.assign(capacity_, 0);
    readPos_ = 0;
    writePos_ = 0;
    used_ = 0;
    // 预缓冲目标按同一采样布局换算成字节，按 4 字节（S16 立体声一帧）对齐，
    // 且不少于一个 Opus 包，避免单包就大于目标导致永远凑不满。
    const size_t bytesPerSecond =
        static_cast<size_t>(sampleRate) * static_cast<size_t>(channelCount) * 2u;
    size_t prebufferBytes = bytesPerSecond * kPrebufferMs / 1000u;
    const size_t packetBytes =
        static_cast<size_t>(samplesPerFrame) * static_cast<size_t>(channelCount) *
        2u;
    prebufferBytes = std::max(prebufferBytes, packetBytes);
    prebufferBytes = (prebufferBytes + 3u) & ~static_cast<size_t>(3);
    prebufferBytes_ = prebufferBytes;
    primed_ = false;
    playbackLogged_ = false;
    lastStatsMs_ = monotonicMillis();
    lastStatsBytes_ = bytesPlayed_.load();
    statsLines_ = 0;
  }
  {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    stats_.sampleRate = sampleRate;
    stats_.channels = channelCount;
    stats_.samplesPerFrame = samplesPerFrame;
  }
  // 一次性参数行：采样率/声道/流数/samplesPerFrame 是排查音频问题的第一步。
  logWarn(formatText(
      "audio init: config=0x%X %dHz %dch S16LE, streams=%d coupled=%d "
      "samplesPerFrame=%d, ring=%zu bytes",
      audioConfiguration, sampleRate, channelCount, streams, coupledStreams,
      samplesPerFrame, ringBytes));
  if (!startRenderer()) {
    // 按约定返回非 0 让 common-c 放弃启动音频；已建的解码器/缓冲一并回收。
    stopLocked();
    return -1;
  }
  return 0;
}

void OpusAudioSink::decodeAndPlaySample(char* sampleData, int sampleLength) {
  if (sampleData == nullptr || sampleLength <= 0) return;
  {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    ++stats_.packets;
    stats_.bytes += static_cast<unsigned long long>(sampleLength);
  }
  std::lock_guard<std::mutex> decoderGuard(decoder_lock_);
  if (decoder_ == nullptr || pcm_.empty()) {
    // init 失败或已 stop：晚到的包只计数，不解码。
    discarded_.fetch_add(static_cast<uint64_t>(sampleLength));
    return;
  }
  const int decoded = opus_multistream_decode(
      decoder_, reinterpret_cast<const unsigned char*>(sampleData), sampleLength,
      pcm_.data(), decodeSamplesPerFrame_, 0);
  if (decoded < 0) {
    // 单个坏包只损失这点声音：计数并限频打日志，绝不让音频线程崩溃。
    const uint64_t failures = decodeFailures_.fetch_add(1) + 1;
    const uint64_t now = monotonicMillis();
    if (now >= lastDecodeLogMs_ + kDecodeLogIntervalMs) {
      lastDecodeLogMs_ = now;
      logWarn(formatText(
          "audio: opus decode failed [%d], failures=%llu, %d payload bytes, "
          "%dHz %dch",
          decoded, static_cast<unsigned long long>(failures), sampleLength,
          decodeRate_, decodeChannels_));
    }
    return;
  }
  // 解码器在 priming 时可能解出 0 帧，这不是失败，也没有可入队的 PCM。
  if (decoded == 0) return;
  pushPcm(reinterpret_cast<const uint8_t*>(pcm_.data()),
          static_cast<size_t>(decoded) * static_cast<size_t>(decodeChannels_) *
              2u);
}

void OpusAudioSink::pushPcm(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return;
  std::lock_guard<std::mutex> guard(lock_);
  if (!started_ || capacity_ == 0) {
    // 没有渲染器（stop 之后 / init 失败）时如实计入丢弃，不假装播放。
    discarded_.fetch_add(size);
    return;
  }
  if (size > capacity_) {
    // 单块比整个环还大：只留最新的 PCM。
    const size_t excess = size - capacity_;
    dropped_.fetch_add(excess);
    data += excess;
    size = capacity_;
  }
  const size_t freeBytes = capacity_ - used_;
  if (size > freeBytes) {
    // 丢掉最旧的音频，让播放点紧贴实时而不是越积越晚。
    const size_t dropFront = size - freeBytes;
    dropped_.fetch_add(dropFront);
    readPos_ = (readPos_ + dropFront) % capacity_;
    used_ -= dropFront;
  }
  const size_t first = std::min(size, capacity_ - writePos_);
  std::memcpy(&ring_[writePos_], data, first);
  if (size > first) std::memcpy(&ring_[0], data + first, size - first);
  writePos_ = (writePos_ + size) % capacity_;
  used_ += size;
}

size_t OpusAudioSink::fill(uint8_t* out, size_t size) {
  if (out == nullptr || size == 0) return 0;
  bool starved = false;         // 这次回调从播放态掉回预缓冲（计一次欠载）
  bool playbackStarted = false;  // 本次进入播放态，打一行一次性的日志
  size_t real = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (started_) {
      // 预缓冲：积够目标（或至少一个回调的量）才出声，之前的回调只吐静音，
      // 这段静音不算欠载。
      if (!primed_ && used_ >= std::max(prebufferBytes_, size)) {
        primed_ = true;
        if (!playbackLogged_) {
          playbackLogged_ = true;
          playbackStarted = true;
        }
      }
      if (primed_) {
        if (used_ >= size) {
          real = size;
          const size_t first = std::min(real, capacity_ - readPos_);
          std::memcpy(out, &ring_[readPos_], first);
          if (real > first) std::memcpy(out + first, &ring_[0], real - first);
          readPos_ = (readPos_ + real) % capacity_;
          used_ -= real;
          bytesPlayed_.fetch_add(real);
        } else {
          // 播放中取不满一个回调：整块补静音并退回预缓冲，环里剩下的数据
          // 留到复播，不再每个回调「半截数据 + 静音」逐次切换。
          primed_ = false;
          starved = true;
        }
      }
    }
    if (real < size) std::memset(out + real, 0, size - real);
  }
  if (starved) underruns_.fetch_add(1);
  if (playbackStarted) {
    logWarn(formatText(
        "audio: prebuffer full (%zu ms target), playback starts", kPrebufferMs));
  }
  logStats(false);
  return real;
}

bool OpusAudioSink::startRenderer() {
  int sampleRate = 0;
  int channelCount = 0;
  {
    std::lock_guard<std::mutex> guard(lock_);
    sampleRate = rate_;
    channelCount = channels_;
  }
  OH_AudioStreamBuilder* builder = nullptr;
  OH_AudioStream_Result result =
      OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER);
  if (result != AUDIOSTREAM_SUCCESS || builder == nullptr) {
    logWarn(formatText("audio: OH_AudioStreamBuilder_Create failed [%d]",
                       static_cast<int>(result)));
    return false;
  }
  // 与 rdp_backend.cxx 完全相同的构建顺序与档位：S16LE / RAW / 普通延迟 /
  // MUSIC，拉模式写回调拥有 OH 音频线程。每一步都检查并记录；声道数被 NDK
  // 拒绝（例如 5.1/7.1 在该设备上不受支持）时明确失败，而不是静默变声。
  struct Step {
    const char* name;
    OH_AudioStream_Result result;
  };
  const Step steps[] = {
      {"SetSamplingRate",
       OH_AudioStreamBuilder_SetSamplingRate(builder, sampleRate)},
      {"SetChannelCount",
       OH_AudioStreamBuilder_SetChannelCount(builder, channelCount)},
      {"SetSampleFormat",
       OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE)},
      {"SetEncodingType", OH_AudioStreamBuilder_SetEncodingType(
                              builder, AUDIOSTREAM_ENCODING_TYPE_RAW)},
      {"SetLatencyMode", OH_AudioStreamBuilder_SetLatencyMode(
                             builder, AUDIOSTREAM_LATENCY_MODE_NORMAL)},
      {"SetRendererInfo",
       OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_MUSIC)},
      {"SetRendererWriteDataCallback",
       OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder,
                                                         audioWriteData, this)},
  };
  for (const Step& step : steps) {
    if (step.result == AUDIOSTREAM_SUCCESS) continue;
    logWarn(formatText(
        "audio: OH_AudioStreamBuilder_%s failed [%d], %dHz %dch", step.name,
        static_cast<int>(step.result), sampleRate, channelCount));
    (void)OH_AudioStreamBuilder_Destroy(builder);
    return false;
  }
  OH_AudioRenderer* created = nullptr;
  result = OH_AudioStreamBuilder_GenerateRenderer(builder, &created);
  (void)OH_AudioStreamBuilder_Destroy(builder);
  if (result != AUDIOSTREAM_SUCCESS || created == nullptr) {
    logWarn(formatText("audio: OH_AudioStreamBuilder_GenerateRenderer failed [%d]",
                       static_cast<int>(result)));
    if (created != nullptr) (void)OH_AudioRenderer_Release(created);
    return false;
  }
  result = OH_AudioRenderer_Start(created);
  if (result != AUDIOSTREAM_SUCCESS) {
    logWarn(formatText("audio: OH_AudioRenderer_Start failed [%d]",
                       static_cast<int>(result)));
    (void)OH_AudioRenderer_Release(created);
    return false;
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    renderer_ = created;
    started_ = true;
    lastStatsMs_ = monotonicMillis();
    lastStatsBytes_ = bytesPlayed_.load();
  }
  logWarn(formatText("audio: renderer started %dHz %dch S16LE", sampleRate,
                     channelCount));
  return true;
}

void OpusAudioSink::stop() {
  std::lock_guard<std::mutex> lifecycleLock(lifecycle_);
  stopLocked();
}

void OpusAudioSink::stopLocked() {
  OH_AudioRenderer* previous = nullptr;
  {
    std::lock_guard<std::mutex> guard(lock_);
    // 先摘掉 renderer 再在锁外停：写回调只拿 lock_，持锁调 Stop() 会死锁
    // （Stop 可能等待正在进行的回调返回）。
    started_ = false;
    previous = renderer_;
    renderer_ = nullptr;
  }
  if (previous != nullptr) {
    OH_AudioStream_Result result = OH_AudioRenderer_Stop(previous);
    if (result != AUDIOSTREAM_SUCCESS) {
      logWarn(formatText("audio: OH_AudioRenderer_Stop failed [%d]",
                         static_cast<int>(result)));
    }
    result = OH_AudioRenderer_Release(previous);
    if (result != AUDIOSTREAM_SUCCESS) {
      logWarn(formatText("audio: OH_AudioRenderer_Release failed [%d]",
                         static_cast<int>(result)));
    }
    // Release() 结束写回调；取一次 lock_ 等仍在途的回调退出，之后它不会再
    // 碰这块内存。
    {
      std::lock_guard<std::mutex> guard(lock_);
    }
  }
  {
    std::lock_guard<std::mutex> decoderGuard(decoder_lock_);
    if (decoder_ != nullptr) {
      opus_multistream_decoder_destroy(decoder_);
      decoder_ = nullptr;
    }
    decodeRate_ = 0;
    decodeChannels_ = 0;
    decodeSamplesPerFrame_ = 0;
    pcm_.clear();
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    ring_.clear();
    capacity_ = 0;
    readPos_ = 0;
    writePos_ = 0;
    used_ = 0;
    primed_ = false;
    prebufferBytes_ = 0;
  }
  logStats(true);
  {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    stats_ = SunshineAudioStats();
  }
}

void OpusAudioSink::logStats(bool force) {
  const uint64_t now = monotonicMillis();
  char line[280];
  {
    std::lock_guard<std::mutex> guard(lock_);
    if (!force && statsLines_ >= kStatsMaxLines) return;
    if (!force && now < lastStatsMs_ + kStatsIntervalMs) return;
    const uint64_t played = bytesPlayed_.load();
    // 渲染器一个字节都没拉过：还没有值得打印的统计。
    if (force && played == 0) return;
    const uint64_t delta = played - lastStatsBytes_;
    const uint64_t elapsed = now > lastStatsMs_ ? now - lastStatsMs_ : 0;
    const uint64_t ratePerSecond = elapsed > 0 ? delta * 1000u / elapsed : 0;
    const uint64_t under = underruns_.load();
    const uint64_t lost = dropped_.load();
    const uint64_t failures = decodeFailures_.load();
    const uint64_t lostDiscarded = discarded_.load();
    const unsigned sampleRate = static_cast<unsigned>(rate_);
    const unsigned channelCount = static_cast<unsigned>(channels_);
    // 缓冲水位与预缓冲目标都换算成毫秒：真机日志里一眼能看出是在预缓冲、
    // 水位是否贴近目标，还是在播放中偶发欠载。
    const size_t bytesPerSecond =
        static_cast<size_t>(sampleRate) * static_cast<size_t>(channelCount) * 2u;
    const uint64_t bufferedMs =
        bytesPerSecond > 0
            ? static_cast<uint64_t>(used_) * 1000u / bytesPerSecond
            : 0;
    const uint64_t prebufferMs =
        bytesPerSecond > 0
            ? static_cast<uint64_t>(prebufferBytes_) * 1000u / bytesPerSecond
            : 0;
    const char* bufferingState = primed_ ? "playing" : "prebuffering";
    lastStatsMs_ = now;
    lastStatsBytes_ = played;
    ++statsLines_;
    std::snprintf(
        line, sizeof(line),
        "audio: %llu bytes played (rate=%llu B/s), underrun=%llu, dropped=%llu, "
        "%uHz %uch, buffer=%llums (prebuf=%llums, %s), decodefail=%llu, "
        "discarded=%llu",
        static_cast<unsigned long long>(played),
        static_cast<unsigned long long>(ratePerSecond),
        static_cast<unsigned long long>(under),
        static_cast<unsigned long long>(lost), sampleRate, channelCount,
        static_cast<unsigned long long>(bufferedMs),
        static_cast<unsigned long long>(prebufferMs), bufferingState,
        static_cast<unsigned long long>(failures),
        static_cast<unsigned long long>(lostDiscarded));
  }
  logDebug(line);
}

SunshineAudioStats OpusAudioSink::stats() const {
  std::lock_guard<std::mutex> guard(stats_mutex_);
  return stats_;
}

}  // namespace

SunshineAudioSink* SunshineAudioSink::create() {
  return new OpusAudioSink();
}

}  // namespace tianguan
