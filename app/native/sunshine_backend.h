// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 会话后端（只在 OHOS 目标里编译，TIANGUAN_WITH_SUNSHINE）。
//
// 与 rdp_backend.h 同一套宿主契约：后端自己拥有一次完整的连接尝试
// （身份 -> /serverinfo -> 需要时配对 -> /applist -> /launch -> LiStartConnection
//  -> 断线重连 -> 停止），但它不认识 diagnostics.cxx 的 SessionState，所有状态写入、
// 帧提交和输入出队都通过下面的 SunshineHost 回调完成。宿主的适配层负责和 VNC/RDP
// 完全相同的 stop / generation 语义、phase / errorCode 体系和 BGRA 帧镜像。
//
// 「主机结束串流」类的终止错误码是 0（ML_ERROR_GRACEFUL_TERMINATION，
// Limelight.h:421）与 -1（控制流被对端断开且没有 termination 消息，
// ControlStream.c:1600 / 1369；Sunshine 的 /cancel 与主机端应用退出都走这条）。
// 判定按「终止前正常画面的持续时长」= 最近非黑帧 - 首个非黑帧：< 3000 ms（含
// 从未有画面）视为主机显示会话状态变化（控制台会话被接回、被远程桌面占用等）
// 造成的瞬时黑屏，自动重连最多 3 次（2/4/8s 退避）；>= 3000 ms 说明用户已正常
// 观看，按主机主动结束处理，不自动重连。
//
// 线程模型（详见 sunshine_backend.cxx 顶部注释）：
//   * 会话线程：本文件 runSunshineSession()，负责 HTTP 控制面、common-c 的
//     启动/停止，以及连接建立后的等待循环；
//   * 输入线程：连接建立后由后端自开，10ms 节拍调用 host.drainInput()；
//   * common-c 回调线程：视频解码、音频解码、连接终止（终止回调由 common-c
//     自己的 detached 线程投递）；这些回调只写本文件的进程级状态，或调用
//     宿主里持锁的适配函数。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tianguan {

// 数值与 diagnostics.cxx 的 SessionPhase 一一对应（适配层 static_assert 校验）。
// 这里重复定义是为了让后端不包含 VNC 侧的翻译单元。PairingRequired 是本次新增的
// 状态：会话在等用户提交 PIN，既不算失败也不算已连接。
enum SunshinePhase {
  SunshinePhaseConnecting = 0,
  SunshinePhaseConnected = 1,
  SunshinePhaseDisconnected = 2,
  SunshinePhaseFailed = 3,
  SunshinePhaseTrustRequired = 4,
  SunshinePhaseReconnecting = 5,
  SunshinePhasePairingRequired = 6
};

// 一次连接尝试需要的全部参数。调用方在会话线程启动前从 SessionState 拷出。
struct SunshineOptions {
  std::string host;
  int port = 47989;          // Sunshine 的 HTTP 端口（HTTPS 默认取 port - 5）
  std::string stateDir;      // 应用沙箱绝对路径：身份、服务端 pin 都落在这里
  std::string app = "Desktop";
  int width = 1280;
  int height = 720;
  int fps = 60;
  int bitrate = 5000;        // kbps
  std::string formats;       // "h264,hevc"；空 = 按设备硬解能力自动
  bool encryption = true;    // false -> ENCFLG_NONE（sunshineEncryption=none）
};

// 画面全黑检测（Sunshine 专用，进程级单例，与 common-c 的回调状态同一生命周期）。
//
// 视频 sink 在 AVCodec 输出回调线程里对每一帧做稀疏亮度采样（每 16 行取一行、
// 每 16 像素取一个点，取 R/G/B 平均并忽略 alpha；平均亮度 < 4 判为黑帧），然后
// 只调用 sunshineNoteVideoLuma() 上报该帧的单调时钟毫秒时刻与黑/非黑判定；连续
// 全黑时长由状态读取方在读取时换算，解码线程上不做时间差、不加锁。
//   blackMs        当前连续全黑（或本次尝试迟迟没有帧）的毫秒数，出现非黑帧即清零；
//   lastFrameAgeMs 距最近一帧解码完成的毫秒数；本次尝试还没有帧时等于自视频
//                  setup 以来的毫秒数。
// 这些量只用于诊断/界面提示，不改变视频路径本身。
struct SunshinePictureStatus {
  SunshinePictureStatus() : blackMs(0), lastFrameAgeMs(0) {}
  unsigned long long blackMs;
  unsigned long long lastFrameAgeMs;
};

