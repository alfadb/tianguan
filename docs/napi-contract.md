# NAPI 接口契约（多会话 + 剪贴板）

本文件定义 `libtianguan_diag` 在下一阶段对外暴露的接口。原生与 ArkTS 两侧都以此为准；改动必须先改本文件。

## 设计原则

- **每个运行期会话有独立 id**：所有运行期接口第一个参数都是 `sessionId`（字符串，由 `createSession` 返回）。
- **代次号防串线**：每次重连 `generation` 加一。迟到的帧、迟到的输入、针对旧连接的身份确认都必须被拒绝。
- **像素所有权在 ArkTS**：调用方提供 `ArrayBuffer`，原生只往里写，不把内部缓冲指针交出去。
- **状态结构化**：返回 JSON，界面不解析英文错误串。

## 会话生命周期

| 接口 | 说明 |
|---|---|
| `createSession(optionsJson: string): string` | 创建会话并异步连接。返回 `sessionId`；返回不代表已连上，状态用 `sessionStatus` 查。 |
| `closeSession(sessionId: string): string` | **非阻塞**：立即摘除会话、停止读、清输入，线程回收交给后台。返回 `CLOSED` / `INVALID_SESSION`。重复关闭安全。 |
| `pendingCloses(): number` | 尚未完成线程回收的已关闭会话数。退出流程轮询到 0 或期限（当前 3 秒）后强退。 |
| `listSessions(): string` | 返回 JSON 数组，每项含 id、state、width、height、generation、profileName，**以及 `host`/`port`**（用于给孤儿会话恢复端点，信任确认依赖它）。 |
| `sessionStatus(sessionId: string): string` | 返回该会话状态 JSON。 |
| `reconnectSession(sessionId: string): string` | 立即重连，`generation` 递增。`state=trust-required` 调用时把已展示的 `fingerprint` 记为信任锚。 |
| `forgetTrustedFingerprint(sessionId: string): string` | 清空该会话的 `expectedPin` 与 `fingerprint`，并按与 `reconnectSession` 相同的逻辑立即重连（`generation` 递增）。返回 `OK` / `INVALID_SESSION`。 |
| `submitPin(sessionId: string, pin: string): boolean` | **Sunshine/Moonlight 专用**：把服务端网页上显示的 4 位 PIN 交给处于 `state=pairing-required` 的会话（只接受一次，后端取走即清）。`true` = 已放入该会话的 PIN 槽；`false` = 会话不存在 / 不是 Sunshine / 当前不在 `pairing-required` / 不是 4 位数字。PIN 不进日志、不进状态 JSON。 |

`optionsJson` 字段：

```json
{ "host": "192.168.50.62", "port": 5901,
  "protocol": "rdp",          // 可选，"vnc"（缺省）| "rdp" | "sunshine"
  "username": "…",            // 可选，仅 RDP
  "domain": "…",              // 可选，仅 RDP，可空
  "password": "…",            // 可选，短生命周期，不落盘、不入日志
  "expectedFingerprint": "…", // 可选，来自信任库；VNC 与 RDP 都使用
  "security": "None",         // 可选，默认 "RA2_256,RA2"；仅 VNC
  "name": "测试机",
  // 以下仅在 protocol="sunshine" 时生效
  "sunshineStateDir": "/data/storage/el2/base/files/sunshine",
  "sunshineApp": "Desktop",   // 可选，默认 "Desktop"
  "sunshineWidth": 1280, "sunshineHeight": 720, "sunshineFps": 60,
  "sunshineBitrate": 20000,   // 可选，默认 1280/720/60/20000（bitrate 单位 kbps）
  "sunshineFormats": "h264,hevc", // 可选，缺省按设备硬解能力自动
  "sunshineEncryption": "none",  // 可选，默认 ENCFLG_ALL；"none" 关闭
  "sunshineHostCursorHidden": false }  // 可选，上次记录的主机光标隐藏状态
```

`protocol` 选择原生后端：缺省或 `"vnc"` 走 RFB，`"rdp"` 走 FreeRDP，`"sunshine"` 走 Moonlight（Sunshine 服务端）。`username`/`domain` 只在 RDP 下有意义（`domain` 可空），用于 RDP 登录；VNC 与 Sunshine 忽略这两个字段。RDP **必须**带 `password`（没有「无需密码」语义）。`security`（RFB 安全类型）只用于 VNC。`expectedFingerprint` 两种协议都用于服务端证书指纹固定：VNC 是 RA2 指纹，RDP 是服务端 X.509 证书的 SHA-256 指纹（小写、字节间用 `-` 连接，如 `84-64-…`）；RDP 首次连接（该字段为空）会以 `state=trust-required` 结束本次尝试并带出 `fingerprint`，不自动重试，界面确认后由 `reconnectSession` 重连，后续连接必须带该 pin。RDP 与 VNC 复用同一套会话状态（`connecting`/`connected`/`failed`/`reconnecting` 等）与 `errorCode`（`auth-failed`/`fingerprint-mismatch`/`protocol`/`network`；RDP 另有 `session-taken-over`），因此界面的失败提示与密码重输流程对两种协议一致。

