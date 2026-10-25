# 天关：原生内核诊断 HAP

这是独立 Stage ArkTS 应用，bundle `cn.alfadb.tianguan`，版本 `0.0.1` / code `1`，API 26，`2in1` 与 `phone` 双设备。它通过正式 HAP 加载原生内核诊断库；当前界面提供连接管理与多会话远程桌面（VNC / RDP）。`module.json5` 当前声明普通权限 `ohos.permission.INTERNET`（网络）与 `ohos.permission.PREPARE_APP_TERMINATE`（托盘退出清理，system_grant），二者都不需要 ACL；受限权限 `ohos.permission.READ_PASTEBOARD` 因签名 Profile 的 ACL 为空而暂时保持未声明（见「自动剪贴板」）。不申请后台或 VPN 权限。

本轮已把测试手机 PLA-AL10（HarmonyOS 7.0.0.107 / API 26 / arm64）纳入调试范围：`entry/src/main/module.json5` 的 `deviceTypes` 与 `entry/build-profile.json5` 的 `config.deviceType` 均为 `['2in1', 'phone']`。本轮加入网络测试后重建的 unsigned HAP 内 `module.json` 与 `pack.info` 均同时含 `2in1` 与 `phone`，`requestPermissions` 仅含 `ohos.permission.INTERNET`；此前的纯诊断包没有权限声明。

## 真机验证结果（2026-10-20）

签名包 SHA-256 `02215cfacfcc703685afd795352e58bd0de0014029a707a26ec13fadcf1611f4`，由企业账号调试证书与含两台设备的调试 Profile 签名；`verify-app` 成功，包内 Profile 与外部 `.p7b` 逐字节一致。

- **MateBook Fold（SLM-W32，API 26）**：安装成功（`install bundle successfully`），`aa start` 成功，进程存在；从应用私有目录取回 `diag-result.txt`，三项全部 PASS（RA2_256 工厂、内存 RFB 3.8 Raw 2×1 像素、Thread 协作退出）。证据：`.work/device-runs/pc-diag/diag-result.txt`。
- **手机 PLA-AL10（API 26）**：安装成功；解锁后 `aa start` 成功，进程存在；取回的 `diag-result.txt` 三项同样全部 PASS。证据：`.work/device-runs/phone-diag/diag-result.txt`。

启动页面自动运行一次 `import diag from 'libtianguan_diag.so'; diag.runDiagnostics(): Promise<string>`，按钮可以复跑且执行期间禁用。逐项 PASS/FAIL 文本完全来自真实 native 实现，壳不补造成功结果。native 应覆盖 RA2 对象工厂、内存 Raw 像素和 Thread 协作退出。异常显示为错误。版本、时间和结果/错误写入本应用 `filesDir/diag-result.txt`，同时使用 `TianguanDiag` 标签输出日志；保存失败也会在界面与日志显示。没有用户 Documents 或全局文件写入。

## 受控网络测试模式（显式测试用）

连接界面已有实时会话，画面渲染与键鼠转发见下文「输入与显示（2026-10-21）」；下面这一段描述的是显式测试用的一次性网络测试模式，它只验证 TCP 连接、认证、首帧解码与键鼠事件已发送，不渲染画面、不验证服务端收到输入。服务端身份采用显式信任：首次连接只展示指纹、不认证；用户确认后写入本机信任库，之后仅当指纹完全一致才继续，不一致直接拒绝。

- 启动时只自动执行内存诊断并写 `diag-result.txt`，不再自动发起网络连接；「连接」按钮由用户在界面手动触发，连接期间禁用。
- 网络测试读取 `filesDir/vnc-test.json`，格式 `{"host":"<server-ip>","port":5901}`，可选 `"security":"RA2_256,RA2"` 指定协商列表（缺省即 `RA2_256,RA2`；显式支持 `RA2ne_256`、`RA2ne`、`VncAuth` 仅用于互通测试，明文类型会被结果如实标注），连接界面还会在已信任该 `host:port` 时自动加入 `"expectedFingerprint"`。文件不存在时返回一行 `SKIP network test: no vnc-test.json`，不算失败；内容无效则报告 FAIL。
- 读取 `filesDir/vnc-test.pass`：单行密码，允许末尾 LF 或 CRLF，内容 1..512 字节；缺失返回 `SKIP network test: no vnc-test.pass`。空文件、内部换行或 NUL 字节报告 FAIL。连接界面会把输入的密码写入这个应用沙箱内的临时文件供 native 读取，连接结束（成功或失败）后由 ArkTS 删除；密码不打印、不写日志、不写结果文件，也不进入信任库。native 只在内存中使用密码，认证成功或对象析构时擦除。
- native 使用真实 TigerVNC `TcpSocket` 与 `rfb::CConnection` 子类，安全类型由配置决定（缺省 `RA2_256,RA2`），`setShared(true)`，`setPreferredEncoding(0)`。`getUserPasswd` 在测试模式下接受服务端给出的回调形态（`user` 槽位可空、`secure` 可假，均如实记入结果），密码缺失或为空时抛异常；正式版本必须改为按策略拒绝明文与自动接受。
- `showMsgBox` 不再自动接受：请求未带 `expectedFingerprint` 时中止握手并返回 `NEEDS_CONFIRMATION: <指纹>`；指纹与期望值不一致时返回 `FAIL fingerprint mismatch: got <实际>, expected <期望>`；一致时才继续认证，并在结果中记 `fingerprint: <值> (trusted)`。
- `initDone` 按服务端 `pf()` 与宽高分配 `ManagedPixelBuffer`；宽高超过 4096×4096 或非正数则失败。`framebufferUpdateEnd` 先调用基类 flush 解码线程，仅当本帧确有非零解码区域且中心像素已被覆盖时，才用 `rgbFromBuffer` 读取中心像素并标记首帧。
- 首帧后发送 key `a` 按下/释放与 pointer `(1,1)` button1 按下/释放并 flush；结果只写 `input sent`，不声称服务端已收到。
- 结果每行带 PASS/FAIL：`PASS TCP connected host:port`、`PASS RA2_256 authentication complete`（仅在真实 `authSuccess` 回调后输出，不以 `isSecure()` 或安全类型代替认证证据）、`PASS first frame WxH`、`PASS center pixel RRGGBB`、`PASS input sent (...)`。失败时返回 `FAIL network test: <reason>` 而不是 reject；总预算 20 秒，覆盖连接、认证与首帧等待。
- native 返回文本由 ArkTS 写入 `filesDir/vnc-result.txt`，同时显示在界面；密码从不出现在界面或该文件。

### 实测结果（2026-10-20，指纹确认实现前的旧测试模式）

以下为指纹确认实现前的旧测试模式结果，仅作历史记录；该模式（`identity accepted in test mode`、自动接受任意服务端指纹）已不再存在于当前代码。

最终签名包 SHA-256 `3edd352c4aab732aa125ae525d4928613e748cfa642245fa230644f9f0ca6ce5`（含 `ohos.permission.INTERNET`，由当时代码构建），分别部署到 MateBook Fold 与手机 PLA-AL10，两台设备连接测试服务 `192.168.50.62:5901` 均返回全部 PASS：

```text
PASS TCP connected 192.168.50.62:5901
identity accepted in test mode: Fingerprint: 84-64-0d-21-6a-ab-fa-a6
PASS RA2_256 authentication complete
PASS first frame 1920x1200
PASS center pixel <PC 404040 / 手机 000000>
PASS input sent (key a press/release, pointer 1,1 button1 press/release)
```

证据：`.work/device-runs/pc-net3/vnc-result.txt`（PC）与 `.work/device-runs/phone-net2/vnc-result.txt`（手机）。两台设备均完成真实 TCP 连接、RA2_256 加密认证与首帧解码；尚未做服务端输入回执验证，测试模式接受任意服务端指纹，不代表生产安全策略。

### 安全类型协商与错误密码（2026-10-20）

签名包 SHA-256 `80cf2f1abdb6c639b372e8bad3eda97b73ba344a000d108b3fdd3f83ce15fd1c`。`vnc-test.json` 新增可选 `security` 字段：缺省为 `RA2_256,RA2`（仅加密类型），显式给出时按该列表协商，用于覆盖降级分支。

在 62 测试服务上实测（PC 四种，手机两种）：

