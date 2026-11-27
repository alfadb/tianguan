// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sunshine / Moonlight 会话后端（只在 OHOS 目标里编译，TIANGUAN_WITH_SUNSHINE）。
//
// 一次连接尝试（每个 generation 一个，结构对齐 rdp_backend.cxx）：
//
//   beginAttempt -> connecting -> load_or_create_identity(state_dir)
//     -> NvHttpClient(http_port=会话端口, https_port=端口-5)
//     -> get_server_info()：指纹不一致 -> failed/fingerprint-mismatch；
//        pair_status != 1 -> pairing-required ->（等 UI 提交 PIN）-> /pair
//     -> get_app_list()：按标题匹配 -> /launch（mode=WxHxFPS，拿 rikey/rikeyid）
//     -> LiStartConnection()（视频/音频/连接回调 + 输入线程）
//     -> 连上 -> 等终止/停止/换代 -> LiStopConnection -> 退避重连
//   stop 时：看门狗线程在启动阶段用 LiInterruptConnection() 打断阻塞的握手，
//   会话线程在连接建立后 10ms 内自己发现停止并 LiStopConnection()。
//
// 为什么停止路径不是「另一个线程调 LiStopConnection()」：common-c 明确写着
// LiStopConnection()/LiStartConnection() 都不是线程安全的（它们共用全局 stage 和
// 流对象），文档给出的唯一跨线程中止手段是 LiInterruptConnection()。因此看门狗只
// 调 LiInterruptConnection()，LiStopConnection() 始终由持有串流的那条线程调用；
// 这样也避免了两个线程同时拆 enet/RTSP 状态。
//
// 线程清单：
//   * 会话线程：runSunshineSession() 及其 runMediaStream()（HTTP 控制面 + common-c
//     启停 + 连接期间的等待循环），宿主 setPhase/resizeFrame 在这里调用；
//   * 看门狗线程：只在 LiStartConnection() 期间存在，50ms 检查一次 stop；
//   * 输入线程：连上后存在，10ms 节拍 host.drainInput() -> LiSend*()；
//   * common-c 回调线程：视频/音频解码线程 + 终止通知的 detached 线程。视频 setup 会
//     调 host.resizeFrame()，其余回调只写本文件的进程级状态。
// 宿主函数都只做「取会话锁 + 短拷贝」，任何线程调用都不会长阻塞，见
// diagnostics.cxx 适配层的注释。
//
// 进程级单例：moonlight-common-c 的回调没有 context 指针，且整库是进程级状态。
// 因此本文件同一时刻只允许一个串流（g_streamMutex），回调通过 g_media 读写当前
// 尝试；g_media 是进程生命周期对象，迟到的终止通知不会踩到已释放的栈对象。
#include "sunshine_backend.h"

#include <Limelight.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avcodec_base.h>

#if defined(__MUSL__)
#include <hilog/log.h>
#endif

#include "sunshine/nv_http.h"
#include "sunshine_audio.h"
#include "sunshine_video.h"