### Sunshine/Moonlight

- `sunshineStateDir` 是**必填**的客户端状态目录（应用沙箱绝对路径）：客户端证书/私钥、uniqueid、以及配对后固定的服务端证书与指纹都落在这里。**缺失时 `createSession` 仍然成功**，会话以 `state=failed`、`errorCode="protocol"` 出现，`error` 说明缺少状态目录（界面需要一个会话才能展示这条错误；`createSession` 直接返回 `ERROR:` 的路径无法显示在会话列表里）。
- `port` 对 Sunshine 是**明文 HTTP（nvhttp）端口**（Sunshine 默认 47989）。HTTPS 端口默认取 `port - 5`（即 47984）；若按该端口探测 HTTPS 失败、且 `/serverinfo` 公布了不同的 `HttpsPort`，则改用服务端公布的值重建客户端再探；RTSP 端口由 `/launch` 的 `sessionUrl0` 决定。
- `sunshineApp` 与 `/applist` 的标题做**不区分大小写的包含匹配**，找不到就用第一个应用；实际启动的标题通过状态 JSON 的可选字段 `app` 回传。
- `sunshineWidth`/`sunshineHeight`/`sunshineFps`/`sunshineBitrate` 默认 1280/720/60/20000 kbps，即 `/launch` 的 `mode=WxHxFPS`。默认码率取 20000 是因为 Moonlight 只有在视频码率 ≥ 15000 kbps（common-c `Limelight-internal.h` 的 `HIGH_AUDIO_BITRATE_THRESHOLD`）时才请求高音质音频（每声道 256 kbps，否则 96 kbps）。
- `sunshineFormats` 缺省时自动选择：H.264 恒有；HEVC 需要设备有 `video/hevc` 硬解**且**服务端 `/serverinfo` 声明支持（否则协商会失败）。显式给出时按给定值（`h264` / `hevc`，`h265` 为 `hevc` 的别名）。
- `sunshineEncryption` 缺省为 `ENCFLG_ALL`（音视频都加密）；传 `"none"` 时关成 `ENCFLG_NONE`（远端输入加密始终开启，无法关闭）。
- **残留会话**：服务端上一条会话没被取消时，`/launch` 会返回 `status_code=400: An app is already running on this host`（旧版本直接把这条当成「连接已断开」）。现在按 Moonlight 客户端的通行做法处理：`/serverinfo` 的 `state=SUNSHINE_SERVER_BUSY` 且 `currentgame` 等于要启动的应用 ID → 用 `/resume` 接管续播（参数与 `/launch` 相同，服务端上的应用继续跑）；BUSY 但是别的应用 → 先 `/cancel`，每 500ms 轮询 `/serverinfo`、最多 10 秒等服务端回到空闲，再按服务端当时的状态用 `/resume`（仍指向同一应用）或 `/launch`。`/launch` 仍返回 400 且消息含 `already running` 时，再执行一次同样的「cancel → 等空闲 → launch/resume」，只重试一次；`/cancel` 自身失败只记日志，不让会话失败。
- 停止/关窗（含 `closeSession` 打断会话线程）、`reconnectSession` 换代、对端结束或网络断开，只要本次尝试走到过 `/launch`，都会尽力向服务端发 `/cancel`；进程被杀来不及发的情况由上面的连接时处理兜底。
- **主机结束串流时的重连规则**：`connectionTerminated(0)` 是 `ML_ERROR_GRACEFUL_TERMINATION`（`Limelight.h:421`，主机主动结束）；`connectionTerminated(-1)` 是控制流被对端断开且没有 termination 消息（`ControlStream.c:1600` 的 `ENET_EVENT_TYPE_DISCONNECT`，另有 `ControlStream.c:1369` 的断开握手超时），Sunshine 的 `/cancel` 与主机端应用退出都走这条，协议层无法与真正的网络中断区分。两者都按「终止前正常画面的持续时长」（首个非黑帧到最近非黑帧的跨度）判定：**< 3000 ms（含从未有画面）** → 视为主机显示会话状态变化（控制台会话被接回、被远程桌面占用等）造成的瞬时黑屏，会话进入 `state=reconnecting`，按 2s/4s/8s 退避自动重连，每次重新走 `/serverinfo` →（必要时 `/resume`，或 `/cancel`→等空闲）→`/launch`；**最多自动重连 3 次**，仍失败则 `state=failed`、`errorCode="network"`，`error` 固定为「主机多次结束串流且始终无画面：请确认主机控制台已登录（未被远程桌面占用）后重试」，由用户手动重连。**≥ 3000 ms** → 用户已正常观看，按主机主动结束处理：不自动重连，停在 `state=disconnected`（`error`「主机结束了串流」），保留会话供用户手动重连。其它错误码（网络中断、无视频流量等）的重连策略不变（`reconnecting` + 1s/2s/4s 退避）。代价：若纯网络中断先以 -1 到达、且此前已正常观看 ≥3 秒，会停在下线状态而不是自动重连，需用户手动重连。
- **重连快路径**（减少「终止到首帧」的等待）：主机结束类终止（0/-1）**立即**进入 `state=reconnecting`（在任何 HTTP 之前，UI 1 秒内就能显示「正在重新连接…」），并且不再向服务端发 `/cancel`（真机上这里白等过一次 10.5 s 超时）。重连期间复用首次连接学到的 HTTPS 端口与已固定的证书，**只走 HTTPS `/serverinfo` 快路径**：单次 5 s 超时，失败 1 s 退避重试，最多持续 60 s；只有拿到确定答案（成功，或 401/403 证书不再被接受而转去配对，或用户停止）才离开，超时类失败**不退回**明文+HTTPS 的长探测（首次连接路径不变，仍 10 s）。本会话首次 `/applist` 匹配到的应用 ID 会被缓存，重连不再拉 `/applist`（仅当 `/launch` 用缓存 ID 报 404 时重拉一次并重试）。RTSP 握手阶段失败（主机尚未就绪）保持 1 s 退避重试，但计入上面 60 s 的总预算，W 级日志打印「主机尚未就绪，第 n 次重试」；预算耗尽则 `failed/network` + 原有提示。每次 phase 变化都会打一条带墙钟时间的 W 级日志；`/launch` 后 5 s 还没有首帧也会打一行 W 级日志说明当前阶段（RTSP/控制流/视频）。
- Sunshine 的生命周期与重连日志（serverinfo、配对阶段、launch/resume/cancel、终止及错误码、自动重连 n/3、放弃原因）与每 5 秒一条的媒体统计都用 **W 级**输出（hilog 默认可见）；更细的诊断仍是 D 级。
- 音频能力位声明 `CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION`：音频实现按 `samplesPerFrame` 解码、支持任意 Opus 帧长；刻意不声明 `CAPABILITY_SLOW_OPUS_DECODER`（服务端会按「解码器很慢」降级帧长/音质，本实现是实时解码，不需要降级）。