// 视频解码回调线程每解码出一帧调用一次。frameTimeMs 是单调时钟毫秒，black 是
// 该帧的亮度判定（见上）。只写两个原子时间戳，可在任意线程调用。
void sunshineNoteVideoLuma(unsigned long long frameTimeMs, bool black);
// 状态读取（任意线程）：返回当前尝试的画面状态；没有活动串流时两个量都是 0。
SunshinePictureStatus sunshinePictureStatus();

// 一帧解码结果的矩形（BGRA_8888，stride = w * 4）。和 RdpRect 同义，只是命名空间
// 上属于 Sunshine；视频解码实现通过 SunshineFrameSink 交给宿主。
struct SunshineRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int stride = 0;
  const uint8_t* pixels = nullptr;
};

// 从共享会话队列取出的输入事件。后端自己维护按下的键/按钮集合，所以 release
// 能精确抬起它自己按下的东西。
struct SunshineInputSink {
  void* ctx = nullptr;
  // x/y 已是远端像素（宿主按 contain 缩放换算过），mask 是 RFB 约定：
  // bit0 左键、bit1 中键、bit2 右键、bit3 滚轮上、bit4 滚轮下。
  void (*pointer)(void* ctx, int x, int y, int mask) = nullptr;
  void (*key)(void* ctx, int keysym, int pressed) = nullptr;
  // Moonlight 协议没有剪贴板通道，后端只记录并忽略（注释见 .cxx）。
  void (*clipboard)(void* ctx, const char* text) = nullptr;
  // releaseSessionInput() 推进了释放纪元：抬起本次尝试按下的所有键和按钮。
  void (*release)(void* ctx) = nullptr;
};

// 会话侧桥。除 stopRequested()/generation() 可在任意线程查询外，全部回调都由
// 后端在会话线程或后端自开的输入线程里调用；宿主实现只用各自会话锁做短临界区，
// 因此从非会话线程调用是安全的（见 diagnostics.cxx 适配层的注释）。
struct SunshineHost {
  void* user = nullptr;

  bool (*stopRequested)(void* user) = nullptr;
  int (*generation)(void* user) = nullptr;
  // 自动重试时递增 generation（与 RDP 的 beginAttempt 同义）。*lastApplied 由后端持有。
  int (*beginAttempt)(void* user, int* lastApplied) = nullptr;
  // error/errorCode 可为 null，分别表示 "" / "none"。
  void (*setPhase)(void* user, int phase, const char* error,
                   const char* errorCode, int retryInMs) = nullptr;
  bool (*sleepUnlessStopped)(void* user, int milliseconds) = nullptr;
  // 一直等到用户手动重连或停止（用于不会自愈的失败）。
  void (*waitForReconnect)(void* user) = nullptr;
  // pairing-required 阶段等 UI 提交 4 位 PIN。返回 true 并填 *pin 表示拿到；
  // 返回 false 表示被 stop 打断或 generation 已经变化。PIN 只活到本次配对。
  bool (*waitForPin)(void* user, int generation, std::string* pin) = nullptr;
  // /serverinfo 的 appversion，pairing-required 时随状态 JSON 的 pairing 字段展示。
  void (*setServerAppVersion)(void* user, int generation,
                              const char* appVersion) = nullptr;
  // 实际启动的应用标题（/applist 匹配结果），作为状态 JSON 的可选字段。
  void (*setAppTitle)(void* user, int generation, const char* title) = nullptr;

