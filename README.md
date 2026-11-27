# 天关 / Tianguan（TG）

**HarmonyOS 上的原生 VNC / RDP / Sunshine 远程桌面客户端。**

GPL-3.0-or-later · 当前版本 1.1.0 · API 26+ · 包内支持手机 / 平板 / PC（2in1）；1.0.0 上架只勾 PC/2in1，1.1.0 起纳入手机与平板

![天关的连接列表](docs/store/screenshots/pc-01-main.png)

天关把每个远程会话做成一个独立窗口，从 Dock 或系统窗口选择器直接切换，主窗口只负责连接管理。它连接运行 VNC 或 RDP 服务的主机，包括 Windows、Linux 与树莓派；也可以连接运行 Sunshine 的主机，以 Moonlight 串流协议低延迟查看桌面。

画面经原生 EGL/GLES 渲染，不经过 WebView。VNC 已在鸿蒙 PC 与手机验证，RDP 与 Sunshine 已在鸿蒙 PC 验证。1.0.0 已上架 AppGallery；仓库不提供预编译安装包。

Tianguan (TG) is a native VNC, RDP and Sunshine client for HarmonyOS. Its name comes from a star gate in the *Jin Shu* (Book of Jin). It also connects to hosts running Sunshine through the Moonlight streaming protocol. The 1.0.0 release targets PC and 2-in-1 devices only; 1.1.0 adds phone and tablet support. It is an independent community project, not affiliated with or endorsed by Huawei.

## 功能

连接以卡片列表呈现，保存过的主机按分组排列，可按名称或 `host:port` 搜索。每张卡片单独记录协议与端口，VNC、RDP 与 Sunshine 分开保存。密码存入系统密钥库，不写入配置文件。

每个会话对应一个独立窗口，拥有自己的 socket、线程、帧缓冲与剪贴板；关闭窗口即结束会话。2in1 上主窗口可驻留系统托盘，Dock 与系统窗口选择器各有一张卡片。

RDP 走 GFX 图形管线与 H.264/AVC 解码，VNC 解码原始像素，Sunshine 的视频经系统硬解为 BGRA，三者共用同一条原生 EGL/GLES 渲染路径，画面经 XComponent(SURFACE) 直出。三种协议都支持双指缩放与平移。

RDP 经 rdpsnd 输出 PCM 音频，实测 44.1 kHz 立体声；VNC 协议不包含音频通道，VNC 会话没有声音；Sunshine 的音频为 Opus，经 OH_AudioRenderer 播放。剪贴板双向同步纯文本，单次上限 1 MiB，`\r\n` 归一为 `\n`；本机 → 远端需 `READ_PASTEBOARD` 授权；Sunshine 协议没有剪贴板通道，Sunshine 会话不支持剪贴板同步。

Sunshine 连接运行 Sunshine 的主机，默认端口 47989。首次连接需配对：应用里输入 4 位 PIN，再到主机 Sunshine 网页的 PIN 页提交同一个 PIN；配对后服务端证书指纹固定，变化即拒绝连接。连接表单提供「画质」档位：流畅 1280×720 / 60 fps / 15 Mbps、均衡 1920×1080 / 60 fps / 20 Mbps（默认）、高清 2560×1440 / 60 fps / 40 Mbps、超清 3840×2160 / 60 fps / 80 Mbps，另有自定义（宽、高、帧率、码率、视频编码 自动/H.264/HEVC）；分辨率、帧率、码率在重连后生效。视频经系统硬解（H.264/HEVC，8-bit）后转 BGRA，与 VNC/RDP 共用渲染路径；音频为 Opus，码率低于 15 Mbps 时降为普通音质（协议规定）。键鼠为绝对鼠标加键盘（X11 keysym → Win32 VK），按物理按键转发；应用名手填，默认「Desktop」。Sunshine 会话的分辨率由画质档位决定，不跟随窗口。

窗口宽度低于 600 vp 时（手机与较窄的 2in1 窗口）采用单列布局，旋转后重新排列，支持双指缩放。长按约 0.5 秒映射为右键，返回即断开会话。

![RDP 会话：Windows 桌面](docs/store/screenshots/pc-02-rdp.png)

![VNC 会话：Linux 桌面](docs/store/screenshots/pc-03-vnc.png)

## 设计取舍

做天关的直接原因是 HarmonyOS 上缺少可直接使用的原生远程桌面客户端。取舍集中在渲染、会话、身份校验、输入、项目边界与上游依赖六处。