`security` 是向服务端声明的 RFB 安全类型（逗号分隔）。默认只声明加密的 `RA2_256,RA2`；**服务器若只公布 `None`(1)**，必须显式传 `"None"`，否则握手会以 `No matching security types` 失败。界面在「该服务器无需密码」勾选时传 `None`。RDP 没有对应的「无需密码」选项，界面也不显示该勾选。

## 状态 JSON

```json
{ "id": "s1", "state": "connecting|connected|disconnected|failed|trust-required|reconnecting|pairing-required",
  "error": "", "errorCode": "none|auth-failed|fingerprint-mismatch|network|protocol|session-taken-over|pair-failed",
  "width": 1920, "height": 1200, "generation": 3,
  "version": 118, "cursor": {"w":16,"h":23,"hotX":2,"hotY":0,"blank":false,"version":4},
  "clipboard": { "version": 7 },
  "retryInMs": 4000, "fingerprint": "84-64-…", "pixelBytes": 9216000,
  "resizeSupport": "unknown",
  "pictureBlack": false,
  "pairing": { "host": "192.168.50.62", "appVersion": "7.1.431.-1" },
  "app": "Desktop",
  "resize": { "width": 1600, "height": 900 },
  "remoteCursorHidden": false }
```

- `state=trust-required` 时附带 `fingerprint`，界面据此展示确认；确认写入 ArkTS 侧信任库后由界面调用 `reconnectSession`。
- `state=pairing-required`（Sunshine 专用）时附带 `pairing: {host, appVersion}`：会话正在等界面把服务端网页上显示的 4 位 PIN 用 `submitPin()` 提交上来。它是「等用户动作」的状态（与 `trust-required` 同级），不是失败；关窗/停止/`closeSession` 都能打断它。
- `errorCode=pair-failed`（Sunshine 专用）表示上一次配对失败：正常重试路径（PIN 错误 / 网页端超时）下会话回到 `state=pairing-required`，`error` 带服务端消息，界面重新调用 `submitPin()` 即可再试；外层 `PairError` 兜底分支也可能以 `state=reconnecting` + `pair-failed` 出现，界面不应假定两者绑定。连续两次传输层失败会转为 `state=failed` + `errorCode="network"`。
- `app`（Sunshine 专用，可选）是 `/applist` 实际匹配并启动的应用标题；没有匹配时会回退到第一个应用，与请求的 `sunshineApp` 可能不同。
- `remoteCursorHidden`（Sunshine 专用，布尔）：本地记录的「主机光标是否已隐藏」（本地光标模式）。Sunshine 把光标画在视频里，延迟等于视频链路；隐藏主机光标后由客户端显示本地系统光标。该状态是主机进程级、跨会话保持、**客户端无法查询**，只能由客户端记录（ArkTS 持久化到连接条目，下次 `createSession` 通过 `sunshineHostCursorHidden` 传回）；Sunshine 进程重启后会回到「显示」，此时界面应把记录重置为 `false` 后再切换。
- `resize`（Sunshine 专用，可选）表示**正在**按 `requestDesktopSize` 的新尺寸重启串流：`{width, height}` 是目标分辨率。出现该字段时界面可显示「正在按窗口调整分辨率…」；新尺寸的串流 `connectionStarted` 后该字段消失（`resizeSupport` 变为 `"ok"`），重启失败/会话终态时也会消失。
- `state=failed` 且 `errorCode=fingerprint-mismatch` 时也附带当前观测到的 `fingerprint`（VNC 与 RDP 一致）：它是本次连接中服务端实际出示的指纹，供界面与已信任值对比展示。
- `errorCode` 是枚举；`error` 只用于辅助显示。指纹不一致时 `error` 统一为「服务端指纹与已信任值不一致，已拒绝连接」。
- `errorCode=session-taken-over` 只出现在 RDP：同一 Windows 账号已有另一个远程会话，服务端用 `ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION` 结束了本连接（连接阶段或已连接之后都可能发生）。会话为 `state=failed`，且原生后端**不会自动重连**——自动重连会反过来顶掉另一个窗口，形成两个会话互相接管、最终远端会话不可用的循环。`error` 固定为「该账号的远程会话已被其它连接接管：同一 Windows 账号只允许一个远程会话」，界面提示后由用户手动调用 `reconnectSession`。
- `resizeSupport` 按会话独立维护，重连后复位为 `"unknown"`：`"ok"` 表示服务端已按客户端请求确认/应用过至少一次（VNC：收到 `reasonClient` 的成功答复；RDP：收到与在途请求对应的 `DesktopResize`）；`"rejected"` 只可能来自 VNC（服务端明确拒绝）。RDP 协议没有拒绝应答，请求被忽略时保持 `"unknown"`，界面必须自行超时并停用该会话的自动跟随。
- `pictureBlack`（Sunshine 专用，仅 `state=connected` 时出现）：布尔。视频解码路径在解码回调线程里对每一帧做稀疏亮度采样（每 16 行取一行、每 16 像素取一个点，取 R/G/B 平均并忽略 alpha），平均亮度 < 4 视为黑帧；连续全黑或本次尝试迟迟没有帧达到 3000 ms 时为 `true`，出现任一非黑帧后立即变为 `false`。常见原因是主机控制台上没有可用的登录会话（例如正被另一个远程桌面会话占用，Windows 把控制台会话搬走），界面可据此提示用户去主机上断开远程桌面或重新登录控制台。VNC/RDP 不产生该字段；未连接时不出现（读取方应按 `false` 处理）。