namespace tianguan {
namespace {

// 与 rdp_backend.cxx 相同的日志策略：一次性/错误信息走 WARN，周期统计走 DEBUG。
// 任何日志都不打印 PIN、rikey、私钥或剪贴板内容。
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

void wipeString(std::string& value) {
  if (value.empty()) return;
  volatile char* data = &value[0];
  for (size_t i = 0; i < value.size(); ++i) data[i] = 0;
  value.clear();
}

// 服务端消息可能带换行；状态 JSON 的 error 必须是单行（与 VNC/RDP 路径一致）。
std::string sanitizeLine(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i)
    out.push_back((text[i] == '\n' || text[i] == '\r') ? ' ' : text[i]);
  return out;
}

// ---------------------------------------------------------------------------
// keysym -> Win32 虚拟键码
//
// ArkTS 侧发来的是 X11 keysym（与 VNC/RDP 路径完全一致）。Moonlight 的
// LiSendKeyboardEvent() 收的是 Win32 VK，所以这里只做一次纯映射；大小写与
// Shift 后的符号都落到「US 布局上同一个物理键」，由 UI 单独发送的 Shift 键决定
// 最终字符（和 rdp_backend.cxx 的 scancode 表同一策略）。
// ---------------------------------------------------------------------------
enum {
  VK_BACK = 0x08,
  VK_TAB = 0x09,
  VK_CLEAR = 0x0C,
  VK_RETURN = 0x0D,
  VK_PAUSE = 0x13,
  VK_CAPITAL = 0x14,
  VK_ESCAPE = 0x1B,
  VK_SPACE = 0x20,
  VK_PRIOR = 0x21,
  VK_NEXT = 0x22,
  VK_END = 0x23,
  VK_HOME = 0x24,
  VK_LEFT = 0x25,
  VK_UP = 0x26,
  VK_RIGHT = 0x27,
  VK_DOWN = 0x28,
  VK_SNAPSHOT = 0x2C,
  VK_INSERT = 0x2D,
  VK_DELETE = 0x2E,
  VK_LWIN = 0x5B,
  VK_RWIN = 0x5C,
  VK_APPS = 0x5D,
  VK_NUMPAD0 = 0x60,
  VK_NUMPAD9 = 0x69,
  VK_MULTIPLY = 0x6A,
  VK_ADD = 0x6B,
  VK_SEPARATOR = 0x6C,
  VK_SUBTRACT = 0x6D,
  VK_DECIMAL = 0x6E,
  VK_DIVIDE = 0x6F,
  VK_F1 = 0x70,
  VK_F24 = 0x87,
  VK_NUMLOCK = 0x90,
  VK_SCROLL = 0x91,
  VK_LSHIFT = 0xA0,
  VK_RSHIFT = 0xA1,
  VK_LCONTROL = 0xA2,
  VK_RCONTROL = 0xA3,
  VK_LMENU = 0xA4,
  VK_RMENU = 0xA5,
  VK_OEM_1 = 0xBA,
  VK_OEM_PLUS = 0xBB,
  VK_OEM_COMMA = 0xBC,
  VK_OEM_MINUS = 0xBD,
  VK_OEM_PERIOD = 0xBE,
  VK_OEM_2 = 0xBF,
  VK_OEM_3 = 0xC0,
  VK_OEM_4 = 0xDB,
  VK_OEM_5 = 0xDC,
  VK_OEM_6 = 0xDD,
  VK_OEM_7 = 0xDE,
  VK_OEM_102 = 0xE2
};

short keysymToVk(int keysym) {
  if (keysym >= 'a' && keysym <= 'z')
    return static_cast<short>(keysym - 'a' + 'A');
  if (keysym >= 'A' && keysym <= 'Z') return static_cast<short>(keysym);
  if (keysym >= '0' && keysym <= '9') return static_cast<short>(keysym);
  // 可打印 ASCII：Shift 后的符号映射到同一个物理键。
  switch (keysym) {
    case ' ': return VK_SPACE;
    case '-':
    case '_': return VK_OEM_MINUS;
    case '=':
    case '+': return VK_OEM_PLUS;
    case '[':
    case '{': return VK_OEM_4;
    case ']':
    case '}': return VK_OEM_6;
    case '\\':
    case '|': return VK_OEM_5;
    case ';':
    case ':': return VK_OEM_1;
    case '\'':
    case '"': return VK_OEM_7;
    case '`':
    case '~': return VK_OEM_3;
    case ',':
    case '<': return VK_OEM_COMMA;
    case '.':
    case '>': return VK_OEM_PERIOD;
    case '/':
    case '?': return VK_OEM_2;
    case '!': return static_cast<short>('1');
    case '@': return static_cast<short>('2');
    case '#': return static_cast<short>('3');
    case '$': return static_cast<short>('4');
    case '%': return static_cast<short>('5');
    case '^': return static_cast<short>('6');
    case '&': return static_cast<short>('7');
    case '*': return static_cast<short>('8');
    case '(': return static_cast<short>('9');
    case ')': return static_cast<short>('0');
    default: break;
  }
  // F1..F24 与小键盘数字是连续区间，先按区间算，避免 40 行 case。
  if (keysym >= 0xFFBE && keysym <= 0xFFD5)
    return static_cast<short>(VK_F1 + (keysym - 0xFFBE));
  if (keysym >= 0xFFB0 && keysym <= 0xFFB9)
    return static_cast<short>(VK_NUMPAD0 + (keysym - 0xFFB0));
  switch (keysym) {
    // 编辑键 / 方向键 / 导航键
    case 0xFF08: return VK_BACK;      // BackSpace
    case 0xFF09: return VK_TAB;       // Tab
    case 0xFE20: return VK_TAB;       // ISO_Left_Tab（Shift+Tab，Shift 由 UI 单发）
    case 0xFF0A: return VK_RETURN;    // Linefeed
    case 0xFF0D: return VK_RETURN;    // Return
    case 0xFF1B: return VK_ESCAPE;    // Escape
    case 0xFF50: return VK_HOME;      // Home
    case 0xFF51: return VK_LEFT;      // Left
    case 0xFF52: return VK_UP;        // Up
    case 0xFF53: return VK_RIGHT;     // Right
    case 0xFF54: return VK_DOWN;      // Down
    case 0xFF55: return VK_PRIOR;     // Page_Up
    case 0xFF56: return VK_NEXT;      // Page_Down
    case 0xFF57: return VK_END;       // End
    case 0xFF63: return VK_INSERT;    // Insert
    case 0xFFFF: return VK_DELETE;    // Delete
    case 0xFF0B: return VK_CLEAR;     // Clear
    case 0xFF61: return VK_SNAPSHOT;  // Print
    case 0xFF15: return VK_SNAPSHOT;  // Sys_Req
    case 0xFF13: return VK_PAUSE;     // Pause
    case 0xFF67: return VK_APPS;      // Menu
    case 0xFF7E: return VK_RMENU;     // Mode_switch（AltGr）
    // 锁定键
    case 0xFFE5: return VK_CAPITAL;   // Caps_Lock
    case 0xFF7F: return VK_NUMLOCK;   // Num_Lock
    case 0xFF14: return VK_SCROLL;    // Scroll_Lock
    // 修饰键
    case 0xFFE1: return VK_LSHIFT;    // Shift_L
    case 0xFFE2: return VK_RSHIFT;    // Shift_R
    case 0xFFE3: return VK_LCONTROL;  // Control_L
    case 0xFFE4: return VK_RCONTROL;  // Control_R
    case 0xFFE7: return VK_LWIN;      // Meta_L
    case 0xFFE8: return VK_RWIN;      // Meta_R
    case 0xFFE9: return VK_LMENU;     // Alt_L
    case 0xFFEA: return VK_RMENU;     // Alt_R
    case 0xFFEB: return VK_LWIN;      // Super_L
    case 0xFFEC: return VK_RWIN;      // Super_R
    // 小键盘（KP_0..KP_9 已在上面按区间处理）
    case 0xFFAA: return VK_MULTIPLY;  // KP_Multiply
    case 0xFFAB: return VK_ADD;       // KP_Add
    case 0xFFAC: return VK_SEPARATOR; // KP_Separator
    case 0xFFAD: return VK_SUBTRACT;  // KP_Subtract
    case 0xFFAE: return VK_DECIMAL;   // KP_Decimal
    case 0xFFAF: return VK_DIVIDE;    // KP_Divide
    case 0xFF8D: return VK_RETURN;    // KP_Enter
    case 0xFFBD: return VK_OEM_PLUS;  // KP_Equal
    case 0xFF80: return VK_SPACE;     // KP_Space
    case 0xFF95: return VK_HOME;      // KP_Home
    case 0xFF96: return VK_LEFT;      // KP_Left
    case 0xFF97: return VK_UP;        // KP_Up
    case 0xFF98: return VK_RIGHT;     // KP_Right
    case 0xFF99: return VK_DOWN;      // KP_Down
    case 0xFF9A: return VK_PRIOR;     // KP_Page_Up
    case 0xFF9B: return VK_NEXT;      // KP_Page_Down
    case 0xFF9C: return VK_END;       // KP_End
    case 0xFF9E: return VK_INSERT;    // KP_Insert
    case 0xFF9F: return VK_DELETE;    // KP_Delete
    default: return 0;
  }
}

// 该 VK 是否修饰键，以及它在 LiSendKeyboardEvent() 的 modifiers 里对应的位。
// 返回 0 表示普通键。
char modifierBitForVk(short vk) {
  switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT: return MODIFIER_SHIFT;
    case VK_LCONTROL:
    case VK_RCONTROL: return MODIFIER_CTRL;
    case VK_LMENU:
    case VK_RMENU: return MODIFIER_ALT;
    case VK_LWIN:
    case VK_RWIN: return MODIFIER_META;
    default: return 0;
  }
}

// ---------------------------------------------------------------------------
// 输入翻译：共享队列 -> Moonlight 输入事件
//
// 只在输入线程里读写（streamWidth_/streamHeight_ 例外：视频 setup 在 common-c
// 线程写，这里读，所以用 atomic）。pointerMask_/modifiers_ 记录本端已经发出去的
// 状态，release 时按此精确抬起。RFB mask 约定与 rdp_backend.cxx 一致：
// bit0 左键、bit1 中键、bit2 右键、bit3 滚轮上、bit4 滚轮下。
// ---------------------------------------------------------------------------
class InputTranslator {
 public:
  InputTranslator()
      : streamWidth_(1280), streamHeight_(720), pointerMask_(0), modifiers_(0),
        dropLogs_(0) {}

  void setStreamSize(int width, int height) {
    if (width > 0) streamWidth_.store(width);
    if (height > 0) streamHeight_.store(height);
  }

  void reset() {
    pressedKeys_.clear();
    pointerMask_ = 0;
    modifiers_ = 0;
    dropLogs_ = 0;
  }