- 原生渲染而非 WebView：画面经 XComponent(SURFACE) + EGL/GLES 直出，避免每帧拷贝与额外一层 JS 桥。
- 一个会话一个 UIAbility（`launchType: "multiton"`）：窗口即会话，关窗即断开，避免窗口关闭后连接仍留在后台的状态。
- 身份校验用 TOFU 指纹而不是证书链：VNC 侧上游只提供 64 位截断指纹，RDP 侧为 SHA-256。
- 中文输入交给远端输入法：客户端不实现 IME 组合输入，只按物理按键转发，映射表之外的 Unicode 无法直接输入。
- 上游不 vendor：TigerVNC 与 FreeRDP 按固定版本、moonlight-common-c 按固定提交（上游没有 tag）由脚本获取并交叉编译，libopus 同样由脚本按固定提交获取；仓库只保留脚本。
- 流媒体内核复用 moonlight-common-c，配对、HTTP 与证书层自写：RTSP/RTP 收流与音视频解包交给上游内核，nvhttp 端点、`/pair` 握手与服务端证书指纹固定由项目自己实现。

## 名字：天关

天关是《晋书·卷十一·天文志上》记载的一颗星，原文为「天关一星，在五车南，亦曰天门，日月之所行也，主边事，主关闭。」原文见 [维基文库](https://zh.wikisource.org/zh-hans/晉書/卷011)。它在五车之南，又名天门；「日月之所行」指日月运行经过此处，「主边事，主关闭」指其掌管边关与门户启闭。

远程桌面的语义与此接近：跨过网络进入另一台机器，连接与断开对应门的开合。这条联系是产品层面的引申，古义以上面的引文为准。

命名沿用一套星官体例：项目所有者另一套以星官命名的项目「太一」先用了「柱史」「阁道」等名字，天关是这套体例里选出的下一个星官名。选名约束是可查古籍原文、能配合太一体系、不与已有名字重复、英文用拼音。

英文名 Tianguan，简称 TG；仓库 [alfadb/tianguan](https://github.com/alfadb/tianguan)，包名 `cn.alfadb.tianguan`。曾用名「鸿桌 / HongDesk（HD）」已不再使用；中科院的爱因斯坦探针卫星也叫「天关」，两者只是同名。天关是独立社区项目，非华为官方，与华为没有隶属或背书关系。

## 当前状态

1.0.0 已覆盖安装到 PC 与手机（`install -r`，连接与密钥库保留）；PC 上 VNC 与 RDP、手机上 VNC 各一条连接均已复测连通。Sunshine 为 1.1.0 之后新增，尚未发版。1.0.0 已上架 AppGallery（2026-11-27 用户确认在架，记录日期 2026-11-05）。1.1.0（versionCode 110）的本地发版材料已备：版本号已写入 `app/AppScope/app.json5` 与 `app/build-profile.json5`（两处必须同改，product 覆盖 `app.json5`），CHANGELOG 已增补；发布证书与 Profile 已办，发布签名包 `.work/release/app-default-signed.app` 已产出并通过 `hap-sign-tool verify-app` 验签；尚未提审。本次计划把手机与平板纳入同一应用的「支持设备」：包内 `deviceTypes` 一直是 `phone/tablet/2in1`，纳入手机/平板不需要改包、不需要重建包配置，只需在 AGC 勾选。APP 备案办理中（2026-11-27 用户确认）。版权材料按 2026-11-27 核实的口径：AGC 勾「手机」后，版本信息里的「应用版权证书或代理证书」在发布范围含中国大陆时必选，该栏接受《免责函》《计算机软件著作权登记证书》《APP电子版权认证证书》《软件著作权认证证书》，须在提审前备妥其中一种；官方未给「可后补/承诺」通道，且该栏必选的说法与《应用资质审核要求》80301 把软著列为「非必选资质」存在冲突，建议提审前用 AGC 工单确认《免责函》/《APP电子版权认证证书》能否满足该栏。版本变更见 [CHANGELOG.md](CHANGELOG.md)。

已验证：

- VNC 连接真实服务端，完成 RA2_256 认证、画面与键鼠交互；RA2 与 VncAuth 的协商在 PC 上覆盖过。
- RDP 连接 Windows，完成 NLA 登录，画面、键鼠、剪贴板（双向，含手机端）、音频与 GFX + H.264/AVC 均实测通过。
- Sunshine 配对一次成功；默认档位下协商到 HEVC，1920×1080@60 出画，首帧在配对完成后约 2 秒；H.264 与 HEVC 均出画。
- Sunshine 键盘解锁 Windows 锁屏、鼠标可用（用户实测）；音频可听，视频码率 20 Mbps 时音频为高音质档（每声道 256 kbps，设备统计约 200 包/秒），用户反馈音质明显优于 5 Mbps 时。
- Sunshine 强杀应用后重连可自动接管残留会话（约 4 秒出画）；画质档位切换生效；原有 VNC 卡片回归正常。
- 多会话、多窗口、Dock 窗口选择器与 2in1 系统托盘可用；关窗即断开会话。
- GL 呈现连续采样 3 分钟，显存平稳，无 GPU 错误。
- 滚轮经 `onAxisEvent` 转发为 RFB 按钮 4 / 5，PC 真机实测滚动正常。

未实现：

- VNC 的 X509Vnc：客户端没有 VeNCrypt/TLS 通道，依赖构建未启用 GnuTLS；服务端身份目前只做指纹 TOFU。
- RDP 的磁盘、打印机、智能卡、摄像头与多显示器重定向：通道未接入，当前只有画面、键鼠、剪贴板与音频。
- Sunshine 的剪贴板同步：协议没有该通道，双向同步对 Sunshine 会话不可用。
- Sunshine 的手柄、相对鼠标与映射表之外的文本直接输入：协议支持 UTF-8 文本事件，尚未接入。
- Sunshine 的 AV1、HDR / 10-bit：均未接入。
- Sunshine 的应用列表选择：当前手填应用名。

未验证：

- RDP 服务端指纹变化后的拒绝路径：只有代码审查；构造该场景要更换或伪造服务端证书，尚未在真机复现。
- 手机与平板上的 Sunshine 会话。
- Sunshine 的长时间稳定性、多会话与断线重连：流媒体内核为进程级单例，同时只允许一路 Sunshine 串流。
- Sunshine 的 5.1 / 7.1 声道。

构建通过不等于功能通过：已验证、未实现与未验证分开记录。逐项证据见[应用说明](app/README.md)的「验证边界」，VNC 内核实验的复现步骤见 [VNC 实验说明](experiments/vnc-core/README.md)，Sunshine/Moonlight 协议实验见 [实验说明](experiments/sunshine-core/README.md)。

## 边界

- 非华为官方项目，与华为没有隶属、背书或合作关系。
- 不是远程桌面服务端：目标主机需自行运行 VNC、RDP 或 Sunshine 服务（本项目不是 Sunshine 服务端）。
- 包内只有手机、平板与 PC（2in1），不含智慧屏；1.0.0 上架只勾 PC/2in1，1.1.0 起按手机 + 平板 + PC/2in1 勾选。设备类型只能增不能减：应用一旦发布，升级只能增加设备类型，不能删除已选设备。
- RDP 不支持 HEVC：GFX 协议只定义 AVC；Sunshine 会话支持 H.264 与 HEVC，AV1 未接入。

## 构建

仓库不提供预编译安装包，需要自行构建。

```bash
bash app/build.sh                  # 未签名 HAP
bash scripts/test.sh               # 原生 CTest + ArkTS 单测，不需要设备
bash scripts/build-release.sh      # 未签名 .app
```
原生库先由 `scripts/fetch-tigervnc.sh`、`scripts/build-vnc-deps.sh ohos`、`scripts/build-rdp-deps.sh ohos`、`scripts/fetch-moonlight.sh`、`scripts/build-sunshine-deps.sh ohos` 准备到 `.work/`，上游源码不入库。脚本内固定了本机命令行工具路径，换机器时需要修改。签名与上架流程见[发布构建与签名方案](docs/release-pipeline.md)。

## 许可与贡献

天关自有代码采用 GPL-3.0-or-later：LICENSE 为 GPLv3 官方原文，「or later」由 README 与[许可说明](docs/licensing.md)明确。上游 TigerVNC、FreeRDP、FFmpeg、moonlight-common-c（GPL-3.0，含 MIT 的 enet、nanors）与 libopus（BSD-3）各按自己的许可分发，源码不 vendor 进仓库。参与不要求签署 CLA，方式见 [CONTRIBUTING.md](CONTRIBUTING.md)；源码在 https://github.com/alfadb/tianguan 。上架用的最终用户协议见 [docs/store/eula.md](docs/store/eula.md)：它与 GPL 兼容，不改变 GPL 授予的权利。
