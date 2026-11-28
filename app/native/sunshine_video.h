// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 视频渲染接口。
//
// 实现是 OHOS AVCodec 硬解（`OH_VideoDecoder`，Buffer 模式）：输入是 common-c
// 给的 AnnexB 解码单元，输出 NV12/NV21 经 libswscale 转 BGRA 后交给宿主的帧
// 镜像（SunshineFrameSink 的 resize/commit），从而复用现有的 EGL 渲染、缩放与
// 坐标换算。线程与锁定策略见 sunshine_video.cxx 顶部注释：
//   setup():  按 VIDEO_FORMAT_* 选 mime（H.264 -> video/avc，HEVC -> video/hevc），
//             创建解码器、Configure（宽高 / AV_PIXEL_FORMAT_NV12 / 低延迟）、
//             RegisterCallback、Prepare、Start；其他格式（AV1 等）返回 false；
//   submitDecodeUnit(): 把 PDECODE_UNIT 的 bufferList 顺序拼成一个 AnnexB 缓冲，
//             有解码器交还的输入 buffer 就立刻 PushInputBuffer，否则进 4 深有界
//             队列；队列满则丢弃该解码单元并返回 DR_NEED_IDR；
//   stop():   停并销毁解码器、释放 sws 上下文与 BGRA 缓冲、清空输入队列，但不释放
//             本对象（LiStopConnection 之后后端才 delete）。可以从会话线程调用，
//             而 AVCodec 回调线程可能正在跑（内部等待在途回调结束）。
// 帧几何：解码尺寸以解码器输出描述为准（OH_MD_KEY_VIDEO_PIC_WIDTH/HEIGHT）；
// 首帧或分辨率变化必须先调用 frameSink.resize() 再提交新尺寸的帧，宿主据此重建
// 镜像并丢弃上一代像素。同一时刻只有一个线程（AVCodec 输出回调线程）提交像素。
#pragma once

#include <Limelight.h>

#include <cstdint>

#include "sunshine_backend.h"

namespace tianguan {

// 解码结果交给宿主的出口。user 只在一次连接内有效，generation 用来丢弃上一代
// 连接迟到的帧（宿主会再核对 generation）。
struct SunshineFrameSink {
  void* user = nullptr;
  void (*resize)(void* user, int generation, int width, int height) = nullptr;
  void (*commit)(void* user, int generation, const SunshineRect* rects,
                 size_t count, int width, int height) = nullptr;

  // 零拷贝（Surface 模式）帧源，Sunshine 专用；可选，宿主不实现时解码器走原来的
  // Buffer 模式（NV12 -> BGRA -> commit）。
  //   acquireExternalSource: 申请一个外部纹理源。成功时填 *textureId（OES 纹理）
  //     与 *nativeWindow（交给 OH_VideoDecoder_SetSurface 的 OHNativeWindow，用
  //     void* 表示，避免本头文件依赖 OHOS 类型），失败返回 false（surface 尚未
  //     attach、超时、或平台不支持）——调用方必须回退 Buffer 模式。
  //   setSourceSize: 报告 Surface 模式下的有效画面尺寸（没有 stride/像素格式），
  //     宿主用它更新镜像/状态尺寸，渲染线程用它做 contain 几何。
  //   releaseExternalSource: 归还外部纹理源，必须在解码器 Stop/Destroy 之后调用。
  bool (*acquireExternalSource)(void* user, int generation, unsigned int* textureId,
                                void** nativeWindow) = nullptr;
  void (*setSourceSize)(void* user, int generation, int width, int height) = nullptr;
  void (*releaseExternalSource)(void* user, int generation) = nullptr;
};

// 只读计数，供后端定期打日志（不含任何像素内容）。
//   frames / idrFrames：成功解码并提交给宿主的帧数 / 输出里带同步帧标志的帧数；
//   bytes：送进解码器的 AnnexB 字节数（含 SPS/PPS/VPS）；
//   width/height/fps：setup 协商值（不是解码器输出尺寸，输出尺寸见日志）。
struct SunshineVideoStats {
  SunshineVideoStats()
      : frames(0), idrFrames(0), bytes(0), videoFormat(0), width(0), height(0),
        fps(0) {}
  unsigned long long frames;
  unsigned long long idrFrames;
  unsigned long long bytes;
  int videoFormat;  // Limelight VIDEO_FORMAT_*
  int width;
  int height;
  int fps;
};

// 解码渲染器。create() 永远返回可用实例（失败抛 std::bad_alloc），由后端在每次
// 连接尝试里创建一次，并在 LiStopConnection() 返回后延时释放。
class SunshineVideoSink {
 public:
  static SunshineVideoSink* create();

