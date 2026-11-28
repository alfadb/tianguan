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
#include <cmath>
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

#include "render.h"
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

// 单调时钟毫秒：只用于计算「多久没有非黑帧 / 多久没有帧」这类时长，不受系统
// 时间调整影响。与 VNC/RDP 的时长统计同一口径。
unsigned long long monotonicMillis() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

// 墙钟时间（真机日志对照用）：HH:MM:SS.mmm。
std::string wallClockText() {
  struct timespec now;
  ::clock_gettime(CLOCK_REALTIME, &now);
  struct tm local;
  ::localtime_r(&now.tv_sec, &local);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour,
                local.tm_min, local.tm_sec,
                static_cast<int>(now.tv_nsec / 1000000));
  return std::string(buffer);
}

const char* phaseLabel(int phase) {
  switch (phase) {
    case SunshinePhaseConnecting: return "connecting";
    case SunshinePhaseConnected: return "connected";
    case SunshinePhaseDisconnected: return "disconnected";
    case SunshinePhaseFailed: return "failed";
    case SunshinePhaseTrustRequired: return "trust-required";
    case SunshinePhaseReconnecting: return "reconnecting";
    case SunshinePhasePairingRequired: return "pairing-required";
    default: return "(none)";
  }
}

// 上一次上报的阶段，只用于「每次 phase 变化都打一条 W 级日志」。
std::atomic<int> g_lastPhase(-1);

void logPhaseTransition(int phase, const std::string& error, const char* errorCode,
                        int retryInMs) {
  const int previous = g_lastPhase.exchange(phase);
  if (previous == phase) return;
  logWarn(formatText("sunshine phase %s -> %s at %s (retryInMs=%d, errorCode=%s)%s%s",
                     phaseLabel(previous), phaseLabel(phase),
                     wallClockText().c_str(), retryInMs,
                     errorCode != nullptr ? errorCode : "none",
                     error.empty() ? "" : ", error: ", error.c_str()));
}

// 所有阶段上报都走这里：先 W 级记一条带时间的转换日志，再写宿主状态。
// （真机上 D 级不可见，没有这条日志就无法把 UI 显示时间和终止/重连时刻对上。）
void setHostPhase(const SunshineHost& host, int phase, const std::string& error,
                  const char* errorCode, int retryInMs) {
  logPhaseTransition(phase, error, errorCode, retryInMs);
  host.setPhase(host.user, phase, error.c_str(), errorCode, retryInMs);
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
// 输入延迟窗口（5 秒）：从「入队」到「LiSend* 返回」的间隔，以及发送次数/失败数。
// 只在后端输入线程上读写（drain 适配层在 sink 调用返回后调用 noteLag()），所以
// 不需要锁；窗口满后由 takeStats() 一次性取走并清零。
struct InputWindowStats {
  InputWindowStats() : avgMs(0), p95Ms(0), samples(0), sendCalls(0), sendFail(0) {}
  double avgMs;
  double p95Ms;
  size_t samples;
  unsigned long long sendCalls;
  unsigned long long sendFail;
};

// ---------------------------------------------------------------------------
// 手柄透传（单只手柄，controller number 0 / mask 0x0001）
//
// 输入 API（LiSend*）是线程安全的、内部排队，所以 NAPI 线程直接调用即可；这里
// 只用一把锁保护「是否已 arrival / 上次状态 / 5 秒窗口计数」。状态按串流尝试
// 复位：会话停止/断开/换代时若已 arrival，会话线程补一条 mask=0 的空事件移除。
// ---------------------------------------------------------------------------
struct GamepadState {
  std::mutex mutex;
  bool arrived = false;
  bool hasLast = false;
  uint32_t lastButtons = 0;
  uint8_t lastLeftTrigger = 0;
  uint8_t lastRightTrigger = 0;
  int lastLeftX = 0;
  int lastLeftY = 0;
  int lastRightX = 0;
  int lastRightY = 0;
  unsigned long long updates = 0;  // 5 秒窗口内的状态更新次数
};
GamepadState g_gamepad;

// arrival 声明的按键与能力：A/B/X/Y、方向、LB/RB、Play/Back、LS/RS 按下、Special。
const uint32_t kGamepadArrivalButtons =
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG |
    RIGHT_FLAG | LB_FLAG | RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG |
    RS_CLK_FLAG | SPECIAL_FLAG;
const uint16_t kGamepadArrivalCapabilities =
    static_cast<uint16_t>(LI_CCAP_ANALOG_TRIGGERS | LI_CCAP_RUMBLE);

// 震动回调日志限频（鸿蒙没有传给手柄的震动通道，只记录）：最多每 2 秒一行。
std::atomic<unsigned long long> g_lastRumbleLogMs(0);
std::atomic<unsigned long long> g_rumbleDropped(0);

void logRumbleThrottled(const std::string& line) {
  const unsigned long long now = monotonicMillis();
  const unsigned long long last = g_lastRumbleLogMs.load();
  if (last != 0 && now - last < 2000) {
    g_rumbleDropped.fetch_add(1);
    return;
  }
  g_lastRumbleLogMs.store(now);
  const unsigned long long dropped = g_rumbleDropped.exchange(0);
  logWarn(dropped > 0 ? formatText("%s (+%llu similar suppressed)", line.c_str(),
                                   dropped)
                      : line);
}

// 鼠标按键窗口统计（5 秒）：按下/抬起/合成抬起/纯移动事件的次数。只读日志用，
// 不参与任何输入逻辑。
struct MouseWindowStats {
  MouseWindowStats()
      : press(0), release(0), releaseSynth(0), moveEvents(0) {}
  unsigned long long press;
  unsigned long long release;
  unsigned long long releaseSynth;
  unsigned long long moveEvents;
};

class InputTranslator {
 public:
  InputTranslator()
      : streamWidth_(1280), streamHeight_(720), pointerMask_(0), modifiers_(0),
        dropLogs_(0), lastX_(0), lastY_(0), lagCount_(0), lagSumUs_(0),
        sendCalls_(0), sendFail_(0), lastButtonEventMs_(0), buttonLogSecond_(0),
        buttonLogLines_(0), buttonLogSkipped_(0), pressCount_(0), releaseCount_(0),
        releaseSynthCount_(0), moveEventCount_(0) {}

  // 一条「入队 -> 发送返回」的延迟样本（微秒）。只在输入线程调用。
  void noteLag(unsigned long long lagUs) {
    if (lagCount_ < kLagSamples) lagUs_[lagCount_] = lagUs;
    if (lagCount_ < kLagSamples) ++lagCount_;
    lagSumUs_ += lagUs;
  }

  // 取走并清空当前 5 秒窗口；avg/p95 以毫秒返回。
  InputWindowStats takeStats() {
    InputWindowStats stats;
    stats.samples = lagCount_;
    stats.sendCalls = sendCalls_;
    stats.sendFail = sendFail_;
    if (lagCount_ > 0) {
      stats.avgMs = static_cast<double>(lagSumUs_) / lagCount_ / 1000.0;
      std::vector<unsigned long long> ordered(lagUs_, lagUs_ + lagCount_);
      std::sort(ordered.begin(), ordered.end());
      const size_t index =
          static_cast<size_t>((static_cast<double>(ordered.size() - 1)) * 0.95);
      stats.p95Ms = static_cast<double>(ordered[index]) / 1000.0;
    }
    lagCount_ = 0;
    lagSumUs_ = 0;
    sendCalls_ = 0;
    sendFail_ = 0;
    return stats;
  }

  // 取走并清空鼠标按键窗口（5 秒汇总日志用）。
  MouseWindowStats takeMouseStats() {
    MouseWindowStats stats;
    stats.press = pressCount_;
    stats.release = releaseCount_;
    stats.releaseSynth = releaseSynthCount_;
    stats.moveEvents = moveEventCount_;
    pressCount_ = 0;
    releaseCount_ = 0;
    releaseSynthCount_ = 0;
    moveEventCount_ = 0;
    return stats;
  }

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
    // 绝对坐标：x/y 已是远端像素，参考尺寸取「当前流」的最新尺寸（setStreamSize()
    // 在每次视频 setup 里更新，分辨率调整后自然跟随）。
    noteSend(LiSendMousePositionEvent(static_cast<short>(x), static_cast<short>(y),
                                      static_cast<short>(width),
                                      static_cast<short>(height)));
    lastX_ = x;
    lastY_ = y;
    const int held = mask & 7;
    const int changed = held ^ (pointerMask_ & 7);
    if ((changed & 1) != 0)
      buttonEvent("LEFT", (held & 1) != 0, x, y, "mask",
                  LiSendMouseButtonEvent((held & 1) != 0 ? BUTTON_ACTION_PRESS
                                                        : BUTTON_ACTION_RELEASE,
                                         BUTTON_LEFT));
    if ((changed & 2) != 0)
      buttonEvent("MIDDLE", (held & 2) != 0, x, y, "mask",
                  LiSendMouseButtonEvent((held & 2) != 0 ? BUTTON_ACTION_PRESS
                                                        : BUTTON_ACTION_RELEASE,
                                         BUTTON_MIDDLE));
    if ((changed & 4) != 0)
      buttonEvent("RIGHT", (held & 4) != 0, x, y, "mask",
                  LiSendMouseButtonEvent((held & 4) != 0 ? BUTTON_ACTION_PRESS
                                                        : BUTTON_ACTION_RELEASE,
                                         BUTTON_RIGHT));
    if (changed == 0 && (mask & 0x18) == 0) ++moveEventCount_;
    pointerMask_ = held;
    // 一个 mask 最多带一个方向的滚轮位；两个都有时按上滚处理（与 RDP 路径一致）。
    if ((mask & 8) != 0)
      noteSend(LiSendScrollEvent(1));
    else if ((mask & 16) != 0)
      noteSend(LiSendScrollEvent(-1));
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
      noteSend(LiSendKeyboardEvent(vk, KEY_ACTION_DOWN, modifiers_));
      pressedKeys_.insert(vk);
      if (bit != 0) modifiers_ = static_cast<char>(modifiers_ | bit);
    } else {
      if (bit != 0) modifiers_ = static_cast<char>(modifiers_ & ~bit);
      noteSend(LiSendKeyboardEvent(vk, KEY_ACTION_UP, modifiers_));
      pressedKeys_.erase(vk);
    }
  }

  void release() {
    // 合成所有已按下键的抬起；修饰键先把自己的位摘掉再发，和 key() 的规则一致。
    for (std::set<short>::const_iterator it = pressedKeys_.begin();
         it != pressedKeys_.end(); ++it) {
      const char bit = modifierBitForVk(*it);
      const char mods = bit != 0 ? static_cast<char>(modifiers_ & ~bit) : modifiers_;
      noteSend(LiSendKeyboardEvent(*it, KEY_ACTION_UP, mods));
    }
    pressedKeys_.clear();
    modifiers_ = 0;
    const int held = pointerMask_ & 7;
    if ((held & 1) != 0)
      buttonEvent("LEFT", false, lastX_, lastY_, "release-synth",
                  LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT));
    if ((held & 2) != 0)
      buttonEvent("MIDDLE", false, lastX_, lastY_, "release-synth",
                  LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MIDDLE));
    if ((held & 4) != 0)
      buttonEvent("RIGHT", false, lastX_, lastY_, "release-synth",
                  LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT));
    pointerMask_ = 0;
  }

  // Moonlight 协议没有剪贴板通道：setClipboard 的文本出队后直接丢弃。
  void clipboard(const char*) {}

 private:
  // 每次 LiSend* 调用都计数；返回非 0（如 -2 输入流未就绪）单独累计，便于在
  // W 级日志里区分「没在发」和「发了但没被接收」。
  void noteSend(int result) {
    ++sendCalls_;
    if (result != 0) ++sendFail_;
  }

  // 鼠标按键状态变化日志（W 级，每秒最多 40 行；超出的只计数，在下一行末尾汇总）。
  // 只观察，不影响发送：result 已经由调用处的 LiSendMouseButtonEvent 返回。
  void buttonEvent(const char* button, bool press, int x, int y,
                   const char* source, int result) {
    noteSend(result);
    // release() 合成（releaseEpoch/失焦）的抬起单独计数，便于和真实 mask 变化区分。
    if (press) {
      ++pressCount_;
    } else {
      ++releaseCount_;
      if (std::strcmp(source, "release-synth") == 0) ++releaseSynthCount_;
    }
    const unsigned long long now = monotonicMillis();
    const unsigned long long dt =
        lastButtonEventMs_ == 0 ? 0 : now - lastButtonEventMs_;
    lastButtonEventMs_ = now;
    if (now / 1000 != buttonLogSecond_) {
      buttonLogSecond_ = now / 1000;
      buttonLogLines_ = 0;
    }
    if (buttonLogLines_ >= 40) {
      ++buttonLogSkipped_;
      return;
    }
    ++buttonLogLines_;
    std::string suffix;
    if (buttonLogSkipped_ > 0) {
      suffix = formatText(" (+%llu skipped)", buttonLogSkipped_);
      buttonLogSkipped_ = 0;
    }
    logWarn(formatText(
        "sunshine mouse: %s %s at (%d,%d) dt=%llums src=%s%s%s", button,
        press ? "press" : "release", x, y, dt, source,
        result != 0 ? " send_failed" : "", suffix.c_str()));
  }

  static const size_t kLagSamples = 4096;
  std::atomic<int> streamWidth_;
  std::atomic<int> streamHeight_;
  std::set<short> pressedKeys_;
  int pointerMask_;
  char modifiers_;
  int dropLogs_;
  int lastX_;
  int lastY_;
  unsigned long long lagUs_[kLagSamples];
  size_t lagCount_;
  unsigned long long lagSumUs_;
  unsigned long long sendCalls_;
  unsigned long long sendFail_;
  unsigned long long lastButtonEventMs_;
  unsigned long long buttonLogSecond_;
  int buttonLogLines_;
  unsigned long long buttonLogSkipped_;
  unsigned long long pressCount_;
  unsigned long long releaseCount_;
  unsigned long long releaseSynthCount_;
  unsigned long long moveEventCount_;
};

