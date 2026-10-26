# 天关 / Tianguan（TG）

面向 HarmonyOS 的 VNC + RDP 远程桌面客户端，覆盖手机 / 平板 / 智慧屏 / PC / 2in1，关注友好的界面与连接管理。当前版本 **1.0.0**（versionCode 100），变更见 [CHANGELOG.md](CHANGELOG.md)。

Tianguan (TG) is a community VNC and RDP client for HarmonyOS, available on phones, tablets, smart screens, PCs and 2-in-1 devices.

## 当前状态

**host 隔离真实 RA2_256 链路已通过；HarmonyOS 应用已在 MateBook Fold 与手机 PLA-AL10 上连上真实 VNC 服务**：原 host CTest **9/9** 通过，独立真实 TigerVNC 1.15.0 RA2_256 harness **3/3** 通过；OHOS API26 ARM64 客户端已交叉编译、链接成功。独立诊断 HAP 已用企业账号调试证书签名，在两台设备上均安装、启动成功，三项原生诊断全部 PASS。含 `ohos.permission.INTERNET` 的测试包在两台设备上均完成真实 TCP 连接、RA2_256 加密认证、1920×1200 首帧解码与键鼠事件发送（测试服务 `192.168.50.62:5901`）；安全类型协商已覆盖 RA2_256 / RA2 / VncAuth，错误密码在两台设备上均返回可读的 `Authentication failed`。已实现服务端指纹确认（首次 `NEEDS_CONFIRMATION`、确认后写入信任库、不匹配拒绝）与可输入的连接界面；PC 上人工确认流程已实测通过。首帧已能编码为 JPEG 并在界面以 `PixelMap` 显示（开发机对 62 实测为真实 XFCE 桌面画面）。输入侧已做服务端闭环验证：双击桌面 Home 图标后，服务端画面出现被真实打开的应用程序菜单、Thunar 文件管理器与 Application Finder 窗口，逐像素比较 26 万像素变化，证明点击到达并由服务端执行。本轮已从单会话长连接升级为多会话：原生侧每个会话独立（socket、线程、输入队列、帧缓冲、光标、generation、剪贴板），界面按会话启动独立 UIAbility 实例（`launchType: "multiton"`），主窗口只做连接管理；点 Dock 图标会弹出系统窗口选择器，主窗口与每个会话各一张卡片，可直接激活；系统托盘常驻（仅 2in1）。画面已从 Canvas 迁移到 XComponent(SURFACE) + 原生 EGL/GLES 渲染（旧 Canvas 路径保留常量开关，默认关闭），解决了 Canvas 每帧缓存纹理导致的显存增长与杀进程问题。双向自动剪贴板（仅纯文本）改为事件驱动 + 异步读取，本机 → 远端已恢复 `READ_PASTEBOARD` 声明，首次打开会话经系统授权后可用（PC 端端到端已实测），远端 → 本机已验证。键盘增加按键抬起修复（systemKeyCode 配对）。多实例窗口、托盘、退出、会话窗口键鼠、远端 → 本地剪贴板与 GL 显存稳定性均已在 SLM-W32 真机验证。客户端中文输入法（IME）组合输入与 X509 证书链校验仍未实现；VNC 与 RDP 主线均已在真机验证。

当前验证边界：已在 SLM-W32（HarmonyOS 7.0.0.111 / API 26）真机验证多实例窗口（打开/去重/关窗断会话/退出清理）、Dock 窗口选择器、托盘打开与退出、会话窗口键鼠、远端 → 本地剪贴板、GL 3 分钟显存平稳/无 GPU 错误/60 fps 内容下 fps ≥ 62、颜色与布局对齐，以及剪贴板事件驱动后周期性卡顿消失；**未做/受限**：本机 → 远端剪贴板需要 `READ_PASTEBOARD` 授权（ACL 已放行、声明已恢复，PC 端已实测；手机端尚未端到端实测）、主窗口隐藏态从托盘打开会话时主窗口会出现在会话窗口后面（外观限制；尝试修复会导致会话窗口不可见，已回滚）、滚轮未做可视化真机验收、IME 客户端组合输入 / X509 证书链校验未实现。详见 [应用说明](app/README.md) 的「验证边界」。

实验使用真实 TigerVNC `CConnection` / `ManagedPixelBuffer`。原 2×1 fixture 验证 Raw 像素 `112233` / `445566` 与键鼠，另有线程适配行为测试；独立真实 64×48 case 已证明 `authSuccess` → 首帧中心 RGB `112233` → 服务端 X observer 收到 `a` 按放及 `(16,12)` button1 按放。wrong-pin 在 `getUserPasswd` 前拒绝，wrong-password 有 owned 服务端唯一连接的精确认证失败及关闭证据。只打印 callback 后退出、不连接的假 client 会被 harness 判 FAIL，此防假通过反例不计入正式通过数。默认无参数不连接；真实测试须显式使用 `--ra2-test`，仅限隔离 loopback、非 5901 端口、credential-file 与 pin。pin 仅为上游 64-bit 截断指纹核对，本实验不是生产身份校验方案。

