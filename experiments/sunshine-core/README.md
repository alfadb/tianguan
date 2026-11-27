# Sunshine / Moonlight 接入实验（host）

本目录是为天关接入第三种远程桌面协议 Sunshine/Moonlight 做的 **host 侧最小实验**，对应 app 接入的第 1 步：完成配对、取会话参数、把 H.264/HEVC 码流拉回本地并校验。**结论：控制面与视频链路已在两台主机验证——本机 Linux Sunshine 与 Windows 主机 192.168.50.12（Alfadb-V-Win）；音频两次都未通过（本机 PulseAudio 采集失败，.12 声音不在被采集的控制台会话），只保留网络侧旁证；鸿蒙侧当时尚未开始。** 鸿蒙侧已于 2026-11-27 接入并在鸿蒙 PC 验证（配对、H.264/HEVC 画面、键鼠、音频），见根 README 与 app/README.md；本目录保留为 host 侧复现与排查工具。本目录自有代码 GPL-3.0-or-later；上游 moonlight-common-c 为 GPL-3.0，固定提交、不 vendor。

## 组成

三部分，产出两个可执行文件：

| 部分 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| common | [src/common](src/common) 的 `identity` / `xml_util` / `nv_http` / `pairing` | 客户端身份、极简 XML、六个 nvhttp 端点、`/pair` 五阶段、服务端证书指纹固定 | OpenSSL、Threads |
| ctl | [src/ctl/main.cxx](src/ctl/main.cxx) | `identity` / `serverinfo` / `applist` / `launch` / `resume` / `cancel` / `unpair` / `pair` / `selftest` | common |
| stream | [src/stream](src/stream) | 只吃 HTTP 层给出的值，驱动 common-c 拉流、统计帧、dump Annex B、输入冒烟 | common-c、enet |

边界要分清：**六个 nvhttp 端点（serverinfo、applist、launch、resume、cancel、unpair）与 `/pair` 握手、证书及指纹固定全部自己实现**——moonlight-common-c 不发任何 HTTP。common-c 负责 RTSP 握手、RTP 收流与音视频解包、FEC/纠错重组（nanors）、控制通道与输入通道；`/launch` 给出的 `sessionUrl0`、`rikey`、`rikeyid` 由 ctl 产出、原样交给 stream。

## 构建与运行

工作目录为仓库根。前置：CMake ≥3.20、Ninja、C/C++17 编译器、OpenSSL 开发包（common-c 自身也 `find_package(OpenSSL)` 并链接 crypto）。本机实测工具链为 CMake 3.31.6、Ninja 1.12.1、GCC 14.2.0、OpenSSL 3.5.9。

```sh
bash scripts/fetch-moonlight.sh     # 固定提交 + 递归子模块；已存在目录只校验、不删除不重置
# common-c 默认出共享库，且上游只有 enet 带 install 规则，库与 common-c 头需手工组装
cmake -S .work/moonlight-common-c -B .work/sunshine-deps/build-common-c -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/.work/sunshine-deps/host"
cmake --build .work/sunshine-deps/build-common-c
cmake --install .work/sunshine-deps/build-common-c   # 只装 enet 的 libenet.a 与 include/enet/*
mkdir -p .work/sunshine-deps/host/lib/static
cp .work/sunshine-deps/build-common-c/libmoonlight-common-c.a .work/sunshine-deps/host/lib/static/
cp .work/moonlight-common-c/src/*.h .work/sunshine-deps/host/include/

cmake -S experiments/sunshine-core -B .work/sunshine-spike/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .work/sunshine-spike/build
```

产出 `.work/sunshine-spike/build/tg-sunshine-ctl` 与 `tg-sunshine-stream`。注意：本次 `CMakeCache.txt` 记录的安装前缀是 `.work/sunlight-deps/host`（少一个 `e`），而实际目录是 `.work/sunshine-deps/`；复现时按实际目录写正确拼写，不要照抄缓存。

配对（PIN 只从标准输入读，不进命令行）：

```sh
TG=.work/sunshine-spike/build/tg-sunshine-ctl
$TG identity                      # 首次自动生成证书/私钥/uniqueid，可重复执行
printf '%s\n' '<4位PIN>' | $TG pair
# 服务端收到首个请求后会挂起，等你在 Sunshine 网页管理页的 PIN 页提交同一个 PIN
$TG serverinfo && $TG applist
$TG launch --appid 881448767 --mode 1280x720x60   # 输出 sessionUrl0 / rikey / rikeyid
```

`rikeyid` 打印为十进制；stream 要的是同一 32 位值的 8 位十六进制（`printf '%08x'`）。`--width/--height/--fps` 默认 1280x720@60，须与 `--mode` 一致。`--appid` 以 `applist` 实际输出为准，示例里的 ID 只是本机 Desktop。