// ---------------------------------------------------------------------------
// 进程级串流状态（moonlight-common-c 是进程级单例，回调没有 context）
// ---------------------------------------------------------------------------
struct MediaContext {
  MediaContext()
      : host(nullptr), generation(0), video(nullptr), audio(nullptr),
        active(false), started(false), terminated(false), terminationCode(0),
        failedStage(0), failedStageError(0), pictureFrameMs(0),
        pictureBlackStartMs(0), pictureFirstNonBlackMs(0),
        pictureLastNonBlackMs(0), terminationPictureMs(0), currentStage(0),
        firstFrameTimeoutLogged(false), negotiatedVideoFormat(0), rttAvgMs(0),
        rttMinMs(0), rttValid(false) {}

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
  // 画面全黑检测（见 sunshine_backend.h）：视频解码回调线程只写这两个时间戳，
  // 状态轮询线程读取后换算成 blackMs / lastFrameAgeMs，采样路径不加锁。
  //   pictureFrameMs        最近一帧解码完成时刻（0 = 本次尝试还没有帧）；
  //   pictureBlackStartMs   本次尝试 video setup 的时刻：从未出现非黑帧时
  //                         「连续全黑/无帧」从这里起算；
  //   pictureFirstNonBlackMs 本次尝试首个非黑帧的时刻（0 = 还没有非黑帧）；
  //   pictureLastNonBlackMs  最近一次非黑帧的时刻（0 = 本次尝试还没有非黑帧）。
  std::atomic<unsigned long long> pictureFrameMs;
  std::atomic<unsigned long long> pictureBlackStartMs;
  std::atomic<unsigned long long> pictureFirstNonBlackMs;
  std::atomic<unsigned long long> pictureLastNonBlackMs;
  // 终止那一刻的「本次会话正常画面持续时长」快照（最近非黑帧 - 首个非黑帧；
  // 0 = 从未有画面）。connectionTerminated() 里写入，会话线程据此区分
  // 「瞬时黑屏（主机显示会话变化）」和「用户已正常观看后的主机主动结束」。
  std::atomic<unsigned long long> terminationPictureMs;
  // 「launch 后 5 秒还没有首帧」的诊断：当前初始化阶段（stageStarting 更新）与
  // 只打一次的标记，便于判断慢在 RTSP/控制流/视频哪一段。
  std::atomic<int> currentStage;
  std::atomic<bool> firstFrameTimeoutLogged;
  // 当前流实际协商到的 VIDEO_FORMAT_*（videoSetup 成功时写入）；分辨率调整时的
  // 码率系数按它判断 HEVC。0 = 还没有协商过。
  std::atomic<int> negotiatedVideoFormat;
  // 网络往返时延（common-c 的 ENet 统计，LiGetEstimatedRttInfo）：平均 RTT 与
  // 「本窗口最小估计」（avg - variance，接口没有单独的 min 字段）。只在
  // LiStartConnection..LiStopConnection 之间的输入线程里采样，NAPI 线程只读。
  std::atomic<unsigned int> rttAvgMs;
  std::atomic<unsigned int> rttMinMs;
  std::atomic<bool> rttValid;

  std::mutex textMutex;
  std::string terminationText;
};

// 同一时刻只允许一个串流：common-c 的全局状态（流对象、stage、回调）是单例。
std::timed_mutex g_streamMutex;
MediaContext g_media;
// 本次会话线程已经执行的「主机结束串流且无画面」自动重连次数。会话线程写、
// 输入线程的限频媒体统计读（所以用原子量）；0 表示当前没有在自动重连。
std::atomic<int> g_autoReconnects(0);