### 分段耗时（`perf`）

`perf` 是所有协议都有的对象：`{windowMs, frames, delivered, decode, convert, pixWait, commit, takeFrame}`，每个 timing 子对象是 `{n, mean, p50, max}`（微秒），表示最近一个 1 秒窗口的摘要；`frames`/`delivered` 是该窗口的提交帧数与 `takeFrame` 交付数。

`protocol="sunshine"` 且 `state=connected` 时，同一个 `perf` 对象里**追加**（不新增重复的 `perf` 键）最近一个 5 秒窗口的视频链路分段耗时，单位毫秒：

| 字段 | 含义 |
| --- | --- |
| `inFps` / `decFps` / `outFps` | 输入（common-c 交付）帧率 / 解码输出帧率 / 实际上屏帧率（`outFps` 由渲染线程统计，窗口内 swap 成功数 / 窗口长度） |
| `netToSubmitMsAvg` | common-c 入队（`enqueueTimeUs`）到 `submitDecodeUnit` 被调用的平均耗时 |
| `submitToOutputMsAvg` | 送进 AVCodec 到对应帧输出回调的平均耗时（按 PTS 配对） |
| `convertMsAvg` | 输出回调里 NV12/NV21 → BGRA 的 libswscale 转换平均耗时 |
| `commitMsAvg` | `commitFrame`（含镜像拷贝）平均耗时 |
| `lagMsAvg` / `lagMsP95` | Buffer 模式：镜像提交时间戳到渲染线程取走该帧的滞后；Surface 模式：帧可用回调到完成 `UpdateSurfaceImage`（见「零拷贝」一节） |
| `perfWindowMs` | 上述 5 秒窗口的实际长度 |

- 这些都是**只读快照**，读取不等待解码或渲染；窗口没有样本时为 `0`。未连接、还没攒满一个 5 秒窗口、或没有挂载原生渲染器时，对应字段为 `0`（`perfWindowMs=0` 表示解码侧尚未发布过窗口）。
- 只有 Sunshine 的帧镜像会写提交时间戳（`commitTimeUs`），所以 `lagMsAvg`/`lagMsP95` 只对 Sunshine 有意义；VNC/RDP 不产生这两个字段。Surface 模式下 `convertMsAvg`/`commitMsAvg` 恒为 0（CPU 转换与镜像拷贝都不存在）。
- 后端同时每 5 秒打一条 W 级日志：`sunshine perf: in=…fps dec=…fps out=…fps netToSubmit=…/… submitToOutput=…/… convert=…/… commit=…/… lag=…/… swap=…/… surface=…/… (avg/p95 ms, window=…ms, dropped=…)`，其中 `swap` 是「取到帧（Surface 模式是完成 `UpdateSurfaceImage`）到 `eglSwapBuffers` 返回」，`surface` 只在 Surface 模式下非 0（帧可用到 swap 返回）；渲染线程自己的 `TianguanRender` 统计行也带 `lag=`/`pickupSwap=`（Surface 模式带 `src=surface`/`frameToSwap=`）。这些数字只用于定位瓶颈，不改变任何渲染或解码行为。