| 请求类型 | 实际协商 | 保护 | 结果 |
|---|---|---|---|
| `RA2_256,RA2`（默认） | RA2_256 | AES-256 加密 | PASS（PC、手机） |
| `RA2` | RA2 | AES-128 加密 | PASS（PC） |
| `VncAuth` | VncAuth | 明文 | PASS（PC） |
| 默认 + 错误密码 | — | — | `FAIL network test: Authentication failed`（PC、手机，可读且快速返回，未卡死） |

结果行会打印 `requested security types` 与实际协商的 `authentication complete with <类型> (<保护>)`。注意：上游 `CSecurityRSAAES::isSecure()` 只对 RA2_256 返回真，因此本实现按协商到的类型显式标注加密与否，不采用该 helper。证据见 `.work/device-runs/pc-security/` 与 `.work/device-runs/phone-security/`。

### 画面快照显示（2026-10-20）

连接时 `vnc-test.json` 带 `"snapshot":"true"`：native 在收到首帧后把解码出的 framebuffer 编码为 JPEG（质量 85），写到 `filesDir/snapshot.jpg`，结果中增加 `PASS snapshot snapshot.jpg (<字节数>)`。界面随后读取该文件、用 `image.createImageSource` 创建 `PixelMap` 并在页面中显示。

这是**静态首帧**，不是实时桌面：连接在首帧后结束，鼠标和键盘还不能操作远端。实测（开发机对 62）：`1920x1200`、约 33 万字节的合法 JPEG，内容为真实 XFCE 桌面（顶栏、桌面图标、dock 均可见）。

实现注意：`PixelBuffer::getBuffer()` 返回的 stride 单位是**像素**而非字节，行偏移必须乘 `bpp/8`；用错会得到错位、条纹状的图像。

已知边界：只取首帧且只显示一次；不支持增量更新、不处理窗口缩放、不做输入回传。

### 输入是否真的到达远端（2026-10-20）

只打印“input sent”不能证明服务端收到。快照模式加入闭环验证：首帧后记录 framebuffer，双击桌面 Home 图标，等待 900 ms 并继续处理服务端更新，再取一帧逐像素比较，输出变化像素数与包围盒：

```text
PASS input observed: 261108 pixels changed, bbox (107,0)-(1847,864)
```

对应的 `snapshot-after.jpg` 里可以看到被真实打开的应用菜单、Thunar 文件管理器（`/home/worker`）和 Application Finder 窗口，说明点击确实由服务端执行，而不是客户端自报。证据：开发机 `/tmp/tg-inp/snapshot.jpg`、`snapshot-after.jpg`。

注意：变化像素数会因桌面状态而不同（若目标图标已被遮挡或菜单已展开，可能为 0）；判定“输入到达”应以服务端画面出现预期窗口为准，而不是只看计数。

### 实时会话与鼠标转发（2026-10-20，单会话历史版本）

本节记录 2026-10-20 的单会话实现，已被 2026-10-21 的多会话接口取代：当前所有运行期接口都带 `sessionId`，状态是 JSON，画面按「交付基线」取 `takeFrame`（见「多会话与窗口（2026-10-21）」与「输入与显示（2026-10-21）」）。以下保留原始记录。

界面改为长连接会话，不再是“连一次取一帧”：

- 当时 native 新增无 `sessionId` 的 `startSession(dir)` / `stopSession()` / `sessionStatus()` / `sendPointer(x, y, mask)`，`sessionStatus()` 返回 `key=value` 文本（错误写在 `error=`）。这套单会话接口只在过渡期作为兼容层保留，新界面不再依赖；当前契约见 [docs/napi-contract.md](../docs/napi-contract.md)。
- 当时界面点「连接」后每 30 ms 轮询该文本状态，用 `takeRegion` 原子取回「矩形 + 像素」，`writePixels` 只把变化区贴到整屏 `PixelMap` 镜像，再把整幅镜像 `drawImage` 到 Canvas 显示；点「断开」调用 `stopSession()`。
- `Image` 组件接收触摸事件，按显示区域与远端分辨率换算坐标，`Down`/`Move` 发 `mask=1`、其它发 `mask=0`，通过 `sendPointer` 写入 VNC 指针事件——这是用户真实操作，不再是写死的测试点击。
- 当时未信任服务器时，会话线程在认证前中止，`sessionStatus()` 返回 `error=server not trusted yet: <指纹>`，界面据此显示「确认并信任」；确认后写入信任库并自动重连。当前状态 JSON 用 `state=trust-required` 加 `fingerprint` 表达同一状态（见契约）。

开发机对 62 的自测（`--session-test`）：`start: STARTED` → 6 秒后 `running=1 version=1 size=1920x1200` → `pointer: OK` → 2 秒后 `version=6`（说明帧在持续更新）→ `stop: STOPPED`。

边界：界面拿到的是原始 `BGRA_8888` 变化区，未做带宽自适应；坐标按 `Contain` 等比缩放后的内容区换算，落在黑边的输入被忽略。

### 键盘转发（2026-10-20）

- native 新增 `sendKey(keysym, pressed)`：仅当会话处于 `NORMAL` 时调用 `CConnection::sendKeyPress/sendKeyRelease`，未连接返回 `NOT_CONNECTED`，握手未完成返回 `NOT_READY`。
- 键盘事件只在页面根组件上处理（`Remote.ets` 根 Column 的 `onKeyEvent`），按 `event.keyCode` 映射成 X11 keysym，**不能用 `keyText`**：HarmonyOS 的 `keyText` 对字母/数字返回 `"KEYCODE_A"` 这类常量名，当字符用会算出 0 并把按键丢掉。映射规则：字母 `KEYCODE_A..Z`（2017..2042）→ `0x61 + 偏移`、数字 `KEYCODE_0..9`（2000..2009）→ `0x30 + 偏移`、标点查表（空格/减号/等号/方括号/反斜杠/分号/引号/逗号/句点/斜杠/反引号）；特殊键显式映射（Enter `0xff0d`、Backspace `0xff08`、Delete `0xffff`、Tab `0xff09`、Esc `0xff1b`、方向键 `0xff51`–`0xff54`、Home/End/PageUp/PageDown、Shift/Ctrl/Alt `0xffe1`–`0xffea`）。Shift 组合由客户端本地换算上档 keysym（详见「输入与显示（2026-10-21）」）。按下发 `pressed=1`，抬起发 `pressed=0`。
- 键盘到达验证（开发机对 62）：打开应用菜单后发送 Down 键，逐像素比较选中项移动，输出 `PASS keyboard observed: 110 pixels changed after Down key`。

边界：输入法（IME）组合输入尚未处理，当前按物理按键逐个转发；Unicode 直接输入、粘贴、快捷键组合的本地拦截尚未验证。

### 隐藏本机光标（2026-10-20）

鼠标进入画面区域时调用 `pointer.setPointerVisible(false)` 隐藏本机光标，离开画面时恢复。该 API 是公开接口（`@ohos.multimodalInput.pointer`，API 9 起，无权限要求），作用范围是当前窗口；导入方式为默认导入 `import pointer from '@ohos.multimodalInput.pointer'`。断开连接时也会恢复可见，避免光标一直消失。

注意：2026-10-21 起改为客户端本地绘制远端光标——声明支持 `supportsLocalCursor` / `supportsCursorPosition`，光标按物理指针位置绘制，移动鼠标不产生画面更新；服务端首次下发的 `0x0` 空光标表示「这次它自己把光标画进画面」，此时客户端隐藏自己的光标图层，发过指针事件后服务端改发真实形状。详见「输入与显示（2026-10-21）」。

### 连接管理与密码保存（2026-10-21）