```sh
DEC_RIKEYID=380662115               # 换成 /launch 打印的十进制 rikeyid
.work/sunshine-spike/build/tg-sunshine-stream \
  --session-url 'rtspenc://127.0.0.1:48010' --appversion '7.1.431.-1' \
  --gfeversion '3.23.0.74' --codec-support 0x301 \
  --rikey '<32位hex>' --rikeyid "$(printf '%08x' "$DEC_RIKEYID")" \
  --formats h264,hevc --seconds 30 --input-smoke \
  --dump-video .work/sunshine-spike/dump/h264.bin
$TG cancel
```

只给 `--formats h264` 才会协商到 H.264；给 `h264,hevc` 时本机服务端优先 HEVC，给 `h264,hevc,av1` 而服务端无 AV1 时自动落到 HEVC。`--repeat N` 可在同一进程内连跑 N 次作自检。

## 已验证（2026-11-27）

两轮实测：本机 Linux Sunshine 2026.516.143833（KMS 抓屏 nvidia-drm card2 + NVENC）与 Windows 主机 192.168.50.12。离线解码用 ffmpeg/ffprobe 7.1.5；证据均在 `.work/sunshine-spike/`，Windows 轮对应 `win-run-*`（修复前）、`win2-*`（修复后视频）、`win3-*`（音频）。

### 本机 Linux Sunshine

| 项目 | 已证实 | 证据 |
| --- | --- | --- |
| 上游与构建 | 固定提交 `b4ee1cea3d19e13c0b37c26585e6b1ee174effcb`（2026-11-22），子模块 enet `98dbb897ac4a7208038e0782cddedf89dcf0723a`、nanors `a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29`；`.work/sunshine-deps/host/` 下两个静态库与 `include/` 就位，实验工具 configure/build 退出 0 | `build.log`、`configure.log`、`.work/sunshine-deps/host/` |
| 配对 | 五个阶段一次通过（前四阶段明文 HTTP 47989，`pairchallenge` 走 HTTPS 47984 并带客户端证书）：`getservercert` 9039 ms（等网页提交 PIN），其余四阶段 2 / 6 / 4 / 10 ms（合计 22 ms）；成功后写入 `server-127.0.0.1-47984.pem/.sha256`，此后所有 TLS 请求强制比对指纹 | `run-pair.log` |
| serverinfo | 明文 HTTP 200、HTTPS 200 且 `PairStatus: 1`；`HttpsPort: 47984`、`ServerCodecModeSupport: 769`、`MaxLumaPixelsHEVC: 1869449984`、未提供 AV1 | `run-serverinfo.log` |
| applist | 3 项：Desktop `881448767`、Low Res Desktop `303580669`、Steam Big Picture `1093255277`（均 HDR=1） | `run-applist.log` |
| launch / cancel | 三次循环均 HTTP 200，`state` 回到 `SUNSHINE_SERVER_FREE` | `run-launch-*.log`、`run-cancel-1.log` |
| H.264 | `h264_nvenc`（High）30 秒 1794 帧（约 59.0 fps）、IDR 1、起始码与 IDR 顺序违规均 0、14,430,921 字节、首帧 460.4 ms | `run-stream-h264.log`、`dump/h264.bin` |
| HEVC | `hevc_nvenc`（Main）30 秒 1794 帧、IDR 1、违规 0、1,772,470 字节、首帧 401.0 ms | `run-stream-hevc.log`、`dump/hevc.bin` |
| 离线解码 | ffmpeg/ffprobe 7.1.5 解 H.264 与 HEVC 均 0 错误（stderr 为空），抽帧 PNG 1280×720 | `dump/decode-*.err`、`dump/frame-*.png` |
| AV1 回退 | 声明 `h264,hevc,av1`、服务端无 AV1 时协商到 HEVC：10 秒 595 帧、587,858 字节 | `run-stream-av1.log`、`dump/av1-fallback.bin` |
| 输入冒烟 | 移动、左键按下/抬起、键按下/抬起、UTF-8 文本、滚轮共 7 个调用返回码全 0 | `run-stream-*.log` |
| 重启后 | Sunshine 重启后无需重新配对即可拉流：10 秒 600 帧 HEVC、首帧 333.1 ms | `run-stream-postrestart.log` |

### Windows 主机 192.168.50.12