  void pointer(int x, int y, int mask) {
    int width = streamWidth_.load();
    int height = streamHeight_.load();
    if (width <= 0) width = 1280;
    if (height <= 0) height = 720;
    x = std::max(0, std::min(x, width - 1));
    y = std::max(0, std::min(y, height - 1));
    // 绝对坐标：x/y 已是远端像素，参考尺寸就是流分辨率。
    LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y),
                             static_cast<short>(width),
                             static_cast<short>(height));
    const int held = mask & 7;
    const int changed = held ^ (pointerMask_ & 7);
    if ((changed & 1) != 0)
      LiSendMouseButtonEvent((held & 1) != 0 ? BUTTON_ACTION_PRESS
                                             : BUTTON_ACTION_RELEASE,
                             BUTTON_LEFT);
    if ((changed & 2) != 0)
      LiSendMouseButtonEvent((held & 2) != 0 ? BUTTON_ACTION_PRESS
                                             : BUTTON_ACTION_RELEASE,
                             BUTTON_MIDDLE);
    if ((changed & 4) != 0)
      LiSendMouseButtonEvent((held & 4) != 0 ? BUTTON_ACTION_PRESS
                                             : BUTTON_ACTION_RELEASE,
                             BUTTON_RIGHT);
    pointerMask_ = held;
    // 一个 mask 最多带一个方向的滚轮位；两个都有时按上滚处理（与 RDP 路径一致）。
    if ((mask & 8) != 0)
      LiSendScrollEvent(1);
    else if ((mask & 16) != 0)
      LiSendScrollEvent(-1);
  }

  void key(int keysym, int pressed) {
    const short vk = keysymToVk(keysym);
    if (vk == 0) {
      // 未映射的键丢弃；只在开头打几条，避免按住一个键刷满日志。
      if (dropLogs_ < 8) {
        ++dropLogs_;
        logWarn(formatText("sunshine dropped key keysym 0x%04X: no VK mapping",
                           static_cast<unsigned>(keysym)));
      }
      return;
    }
    const char bit = modifierBitForVk(vk);
    if (pressed != 0) {
      // 修饰键本身按下时，modifiers 里还不包含它自己。
      LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, modifiers_);
      pressedKeys_.insert(vk);
      if (bit != 0) modifiers_ = static_cast<char>(modifiers_ | bit);
    } else {
      if (bit != 0) modifiers_ = static_cast<char>(modifiers_ & ~bit);
      LiSendKeyboardEvent(vk, KEY_ACTION_UP, modifiers_);
      pressedKeys_.erase(vk);
    }
  }

  void release() {
    // 合成所有已按下键的抬起；修饰键先把自己的位摘掉再发，和 key() 的规则一致。
    for (std::set<short>::const_iterator it = pressedKeys_.begin();
         it != pressedKeys_.end(); ++it) {
      const char bit = modifierBitForVk(*it);
      const char mods = bit != 0 ? static_cast<char>(modifiers_ & ~bit) : modifiers_;
      LiSendKeyboardEvent(*it, KEY_ACTION_UP, mods);
    }
    pressedKeys_.clear();
    modifiers_ = 0;
    const int held = pointerMask_ & 7;
    if ((held & 1) != 0)
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    if ((held & 2) != 0)
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE);
    if ((held & 4) != 0)
      LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    pointerMask_ = 0;
  }

  // Moonlight 协议没有剪贴板通道：setClipboard 的文本出队后直接丢弃。
  void clipboard(const char*) {}

 private:
  std::atomic<int> streamWidth_;
  std::atomic<int> streamHeight_;
  std::set<short> pressedKeys_;
  int pointerMask_;
  char modifiers_;
  int dropLogs_;
};

// ---------------------------------------------------------------------------
// 进程级串流状态（moonlight-common-c 是进程级单例，回调没有 context）
// ---------------------------------------------------------------------------
struct MediaContext {
  MediaContext()
      : host(nullptr), generation(0), video(nullptr), audio(nullptr),
        active(false), started(false), terminated(false), terminationCode(0),
        failedStage(0), failedStageError(0) {}

  const SunshineHost* host;
  int generation;
  SunshineVideoSink* video;
  SunshineAudioSink* audio;
  InputTranslator input;

  std::atomic<bool> active;   // 串流期间为 true，LiStopConnection 之后置 false
  std::atomic<bool> started;  // connectionStarted 已触发
  std::atomic<bool> terminated;
  std::atomic<int> terminationCode;
  std::atomic<int> failedStage;
  std::atomic<int> failedStageError;

  std::mutex textMutex;
  std::string terminationText;
};

// 同一时刻只允许一个串流：common-c 的全局状态（流对象、stage、回调）是单例。
std::timed_mutex g_streamMutex;
MediaContext g_media;

// ---------------------------------------------------------------------------
// common-c 回调
// ---------------------------------------------------------------------------
int videoSetupCallback(int videoFormat, int width, int height, int redrawRate,
                       void* context, int flags) {
  (void)context;
  (void)flags;
  MediaContext& media = g_media;
  if (!media.active.load() || media.video == nullptr) return -1;
  const bool ok = media.video->setup(videoFormat, width, height, redrawRate);
  if (!ok) {
    logWarn(formatText("sunshine video setup failed: format=0x%X %dx%d@%d",
                       videoFormat, width, height, redrawRate));
    return -1;
  }
  // 先把几何交给宿主（重建镜像），再让解码器出帧；输入线程的参考分辨率也在这里更新。
  media.input.setStreamSize(width, height);
  if (media.host != nullptr && media.host->resizeFrame != nullptr)
    media.host->resizeFrame(media.host->user, media.generation, width, height);
  logDebug(formatText("sunshine video setup: format=0x%X %dx%d@%d", videoFormat,
                      width, height, redrawRate));
  return 0;
}

int videoSubmitCallback(PDECODE_UNIT decodeUnit) {
  MediaContext& media = g_media;
  if (!media.active.load() || media.video == nullptr) return DR_OK;
  return media.video->submitDecodeUnit(decodeUnit);
}

void videoStopCallback(void) {
  MediaContext& media = g_media;
  if (media.video != nullptr) media.video->stop();
}

int audioInitCallback(int audioConfiguration,
                      const POPUS_MULTISTREAM_CONFIGURATION opusConfig,
                      void* context, int flags) {
  (void)context;
  (void)flags;
  MediaContext& media = g_media;
  if (media.audio == nullptr) return 0;
  return media.audio->init(audioConfiguration, opusConfig);
}

void audioDecodeCallback(char* sampleData, int sampleLength) {
  MediaContext& media = g_media;
  if (!media.active.load() || media.audio == nullptr) return;
  media.audio->decodeAndPlaySample(sampleData, sampleLength);
}

void audioStopCallback(void) {
  MediaContext& media = g_media;
  if (media.audio != nullptr) media.audio->stop();
}

void stageStartingCallback(int stage) {
  if (!g_media.active.load()) return;
  logDebug(formatText("sunshine stage starting: %s", LiGetStageName(stage)));
}

void stageCompleteCallback(int stage) {
  if (!g_media.active.load()) return;
  logDebug(formatText("sunshine stage complete: %s", LiGetStageName(stage)));
}

void stageFailedCallback(int stage, int errorCode) {
  MediaContext& media = g_media;
  if (!media.active.load()) return;
  media.failedStage.store(stage);
  media.failedStageError.store(errorCode);
  unsigned int portFlags = LiGetPortFlagsFromStage(stage);
  char ports[128];
  ports[0] = '\0';
  LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
  logWarn(formatText("sunshine stage failed: %s (stage %d), error %d%s%s",
                     LiGetStageName(stage), stage, errorCode,
                     ports[0] != '\0' ? ", likely ports: " : "", ports));
}

void connectionStartedCallback(void) {
  MediaContext& media = g_media;
  if (!media.active.load() || media.host == nullptr) return;
  media.started.store(true);
  // 该回调在 LiStartConnection() 内部、会话线程上触发，所以直接写宿主状态是安全的。
  media.host->setPhase(media.host->user, SunshinePhaseConnected, "", "none", 0);
  logWarn("sunshine connection established");
}