### 重新确认服务端指纹

```
forgetTrustedFingerprint(sessionId: string): string
```

- **同步**返回。会话存在时：清空该会话的期望指纹 `expectedPin` 与已观测指纹 `fingerprint`，然后触发一次与 `reconnectSession` 等价的重连（`generation` 递增，正在退避或等待确认的重连立即进行）。下一次连接因没有期望指纹而停在 `state=trust-required`，并带回当前观测到的服务端指纹，由用户再次确认。返回 `OK`；会话不存在返回 `INVALID_SESSION`。
- 对处于 `failed` / `trust-required` / `connected` / `connecting` 等任何状态的会话都允许调用；清 pin 后一律按重连处理。
- **安全含义**：它只让会话重新展示服务端当前出示的指纹，不自动信任任何指纹，也不读写 ArkTS 侧信任库文件。真正建立信任仍必须由用户在界面上确认、界面写入信任库，再由界面调用 `reconnectSession`（该调用在 `trust-required` 状态下才把当前 `fingerprint` 记为信任锚）。因此它不能把「指纹不一致」直接变成「已连接」。

### Sunshine 配对（PIN）

```
submitPin(sessionId: string, pin: string): boolean
```

- **同步**返回。会话必须是 `protocol="sunshine"` 且当前 `state=pairing-required`；PIN 必须是 4 位 ASCII 数字。满足条件时把 PIN 放进该会话的 PIN 槽、唤醒等待中的会话线程并返回 `true`；否则静默返回 `false`（不抛错，不改变状态）。
- PIN 只被后端取走**一次**，取走后立即清空；它不进日志、不进状态 JSON、不落盘。离开 `pairing-required`（配对成功、失败重试、停止、重连）都会丢弃槽里的 PIN。
- 典型流程：`sessionStatus` 看到 `state=pairing-required` → 界面提示用户打开服务端网页（Sunshine 首页）读取 4 位 PIN → 用户输入 → `submitPin(sessionId, pin)`。PIN 正确后会话自动进入 `/applist` → `/launch` → `connected`，不需要 `reconnectSession`。
- PIN 错误或网页端在 5 分钟内没提交：会话回到 `state=pairing-required`，带 `errorCode="pair-failed"` 与服务端消息（如「PIN 错误」/「网页端未在 300 秒内提交 PIN」），界面可以让用户重新输入，**不需要**重连。
- 服务端证书与配对时固定的指纹不一致（`PinMismatchError`）：`state=failed` + `errorCode="fingerprint-mismatch"`，`error` 说明需要重新配对；会话不会自动重试，由用户处理（当前版本没有通过 NAPI 清除 Sunshine pin 文件的入口）。
- 配对成功后服务端证书与指纹由 native 侧落盘在 `sunshineStateDir`，不经过 ArkTS 信任库；`expectedFingerprint`/`forgetTrustedFingerprint` 只作用于 VNC/RDP 的信任锚，对 Sunshine 无效。
- 连接建立后断线：会话进入 `state=reconnecting` + `errorCode="network"` 并按 1s/2s/4s 退避自动重连（每次重连重新走 `/serverinfo` → `/launch`）；`connected` 期间收到 `generation` 变化（`reconnectSession`）会立即拆流重连。
- 视频用 OHOS AVCodec 硬解：原生渲染器 attach 时走零拷贝 Surface 模式（见「画面」里的说明），否则回退 NV12→BGRA 的 Buffer 模式；音频用 libopus 多流解码 + `OH_AudioRenderer` 播放；`width`/`height` 按协商到的流分辨率出现在状态里。

## 画面

```json
takeFrame(sessionId: string, buffer: ArrayBuffer, afterVersion: number, forceFull: boolean): string
```

返回 JSON 元数据，或字符串 `NONE`（无新内容）：

```json
{ "x": 0, "y": 0, "w": 1920, "h": 1200, "version": 119, "generation": 3, "full": true }
```

### 零拷贝（Surface 模式）视频路径

Sunshine 会话在**原生渲染器已 attach** 时走零拷贝路径：AVCodec 用 Surface 模式（`OH_VideoDecoder_SetSurface`）把帧直接渲染进 `OH_NativeImage` 的 `OHNativeWindow`，渲染线程把同一块画面当 OES 外部纹理（`GL_TEXTURE_EXTERNAL_OES` + `samplerExternalOES`，采样坐标乘 `OH_NativeImage_GetTransformMatrixV2`）直接采样上屏。这条路上没有 NV12→BGRA 的 CPU 转换、没有整帧镜像拷贝、也没有每帧 `glTexSubImage2D` 上传。

