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

`optionsJson` 字段：

```json
{ "host": "192.168.50.62", "port": 5901,
  "protocol": "rdp",          // 可选，"vnc"（缺省）| "rdp"
  "username": "…",            // 可选，仅 RDP
  "domain": "…",              // 可选，仅 RDP，可空
  "password": "…",            // 可选，短生命周期，不落盘、不入日志
  "expectedFingerprint": "…", // 可选，来自信任库；VNC 与 RDP 都使用
  "security": "None",         // 可选，默认 "RA2_256,RA2"；仅 VNC
  "name": "测试机" }
```

`protocol` 选择原生后端：缺省或 `"vnc"` 走 RFB，`"rdp"` 走 FreeRDP。`username`/`domain` 只在 RDP 下有意义（`domain` 可空），用于 RDP 登录；VNC 忽略这两个字段。RDP **必须**带 `password`（没有「无需密码」语义）。`security`（RFB 安全类型）只用于 VNC。`expectedFingerprint` 两种协议都用于服务端证书指纹固定：VNC 是 RA2 指纹，RDP 是服务端 X.509 证书的 SHA-256 指纹（小写、字节间用 `-` 连接，如 `84-64-…`）；RDP 首次连接（该字段为空）会以 `state=trust-required` 结束本次尝试并带出 `fingerprint`，不自动重试，界面确认后由 `reconnectSession` 重连，后续连接必须带该 pin。RDP 与 VNC 复用同一套会话状态（`connecting`/`connected`/`failed`/`reconnecting` 等）与 `errorCode`（`auth-failed`/`fingerprint-mismatch`/`protocol`/`network`），因此界面的失败提示与密码重输流程对两种协议一致。

`security` 是向服务端声明的 RFB 安全类型（逗号分隔）。默认只声明加密的 `RA2_256,RA2`；**服务器若只公布 `None`(1)**，必须显式传 `"None"`，否则握手会以 `No matching security types` 失败。界面在「该服务器无需密码」勾选时传 `None`。RDP 没有对应的「无需密码」选项，界面也不显示该勾选。

## 状态 JSON

```json
{ "id": "s1", "state": "connecting|connected|disconnected|failed|trust-required|reconnecting",
  "error": "", "errorCode": "none|auth-failed|fingerprint-mismatch|network|protocol",
  "width": 1920, "height": 1200, "generation": 3,
  "version": 118, "cursor": {"w":16,"h":23,"hotX":2,"hotY":0,"blank":false,"version":4},
  "clipboard": { "version": 7 },
  "retryInMs": 4000, "fingerprint": "84-64-…", "pixelBytes": 9216000,
  "resizeSupport": "unknown" }
```

- `state=trust-required` 时附带 `fingerprint`，界面据此展示确认；确认写入 ArkTS 侧信任库后由界面调用 `reconnectSession`。
- `state=failed` 且 `errorCode=fingerprint-mismatch` 时也附带当前观测到的 `fingerprint`（VNC 与 RDP 一致）：它是本次连接中服务端实际出示的指纹，供界面与已信任值对比展示。
- `errorCode` 是枚举；`error` 只用于辅助显示。指纹不一致时 `error` 统一为「服务端指纹与已信任值不一致，已拒绝连接」。
- `resizeSupport` 按会话独立维护，重连后复位为 `"unknown"`：`"ok"` 表示服务端已按客户端请求确认/应用过至少一次（VNC：收到 `reasonClient` 的成功答复；RDP：收到与在途请求对应的 `DesktopResize`）；`"rejected"` 只可能来自 VNC（服务端明确拒绝）。RDP 协议没有拒绝应答，请求被忽略时保持 `"unknown"`，界面必须自行超时并停用该会话的自动跟随。

### 重新确认服务端指纹

```
forgetTrustedFingerprint(sessionId: string): string
```

- **同步**返回。会话存在时：清空该会话的期望指纹 `expectedPin` 与已观测指纹 `fingerprint`，然后触发一次与 `reconnectSession` 等价的重连（`generation` 递增，正在退避或等待确认的重连立即进行）。下一次连接因没有期望指纹而停在 `state=trust-required`，并带回当前观测到的服务端指纹，由用户再次确认。返回 `OK`；会话不存在返回 `INVALID_SESSION`。
- 对处于 `failed` / `trust-required` / `connected` / `connecting` 等任何状态的会话都允许调用；清 pin 后一律按重连处理。
- **安全含义**：它只让会话重新展示服务端当前出示的指纹，不自动信任任何指纹，也不读写 ArkTS 侧信任库文件。真正建立信任仍必须由用户在界面上确认、界面写入信任库，再由界面调用 `reconnectSession`（该调用在 `trust-required` 状态下才把当前 `fingerprint` 记为信任锚）。因此它不能把「指纹不一致」直接变成「已连接」。

## 画面

```json
takeFrame(sessionId: string, buffer: ArrayBuffer, afterVersion: number, forceFull: boolean): string
```

返回 JSON 元数据，或字符串 `NONE`（无新内容）：

```json
{ "x": 0, "y": 0, "w": 1920, "h": 1200, "version": 119, "generation": 3, "full": true }
```

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
- VNC 与 RDP 都用同一个 `resizeSupport` 字段，因此页面无需按协议分支读取。

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