| 项目 | 已证实 | 证据 |
| --- | --- | --- |
| 配对 | 五个阶段一次通过（等网页提交 PIN 约 96 秒 / 95793 ms；其余四阶段 2 / 3 / 8 / 19 ms，合计 32 ms）；pin 按主机单独存为 `server-192.168.50.12-47984.pem/.sha256` | `win-run-pair.log` |
| serverinfo / applist | HTTPS `PairStatus: 1`；`ServerCodecModeSupport: 1835777`（`0x1C0301`：H.264 + HEVC，含 Main10 与 4:4:4，无 AV1）；applist 为 Desktop `881448767`、Steam Big Picture `1093255277` | 当次命令输出（未单独留日志）；`win2-stream-h264.log` 的 `codec support=0x1C0301` |
| 首轮（修复前） | 控制通道正常、会话进入 BUSY，但 0 帧 0 音频包，11.51 秒后以「无视频流量」断开；根因见「已知环境陷阱」 | `win-run-stream-h264*.log` |
| H.264（修复后） | 20 秒 398 帧（约 19.06 fps）、IDR 1、起始码/IDR 顺序违规 0、1,107,248 字节、首帧 2137.9 ms；ffprobe 为 H.264 High 1280×720，ffmpeg 解码 0 错误 | `win2-stream-h264.log` |
| HEVC（修复后） | 20 秒 395 帧（约 18.97 fps）、IDR 1、违规 0、594,024 字节、首帧 2240.9 ms；ffprobe 为 HEVC Main 1280×720，ffmpeg 解码 0 错误 | `win2-stream-hevc.log` |
| 画面内容 | 第 2 秒帧 signalstats `YAVG≈51` / `YMIN=16` / `YMAX=235`，非全黑；画面为该机控制台会话的锁屏画面，内容正确。约 19 fps 是桌面静止时编码器按需出帧，不是网络问题 | `dump/win-frame-{h264,hevc}-small.png`（320×180 缩放，YMAX 会略低；原始 dump 已删除） |
| launch / cancel | 三次均 HTTP 200，`state` 回到 `SUNSHINE_SERVER_FREE` | `win2-launch-*.log`、`win3-launch-*.log`；cancel 与 state 为当次输出 |

## 未通过

- 音频（本机 Linux）：服务端 `Couldn't connect to pulseaudio: Access denied` → `There will be no audio`，整段 0 包。这是本机音频采集环境问题，不是协议不通。
- 音频（Windows .12）：我们自己的复测 0 包——Sunshine 采集的是控制台会话（锁屏所在会话）的默认播放设备，而操作者只能 RDP 进另一个会话，计划任务播放的声音没有进被采集的会话。两台主机上客户端音频通道都协商成功（48 kHz 立体声，`samplesPerFrame=240`，`audioConfiguration=0x302CA`），但解码/播放仍未实现，本机也没有 libopus。**旁证（未独立复现）**：网络侧 18:36 用同一客户端（`tg-sunshine-stream`，HEVC 16 秒）实测收到 2034 个音频包（他们记录的数字），说明服务端有声音时音频包能到达本客户端。
- 画面全黑：仅本机 Linux——抓的是显卡 HDMI 输出，该输出上没有桌面内容（帧统计 `YAVG=YMAX=16`）；Windows .12 上画面内容已证实（锁屏画面，见上表）。

## 未验证

- 输入实际效果：只到「发送返回 0」；本机 Sunshine 无 uinput（虚拟触摸屏/笔平板创建失败），Windows 轮未做输入测试，远端是否真的收到未验证。
- 长时间稳定性（Linux 单次 30 秒、Windows 单次 20 秒）、多会话、断线重连。
- 鸿蒙侧全部：交叉编译、AVCodec 硬解、音频输出、ArkTS/NAPI 集成。
- Gen 6 及更早的 SHA-1 配对：只实现 Gen 7+（SHA-256），旧代未测。
- 网络侧那次 2034 音频包的实测：我们没有独立复现。

## 已知环境陷阱

**症状**：控制通道正常、会话进入 BUSY，但视频/音频 0 包，约 11 秒后以「无视频流量」断开（`ping_timeout` 默认 10 秒）。

**根因（网络侧定位）**：Windows 虚拟机的 VirtIO 网卡驱动 netkvm（virtio-win 0.1.285）在 USO 开启时，把批量 UDP 发送合并成 UDP 头长度为 0 的大帧（4266–8490 字节），被节点连接跟踪当坏包丢弃，流停在 `UNREPLIED`；控制通道是逐个发送的小包，所以不受影响。升级到 virtio-win 0.1.302 后修复。集群侧 NetworkPolicy 为 0、flannel 出向 NAT 按网段不按端口，从未拦截。上游 issue：<https://github.com/virtio-win/kvm-guest-drivers-windows/issues/1525>。