- **回退**：渲染表面还没 attach（最多等 10 秒）、窗口被隐藏、或解码器 `SetSurface`/`Configure`/`Prepare` 失败时，视频路径回退到原来的 Buffer 模式，行为与接入零拷贝前完全一致；日志里写的是 `surface mode (zero-copy)` 还是 `buffer mode NV12`。
- **已知限制**：Surface 模式下没有像素写进 BGRA 帧镜像，所以 `takeFrame`/`copyRegion`/`takeFrameStruct` 在 Sunshine 会话下没有像素可交付（返回 `NONE`；镜像保持全黑，尺寸与状态里的 `width`/`height` 一致，由 `setSourceSize` 更新）；只有原生渲染器（XComponent 呈现）能看到画面。渲染表面在串流中途重建（`detachRenderSurface` 后重新 attach）时，旧的外部纹理源随旧渲染器销毁，解码器继续向失效窗口提交的帧会被丢弃，画面要等重新建立串流（重连）才恢复。
- **黑屏检测**：Surface 模式下由渲染线程每 500 ms 把外部纹理画进 32×32 的 FBO 再 `glReadPixels`，按平均亮度判定（< 4 为黑，阈值与 Buffer 模式一致），结果照旧经 `pictureBlack` 字段暴露。
- **统计**：`perf` 的 `convertMsAvg`/`commitMsAvg` 在 Surface 模式下恒为 0（这两段不存在）；`lagMsAvg` 是「帧可用回调 → 完成 `UpdateSurfaceImage`」，`swap` 是「完成 `UpdateSurfaceImage` → `eglSwapBuffers` 返回」，后端 5 秒 W 日志另外多一个 `surface=平均/P95`（帧可用 → swap 返回，含 `eglSwapInterval(1)` 的 vsync 等待）。渲染线程的 `TianguanRender` 统计行在该模式下带 `src=surface` 与 `frameToSwap=`。

- 像素格式固定 **BGRA_8888**，`stride = w * 4`。
- `buffer` 必须至少 `w*h*4`；不足时返回 `ERROR:BUFFER_TOO_SMALL`，不写入。
- `forceFull` 为 true 时返回完整画面。
- `afterVersion` 必须等于上一次成功交付的版本（`takenVersion`，且 `pixEpoch` 一致），否则返回完整画面，而不是局部块。
- 交付基线在 `memcpy` 成功后才推进；`NONE` / `BUFFER_TOO_SMALL` 不推进。

```
requestFullFrame(sessionId: string): string
```

请求一次非增量整屏更新（空闲桌面不会主动推全帧，界面就绪后必须调用一次）。

### 远端分辨率（VNC ExtendedDesktopSize / RDP DISPLAY_CONTROL）

```
requestDesktopSize(sessionId: string, width: number, height: number): boolean
```

- **同步**返回。`true`：已受理并交给会话线程发送；`false`：会话不存在、未连接、不是 VNC/RDP，或尺寸非法。尺寸范围：VNC 为 `[1, 4096]`；RDP 为 `[200, 4096]`（MS-RDPEDISP 的 `Width`/`Height` 允许 `200..8192`，会话镜像沿用 4096 上限，低于 200 的请求直接拒绝，不产生非法 PDU）。
- 原生不在 NAPI 线程写 socket：只记录待发尺寸，由各自会话线程消费。
- **同一时刻只挂起一个未决请求**（两种协议一致）：上一个请求还没得到结果时，新的调用只覆盖「尚未发出」的尺寸，不会并发写出两个请求；重连后待发/在途状态与 `resizeSupport` 一起复位。
- **VNC**：会话线程在协议状态 `NORMAL` 时调用 `CMsgWriter::writeSetDesktopSize()`。ScreenSet 固定为一个屏幕：`id=0`、矩形 `(0,0,width,height)`、`flags=0`。服务端是否支持由 `resizeSupport` 异步报告；若服务端从未宣告 ExtendedDesktopSize，请求会留在待发状态并保持 `"unknown"`。服务端也可能给出近似模式（例如偶数化/最接近的可用模式），`width`/`height` 与交付帧以服务端实际采用的几何为准。
- **RDP**：`PreConnect` 打开 `FreeRDP_SupportDisplayControl` 与 `FreeRDP_DynamicResolutionUpdate`（等同参照客户端的 `/dynamic-resolution`），client-common 加载 `disp` 动态通道；服务端打开 `Microsoft::Windows::RDS::DisplayControl` 并下发 caps 后，会话线程构造单个显示器的 `DISPLAY_CONTROL_MONITOR_LAYOUT`（`Flags=PRIMARY`、`Left=Top=0`、`Width/Height=请求值`、`PhysicalWidth/Height=请求值`、orientation/scale 取协商设置）并调用 `DispClientContext::SendMonitorLayout`。FreeRDP 的 disp 插件会把宽度规整为偶数并夹到 `200..8192`，所以奇数宽度可能以相邻偶数值生效。
- **RDP 的判定**：没有接受/拒绝应答。只有收到 `DesktopResize`（服务端应用了新的桌面尺寸）才把 `resizeSupport` 置 `"ok"`；请求被忽略则一直保持 `"unknown"`，由界面超时判定。`"rejected"` 不会由 RDP 产生。
- **Sunshine（Moonlight）**：分辨率只能在 `/launch` 的 `mode` 里指定，所以调整 = **停流 → `/cancel` → 用新尺寸重新 `/launch` → 重启串流**。参数校验：宽高必须为**偶数**、范围 **16–7680**、宽高比不超过 **4:1**（任一方向），否则返回 `false`。会话线程每 200 ms 取一次请求槽：与当前流尺寸相同则直接 ack；不同则立即 `state=reconnecting` 并出现上面的 `resize` 字段，停止当前媒体（**不发 `/cancel`**）后按新尺寸重启。**续播优先**：直接 `GET /resume`（appid 不变、`mode=新WxHxFPS`、码率按下面的公式），用返回的 `sessionUrl0/rikey/rikeyid` 起新流——实测停流→首帧约 11.4 s 且没有 RTSP 失败；`/resume` 失败（HTTP 非 200、`nv_status_code` 4xx、超时等）才退回原路径「`/cancel` → 等空闲 → `/launch`」，并保留陈旧会话清理逻辑。走哪条路径与耗时都打 W 级日志。处理期间到达的新请求只覆盖待发槽（最后一个生效），下一次轮询会再处理。码率按像素同比、带 0.8 次幂与编码系数缩放：
`new = baseBitrate × ((w×h) / (baseW×baseH))^0.8 × (HEVC ? 0.7 : 1.0)`
（base 取 `sunshineWidth/Height/Bitrate`），四舍五入到 1000 kbps 后钳在 **15000–80000 kbps**；帧率与编码格式不变。HEVC 判定取**当前流实际协商到的视频格式**（视频 setup 的 `videoFormat` 落在 `VIDEO_FORMAT_MASK_H265` 内即算，含 Main10），首次协商前按「自动且设备有 HEVC 硬解」处理。以 1080p / 20 Mbps 为基准的示例：1440p HEVC ≈ 22 Mbps、4K HEVC ≈ 42 Mbps、1440p H.264 ≈ 32 Mbps、720p（无论编码）回落到下限 **15 Mbps**。整个调整计入本次会话，不计入「主机结束且无画面」的自动重连次数。
- **Sunshine 的 ack 语义与 RDP 一致**：会话线程「取到」请求时把它标记为 in-flight（`desktopAwaiting`），此时可以再接收新请求但只保留最后一个；正 ack 是新尺寸的串流重新 `connectionStarted`（没有服务端应答），`resizeSupport` 由 `"unknown"` 变 `"ok"`；重启失败/会话终态只清 in-flight 与 `resize` 字段，`resizeSupport` 保持 `"unknown"`，由界面超时判定。
- VNC、RDP 与 Sunshine 都用同一个 `resizeSupport` 字段，因此页面无需按协议分支读取。