void connectionTerminatedCallback(int errorCode) {
  MediaContext& media = g_media;
  unsigned int portFlags = LiGetPortFlagsFromTerminationErrorCode(errorCode);
  char ports[128];
  ports[0] = '\0';
  LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
  const std::string text =
      formatText("串流已终止（错误码 %d%s%s）", errorCode,
                 ports[0] != '\0' ? "，可能被占用的端口：" : "", ports);
  {
    std::lock_guard<std::mutex> lock(media.textMutex);
    media.terminationText = text;
  }
  media.terminationCode.store(errorCode);
  media.terminated.store(true);
  logWarn("sunshine " + text);
}

void logMessageCallback(const char* format, ...) {
  char message[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  // common-c 会打印分段行（"...", "done\n"），去掉行尾换行避免日志出现空行。
  std::string line(message);
  while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
    line.resize(line.size() - 1);
  if (!line.empty()) logDebug("moonlight: " + line);
}

void setHdrModeCallback(bool hdrEnabled) {
  if (!g_media.active.load()) return;
  logDebug(std::string("sunshine host HDR mode: ") + (hdrEnabled ? "on" : "off"));
}

// ---------------------------------------------------------------------------
// 能力探测与参数解析
// ---------------------------------------------------------------------------

// 设备是否有 HEVC 硬解：OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_HEVC,
// false, HARDWARE) != nullptr。
//
// 视频路径现在直链 OHOS AVCodec（libnative_media_vdec / _core / _codecbase /
// native_buffer，见 sunshine_video.cxx 与 CMakeLists.txt），所以这里也直接调用，
// 不再用 dlopen/dlsym 运行时解析——那套写法原本只是为了避免新增共享依赖。
// 显式 sunshineFormats 不受这里影响；探测返回空只表示自动模式不选 HEVC。
bool hasHevcHardwareDecoder() {
  return OH_AVCodec_GetCapabilityByCategory(OH_AVCODEC_MIMETYPE_VIDEO_HEVC, false,
                                            HARDWARE) != nullptr;
}

// "h264,hevc" -> VIDEO_FORMAT_* 掩码。空串返回 0（交给自动探测）。大小写不敏感，
// 允许前后空白。未知名字忽略（parseSessionOptions 已经拒绝过非法值，这里是兜底）。
int parseFormatsMask(const std::string& text) {
  int mask = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) end = text.size();
    std::string token = text.substr(start, end - start);
    size_t first = 0;
    while (first < token.size() &&
           std::isspace(static_cast<unsigned char>(token[first])) != 0)
      ++first;
    size_t last = token.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(token[last - 1])) != 0)
      --last;
    token = token.substr(first, last - first);
    for (size_t i = 0; i < token.size(); ++i)
      token[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(token[i])));
    if (token == "h264")
      mask |= VIDEO_FORMAT_H264;
    else if (token == "hevc" || token == "h265")
      mask |= VIDEO_FORMAT_H265;
    if (end == text.size()) break;
    start = end + 1;
  }
  return mask;
}

// 自动模式：H.264 恒有；HEVC 需要设备有硬解、且服务端声明支持（否则 RTSP 协商
// 会选中一个服务端编不出来的格式，直接失败）。
int chooseVideoFormats(const SunshineOptions& options, uint32_t serverCodecSupport) {
  if (!options.formats.empty()) {
    const int requested = parseFormatsMask(options.formats);
    if (requested != 0) return requested;
  }
  int mask = VIDEO_FORMAT_H264;
  if ((serverCodecSupport & SCM_MASK_HEVC) != 0 && hasHevcHardwareDecoder())
    mask |= VIDEO_FORMAT_H265;
  return mask;
}

std::string videoFormatsText(int mask) {
  std::string text;
  if ((mask & VIDEO_FORMAT_H264) != 0) text += "h264";
  if ((mask & VIDEO_FORMAT_H265) != 0) {
    if (!text.empty()) text += ",";
    text += "hevc";
  }
  if (text.empty()) text = "none";
  return text;
}

bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
  if (needle.empty()) return false;
  const size_t n = needle.size();
  if (haystack.size() < n) return false;
  for (size_t i = 0; i + n <= haystack.size(); ++i) {
    size_t j = 0;
    while (j < n &&
           std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
               std::tolower(static_cast<unsigned char>(needle[j])))
      ++j;
    if (j == n) return true;
  }
  return false;
}

// 按标题不区分大小写「包含」匹配；没有匹配时用第一个应用。
const tg::sunshine::AppInfo* pickApp(
    const std::vector<tg::sunshine::AppInfo>& apps, const std::string& wanted) {
  if (apps.empty()) return nullptr;
  for (size_t i = 0; i < apps.size(); ++i) {
    if (containsIgnoreCase(apps[i].title, wanted)) return &apps[i];
  }
  return &apps[0];
}

bool parseHexBytes(const std::string& text, uint8_t* output, size_t outputLength) {
  std::string digits = text;
  if (digits.size() > 2 && digits[0] == '0' &&
      (digits[1] == 'x' || digits[1] == 'X'))
    digits = digits.substr(2);
  if (digits.size() != outputLength * 2) return false;
  for (size_t i = 0; i < outputLength; ++i) {
    int high = 0;
    int low = 0;
    for (int half = 0; half < 2; ++half) {
      const char c = digits[i * 2 + static_cast<size_t>(half)];
      int value = -1;
      if (c >= '0' && c <= '9') value = c - '0';
      else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
      if (value < 0) return false;
      if (half == 0) high = value;
      else low = value;
    }
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

// 构造 NvHttpClient 的选项。https_port 为 0 时交给 /serverinfo 的 HttpsPort 发现。
tg::sunshine::NvHttpClient::Options makeClientOptions(
    const SunshineOptions& options, uint16_t httpsPort) {
  tg::sunshine::NvHttpClient::Options clientOptions;
  clientOptions.host = options.host;
  clientOptions.http_port = static_cast<uint16_t>(options.port);
  clientOptions.https_port = httpsPort;
  clientOptions.state_dir = options.stateDir;
  // getservercert 阶段服务端会挂起到网页端提交 PIN，单独给 5 分钟。
  clientOptions.pin_wait_ms = 300000;
  return clientOptions;
}

void pairProgress(const tg::sunshine::PairStageRecord& stage) {
  if (stage.ok) {
    logDebug(formatText("sunshine pair stage %s ok (%lld ms)", stage.name.c_str(),
                        stage.elapsed_ms));
  } else {
    logWarn(formatText("sunshine pair stage %s failed (%lld ms): %s",
                       stage.name.c_str(), stage.elapsed_ms, stage.detail.c_str()));
  }
}

// ---------------------------------------------------------------------------
// 残留会话（/launch 报 "An app is already running"）
//
// 服务端上一条会话没被 /cancel 掉时，/launch 返回 status_code=400
// "An app is already running on this host"。Moonlight 客户端的通行做法：
//   * 要启动的应用就是服务端正在跑的那个 -> /resume 接管续播（参数同 /launch）；
//   * 服务端在跑别的应用 -> 先 /cancel，等服务端回到空闲，再 /launch；
//   * /cancel 自身失败只记日志，绝不让会话失败。
// ---------------------------------------------------------------------------

bool serverBusy(const tg::sunshine::ServerInfo& info) {
  return info.state == "SUNSHINE_SERVER_BUSY";
}

// /launch 的「应用已在运行」判定：Sunshine 用 status_code=400 + 该文案。
bool launchAlreadyRunning(const tg::sunshine::NvError& error) {
  if (error.nv_status_code() != 400) return false;
  return containsIgnoreCase(error.status_message(), "already running") ||
         containsIgnoreCase(error.what(), "already running");
}

// /cancel 尽力而为：服务端可能已经自行清理，失败只记日志，不让会话失败。
void cancelServerSession(tg::sunshine::NvHttpClient& client) {
  try {
    client.cancel();
    logWarn("sunshine: host session cancelled");
  } catch (const std::exception& error) {
    logDebug(std::string("sunshine cancel failed (ignored): ") + error.what());
  }
}

// 轮询 /serverinfo 直到服务端空闲：间隔 500ms、按 deadline 最多 10 秒（用时间而
// 不是次数截止，单次探测自己也可能因为网络慢而耗时）。
// 返回 false 表示被 stop 或换代打断，调用方应立即结束本次尝试；返回 true 表示
// 轮询结束——即使超时也继续 /launch（state 标记可能滞后，真正的失败由调用方的
// 「已经有一个在运行」兜底再处理一次）。
bool waitServerFree(tg::sunshine::NvHttpClient& client, const SunshineHost& host,
                    int generation, tg::sunshine::ServerInfo* info) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (host.stopRequested(host.user)) return false;
    if (host.generation != nullptr && host.generation(host.user) != generation)
      return false;
    try {
      *info = client.get_server_info().server;
    } catch (const tg::sunshine::PinMismatchError&) {
      // 指纹不一致必须让整次连接失败，不能在这里被当成瞬时错误吞掉。
      throw;
    } catch (const std::exception& error) {
      logDebug(std::string("sunshine serverinfo poll failed (ignored): ") + error.what());
    }
    if (!serverBusy(*info)) return true;
    if (!host.sleepUnlessStopped(host.user, 500)) return false;
  }
  return true;
}