**排查顺序**：以后遇到「控制通道通、视频/音频不通」，先看服务端日志有无 `CLIENT CONNECTED`、编码器是否启动，再请网络侧在节点抓包/查 conntrack，不要先怀疑客户端或应用层。

**规避**：用本宿主机上其他 Windows 虚拟机做串流主机，需 virtio-win ≥ 0.1.300 或关闭 USO。

**PING 与超时**：客户端视频/音频 PING 为 20 字节（新协议）或 4 字节（旧协议）；Sunshine 依 PING 的来源地址回发媒体，`ping_timeout` 默认 10 秒——这解释了首轮约 11 秒的「无视频流量」断开。

## 已知限制与注意事项

- Sunshine 没有 `/unpair` HTTP 端点，`unpair` 子命令对它是 404；解除配对要走网页管理页。
- 主机重装或清空状态后，先删状态目录里的 pin（`server-<host>-<httpsPort>.pem/.sha256`）再重新配对，否则旧指纹会拒绝新证书。
- 第一阶段默认等 300 秒（`--pin-wait` 可调）；其余各阶段 IO 超时 10 秒，不要据此判断长稳。
- PIN 只从 stdin 读、TTY 下关回显；不写命令行、日志、文档。状态目录默认 `.work/sunshine-spike`，私钥 0600 且不打印，pin 加载时从 PEM 重算 SHA-256 比对。
- 上游只有 enet 带 install 规则；换机器或清 `.work` 后需按上面的命令手工组装库与头。
- `scripts/fetch-moonlight.sh` 固定提交、不跟随上游 HEAD；已有 checkout 只校验，不一致直接报错，不自动删除或重置。

## 与 app 接入的关系

可直接迁入：

- `identity`：自签 RSA-2048 / X.509、`CN=NVIDIA GameStream Client`、64 位 `uniqueid`，纯 OpenSSL，无平台 API。
- `xml_util`：解析 serverinfo / applist / launch / pair 响应足够。
- `nv_http`：六个端点、HTTP 状态码与 NV `status_code` 双重错误、TLS 客户端证书、pin 比对；app 已静态链接 OpenSSL 3.5.9（RDP 依赖），可复用同一套。
- `pairing`：五阶段流程与 TOFU 固定，失败带阶段名。
- `STREAM_CONFIGURATION` 的填法：`LiInitializeStreamConfiguration` 后填 width/height/fps/bitrate；`packetSize=1392`、`streamingRemotely=STREAM_CFG_AUTO`、`audioConfiguration=AUDIO_CONFIGURATION_STEREO`、`supportedVideoFormats` 用掩码；SDR 用 `COLORSPACE_REC_709` / `COLOR_RANGE_LIMITED`、`ENCFLG_NONE`；`remoteInputAesKey` 放 16 字节 rikey，`remoteInputAesIv` 前 4 字节放 rikeyid 大端、其余清零。

必须重写：

- 视频回调：`drSetup` / `drSubmitDecodeUnit` → OHOS AVCodec 硬解。现在只校验 Annex B 并 dump，不渲染。
- 音频回调：`arInit` / `arDecodeAndPlaySample` → libopus 解码 + `OH_AudioRenderer`，且先解决服务端采集。
- 宿主桥：`app/native/napi_bridge.cxx` 与 [docs/napi-contract.md](../../docs/napi-contract.md) 的 sessionId/generation 模型；会话、帧镜像、EGL 渲染复用 VNC/RDP 已有路径，不按协议分叉。
- 状态目录：从 `.work/sunshine-spike` 改为应用沙箱（`filesDir`），证书/私钥按现有落盘与权限惯例处理。

下一步：① OHOS API26 arm64 交叉编译 common-c（enet + nanors），确认线程/socket 可用；② 用 AVCodec 解同一份 dump，对照 1280×720/60 与首帧延迟；③ 解决服务端音频采集（Linux 的 PulseAudio、.12 的被采集会话）后验证音频包，再接 libopus；④ 输入接 OHOS 注入；⑤ 长稳与断线重连；⑥ 文件级许可审计。

## 许可

moonlight-common-c 为 GPL-3.0，固定提交 `b4ee1cea…`，由 `scripts/fetch-moonlight.sh` 放在 `.work/`，**不 vendor 进仓库**；与本项目 GPL-3.0-or-later 兼容。其子模块 enet 与 nanors 均为 MIT，保留其版权与许可声明。引入 app 前须按项目惯例做文件级许可审计（可参考 [VNC 实验说明](../vnc-core/README.md) 的「有界许可核对」），并更新 [docs/licensing.md](../../docs/licensing.md) 与最终用户协议 [docs/store/eula.md](../../docs/store/eula.md) 的第三方表；本实验未发布二进制，也未复制上游代码进仓库。