// ---------------------------------------------------------------------------
// common-c 回调
// ---------------------------------------------------------------------------
int videoSetupCallback(int videoFormat, int width, int height, int redrawRate,
                       void* context, int flags) {
  (void)context;
  (void)flags;
  MediaContext& media = g_media;
  if (!media.active.load() || media.video == nullptr) return -1;
  // 黑屏计时从本次视频 setup 开始：首帧迟迟不来也要能被判成「没有画面」。
  // 之后每帧由视频 sink 在解码回调线程里更新（见 sunshineNoteVideoLuma）。
  media.pictureFrameMs.store(0);
  media.pictureFirstNonBlackMs.store(0);
  media.pictureLastNonBlackMs.store(0);
  media.pictureBlackStartMs.store(monotonicMillis());
  const bool ok = media.video->setup(videoFormat, width, height, redrawRate);
  if (!ok) {
    logWarn(formatText("sunshine video setup failed: format=0x%X %dx%d@%d",
                       videoFormat, width, height, redrawRate));
    return -1;
  }
  // 记录协商到的格式：resize 的码率缩放用它判断 HEVC（H265/Main10 家族）。
  media.negotiatedVideoFormat.store(videoFormat);
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
  g_media.currentStage.store(stage);
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

void rumbleCallback(unsigned short controllerNumber, unsigned short lowFreqMotor,
                    unsigned short highFreqMotor) {
  if (!g_media.active.load()) return;
  // 鸿蒙没有把手柄震动的通道（Vibrator NDK 不接收 Moonlight 的 rumble 语义），
  // 这里只记录限频日志；能力位仍声明 LI_CCAP_RUMBLE，让主机以为手柄支持震动，
  // 以免某些游戏禁用相关特性。
  logRumbleThrottled(formatText(
      "sunshine gamepad: host rumble controller=%u low=%u high=%u "
      "(no haptic channel on this device; ignored)",
      static_cast<unsigned>(controllerNumber), static_cast<unsigned>(lowFreqMotor),
      static_cast<unsigned>(highFreqMotor)));
}

void rumbleTriggersCallback(uint16_t controllerNumber, uint16_t leftTriggerMotor,
                            uint16_t rightTriggerMotor) {
  if (!g_media.active.load()) return;
  logRumbleThrottled(formatText(
      "sunshine gamepad: host trigger rumble controller=%u left=%u right=%u "
      "(no haptic channel on this device; ignored)",
      static_cast<unsigned>(controllerNumber),
      static_cast<unsigned>(leftTriggerMotor),
      static_cast<unsigned>(rightTriggerMotor)));
}

void connectionStartedCallback(void) {
  MediaContext& media = g_media;
  if (!media.active.load() || media.host == nullptr) return;
  media.started.store(true);
  // 该回调在 LiStartConnection() 内部、会话线程上触发，所以直接写宿主状态是安全的。
  setHostPhase(*media.host, SunshinePhaseConnected, "", "none", 0);
  // 按新尺寸重启的串流已经建立：这是 Sunshine 的唯一正 ack（没有服务端应答）。
  // 没有 in-flight 请求时该调用是空操作。
  if (media.host->finishDesktopSizeRequest != nullptr)
    media.host->finishDesktopSizeRequest(media.host->user, media.generation, 0, 0, true);
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
  // 终止那一刻快照「本次会话正常画面持续了多久」= 最近非黑帧 - 首个非黑帧
  // （0 = 本次会话从未出现非黑帧）。上层据此区分「用户已正常观看后的主机主动
  // 结束」和「主机显示会话状态变化造成的瞬时黑屏（要自动重连）」。
  const unsigned long long firstNonBlack = media.pictureFirstNonBlackMs.load();
  const unsigned long long lastNonBlack = media.pictureLastNonBlackMs.load();
  media.terminationPictureMs.store(
      firstNonBlack != 0 && lastNonBlack >= firstNonBlack
          ? lastNonBlack - firstNonBlack
          : 0);
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

// resize 码率用的编解码系数：以当前流实际协商到的格式为准（H265 / Main10 等
// VIDEO_FORMAT_MASK_H265 内为 HEVC -> 0.7，其余含 H.264 -> 1.0）。首次协商前
// （格式未知）按「自动且设备有 HEVC 硬解」处理，与 chooseVideoFormats() 一致。
bool resizeUsesHevc(const SunshineOptions& options) {
  const int format = g_media.negotiatedVideoFormat.load();
  if (format != 0) return (format & VIDEO_FORMAT_MASK_H265) != 0;
  return options.formats.empty() && hasHevcHardwareDecoder();
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
    const SunshineOptions& options, uint16_t httpsPort, int timeoutMs) {
  tg::sunshine::NvHttpClient::Options clientOptions;
  clientOptions.host = options.host;
  clientOptions.http_port = static_cast<uint16_t>(options.port);
  clientOptions.https_port = httpsPort;
  clientOptions.state_dir = options.stateDir;
  // 首次连接给 10 s（局域网首次 TLS/RTSP 探测可能慢）；重连路径收到 5 s，
  // 失败就重试而不是干等一次超时（真机上曾出现单次请求 10.5 s 的超时）。
  clientOptions.connect_timeout_ms = timeoutMs;
  clientOptions.io_timeout_ms = timeoutMs;
  // getservercert 阶段服务端会挂起到网页端提交 PIN，单独给 5 分钟（不随重连缩短）。
  clientOptions.pin_wait_ms = 300000;
  return clientOptions;
}

void pairProgress(const tg::sunshine::PairStageRecord& stage) {
  if (stage.ok) {
    // 配对是一次性生命周期事件，用 W 级保证真机日志可见（D 级在手机上被过滤）。
    logWarn(formatText("sunshine pair stage %s ok (%lld ms)", stage.name.c_str(),
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
    // 取消失败不致命，但它是重连流程的一部分，真机上要能看到（W 级）。
    logWarn(std::string("sunshine cancel failed (ignored): ") + error.what());
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

enum PickLaunchOutcome {
  PickLaunchStarted = 0,
  PickLaunchAborted = 1,  // stop 或换代：调用方结束/重启本次尝试
  PickLaunchNoApps = 2    // /applist 为空
};

// 选应用并通过 /launch（或 /resume）启动，处理两件事：
//   * 应用 ID 缓存：本会话首次匹配到的应用缓存在 cachedAppId 里，重连不再拉
//     /applist（实测一次 HTTPS 往返约 5 s）；只有 launch 用缓存 ID 报 404/400
//     「应用不存在」时才重新拉一次 applist 并重试一次。
//   * 主机残留会话：BUSY 且是目标应用 -> /resume；别的应用 -> cancel→等空闲→launch。
// 返回 PickLaunchAborted 表示被 stop 或换代打断；异常原样抛出。
PickLaunchOutcome pickAndLaunchApp(
    const SunshineHost& host, int generation, tg::sunshine::NvHttpClient& client,
    const SunshineOptions& options, tg::sunshine::ServerInfo* info,
    int* cachedAppId, std::string* cachedTitle,
    tg::sunshine::LaunchOptions* launchOptions, tg::sunshine::SessionLaunch* launch,
    bool* resumed) {
  if (*cachedAppId == 0) {
    const std::vector<tg::sunshine::AppInfo> apps = client.get_app_list();
    const tg::sunshine::AppInfo* app = pickApp(apps, options.app);
    if (app == nullptr) return PickLaunchNoApps;
    *cachedAppId = app->id;
    *cachedTitle = app->title;
  }
  launchOptions->app_id = *cachedAppId;
  if (host.setAppTitle != nullptr)
    host.setAppTitle(host.user, generation, cachedTitle->c_str());

  int busyRetry = 0;
  int appRefresh = 0;
  for (;;) {
    // 配对前那次 /serverinfo 可能已过期：忙时重新读一次再决定 resume/cancel。
    if (serverBusy(*info)) {
      try {
        *info = client.get_server_info().server;
      } catch (const tg::sunshine::PinMismatchError&) {
        throw;
      } catch (const std::exception& error) {
        logWarn(std::string("sunshine serverinfo refresh failed (ignored): ") +
                error.what());
      }
    }
    *resumed = serverBusy(*info) && info->current_game == *cachedAppId;
    try {
      if (*resumed) {
        // 同一个应用还在跑：/resume 接管续播，参数与 /launch 相同。
        *launch = client.resume(*launchOptions);
      } else if (serverBusy(*info)) {
        if (!cancelAndStartApp(client, host, generation, *launchOptions, *cachedAppId,
                               *info, launch, resumed))
          return PickLaunchAborted;
      } else {
        *launch = client.launch(*launchOptions);
      }
      return PickLaunchStarted;
    } catch (const tg::sunshine::NvError& error) {
      if (!*resumed && busyRetry == 0 && launchAlreadyRunning(error)) {
        // state 标记与实际不一致：/launch 仍可能 400 "already running"。
        // 只重试一次（cancel -> 等空闲 -> launch/resume）。
        ++busyRetry;
        logWarn("sunshine: launch reported an app already running; retrying once");
        if (!cancelAndStartApp(client, host, generation, *launchOptions, *cachedAppId,
                               *info, launch, resumed))
          return PickLaunchAborted;
        return PickLaunchStarted;
      }
      if (appRefresh == 0 && *cachedAppId != 0 &&
          (error.nv_status_code() == 404 || error.http_status() == 404)) {
        // 缓存的应用 ID 已被服务端拒绝（应用列表变了）：重新拉一次再试一次。
        ++appRefresh;
        logWarn("sunshine: cached app id rejected by launch; refreshing app list once");
        const std::vector<tg::sunshine::AppInfo> apps = client.get_app_list();
        const tg::sunshine::AppInfo* app = pickApp(apps, options.app);
        if (app == nullptr) return PickLaunchNoApps;
        *cachedAppId = app->id;
        *cachedTitle = app->title;
        launchOptions->app_id = *cachedAppId;
        if (host.setAppTitle != nullptr)
          host.setAppTitle(host.user, generation, cachedTitle->c_str());
        continue;
      }
      throw;
    }
  }
}

// ---------------------------------------------------------------------------
// 串流阶段（LiStartConnection 及其等待/停止）
// ---------------------------------------------------------------------------
struct MediaOutcome {
  MediaOutcome()
      : stopped(false), connected(false), restart(false), resize(false),
        resizeWidth(0), resizeHeight(0), terminationCode(0), pictureMs(0),
        failedStage(0) {}
  bool stopped;    // 宿主已要求停止
  bool connected;  // LiStartConnection 成功过
  bool restart;    // generation 变化（手动重连），应立即进入下一次尝试
  bool resize;     // 客户端请求按新尺寸重启（窗口调整分辨率）
  int resizeWidth;
  int resizeHeight;
  std::string text;  // 启动失败或终止原因
  int terminationCode;        // common-c 终止错误码（0 / -1 = 主机结束类）
  unsigned long long pictureMs;  // 本次会话正常画面持续时长（0 = 从未有画面）
  int failedStage;            // LiStartConnection 失败的阶段（0 = 不是阶段失败）
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
  // 画面全黑检测也按尝试复位；真正的计时起点在 videoSetupCallback。
  media.pictureFrameMs.store(0);
  media.pictureFirstNonBlackMs.store(0);
  media.pictureLastNonBlackMs.store(0);
  media.pictureBlackStartMs.store(0);
  media.terminationPictureMs.store(0);
  media.currentStage.store(0);
  media.firstFrameTimeoutLogged.store(false);
  media.rttAvgMs.store(0);
  media.rttMinMs.store(0);
  media.rttValid.store(false);
  {
    std::lock_guard<std::mutex> lock(g_gamepad.mutex);
    g_gamepad.arrived = false;
    g_gamepad.hasLast = false;
    g_gamepad.updates = 0;
    g_gamepad.lastButtons = 0;
  }
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
  // 零拷贝（Surface 模式）桥接：视频 sink 只会转调宿主的同名回调，宿主不实现时
  // 视频路径回退 Buffer 模式。
  frameSink.acquireExternalSource = [](void* user, int frameGeneration,
                                       unsigned int* textureId,
                                       void** nativeWindow) -> bool {
    const SunshineHost* sinkHost = static_cast<const SunshineHost*>(user);
    if (sinkHost->acquireExternalSource == nullptr) return false;
    return sinkHost->acquireExternalSource(sinkHost->user, frameGeneration, textureId,
                                           nativeWindow);
  };
  frameSink.setSourceSize = [](void* user, int frameGeneration, int width, int height) {
    const SunshineHost* sinkHost = static_cast<const SunshineHost*>(user);
    if (sinkHost->setSourceSize != nullptr)
      sinkHost->setSourceSize(sinkHost->user, frameGeneration, width, height);
  };
  frameSink.releaseExternalSource = [](void* user, int frameGeneration) {
    const SunshineHost* sinkHost = static_cast<const SunshineHost*>(user);
    if (sinkHost->releaseExternalSource != nullptr)
      sinkHost->releaseExternalSource(sinkHost->user, frameGeneration);
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
  connectionCallbacks.rumble = rumbleCallback;
  connectionCallbacks.rumbleTriggers = rumbleTriggersCallback;

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
  const std::chrono::steady_clock::time_point streamStart =
      std::chrono::steady_clock::now();
  std::atomic<bool> startReturned(false);
  std::thread watchdog;
  try {
    watchdog = std::thread([&host, &startReturned, &media, streamStart]() {
      while (!startReturned.load()) {
        if (host.stopRequested(host.user)) {
          LiInterruptConnection();
          return;
        }
        // launch 后 5 秒还没连上：打一行 W 级日志说明卡在哪个阶段（RTSP/控制流/
        // 视频），否则真机上只能看到黑屏，不知道慢在哪。只打一次。
        if (!media.started.load() && !media.firstFrameTimeoutLogged.load()) {
          const long long elapsedMs = static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - streamStart).count());
          if (elapsedMs >= 5000) {
            media.firstFrameTimeoutLogged.store(true);
            const int stage = media.currentStage.load();
            logWarn(formatText(
                "sunshine: no first frame %lld ms after launch; setup at stage %s "
                "(stage %d), established=0",
                elapsedMs, LiGetStageName(stage), stage));
          }
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
    inputThread = std::thread(
        [&host, &media, &inputSink, &applied, &inputDone, streamStart, generation]() {
      int ticks = 0;
      int cursorToggleLogs = 0;
      while (!inputDone.load()) {
        if (host.stopRequested(host.user) || media.terminated.load()) break;
        // drainInput() 内部逐条调用 sink -> LiSend*，返回即已发出；这里紧接着只做
        // 2 ms 的兜底轮询等待（不做任何额外 sleep 或攒批），把输入延迟压到最低。
        if (host.drainInput != nullptr)
          host.drainInput(host.user, &inputSink, &applied);
        // 主机光标切换（本地光标模式）：Sunshine input.cpp apply_shortcut 只在
        // shortcutFlags == SHORTCUT 时翻转全局 display_cursor，而 shortcutFlags
        // 是由「修饰键本身」的按下/抬起事件经 update_shortcutFlags 维护的。只发
        // 一个带修饰位标记的 N 键不会触发快捷键，N 还会被传给桌面。所以这里按
        // 真实按键顺序模拟：Ctrl↓ Alt↓ Shift↓ N↓ N↑ Shift↑ Alt↑ Ctrl↑。
        // 必须在输入流就绪后发送，所以放在这里（2 ms 节拍）；任一步返回非 0 立即
        // 中止并保持待发，下一 tick 重试。该路径直接调用 LiSendKeyboardEvent、
        // 不经过 InputTranslator::key()，因此不会污染它对修饰键/按下键的跟踪。
        if (host.takeCursorHiddenRequest != nullptr) {
          bool hidden = false;
          if (host.takeCursorHiddenRequest(host.user, &hidden)) {
            struct ChordStep {
              short key;
              char action;
              char modifiers;
            };
            const ChordStep steps[] = {
                {0x11, KEY_ACTION_DOWN, static_cast<char>(MODIFIER_CTRL)},
                {0x12, KEY_ACTION_DOWN,
                 static_cast<char>(MODIFIER_CTRL | MODIFIER_ALT)},
                {0x10, KEY_ACTION_DOWN,
                 static_cast<char>(MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)},
                {0x4E, KEY_ACTION_DOWN,
                 static_cast<char>(MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)},
                {0x4E, KEY_ACTION_UP,
                 static_cast<char>(MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT)},
                {0x10, KEY_ACTION_UP,
                 static_cast<char>(MODIFIER_CTRL | MODIFIER_ALT)},
                {0x12, KEY_ACTION_UP, static_cast<char>(MODIFIER_CTRL)},
                {0x11, KEY_ACTION_UP, static_cast<char>(0)},
            };
            const size_t stepCount = sizeof(steps) / sizeof(steps[0]);
            std::string codes;
            size_t failedStep = stepCount;
            for (size_t i = 0; i < stepCount; ++i) {
              const int result = LiSendKeyboardEvent(
                  steps[i].key, steps[i].action, steps[i].modifiers);
              codes += formatText("%s%d", i == 0 ? "" : "/", result);
              if (result != 0) {
                failedStep = i;
                break;  // 中止：保持待发，下一 tick 重试整串
              }
            }
            if (failedStep == stepCount) {
              logWarn(formatText(
                  "sunshine: host cursor %s (Ctrl+Alt+Shift+N, rc=%s)",
                  hidden ? "hidden" : "shown", codes.c_str()));
              if (host.noteCursorHidden != nullptr)
                host.noteCursorHidden(host.user, generation, hidden);
            } else if (cursorToggleLogs < 3) {
              ++cursorToggleLogs;
              logWarn(formatText(
                  "sunshine: host cursor toggle aborted at step %llu (rc=%s); "
                  "will retry",
                  static_cast<unsigned long long>(failedStep), codes.c_str()));
            }
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // 连上了但 5 秒还没有任何解码帧：打一行 W 级日志（只打一次），说明卡在
        // 视频阶段，而不是让用户对着黑屏猜。
        if (!media.firstFrameTimeoutLogged.load() &&
            media.pictureFrameMs.load() == 0) {
          const long long elapsedMs = static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - streamStart).count());
          if (media.started.load() && elapsedMs >= 5000) {
            media.firstFrameTimeoutLogged.store(true);
            const int stage = media.currentStage.load();
            logWarn(formatText(
                "sunshine: established but no decoded frame %lld ms after launch; "
                "last stage %s (stage %d)",
                elapsedMs, LiGetStageName(stage), stage));
          }
        }
        // 每 5 秒打一次媒体计数（只读计数，不含任何画面内容）。2 ms 节拍下
        // 2500 个 tick = 5 秒。
        if (++ticks % 2500 == 0 && media.video != nullptr) {
          const SunshineVideoStats videoStats = media.video->stats();
          const SunshineAudioStats audioStats =
              media.audio != nullptr ? media.audio->stats() : SunshineAudioStats();
          const SunshinePictureStatus picture = sunshinePictureStatus();
          // 媒体统计也走 W：手机上默认看不到 D 级，无法确认帧/音频/重连计数。
          // 限频不变（每 500 个 10ms 节拍 = 5 秒一次）。
          logWarn(formatText(
              "sunshine media: frames=%llu (idr %llu), video bytes=%llu, audio "
              "packets=%llu, audio bytes=%llu, black=%llums, last frame=%llums, "
              "autoReconnects=%d",
              videoStats.frames, videoStats.idrFrames, videoStats.bytes,
              audioStats.packets, audioStats.bytes, picture.blackMs,
              picture.lastFrameAgeMs, g_autoReconnects.load()));
          // 输入延迟窗口（入队 -> LiSend* 返回）与网络 RTT 一起放进下面的 perf
          // 行，方便一行对照「鼠标传递延迟」和「画面延迟」。LiGetEstimatedRttInfo
          // 只能在 LiStartConnection()/LiStopConnection() 之间调用（输入线程正好
          // 只在这个窗口里运行）；它内部是 ENet 统计的 trylock + 原子缓存，不阻塞。
          const InputWindowStats inputStats = media.input.takeStats();
          uint32_t rttMs = 0;
          uint32_t rttVarianceMs = 0;
          const bool rttOk = LiGetEstimatedRttInfo(&rttMs, &rttVarianceMs);
          std::string rttText = "n/a";
          if (rttOk) {
            // 接口只给 avg 与 variance（enet roundTripTime/Variance，毫秒），没有
            // 单独的 min：本窗口最小估计取 max(0, avg - variance)，并保留本次串流
            // 观察到的最小值。
            const unsigned int minEstimate =
                rttMs > rttVarianceMs ? rttMs - rttVarianceMs : 0;
            const unsigned int previousMin = media.rttMinMs.load();
            const unsigned int sessionMin =
                previousMin == 0 ? minEstimate : std::min(previousMin, minEstimate);
            media.rttAvgMs.store(rttMs);
            media.rttMinMs.store(sessionMin);
            media.rttValid.store(true);
            rttText = formatText("%u/%u", rttMs, sessionMin);
          }
          // 视频链路分段耗时（同一 5 秒窗口，avg/p95 毫秒）：
          //   解码侧（sunshine_video.cxx）netToSubmit / submitToOutput / convert /
          //     commit 与 in/dec 帧率；
          //   渲染侧（render.cxx）实际上屏帧率 out、取帧滞后 lag、取帧到 swap 返回
          //     的 swap；Surface（零拷贝）源下 lag=帧可用->UpdateSurfaceImage，
          //     swap=UpdateSurfaceImage->swap，surface=帧可用->swap（另含 vsync 等待）。
          // 两边都是进程级只读快照，读它不阻塞解码或渲染线程。
          const SunshineVideoPerf videoPerf = sunshineVideoPerf();
          const RenderFramePerf renderPerf = renderFramePerf();
          logWarn(formatText(
              "sunshine perf: rtt=%s inputLag=%.2f/%.2f "
              "in=%.1ffps dec=%.1ffps out=%.1ffps "
              "netToSubmit=%.1f/%.1f submitToOutput=%.1f/%.1f convert=%.1f/%.1f "
              "commit=%.1f/%.1f lag=%.1f/%.1f swap=%.1f/%.1f surface=%.1f/%.1f "
              "captureToSwap=%.1f/%.1f receiveToSwap=%.1f/%.1f "
              "(avg/p95 ms, latency=%s, window=%llums, dropped=%llu, "
              "sendCalls=%llu sendFail=%llu%s)",
              rttText.c_str(), inputStats.avgMs, inputStats.p95Ms,
              videoPerf.inFps, videoPerf.decFps, renderPerf.outFps,
              videoPerf.netToSubmitAvgMs, videoPerf.netToSubmitP95Ms,
              videoPerf.submitToOutputAvgMs, videoPerf.submitToOutputP95Ms,
              videoPerf.convertAvgMs, videoPerf.convertP95Ms,
              videoPerf.commitAvgMs, videoPerf.commitP95Ms,
              renderPerf.lagAvgMs, renderPerf.lagP95Ms,
              renderPerf.pickupToSwapAvgMs, renderPerf.pickupToSwapP95Ms,
              renderPerf.frameAvailableToSwapAvgMs,
              renderPerf.frameAvailableToSwapP95Ms, renderPerf.captureToSwapAvgMs,
              renderPerf.captureToSwapP95Ms, renderPerf.receiveToSwapAvgMs,
              renderPerf.receiveToSwapP95Ms,
              renderPerf.latencySource == 1 ? "pts"
              : (renderPerf.latencySource == 2 ? "recv" : "none"),
              videoPerf.windowMs, videoPerf.droppedUnits, inputStats.sendCalls,
              inputStats.sendFail,
              rttOk ? "" : ", rtt unavailable (ENet stats not ready yet)"));
          // 鼠标按键窗口汇总：真实按下/抬起、releaseEpoch/失焦合成的抬起、纯移动
          // 事件次数。只做日志，用来排查「按住移动时锁定丢失」这类多余按键问题。
          // 手柄窗口汇总：本次窗口内的状态更新次数与最后一次按钮位图。NAPI 线程
          // 直接写（同一把锁），这里取走并清零。
          unsigned long long padUpdates = 0;
          uint32_t padButtons = 0;
          {
            std::lock_guard<std::mutex> lock(g_gamepad.mutex);
            padUpdates = g_gamepad.updates;
            padButtons = g_gamepad.lastButtons;
            g_gamepad.updates = 0;
          }
          logWarn(formatText("sunshine gamepad: updates=%llu lastButtons=0x%08X",
                             padUpdates, padButtons));
          const MouseWindowStats mouseStats = media.input.takeMouseStats();
          logWarn(formatText(
              "sunshine mouse: press=%llu release=%llu releaseSynth=%llu "
              "moveEvents=%llu",
              mouseStats.press, mouseStats.release, mouseStats.releaseSynth,
              mouseStats.moveEvents));
        }
      }
        });
  } catch (...) {
    // Degraded but safe: keep the stream, drop the input for this attempt. The
    // stream slot and every sink stay owned by this thread, so nothing dangles.
    logWarn("sunshine: cannot create the input thread; input disabled for this stream");
  }

  // 连接期间的等待循环：停止、对端终止、换代（手动重连）或分辨率调整。
  int resizePollTicks = 0;
  while (!host.stopRequested(host.user) && !media.terminated.load()) {
    const int currentGeneration =
        host.generation != nullptr ? host.generation(host.user) : generation;
    if (currentGeneration != generation) {
      outcome.restart = true;
      break;
    }
    // 每 200ms（20 个 10ms 节拍）取一次客户端主动分辨率请求。take 之后请求槽
    // 处于 in-flight，处理期间再来的请求只覆盖待发槽（最后一个生效），下一次
    // 轮询会再处理——与 RDP 的请求槽语义一致。
    if (host.takeDesktopSizeRequest != nullptr && ++resizePollTicks >= 20) {
      resizePollTicks = 0;
      int resizeWidth = 0;
      int resizeHeight = 0;
      if (host.takeDesktopSizeRequest(host.user, &resizeWidth, &resizeHeight)) {
        if (resizeWidth == options.width && resizeHeight == options.height) {
          // 和当前流尺寸相同：不需要重启，直接 ack。
          if (host.finishDesktopSizeRequest != nullptr)
            host.finishDesktopSizeRequest(host.user, generation, resizeWidth,
                                          resizeHeight, true);
        } else {
          // 立即进入 reconnecting（适配层在 take 时已写好 resize 标记）；真正的
          // 停流/重新 launch 由外层循环完成。
          logWarn(formatText("sunshine: resize request %dx%d (current %dx%d)",
                             resizeWidth, resizeHeight, options.width,
                             options.height));
          setHostPhase(host, SunshinePhaseReconnecting,
                       "正在按窗口调整分辨率…", "none", 0);
          outcome.resize = true;
          outcome.resizeWidth = resizeWidth;
          outcome.resizeHeight = resizeHeight;
          break;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 停止/断开/换代：若本次已把手柄「插上」主机，补一条 mask=0 的空事件移除，
  // 避免主机端留下一个一直按住的虚拟手柄。
  if (g_media.started.load()) sunshineReleaseGamepad();

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
  // 终止原因与「本次会话是否交付过画面」（connectionTerminated 时的快照）。
  outcome.terminationCode = media.terminationCode.load();
  outcome.pictureMs = media.terminationPictureMs.load();
  outcome.failedStage = media.failedStage.load();
  return outcome;
}

}  // namespace

// 输入队列延迟样本：由 diagnostics.cxx 的 drain 适配层在每次 sink 调用（即
// LiSend* 返回）之后调用，入参是「入队 -> 发送返回」的微秒数。只在后端输入线程
// 上调用，所以直接写 InputTranslator 的窗口，不加锁。
void sunshineNoteInputQueueLag(unsigned long long lagUs) {
  if (!g_media.active.load()) return;
  g_media.input.noteLag(lagUs);
}

// 手柄透传：NAPI 线程直接调用。见 sunshine_backend.h 的说明。
bool sunshineSendGamepad(uint32_t buttons, uint8_t leftTrigger,
                         uint8_t rightTrigger, int leftStickX, int leftStickY,
                         int rightStickX, int rightStickY) {
  if (!g_media.active.load() || !g_media.started.load()) return false;
  const short lx = static_cast<short>(std::max(-32768, std::min(32767, leftStickX)));
  const short ly = static_cast<short>(std::max(-32768, std::min(32767, leftStickY)));
  const short rx = static_cast<short>(std::max(-32768, std::min(32767, rightStickX)));
  const short ry = static_cast<short>(std::max(-32768, std::min(32767, rightStickY)));
  std::lock_guard<std::mutex> lock(g_gamepad.mutex);
  if (!g_gamepad.arrived) {
    const int result = LiSendControllerArrivalEvent(
        0, 0x0001, LI_CTYPE_XBOX, kGamepadArrivalButtons,
        kGamepadArrivalCapabilities);
    if (result != 0) {
      logWarn(formatText("sunshine gamepad: arrival failed (%d); will retry", result));
      return false;
    }
    g_gamepad.arrived = true;
    logWarn("sunshine gamepad: arrival (controller 0, Xbox, analog triggers + rumble)");
  }
  if (g_gamepad.hasLast && buttons == g_gamepad.lastButtons &&
      leftTrigger == g_gamepad.lastLeftTrigger &&
      rightTrigger == g_gamepad.lastRightTrigger && lx == g_gamepad.lastLeftX &&
      ly == g_gamepad.lastLeftY && rx == g_gamepad.lastRightX &&
      ry == g_gamepad.lastRightY)
    return true;  // 与上次完全相同的状态：不重复发送
  const int result = LiSendMultiControllerEvent(
      0, 0x0001, static_cast<int>(buttons), leftTrigger, rightTrigger, lx, ly, rx,
      ry);
  if (result != 0) {
    logWarn(formatText("sunshine gamepad: state send failed (%d)", result));
    return false;
  }
  g_gamepad.hasLast = true;
  g_gamepad.lastButtons = buttons;
  g_gamepad.lastLeftTrigger = leftTrigger;
  g_gamepad.lastRightTrigger = rightTrigger;
  g_gamepad.lastLeftX = lx;
  g_gamepad.lastLeftY = ly;
  g_gamepad.lastRightX = rx;
  g_gamepad.lastRightY = ry;
  ++g_gamepad.updates;
  return true;
}

bool sunshineReleaseGamepad() {
  if (!g_media.active.load() || !g_media.started.load()) return false;
  std::lock_guard<std::mutex> lock(g_gamepad.mutex);
  if (!g_gamepad.arrived) return true;
  const int result = LiSendMultiControllerEvent(0, 0x0000, 0, 0, 0, 0, 0, 0, 0);
  g_gamepad.arrived = false;
  g_gamepad.hasLast = false;
  if (result == 0) {
    logWarn("sunshine gamepad: removed (controller 0)");
  } else {
    logWarn(formatText("sunshine gamepad: removal failed (%d)", result));
  }
  return result == 0;
}

// 当前串流最近一次采到的网络 RTT（毫秒）与本串流观察到的最小估计；还没采到
// （ENet 统计未就绪 / 未连接）时返回 false。NAPI 线程可调用，只读原子量。
bool sunshineRttStatus(unsigned int* avgMs, unsigned int* minMs) {
  if (!g_media.rttValid.load()) return false;
  if (avgMs != nullptr) *avgMs = g_media.rttAvgMs.load();
  if (minMs != nullptr) *minMs = g_media.rttMinMs.load();
  return true;
}

// 视频解码回调线程每帧上报一次（见 sunshine_backend.h）。这里只写原子时间戳：
// 不读时钟、不加锁，所以对解码线程的开销是一次 store。
void sunshineNoteVideoLuma(unsigned long long frameTimeMs, bool black) {
  MediaContext& media = g_media;
  if (!media.active.load()) return;
  media.pictureFrameMs.store(frameTimeMs);
  // 非黑帧把「连续全黑/无帧」的起点推进到这一刻；黑帧不动它，时长自然增长。
  // 同时记下首个与最近一个非黑帧的时刻，终止时用两者的差衡量「正常画面持续了
  // 多久」（只有视频输出回调线程会调这里，先读后写不需要 CAS）。
  if (!black) {
    if (media.pictureFirstNonBlackMs.load() == 0)
      media.pictureFirstNonBlackMs.store(frameTimeMs);
    media.pictureLastNonBlackMs.store(frameTimeMs);
  }
}

// 状态读取方（statusJson 的 NAPI 线程）在读取时换算时长；时间戳都是进程级原子
// 量，因此这里不需要任何锁，也不会和视频回调线程互相等待。
SunshinePictureStatus sunshinePictureStatus() {
  SunshinePictureStatus status;
  if (!g_media.active.load()) return status;
  const unsigned long long now = monotonicMillis();
  const unsigned long long frameMs = g_media.pictureFrameMs.load();
  const unsigned long long lastNonBlack = g_media.pictureLastNonBlackMs.load();
  const unsigned long long blackStart = g_media.pictureBlackStartMs.load();
  // blackMs 从最近一次非黑帧（本次尝试从未出现非黑帧时是 video setup）起算：
  // 全黑帧持续到达、或者根本没有帧，它都会增长；任何非黑帧都把它清零。
  const unsigned long long baseline = lastNonBlack != 0 ? lastNonBlack : blackStart;
  if (baseline != 0) status.blackMs = now - baseline;
  if (frameMs != 0) {
    status.lastFrameAgeMs = now - frameMs;
  } else if (baseline != 0) {
    // 本次尝试还没有任何帧：从视频 setup 起算「没有画面」的时长。
    status.lastFrameAgeMs = now - baseline;
  }
  return status;
}

enum ProbeOutcome {
  ProbeOk = 0,
  ProbeNeedPair = 1,  // HTTPS 返回 401/403：客户端证书不再被接受（确定性失败）
  ProbeStopped = 2    // stop 或换代
};

// 重连专用 /serverinfo 探测：只用 HTTPS 快路径（单次 5 s 超时），失败退避 1 s
// 重试、最多持续 60 s；只有拿到确定答案才离开：
//   * 成功；* 401/403（证书不再被接受，交给配对流程）；* stop/换代。
// 超时/网络类失败不再退回「明文 + HTTPS」的长探测——真机上正是那一步多花了 20 s。
ProbeOutcome probeHttpsWithRetry(const SunshineHost& host, int generation,
                                 tg::sunshine::NvHttpClient& client,
                                 tg::sunshine::ServerInfo* info) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  int tries = 0;
  for (;;) {
    if (host.stopRequested(host.user)) return ProbeStopped;
    if (host.generation != nullptr && host.generation(host.user) != generation)
      return ProbeStopped;
    std::string errorText;
    try {
      *info = client.get_server_info_https();
      if (tries > 0)
        logWarn(formatText("sunshine: HTTPS serverinfo succeeded after %d retries", tries));
      return ProbeOk;
    } catch (const tg::sunshine::PinMismatchError&) {
      throw;  // 指纹不一致：确定性失败，外层按 failed/fingerprint-mismatch 处理
    } catch (const tg::sunshine::NvError& error) {
      if (error.http_status() == 401 || error.http_status() == 403)
        return ProbeNeedPair;
      errorText = error.what();
    } catch (const std::exception& error) {
      errorText = error.what();
    }
    ++tries;
    if (std::chrono::steady_clock::now() >= deadline) {
      logWarn(formatText("sunshine: serverinfo still unreachable after 60 s (%s)",
                         errorText.c_str()));
      throw tg::sunshine::NvError(
          "60 秒内无法连接 Sunshine 服务端（" + errorText + "）", 0, -1, "", "");
    }
    logWarn(formatText(
        "sunshine: serverinfo probe failed (try %d): %s; retry in 1000 ms", tries,
        errorText.c_str()));
    if (!host.sleepUnlessStopped(host.user, 1000)) return ProbeStopped;
  }
}

void runSunshineSession(const SunshineHost& host, const SunshineOptions& options) {
  // 缺少状态目录属于配置错误：会话进入 failed/protocol 并等用户重连，绝不落到 VNC
  // 循环上（端口和握手完全不同）。
  if (options.stateDir.empty()) {
    setHostPhase(host, SunshinePhaseFailed,
                 "缺少 Sunshine 状态目录（sunshineStateDir，须传应用沙箱绝对路径）",
                 "protocol", 0);
    host.waitForReconnect(host.user);
    return;
  }

  g_lastPhase.store(-1);
  // 会话内可变副本：分辨率调整会改宽高与码率（原始 options 只作为缩放基准）。
  SunshineOptions current = options;
  // 码率缩放基准：会话选项里的原始分辨率与码率。
  const long long basePixels =
      static_cast<long long>(options.width) * options.height;
  const int baseBitrate = options.bitrate;
  int lastApplied = 0;
  int backoffMs = 1000;
  int transportPairFailures = 0;
  // 「主机结束串流且本次会话从未有画面」的连续自动重连次数（最多 3 次）。
  int autoReconnects = 0;
  // 首次连接与重连分开对待：首次走完整 /serverinfo（明文探测 HttpsPort + HTTPS，
  // 10 s 超时）；之后直接用学到/缓存的值走 HTTPS 快路径（4 s 超时）。
  bool firstAttempt = true;
  uint16_t savedHttpsPort = 0;
  // 本会话缓存的目标应用（首次 /applist 匹配结果）。重连不再拉 /applist，
  // 除非 /launch 用缓存 ID 报 404（应用列表变了）才重拉一次。
  int cachedAppId = 0;
  std::string cachedAppTitle;
  // 自动重连窗口里的总预算（60 s）：serverinfo 探测与 RTSP 阶段重试共同消耗，
  // 主机端在预算内恢复就能连上，超时才判定失败。hostNotReadyRetries 只用于
  // W 级日志「主机尚未就绪，第 n 次重试」。
  int hostNotReadyRetries = 0;
  std::chrono::steady_clock::time_point reconnectDeadline =
      std::chrono::steady_clock::now();

  int generation = 0;
  // 分辨率调整的续跑标记：下一次尝试沿用同一个 generation，并且不重置请求槽
  // （beginAttempt 会清 desktopPending/desktopAwaiting，而 in-flight 与处理期间
  // 到达的最新请求必须保留）。
  bool resizeContinuation = false;
  // 分辨率调整续播成功后，下一次迭代直接用它起流（跳过 identity/probe/applist）。
  bool pendingResumeLaunch = false;
  tg::sunshine::SessionLaunch resumeLaunch;
  std::string resumeAppVersion;
  std::string resumeGfeVersion;
  int resumeCodecSupport = 0;
  // 客户端跨迭代保留：续播路径不重建它（后续的 cancel/launch 还要用）。
  std::unique_ptr<tg::sunshine::NvHttpClient> client;
  while (!host.stopRequested(host.user)) {
    const bool continuation = resizeContinuation;
    resizeContinuation = false;
    if (!continuation) generation = host.beginAttempt(host.user, &lastApplied);
    // 自动重连窗口里不要先跳回 connecting：UI 的「正在重新连接…」只在
    // reconnecting 状态显示，探测/重试期间保持该状态直到真正连上。分辨率调整
    // 同理，保持 reconnecting 直到新尺寸的串流建立。
    if (autoReconnects == 0 && !continuation)
      setHostPhase(host, SunshinePhaseConnecting, "", "none", 0);

    try {
      // 本次尝试要用的服务端信息与 launch 结果。分辨率调整的续播（/resume 已在
      // 上一次迭代末尾拿到新的 sessionUrl0/rikey）直接复用，跳过整段控制面。
      tg::sunshine::ServerInfo info;
      tg::sunshine::SessionLaunch launch;
      bool startReady = false;
      bool resumedFastPath = false;
      if (pendingResumeLaunch) {
        pendingResumeLaunch = false;
        resumedFastPath = true;
        launch = resumeLaunch;
        info.appversion = resumeAppVersion;
        info.gfe_version = resumeGfeVersion;
        info.server_codec_mode_support = static_cast<uint32_t>(resumeCodecSupport);
        logWarn(formatText(
            "sunshine: resize continuation (%dx%d) uses the /resume session",
            current.width, current.height));
        startReady = true;
      }
      if (!startReady) {
      // 1. 客户端身份（证书/私钥/uniqueid），缺失时生成并落盘。
      const tg::sunshine::Identity identity =
          tg::sunshine::load_or_create_identity(current.stateDir);
      logDebug(formatText("sunshine identity ready (created_now=%d)",
                          identity.created_now ? 1 : 0));

      // 2. nvhttp 客户端：http_port = 会话端口；首次连接按「端口-5」猜 HTTPS 端口，
      //    重连直接复用上次成功探测到的端口。
      const int probeTimeoutMs = firstAttempt ? 10000 : 5000;
      uint16_t httpsPort = savedHttpsPort != 0
                               ? savedHttpsPort
                               : (options.port > 5
                                      ? static_cast<uint16_t>(options.port - 5)
                                      : 0);
      client.reset(new tg::sunshine::NvHttpClient(
          makeClientOptions(current, httpsPort, probeTimeoutMs)));

      // 3. /serverinfo。首次连接：完整探测（明文拿 HttpsPort + HTTPS），10 s 超时。
      //    重连：只走 HTTPS 快路径 + 1 s 退避重试（见 probeHttpsWithRetry），超时
      //    类失败不再退回明文长探测；只有 401/403（证书不再被接受）才补一次完整
      //  探测去走配对流程。
      tg::sunshine::ServerInfoProbe probe;
      bool needFullProbe = firstAttempt;
      if (!firstAttempt) {
        if (savedHttpsPort == 0) {
          // 还没学到 HTTPS 端口（理论上不会发生）：做一次完整探测。
          needFullProbe = true;
        } else {
          const ProbeOutcome probeOutcome =
              probeHttpsWithRetry(host, generation, *client, &info);
          if (probeOutcome == ProbeStopped) {
            if (host.stopRequested(host.user)) break;
            continue;  // 手动换代
          }
          if (probeOutcome == ProbeOk) {
            probe.server = info;
            probe.https_ok = true;
            probe.https_port_used = savedHttpsPort;
          } else {
            // 401/403：服务端不再信任客户端证书。补一次完整探测拿 appversion 等，
            // 再交给下面的配对流程（这是确定答案，不是超时兜底）。
            logWarn("sunshine: server rejected the client certificate; full probe for pairing");
            needFullProbe = true;
          }
        }
      }
      if (needFullProbe) {
        probe = client->get_server_info();
        info = probe.server;
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
        if (firstAttempt && info.https_port != 0 &&
            static_cast<uint16_t>(info.https_port) != httpsPort &&
            !probe.https_ok) {
          // 服务端在别的 HTTPS 端口上：按它公布的值重建客户端并重新探测。
          httpsPort = static_cast<uint16_t>(info.https_port);
          client.reset(new tg::sunshine::NvHttpClient(
              makeClientOptions(current, httpsPort, probeTimeoutMs)));
          probe = client->get_server_info();
          info = probe.server;
        }
      }
      // 记住这次用到的 HTTPS 端口，后续重连走快路径；首次连接到此结束。
      if (probe.https_ok && probe.https_port_used != 0)
        savedHttpsPort = probe.https_port_used;
      else if (info.https_port != 0)
        savedHttpsPort = static_cast<uint16_t>(info.https_port);
      firstAttempt = false;
      if (info.appversion.empty()) {
        setHostPhase(host, SunshinePhaseFailed,
                     "服务端 /serverinfo 缺少 appversion，无法建立 Moonlight 会话",
                     "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      // 生命周期日志统一用 W 级：真机默认只显示 W 及以上，D 级的诊断在手机上
      // 看不到。只打印状态/版本，不含 PIN、rikey、证书或剪贴板内容。
      logWarn(formatText("sunshine serverinfo: state=%s, pairStatus=%d, appversion=%s",
                         info.state.empty() ? "(unset)" : info.state.c_str(),
                         info.pair_status, info.appversion.c_str()));
      if (host.setServerAppVersion != nullptr)
        host.setServerAppVersion(host.user, generation, info.appversion.c_str());

      // 4. 配对：未配对时进入 pairing-required 等 UI 提交 PIN。
      if (info.pair_status != 1) {
        bool paired = false;
        bool terminal = false;
        // Enter pairing-required once. A failed pair re-enters the wait with its
        // pair-failed error text set by the catch below; the top of the loop
        // must not overwrite that (otherwise the UI would never see it).
        setHostPhase(host, SunshinePhasePairingRequired, "", "none", 0);
        while (!paired) {
          if (host.stopRequested(host.user)) break;
          std::string pin;
          if (!host.waitForPin(host.user, generation, &pin)) {
            // stop 或手动换代：回到外层重新判断。
            wipeString(pin);
            break;
          }
          setHostPhase(host, SunshinePhaseConnecting, "正在配对", "none", 0);
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
        setHostPhase(host, SunshinePhaseFailed,
                     "服务端 /serverinfo 未声明 ServerCodecModeSupport，无法协商视频格式",
                     "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }

      // 5/6. 应用与启动：首次拉 /applist 并缓存目标应用 ID，重连直接用缓存 ID；
      //       /launch 处理主机残留会话（resume / cancel→等空闲），缓存 ID 失效
      //       时重拉 applist 再试一次（见 pickAndLaunchApp）。
      if (cachedAppId != 0) {
        logWarn(formatText("sunshine: reusing cached app \"%s\" (#%d), skipping /applist",
                           cachedAppTitle.c_str(), cachedAppId));
      }
      tg::sunshine::LaunchOptions launchOptions;
      launchOptions.width = current.width;
      launchOptions.height = current.height;
      launchOptions.fps = current.fps;
      bool resumed = false;
      const PickLaunchOutcome pickOutcome = pickAndLaunchApp(
          host, generation, *client, current, &info, &cachedAppId, &cachedAppTitle,
          &launchOptions, &launch, &resumed);
      if (pickOutcome == PickLaunchAborted) {
        if (host.stopRequested(host.user)) break;
        continue;  // 手动换代
      }
      if (pickOutcome == PickLaunchNoApps) {
        setHostPhase(host, SunshinePhaseFailed,
                     "服务端应用列表为空，无法启动 Sunshine 应用", "protocol", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      logWarn(formatText("sunshine launch: app=\"%s\" (id %d), resumed=%d, sessionUrl set=%d",
                         cachedAppTitle.c_str(), cachedAppId, resumed ? 1 : 0,
                         launch.session_url0.empty() ? 0 : 1));
      }  // !startReady（控制面）

      logWarn(formatText("sunshine stream start: %dx%d @ %d fps, %d kbps, resume=%d",
                         current.width, current.height, current.fps,
                         current.bitrate, resumedFastPath ? 1 : 0));

      // 7. 串流（阻塞到终止 / 停止 / 换代）。
      const MediaOutcome outcome =
          runMediaStream(host, current, info.appversion, info.gfe_version,
                         static_cast<int>(info.server_codec_mode_support),
                         launch.session_url0, launch.rikey, launch.rikeyid,
                         generation);
      backoffMs = 1000;
      // 分辨率调整（窗口拖动/最大化/全屏）：停流 -> /cancel -> 用新尺寸重新
      // launch（Moonlight 的分辨率只能在 launch 时指定）。整个调整沿用同一个
      // generation（不重置请求槽），也不计入 network 重连次数。
      if (outcome.resize && !outcome.stopped) {
        const int newWidth = outcome.resizeWidth;
        const int newHeight = outcome.resizeHeight;
        // new = baseBitrate × (pixels/basePixels)^0.8 × (HEVC ? 0.7 : 1.0)。
        // 0.8 次幂让码率随像素增长得比线性慢；HEVC 再打 0.7（同画质更省）。
        const double ratio =
            basePixels > 0
                ? static_cast<double>(newWidth) * newHeight /
                      static_cast<double>(basePixels)
                : 1.0;
        const bool hevc = resizeUsesHevc(options);
        const double codecFactor = hevc ? 0.7 : 1.0;
        const double raw = static_cast<double>(baseBitrate) *
                           std::pow(ratio, 0.8) * codecFactor;
        long long rounded = 0;
        if (!std::isfinite(raw)) {
          // NaN/inf 按钳位处理：NaN 落最小值，+inf 落最大值。
          rounded = raw > 0.0 ? 80000 : 15000;
        } else {
          rounded = static_cast<long long>(std::llround(raw / 1000.0)) * 1000;
        }
        // 四舍五入到 1000 kbps，再钳在 15000..80000。
        rounded = std::max(15000LL, std::min(80000LL, rounded));
        const int newBitrate = static_cast<int>(rounded);
        logWarn(formatText(
            "sunshine: resize request %dx%d -> ratio %.3f, codec %s (factor %.1f), "
            "scaled bitrate %d kbps (base %dx%d %d kbps)",
            newWidth, newHeight, ratio, hevc ? "HEVC" : "H.264", codecFactor,
            newBitrate, options.width, options.height, baseBitrate));
        current.width = newWidth;
        current.height = newHeight;
        current.bitrate = newBitrate;
        // 续播优先：/resume 带新 mode，Sunshine 返回新的 sessionUrl0/rikey/
        // rikeyid，新流按新分辨率出画。实测停流→首帧约 11.4 s、0 次 RTSP 失败；
        // 而「cancel → 立即再 launch」会撞上陈旧会话，RTSP 握手要等 ~20 s。
        tg::sunshine::LaunchOptions resumeOptions;
        resumeOptions.app_id = cachedAppId;
        resumeOptions.width = newWidth;
        resumeOptions.height = newHeight;
        resumeOptions.fps = current.fps;
        const std::chrono::steady_clock::time_point resumeStart =
            std::chrono::steady_clock::now();
        try {
          const tg::sunshine::SessionLaunch resumedLaunch =
              client->resume(resumeOptions);
          const long long resumeMs = static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - resumeStart).count());
          logWarn(formatText(
              "sunshine: resize via /resume ok in %lld ms; starting new stream",
              resumeMs));
          resumeLaunch = resumedLaunch;
          resumeAppVersion = info.appversion;
          resumeGfeVersion = info.gfe_version;
          resumeCodecSupport = static_cast<int>(info.server_codec_mode_support);
          pendingResumeLaunch = true;
          resizeContinuation = true;
          continue;
        } catch (const tg::sunshine::PinMismatchError&) {
          throw;
        } catch (const std::exception& error) {
          // 续播失败（HTTP 非 200、nv_status_code 4xx、超时等）：退回原路径
          // 「/cancel -> 等 FREE -> /launch」，保留陈旧会话清理逻辑。
          const long long failedMs = static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - resumeStart).count());
          logWarn(formatText(
              "sunshine: resize /resume failed after %lld ms (%s); falling back to "
              "cancel + launch",
              failedMs, error.what()));
        }
        if (client != nullptr) cancelServerSession(*client);
        resizeContinuation = true;
        continue;
      }
      // 主机已经结束串流（0/-1）时它自己已经清理过会话，再 /cancel 只会白等一次
      // 网络超时（真机实测 10.5 s）。只有客户端主动停止、或其它错误码（服务端上
      // 可能还留着会话）才发 /cancel。closeSession() 只置 stop 并唤醒本线程，
      // cancel 仍由会话线程在这里发出；进程被杀来不及发的情况由连接时的残留会话
      // 处理兜底（见 cancelAndStartApp）。
      const bool hostEndedStream =
          outcome.connected &&
          (outcome.terminationCode == 0 || outcome.terminationCode == -1);
      if (client != nullptr && (outcome.stopped || !hostEndedStream))
        cancelServerSession(*client);
      if (outcome.stopped) break;
      if (outcome.restart) {
        // 用户手动重连：重新给 3 次「无画面自动重连」机会。
        autoReconnects = 0;
        g_autoReconnects.store(0);
        continue;
      }

      if (!outcome.connected) {
        // RTSP 握手失败通常意味着主机端还没从上一段会话里恢复。保持 1 s 退避
        // 重试（backoffMs 每次尝试都会重置回 1000），但把它计入自动重连的总
        // 预算（60 s），并用 W 级日志打出「主机尚未就绪，第 n 次重试」。
        if (autoReconnects > 0 && outcome.failedStage == STAGE_RTSP_HANDSHAKE) {
          const std::chrono::steady_clock::time_point now =
              std::chrono::steady_clock::now();
          if (now >= reconnectDeadline) {
            autoReconnects = 0;
            hostNotReadyRetries = 0;
            g_autoReconnects.store(0);
            logWarn("sunshine: reconnect budget exhausted; host still not ready");
            setHostPhase(
                host, SunshinePhaseFailed,
                "主机多次结束串流且始终无画面：请确认主机控制台已登录（未被远程桌面占用）后重试",
                "network", 0);
            host.waitForReconnect(host.user);
            continue;
          }
          ++hostNotReadyRetries;
          const long long leftMs = static_cast<long long>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  reconnectDeadline - now).count());
          logWarn(formatText(
              "sunshine: 主机尚未就绪，第 %d 次重试（剩余 %lld ms）",
              hostNotReadyRetries, leftMs > 0 ? leftMs : 0));
        }
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

      // 连接建立后被终止。先归类「主机结束串流」类的错误码：
      //   * 0  = ML_ERROR_GRACEFUL_TERMINATION（Limelight.h:421）：主机主动结束；
      //   * -1 = Limelight.h 里没有这个常量，它来自控制流：ControlStream.c:1600
      //          （ENET_EVENT_TYPE_DISCONNECT，即对端断开且没有 termination 消息）
      //          与 ControlStream.c:1369（断开握手超时）。Sunshine 的 /cancel、
      //          主机端应用退出都走这条；它与「真正的网络中断」在协议层不可区分。
      // 两者都按「终止前正常画面持续了多久」判定：
      //   * < 3 秒（含从未有画面）：用户还没看到稳定画面，视为主机显示会话状态
      //     变化（控制台会话被接回/远程桌面占用等）造成的瞬时黑屏 -> 自动重连；
      //   * >= 3 秒：用户已经正常观看 -> 主机主动结束，不自动重连。
      if (hostEndedStream && outcome.pictureMs >= 3000) {
        // 用户已经看到 >=3 秒正常画面：主机主动结束（关游戏、/cancel 等）。
        // 不自动重连，停在 disconnected，保留会话供手动重连。
        autoReconnects = 0;
        g_autoReconnects.store(0);
        logWarn(formatText(
            "sunshine: host ended the stream after %llu ms of picture; no auto "
            "reconnect",
            outcome.pictureMs));
        setHostPhase(host, SunshinePhaseDisconnected, "主机结束了串流", "none", 0);
        host.waitForReconnect(host.user);
        continue;
      }
      if (hostEndedStream) {
        // 主机结束串流、但正常画面不到 3 秒（或从未有画面）：典型场景是主机
        // 显示会话被接回/远程桌面占用（tscon 等），串流被主机踢掉，客户端只会
        // 看到黑屏。按「主机显示会话状态变化」处理：自动重连，最多 3 次
        // （2/4/8s 退避），每次重新走 /serverinfo ->（resume/cancel）-> /launch。
        if (autoReconnects >= 3) {
          autoReconnects = 0;
          g_autoReconnects.store(0);
          logWarn("sunshine: giving up after 3 no-picture auto reconnects; "
                  "host display session is probably unavailable");
          setHostPhase(host, SunshinePhaseFailed,
                       "主机多次结束串流且始终无画面：请确认主机控制台已登录（未被远程桌面占用）后重试",
                       "network", 0);
          host.waitForReconnect(host.user);
          continue;
        }
        ++autoReconnects;
        g_autoReconnects.store(autoReconnects);
        hostNotReadyRetries = 0;
        reconnectDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        const int delayMs = 2000 << (autoReconnects - 1);  // 2s / 4s / 8s
        logWarn(formatText(
            "sunshine: host ended the stream after %llu ms of picture (<3000 ms); "
            "auto reconnect %d/3 in %d ms",
            outcome.pictureMs, autoReconnects, delayMs));
        setHostPhase(
            host, SunshinePhaseReconnecting,
            formatText("主机结束了串流且没有画面，%d 秒后自动重连（%d/3）",
                       delayMs / 1000, autoReconnects),
            "network", delayMs);
        if (!host.sleepUnlessStopped(host.user, delayMs)) {
          if (host.stopRequested(host.user)) break;
        }
        continue;
      }

      // 其它错误码（网络中断、无视频流量等）：保持既有的 reconnecting +
      // 1s..4s 退避重连，/cancel 已在上面发过。
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