复现步骤、日志与验证边界见 [VNC 实验说明](experiments/vnc-core/README.md)。三个入口为[源码获取脚本](scripts/fetch-tigervnc.sh)、[五依赖构建脚本](scripts/build-vnc-deps.sh)（`host|ohos [jobs: 1..4]`）及 [pixman 构建脚本](scripts/build-vnc-pixman.sh)（`host|ohos-arm64 [jobs: 1..4]`）。五依赖每次创建 unique run，必须使用本次日志打印的 `PREFIX=`；pixman prefix 按所选模式及日志取用。上游固定为 TigerVNC v1.15.0（commit `8bdf8734ce12c8ed3c232496fc8a4ccdb04636b8`），外部 checkout 保持 clean，未 vendor 入仓库；源码、依赖与产物位于 `.work`。

OHOS 产物为 `.work/vnc-core-ohos-api26/vnc-core`：ELF `Machine: AArch64`，interpreter `/lib/ld-musl-aarch64.so.1`，NEEDED 仅 `libdeviceinfo_ndk.z.so`、`libc++_shared.so`、`libc.so`；无 host 库、RPATH/RUNPATH 或 `pthread_cancel` 符号，真实 RA2（`CSecurityRSAAES`）及 `RawDecoder` 实现符号保留。唯一平台补丁见 [ohos-thread.patch](experiments/vnc-core/ohos-thread.patch)：官方 `Thread` 析构调用 SDK 缺失的 `pthread_cancel`，因此仅在 OHOS 构建副本启用 `TG_OHOS_NO_PTHREAD_CANCEL`（host 专用测试也启用）；析构时仍 running 则 trap 终止进程，正常 `DecodeThread` 的 `stop()` / `wait()` 协作退出路径保持。编译副本有明确源码变更，不能称为完全未修改上游实现。