  // 重建帧镜像（视频 setup 时调用一次）：语义与 RdpHost::resizeFrame 完全相同。
  void (*resizeFrame)(void* user, int generation, int width, int height) = nullptr;
  // 提交一帧解码结果（BGRA）：语义与 RdpHost::commitFrame 完全相同。
  void (*commitFrame)(void* user, int generation, const SunshineRect* rects,
                      size_t count, int width, int height) = nullptr;
  // 零拷贝视频路径（Sunshine 专用，可选）：由宿主把会话的渲染表面包成外部纹理源。
  //   acquireExternalSource: 成功时填 *textureId（OES 纹理）与 *nativeWindow（给
  //     OH_VideoDecoder_SetSurface 的 OHNativeWindow，用 void* 表示）；返回 false
  //     表示渲染表面还没 attach / 超时 / 被隐藏，视频路径会回退 Buffer 模式。
  //     会在 common-c 视频线程（setup 内）阻塞等待，最长约 10 秒。
  //   setSourceSize: Surface 模式的有效画面尺寸（无 stride/像素格式），宿主据此
  //     更新状态里的宽高并按新尺寸重建（空的）镜像。
  //   releaseExternalSource: 归还外部纹理源；视频路径保证在解码器 Stop/Destroy
  //     之后才调用，宿主可以安全销毁 OH_NativeImage。
  bool (*acquireExternalSource)(void* user, int generation, unsigned int* textureId,
                                void** nativeWindow) = nullptr;
  void (*setSourceSize)(void* user, int generation, int width, int height) = nullptr;
  void (*releaseExternalSource)(void* user, int generation) = nullptr;

  // 客户端主动分辨率（NAPI requestDesktopSize）：会话线程在已连接期间每 200ms
  // 取一次。返回 true 时填 width/height，并把请求槽标记为 in-flight；语义与
  // RdpHost 的同名回调一致（一次只处理一个，处理期间到达的新请求只保留最后一个）。
  bool (*takeDesktopSizeRequest)(void* user, int* width, int* height) = nullptr;
  // 结束 in-flight 请求：applied=true 表示按新尺寸重启的串流已经建立（Sunshine
  // 没有服务端应答，这就是正 ack）；false/终态表示失败或会话结束，只清 in-flight
  // 与 resize 标记，不改 resizeSupport（保持 unknown）。
  void (*finishDesktopSizeRequest)(void* user, int generation, int width,
                                   int height, bool applied) = nullptr;

  // 主机光标（本地光标模式）：Sunshine 的光标画在视频里，延迟等于视频链路，
  // 所以用「Ctrl+Alt+Shift+N」切换主机全局 display_cursor（Sunshine input.cpp
  // apply_shortcut），再由客户端显示本地系统光标。该状态是主机进程级、跨会话
  // 保持的，客户端只能记录、无法查询。输入线程每 tick 取一次待发送目标：
  // 返回 true 时 *hidden 是目标值（peek，不清除；发送成功后用 noteCursorHidden
  // 确认，失败下一 tick 重试）。
  bool (*takeCursorHiddenRequest)(void* user, bool* hidden) = nullptr;
  // 组合键发送成功：更新本地记录并清掉待发标记。
  void (*noteCursorHidden)(void* user, int generation, bool hidden) = nullptr;

  // 清掉上一次连接遗留的指针/按键输入，返回 releaseEpoch。
  unsigned (*prepareInput)(void* user) = nullptr;
  // 把队列整体换出后逐条喂给 sink；*appliedRelease 在真正释放后推进。
  void (*drainInput)(void* user, const SunshineInputSink* sink,
                     unsigned* appliedRelease) = nullptr;
};

// 阻塞直到宿主要求停止。内部按 generation 执行
// 「serverinfo -> 必要时配对 -> 选应用 -> launch -> 串流 -> 断线退避重试」循环。
void runSunshineSession(const SunshineHost& host, const SunshineOptions& options);

}  // namespace tianguan