### 视图变换（缩放/平移）

```
setViewTransform(sessionId: string, zoom: number, panX: number, panY: number): string
```

- `zoom` 是相对 contain 适配的倍数，默认 `1.0`，原生夹到 `[1, maxZoom]`（当前 `maxZoom=4`），不能缩到比窗口小。
- `panX`/`panY` 单位是 surface 像素，相对居中位置的偏移。内容在某个轴上仍不大于 surface 时该轴忽略 pan（保持居中）；内容大于 surface 时夹在 `[surface-content, 0]`，不会拖出空白。夹取使用当前 surface 的实际尺寸（每帧 `eglQuerySurface`），旋转/改窗口后自动重新夹。
- 返回 `OK`，或 `ERROR:NO_RENDERER` / `ERROR:INVALID_ARGUMENTS`。
- 改视图后会重绘已上传的纹理一次，即使远端没有新帧。

几何的唯一真源是原生：`renderStats(sessionId)` 返回实际绘制矩形，界面必须用它做输入映射，不得自行按 contain 复算：

```json
{ "contentLeft": 40, "contentTop": 0, "contentScale": 1.125, "contentScaleY": 1.125,
  "contentW": 2160, "contentH": 1350, "zoom": 2.0, "panX": -120.0, "panY": 0.0,
  "maxZoom": 4.0 }
```

- `contentLeft`/`contentTop`：绘制矩形左上角（surface 像素，左上原点）。
- `contentScale = contentW / texWidth`，`contentScaleY = contentH / texHeight`；远端坐标 `remoteX = (x - contentLeft) / contentScale`，`remoteY = (y - contentTop) / contentScaleY`。
- `zoom`/`panX`/`panY` 是实际生效值（zoom 已夹、pan 已按当前 surface 夹）。
- `renderStats` 原有字段（state/fps/width/height/texWidth/texHeight/…）含义不变。Canvas 回退路径（`USE_GL_RENDER=false`）没有原生渲染器，不提供这些字段，也不支持缩放。

### 光标

```
copyCursor(sessionId: string, buffer: ArrayBuffer): string
```

- 把该会话当前光标图像（**RGBA_8888**，`stride = w * 4`；尺寸与热点见状态 JSON 的 `cursor`）拷进 `buffer`，返回 `OK <字节数>`。
- 无光标（`cursor.blank=true` 或尚未收到形状）时返回 `OK 0`，按空串 / `NONE` 处理。
- `buffer` 至少 `w*h*4`；原生只写入实际光标字节数，不会超出容量。
- 与状态 JSON 的 `cursor.version` 配合使用：版本不变说明形状未变，不必重复取；版本递增时再取一次。
- 单参数旧形式 `copyCursor(buffer)` 仍等价于「默认会话」。