已确认目标 PC 为 MateBookFold SLM-W32、API26 ARM64。原未签产物在 `/data/local/tmp` 执行返回 126 / `Permission denied`；SDK 官方 `selfSign1` 副本经 `display-sign` 确认 selfsigned 后，同目录仍返回 126，不能归因于缺签名。按[官方 Docs/hishell 说明](https://consumer.huawei.com/cn/support/content/zh-cn16078461/)可新建目录并投递文件，但 Docs 下 `chmod` 被拒（remote 1）后即停止，未执行 capabilities，不能称为 Docs exec126 或 hishell 失败。`hdc` 返回 0 不等于程序返回 0，具体策略原因仍未确定；未改变设备安全属性，不能说 PC 程序已运行。[官方二进制签名文档](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/binary-sign-tool)及 Docs/hishell 支持也不等于 hdc 身份具有执行权限。

独立诊断 HAP `cn.alfadb.tianguan`（历史轮次 0.0.1 / code 1；当前版本 1.0.0 / code 100）、API26、`2in1` 与 `phone` 双设备，见[应用诊断说明](app/README.md)。真实共享库 `.work/hap-native-ohos/libtianguan_diag.so` 已构建为 AArch64 shared ELF，包含 NAPI async + promise；同源 host 诊断 **3/3 PASS**（RA2 工厂、完整内存 RFB 3.8 Raw 2×1、真实 Thread 协作退出），无 socket/main/alarm。签名使用企业账号调试证书与含两台设备的调试 Profile（`verify-app` 通过、包内 Profile 与外部 `.p7b` 一致），签名包 SHA-256 `02215cfacfcc703685afd795352e58bd0de0014029a707a26ec13fadcf1611f4`。MateBook Fold 上安装、启动成功，取回的 `diag-result.txt` 三项均 PASS（证据 `.work/device-runs/pc-diag/diag-result.txt`）；手机 PLA-AL10 安装成功但锁屏下无法远程启动。该轮 RA2 主线只有 host 证明；画面 UI、中文 IME、X509、RDP 与手机端 native 运行当时均未通过。

项目名称为「天关」，英文名 Tianguan，简称 TG；GitHub 仓库为 [alfadb/tianguan](https://github.com/alfadb/tianguan)。

名称沿用「太一」体系的星官命名意象。[《晋书·卷十一》](https://zh.wikisource.org/zh-hans/晉書/卷011)记载：“天关一星，在五车南，亦曰天门，日月之所行也，主边事，主关闭。”本项目借「天关」表达通往远端桌面的入口；“远端入口”是现代寓意。

这是独立社区项目，非华为官方项目，与华为不存在官方隶属或背书关系。

## 产品方向与技术内核

- 平台：HarmonyOS 手机 / 平板 / 智慧屏 / PC / 2in1，API 26（HarmonyOS 7.0+）。
- 体验：提供易用的连接管理、清晰的连接状态与友好的远程桌面界面。
- VNC：已集成 TigerVNC 1.15.0 内核，支持 RA2、RA2_256 与 VncAuth；X509Vnc 的证书链校验尚未实现。
- RDP：已集成 FreeRDP 3.9.0 内核，支持 NLA/TLS 连接、画面、键鼠、剪贴板与音频。
- 中文输入：须对 VNC 与 RDP 分别验证输入链路，包括本地输入法提交、协议编码与远端应用接收；不能从某一协议的结果推断另一协议支持情况。

TigerVNC 与 FreeRDP 均已集成到 HarmonyOS 应用。上游实验与第三方许可核对范围见 [VNC 实验说明](experiments/vnc-core/README.md)及[许可说明](docs/licensing.md)。

## 分阶段验收目标

以下阶段截至 1.0.0 的状态，保留设备、版本、配置与测试结果作为验收证据：

1. **OHOS 交叉编译与真机运行（已完成）**：VNC 内核已通过 OHOS API26 ARM64 交叉编译与链接；应用已在 HarmonyOS PC（SLM-W32）与手机（PLA-AL10）上安装、启动、连接真实服务并完成画面与键鼠交互。
2. **VNC RA2_256 主线（已完成）**：隔离真实 TigerVNC 1.15.0 已完成 RA2_256 认证、像素解码与服务端键鼠见证；设备端已连接服务端、显示远端画面并验证交互，RA2 与 VncAuth 协商亦已在设备上覆盖。
3. **VNC X509Vnc（未完成）**：X509Vnc 的证书链验证尚未实现，包括可信证书成功连接，以及不可信、过期或身份不匹配证书的处理。
4. **RDP 安全连接与输入（已完成）**：已在真机验证 NLA/TLS 连接、远端画面、键鼠、剪贴板与音频；中文输入通过远端 Windows 输入法完成，客户端不做 IME 转发。

## 构建发布包

上架用的 `.app`（App Pack）由发布脚本构建，默认产出未签名包：

```bash
bash scripts/build-release.sh
# 产物：app/build/outputs/default/app-default-unsigned.app
```

脚本在 `app/` 下执行的实际命令：

```bash
<SDK>/bin/hvigorw --no-daemon clean assembleApp --mode project \
  -p product=default -p buildMode=release
```

`assembleApp` 必须配 `--mode project`，不能传 `-p module=...`（模块模式参数会被拒绝）。脚本会校验包内含 `pack.info` 与 `entry-*.hap`，并打印路径、大小与 SHA-256。

可选签名：下面 5 个环境变量全部提供时，脚本用仓外的 `HapSignLauncher` 调用 SDK 的 `hap-sign-tool` 签名，输出到 `.work/release/`；缺少任意一个只产出未签名包并提示。签名材料与口令文件必须放在仓库外。

| 变量 | 含义 |
| --- | --- |
| `TIANGUAN_RELEASE_P12` | 发布证书私钥 keystore（`.p12`） |
| `TIANGUAN_RELEASE_CER` | 发布证书（`.cer`） |
| `TIANGUAN_RELEASE_PROFILE` | 发布 Profile（`.p7b`） |
| `TIANGUAN_RELEASE_P12_PASS_FILE` | keystore 口令文件（单行，权限 0600） |
| `TIANGUAN_RELEASE_ALIAS` | keystore 内的 key alias |

口令只经文件读入，不出现在命令行参数、环境变量或日志里。日常调试仍用 `bash app/build.sh` 构建未签名 HAP。发布流程、签名字段与 AGC 注意事项见 [发布构建与签名方案](docs/release-pipeline.md)。

## 许可与贡献

本项目自有代码采用 **GNU General Public License v3.0 or later（GPL-3.0-or-later）**：你可以依照 GPL 第 3 版，或自由软件基金会发布的任何后续版本，使用、修改和分发本项目自有代码。

[LICENSE](LICENSE) 保留 GPLv3 官方完整原文；“or later”的项目授权选择由本节与[许可说明](docs/licensing.md)明确。上游文件仍须遵循各自许可，不能据此推定所有第三方文件都采用本项目许可。

参与方式见 [CONTRIBUTING.md](CONTRIBUTING.md)。