// cancel -> 等空闲 -> 启动。cancel 后若服务端仍指向同一个应用，用 /resume 接管
// （参数与 /launch 相同），否则 /launch。返回 false 表示被 stop/换代打断；内部
// launch/resume 的异常原样抛出，调用方只会兜底一次。
bool cancelAndStartApp(tg::sunshine::NvHttpClient& client, const SunshineHost& host,
                       int generation, const tg::sunshine::LaunchOptions& options,
                       int appId, tg::sunshine::ServerInfo info,
                       tg::sunshine::SessionLaunch* launch, bool* resumed) {
  if (resumed != nullptr) *resumed = false;
  cancelServerSession(client);
  if (!waitServerFree(client, host, generation, &info)) return false;
  if (info.current_game == appId) {
    *launch = client.resume(options);
    if (resumed != nullptr) *resumed = true;
    logWarn("sunshine: resumed the app that was already running on the host");
  } else {
    *launch = client.launch(options);
  }
  return true;
}

// ---------------------------------------------------------------------------
// 串流阶段（LiStartConnection 及其等待/停止）
// ---------------------------------------------------------------------------
struct MediaOutcome {
  MediaOutcome() : stopped(false), connected(false), restart(false) {}
  bool stopped;    // 宿主已要求停止
  bool connected;  // LiStartConnection 成功过
  bool restart;    // generation 变化（手动重连），应立即进入下一次尝试
  std::string text;  // 启动失败或终止原因
};

// 在 stop 可打断的前提下最多等 timeoutMs 拿到进程级串流槽，避免两条 Sunshine
// 会话同时驱动 common-c 的全局状态。返回 false 表示没拿到（stop 或超时）。
bool acquireStreamSlot(const SunshineHost& host, int timeoutMs) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!g_streamMutex.try_lock()) {
    if (host.stopRequested(host.user)) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return true;
}