  virtual ~SunshineVideoSink() {}

  // 绑定宿主的帧提交出口与本次尝试的 generation（每次连接前调用一次）。
  virtual void bindFrameSink(const SunshineFrameSink& sink, int generation) = 0;
  // DECODER_RENDERER_CALLBACKS::setup。返回 false 会让 common-c 放弃本次启动。
  virtual bool setup(int videoFormat, int width, int height, int fps) = 0;
  // DECODER_RENDERER_CALLBACKS::submitDecodeUnit。返回 DR_OK / DR_NEED_IDR。
  virtual int submitDecodeUnit(PDECODE_UNIT decodeUnit) = 0;
  // DECODER_RENDERER_CALLBACKS::stop：视频线程已经停下，停解码器并清计数，不释放对象。
  virtual void stop() = 0;
  virtual SunshineVideoStats stats() const = 0;
};

// 视频链路上的分段耗时快照：最近一个完整的 5 秒统计窗口，耗时单位毫秒。
// 与 sunshine_backend.h 的 SunshinePictureStatus 一样是进程级只读快照：视频线程
// 与 AVCodec 输出回调线程写入，后端（5 秒汇总日志）和宿主（statusJson.perf）在
// 任意线程读取，读到的是一份算好的值，不加锁等待解码。
//   netToSubmit      common-c 入队（enqueueTimeUs）到 submitDecodeUnit 被调用；
//   submitToOutput   送进 AVCodec 到对应帧的输出回调（按 PTS 配对，配不上用最近
//                    已配对样本的均值，见 sunshine_video.cxx）；
//   convert          输出回调里 NV12/NV21 -> BGRA（libswscale）的耗时；
//   commit           frameSink.commit（含宿主镜像拷贝）的耗时。
// inFps/decFps 是本窗口的输入帧率与解码输出帧率；上屏帧率在 render.h 的
// RenderFramePerf.outFps 里（由渲染线程统计）。
struct SunshineVideoPerf {
  SunshineVideoPerf()
      : windowMs(0),
        inFrames(0),
        decFrames(0),
        droppedUnits(0),
        inFps(0.0),
        decFps(0.0),
        netToSubmitAvgMs(0.0),
        netToSubmitP95Ms(0.0),
        submitToOutputAvgMs(0.0),
        submitToOutputP95Ms(0.0),
        convertAvgMs(0.0),
        convertP95Ms(0.0),
        commitAvgMs(0.0),
        commitP95Ms(0.0) {}
  unsigned long long windowMs;      // 窗口实际长度（0 = 还没有完整窗口）
  unsigned long long inFrames;      // 窗口内 submitDecodeUnit 调用数
  unsigned long long decFrames;     // 窗口内解码输出并提交给宿主的帧数
  unsigned long long droppedUnits; // 窗口内因输入队列满丢弃的解码单元数
  double inFps;
  double decFps;
  double netToSubmitAvgMs, netToSubmitP95Ms;
  double submitToOutputAvgMs, submitToOutputP95Ms;
  double convertAvgMs, convertP95Ms;
  double commitAvgMs, commitP95Ms;
};

// 最近一个完整 5 秒窗口的耗时快照；没有连接或还没到 5 秒时全 0。
SunshineVideoPerf sunshineVideoPerf();

}  // namespace tianguan