- **多连接管理**：主界面列出保存的连接（名称、协议与 `host:port`），可「使用」「删除」；表单填写名称/地址/端口/密码后可「保存连接」。列表存在应用沙箱 `filesDir/connections.json`，只含名称、地址、端口、协议、RDP 用户名/域和 `savePassword` 标志，**不含密码**。
- **密码保存**：勾选「保存密码」时，密码写入系统密钥库（`@ohos.security.asset`，`asset.add` / `asset.query` / `asset.remove`，别名 `tianguan:<host>:<port>`，可访问性 `DEVICE_FIRST_UNLOCKED`），不写入 `connections.json`、不写日志、不回显。若系统密钥库不可用，保存会失败并在状态栏提示，此时连接仍可用但密码不落盘。选择已保存的连接且勾选保存密码时，会自动从密钥库取回密码填入输入框。
- **协议：VNC / RDP（2026-10-24）**：连接可保存为 VNC 或 RDP。RDP 条目额外记录用户名、域与密码：用户名必填、域可留空；密码与 VNC 一样只进系统密钥库，但使用独立别名 `tianguan:rdp:<host>:<port>`，与同地址的 VNC 连接互不覆盖（VNC 继续沿用旧别名 `tianguan:<host>:<port>`，已存密码不受影响）。VNC 默认端口 5901，RDP 默认端口 3389；切换协议时，端口只在为空或仍等于另一协议默认值时才跟随切换，用户手填的自定义端口不会被覆盖。**RDP 已接入原生 FreeRDP 后端**（画面 + 键鼠；剪贴板、音频、证书固定等未做）：点「连接」或「重连」会像 VNC 一样创建会话，不再弹「尚未接入」提示；详见「RDP 接入（2026-10-24）」。
- **远程画面页**：连接启动后为该会话启动独立 `RemoteAbility` 实例（`multiton`），页面 `pages/Remote` 承载远程画面、鼠标键盘、光标与剪贴板；同一会话再次「打开」只激活已有窗口，不新建实例。打开失败直接结束该会话并提示，不再降级到主窗口。详见「多会话与窗口（2026-10-21，2026-10-23 改为多实例）」。
- **连接失败提示（2026-10-24）**：连接失败时主窗口会提示中文原因（密码错误、服务端指纹与已信任值不一致、网络连接中断、协议不匹配或服务器拒绝等），失败行显示「失败：<原因>」。密码错误时关闭该失败会话及其空窗口，并弹出密码重输框；确认后若该连接勾选了「保存密码」，先把新密码写入系统密钥库，再用新密码重新连接（不复用旧会话）。指纹不一致、网络、协议等其它失败只给一条带原因的提示。

冒烟状态（早于多会话改造）：签名包已安装并启动到手机 PLA-AL10（进程存活、无应用错误日志），但连接管理与远程窗口的交互尚未人工点验；PC 当晚关机，尚未部署。

边界：`connections.json` 明文保存地址与名称（不含密码）；窗口尺寸不持久记忆；同一会话同时只有一个远程窗口（再次打开是激活），多个会话各自独立实例。

### 指纹确认与连接界面（2026-10-20）

连接界面（`Index.ets`）提供服务器地址、端口、密码与「连接」按钮；地址默认 `192.168.50.62`，端口默认 `5901`。密码输入框使用 `InputType.Password`，不回显；点「连接」时密码只写入应用沙箱的临时文件 `filesDir/vnc-test.pass` 供 native 读取，连接结束（成功或失败）后由 ArkTS 删除，不写入信任库、结果文件或日志。启动时只自动运行内存诊断，不再自动发起网络连接。

信任库为 `filesDir/vnc-pins.json`，简单 JSON 对象，键为 `host:port`，值为服务端指纹：

```json
{"192.168.50.62:5901":"84-64-0d-21-6a-ab-fa-a6"}
```

旧版本使用的 `vnc-pins.txt`（每行 `<host>:<port> <指纹>`）不再作为主信任库：当 `vnc-pins.json` 不存在或为空时，界面会把旧文件内容迁移成 JSON 后再使用，此后以 JSON 为准。

流程：

1. 点「连接」时读取信任库；有该 `host:port` 的指纹时，`vnc-test.json` 带上 `expectedFingerprint`，没有则不带。
2. 无期望指纹：native 在认证前中止，返回 `NEEDS_CONFIRMATION: <指纹>`；界面展示该指纹与「确认并信任」按钮，此时未认证，不显示为已连接。
3. 点「确认并信任」：把 `host:port → 指纹` 写入 `vnc-pins.json`，立即带该指纹重连；成功后界面提示「已信任该服务器，连接成功（首帧已解码）」。
4. 已信任地址再次连接：指纹一致才认证；不一致返回 `FAIL fingerprint mismatch: got <实际>, expected <期望>`，界面显示「服务端指纹与已信任值不一致，已拒绝连接」，不会自动把新指纹写入信任库，也不显示为已连接。
5. 「重新检查指纹」按钮用空期望指纹重连，强制重新走一次展示与确认流程（更换服务端或人工复核时使用）。
6. native 返回的完整文本写入 `filesDir/vnc-result.txt` 并显示在界面。

失败语义：

- `NEEDS_CONFIRMATION: <指纹>`：未认证，等待人工确认；不是连接成功。
- `FAIL fingerprint mismatch: got <实际>, expected <期望>`：与已信任值不一致，已拒绝连接，信任库不变。
- 其他 `FAIL network test: <reason>`：TCP、认证、首帧或输入阶段失败，同样不显示为已连接。

边界：上述 2026-10-20 的验收范围只到 TCP 连接、RA2_256/RA2 加密认证、首帧解码与键鼠事件已发送，当时不渲染远程画面、不转发真实桌面会话、不含中文输入法、不支持 RDP，也不做 X509 证书校验，不验证服务端是否收到输入；实时画面与输入见「输入与显示（2026-10-21）」。首次连接必须人工核对指纹；确认后只保证之后同一地址的指纹变化会被拒绝，不代表服务端可信或会话安全。

### 本轮构建与验收（指纹确认，2026-10-20）

- host 构建：`cmake --build .work/hap-native-host --target tianguan_diag_host -j 4` 退出 0。
- 对 62 服务 `192.168.50.62:5901` 三组实测（密码只放在本地 600 临时文件，不写入仓库、文档或本输出）：

```text
# a. 不带 expectedFingerprint
requested security types: RA2_256,RA2
PASS TCP connected 192.168.50.62:5901
NEEDS_CONFIRMATION: 84-64-0d-21-6a-ab-fa-a6
exit_code=0

# b. 带正确指纹
requested security types: RA2_256,RA2
PASS TCP connected 192.168.50.62:5901
fingerprint: 84-64-0d-21-6a-ab-fa-a6 (trusted)
credential callback: userSlot=no, secure=yes
PASS authentication complete with RA2_256 (AES-256 encrypted)
PASS first frame 1920x1200
PASS center pixel 000000
PASS input sent (key a press/release, pointer 1,1 button1 press/release)
exit_code=0

# c. 带错误指纹 00-...
requested security types: RA2_256,RA2
PASS TCP connected 192.168.50.62:5901
FAIL fingerprint mismatch: got 84-64-0d-21-6a-ab-fa-a6, expected 00-00-00-00-00-00-00-00
exit_code=1
```

a 组只有 TCP 连接与指纹提示，没有认证回调或认证成功行；b 组完成认证、首帧与键鼠发送；c 组在认证前拒绝，且不写信任库。

- OHOS：`cmake --build .work/hap-native-ohos --target tianguan_diag -j 4` 退出 0；产物为 AArch64 shared ELF，`DT_NEEDED` 为 `libdeviceinfo_ndk.z.so`、`libace_napi.z.so`、`libc++_shared.so`、`libc.so`。
- `bash app/build.sh` 退出 0；unsigned HAP SHA-256 `f342a26b373c2bff4282fb1a128dde59cb71176266906b5b0996201803827e0a`；包内 `requestPermissions` 仅 `[{"name":"ohos.permission.INTERNET"}]`，`deviceTypes` 为 `["2in1","phone"]`，`pack.info` 的 `apiVersion` 为 26。HAP 打包会写入文件时间，同一源码重新打包的 SHA-256 可能不同，签名包以实际签名产物为准。
- 设备端：PC 上已由人工完成过一次「确认并信任」流程（当时信任库还是旧 `vnc-pins.txt` 格式），取回的结果显示 `fingerprint: 84-64-0d-21-6a-ab-fa-a6 (trusted)`、RA2_256 认证、首帧与 input sent，证据 `.work/device-runs/pc-ui-confirm/vnc-result.txt`。JSON 信任库版本（含旧格式迁移）的签名包由主会话安装启动，其点击流程未由本轮执行者复测。

### 配置文件与 hdc 回取

界面的「连接」每次都会用输入覆盖 `filesDir/vnc-test.json` 与 `filesDir/vnc-test.pass`，因此不需要手工推送这两个文件；密码文件只在连接期间存在，连接结束后由 App 删除。以下命令仅用于从已安装可调试包的设备回取结果与信任库；将 `<实际设备标识>` 替换为真实值：