MediaOutcome runMediaStream(const SunshineHost& host, const SunshineOptions& options,
                            const std::string& appVersion,
                            const std::string& gfeVersion, int codecSupport,
                            const std::string& sessionUrl,
                            const std::string& rikey, uint32_t rikeyid,
                            int generation) {
  MediaOutcome outcome;

  uint8_t key[16];
  std::memset(key, 0, sizeof(key));
  if (!parseHexBytes(rikey, key, sizeof(key))) {
    outcome.text = "launch 返回的 rikey 不是 32 位十六进制";
    return outcome;
  }
  // 服务端把 rikeyid 当大端 32 位数读：IV 前 4 字节 = rikeyid 大端，其余 0。
  char iv[16];
  std::memset(iv, 0, sizeof(iv));
  iv[0] = static_cast<char>((rikeyid >> 24) & 0xFF);
  iv[1] = static_cast<char>((rikeyid >> 16) & 0xFF);
  iv[2] = static_cast<char>((rikeyid >> 8) & 0xFF);
  iv[3] = static_cast<char>(rikeyid & 0xFF);

  if (!acquireStreamSlot(host, 10000)) {
    if (host.stopRequested(host.user)) {
      outcome.stopped = true;
    } else {
      outcome.text = "已有另一个 Sunshine 串流在运行（moonlight-common-c 是进程级单例）";
    }
    return outcome;
  }
  // 从这一行起独占串流槽；下面的 return 都走这个 unique_lock 的析构。
  std::unique_lock<std::timed_mutex> slot(g_streamMutex, std::adopt_lock);

  std::unique_ptr<SunshineVideoSink> video(SunshineVideoSink::create());
  std::unique_ptr<SunshineAudioSink> audio(SunshineAudioSink::create());

  MediaContext& media = g_media;
  media.host = &host;
  media.generation = generation;
  media.video = video.get();
  media.audio = audio.get();
  media.started.store(false);
  media.terminated.store(false);
  media.terminationCode.store(0);
  media.failedStage.store(0);
  media.failedStageError.store(0);
  media.input.reset();
  {
    std::lock_guard<std::mutex> lock(media.textMutex);
    media.terminationText.clear();
  }

  SunshineFrameSink frameSink;
  frameSink.user = const_cast<SunshineHost*>(&host);
  frameSink.resize = [](void* user, int frameGeneration, int width, int height) {
    const SunshineHost* sinkHost = static_cast<const SunshineHost*>(user);
    if (sinkHost->resizeFrame != nullptr)
      sinkHost->resizeFrame(sinkHost->user, frameGeneration, width, height);
  };
  frameSink.commit = [](void* user, int frameGeneration,
                        const SunshineRect* rects, size_t count, int width,
                        int height) {
    const SunshineHost* sinkHost = static_cast<const SunshineHost*>(user);
    if (sinkHost->commitFrame != nullptr)
      sinkHost->commitFrame(sinkHost->user, frameGeneration, rects, count,
                            width, height);
  };
  video->bindFrameSink(frameSink, generation);

  const int videoFormats = chooseVideoFormats(options, static_cast<uint32_t>(codecSupport));
  if (videoFormats == 0) {
    outcome.text = "没有可用的视频格式（设备无 H.264/HEVC 硬解且未指定 sunshineFormats）";
    return outcome;
  }

  SERVER_INFORMATION serverInfo;
  LiInitializeServerInformation(&serverInfo);
  serverInfo.address = options.host.c_str();
  serverInfo.serverInfoAppVersion = appVersion.c_str();
  serverInfo.serverInfoGfeVersion = gfeVersion.empty() ? nullptr : gfeVersion.c_str();
  serverInfo.rtspSessionUrl = sessionUrl.c_str();
  serverInfo.serverCodecModeSupport = codecSupport;

  STREAM_CONFIGURATION streamConfig;
  LiInitializeStreamConfiguration(&streamConfig);
  streamConfig.width = options.width;
  streamConfig.height = options.height;
  streamConfig.fps = options.fps;
  streamConfig.bitrate = options.bitrate;
  // moonlight-qt：1392 字节视频包，远端/局域网由 common-c 的 STREAM_CFG_AUTO 判定。
  streamConfig.packetSize = 1392;
  streamConfig.streamingRemotely = STREAM_CFG_AUTO;
  streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  streamConfig.supportedVideoFormats = videoFormats;
  streamConfig.clientRefreshRateX100 = 0;
  streamConfig.colorSpace = COLORSPACE_REC_709;
  streamConfig.colorRange = COLOR_RANGE_LIMITED;
  streamConfig.encryptionFlags = options.encryption ? ENCFLG_ALL : ENCFLG_NONE;
  std::memcpy(streamConfig.remoteInputAesKey, key, sizeof(streamConfig.remoteInputAesKey));
  std::memcpy(streamConfig.remoteInputAesIv, iv, sizeof(streamConfig.remoteInputAesIv));

  DECODER_RENDERER_CALLBACKS videoCallbacks;
  LiInitializeVideoCallbacks(&videoCallbacks);
  videoCallbacks.setup = videoSetupCallback;
  videoCallbacks.submitDecodeUnit = videoSubmitCallback;
  videoCallbacks.stop = videoStopCallback;
  // 硬件解码器是异步出帧的（submitDecodeUnit 只是把解码单元交给 AVCodec），
  // 但 common-c 的同步回调语义仍然成立：返回 DR_OK 表示这一帧已经收下，像素在
  // AVCodec 输出回调线程里经 SunshineVideoSink 交给宿主。capabilities=0 表示
  // 不用拉模式；输入队列有界（满了返回 DR_NEED_IDR），所以不做参考帧失效。
  videoCallbacks.capabilities = 0;

  AUDIO_RENDERER_CALLBACKS audioCallbacks;
  LiInitializeAudioCallbacks(&audioCallbacks);
  audioCallbacks.init = audioInitCallback;
  audioCallbacks.decodeAndPlaySample = audioDecodeCallback;
  audioCallbacks.stop = audioStopCallback;
  // 音频实现按 opusConfig->samplesPerFrame 解码，能处理任意帧长，所以声明
  // CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION：服务端可以根据网络状况调整
  // Opus 帧长（比如 10ms/20ms 混用）而不必退回到最短帧，音质与码率控制都更好。
  // 刻意不设 CAPABILITY_SLOW_OPUS_DECODER——那一位会让服务端按「解码器很慢」
  // 降级（加长帧时长/牺牲音质），本实现的解码是实时拉模式，不需要也不应该降级。
  audioCallbacks.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;

  CONNECTION_LISTENER_CALLBACKS connectionCallbacks;
  LiInitializeConnectionCallbacks(&connectionCallbacks);
  connectionCallbacks.stageStarting = stageStartingCallback;
  connectionCallbacks.stageComplete = stageCompleteCallback;
  connectionCallbacks.stageFailed = stageFailedCallback;
  connectionCallbacks.connectionStarted = connectionStartedCallback;
  connectionCallbacks.connectionTerminated = connectionTerminatedCallback;
  connectionCallbacks.logMessage = logMessageCallback;
  connectionCallbacks.setHdrMode = setHdrModeCallback;

  logWarn(formatText(
      "sunshine connecting: host=%s, mode=%dx%dx%d, %d kbps, formats=%s, "
      "encryption=%s, appversion=%s",
      options.host.c_str(), options.width, options.height, options.fps,
      options.bitrate, videoFormatsText(videoFormats).c_str(),
      options.encryption ? "all" : "none", appVersion.c_str()));

  media.active.store(true);

  // 看门狗：LiStartConnection 内的握手可能阻塞数秒（RTSP/enet 重试）。common-c
  // 文档给的跨线程中止手段只有 LiInterruptConnection()；LiStopConnection() 由会话
  // 线程在启动返回后自己调用。
  std::atomic<bool> startReturned(false);
  std::thread watchdog;
  try {
    watchdog = std::thread([&host, &startReturned]() {
      while (!startReturned.load()) {
        if (host.stopRequested(host.user)) {
          LiInterruptConnection();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
  } catch (...) {
    // No watchdog means a stop could not interrupt a blocked handshake, so do
    // not start at all. Clearing active first keeps late callbacks away from
    // the sinks that are about to be released.
    media.active.store(false);
    outcome.text = "无法创建 Sunshine 串流看门狗线程";
    return outcome;
  }

  const int startResult = LiStartConnection(
      &serverInfo, &streamConfig, &connectionCallbacks, &videoCallbacks,
      &audioCallbacks, nullptr, 0, nullptr, 0);
  startReturned.store(true);
  if (watchdog.joinable()) watchdog.join();

  if (startResult != 0) {
    // 启动失败：LiStartConnection 的 Cleanup 已经调过一次 LiStopConnection()，
    // 再调一次是空操作（stage 已回到 0），与实验客户端一致。
    LiStopConnection();
    media.active.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (host.stopRequested(host.user)) {
      outcome.stopped = true;
      return outcome;
    }
    const int failedStage = media.failedStage.load();
    if (failedStage != 0) {
      unsigned int portFlags = LiGetPortFlagsFromStage(failedStage);
      char ports[128];
      ports[0] = '\0';
      LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
      outcome.text = formatText(
          "Sunshine 串流启动失败（阶段「%s」，错误码 %d%s%s）",
          LiGetStageName(failedStage), media.failedStageError.load(),
          ports[0] != '\0' ? "，可能被占用的端口：" : "", ports);
    } else {
      outcome.text = formatText("Sunshine 串流启动失败（LiStartConnection 返回 %d）",
                                startResult);
    }
    return outcome;
  }

  outcome.connected = true;

  // 输入线程：10ms 节拍把共享队列翻译成 Moonlight 输入事件。它只调用宿主的
  // drainInput（取会话 input 锁、换出队列、逐条回调），不碰会话线程的 socket。
  const unsigned appliedRelease = host.prepareInput != nullptr
                                      ? host.prepareInput(host.user)
                                      : 0;
  unsigned applied = appliedRelease;
  SunshineInputSink inputSink;
  inputSink.ctx = &media.input;
  inputSink.pointer = [](void* ctx, int x, int y, int mask) {
    static_cast<InputTranslator*>(ctx)->pointer(x, y, mask);
  };
  inputSink.key = [](void* ctx, int keysym, int pressed) {
    static_cast<InputTranslator*>(ctx)->key(keysym, pressed);
  };
  inputSink.clipboard = [](void* ctx, const char* text) {
    static_cast<InputTranslator*>(ctx)->clipboard(text);
  };
  inputSink.release = [](void* ctx) { static_cast<InputTranslator*>(ctx)->release(); };

  std::atomic<bool> inputDone(false);
  std::thread inputThread;
  try {
    inputThread = std::thread([&host, &media, &inputSink, &applied, &inputDone]() {
      int ticks = 0;
      while (!inputDone.load()) {
        if (host.stopRequested(host.user) || media.terminated.load()) break;
        if (host.drainInput != nullptr)
          host.drainInput(host.user, &inputSink, &applied);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // 每 5 秒打一次媒体计数（只读计数，不含任何画面内容）。
        if (++ticks % 500 == 0 && media.video != nullptr) {
          const SunshineVideoStats videoStats = media.video->stats();
          const SunshineAudioStats audioStats =
              media.audio != nullptr ? media.audio->stats() : SunshineAudioStats();
          logDebug(formatText(
              "sunshine media: frames=%llu (idr %llu), video bytes=%llu, audio "
              "packets=%llu, audio bytes=%llu",
              videoStats.frames, videoStats.idrFrames, videoStats.bytes,
              audioStats.packets, audioStats.bytes));
        }
      }
    });
  } catch (...) {
    // Degraded but safe: keep the stream, drop the input for this attempt. The
    // stream slot and every sink stay owned by this thread, so nothing dangles.
    logWarn("sunshine: cannot create the input thread; input disabled for this stream");
  }

  // 连接期间的等待循环：停止、对端终止或换代（手动重连）三者之一。
  while (!host.stopRequested(host.user) && !media.terminated.load()) {
    const int currentGeneration =
        host.generation != nullptr ? host.generation(host.user) : generation;
    if (currentGeneration != generation) {
      outcome.restart = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  inputDone.store(true);
  if (inputThread.joinable()) inputThread.join();

  // stop 路径：会话线程自己拆流（LiStopConnection 会 LiInterruptConnection()、
  // 停并 join 视频/音频线程），随后宿主才允许释放本对象。
  LiStopConnection();
  media.active.store(false);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  outcome.stopped = host.stopRequested(host.user);
  {
    std::lock_guard<std::mutex> lock(media.textMutex);
    outcome.text = media.terminationText;
  }
  if (outcome.text.empty())
    outcome.text = "串流已结束";
  return outcome;
}

}  // namespace

// Small helper so the many phase transitions can pass std::string messages
// without scattering .c_str() at every call site (RdpHost::setPhase takes a
// const char* like the rest of the host contract).
void setHostPhase(const SunshineHost& host, int phase, const std::string& error,
                  const char* errorCode, int retryInMs) {
  host.setPhase(host.user, phase, error.c_str(), errorCode, retryInMs);
}

void runSunshineSession(const SunshineHost& host, const SunshineOptions& options) {
  // 缺少状态目录属于配置错误：会话进入 failed/protocol 并等用户重连，绝不落到 VNC
  // 循环上（端口和握手完全不同）。
  if (options.stateDir.empty()) {
    host.setPhase(host.user, SunshinePhaseFailed,
                  "缺少 Sunshine 状态目录（sunshineStateDir，须传应用沙箱绝对路径）",
                  "protocol", 0);
    host.waitForReconnect(host.user);
    return;
  }

  int lastApplied = 0;
  int backoffMs = 1000;
  int transportPairFailures = 0;

  while (!host.stopRequested(host.user)) {
    const int generation = host.beginAttempt(host.user, &lastApplied);
    host.setPhase(host.user, SunshinePhaseConnecting, "", "none", 0);

    try {
      // 1. 客户端身份（证书/私钥/uniqueid），缺失时生成并落盘。
      const tg::sunshine::Identity identity =
          tg::sunshine::load_or_create_identity(options.stateDir);
      logDebug(formatText("sunshine identity ready (created_now=%d)",
                          identity.created_now ? 1 : 0));

      // 2. nvhttp 客户端：http_port = 会话端口；https_port 先按端口-5 猜，
      //    /serverinfo 报的 HttpsPort 与之不同时以服务端为准重建一次。
      uint16_t httpsPort =
          options.port > 5 ? static_cast<uint16_t>(options.port - 5) : 0;
      std::unique_ptr<tg::sunshine::NvHttpClient> client(
          new tg::sunshine::NvHttpClient(makeClientOptions(options, httpsPort)));

      // 3. /serverinfo。PinMismatchError 直接抛给外层（failed/fingerprint-mismatch）。
      tg::sunshine::ServerInfoProbe probe = client->get_server_info();
      tg::sunshine::ServerInfo info = probe.server;
      if (!probe.http_ok && !probe.https_ok) {
        const std::string detail =
            !probe.http_error.empty() ? probe.http_error : probe.https_error;
        setHostPhase(host, SunshinePhaseReconnecting,
                     "无法连接 Sunshine 服务端：" + detail, "network", backoffMs);
        if (!host.sleepUnlessStopped(host.user, backoffMs)) {
          if (host.stopRequested(host.user)) break;
        }
        backoffMs = std::min(backoffMs * 2, 4000);
        continue;
      }
      if (info.https_port != 0 &&
          static_cast<uint16_t>(info.https_port) != httpsPort &&
          !probe.https_ok) {
        // 服务端在别的 HTTPS 端口上：按它公布的值重建客户端并重新探测。
        httpsPort = static_cast<uint16_t>(info.https_port);
        client.reset(
            new tg::sunshine::NvHttpClient(makeClientOptions(options, httpsPort)));
        probe = client->get_server_info();
        info = probe.server;
      }
      if (info.appversion.empty()) {
        host.setPhase(host.user, SunshinePhaseFailed,
                      "服务端 /serverinfo 缺少 appversion，无法建立 Moonlight 会话",
                      "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      if (host.setServerAppVersion != nullptr)
        host.setServerAppVersion(host.user, generation, info.appversion.c_str());

      // 4. 配对：未配对时进入 pairing-required 等 UI 提交 PIN。
      if (info.pair_status != 1) {
        bool paired = false;
        bool terminal = false;
        // Enter pairing-required once. A failed pair re-enters the wait with its
        // pair-failed error text set by the catch below; the top of the loop
        // must not overwrite that (otherwise the UI would never see it).
        host.setPhase(host.user, SunshinePhasePairingRequired, "", "none", 0);
        while (!paired) {
          if (host.stopRequested(host.user)) break;
          std::string pin;
          if (!host.waitForPin(host.user, generation, &pin)) {
            // stop 或手动换代：回到外层重新判断。
            wipeString(pin);
            break;
          }
          host.setPhase(host.user, SunshinePhaseConnecting, "正在配对", "none", 0);
          try {
            client->pair(pin, "Tianguan", pairProgress);
            paired = true;
            transportPairFailures = 0;
            logWarn("sunshine pairing succeeded");
          } catch (const tg::sunshine::PairError& error) {
            const bool transportFailure = error.http_status() == 0;
            if (transportFailure) {
              ++transportPairFailures;
            } else {
              transportPairFailures = 0;
            }
            const std::string message =
                std::string("配对失败：") + error.what();
            if (transportFailure && transportPairFailures >= 2) {
              setHostPhase(host, SunshinePhaseFailed, message, "network", 0);
              host.waitForReconnect(host.user);
              terminal = true;
            } else {
              // PIN 错误 / 网页端超时：回到 pairing-required，让用户重新输入。
              setHostPhase(host, SunshinePhasePairingRequired, message,
                         "pair-failed", 0);
            }
          }
          wipeString(pin);
          if (terminal) break;
        }
        if (terminal) continue;
        if (!paired) {
          if (host.stopRequested(host.user)) break;
          continue;  // 手动重连换代
        }
      }

      if (info.server_codec_mode_support == 0) {
        // 没有编解码能力位时 common-c 的 LiStartConnection 会直接失败；在
        // /launch 之前就报出来，避免无意义地启动服务端应用。
        host.setPhase(host.user, SunshinePhaseFailed,
                      "服务端 /serverinfo 未声明 ServerCodecModeSupport，无法协商视频格式",
                      "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }

      // 5. /applist：标题不区分大小写包含匹配，找不到用第一个。
      const std::vector<tg::sunshine::AppInfo> apps = client->get_app_list();
      const tg::sunshine::AppInfo* app = pickApp(apps, options.app);
      if (app == nullptr) {
        host.setPhase(host.user, SunshinePhaseFailed,
                      "服务端应用列表为空，无法启动 Sunshine 应用", "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      if (host.setAppTitle != nullptr)
        host.setAppTitle(host.user, generation, app->title.c_str());

      // 6. /launch（mode=WxHxFPS，服务端生成 rikey/rikeyid 与 sessionUrl0）。
      //    先处理服务端的残留会话：BUSY 且就是目标应用 -> /resume 接管续播；
      //    BUSY 是别的应用 -> /cancel、等空闲（500ms/最多 10s）再 /launch。
      tg::sunshine::LaunchOptions launchOptions;
      launchOptions.app_id = app->id;
      launchOptions.width = options.width;
      launchOptions.height = options.height;
      launchOptions.fps = options.fps;

      // info 是配对前那次探测的快照，忙时重新读一次再决策，避免拿过期状态去
      // /resume 一个其实已经结束的会话。
      if (serverBusy(info)) {
        try {
          info = client->get_server_info().server;
        } catch (const tg::sunshine::PinMismatchError&) {
          throw;
        } catch (const std::exception& error) {
          logDebug(std::string("sunshine serverinfo refresh failed (ignored): ") +
                   error.what());
        }
      }

      bool resumed = serverBusy(info) && info.current_game == app->id;
      tg::sunshine::SessionLaunch launch;
      try {
        if (resumed) {
          // 同一个应用还在跑：/resume 接管续播，参数与 /launch 相同。
          launch = client->resume(launchOptions);
        } else if (serverBusy(info)) {
          if (!cancelAndStartApp(*client, host, generation, launchOptions, app->id,
                                 info, &launch, &resumed)) {
            if (host.stopRequested(host.user)) break;
            continue;  // 手动换代
          }
        } else {
          launch = client->launch(launchOptions);
        }
      } catch (const tg::sunshine::NvError& error) {
        // 兜底：state 标记与实际不一致时 /launch 仍可能 400 "already running"。
        // 只重试一次（cancel -> 等空闲 -> launch/resume）。
        if (resumed || !launchAlreadyRunning(error)) throw;
        logWarn("sunshine: launch reported an app already running; cancelling and "
                "retrying once");
        if (!cancelAndStartApp(*client, host, generation, launchOptions, app->id,
                               info, &launch, &resumed)) {
          if (host.stopRequested(host.user)) break;
          continue;  // 手动换代
        }
      }
      logWarn(formatText("sunshine launch: app=\"%s\" (id %d), resumed=%d, sessionUrl set=%d",
                         app->title.c_str(), app->id, resumed ? 1 : 0,
                         launch.session_url0.empty() ? 0 : 1));

      // 7. 串流（阻塞到终止 / 停止 / 换代）。
      const MediaOutcome outcome =
          runMediaStream(host, options, info.appversion, info.gfe_version,
                         static_cast<int>(info.server_codec_mode_support),
                         launch.session_url0, launch.rikey, launch.rikeyid,
                         generation);
      backoffMs = 1000;
      // 只要本次尝试走到过 /launch，停止、换代、对端终止、启动失败都尽力 /cancel：
      // 服务端上残留的会话会让下一次 /launch 直接 400 "already running"。
      // closeSession() 只置 stop 并唤醒本线程，cancel 仍由会话线程在这里发出；
      // 进程被杀来不及发的情况由连接时的残留会话处理兜底（见 cancelAndStartApp）。
      cancelServerSession(*client);
      if (outcome.stopped) break;
      if (outcome.restart) continue;

      if (!outcome.connected) {
        // 启动失败：网络类退避重试；stop 已在 outcome 里。
        setHostPhase(host, SunshinePhaseReconnecting,
                     outcome.text.empty() ? std::string("Sunshine 串流启动失败") : outcome.text,
                     "network", backoffMs);
        if (!host.sleepUnlessStopped(host.user, backoffMs)) {
          if (host.stopRequested(host.user)) break;
        }
        backoffMs = std::min(backoffMs * 2, 4000);
        continue;
      }

      // 连接建立后被终止（对端结束/网络断开）：/cancel 已在上面发过，这里退避
      // 重连（重连会重新走 /serverinfo -> /launch，必要时 /resume 续播）。
      setHostPhase(host, SunshinePhaseReconnecting, outcome.text, "network",
                   backoffMs);
      if (!host.sleepUnlessStopped(host.user, backoffMs)) {
        if (host.stopRequested(host.user)) break;
      }
      backoffMs = std::min(backoffMs * 2, 4000);
    } catch (const tg::sunshine::PinMismatchError& error) {
      // 已固定的服务端证书与本次 TLS 握手不一致：拒绝连接，要求重新配对。
      setHostPhase(
          host, SunshinePhaseFailed,
          std::string("服务端证书指纹与已配对记录不一致，需要重新配对（") +
              error.what() + "）",
          "fingerprint-mismatch", 0);
      host.waitForReconnect(host.user);
    } catch (const tg::sunshine::PairError& error) {
      // 配对循环内部已归类；这里只是兜底，避免把异常漏成 unknown。
      setHostPhase(host, SunshinePhaseReconnecting,
                   std::string("配对失败：") + error.what(), "pair-failed",
                   backoffMs);
      if (!host.sleepUnlessStopped(host.user, backoffMs)) {
        if (host.stopRequested(host.user)) break;
      }
      backoffMs = std::min(backoffMs * 2, 4000);
    } catch (const tg::sunshine::NvError& error) {
      const std::string message = sanitizeLine(error.what());
      logWarn("sunshine nvhttp error: " + message);
      if (error.http_status() == 0) {
        setHostPhase(host, SunshinePhaseReconnecting, message, "network",
                     backoffMs);
        if (!host.sleepUnlessStopped(host.user, backoffMs)) {
          if (host.stopRequested(host.user)) break;
        }
        backoffMs = std::min(backoffMs * 2, 4000);
      } else {
        setHostPhase(host, SunshinePhaseFailed, message, "protocol", 0);
        host.waitForReconnect(host.user);
      }
    } catch (const std::exception& error) {
      const std::string message = formatText("Sunshine 会话错误：%s", error.what());
      logWarn(message);
      setHostPhase(host, SunshinePhaseReconnecting, message, "network",
                   backoffMs);
      if (!host.sleepUnlessStopped(host.user, backoffMs)) {
        if (host.stopRequested(host.user)) break;
      }
      backoffMs = std::min(backoffMs * 2, 4000);
    }
  }

  // 终态不在这里写 phase：宿主的适配层按和 RDP 完全相同的规则收尾
  // （failed / trust-required 保持原样，其余收成 disconnected）。
  {
    std::lock_guard<std::mutex> lock(g_media.textMutex);
    g_media.terminationText.clear();
  }
}

}  // namespace tianguan
