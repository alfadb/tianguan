// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 音频渲染接口。
//
// 实现用 libopus 多流解码器把 Moonlight 的 Opus 包解成交织 S16 PCM，再经
// OH_AudioRenderer（拉模式写回调 + 环形缓冲）输出，结构照 rdp_backend.cxx 的
// rdpsnd 音频路径：
//   init():  opus_multistream_decoder_create(sampleRate, channelCount, streams,
//            coupledStreams, mapping)（参数就是下面的 POPUS_MULTISTREAM_CONFIGURATION）；
//            再按 rdp_backend.cxx 的 rdpsnd 播放路径建 OH_AudioStreamBuilder +
//            OH_AudioRenderer（S16LE / RAW / 普通延迟 / MUSIC），拉模式回调从
//            环形缓冲取 PCM；回调先预缓冲约 80 ms 再出声，欠载时退回预缓冲
//            并补静音，避免抖动造成周期性咔哒声；
//   decodeAndPlaySample(): opus_multistream_decode() 解出交织 S16 后写环形缓冲，
//            满时丢最旧的 PCM，让播放跟随实时；
//   stop(): 停止并释放 OH_AudioRenderer、销毁解码器、清空缓冲，但不释放本对象。
// 采样率/声道数以 init 协商值为准（可能是 48kHz 立体声，也可能随服务端变化；
// 5.1/7.1 是否可播放取决于 OH_AudioRenderer 是否接受该声道数，失败会让 init
// 返回非 0 而不是静默变声）。
#pragma once

#include <Limelight.h>

namespace tianguan {

// 只读计数，供后端定期打日志。
struct SunshineAudioStats {
  SunshineAudioStats()
      : packets(0), bytes(0), sampleRate(0), channels(0), samplesPerFrame(0) {}
  unsigned long long packets;  // receive 到的 Opus 包数（不是解码后帧数）
  unsigned long long bytes;
  int sampleRate;
  int channels;
  int samplesPerFrame;
};

// 音频渲染器。create() 永远返回可用实例，由后端在每次连接尝试里创建一次，并在
// LiStopConnection() 返回后延时释放。
class SunshineAudioSink {
 public:
  static SunshineAudioSink* create();

  virtual ~SunshineAudioSink() {}

  // AUDIO_RENDERER_CALLBACKS::init。返回 0 表示成功，非 0 会让 common-c 放弃启动。
  virtual int init(int audioConfiguration,
                   const POPUS_MULTISTREAM_CONFIGURATION opusConfig) = 0;
  // AUDIO_RENDERER_CALLBACKS::decodeAndPlaySample。sampleLength 是字节数。
  virtual void decodeAndPlaySample(char* sampleData, int sampleLength) = 0;
  // AUDIO_RENDERER_CALLBACKS::stop：音频线程已经停下，停止播放并清计数，不释放对象。
  virtual void stop() = 0;
  virtual SunshineAudioStats stats() const = 0;
};

}  // namespace tianguan