```sh
hdc -t <实际设备标识> file recv -b cn.alfadb.tianguan "./data/storage/el2/base/haps/entry/files/vnc-result.txt" "./vnc-result.txt"
hdc -t <实际设备标识> file recv -b cn.alfadb.tianguan "./data/storage/el2/base/haps/entry/files/vnc-pins.json" "./vnc-pins.json"
```

信任库示例（`host:port` → 指纹）：

```json
{"192.168.50.62:5901":"84-64-0d-21-6a-ab-fa-a6"}
```

不要手工写入未经核对的指纹；删除应用数据（或移除信任库文件）后，下一次连接会重新走确认流程。真实密码只放在本地，不要写入仓库、文档或聊天。

### 上一轮构建与自测（基础网络测试，2026-10-20，历史记录）

- host：`cmake --build .work/hap-native-host --target tianguan_diag_host -j 2` 退出 0；原三项内存诊断仍全部 PASS，程序退出 0。
- host 网络自测（不连接真实服务器）：无 `vnc-test.json` → `SKIP network test: no vnc-test.json`，退出 0；有 json 无 pass → `SKIP network test: no vnc-test.pass`，退出 0；配置指向 `127.0.0.1:63999`（无监听）→ `FAIL network test: TCP connect failed: Connection refused`，退出 1，5 ms 内返回；对只 accept 不发 RFB 的本地静默服务 → `PASS TCP connected 127.0.0.1:63998` 后 `FAIL network test: timed out after 20s before authentication completed`，退出 1，实测 20.008 s。空文件、内部换行、超过 512 字节与坏 json 均 FAIL；带末尾 CRLF 的密码被接受并进入连接阶段。自测输出不含密码。
- OHOS：`cmake --build .work/hap-native-ohos --target tianguan_diag -j 2` 退出 0；产物为 AArch64 ELF shared object，`DT_NEEDED` 仅 `libdeviceinfo_ndk.z.so`、`libace_napi.z.so`、`libc++_shared.so`、`libc.so`，无 host 库、无 RPATH/RUNPATH；`RawDecoder::decodeRect` 与 `CSecurityRSAAES::processMsg` 的真实实现符号保留，NAPI 同时导出 `runDiagnostics` 与 `runNetworkTest`。
- `bash app/build.sh` 退出 0；新 unsigned HAP SHA-256 `44070a1fc733075bfd5c57bd016b8fd840b655cbc2e89ed4bf50f7eb8adc9117`，包内 `requestPermissions` 仅 `[{"name":"ohos.permission.INTERNET"}]`，`deviceTypes` 为 `['2in1', 'phone']`。该 unsigned 包的 `modules.abc`、`libtianguan_diag.so`、`module.json`、`pack.info` 与上文设备实测所用签名包逐字节一致。

## 本机 unsigned 构建

从仓库根运行：

```sh
bash app/build.sh
# 或指定另一代理实际生成的原生库
bash app/build.sh /absolute/path/libtianguan_diag.so
```

默认输入为仓库 `.work/hap-native-ohos/libtianguan_diag.so`。缺少输入明确非零退出，不生成替身。脚本校验 AArch64 shared ELF，复制到模块 `entry/libs/arm64-v8a`；实查 `DT_NEEDED`，若需要 `libc++_shared.so` 则从固定 SDK 复制。SDK 固定 `/home/worker/harmonyos/command-line-tools/26.0.0.821`，使用其 bundled Node、ohpm 和 hvigor，执行 `assembleHap --mode module -p product=default -p buildMode=release`。本工程只有本地 native 类型包及官方 SDK plugin 依赖，不配置签名，不读取 profile、keystore 或凭据，不自动安装。

输出：`app/entry/build/default/outputs/default/entry-default-unsigned.hap`。构建缓存与复制库位于本工程忽略的生成目录。本地 `cpp/types` 只有 API 声明，不编译或替代 `app/native` 实现。

## 历史验证记录（2026-10-16，纯诊断轮次）

该轮验证时间为 `2026-10-16T01:23:30Z`（UTC）。已运行 `cmake --build .work/hap-native-host --target tianguan_diag_host -j 2` 并执行 `.work/hap-native-host/tianguan_diag_host`，退出 0，同源真实三项全部 PASS：RA2_256 工厂、RFB 3.8 内存 Raw 2×1、Thread 协作退出。随后 `cmake --build .work/hap-native-ohos --target tianguan_diag -j 2` 与 `bash app/build.sh` 均退出 0，完成更新后的 OHOS shared 与 unsigned HAP。实查库依赖包含 `libc++_shared.so`，已随包携带 SDK runtime；包内诊断库与本轮输入经同一 SDK `llvm-strip` 后字节一致。包内 bundle、版本、API26、`2in1` 与 `phone` 双设备已核对（该历史纯诊断包无权限声明）。未重跑未触及的原 host 9 项或 RA2 socket 集成 3 项。此证据仅覆盖该轮 host 诊断、OHOS 编译链接与本机构建/包内核验；该轮未执行设备诊断。网络测试的设备结果见上文「实测结果（2026-10-20，指纹确认实现前的旧测试模式）」的历史记录。

此前纯诊断未签包 SHA-256：`4515858fe96fef2f85b231824ae815c5f02e56c4915dd64cd846213280f37c85`（`deviceTypes` 增加 `phone` 后重新运行 `bash app/build.sh`，退出 0；包内应用及入口 label 均为 `$string:app_name`，对应源资源值为“天关”，官方资源与 ArkTS 编译通过）。该轮输入共享库 SHA-256：`3032d2c0797d4017d0df7f26073bae693e7e5b6bd74f95840ed14433ae15f8bf`。**未签包不可直接作为可安装交付**；该历史包未签名、未装机。

后续如 native 更新，重新构建并核对包内库。签名前需要以下本机材料路径：

- debug profile `.p7b`：bundle 必须为 `cn.alfadb.tianguan`，并包含已登记的 MateBookFold 调试设备；不能默认使用 NetBird profile。
- 与该 profile 匹配的签名证书及私钥/keystore 的本机路径。密码不要写入聊天、源码或文档。

