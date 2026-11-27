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