## 输入

```
sendPointer(sessionId: string, x: number, y: number, mask: number): string
sendKey(sessionId: string, keysym: number, pressed: number): string
releaseSessionInput(sessionId: string): string   // 释放按键/按钮并清指针与按键队列；剪贴板待发项保留
```

- 返回 `QUEUED`、`COALESCED`、`NOT_CONNECTED`、`NOT_READY`、`INVALID_SESSION`。
- 纯移动（`mask == 0`）可合并为最新位置；按下/抬起/滚轮不得静默丢弃。
### 主机光标（本地光标模式）

```
setRemoteCursorHidden(sessionId: string, hidden: boolean): boolean
```

- **Sunshine/Moonlight 专用**。Sunshine 的光标编在视频里，跟随延迟等于视频链路；隐藏主机光标后由客户端显示本地系统光标。切换方式是发送 Sunshine 快捷键 **Ctrl+Alt+Shift+N**（`src/input.cpp` 的 `apply_shortcut` 翻转主机全局 `display_cursor`，且该组合键不传给桌面）。
- **同步**返回。会话已是目标状态 → `true`（不发送；同时取消尚未发出的反向请求）；请求被受理 → `true`（组合键由后端输入线程在 ~2 ms 内发出，发送成功后 `remoteCursorHidden` 才更新）；会话不存在 / 非 Sunshine / 未连接 → `false`（未连接时仍记录「期望状态」，连接建立后补发一次）。
- 状态是**主机进程级、跨会话保持**的，客户端无法查询：原生只记录，ArkTS 负责持久化到连接条目并通过 `sunshineHostCursorHidden` 传回。**Sunshine 重启后会回到「显示」**，界面若怀疑主机重启过，应把记录重置为 `false` 再做切换。
- 发送与前后状态均打 W 级日志；不涉及 PIN/密钥。

- **Sunshine 输入低延迟**：输入线程每 **2 ms** 轮询一次共享队列（条件唤醒尚未接入，2 ms 即兜底上限），`drainInput` 内逐条同步调用 `LiSend*`，返回后立刻处理下一条，不做攒批或额外 sleep。绝对坐标用 `LiSendMousePositionEvent(x, y, 流宽, 流高)`，参考尺寸取**当前流**的最新值（分辨率调整后随视频 setup 更新）。同 mask 且非 boundary 的尾部移动才会合并（保留最新位置与时间戳），按下/抬起/滚轮等 boundary 事件永不合并或丢弃。每 5 秒一条 W 级日志 `sunshine input: queueLag=avg/p95 sendCalls=N sendFail=M (avg/p95 ms, samples=S)`：queueLag 是「入队 → `LiSend*` 返回」的间隔（`QueuedInput` 带 `steady_clock` 入队时刻），`sendFail` 单独统计 `LiSend*` 返回非 0（如 -2 输入流未就绪）的次数。

### RDP 锁定状态同步

```
syncRemoteLockState(sessionId: string, capsLock: boolean, numLock: boolean): string
```

- 同步返回 `OK` / `INVALID_SESSION`（会话不存在）。
- RDP：把 `capsLock` / `numLock` 作为**绝对状态**，由会话线程通过 FreeRDP 的 Client Synchronize Event（`freerdp_input_send_synchronize_event`，toggleFlags 含 `KBD_SYNC_CAPS_LOCK` / `KBD_SYNC_NUM_LOCK`）发给远端。调用方在 NAPI 线程只登记新值，不写 socket；新值覆盖尚未发送的旧值；若当前未连接，最新值保留到下一次连接进入事件循环后发送。本调用不改变本机状态，也不等待远端确认。
- RDP 上 CapsLock（`0xFFE5`）与 NumLock（`0xFF7F`）的键事件不再转发（静默忽略：不产生扫描码、不计入未映射丢键数、不打丢弃日志）；远端锁定状态只由本接口设置。ScrollLock 仍按原样转发。
- VNC：不做任何事，返回 `OK`（RFB 直接发送 keysym，没有锁定状态 PDU）。

## 剪贴板

| 接口 | 说明 |
|---|---|
| `setClipboard(sessionId: string, text: string): string` | 把本地文本作为 ClientCutText 发给服务端。 |
| `takeClipboard(sessionId: string): string` | 取走服务端最近一次 ServerCutText 的文本并清空；无内容返回空串。 |
| `takeClipboardOrNull(sessionId: string): string \| null` | **ArkTS 包装层提供**（`diag/SessionApi.ets`）：原生只导出 `takeClipboard`，包装层在它抛错或返回非字符串时给 `null`，并保持原有「空串 = 确实没内容」的语义。调用方必须区分两者：`null` 时不得推进已消费版本，应下一轮重试。 |

- 仅支持纯文本；`\n` 与 `\r\n` 归一化为 `\n`。
- 服务端送来的文本长度上限 1 MiB，超出截断并在状态 JSON 记 `clipboardTruncated: true`。

## 兼容性

旧的单会话接口（无 `sessionId`）在过渡期保留，等价于「默认会话」，但新界面不得依赖。