用户已确认本包 debug profile 尚未创建。后续需在 AGC / DevEco 生成本包的 debug profile，并提供匹配证书与私钥的本机路径。可使用同一个开发者证书另发本包 profile，但本轮尚未验证既有证书可复用。字段要求见已核对的[官方 profile 格式文档](https://raw.githubusercontent.com/openharmony/docs/master/zh-cn/application-dev/security/app-provision-structure.md)（`bundle-info.bundle-name`、`debug-info.device-ids`）。本轮未申请签名或安装，也未读取或验证任何 profile；材料待准备不表示用户拒绝或权限不足。

获授权并准备匹配的调试签名后，通过官方 HAP 安装/`aa start` 路径启动。以下命令仅在应用已调试签名、安装并启动，沙箱已挂载且诊断文件保存成功后有效；`<实际设备标识>` 必须替换为目标设备的真实标识：

```sh
/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/toolchains/hdc -t <实际设备标识> file recv -b cn.alfadb.tianguan "./data/storage/el2/base/haps/entry/files/diag-result.txt" "./diag-result.txt"
```

当前使用的 `UIAbilityContext.filesDir` 属于 module-level context，模块名为 `entry`，因此沙箱路径包含 `haps/entry`，不能省略为短路径。参见[官方 Stage 应用上下文说明](https://raw.githubusercontent.com/openharmony/docs/master/zh-cn/application-dev/application-models/application-context-stage.md)和[官方应用文件访问说明](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-device-file-explorer)。若 `libtianguan_diag.so` 静态 import 失败，应用尚未进入诊断，不会生成诊断文件；应查看启动及模块加载日志。文件不存在不能视为 PASS。该历史纯诊断轮次未连接设备；网络测试的签名、安装与设备结果见上文「实测结果（2026-10-20，指纹确认实现前的旧测试模式）」的历史记录，该结果只证明连接、RA2_256 认证、首帧与键鼠发送，不证明服务端收到输入，也不代表生产安全策略或完整远程桌面能力。

## 多会话与窗口（2026-10-21，2026-10-23 改为多实例）

本节描述当前多会话窗口模型，接口以 [docs/napi-contract.md](../docs/napi-contract.md) 为准（含 `sessionId`、`pendingCloses`、`takeClipboardOrNull`、`requestFullFrame`、`copyCursor(sessionId, …)`）。

### 原生侧：每个会话独立

- 每个运行期会话有独立 `sessionId`（`createSession` 返回），所有运行期接口都带 `sessionId`。socket、会话线程、输入队列、帧缓冲、光标、`generation` 与剪贴板状态均按会话隔离。
- `generation` 每次重连加一；迟到的帧、迟到的输入和针对旧连接的身份确认都会被拒绝。会话状态以 JSON 返回，界面不解析英文错误串。
- `closeSession(sessionId)` 非阻塞：立即摘除会话、停止读、清输入，线程回收交给后台；`pendingCloses()` 返回尚未回收完的会话数。

### 独立 UIAbility 实例：每会话一个窗口

- 每个远程会话是一个独立的 `RemoteAbility` 实例（`module.json5` 中 `launchType: "multiton"`，页面 `pages/Remote`）。每次 `startAbility` 都新建实例、任务和顶层窗口，平台不会自动去重，去重由应用负责。
- 窗口名由系统分配（`tianguanN`，递增且不复用），不能从 `sessionId` 推导。进程内用 AppStorage 维护双向注册表：`remote.window.<sessionId>` = 窗口名，`remote.session.<窗口名>` = `sessionId`；ability 在 `loadContent` 之前写入两条键。
- 打开会话（列表「打开」、托盘点会话、新建会话）统一走 `openRemoteWindow(sessionId, 名称, context)`：先查注册表并 `window.findWindow(窗口名)`，窗口存在则 `show()`（主窗口在前台时再 `shiftAppWindowFocus`），返回 `activated`；否则 `startAbility` 启动新的 `RemoteAbility`，最多轮询约 2 秒等注册表出现并校验窗口可找到，返回 `opened`。同一会话的并发打开共享一次在飞请求，双击或「列表 + 托盘」同时触发不会开出两个实例。
- 失败语义：拿不到可用的 ability context 返回 `no-stage`；启动或等待窗口失败返回 `failed` 并结束该会话。**任何失败都不再降级到主窗口内显示**（没有标题、也无法用关窗表示断开）。
- 窗口标题为连接名（拿不到时用 `host:port`，再退到 `sessionId`）。
- 新建会话窗口按屏幕摆放并居中：优先取窗口自身 `displayId` 对应屏幕（`display.getDisplayByIdSync`），失败回退默认屏；宽 = min(1620, 屏宽×0.92)，高 = min(1080, 屏高×0.88)，下限 640×400，四周留 12 px 余量；`left/top` 取屏幕内居中位置。窗口显示后先让系统完成首次布局（约 400 ms），再用 `resizeAsync` + `moveWindowToAsync` 应用矩形，约 200 ms 后按窗口自身 `windowRect`（并记录 WMS `getAllWindowLayoutInfo` 作交叉核对）确认，未命中最多重试 3 次；全部失败只记日志、保留系统默认摆放，不影响会话。位置与尺寸不持久化，用户之后手动移动/缩放不会被覆盖。

### Dock 与系统窗口选择器

- Dock/任务栏里始终只有一个「天关」应用图标（系统按应用合并，没有公开 API 让每个窗口单独占一个 Dock 条目）。
- 点击这个图标会弹出系统窗口选择器：主窗口和每个远程会话各一张带标题的卡片。点卡片直接激活对应窗口，不必先进主窗口再点「打开」；最小化的实例也仍可从这里激活。
- 选择器只列 ability 主窗口。旧的 `remote-<sessionId>` 子窗口不会出现在其中，这正是本次从子窗口改为独立实例的原因。

### 会话身份：不猜

- 页面只从两处取会话 id：本页所在窗口名经 AppStorage 键 `remote.session.<窗口名>` 解析（主通道，ability 在 `loadContent` 前写入），或显式路由参数 `sessionId`（兼容保留）。两处都没有时报错，显示「无法确定本页所属的会话」，**禁用全部输入**，只留关闭窗口的入口。
- 两处都存在但互相冲突时同样拒绝挂载。没有 FIFO、最近窗口或「取注册表第一个会话」之类的兜底。

### 关窗与退出

- 关闭远程会话窗口（系统标题栏 X）= 断开该会话：`windowStageClose` 放行，页面 `aboutToDisappear` 做清理，`onWindowStageDestroy`/`onDestroy` 删除注册表键，`onDestroy` 调用 `closeSessionById(sessionId)`。只结束该实例，其它会话和主窗口不受影响。程序化关闭（删除连接、彻底退出）先用 AppStorage 标志排除，避免重复结束会话。
- 关闭主窗口 = 转入后台，不退出、不断开会话：拦截主窗口的关闭事件并调用 `hideAbility()`（失败退回 `minimize()`）；托盘未启动时直接 `minimize()`。窗口隐藏后进程、托盘与会话继续运行，托盘或热启动用 `showAbility()` 恢复。
- 托盘菜单的「打开 <会话>」同样走 `openRemoteWindow`，已有窗口就激活；主窗口隐藏时源窗口不在前台，只 `show()` 不再强转焦点。托盘右键菜单送达时系统菜单仍持有焦点，主窗口可能在其后又被抬到前台，因此激活已有窗口会在 250 ms 后再补一次 `show()` + 焦点转移（日志 `remote-window activate` 后又出现 `remote-window reactivate`），保证会话窗口最终在最前。
- **已知限制**：托盘菜单点击会让平台顺带启动一次 `EntryAbility`，其 `onNewWant` 会把主窗口显示出来。主窗口已隐藏时从托盘打开会话，会话窗口会正常出现在最前，但主窗口也会出现在它后面（不遮挡）。曾尝试抑制这次 `onNewWant` 并在 500 ms 后 `hideAbility()` 收回，但真机上会连远程会话窗口一起隐藏、后续 `show()` 也恢复不了（multiton4 回归），因此已回滚；会话可打开性优先。
- 只有「退出」才清理：非阻塞关闭所有会话 → 轮询 `pendingCloses()` 最多 3 秒 → 写 AppStorage `remote.quit = true` → 等待约 400 ms → 移除托盘 → `terminateSelf()`。远程实例每 200 ms 检查该标志并各自 `terminateSelf()`（主 ability 的 `terminateSelf()` 只结束自己），因此不会留下其它 ability 实例。托盘自带的「退出」行由 `onPrepareToTerminate` 接管：首次请求取消并异步清理，5 秒内再次请求直接放行，避免退不掉。

边界：Dock 无法为每个会话显示独立图标（平台按应用合并）；窗口选择器比「直接点 Dock 会话图标」多一次选卡片。注册表是进程内状态，进程重启后自然失效。窗口名由系统分配，重开同一会话会得到新窗口名。关窗即断会话是产品约定，已在真机验证（见文末「验证边界」）。

## RDP 接入（2026-10-24）

- **ArkTS → 原生链路**：`connect()` 组装 `SessionOptions` 时按协议分流。RDP 条目带 `protocol: "rdp"`，并在非空时带 `username`/`domain`；密码仍从系统密钥库的 `tianguan:rdp:<host>:<port>` 别名读取（`loadPassword(entry)`）。VNC 条目不传 `protocol`（缺省即 VNC），`security`（None 语义）与 `expectedFingerprint`（`vnc-pins.json` 信任库）也只在 VNC 分支设置。字段定义见 `app/entry/src/main/ets/diag/SessionApi.ets` 的 `SessionOptions`，契约见 [docs/napi-contract.md](../docs/napi-contract.md)。原来的「RDP 连接尚未接入」拦截（`connect()` 与 `reconnectEntry()` 各一处）已删除。
- **原生后端**：FreeRDP 3.9.0 静态链接（`libfreerdp3` / `libfreerdp-client3` / `libwinpr3`，加静态 OpenSSL 3.5.9 与 zlib），依赖由 `scripts/build-rdp-deps.sh ohos` 用 OHOS SDK 交叉编译并安装到 `.work/rdp-deps/ohos/prefix`（需要仓库内 `.work/freerdp-spike` 的固定 FreeRDP 3.9.0 源码检出）。`app/native/CMakeLists.txt` 的 OHOS 目标始终定义 `TIANGUAN_WITH_RDP=1` 并链接该前缀；前缀缺失时构建直接失败（编译期找不到 FreeRDP 头文件，链接期缺 `freerdp-client3` 等静态库），不会产出静默回退到 RFB 的包。host 目标不编译 RDP 后端，`app/native/diagnostics.cxx` 在未定义该宏时把 RDP 入口编译成 `failed` 阶段加 `protocol` 错误码的兜底。
- **体积**：静态链接把 `libtianguan_diag.so` 从约 1.8 MB（上一版 VNC-only 构建为 1,867,232 B）推到约 12 MB（当前 12,533,944 B），unsigned HAP 相应变大；这是为在设备上省掉额外共享库依赖付出的代价。
- **复用度**：RDP 画面走 FreeRDP 的 GDI 位图/绘制订单路径，`EndPaint` 后把脏区提交进与 VNC 相同的帧镜像与 `version`/`generation` 规则，因此 Remote 页、原生 EGL 渲染器、多窗口、托盘与会话注册表都不按协议分支。状态机与 `errorCode` 也复用（`auth-failed`/`protocol`/`network`），主界面的失败原因提示与「密码错误」重输弹层对 RDP 同样生效。
- **已知限制**（首版验收范围定为「画面 + 键鼠」）：无剪贴板通道（`CHANNEL_CLIPRDR=OFF`，远端复制内容被丢弃）；无 GFX / RemoteFX / 视频编码（`CHANNEL_RDPGFX=OFF`、`CHANNEL_VIDEO=OFF`、`WITH_FFMPEG=OFF`，只走 GDI 位图与绘制订单）；无音频（`CHANNEL_RDPSND=OFF`）；无 IME 组合输入与 Unicode 直接输入（键盘仍按物理按键逐个转发）；证书不做固定（RDP 服务端证书仅本次会话接受，不写入 `vnc-pins.json`、不弹确认，VNC 的指纹信任流程不适用于 RDP）；驱动/打印机/智能卡/摄像头/多显示器等重定向通道均未启用。
- **验证状态（2026-10-24）**：已在真机（MateBook Fold SLM-W32，调试签名、`install -r` 覆盖安装）连接 192.168.50.12 与 192.168.50.18。两台均完成 TCP/TLS/NLA 登录、首帧真实桌面、鼠标点击（远端弹出 Windows 开始菜单、通知中心/日历面板）与键盘（Esc 关闭菜单）。设备侧 /proc/<pid>/net/tcp 可见到两台的 3389 连接为 ESTABLISHED。证据在 .work/device-runs/rdp-btnfix-20261024/（12）与 .work/device-runs/rdp18-final-20261024/（18）。已知现象：18 的画面更新在实测中滞后十几到几十秒，原因尚未定位。

## 系统托盘（2026-10-21）

- 仅 `2in1`（PC）启用：先按 `deviceInfo.deviceType === '2in1'`、`canIUse('SystemCapability.PCService.StatusBarManager')` 和平台 capability 探测三重判断，再动态加载 `@kit.DeskTopExtensionKit` 的 `statusBarManager`（`@hms.pcService.statusBarManager`，无权限要求）。手机/平板或缺少该能力的系统静默降级，只记一条日志，不加载该 kit。
- 左键点击托盘图标打开/激活主窗口（`showAbility()` 加自身 `startAbility` 热启动，覆盖「已隐藏」和「仅后台」两种情况）。
- 右键菜单：第一组「打开主窗口」，第二组为每个活跃会话一行「打开 <名称>」，最多 18 行；活跃会话超过 18 个时追加「更多会话…」，该行只打开主窗口。已断开或失败的会话不出现在菜单里。
- 系统自带的「退出」行保留，由 `onPrepareToTerminate` 执行完整清理（见上一节）。
- 平台/运行期失败只降级、不影响会话：模块缺失或 capability 不支持时永久关闭托盘功能；图标/能力探测的瞬时失败不会锁死功能；菜单与悬停提示更新失败保留旧值，下次刷新重试；点击处理失败只记日志。菜单更新做 500 ms 合并，避免平台限流。
- 悬停提示为「天关 · N 个会话」，无活跃会话时为「天关 · 未连接」。

## 自动剪贴板（2026-10-21，2026-10-23 改为事件驱动）

- 双向自动同步，仅纯文本；`\r\n` 与 `\r` 统一归一化为 `\n`。每个会话有独立剪贴板版本，来自状态 JSON 的 `clipboard.version`。
- 远端 → 本机：状态里的版本变化时按会话取一次（`takeClipboardOrNull(sessionId)`），把文本写入本机系统剪贴板；同一版本只取一次。这条方向由状态轮询驱动，不需要读取权限。
- 本机 → 远端（事件驱动，不再定时读剪贴板）：用 `systemPasteboard.on('update', onLocalUpdate)` 监听本机剪贴板变化，回调里只计数并发起一次**异步** `await getData()`；比较、广播与防回环都在 Promise 落地后执行。同一时刻只允许一个读取在途（`LocalReadGate`），在途期间到达的 `update` 合并成「还要再读一次」，不会堆叠。监听只在至少一个会话页可见时注册，最后一个可见页隐藏/销毁时用同一回调引用注销。模块启动/授权后的第一次成功读取只建立基线，不把剪贴板里的历史内容推给远端。
- 兜底与重试：`on('update')` 注册抛错（API/ROM 不支持）时退回 500 ms **异步**兜底轮询（仍只用 `getData()` Promise，无 `getDataSync`）；正常路径为 `mode=event`。另有 1 s 的按需重试定时器，只在「本机文本还有会话没接受」或「远端文本还没写进本机」时存在，且**从不读剪贴板**，替代原来「下一轮轮询再试」的语义。
- 投递语义：本机文本按会话调用 `setClipboard(sessionId, text)`，并记录每个会话已投递的本地版本；只有返回 `QUEUED`/`COALESCED` 才算投递成功，断连或当时未接受的会话由上述按需重试补投，不要求重新复制。
- 防回环：每次写本机剪贴板**之前**先在 `ClipboardLoopGuard` 记账，`update` 事件（或兜底轮询）读到这段文本时按「自己写的」跳过，不再回发；本机到远端与远端到本机两个方向共用一个 Promise 串行队列，避免交错。
- 读取失败：`takeClipboardOrNull` 返回 `null` 表示读取失败，**不消费版本**，保留待处理项，按需重试；返回空串才表示「确实没有新内容」，可以消费版本。写本机剪贴板失败同样保留内容重试。
- 超过 1 MiB 的文本按 UTF-8 码点边界截断到 1 MiB 以内，并记录截断计数；原生侧对服务端文本同样限长，超出时在状态 JSON 标 `clipboardTruncated: true`。
- 读取权限：本机 → 远端读取系统剪贴板需要 `ohos.permission.READ_PASTEBOARD`（system_basic / user_grant）。**当前 `module.json5` 因 ACL 为空保持移除该声明**：声明了该受限权限、而签名 Profile 的 ACL（`acls.allowed-acls`）未允许的 HAP 会在**安装阶段**被系统拒绝（`install failed ... grant request permissions failed`，错误码 9568289）。ACL 审批通过后需**恢复 `module.json5` 中被注释掉的声明块并重新签名**（`abilities` 加上 `RemoteAbility`）；代码侧申请/门控已就绪：`clipboardRequestReadPermission()` 在打开远程会话时调用，先 `checkAccessToken` 判断，未授权则 `requestPermissionsFromUser` 申请一次（每进程最多一次），未授权时读取路径完全停用（不注册 `update`、不启动兜底轮询、不调用 `getData`），日志可见一次性 `TianguanClip perm=0 ... gate=` 说明。「远端 → 本机」写本机剪贴板不需要该权限，不受影响。
- 后台限制：应用在后台时读取系统剪贴板受系统权限限制，「本机 → 远端」会暂停；回到前台、会话页重新可见后重新注册事件监听并按需补读，不再依赖固定轮询。「远端 → 本机」的写入不受此限制。
- 远端复制的内容只写入本机剪贴板，不会自动转发给其它会话；只有用户在本机真实复制的新文本才会广播给各会话。
- 设备实测（SLM-W32 / API 26 / 调试签名）：事件驱动版（加权限门控之前）日志为 `mode=event` 且 `updateEvents` 递增，确认 `on('update')` 在该 ROM 正常投递；异步读取仍被 ACL 拒绝（`bytes=0`、约 100 ms 的 IPC），但它已离开 UI 线程、且没有事件就不读，用户确认此前的周期性卡顿消失。加入权限门控后的当前版本在未授权时直接 `mode=off`、不注册监听也不读取（本轮最终包实测）；授权弹窗/允许后回到 `mode=event` 的本机 → 远端同步路径尚未真机验证（等 ACL）。

## 输入与显示（2026-10-21，2026-10-22/23 呈现改为 XComponent + GL）

本节是远程画面与输入链路的当前实现说明，面向使用者与后续接手者。更早的 JPEG 整帧快照、在主窗口内打开远程页与按 `keyText` 映射的写法已不代表现状，上文对应小节已就地订正。

### 画面

- 默认呈现路径：`XComponent(SURFACE)` + 原生 EGL/GLES 渲染线程（`USE_GL_RENDER = true`，每会话一条，`app/native/render.cxx`）。`attachRenderSurface(sessionId, surfaceId, startSuspended)` 把 surface 绑定到该会话的原生渲染器，surface 重建/销毁或页面释放时 `detachRenderSurface(sessionId)`；窗口隐藏时 `suspendRenderSurface(sessionId, true)` 停住渲染线程，恢复可见时置 false 并请求一次全帧。GL 模式下 ArkTS **不调用** `takeFrame`，原生渲染线程是唯一帧消费者，避免两条路径同时分配纹理。
- 原生取帧与上传：渲染线程经 `takeFrameStruct` 读该会话的 BGRA 镜像（与 `takeFrame` 同一把 pix 锁、同一交付基线与 `takenVersion`/`pixEpoch` 语义，但不序列化 JSON），只把自上次成功交付以来的变化矩形拷入 staging。首帧、整帧或分辨率变化时用 `glTexImage2D` 重建整幅纹理，否则 `glTexSubImage2D` 只上传变化矩形；全程一张纹理。BGRA 字节按 `GL_RGBA` 上传，fragment shader 用 `vec4(texel.bgr, 1.0)` 还原颜色。contain 缩放与黑边每帧按 `eglQuerySurface` 的实际尺寸重算；`eglSwapInterval(1)` 跟随 vsync，没有新版本时 4 ms 轮询且不 swap。
- 状态轮询：GL 模式前台每 250 ms 轮询 `sessionStatus(sessionId)`（JSON），驱动状态文本、指纹确认面板、光标形状/版本与剪贴板版本；Canvas 模式保持 30 ms。输入映射与本地光标位置改用 `renderStats()` 的 `texWidth/texHeight`（实际呈现的纹理尺寸），远端分辨率变化时立即对齐，不等状态轮询。页面/窗口不可见时降为 1 s 只读状态，释放按键/按钮并 park 原生渲染器。

- 交付基线（原生取帧共用）：`afterVersion` 必须等于上一次成功交付的版本（原生记录的 `takenVersion`，且 `pixEpoch` 一致），否则返回整帧而不是局部块。基线只在像素 `memcpy` 成功后推进；`NONE` 与 `ERROR:BUFFER_TOO_SMALL` 都不推进。
- 整帧请求：空闲桌面不会主动推全帧；首帧、窗口由隐藏转可见、远端画面尺寸变化或 `generation` 变化后请求一次非增量整屏更新。**本机窗口缩放不请求服务端全帧**：GL 模式下由原生按新 surface 尺寸重算 contain 缩放与黑边；Canvas 模式下 `onAreaChange` 只更新视图尺寸并重绘当前镜像。本机窗口尺寸不改变远端分辨率。
- 动态分辨率：服务端可中途改变分辨率。GL 模式下原生发现镜像尺寸变化即重建整幅纹理并强制全帧；ArkTS 从 `renderStats()` 同步新的 `texWidth/texHeight`。Canvas 模式下界面丢弃旧 `PixelMap` 与在途渲染，按新尺寸重建帧缓冲并强制全帧。`generation` 变化（重连）同理。
- Canvas 路径（保留开关，默认关闭）：`USE_GL_RENDER = false` 恢复旧的 Canvas + 整屏 `PixelMap` 镜像 + 每帧 `drawImage` 管线。**已知 GPU 缓存问题**：该路径每帧让 RenderService 缓存一份纹理和一份 Vulkan buffer（1920×1200 时各约 8.79 MB），达到约 4 GB 应用 GPU 配额后进程被 SIGKILL，严重时会触发内核 OOM；这正是迁移到 GL 的原因。该路径只用于对比/回退，两条路径不得同时挂载。Canvas 细节：原生按会话把累计变化合并成包围盒，经 `takeFrame(sessionId, buffer, afterVersion, forceFull)` 在同一把锁里原子返回「矩形 + 像素」，像素格式 `BGRA_8888`、`stride = w * 4`；界面 `writePixels` 贴变化区到整屏镜像后再整幅 `drawImage`（分块重绘实测更差，已回退），同一时刻只允许一个渲染在途，旧代次帧丢弃且不推进版本。
- 像素字节序是 BGRA 而不是 RGBA，按 RGBA 解释会红蓝互换。

### 光标

- 光标形状按会话取：`copyCursor(sessionId, buffer)` 把该会话当前光标图像以 `RGBA_8888` 拷入 `buffer`；状态 JSON 的 `cursor.version` 不变就不重复取，版本递增时再取一次。
- 客户端声明支持本地光标（`supportsLocalCursor` / `supportsCursorPosition`），光标由客户端按物理指针位置绘制，移动鼠标不产生画面更新，也不需要网络往返。
- 位置输入驱动：`onMouse` / `onTouch` 更新 `viewPointerX/Y` 后立即按当前缩放与黑边偏移重算本地光标位置，不再等状态轮询；轮询只在光标形状或视图尺寸变化后作为兜底重算。重算先四舍五入再比较，位置没变不写 `@State`；指针落在黑边内或光标隐藏时不动。
- 服务端首次会下发 `0x0` 空光标（TigerVNC 的 `emptyCursor`），语义是「这次服务端自己把光标画进了画面」：客户端此时隐藏自己的光标图层；客户端发过指针事件后，服务端改发真实形状，客户端再按热点与视图缩放比绘制。

### 鼠标

- 指针事件按队列发送：界面只入队，由会话线程写 socket（会话线程可能正卡在阻塞读上，直接写会丢掉抬起事件）。
- 合并规则：只有「队尾也是指针移动、且掩码相同」时才用最新坐标覆盖队尾。掩码不带滚轮位（8/16）的移动都算移动，所以按住左键拖拽（掩码 `1`）时也合并；按下、抬起、滚轮和按键永远入队，掩码变化就是边界，边界事件的坐标与顺序不会被后续移动覆盖。队列超过 4096 时 `trimInputQueue` 只丢最旧的「非边界移动」，保留最新位置与所有按下/抬起/滚轮事件。
- 输入插队：每个 framebuffer update 的每个矩形解码后都会调用一次 `drainSessionInput`，大更新期间指针/键盘事件按矩形边界发出，不必等整段更新结束；发送只走输入方向、每条 RFB 消息整条写出，不打断协议解析。空闲时 socket `poll` 超时由 5 ms 降为 1 ms。
- 按键掩码：左键 `1`、中键 `2`、右键 `4`；滚轮用按钮 4/5，掩码 `8`/`16`，经 `onAxisEvent` + `getVerticalAxisValue()` 取值后按「按下 + 抬起」成对发送。

### 性能打点（perf）

- 原生每秒聚合一次，在 `sessionStatus` JSON 末尾新增 `perf` 对象（只追加、不改既有字段）：`windowMs`、`frames`（该秒提交的 framebuffer update 数）、`delivered`（该秒成功交付的帧数），以及 `decode` / `convert` / `pixWait` / `commit` / `takeFrame` 五组 `{n, mean, p50, max}`。耗时单位是微秒；`decode` 是解码 flush 到转换/拷贝前，`convert` 是把接收矩形转成 BGRA 镜像像素（格式可直接复用时是逐像素 4 字节拷贝并置 alpha=255，否则是 RGB→BGRA 转换），`pixWait` 是提交前等像素锁的时间，`commit` 是持锁写入整屏镜像并推进版本的时间，`takeFrame` 是每次取帧调用（含返回 `NONE` 的调用）。每组最多保留 512 个样本，不逐帧刷日志。
- 像素格式：`perf` 之后另有只追加的 `pf` 对象，报告 framebuffer 实际协商到的格式 `{bpp, depth, redMax, greenMax, blueMax, bigEndian, redShift, greenShift, blueShift}`（来自 `framebuffer->getPF()`；首个 framebuffer 建立前全为 0）。当其为 32bpp、小端、`redMax/greenMax/blueMax=255`、`redShift/greenShift/blueShift=16/8/0`（即内存里的 BGRX）时，原生直接把每行拷进 BGRA 镜像并跳过 `rgbFromBuffer` 与逐像素通道重排；**alpha 写 255，不依赖渲染端忽略 alpha**：服务端的 X 填充字节不是 alpha 值（Xvnc 实测为 0），即使 PixelMap 声明 `BGRA_8888` + `OPAQUE`，也不把「ArkUI/Canvas 会忽略 alpha」当作前提，拷贝时逐个像素 `| 0xFF000000`。其它格式仍走原转换路径，只把每帧新建的 `row`/`converted` 改为复用缓冲。
- ArkTS 侧用单调时钟测 `takeFrame` 同步段、`writePixels` await、`drawImage` 调用段与轮询间隔，并计数 `applying` 跳过次数和实际绘制帧数；每 2 秒用 `hilog` info 打一行 `TianguanPerf ...`，格式为 `poll/take/write/draw=均值/最大值`（毫秒），并附带 `cursor={blank,w,h,visible,left,top}`（服务端光标语义与本地光标图层是否可见）、`ptr={x,y}`（最近一次物理指针的窗口坐标）、`lastMeta={x,y,w,h,full}`（最近一次应用的更新矩形）与 `full=N/M`（本窗口整帧次数/应用帧数），用于区分「远端没发帧」「本地光标没画」这类问题。计时与这些字段不改变渲染行为（没有为测量新增 `await` 或同步点），也不新增 UI 元素。
- 日志级别：周期性的 `TianguanPoll`（ArkTS 轮询分段）与 `TianguanRender`（原生每秒 fps/上传耗时）已从 WARN 降为 **debug**，`TianguanClip` 的 2 s 汇总同样为 debug；正常使用不再周期性刷 warn。一次性诊断（如首帧 `TianguanDiag first-frame pf ...`、渲染线程失败、重连）与真正的错误仍为 WARN/ERROR。

### 键盘

- 键盘事件只在页面根组件上统一处理（`Remote.ets` 根 Column 的 `onKeyEvent`）。**不要**再把 `onKeyEvent` 同时挂在画面容器与根组件上：key 事件会冒泡，两个处理器会让每个键转发两次、按下抬起顺序错乱。
- HarmonyOS 的 `KeyEvent.keyText` 对字母/数字返回的是常量名（如 `"KEYCODE_A"`），不能当字符用；必须按 `event.keyCode` 映射：
  - 字母 `KEYCODE_A..Z`（`2017..2042`）→ `0x61 + 偏移`；
  - 数字 `KEYCODE_0..9`（`2000..2009`）→ `0x30 + 偏移`；
  - 标点查表：空格/减号/等号/方括号/反斜杠/分号/引号/逗号/句点/斜杠/反引号。
- Shift 组合：目标服务端不按修饰键状态处理（单独发 Shift 按下再发 `a`，远端仍是小写），因此由客户端维护 Shift 状态，按下时直接把字符换成上档 keysym（`A`、`!`、`@` 等），并保证同一物理键的按下与抬起使用同一个 keysym。
- **按键抬起修复（systemKeyCode 配对）**：原生输入队列原先用 `sendKeyPress(0, 0, keysym)` 登记按下，而 `CConnection::sendKeyRelease()` 按 **systemKeyCode** 查 `downKeys[]`，按下记在 `0`、抬起查 keysym 必然 miss，所有抬起被静默丢弃——字母按下即出字符看似正常，但 Ctrl/Shift/Alt 会永远停在按下状态。现在 `drainSessionInput()` 统一用 keysym 作 systemKeyCode 登记（`sendKeyPress(keysym, 0, keysym)`），`sendKeyRelease(keysym)` 与 `releaseSessionInput()` 都能命中同一键；`keyCode` 仍为 0，线上仍是标准 RFB KeyEvent 格式。host key-wire 断言（Shift+a 共 4 条事件、重复 release 无写入）与本地 Xvnc + xev 实测（按下后 300 ms 的 release 送达，修复前要等断连兜底才松键）均已通过；真机会话窗口内键盘输入也已验证。

### 已知限制

- 独立实例窗口中的键盘/鼠标已真机验证（见下节「验证边界」）；滚轮只走过代码路径，未做可视化真机验收。
- 中文输入法（IME 组合输入）未处理，键盘按物理按键逐个转发；Unicode 直接输入、粘贴与部分快捷键组合未验证。
- 本机 → 远端自动剪贴板受 `READ_PASTEBOARD` 的 ACL 限制（声明已暂时移除，授权路径代码就绪但未真机走通）。
- 窗口位置/尺寸不持久记忆：新建时按屏幕比例计算并居中，之后用户手动移动或缩放不会被应用覆盖。

## 验证边界（2026-10-24 更新）

设备为 SLM-W32（HarmonyOS 7.0.0.111 / API 26，调试签名，`192.168.50.183:43833`）。以下按“真机验证过”和“未做/受限”分开，避免把构建通过当成功能通过。

### 已真机验证

- **多实例窗口**：每会话独立 `RemoteAbility`（multiton）；同一会话再次「打开」只激活已有窗口，不新建；关掉一个会话窗口只断该会话并 `terminateSelf()`，其它会话继续出帧。
- **新会话窗口居中/不超屏（2026-10-24）**：在 3000×2000 屏（窗口 `displayId=1`）上依次新建两个会话窗口，`getWindowProperties().windowRect` 与 WMS `getAllWindowLayoutInfo` 均记录为 `[690,460,1620,1080]`，水平/垂直严格居中（误差 0），尺寸在屏幕内、四周余量 690/690 与 460/460 px；第二窗口与第一窗口完全重叠（按需求先严格居中，不做层叠偏移）。
- **Dock 与系统窗口选择器**：Dock 始终只有一个「天关」图标；点击后弹出选择器，主窗口和每个会话各一张卡片，点卡片直接激活对应窗口（含最小化实例）。
- **托盘**：左键打开/激活主窗口；「打开 <会话>」能把会话窗口带到最前（含主窗口在前台的情况）；系统托盘的「退出」能完成会话回收并清理所有实例与窗口。
- **键鼠**：会话窗口内键盘输入可在远端 XFCE 终端执行（`echo` 等）；鼠标点击能获得焦点并把输入送到远端；按键抬起经 systemKeyCode 配对修复后不再卡住修饰键。
- **远端 → 本机剪贴板**：远端终端复制后本机剪贴板被写入（日志 `clipboard 远端->本地 同步 N 字节`，本机输入法出现 `local_data_notify`）。
- **GL 呈现与显存**：真实 62 会话连续 24 分钟以上、3 分钟采样内应用 GPU 24.4–33.5 MB 波动（增长恒为 3 MB，无随帧数线性增长），1 个 GPU context；`RENDER_MEMORY_OVER_ERROR`/`KillProcessByPid` 计数为 0，进程存活；受控 60 fps 内容下 fps 62.4–69.4、swapFail=0；颜色与 Canvas 对照最大差 0.7/255，contain 布局/黑边与 Canvas 对齐；两会话同时渲染正常。
- **无周期卡顿**：事件驱动改造完成后实测 `TianguanClip` 为 `mode=event`、`updateEvents` 递增，用户确认此前的周期性卡顿消失；当前版本在未授权时直接停用读取（`mode=off`，不注册监听），授权后的 `mode=event` 路径等 ACL 到位后再复测。

### 未做 / 受限

- **本机 → 远端剪贴板**：等 AGC 的 `READ_PASTEBOARD` ACL 审批；当前声明保持移除，读取完全停用，授权弹窗/允许后的自动同步未真机走通。
- **主窗口隐藏态从托盘打开会话**：会话窗口正常出现且在最先，但平台会顺带拉起主窗口并显示在会话窗口后面（不遮挡，属外观限制）。曾尝试抑制/收回，真机上会导致会话窗口不可见，已回滚（详见「多会话与窗口」）。
- **滚轮**：仅代码路径保留，未做可视化真机验收。
- **RDP 真机连接**：已在 192.168.50.12 与 192.168.50.18 上验证连接、首帧、鼠标点击与键盘（详见「RDP 接入（2026-10-24）」的验证状态）；192.168.50.18 画面更新滞后十几到几十秒的原因尚未定位。RDP 的剪贴板、音频、证书固定、GFX、IME 等能力按该节已知限制未做。
- **IME / Unicode 直接输入 / X509**：未实现或未验证。
