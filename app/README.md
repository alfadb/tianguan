# 天关：HarmonyOS 远程桌面客户端（VNC / RDP）

这是独立 Stage ArkTS 应用，bundle `cn.alfadb.tianguan`，版本 `1.0.0` / code `100`，API 26，`phone`、`tablet` 与 `2in1` 三种设备。它通过正式 HAP 加载原生内核；当前界面提供连接管理与多会话远程桌面（VNC / RDP）。`module.json5` 当前声明普通权限 `ohos.permission.INTERNET`（网络）与 `ohos.permission.PREPARE_APP_TERMINATE`（托盘退出清理，system_grant），二者都不需要 ACL；受限权限 `ohos.permission.READ_PASTEBOARD` 已于 2026-10-26 随 ACL 审批恢复声明（见「自动剪贴板」）。不申请后台或 VPN 权限。

当前调试设备为测试手机 PLA-AL10（HarmonyOS 7.0.0.107 / API 26 / arm64）；`entry/src/main/module.json5` 的 `deviceTypes` 与 `entry/build-profile.json5` 的 `config.deviceType` 均为 `['phone', 'tablet', '2in1']`（2026-10-27 调整，不含智慧屏）。当前 unsigned HAP 内 `module.json` 与 `pack.info` 均含 `phone`、`tablet`、`2in1`，`requestPermissions` 为 `INTERNET`、`PREPARE_APP_TERMINATE` 与 `READ_PASTEBOARD`。

## 真机验证结果（2026-10-20）

签名包 SHA-256 `02215cfacfcc703685afd795352e58bd0de0014029a707a26ec13fadcf1611f4`，由企业账号调试证书与含两台设备的调试 Profile 签名；`verify-app` 成功，包内 Profile 与外部 `.p7b` 逐字节一致。

- **MateBook Fold（SLM-W32，API 26）**：安装成功（`install bundle successfully`），`aa start` 成功，进程存在；从应用私有目录取回 `diag-result.txt`，三项全部 PASS（RA2_256 工厂、内存 RFB 3.8 Raw 2×1 像素、Thread 协作退出）。证据：`.work/device-runs/pc-diag/diag-result.txt`。
- **手机 PLA-AL10（API 26）**：安装成功；解锁后 `aa start` 成功，进程存在；取回的 `diag-result.txt` 三项同样全部 PASS。证据：`.work/device-runs/phone-diag/diag-result.txt`。

该轮启动页面曾自动运行一次 `import diag from 'libtianguan_diag.so'; diag.runDiagnostics(): Promise<string>`，按钮可以复跑且执行期间禁用。当前版本启动时不再自动运行诊断，诊断与连接均由用户触发。逐项 PASS/FAIL 文本完全来自真实 native 实现，壳不补造成功结果。native 应覆盖 RA2 对象工厂、内存 Raw 像素和 Thread 协作退出。异常显示为错误。版本、时间和结果/错误写入本应用 `filesDir/diag-result.txt`，同时使用 `TianguanDiag` 标签输出日志；保存失败也会在界面与日志显示。没有用户 Documents 或全局文件写入。

## 受控网络测试模式（显式测试用）

连接界面已有实时会话，画面渲染与键鼠转发见下文「输入与显示（2026-10-21）」；下面这一段描述的是显式测试用的一次性网络测试模式，它只验证 TCP 连接、认证、首帧解码与键鼠事件已发送，不渲染画面、不验证服务端收到输入。服务端身份采用显式信任：首次连接只展示指纹、不认证；用户确认后写入本机信任库，之后仅当指纹完全一致才继续，不一致直接拒绝。

- 启动时不会自动发起网络连接或运行诊断；「连接」按钮由用户在界面手动触发，连接期间禁用。
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
- 键盘事件只在页面根组件上处理（当时是 `Remote.ets` 根 Column 的 `onKeyEvent`；2026-11-26 起改为输入法之前的 `onKeyPreIme`，见「键盘」）。按 `event.keyCode` 映射成 X11 keysym，**不能用 `keyText`**：HarmonyOS 的 `keyText` 对字母/数字返回 `"KEYCODE_A"` 这类常量名，当字符用会算出 0 并把按键丢掉。映射规则：字母 `KEYCODE_A..Z`（2017..2042）→ `0x61 + 偏移`、数字 `KEYCODE_0..9`（2000..2009）→ `0x30 + 偏移`、标点查表（空格/减号/等号/方括号/反斜杠/分号/引号/逗号/句点/斜杠/反引号）；特殊键显式映射（Enter `0xff0d`、Backspace `0xff08`、Delete `0xffff`、Tab `0xff09`、Esc `0xff1b`、方向键 `0xff51`–`0xff54`、Home/End/PageUp/PageDown、Shift/Ctrl/Alt `0xffe1`–`0xffea`）。Shift 组合由客户端本地换算上档 keysym（详见「输入与显示（2026-10-21）」）。按下发 `pressed=1`，抬起发 `pressed=0`。
- 键盘到达验证（开发机对 62）：打开应用菜单后发送 Down 键，逐像素比较选中项移动，输出 `PASS keyboard observed: 110 pixels changed after Down key`。

边界（2026-10-20 当时）：输入法（IME）组合输入尚未处理，按物理按键逐个转发；Unicode 直接输入、粘贴、快捷键组合的本地拦截尚未验证。当前行为见「键盘」：按键在输入法之前被拦截并转发，映射不到的键保持本机行为并记日志。

### 隐藏本机光标（2026-10-20）

鼠标进入画面区域时调用 `pointer.setPointerVisible(false)` 隐藏本机光标，离开画面时恢复。该 API 是公开接口（`@ohos.multimodalInput.pointer`，API 9 起，无权限要求），作用范围是当前窗口；导入方式为默认导入 `import pointer from '@ohos.multimodalInput.pointer'`。断开连接时也会恢复可见，避免光标一直消失。

注意：2026-10-21 起改为客户端本地绘制远端光标——声明支持 `supportsLocalCursor` / `supportsCursorPosition`，光标按物理指针位置绘制，移动鼠标不产生画面更新；服务端首次下发的 `0x0` 空光标表示「这次它自己把光标画进画面」，此时客户端隐藏自己的光标图层，发过指针事件后服务端改发真实形状。详见「输入与显示（2026-10-21）」。

### 连接管理与密码保存（2026-10-21）

- **多连接管理**：主界面列出保存的连接（名称、协议与 `host:port`），可「使用」「删除」；表单填写名称/地址/端口/密码后可「保存连接」。列表存在应用沙箱 `filesDir/connections.json`，只含名称、地址、端口、协议、RDP 用户名/域和 `savePassword` 标志，**不含密码**。
- **密码保存**：勾选「保存密码」时，密码写入系统密钥库（`@ohos.security.asset`，`asset.add` / `asset.query` / `asset.remove`，别名 `tianguan:<host>:<port>`，可访问性 `DEVICE_FIRST_UNLOCKED`），不写入 `connections.json`、不写日志、不回显。若系统密钥库不可用，保存会失败并在状态栏提示，此时连接仍可用但密码不落盘。选择已保存的连接且勾选保存密码时，会自动从密钥库取回密码填入输入框。删除连接条目时，若没有其它已保存连接共用同一协议、地址与端口（即同一密钥库别名），才会删除已保存的密码；有共用条目时保留。
- **协议：VNC / RDP（2026-10-24）**：连接可保存为 VNC 或 RDP。RDP 条目额外记录用户名、域与密码：用户名必填、域可留空；密码与 VNC 一样只进系统密钥库，但使用独立别名 `tianguan:rdp:<host>:<port>`，与同地址的 VNC 连接互不覆盖（VNC 继续沿用旧别名 `tianguan:<host>:<port>`，已存密码不受影响）。VNC 默认端口 5901，RDP 默认端口 3389；切换协议时，端口只在为空或仍等于另一协议默认值时才跟随切换，用户手填的自定义端口不会被覆盖。**RDP 已接入原生 FreeRDP 后端**（画面 + 键鼠 + 双向剪贴板（本机→远端需 `READ_PASTEBOARD` 授权）+ 音频（PCM/可解码压缩格式）+ 服务端证书 TOFU 固定）：点「连接」或「重连」会像 VNC 一样创建会话，不再弹「尚未接入」提示；详见「RDP 接入（2026-10-24）」。
- **远程画面页**：连接启动后为该会话启动独立 `RemoteAbility` 实例（`multiton`），页面 `pages/Remote` 承载远程画面、鼠标键盘、光标与剪贴板；同一会话再次「打开」通常只激活已有窗口，手机上窗口已最小化时会用同一 `sessionId` 重开一个实例并保留会话（见「手机形态与旋转（2026-10-25）」的「最小化与打开」）。打开失败直接结束该会话并提示，不再降级到主窗口。详见「多会话与窗口（2026-10-21，2026-10-23 改为多实例）」。
- **连接失败提示（2026-10-24）**：连接失败时主窗口会提示中文原因（密码错误、服务端指纹与已信任值不一致、网络连接中断、协议不匹配或服务器拒绝等），失败行显示「失败：<原因>」。密码错误时关闭该失败会话及其空窗口，并弹出密码重输框；确认后若该连接勾选了「保存密码」，先把新密码写入系统密钥库，再用新密码重新连接（不复用旧会话）。指纹不一致、网络、协议等其它失败只给一条带原因的提示。

冒烟状态（早于多会话改造）：签名包已安装并启动到手机 PLA-AL10（进程存活、无应用错误日志），但连接管理与远程窗口的交互尚未人工点验；PC 当晚关机，尚未部署。

边界：`connections.json` 明文保存地址与名称（不含密码）；窗口尺寸不持久记忆；同一会话同时只有一个远程窗口（再次打开一般只是激活，手机上最小化过的是重开），多个会话各自独立实例。

### 指纹确认与连接界面（2026-10-20）

连接界面（`Index.ets`）提供服务器地址、端口、密码与「连接」按钮；地址默认 `192.168.50.62`，端口默认 `5901`。密码输入框使用 `InputType.Password`，不回显；点「连接」时密码只写入应用沙箱的临时文件 `filesDir/vnc-test.pass` 供 native 读取，连接结束（成功或失败）后由 ArkTS 删除，不写入信任库、结果文件或日志。启动时不会自动运行诊断或发起网络连接。

信任库为 `filesDir/vnc-pins.json`，简单 JSON 对象，键为 `host:port`，值为服务端指纹：

```json
{"192.168.50.62:5901":"84-64-0d-21-6a-ab-fa-a6"}
```

旧版本使用的 `vnc-pins.txt`（每行 `<host>:<port> <指纹>`）不再作为主信任库：当 `vnc-pins.json` 不存在或为空时，界面会把旧文件内容迁移成 JSON 后再使用，此后以 JSON 为准。

删除连接条目时，如果没有其它已保存连接共用同一协议、同一 host 与端口的信任键，会同时清除该地址在信任库里的指纹；还有条目共用时保留。

流程：

1. 点「连接」时读取信任库；有该 `host:port` 的指纹时，`vnc-test.json` 带上 `expectedFingerprint`，没有则不带。
2. 无期望指纹：native 在认证前中止，返回 `NEEDS_CONFIRMATION: <指纹>`；界面展示该指纹与「确认并信任」按钮，此时未认证，不显示为已连接。
3. 点「确认并信任」：把 `host:port → 指纹` 写入 `vnc-pins.json`，立即带该指纹重连；成功后界面提示「已信任该服务器，连接成功（首帧已解码）」。
4. 已信任地址再次连接：指纹一致才认证；不一致返回 `FAIL fingerprint mismatch: got <实际>, expected <期望>`，界面显示「服务端指纹与已信任值不一致，已拒绝连接」，不会自动把新指纹写入信任库，也不显示为已连接。
5. 指纹不一致时，会话窗口与连接卡片都会显示「已信任」与「当前看到」两个指纹，并提供「重新确认指纹」入口：点它只会让该会话忘掉已信任的指纹、以空期望指纹重连，随后回到上面的展示与确认流程；必须再点「确认并信任」才会写入信任库，入口本身从不自动信任（更换服务端或人工复核时使用）。
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

不要手工写入未经核对的指纹；需要重新确认某个地址的指纹时，用界面上的「重新确认指纹」入口（会话窗口的指纹不一致面板或连接卡片）即可，不必删除应用数据或移除信任库文件。真实密码只放在本地，不要写入仓库、文档或聊天。

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

## 自动化单测（`bash scripts/test.sh`）

纯逻辑回归的统一入口，不需要设备、网络、签名或凭据，不修改源码：

```sh
bash scripts/test.sh            # 原生几何 + ArkTS 连接逻辑
bash scripts/test.sh --native   # 只跑原生几何单测
bash scripts/test.sh --arkts    # 只跑 ArkTS 连接逻辑单测
```

当前覆盖两个套件：

- **原生几何**（CTest，1 个用例 / 104 项断言）：`app/native/tests/view_geometry_test.cxx` 只依赖 C++ 标准库和 `app/native/view_geometry.h`。覆盖 zoom=1 时非 0 pan 仍居中、放大后内容超出时 pan 夹在 `[surface-content, 0]`（纵向同理）、内容小于表面时强制居中、zoom 夹取 `[1, 4]` 且 NaN/inf 回退 1、NaN pan 回退 0、旋转（surface 宽高互换）后重算，以及 `renderStats` 的 surface 点 → 远端像素映射公式。脚本在 `.work/test-native-host` 用 `-DTIANGUAN_GEOMETRY_TEST_ONLY=ON` 做 host 配置后构建 `tianguan_geometry_test` 并跑 `ctest`；该开关只跳过需要 TigerVNC 固定源码与预编译依赖前缀的 `experiments/vnc-core` 子目录，默认 OFF，正常 host/OHOS 构建不变。测试目标只在 `app/native/CMakeLists.txt` 的 host 分支注册，OHOS 分支不编译测试。
- **ArkTS 连接逻辑**（node，20 个用例）：`app/tests/arkts/connection_codec.test.mjs` 用 node 直接运行。它把 `app/entry/src/main/ets/model/ConnectionCodec.ets` 逐字节复制到临时目录（不改生产源码），用 HarmonyOS SDK 自带的 TypeScript（`.../ets-loader/node_modules/typescript`；可用 `TIANGUAN_TSC` 指向 tsc，或用 `TIANGUAN_SDK_ROOT` 指向 SDK）编译成 `.mjs` 后 import。覆盖连接记录解析与迁移（旧记录无 id 时 id=`host:port`、已有 id 不重写、缺/未知 protocol 归一为 `vnc`、username/domain 缺省 `''`、group/lastConnectedAt 缺省、数组中混入 null/数字/字符串等非法元素时只跳过坏记录而不丢弃整表）、序列化与往返（F8 字段丢失回归）、`pinKeyFor`（VNC `host:port` / RDP `rdp:host:port`）、`defaultPortFor`（VNC 5901 / RDP 3389）与密码 alias（VNC `tianguan:host:port` / RDP `tianguan:rdp:host:port`，同 host:port 不冲突）。

每个阶段打印 `[PASS]/[FAIL] <name>`，结束打印汇总；任一阶段失败脚本以非零状态退出。`ConnectionStore.ets` 只保留文件/密钥库 I/O，纯函数从 `ConnectionCodec.ets` 导入并按原签名再导出，`Index.ets` / `Remote.ets` 的 import 与连接数据格式不变。

真机冒烟（需要设备在线且已解锁、已装签名包、已保存目标连接凭据；脚本会连接并断开真实会话）：

```sh
bash scripts/smoke-device.sh <serial> --name <连接名>
```

可选 `--hap <签名包路径>` 先安装（只 `hdc install -r`，不卸载）、`--timeout <秒>` 调整首帧等待上限、`--keep` 保留设备 `/data/local/tmp` 临时文件；每步 dump、截图与日志写入 `.work/smoke/<timestamp>/`。脚本不修改 `connections.json`、不读取密钥库、不打印凭据。

未覆盖：真机上的手势与输入回传仍按本文其它章节在设备上人工执行；签名安装与真实连接/断开可用上面的 `scripts/smoke-device.sh` 冒烟；RDP 原生后端（FreeRDP、证书校验）没有 host 单测；`ConnectionStore.ets` 的文件读写与系统密钥库路径仍需真机验证。

## 历史验证记录（2026-10-16，纯诊断轮次）

该轮验证时间为 `2026-10-16T01:23:30Z`（UTC）。已运行 `cmake --build .work/hap-native-host --target tianguan_diag_host -j 2` 并执行 `.work/hap-native-host/tianguan_diag_host`，退出 0，同源真实三项全部 PASS：RA2_256 工厂、RFB 3.8 内存 Raw 2×1、Thread 协作退出。随后 `cmake --build .work/hap-native-ohos --target tianguan_diag -j 2` 与 `bash app/build.sh` 均退出 0，完成更新后的 OHOS shared 与 unsigned HAP。实查库依赖包含 `libc++_shared.so`，已随包携带 SDK runtime；包内诊断库与本轮输入经同一 SDK `llvm-strip` 后字节一致。包内 bundle、版本、API26、`2in1` 与 `phone` 双设备已核对（该历史纯诊断包无权限声明）。未重跑未触及的原 host 9 项或 RA2 socket 集成 3 项。此证据仅覆盖该轮 host 诊断、OHOS 编译链接与本机构建/包内核验；该轮未执行设备诊断。网络测试的设备结果见上文「实测结果（2026-10-20，指纹确认实现前的旧测试模式）」的历史记录。

此前纯诊断未签包 SHA-256：`4515858fe96fef2f85b231824ae815c5f02e56c4915dd64cd846213280f37c85`（`deviceTypes` 增加 `phone` 后重新运行 `bash app/build.sh`，退出 0；包内应用及入口 label 均为 `$string:app_name`，对应源资源值为“天关”，官方资源与 ArkTS 编译通过）。该轮输入共享库 SHA-256：`3032d2c0797d4017d0df7f26073bae693e7e5b6bd74f95840ed14433ae15f8bf`。**未签包不可直接作为可安装交付**；该历史包未签名、未装机。

后续如 native 更新，重新构建并核对包内库。签名前需要以下本机材料路径：

- debug profile `.p7b`：bundle 必须为 `cn.alfadb.tianguan`，并包含已登记的 MateBookFold 调试设备；不能默认使用 NetBird profile。
- 与该 profile 匹配的签名证书及私钥/keystore 的本机路径。密码不要写入聊天、源码或文档。

用户已确认当时本包 debug profile 尚未创建；该调试 Profile 后续已于 2026-10-20 创建并完成签名安装（见上文真机验证结果）。新环境发版时需在 AGC / DevEco 生成本包的 debug profile，并提供匹配证书与私钥的本机路径。可使用同一个开发者证书另发本包 profile，但既有证书是否可复用取决于 AGC 的设备与 profile 限制。字段要求见已核对的[官方 profile 格式文档](https://raw.githubusercontent.com/openharmony/docs/master/zh-cn/application-dev/security/app-provision-structure.md)（`bundle-info.bundle-name`、`debug-info.device-ids`）。

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

- 每个远程会话是一个独立的 `RemoteAbility` 实例（`module.json5` 中 `launchType: "multiton"` 与 `removeMissionAfterTerminate: true`，页面 `pages/Remote`）。每次 `startAbility` 都新建实例、任务和顶层窗口，平台不会自动去重，去重由应用负责。
- 窗口名由系统分配（`tianguanN`，递增且不复用），不能从 `sessionId` 推导。进程内用 AppStorage 维护双向注册表：`remote.window.<sessionId>` = 窗口名，`remote.session.<窗口名>` = `sessionId`；ability 在 `loadContent` 之前写入两条键。
- 打开会话（列表「打开」、托盘点会话、新建会话）统一走 `openRemoteWindow(sessionId, 名称, context)`：先查注册表并 `window.findWindow(窗口名)`；窗口存在时，手机上若处于最小化则先按「重开」处理（结束旧实例、用同一 `sessionId` 启动新实例，失败再回到普通激活，见「手机形态与旋转（2026-10-25）」的「最小化与打开」），其余情况按需 `restore()`（仅 2in1）再 `show()`（主窗口在前台时再 `shiftAppWindowFocus`），返回 `activated`；没有窗口才 `startAbility` 启动新的 `RemoteAbility`，最多轮询约 2 秒等注册表出现并校验窗口可找到，返回 `opened`。同一会话的并发打开共享一次在飞请求，双击或「列表 + 托盘」同时触发不会开出两个实例。
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

- 关闭远程会话窗口（系统标题栏 X）= 断开该会话：`windowStageClose` 放行，页面 `aboutToDisappear` 做清理，`onWindowStageDestroy`/`onDestroy` 删除注册表键，`onDestroy` 调用 `closeSessionById(sessionId)`（手机 / 平板上重开窗口时带着「重开」请求，`onDestroy` 识别后跳过断开、会话保留，见「手机形态与旋转（2026-10-25）」的「最小化与打开」）。只结束该实例，其它会话和主窗口不受影响。程序化关闭（删除连接、彻底退出）先用 AppStorage 标志排除，避免重复结束会话。
- **会话的任务卡片随窗口一起消失**：`RemoteAbility` 配置 `removeMissionAfterTerminate: true`。`terminateSelf()` 默认不会清除任务中心里的 mission（SDK 注释），不配置时关闭会话后多任务里会留下已终止实例的卡片，点回只会提示「会话不可用」。配置后，返回键、气泡径向菜单「断开」、列表菜单「断开」以及全局退出等所有以 `terminateSelf()` 收尾的关闭路径都会把该会话的任务卡片一并清掉；会话仍在（窗口最小化、应用退到后台）时实例没有被终止，卡片照常保留、可切回。主列表 `EntryAbility` 不配置该项：关闭主窗口是 `hideAbility()` 转后台而不是终止，主任务应继续留在多任务里。手机 / 平板最小化后的重开路径先终止旧实例、再按同一 `sessionId` 启动新实例，只依赖会话和窗口注册表，不依赖旧任务记录：旧卡片随旧实例清掉，新实例建立自己的新任务。
- 关闭主窗口 = 转入后台，不退出、不断开会话：拦截主窗口的关闭事件并调用 `hideAbility()`（失败退回 `minimize()`）；托盘未启动时直接 `minimize()`。窗口隐藏后进程、托盘与会话继续运行，托盘或热启动用 `showAbility()` 恢复。
- 托盘菜单的「打开 <会话>」同样走 `openRemoteWindow`，已有窗口就激活；主窗口隐藏时源窗口不在前台，只 `show()` 不再强转焦点。托盘右键菜单送达时系统菜单仍持有焦点，主窗口可能在其后又被抬到前台，因此激活已有窗口会在 250 ms 后再补一次 `show()` + 焦点转移（日志 `remote-window activate` 后又出现 `remote-window reactivate`），保证会话窗口最终在最前。
- **已知限制**：托盘菜单点击会让平台顺带启动一次 `EntryAbility`，其 `onNewWant` 会把主窗口显示出来。主窗口已隐藏时从托盘打开会话，会话窗口会正常出现在最前，但主窗口也会出现在它后面（不遮挡）。曾尝试抑制这次 `onNewWant` 并在 500 ms 后 `hideAbility()` 收回，但真机上会连远程会话窗口一起隐藏、后续 `show()` 也恢复不了（multiton4 回归），因此已回滚；会话可打开性优先。
- 只有「退出」才清理：非阻塞关闭所有会话 → 轮询 `pendingCloses()` 最多 3 秒 → 写 AppStorage `remote.quit = true` → 等待约 400 ms → 移除托盘 → `terminateSelf()`。远程实例每 200 ms 检查该标志并各自 `terminateSelf()`（主 ability 的 `terminateSelf()` 只结束自己），因此不会留下其它 ability 实例。托盘自带的「退出」行由 `onPrepareToTerminate` 接管：首次请求取消并异步清理，5 秒内再次请求直接放行，避免退不掉。

边界：Dock 无法为每个会话显示独立图标（平台按应用合并）；窗口选择器比「直接点 Dock 会话图标」多一次选卡片。注册表是进程内状态，进程重启后自然失效。窗口名由系统分配，重开同一会话会得到新窗口名。关窗即断会话是产品约定，已在真机验证（见文末「验证边界」）。

## 全屏与悬浮气泡径向菜单（2026-11-26）

PC 上的会话窗口可以进入 / 退出全屏，画面里还悬浮一个可拖动的气泡，点开后功能按钮沿弧线展开成径向菜单。两者都只作用于本页所在的那个会话窗口，不涉及其它窗口。

### 全屏（PC）

- 进入时对该窗口调用 `maximize(window.MaximizePresentation.ENTER_IMMERSIVE_DISABLE_TITLE_AND_DOCK_HOVER)`：窗口进入 `WindowStatusType.FULL_SCREEN`，标题栏、Dock 与状态栏隐藏，光标移到顶部热区也不会弹出它们。退出用 `recover()`，由系统还原到进入前的位置与大小（进入前会把当时的 `windowRect` 记进日志，便于比对还原结果）。
- 入口是气泡径向菜单里的「全屏 / 退出全屏」按钮。全屏状态按窗口独立；页面监听该窗口的 `windowStatusChange`，并以窗口上报的状态为准，所以按系统 Esc 或系统手势退出全屏后，按钮文字也会同步回「全屏」。退出前先读一次 `getWindowStatus()`：窗口已不在全屏时只同步页面状态，不调用 `recover()`（对已浮动的窗口调用它会报错 1300001）。
- 手机形态的会话窗口本来就是全屏、没有标题栏，径向菜单里不提供该按钮。

### 悬浮气泡与径向菜单

- **外观与位置**：气泡是直径约 44 vp 的圆形，深色半透明底 + 细描边，中央三条短横线暗示「更多」。平时整体透明度约 0.5，鼠标悬停 / 触摸按下时变实到约 0.9，径向菜单展开期间保持变实。新会话首次布局时，气泡完整可见地放在画面右下角，距右边缘和下边缘各约 24 vp（不是贴边半圆）。位置以相对画面区域的归一化坐标（`bubbleFx` / `bubbleFy` ∈ [0,1]）保存在页面状态里；用户拖动 / 吸附过后才连同贴边状态写进 AppStorage 的 `remote.bubble.<sessionId>`（内容 `fx,fy,edge`）。在用户第一次拖动之前，窗口尺寸变化会重新应用右下角默认位置，而不是按旧尺寸的归一化坐标换算——PC 会话窗口打开后约 400 ms 会先自动调整一次大小 / 位置，这样调整后气泡仍距右、下边约 24 vp。手机 / 平板「最小化 → 打开」重开窗口、页面重建后按保存值恢复（未拖动过则仍用默认右下角）；用户拖动过之后，窗口缩放、进入 / 退出全屏、旋转时按新尺寸换算并钳制在画面内，已贴边的保持贴边。会话真正结束（关窗 / 断开）时删除该键，重开窗口则保留；PC 页面不重建，行为不变。
- **点击与拖动**：轻点气泡（按下到抬起位移小于约 8 vp、未触发拖动）切换径向菜单的展开 / 收起；按住并移动超过阈值则拖动气泡跟随指针，拖动开始时先收起菜单。松手时若气泡中心到最近边缘小于约 24 vp，就吸附到该边缘；拖动与点击都由气泡消费，不会传给远端。PC 上鼠标左键拖动同样有效。手势用互斥的 `TapGesture` / `PanGesture` 组合实现，「轻点不拖动、拖动不触发点击」由互斥识别保证。
- **贴边**：吸附后气泡贴在该边缘上，中心向画面内侧缩进约 10 vp（外圈约 12 vp 被窗口裁掉，露出约 32/44，视觉上是大半个圆），透明度约 0.3。窗口最外侧若干像素是系统的窗口缩放边框，从气泡鼓出部分（距窗口边约 10 px 及更内侧）拖动会由气泡接管，可以再拖出来并重新吸附；不要从系统边框区起手。
- **展开方向**：按钮沿半径约 76 vp 的圆弧分布在气泡四周，按钮为直径约 44 vp 的圆形。扇形中心优先朝向画面中心；若某个按钮圆会越过画面边缘，就收窄扇形并在 0–360° 内旋转，直到所有按钮完整落在画面内（四个边与四个角都覆盖）。画面窄到连最窄扇形都放不下时，按钮位置还会逐点钳制在画面内。
- **按钮与摆放**：PC 与平板为「最小化」「全屏 / 退出全屏」「断开」三个；手机为「最小化」「断开」两个。「断开」用红色底 + 红描边，放在扇形 + 端（与另一端的「最小化」相隔整段弧，是扇形里离它最远的按钮）；菜单只在点击气泡后出现，所有按钮离点击点 76 vp，开启菜单的那一下不可能直接落到按钮上。其它按钮里「最小化」在 − 端、全屏居中。所有动作复用原有实现（`minimizeThisWindow()` / `toggleFullscreen()` / `disconnectThisSession()`）；「断开」走与系统关闭窗口相同的路径（`terminateSelf()` → `onDestroy` → `closeSessionById()`），不另起一套断开逻辑。
- **图标与文字提示**：图标全部自绘，按物理像素统一换算（ArkUI 的 Shape / Path 的 commands 坐标单位是 px，不会随 vp 容器缩放）。绘图区 = `vp2px(16)`（PC 密度约 1.525 时约 24.4 px，手机密度 3.5 时 56 px）；Path 的 commands 坐标 = 16 单位设计坐标 × 像素尺寸 / 16，线宽 = 2 × 像素尺寸 / 16（PC 约 3.05 px，手机 7 px）；绘图区用这个像素尺寸作为宽高，在 44 vp 圆钮内居中。四个图标共用同一套换算：最小化一横、全屏四角外扩、退出全屏四角内收、断开一个叉，外接范围都是 14/16 个绘图区（最小化是一条 14/16 长的横线）；退出全屏的内收四角臂展已从 12/16 扩到 14/16，PC 墨迹约 21 px，与全屏一致。圆钮中心为 (22,22)，16 vp 绘图区的左上角为 (14,14)，图标在绘图区内关于 (8,8) 对称，因此圆心与图标中心重合。没有使用 `SymbolGlyph`：本机 SDK 里 `sys.symbol` 只出现在类型声明中，没有可核对的符号资源清单，无法确认资源名在本机可用。文字提示改为一枚自绘小标签（不再用 `bindTips`，实测它在触屏长按下不出现）：PC 鼠标悬停按钮时显示，触屏长按按钮时显示；长按只显示提示、松手不执行动作（约 800 ms 后标签消失），轻点才执行动作。
- **动画**：菜单展开时按钮从气泡中心沿自己的半径弹出（位移 + 缩放），耗时约 200 ms；收起时反向缩回气泡中心。
- **收起时机**：再次点击气泡；点击 / 触摸画面其它位置；PC 上鼠标离开气泡与菜单区域约 1 秒；执行任一按钮动作后；菜单展开时按 Esc 收起菜单，这次 Esc（按下与抬起）都不转发给远端，菜单未展开时 Esc 仍按原规则转发远端。菜单展开时，画面上第一次按下只用于收起菜单：这一次按下及其后续移动、抬起都不发给远端（触屏与鼠标两条路径都如此），被吞掉的按下 / 移动也不会更新本地绘制的远端光标位置；菜单未展开时画面点击行为不变。若这次抬起事件丢失（窗口重排、远端弹窗等），约 2 秒没有该序列的后续事件后自动取消吞事件；窗口失焦、页面隐藏、窗口重新显示、菜单开合时也会清除吞状态。菜单开合时还会补发一次旧按键释放：新的鼠标按下若发现上一次抬起丢失（本地仍记着按键按下），先释放旧按键再处理本次按下，避免第一次点击被静默丢弃。进入 / 退出全屏后直接收起菜单（布局重排后系统可能不再派发悬停离开事件）。
- **事件隔离与焦点**：气泡和菜单按钮都是画面区（`remote_area`）的兄弟节点，落在它们上的鼠标 / 触摸事件不会进入远端输入；指针进入气泡 / 菜单时，客户端本地绘制的远端光标也会隐藏，不会留下幽灵光标。气泡与菜单按钮均 `.focusable(false)`：页面 / 窗口重新获焦时系统不会在它们上画焦点框，点击也不会把键盘焦点从画面（`remote_area`）或页面根（`page_root`）上抢走。
- **为什么去掉「固定」**：径向菜单点开即用、点别处收起，不需要一个常驻的按钮条；「固定 / 取消固定」以及自动延迟收起的状态随之删除。历史形态（2026-11-25 至 2026-11-26）：顶部中央把手 + 横向浮动操作栏（固定 / 最小化 / 全屏 / 断开），2026-11-26 起被悬浮气泡 + 径向菜单取代。
- **验证状态（2026-11-26，p4 最终轮，PC SLM-W32 display 0 + 手机 PLA-AL10）**：已真机验证。PC：四个图标的墨迹最大外接均为 21 px、中心与按钮圆心偏差 ≤1 px（「退出全屏」由上一版 18×18 扩到 21×21）；3 个循环 × 10 步混合压力序列中应到达远端的点击 27/27 次到达、应被吞掉的「点画面收起」3/3 次未发出远端指针（33 次鼠标按下：6 次 swallow=true 均无远端行，27 次 swallow=false 均恰有 report mask0 / mask1 / release 三行）；最小化 → 主列表「打开」第一次点击即恢复，专测 5/5 + 压力序列 3/3 = 8/8；悬停提示出现 / 消失、菜单收起时键盘输入、断开均正常。手机：气泡距右 / 下边 84 px = 24 vp；最小化图标 48×6、断开 50×50（绘图区均 56×56、中心偏差 0.5 px）；点画面收起、长按「最小化」出现文字提示且松手不执行、轻点后退后台、主列表「打开」恢复后气泡位置保持、菜单展开时点远端 dock 图标只收起菜单、贴边吸附与拖出、断开均正常。证据目录：`.work/bubble-verify/`（索引 `p4-INDEX.md`，不入库）。
- 窗口激活：主列表对已存在的会话窗口点「打开」时，PC 上若窗口最小化，先 `restore()` 再 `show()`，保留浮动窗口的位置与大小（`restoreIfMinimized` 仅 2in1 生效）；手机 / 平板上最小化的窗口在本进程内拉不起来，改为重开窗口并保留会话（见「手机形态与旋转」的「最小化与打开」）。读不到窗口状态时按「未最小化」处理，仍走原来的 `show()` 路径。

## 手机形态与旋转（2026-10-25）

手机（PLA-AL10，`192.168.50.199:37193`）与 PC（SLM-W32）共用同一套界面：PC 的宽屏布局与「关闭窗口即断开」不变，手机按窗口宽度自适应，可用系统返回或气泡径向菜单的「断开」结束会话。

- **窄屏断点**：`Index.ets:49` 的 `NARROW_BREAKPOINT_VP` 为 600 vp，比较的是**本窗口**宽度而不是屏幕宽度（2in1 窗口可以比屏幕窄）：根 Column 的 `onAreaChange`（`Index.ets:1723`）把实际宽度写进 `viewportWidth`，`isNarrow()`（`Index.ets:446`）据此切换。`aboutToAppear` 先按 `display.getDefaultDisplaySync()` 的屏宽/密度给初值，首帧就是正确的布局，之后由 `onAreaChange` 按真实宽度更新。
- **窄屏布局**：小于 600 vp 时，左侧 148 vp 分组栏隐藏，改为顶部一行可横向滚动的分组 chips（`groupChip`，`Index.ets:1291`）；顶栏拆两行（「天关」+「新建连接」一行，搜索框独占一行）；连接卡片改两行（`@Prop narrow`，`Index.ets:102`）：第一行名称、`协议 地址`、时间各占满整行，第二行状态、主按钮、「⋯」（`⋯` 用 36 vp 固定宽度，不再被挤出屏幕）。宽屏仍是原来的左侧分组栏 + 卡片单行布局。
- **手机实测（376×809 vp）**：卡片名称 `62`、`VNC 192.168.50.62:5901`、时间、状态、主按钮与「⋯」全部可见且在屏内；此前被 `layoutWeight(1)` 压成 0 宽的名称/地址恢复显示，「⋯」也不再被裁切。
- **手机结束会话：返回或菜单「断开」**：远程画面本身不叠加标题行、关闭按钮或边距，平时不遮挡画面；画面里悬浮一个可拖动的气泡，点开后是径向菜单（见「全屏与悬浮气泡径向菜单（2026-11-26）」），点其中的「断开」即结束本窗口的会话——此前手机上只能强制停止应用，现在有这条正常出口。PC 仍用窗口标题栏的 X 关闭（关窗即断开）。手机没有标题栏，因此 `Remote` 页在 `onBackPress`（`Remote.ets:3382`）里也主动结束本窗口的会话并关窗：`teardown()` 后调用 `closeSessionById(sessionId)` + `closeRemoteWindow(sessionId)`（`endSessionAndWindow`，`Remote.ets:3405`），与列表「断开」走同一条程序化路径。返回 `true` 表示本页已处理；关闭请求在飞时第二次返回交回平台默认，不会把用户困在页面上。实测按系统返回键或右边缘返回手势后，到 62:5901 的连接从 ESTABLISHED 变 TIME_WAIT，卡片回到「未连接」，不是只退到后台。手机上点「断开」会先把主列表拉到前台再结束本窗口，所以断开后停在主列表而不是桌面；PC 的列表本来就在会话窗口后面，直接结束。
- **手机 / 平板上的「最小化」与「打开」**：手机 / 平板上的会话窗口一旦最小化（包括按 Home 退到后台，实测此时窗口状态同样是最小化），本进程内就拉不起来：在这台 PLA-AL10（OpenHarmony 7.0.0.111 / API 26）上，Window 的 `restore()` / `show()` / `showWindow()` / `maximize()` / `recover()` / `raiseToAppTop()` 与 `UIAbilityContext` 的 `showAbility()` / `startSelf()` 全部不可用（报 801、1300009，或调用成功但窗口仍停在最小化），系统多任务是唯一的原生恢复方式。因此主列表对最小化的会话点「打开」改为**重开窗口**：带着「重开」请求结束旧窗口实例（`onDestroy` 识别该请求、跳过断开，会话保留），随即用同一个 `sessionId` 启动新的会话窗口实例，新页面接入仍然活着的会话，画面与触摸恢复且不重连。实现见 `RemoteWindows.ets` 的 `reopenMinimizedWindow` 与 `openRemoteWindowOnce()` 里「已存在 + 最小化 + 非 2in1」的分流、`RemoteAbility.ets` 的 `onDestroy` 重开分支。重开会把双指缩放 / 平移重置回 contain，会话、剪贴板与信任状态仍保留在会话对象里；重开失败（旧实例未退出、新实例启动失败、新窗口未出现）时回退到普通激活流程，主列表仍显示该会话为已连接，可以再点一次「打开」。
- **列表「断开」**：卡片「⋯」菜单新增「断开」（`Index.ets:167`），仅在该连接有活跃会话时出现；它在列表页、不遮挡画面，作为第二种入口，实测有效。
- **屏幕旋转**：`module.json5:35` / `module.json5:54` 给 `EntryAbility` 与 `RemoteAbility` 都配置 `orientation: 'auto_rotation_restricted'`（2026-11-26 起；此前为 `auto_rotation`）：方向跟随传感器，但受系统「旋转锁定」开关控制——锁定开启时旋转手机页面不转，关闭后随手机旋转。手机手动旋转后实测屏幕由 1316×2832 变为 2832×1316，主界面随窗口宽度自动切到宽屏（左侧分组栏回归、卡片单行），远程窗口的 contain 画面按新 surface 尺寸重铺，无拉伸。手机 / 平板启用分辨率跟随后，旋转停稳约 500 ms 会按新的画面区域物理像素再请求一次（长边 ≤ 1920、保持屏幕比例），竖屏时远端桌面变成竖屏尺寸；旋转过程中的连续尺寸变化由同一防抖合并，只发一次。
- **手机 / 平板的分辨率跟随（2026-11-26）**：与 PC 同一套触发、去重、结算与停用逻辑，只是尺寸规则不同（见「远端分辨率跟随窗口（2026-11-25）」）。竖屏画面区域约为 1316×2597 时请求 972×1920，横屏约为 2832×1180 时请求 1920×800；会话窗口重开后是新页面，首帧后照常请求一次。

## 双指缩放与平移（2026-10-26）

GL 模式下远程画面支持双指手势：两指捏合缩放、两指拖动平移。几何全部由原生渲染器决定，界面只转发请求、并按原生回报的矩形做输入映射。

- **范围与下限**：`zoom` 是相对 contain 适配的倍数，`1.0` 即旧 contain 布局，上限 `4.0`（`kMaxViewZoom`）。下限就是 contain（`1.0`），不能缩得比窗口小；缩放围绕捏合中心，捏合点下的远端像素保持不动。
- **平移**：`panX`/`panY` 是相对居中位置的 surface 像素偏移。某个轴上内容仍不大于 surface 时该轴强制居中、忽略 pan；内容大于 surface 时夹在 `[surface-content, 0]`，拖不出黑边空白。实际生效值每帧按 `eglQuerySurface` 的真实尺寸重算，旋转/改窗口后自动重新夹取。
- **仅 GL 模式**：Canvas 回退路径（`USE_GL_RENDER = false`）没有原生渲染器，手势处理器为空，不提供缩放/平移。
- **原生几何是唯一真源**：`app/native/view_geometry.h`（header-only，`ComputeViewGeometry`/`ClampViewZoom`）同时供渲染循环的 `glViewport` 与 `renderStats()` 使用，界面不再自己按 contain 复算。`renderStats` 新增 `contentLeft/contentTop/contentScale/contentScaleY/contentW/contentH/zoom/panX/panY/maxZoom`（surface 物理像素，左上原点）；`zoom=1, pan=0` 与原 contain 布局逐像素一致（host 几何单测 120 组 framebuffer×surface 组合逐项相同，边界夹取、旋转、NaN 均通过）。输入映射为 `remote=(px-contentLeft)/contentScale`，本地光标也用同一矩形，任何 zoom/pan 下都不漂移；手势坐标从 vp 转 surface 像素的因子取 `renderStats().width / viewWidth`（高度同理）。
- **NAPI**：`setViewTransform(sessionId, zoom, panX, panY)` 设置变换，返回 `OK` 或 `ERROR:NO_RENDERER` / `ERROR:INVALID_ARGUMENTS`；改视图后原生会重绘已上传的纹理一次，静态桌面也能立即看到缩放/平移。契约见 [docs/napi-contract.md](../docs/napi-contract.md)。
- **节流与收尾**：手势更新合并到最多每 33 ms 一次 `setViewTransform`（同步 NAPI），手势结束时立即 flush 一次并回读几何。连接/重连、surface 重新挂载、远端分辨率变化都重置到 contain；窗口尺寸变化重发变换，由原生按新 surface 再夹。
- **按下意图判定（2026-10-26，取代 70 ms 固定延迟）**：单指触摸 Down 不再立即按远端按钮，只发一次 mask-0 的移动，等意图明确后再决定按压方式：位移 ≤3 vp 且快速抬手 → 恰好一次左键点击（抬手时补发 press+release）；位移 >3 vp → 立即发左键 press 转为拖拽；按住 550 ms 仍不动 → 发右键（mask=4）单击，此后到抬手不再发任何指针事件，手机与 PC 上都能弹出远端右键菜单（含 Paste）。窗口内第二指落下则整个手势不发任何 press（此前单指立即 press、第二指到达只补 release，press+release 就是一次点击）。旧的 70 ms 固定延迟已删除：固定时间窗既无法区分「长按要右键」与「慢一点才抬起」，窗口之后才落的第二指仍可能漏出一次点击。
- **鼠标与触摸并存（PC）**：PC 上鼠标与触摸同时可用，不再「见过鼠标就整段禁用触摸」；改为逐事件去重——鼠标事件后 150 ms 内、距最后一次鼠标点 10 vp 以内的**单指**触摸被视为鼠标的合成事件丢弃（否则一次单击会变成两次），双指及以上永不去重。PC 上鼠标单击仍恰好一次（同一次物理动作的 7 条合成触摸全部被丢弃），触摸点击、长按与多指手势同时正常工作。
- **三指及以上屏蔽**：从第三指落下到全部抬起，本页不响应捏合/平移、不发任何远端指针事件，zoom/pan 冻结在进入时的值，抬起后不漂移（三/四指属于系统手势）。屏蔽由两条独立路径把关：`onTouch` 维护手指数，手势回调再用 `event.fingerList` 二次判断，任一条数到 ≥3 指都会触发，因此不依赖触摸事件是否被鼠标去重丢掉。
- **手机实测（PLA-AL10）**：74 段双指手势区间内远端 press/release 为 0；单指点击 press/release 各 26 次 1:1；12 次三指手势前后 zoom/pan 完全一致（如 1.896→1.896）；双击与单指拖动正常；缩放实测 1.0→4.0。

## RDP 接入（2026-10-24）

- **ArkTS → 原生链路**：`connect()` 组装 `SessionOptions` 时按协议分流。RDP 条目带 `protocol: "rdp"`，并在非空时带 `username`/`domain`；密码仍从系统密钥库的 `tianguan:rdp:<host>:<port>` 别名读取（`loadPassword(entry)`）。VNC 条目不传 `protocol`（缺省即 VNC），`security`（None 语义）只在 VNC 分支设置；`expectedFingerprint`（`vnc-pins.json` 信任库）两种协议都会设，键由 `pinKeyFor(entry)` 给出：RDP 用 `rdp:<host>:<port>`，VNC 保持旧键 `<host>:<port>`，因此同地址的 VNC 与 RDP 指纹互不覆盖、VNC 旧指纹继续有效。字段定义见 `app/entry/src/main/ets/diag/SessionApi.ets` 的 `SessionOptions`，契约见 [docs/napi-contract.md](../docs/napi-contract.md)。原来的「RDP 连接尚未接入」拦截（`connect()` 与 `reconnectEntry()` 各一处）已删除。
- **原生后端**：FreeRDP 3.9.0 静态链接（`libfreerdp3` / `libfreerdp-client3` / `libwinpr3`，加静态 OpenSSL 3.5.9 与 zlib），依赖由 `scripts/build-rdp-deps.sh ohos` 用 OHOS SDK 交叉编译并安装到 `.work/rdp-deps/ohos/prefix`（需要仓库内 `.work/freerdp-spike` 的固定 FreeRDP 3.9.0 源码检出）。`app/native/CMakeLists.txt` 的 OHOS 目标始终定义 `TIANGUAN_WITH_RDP=1` 并链接该前缀；前缀缺失时构建直接失败（编译期找不到 FreeRDP 头文件，链接期缺 `freerdp-client3` 等静态库），不会产出静默回退到 RFB 的包。host 目标不编译 RDP 后端，`app/native/diagnostics.cxx` 在未定义该宏时把 RDP 入口编译成 `failed` 阶段加 `protocol` 错误码的兜底。
- **体积**：静态链接把 `libtianguan_diag.so` 从约 1.8 MB（上一版 VNC-only 构建为 1,867,232 B）推到约 15.9 MB（当前 15,931,272 B，含 FFmpeg 7.1.5 的 H.264/AAC 解码），unsigned HAP 约 15.2 MB；这是为在设备上省掉额外共享库依赖付出的代价。
- **复用度**：RDP 画面走 FreeRDP 的 GDI 位图/绘制订单路径，`EndPaint` 后把脏区提交进与 VNC 相同的帧镜像与 `version`/`generation` 规则，因此 Remote 页、原生 EGL 渲染器、多窗口、托盘与会话注册表都不按协议分支。状态机与 `errorCode` 也复用（`none`/`auth-failed`/`fingerprint-mismatch`/`network`/`protocol`，RDP 另有 `session-taken-over`：会话被同账号的其它连接接管，桌面版 Windows 同账号只允许一个活动会话，会话停在 `failed` 且不自动重连，需用户手动重连），主界面的失败原因提示与「密码错误」重输弹层对 RDP 同样生效。
- **剪贴板（2026-10-25，2026-10-26 补齐本机 → 远端）**：RDP 走 `cliprdr` 静态通道（`CHANNEL_CLIPRDR=ON`，仅客户端），只支持 CF_UNICODETEXT 文本，两个方向都可用。远端复制 → 本机系统剪贴板已实测通过（Windows 复制、设备端粘贴成功）；本机复制 → 远端依赖受限权限 `ohos.permission.READ_PASTEBOARD`：ACL 已于 2026-10-26 审批通过，`module.json5` 恢复该声明（`usedScene.abilities` 含 `EntryAbility` 与 `RemoteAbility`、`when: 'inuse'`），签名改用 ACL 版 profile `TianGuanDebugDevicesACLDebug.p7b`（`allowed-acls` 含该权限，设备白名单含手机与 PC 两台）。首次打开远程会话会弹系统授权框，允许后 `TianguanClip perm=1` 并进入事件驱动模式（`on('update')` + 异步 `getData`）；未授权时读取路径完全停用（不注册监听、不启动兜底轮询、不调用 `getData`）。文本上限 1 MiB，CRLF 归一化为 LF，UTF-16LE↔UTF-8 用 WinPR 转换，非法代理对丢弃；`cliprdr` 打开后连接无 addin 加载失败。
- **GFX 图形管线（2026-10-25）**：设置 `SupportGraphicsPipeline` 并订阅通道连接事件，在 `rdpgfx` 通道（DVC 名 `Microsoft::Windows::RDS::Graphics`）连上后调用 `gdi_graphics_pipeline_init`；GFX 激活时 `DeactivateClientDecoding` 改为 FALSE（否则 gdi 会把 `SurfaceCommand` 置空、通道连上也不画），关闭或断开时 detach 并恢复原回调。真机实测 .12：日志出现 `rdpgfx channel connected` 与 `first surface command`（codec=8），60 秒内 `endframes`/`commits` 各 +84、`bitmap` 与 `surfbits` 均为 0，说明画面完全由 GFX 绘制、GDI 快速路径未参与；画面正常、键鼠回归通过。此后依赖侧把 FFmpeg 7.1.5 静态编进同一前缀（`WITH_FFMPEG=ON`/`WITH_SWSCALE=ON`，`libfreerdp3.a` 含 `h264_ffmpeg.c.o`/`dsp_ffmpeg.c.o`），`preConnect` 把 `GfxH264`/`GfxAVC444`/`GfxAVC444v2` 置 TRUE，客户端因此声明 AVC420/AVC444/AVC444v2 能力。真机 .12 在远端 Edge 全屏播放视频时实测：播放前 `last codec=8`（ClearCodec），播放中变为 `last codec=11`（AVC420，`RDPGFX_CODECID_AVC420=0x0B`），`endframes`/`commits` 持续增长而 `bitmap`/`surfbits` 仍为 0，说明服务端按内容在 ClearCodec 与 AVC420 之间切换、客户端 H.264 解码真实生效。静态 ffmpeg 让 `libtianguan_diag.so` 从 12,804,192 B 涨到 15,931,272 B（约 15.9 MB），`NEEDED` 仍只有系统库。
- **音频（2026-10-25）**：rdpsnd 静态通道（客户端侧，运行期依赖 rdpdr 静态通道，drive/printer/smartcard 仍为 OFF）。preConnect 设 AudioPlayback=TRUE、AudioCapture=FALSE，并把 {"rdpsnd","sys:tianguan","channel:2"} 同时加入静态与动态通道集合——Windows 实际使用 AUDIO_PLAYBACK_DVC 动态实例，只配置静态实例会让动态实例回落到内置 fake sink 并静默丢弃音频。参数里不再带 `format:1`：它会把 rdpsnd 的 `fixed_format` 钉成 PCM，协商阶段就把所有压缩格式过滤掉。自定义 addin provider 只拦截 rdpsnd/tianguan，设备插件接受 PCM 16-bit 2ch（8k~192k），以及 `freerdp_dsp_supports_format` 认可、同样 2ch 的可解码压缩格式（本构建的 FFmpeg 有 AAC/AAC-LATM 等解码器；`dsp_ffmpeg.c` 未开 `WITH_DSP_EXPERIMENTAL`，ADPCM/A-law 等仍会被它过滤）。播放用 OHOS OHAudio（OH_AudioStreamBuilder → S16LE 渲染器 + 写回调 + 半秒环形缓冲，格式变化时重建）；压缩数据先经 `freerdp_dsp_context_new`/`freerdp_dsp_context_reset`/`freerdp_dsp_decode` 解成与所选格式同采样率、同声道的 S16LE PCM（码流真实参数不符时由 libswresample 转换），PCM 原样入缓冲，解码失败只计数并限频记日志。真机实测（PCM 轮）：Windows 播放声音、设备端可听见；日志 server offered 30 formats / 1 usable PCM、format selected 44100Hz 2ch 16bit、bytes played 速率 ≈176 KB/s（= 44100×2×2），与 PCM 参数一致。启用 DSP 解码后实测 Windows 在有 PCM 可选时仍选 PCM，AAC/ADPCM 未被协商到（属服务端偏好）。已知限制：缓冲满时丢弃最旧数据（实测累计约 0.6 秒）。
- **服务端证书固定（TOFU，2026-10-25）**：RDP 不再无条件接受服务端证书。FreeRDP 证书回调送来的 SHA-256 指纹（`AB:CD:…`）在原生层统一转成天关现有格式（小写、字节间用 `-` 连接），状态 JSON 的 `fingerprint` 与界面看到的都是这个格式。首次连接（信任库里没有该键）时原生把指纹写入会话状态并置 `state=trust-required`，本次连接被拒绝且不自动重试；主界面卡片显示「需要确认身份」、指纹与「确认并信任」，确认后界面把指纹写入 `vnc-pins.json` 的 `rdp:<host>:<port>` 键并调用 `reconnectSession` 重连。之后的连接由 `connect()` 读回该 pin 并作为 `expectedFingerprint` 传给原生；回调里指纹一致（忽略大小写与分隔符差异）才接受本次会话（返回 2，只接受本次会话，不写 FreeRDP 自己的 known_hosts），不一致则拒绝连接，阶段变为 `failed`、`errorCode=fingerprint-mismatch`，提示「服务端指纹与已信任值不一致，已拒绝连接」。证书变化回调 `VerifyChangedCertificateEx` 走完全相同的判断，不存在「变化回调无条件接受」的漏洞。VNC 的流程与 `vnc-pins.json` 旧键 `<host>:<port>` 保持不变。
- **同一 Windows 账号只允许一个会话窗口（2026-11-26）**：RDP 按「协议 rdp + host + port + username」判定账号，跨连接条目（包括「复制」出的副本）在任意时刻最多只有一个会话窗口，不论该会话处于连接中、已连接、需要确认身份、失败还是重连中；VNC 的多会话行为保持不变。卡片「⋯」菜单对 RDP 连接不再提供「新建会话」；任何会新建会话的路径（「连接」、「保存并连接」、密码错误后的重试等）发现该账号已有会话时都不新建，改为激活已有窗口，处于失败/已断开/重连中时按现有逻辑对已有会话重连，并提示「同一个 Windows 账号只允许一个远程会话，已切换到现有会话」。同一账号的会话被其它连接顶掉时，原生返回 `state=failed`、`errorCode=session-taken-over` 且不再自动重连；主界面提示「该账号的会话已被别的连接接管。在远端注销被接管的会话，或直接点「重连」即可重新连接。」，远端窗口原样显示原生给出的中文原因，卡片主按钮为「重连」（只重连已有会话，不新建）。会话被接管或其它失败终态（指纹不一致除外，它有独立的重新确认面板）时，会话窗口内显示原因与「重新连接」入口，并可直接「返回连接列表」；窗口内的「返回连接列表」与气泡径向菜单「断开」走同一路径（关闭窗口并结束所属会话）。原因是桌面版 Windows 同一账号只允许一个活动会话，第二个连接会把第一个顶掉，两个客户端互相重连会把远端会话折腾到最终不出画面。
- **已知限制**（首版验收范围定为「画面 + 键鼠」）：剪贴板仅支持 CF_UNICODETEXT 文本，两个方向都已可用（本机 → 远端需首次打开会话时授权 `READ_PASTEBOARD`，见上一条）；GFX 已启用（`rdpgfx` 动态通道，`CHANNEL_RDPGFX=ON` 客户端侧；服务端选 ClearCodec/Progressive/RemoteFX 时由 FreeRDP GDI surface 管线解码并复用现有帧镜像；H.264/AVC 已启用：依赖以 FFmpeg 7.1.5 静态解码（H.264/AAC/ADPCM + swscale/swresample），客户端声明 AVC420/AVC444/AVC444v2 能力，服务端按内容在 ClearCodec 与 AVC420 之间切换，真机全屏视频时实测到 `last codec=11`）；RDP GFX 协议只定义 AVC（AVC420/AVC444 系），没有 H.265/HEVC，因此 H.265 不支持也不可协商；`CHANNEL_VIDEO` 仍为 OFF（GFX 的 AVC 解码由 `WITH_VIDEO_FFMPEG` 提供，不需要 VIDEO 动态通道）；`app/native/rdp_backend.cxx` 里的 `kEnableGfx` 常量可回退到改动前的 GDI-only 行为；音频已实现（`rdpsnd`，客户端侧，见下一条）：PCM 与可解码的压缩格式（AAC 等，取决于 FFmpeg 解码器与 FreeRDP dsp 支持）都可接受；实测 Windows 在有 PCM 可选时仍偏好 PCM，AAC/ADPCM 未被协商到（属服务端偏好）；中文输入由远端 Windows 输入法完成（客户端在输入法之前拦截并按物理按键逐个转发，映射覆盖主键区/功能键/小键盘/浏览器与应用启动键，候选窗与上屏在远程画面里；本机输入法不再截获物理键盘）；不在客户端做 IME 转发，也不支持映射表之外的 Unicode 直接输入（映射不到的键保持本机行为并记日志）；证书固定已实现 TOFU（见上一条），但仍不校验证书链与主机名；证书变化后需先在会话窗口或连接卡片点「重新确认指纹」，核对服务端给出的新指纹后再点「确认并信任」，不存在自动重新信任；驱动/打印机/智能卡/摄像头/多显示器等重定向通道均未启用。
- **验证状态（2026-10-24）**：已在真机（MateBook Fold SLM-W32，调试签名、`install -r` 覆盖安装）连接 192.168.50.12 与 192.168.50.18。两台均完成 TCP/TLS/NLA 登录、首帧真实桌面、鼠标点击（远端弹出 Windows 开始菜单、通知中心/日历面板）与键盘（Esc 关闭菜单）。设备侧 /proc/<pid>/net/tcp 可见到两台的 3389 连接为 ESTABLISHED。证据在 .work/device-runs/rdp-btnfix-20261024/（12）与 .work/device-runs/rdp18-final-20261024/（18）。延迟实测（2026-10-25）：稳态下「点击 → 画面出现」为亚秒级（.12 0.6–0.9 s，.18 0.9–1.6 s，两台均 < 2 s），此前「.18 持续滞后十几到几十秒」未复现；新连接会先显示 Windows 锁屏：实测首帧即锁屏，约 3 s 后自动解锁，桌面在 +5.2~6.2 s 就绪；桌面就绪后的首次远端点按约 3.5 s 才出通知中心（客户端 0.6 s 内已执行并画出光标，多出的时间在 Windows 侧），随后恢复到 0.11~0.97 s。此前一次 > 7.9 s 的慢首击未能复现。测量方法与逐帧数据见 .work/device-runs/rdp-latency-20261025/ 与 .work/device-runs/rdp-firstinput-20261025/。剪贴板「远端 → 本机」已由用户实测通过；「本机 → 远端」已在 PC 上端到端实测通过（PC 打开 RDP 12 会话、首次弹授权框并允许，在 PC 上复制文本后到远端 Windows 里 Ctrl+V，内容到达）。手机端也已验证：手机没有物理键盘、无法在远端按 Ctrl+V，改由长按画面弹出的远端右键菜单点「粘贴」完成，内容到达远端。中文输入已由用户实测：在 RDP 会话中使用远端 Windows 输入法可正常上屏。GFX 图形管线已启用并在 .12 上实测接管（bitmap/surfbits 计数为 0）。音频已由用户在真机上听验通过（PCM 44.1kHz 立体声，日志速率与 PCM 参数一致）。H.264/AVC 已由用户真机验证：远端 Edge 全屏播放视频后 hilog 出现 `last codec=11`（AVC420），`endframes`/`commits` 持续增长而 `bitmap`/`surfbits` 为 0（播放前为 codec=8 ClearCodec）。压缩音频的解码路径已就绪（去掉 `format:1`、`freerdp_dsp_decode` 解成 PCM），但实测 Windows 在有 PCM 可选时仍选 PCM，AAC/ADPCM 未被协商到。

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
- 读取权限：本机 → 远端读取系统剪贴板需要 `ohos.permission.READ_PASTEBOARD`（system_basic / user_grant）。该权限已由 ACL 放行（2026-10-26 审批通过，签名 Profile `TianGuanDebugDevicesACLDebug.p7b` 的 `allowed-acls` 含该权限，设备白名单含手机与 PC 两台），`module.json5` 已恢复声明（`usedScene.abilities` 含 `EntryAbility` 与 `RemoteAbility`、`when: 'inuse'`）；声明了该受限权限、而签名 Profile 的 ACL（`acls.allowed-acls`）未允许的 HAP 会在**安装阶段**被系统拒绝（`install failed ... grant request permissions failed`，错误码 9568289），两者必须一起更新。代码侧：`clipboardRequestReadPermission()` 在打开远程会话时调用，先 `checkAccessToken` 判断，未授权则 `requestPermissionsFromUser` 申请一次（每进程最多一次）；首次打开会话会弹系统授权框，允许后 `TianguanClip perm=1` 进入事件驱动读取；未授权时读取路径完全停用（不注册 `update`、不启动兜底轮询、不调用 `getData`），日志可见一次性 `TianguanClip perm=0 ... gate=` 说明。「远端 → 本机」写本机剪贴板不需要该权限，不受影响。
- 后台限制：应用在后台时读取系统剪贴板受系统权限限制，「本机 → 远端」会暂停；回到前台、会话页重新可见后重新注册事件监听并按需补读，不再依赖固定轮询。「远端 → 本机」的写入不受此限制。
- 远端复制的内容只写入本机剪贴板，不会自动转发给其它会话；只有用户在本机真实复制的新文本才会广播给各会话。
- 设备实测（SLM-W32 / API 26 / 调试签名）：事件驱动版（加权限门控之前）日志为 `mode=event` 且 `updateEvents` 递增，确认 `on('update')` 在该 ROM 正常投递；异步读取仍被 ACL 拒绝（`bytes=0`、约 100 ms 的 IPC），但它已离开 UI 线程、且没有事件就不读，用户确认此前的周期性卡顿消失。加入权限门控后的当前版本在未授权时直接 `mode=off`、不注册监听也不读取（本轮最终包实测）；2026-10-26 换 ACL 版 profile 重签后，首次打开会话弹系统授权框，允许后 `TianguanClip perm=1`、回到 `mode=event`。

## 输入与显示（2026-10-21，2026-10-22/23 呈现改为 XComponent + GL）

本节是远程画面与输入链路的当前实现说明，面向使用者与后续接手者。更早的 JPEG 整帧快照、在主窗口内打开远程页与按 `keyText` 映射的写法已不代表现状，上文对应小节已就地订正。

### 画面

- 默认呈现路径：`XComponent(SURFACE)` + 原生 EGL/GLES 渲染线程（`USE_GL_RENDER = true`，每会话一条，`app/native/render.cxx`）。`attachRenderSurface(sessionId, surfaceId, startSuspended)` 把 surface 绑定到该会话的原生渲染器，surface 重建/销毁或页面释放时 `detachRenderSurface(sessionId)`；窗口隐藏时 `suspendRenderSurface(sessionId, true)` 停住渲染线程，恢复可见时置 false 并请求一次全帧。GL 模式下 ArkTS **不调用** `takeFrame`，原生渲染线程是唯一帧消费者，避免两条路径同时分配纹理。
- 原生取帧与上传：渲染线程经 `takeFrameStruct` 读该会话的 BGRA 镜像（与 `takeFrame` 同一把 pix 锁、同一交付基线与 `takenVersion`/`pixEpoch` 语义，但不序列化 JSON），只把自上次成功交付以来的变化矩形拷入 staging。首帧、整帧或分辨率变化时用 `glTexImage2D` 重建整幅纹理，否则 `glTexSubImage2D` 只上传变化矩形；全程一张纹理。BGRA 字节按 `GL_RGBA` 上传，fragment shader 用 `vec4(texel.bgr, 1.0)` 还原颜色。contain 缩放与黑边每帧按 `eglQuerySurface` 的实际尺寸重算；`eglSwapInterval(1)` 跟随 vsync，没有新版本时 4 ms 轮询且不 swap。
- 状态轮询：GL 模式前台每 250 ms 轮询 `sessionStatus(sessionId)`（JSON），驱动状态文本、指纹确认面板、光标形状/版本与剪贴板版本；Canvas 模式保持 30 ms。输入映射与本地光标位置改用 `renderStats()` 的 `texWidth/texHeight`（实际呈现的纹理尺寸），远端分辨率变化时立即对齐，不等状态轮询。页面/窗口不可见时降为 1 s 只读状态，释放按键/按钮并 park 原生渲染器。

- 交付基线（原生取帧共用）：`afterVersion` 必须等于上一次成功交付的版本（原生记录的 `takenVersion`，且 `pixEpoch` 一致），否则返回整帧而不是局部块。基线只在像素 `memcpy` 成功后推进；`NONE` 与 `ERROR:BUFFER_TOO_SMALL` 都不推进。
- 整帧请求：空闲桌面不会主动推全帧；首帧、窗口由隐藏转可见、远端画面尺寸变化或 `generation` 变化后请求一次非增量整屏更新。**本机窗口缩放不请求服务端全帧**：GL 模式下由原生按新 surface 尺寸重算 contain 缩放与黑边；Canvas 模式下 `onAreaChange` 只更新视图尺寸并重绘当前镜像。窗口 / 屏幕尺寸变化会另行请求远端改分辨率（见「远端分辨率跟随窗口（2026-11-25）」）：PC 上按 1920×1200 规则，手机 / 平板上按保持屏幕比例、长边 ≤ 1920 的规则；请求前后的画面都按当时的 surface 尺寸等比缩放。
- 动态分辨率：服务端可中途改变分辨率，客户端也可以主动请求（PC、手机 / 平板在窗口或屏幕尺寸变化后的自动跟随，见「远端分辨率跟随窗口（2026-11-25）」）。GL 模式下原生发现镜像尺寸变化即重建整幅纹理并强制全帧；ArkTS 从 `renderStats()` 同步新的 `texWidth/texHeight`。Canvas 模式下界面丢弃旧 `PixelMap` 与在途渲染，按新尺寸重建帧缓冲并强制全帧。`generation` 变化（重连）同理。
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
- ArkTS 侧用单调时钟测 `takeFrame` 同步段、`writePixels` await、`drawImage` 调用段与轮询间隔，并计数 `applying` 跳过次数和实际绘制帧数；每 2 秒用 `hilog.debug` 打一行 `TianguanPerf ...`，格式为 `poll/take/write/draw=均值/最大值`（毫秒），并附带 `cursor={blank,w,h,visible,left,top}`（服务端光标语义与本地光标图层是否可见）、`ptr={x,y}`（最近一次物理指针的窗口坐标）、`lastMeta={x,y,w,h,full}`（最近一次应用的更新矩形）与 `full=N/M`（本窗口整帧次数/应用帧数），用于区分「远端没发帧」「本地光标没画」这类问题。计时与这些字段不改变渲染行为（没有为测量新增 `await` 或同步点），也不新增 UI 元素。
- 日志级别（2026-10-25）：周期性汇总全部为 **debug**。`TianguanPoll`（ArkTS 轮询分段）、`TianguanRender`（原生每秒 fps/上传耗时及每 5 s 的 `session=...` 行）与 `TianguanClip` 的 2 s 汇总此前已降为 debug；本轮把 `TianguanPerf`（ArkTS 2 s 渲染分段）从 info、原生 `gfx` 的 5 s surface 统计与 `audio` 的 5 s 播放统计从 WARN 也降为 debug。稳态下按默认级别（`hilog -L I,W,E,F`）抓 12 秒为 0 行，正常使用不再周期性刷 info/warn。一次性诊断（如首帧 `TianguanDiag first-frame pf ...`、通道 attached、`gfx: first surface command`）与真正的错误/警告仍保持原级别可见。

### 键盘（2026-11-26：前台窗口拦截全部按键，转发发生在输入法之前）

- 键盘只有一条路径：页面根 Column 的 `onKeyPreIme`（`Remote.ets` 的 `handleKeyPreIme`）。`onKeyPreIme` 在输入法之前触发；会话处于**已连接**状态时，系统导航/电源键、设备控制键、以及映射不到 keysym 的键返回 `false`（交给系统，不 `stopPropagation`、不转发），其余每一个到达本页的按键都返回 `true`（并调用 `stopPropagation()`），声明本页已处理，因此本机不再执行任何默认动作（包括输入法组合，以及能到达应用层的系统快捷键），也不会再触发同组件的 `onKeyEvent`。**不要**再挂 `onKeyEvent` 或第二处键盘处理器：key 事件会冒泡，两条路径会把同一个键发两次、按下抬起顺序错乱（历史上出现过 Shift 组合错乱）。
- 未连接时（会话未解析、连接中、需要确认身份、失败、已断开等）`onKeyPreIme` 返回 `false`，保持原行为，本机输入法与系统按键照常工作。连接与否以状态轮询拿到的 `state=connected` 为准。
- 系统导航 / 电源键不拦截：`KEYCODE_BACK`、`KEYCODE_HOME`、`KEYCODE_POWER`、`KEYCODE_VIRTUAL_MULTITASK` 在 `handleKeyPreIme` 最前面直接返回 `false`，既不 `stopPropagation` 也不转发，也不在 keysym 表里。返回键仍走本页 `onBackPress` 结束会话，主页键、电源键、最近任务键仍由系统处理。
- 设备控制键不映射、不拦截：音量上/下/静音/麦克风静音、亮度加/减/循环/最小/最大/归零/切换、媒体播放/暂停/停止/上一曲/下一曲/快进/快退/弹出/关闭/录制/重复/播放/上一首/随机、音频与低音增强、键盘背光开关/加减、睡眠/唤醒/挂起/屏保/电源、相机等，与系统导航键一样在 `handleKeyPreIme` 最前面返回 `false`，让本机系统照常响应。理由：远端音频是在本机播放的（调本机音量才符合预期）、鸿蒙上这类系统默认响应实测无法可靠抑制、VNC 本身没有音频。浏览器 / 应用启动 / 编辑类按键不属于设备控制，仍照常转发。
- `keyCode` → X11 keysym 是一张模块级查表（`KEYCODE_KEYSYM`），不再用范围 `if`。覆盖：主键区字母/数字/标点（US 布局；`keyText` 对字母/数字返回的是 `"KEYCODE_A"` 这类常量名，不能当字符用）、Enter/退格/Delete/Tab/Esc、方向键/Home/End/PageUp/PageDown/Insert、Shift/Ctrl/Alt/Win（`Super_L`/`Super_R`）、CapsLock/NumLock/ScrollLock、Menu、PrintScreen（`KEYCODE_SYSRQ` → `XK_Print`）、Break、F1–F24、小键盘全部（KP_0–KP_9、KP_Divide/Multiply/Subtract/Add/Decimal/Separator/Enter/Equal 和左右括号；X11 没有单独的 KP_Comma，`KEYCODE_NUMPAD_COMMA` 用 `XK_KP_Separator`）、日/韩文输入模式键，以及浏览器 / 应用启动 / 编辑类按键（WWW/搜索/邮件/计算器、浏览器 Forward/Stop/Refresh/Favorites、Copy/Cut/Paste/Undo/Redo/Find/Help、Open/New/Save/Send/Reply、CD/Video 等）。设备控制键不在表里，见上一条。
- 映射不到的键保持本机行为：`handleKeyPreIme` 返回 `false`，不 `stopPropagation`、不转发，本机系统照常响应；同时记一条限频 `hilog.info`（含 keyCode）。这类键包括 `KEYCODE_FN`、`KEYCODE_FUNCTION` 等纯修饰/硬件键、TV/厂商键，以及只有新版 evdev keysym 的键。
- Shift 上档换算只作用于主键区可打印 ASCII：客户端维护 `shiftDown`，按下时把字符换成上档 keysym（`A`、`!`、`@` 等），并保证同一物理键的按下与抬起使用同一个 keysym；小键盘、导航键和功能键不参与 Shift 换算。
- CapsLock / NumLock 也在本机换算：VNC 把 keysym 当作最终字符，不会再用远端锁定状态变换。字母大小写按「Shift 按下 XOR CapsLock 开启」决定；非字母可打印字符仍只看 Shift。NumLock 关闭时小键盘 `KP_0`–`KP_9` 与 `KP_Decimal` 换算成导航含义（Insert / End / Down / PageDown / Left / Begin / Right / Home / Up / PageUp / Delete），NumLock 开启时保持数字；小键盘的加减乘除与回车不换算。锁定状态以本机镜像为准：CapsLock / NumLock 在一次物理按下的首个 Down 时翻转镜像（初值 false），换算只读镜像。镜像在页面首次使用前、以及每次窗口失焦（`WINDOW_INACTIVE`）或页面隐藏之后标记为待同步；下一个按键事件到达时，若 `KeyEvent.isCapsLockOn` / `isNumLockOn` 有定义，只用它把镜像初始化一次并清除待同步标记，之后仍靠本机翻转——这是因为这两个字段在运行时有定义但不一定随按键变化，不能逐事件采信。`releaseAllInputs()` 只清「仍按住」的边沿，不清镜像；CapsLock / NumLock 按键在 VNC 上照常转发；在 RDP 上两键都被原生静默忽略，远端锁定状态由页面绝对值同步（见下一条）。
- RDP 侧的锁定状态与小键盘：RDP 的 CapsLock / NumLock 状态由远端自己维护，连上时可能与本机相反。页面在本机镜像变化时调用 `syncRemoteLockState(sessionId, capsLock, numLock)`，把本机锁定状态作为**绝对值**推给远端（不依赖连上时远端的状态），并且 CapsLock / NumLock 键本身在 RDP 上不再转发（原生静默忽略）；这样「连上之前本机已开着 CapsLock」时远端也一致，之后每次本机翻转都会重新同步，不会出现两边各翻一次、一直相反。远端锁定指示跟随本机镜像。VNC 上 CapsLock / NumLock 键照常转发（keysym 即最终字符）。小键盘数字 `KP_0`–`KP_9` 与小数点改走主键盘数字/句点扫描码，因此页面判断 NumLock 开时一定出数字（不依赖远端 Windows 的 NumLock 状态），关时页面已换算成导航键。NumLock 关闭时的小键盘 5（Begin/Clear）在 RDP 上不发任何键（见「已知限制」），VNC 上照常发 Begin。
- `KeyType.CANCEL`（API 26）到达时按「所有已按下的键被取消」处理，走 `releaseAllInputs()`：本地状态清零，原生补发所有已记录按键的抬起。
- 可诊断性：`sendKey` 的返回值不是 `QUEUED` 时限频记一条 `hilog.warn`（含 keysym 与返回值）。
- **按键抬起修复（systemKeyCode 配对）**：原生输入队列原先用 `sendKeyPress(0, 0, keysym)` 登记按下，而 `CConnection::sendKeyRelease()` 按 **systemKeyCode** 查 `downKeys[]`，按下记在 `0`、抬起查 keysym 必然 miss，所有抬起被静默丢弃——字母按下即出字符看似正常，但 Ctrl/Shift/Alt 会永远停在按下状态。现在 `drainSessionInput()` 统一用 keysym 作 systemKeyCode 登记（`sendKeyPress(keysym, 0, keysym)`），`sendKeyRelease(keysym)` 与 `releaseSessionInput()` 都能命中同一键；`keyCode` 仍为 0，线上仍是标准 RFB KeyEvent 格式。host key-wire 断言（Shift+a 共 4 条事件、重复 release 无写入）与本地 Xvnc + xev 实测（按下后 300 ms 的 release 送达，修复前要等断连兜底才松键）均已通过；真机会话窗口内键盘输入也已验证。

### 已知限制

- 独立实例窗口中的键盘/鼠标已真机验证（见下节「验证边界」）；滚轮已在 PC 真机实测可用（见「验证边界」）。
- 中文输入（IME 组合输入）不在客户端处理：按键在输入法之前就被本页拦截并转发（系统导航/电源键、设备控制键与映射不到的键除外），本机输入法不再截获物理键盘，中文由远端输入法（如 Windows 输入法）完成；Unicode 直接输入、粘贴与部分快捷键组合仍未逐项验证。
- 音量 / 亮度 / 媒体播放 / 摄像头等设备控制键按设计保持本机行为、不转发到远端：远端音频在本机播放（VNC 没有音频），鸿蒙上这类系统默认响应无法可靠抑制；详见「键盘」。
- 系统级快捷键无法全部拦截：被系统在应用之前消费的键（例如部分 Win/Meta 组合、三指手势、电源/指纹等硬件键）到不了应用的 `onKeyPreIme`，本应用无法转发；能拦到哪些系统组合键以真机实测为准（待补）。系统返回/主页/电源/最近任务键与设备控制键按设计不拦截，保持本机行为（见「键盘」）。
- RDP 后端没有对应扫描码、会被原生丢弃并记日志的键（VNC 上仍可用）：小键盘左右括号、其余未在原生表里的 XF86 键（具体以原生表为准）。NumLock 关闭时的小键盘 5（`XK_Begin` / Clear）在 RDP 上**不发任何键**：没有扫描码能让 Windows 不依赖自身 NumLock 状态读成 Clear；VNC 上照常发 Begin，其余小键盘导航键在 RDP 上均有映射。CapsLock / NumLock 键本身在 RDP 上都被原生忽略，远端锁定状态由页面通过 `syncRemoteLockState` 以绝对值同步（不依赖连上时远端的状态）；VNC 上两键照常转发。RDP 可转发的系统类键为 Print、Pause/Break、KP_Equal、WWW、Search、Mail、Calculator、浏览器 Forward/Stop/Refresh/Favorites；音量/亮度/媒体播放等设备控制键已改为不转发，不在 RDP 讨论范围。
- RDP 锁定状态同步只包含 CapsLock 与 NumLock：ScrollLock 位固定为关闭，因此每次同步（进入会话后的首个按键、按 CapsLock / NumLock 时）都会把远端的 ScrollLock 置为关闭；ScrollLock 键本身仍照常转发，按一下可在下一次同步之前重新打开。VNC 不受影响。
- 本机 → 远端自动剪贴板需要 `READ_PASTEBOARD` 授权（ACL 已放行、声明已恢复，首次打开会话允许后可用；PC 端已端到端实测）。
- 窗口位置/尺寸不持久记忆：新建时按屏幕比例计算并居中，之后用户手动移动或缩放不会被应用覆盖。
- PC 端缩放需触屏双指；鼠标不缩放（未实现 Ctrl+滚轮）。`onMouse` 路径仍即时响应、不做触摸的按下意图判定；触摸路径先判定意图（快速点击=左键、移动=拖拽、按住 550 ms=右键）再发按压。
- 悬浮气泡与径向菜单已于 2026-11-26 的 p4 最终轮在 PC / 手机真机验证通过（设备与数字见「验证边界」的「悬浮气泡与径向菜单（2026-11-26，p4 最终轮）」）。已知边界：窗口最外侧若干像素是系统的窗口缩放边框，贴边气泡请从鼓出部分抓取（实测距窗口边 10 px 及以内侧可靠，7 px 处会被系统接管）；「鼠标按下发现上次抬起丢失」的兜底在本轮验证中 0 次触发、尚未实测；手机进程只输出 W 级日志，D / INFO 级「吞事件 / 远端指针」行未落盘，手机「点画面只收起菜单」以 dumpLayout、截图差分与窗口状态为证；手机横竖屏切换未测（无法自动旋转）。旧横向操作栏的验证记录只适用于「当时的操作栏形态」（2026-11-26 版本：点击顶部中央把手展开，见「已真机验证」的「操作栏点击把手展开（2026-11-26）」），不能作为气泡形态的证据。历史记录中的行为（悬停把手变亮、移开约 1 秒收起、固定常驻、手机按钮为固定 / 最小化 / 断开等）均属于当时的操作栏形态，现已被气泡 + 径向菜单取代。
- RDP 服务端不支持动态分辨率时的自动回退分支未实测（本轮的 Windows 服务端支持动态分辨率，没有可复现「请求被忽略」的服务端）。
- 个别 ROM 若不投递 `windowStatusChange`，按系统方式退出全屏（Esc / 系统手势）后，径向菜单的「全屏 / 退出全屏」按钮文字可能滞后到下一次窗口事件。
- 手机上打开会话窗口时仍有几条「调整位置 / 尺寸失败后放弃居中」的告警（会话窗口本就全屏），无功能影响。
- 分辨率跟随会让远端分辨率随窗口变大，带宽与远端编码开销随之增加，因此 PC 上限为 1920×1200；手机 / 平板上限为长边 1920（短边不小于 480、保持屏幕比例）。
- 只给近似尺寸的服务端会得到近似值：页面按远端实际尺寸等比显示，只为真正的新目标再次请求，不会为凑齐目标值重发。
- 平板没有真机：手机 / 平板的重开路径走同一条非 2in1 代码，但只在手机（PLA-AL10）上验证过。
- 重开的失败分支（旧实例未退出、新实例启动失败、新窗口未出现）只有代码兜底与日志，未人为构造验证。
- 重开会重置双指缩放与平移（回到 contain fit），会话本身与剪贴板、信任状态不受影响。

## 远端分辨率跟随窗口（2026-11-25）

PC、手机与平板都可以让远端桌面跟随会话窗口大小改变分辨率，避免「大窗口看小桌面」靠放大补偿（手机 / 平板自 2026-11-26 起启用；此前只对 PC 启用）。

- **启用条件**：`deviceInfo.deviceType` 为 `2in1`（PC）、`phone` 或 `tablet`；其它类型不请求，保持远端原分辨率等比缩放。
- **目标尺寸**：画面区域（`XComponent` surface）的物理像素，四舍五入后向下取偶，按形态分两种规则：
  - PC（`2in1`）：按比例缩放到宽 ≤ 1920、高 ≤ 1200；目标低于 640×400 时不请求（窗口太小，改分辨率得不偿失）。
  - 手机 / 平板（`phone` / `tablet`）：按比例缩放到长边 ≤ 1920，保持屏幕比例，因此竖屏窗口得到竖屏远端分辨率；短边低于 480 时不请求。取偶后长边不会超过 1920。
- **触发时机**：窗口 / 画面区域尺寸真实变化（拖动窗口、最大化、全屏、手机 / 平板旋转屏幕都走这条路径）、稳定约 500 ms 后；连上并拿到首帧后一次。拖动或旋转过程中不请求，只在停稳后计时。
- **同一目标只发一次请求**：服务端可能以最接近的、它支持的尺寸响应，所以不会因为差几像素而反复请求；远端宽高发生任意变化即视为本次成功（不要求恰好等于目标值）。若请求在途时窗口又变了，结束后按最新目标再看一次。
- **停用**：被拒绝（状态里的 `resizeSupport` 为 `rejected`）、约 3 秒内远端宽高仍无变化、或接口返回 `false`，则对该会话窗口**永久**停用自动跟随（重连也不恢复），画面继续按等比缩放显示；已有会话照常可用，不会因此断开。
- **原生接口**：`requestDesktopSize(sessionId, width, height)`，同步返回 `true`（已受理并交给会话线程发送）或 `false`（会话不存在、未连接或尺寸非法）。VNC 走 ExtendedDesktopSize（`writeSetDesktopSize`），RDP 走 DISPLAY_CONTROL（MS-RDPEDISP，disp 动态通道）；两者共用状态 JSON 的 `resizeSupport` 字段，取值 `unknown` / `ok` / `rejected`。只有 VNC 会给出 `rejected`；RDP 没有拒绝应答，请求被忽略时保持 `unknown`，由页面超时判定。契约见 [docs/napi-contract.md](../docs/napi-contract.md)。
- **构建要求**：RDP 的动态分辨率需要 FreeRDP 依赖编入 disp 通道。`scripts/build-rdp-deps.sh` 已改为打开该通道（`-DCHANNEL_DISP=ON`、client 侧 ON、server 侧 OFF），并在校验段断言 `libfreerdp-client3.a` 含 `disp_main.c.o` / `disp_common.c.o`、`config.h` 定义 `CHANNEL_DISP` 与 `CHANNEL_DISP_CLIENT` 且未打开 `CHANNEL_DISP_SERVER`，防止依赖重建时静默丢掉它。打包必须使用含该接口的原生库，即 `bash app/build.sh <libtianguan_diag.so 路径>`。
- **诊断**：hilog 关键字 `remote resize request`（形态 `form=pc|mobile`、本次画面区域物理像素 `area`、上一次请求时的画面区域物理像素 `prevArea`、发出的目标与请求前远端尺寸，旋转前后可直接对照）、`remote resize settled`（成功与前后尺寸）、`remote resize follow disabled`（停用原因）；另有 `remote fullscreen`、`remote bubble menu` 与 FreeRDP 的 `disp:` 通道日志。

## 验证边界（2026-11-26，p4 最终轮）

设备为 SLM-W32（HarmonyOS 7.0.0.111 / API 26，调试签名，`192.168.50.180:43833`）与手机 PLA-AL10（`192.168.50.199:37193`，屏幕 1316×2832、density 3.5，即 376×809 vp）。以下按“真机验证过”和“未做/受限”分开，避免把构建通过当成功能通过。

### 已真机验证

- **多实例窗口**：每会话独立 `RemoteAbility`（multiton）；同一会话再次「打开」一般只激活已有窗口，手机上窗口最小化过时改为重开实例并保留会话（见 2026-11-25 的「最小化与打开」记录）；关掉一个会话窗口只断该会话并 `terminateSelf()`，其它会话继续出帧。
- **新会话窗口居中/不超屏（2026-10-24）**：在 3000×2000 屏（窗口 `displayId=1`）上依次新建两个会话窗口，`getWindowProperties().windowRect` 与 WMS `getAllWindowLayoutInfo` 均记录为 `[690,460,1620,1080]`，水平/垂直严格居中（误差 0），尺寸在屏幕内、四周余量 690/690 与 460/460 px；第二窗口与第一窗口完全重叠（按需求先严格居中，不做层叠偏移）。
- **Dock 与系统窗口选择器**：Dock 始终只有一个「天关」图标；点击后弹出选择器，主窗口和每个会话各一张卡片，点卡片直接激活对应窗口（含最小化实例）。
- **托盘**：左键打开/激活主窗口；「打开 <会话>」能把会话窗口带到最前（含主窗口在前台的情况）；系统托盘的「退出」能完成会话回收并清理所有实例与窗口。
- **键鼠**：会话窗口内键盘输入可在远端 XFCE 终端执行（`echo` 等）；鼠标点击能获得焦点并把输入送到远端；按键抬起经 systemKeyCode 配对修复后不再卡住修饰键。
- **远端 → 本机剪贴板**：远端终端复制后本机剪贴板被写入（日志 `clipboard 远端->本地 同步 N 字节`，本机输入法出现 `local_data_notify`）。
- **GL 呈现与显存**：真实 62 会话连续 24 分钟以上、3 分钟采样内应用 GPU 24.4–33.5 MB 波动（增长恒为 3 MB，无随帧数线性增长），1 个 GPU context；`RENDER_MEMORY_OVER_ERROR`/`KillProcessByPid` 计数为 0，进程存活；受控 60 fps 内容下 fps 62.4–69.4、swapFail=0；颜色与 Canvas 对照最大差 0.7/255，contain 布局/黑边与 Canvas 对齐；两会话同时渲染正常。
- **无周期卡顿**：事件驱动改造完成后实测 `TianguanClip` 为 `mode=event`、`updateEvents` 递增，用户确认此前的周期性卡顿消失；当前版本在未授权时直接停用读取（`mode=off`，不注册监听），授权后的 `mode=event` 路径已在手机上走通（`TianguanClip perm=1`）。
- **手机形态（2026-10-25，PLA-AL10）**：VNC 连接、返回断开、窄屏/横屏布局与旋转均已在手机上验证：窄屏为 376×809 vp，旋转后屏幕 1316×2832 → 2832×1316；系统返回键与右边缘返回手势都能结束当前会话（到 62:5901 的连接由 ESTABLISHED 变 TIME_WAIT、卡片回「未连接」）；旋转后主界面自动切宽屏、远程画面按 contain 重铺无拉伸。该轮版本远程画面不叠加任何控件（2026-11-26 起画面里另有可拖动的悬浮气泡与径向菜单，见「全屏与悬浮气泡径向菜单（2026-11-26）」；当时的操作栏形态见该节的历史说明）；PC 仍用窗口标题栏关闭，无回归。
- **双指缩放与平移（2026-10-26，PLA-AL10 手机）**：捏合缩放实测 1.0→4.0 连续可用，围绕捏合中心、平移夹边界不出黑边；74 段双指手势区间内远端 press/release 为 0（捏合不再误点击），单指点击 press/release 各 26 次 1:1，三指手势 12 次前后 zoom/pan 完全一致（屏蔽生效、无漂移），双击与单指拖动正常。PC 端已装同版本包，鼠标单击回归见下一条。
- **PC 端鼠标单击回归（2026-10-26，SLM-W32）**：PC 已装与当前 HEAD 同版本的签名包（`.work/tianguan-signed/tianguan-0.0.1-debug-smoke-signed.hap`，sha256 `95e52459…`）；实测鼠标单击仍只生效一次（点任务栏时钟只弹出一次通知中心），窗口标题栏关闭仍断开会话。
- **输入模型（2026-10-26，PLA-AL10 手机 + SLM-W32 PC）**：单指点击每次恰好一次；长按 550 ms 弹出远端右键菜单（含 Paste）；PC 上鼠标单击不重复（同一次物理动作的合成触摸被全部丢弃），触摸点击/长按/多指手势与鼠标同时可用；3/4/5 指实测 zoom 恒定 1.000、零 press/release。PC 触摸单击/长按修复后的自动验证为每次恰好 1×move(0)+press(1)+release，longClick 551 ms → mask=4 且无左键。PC 双指外扩放大已由用户手动确认可用（此前一次日志里 zoom 一直是 1.000，是因为当时手指没有真正张开，不是功能缺陷），捏合缩放/平移与手机一致正常。
- **滚轮（2026-10-21，SLM-W32）**：会话窗口内滚轮可用，经 `onAxisEvent` 取垂直轴值、按 RFB 按钮 4 / 5 发送按下与释放；用户实测滚动正常。
- **手机端剪贴板「本机 → 远端」（PLA-AL10）**：手机没有物理键盘，无法按 Ctrl+V 触发远端粘贴；长按画面弹出远端右键菜单后点「粘贴」，内容到达远端，端到端验证通过。
- **全屏、操作栏（当时的操作栏形态）与分辨率跟随（2026-11-25，PC 双屏）**：华为 MateBook Fold（机身折叠屏，半折状态可见 2472×1608）外接 3000×2000 显示器，双屏。全屏：两块屏进入 / 退出，窗口矩形等于所在屏矩形、逐边误差 0，标题栏 / Dock / 状态栏隐藏。操作栏（当时的操作栏形态：2026-11-25 版本，悬停热区唤出方式）：唤出、收起、固定、最小化、全屏 / 退出全屏、断开、光标隐藏均通过。分辨率跟随：VNC（TigerVNC / Xvnc 类服务端，XFCE 桌面）与 RDP（Windows）在两块屏上首帧、拖动后停住、最大化、全屏、退出全屏全部通过，请求值与按屏幕尺寸手算的预期逐项一致，分辨率变化后指针落点与键盘正常。
- **手机操作栏（2026-11-25，当时的操作栏形态，PLA-AL10）**：顶部把手、操作栏弹出 / 收起、固定、断开后回到主列表且卡片为「未连接」、触摸事件不穿透到远端、全程不发起分辨率请求、双指缩放正常，横竖屏均覆盖。证据目录：`.work/fullscreen-verify/`（不入库）。「全程不发起分辨率请求」是 2026-11-25 及之前版本的行为（当时手机 / 平板不跟随分辨率）；此后手机 / 平板也启用，见「远端分辨率跟随窗口（2026-11-25）」。
- **最小化与「打开」重开（2026-11-25，PLA-AL10）**：最小化 → 主列表「打开」重开成功，恢复后画面与触摸正常；按 Home 退到后台 → 「打开」同样恢复；最小化 → 系统多任务恢复 → 断开 → 回到主列表且卡片为「未连接」；当时手机操作栏按钮为固定 / 最小化 / 断开。证据目录：`.work/fix-minrestore/`（不入库）。
- **最小化回归（2026-11-25，PC）**：最小化 → 「打开」仍走 `restore()`、未触发重开，窗口保持原来的位置与大小；当时的操作栏四个按钮齐全，全屏 / 退出全屏正常。
- **操作栏点击把手展开（2026-11-26，当时的操作栏形态，PC SLM-W32 display 0 + 手机 PLA-AL10）**：进入会话不再自动展示操作栏；PC 顶边非中央位置点击到达远端（远端应用菜单正常打开）、鼠标沿顶边扫过不弹出；悬停把手只变亮变长、不展开；点击把手展开；移开约 1 秒收起、固定常驻；全屏进入 / 退出后不动鼠标，操作栏约 1 秒自动收起、把手重新出现（固定态保持常驻）；断开正常；当时手机上按钮为固定 / 最小化 / 断开，返回键断开正常。该形态已于同日被悬浮气泡 + 径向菜单取代；气泡形态已在 p4 最终轮通过（见下一条）。
- **悬浮气泡与径向菜单（2026-11-26，p4 最终轮，PC SLM-W32 display 0 + 手机 PLA-AL10）**：包 `.work/tianguan-signed/tianguan-1.0.0-debug-bubble4-signed.hap`（sha256 `042df380…`，原生库未变）。PC：四个图标的墨迹最大外接全部 21 px、中心与按钮圆心偏差 ≤1 px（「退出全屏」由上一版 18×18 扩到 21×21）；3 个循环 × 10 步混合压力序列中应到达远端的点击 27/27 次到达、应被吞掉的「点画面收起」3/3 次未发出远端指针（hilog 33 次鼠标按下：6 次 swallow=true 均无远端行，27 次 swallow=false 均恰有 report mask0 / mask1 / release 三行）；最小化 → 主列表「打开」第一次点击即恢复，专测 5/5 + 压力序列 3/3 = 8/8；悬停提示出现 / 消失（像素差分只改提示区）、菜单收起时键盘输入、断开均正常。手机：气泡距右 / 下边 84 px = 24 vp；最小化图标 48×6、断开 50×50（绘图区均 56×56、中心偏差 0.5 px）；点画面收起、长按「最小化」出现文字提示且松手不执行、轻点后退后台、主列表「打开」恢复后气泡位置保持、菜单展开时点远端 dock 图标只收起菜单、贴边吸附与拖出、断开均正常。证据目录：`.work/bubble-verify/`（索引 `p4-INDEX.md`，不入库）。

### 未做 / 受限

- **悬浮气泡与径向菜单（p4 已通过；剩余未测）**：p4 最终轮已通过（见「已真机验证」）。尚未覆盖：窗口最外侧若干像素是系统的窗口缩放边框，距窗口边 7 px 处仍会被系统接管（10 px 及以内侧可靠），贴边请从气泡鼓出部分抓取；「鼠标按下发现上次抬起丢失」的兜底本轮 0 次触发、尚未实测；手机进程只输出 W 级日志，D / INFO 级「吞事件 / 远端指针」行未落盘，手机「点画面只收起菜单」以 dumpLayout、截图差分与窗口状态为证；手机横竖屏切换未测（无法自动旋转）；真实物理鼠标手感未测（仅注入）。
- **PC 冷启动后第一次点「连接」只取到焦点（既有现象，未处理）**：两次观察到冷启动后第一次点主列表「连接」只让主窗口取焦、第二次才真正连接（`p4b-20/21`）；与本轮气泡 / 菜单无关，属已有的主窗口焦点 / 时序现象，未在本轮处理。
- **主窗口隐藏态从托盘打开会话**：会话窗口正常出现且在最先，但平台会顺带拉起主窗口并显示在会话窗口后面（不遮挡，属外观限制）。曾尝试抑制/收回，真机上会导致会话窗口不可见，已回滚（详见「多会话与窗口」）。
- **RDP 真机连接**：已在 192.168.50.12 与 192.168.50.18 上验证连接、首帧、鼠标点击与键盘（详见「RDP 接入（2026-10-24）」的验证状态）；延迟实测（2026-10-25）显示稳态下两台均为亚秒级，未复现持续滞后；新连接会先停在 Windows 锁屏约 3 s、桌面在 +5.2~6.2 s 就绪，桌面就绪后的首次远端交互约 3.5 s 才出画面（客户端 0.6 s 内已发出，延迟在 Windows 侧），一次 > 7.9 s 的慢首击未复现。RDP 的剪贴板两个方向都已可用（本机 → 远端在 PC 与手机端均已端到端实测通过，手机端经长按右键菜单粘贴）；中文输入通过远端 Windows 输入法已可用；RDP 服务端证书已做 TOFU 指纹固定（首次确认写入 `rdp:<host>:<port>`，之后指纹变化拒绝）；GFX 已启用并实测接管；H.264/AVC 已启用并在真机全屏视频时实测协商到 AVC420（`last codec=11`）；H.265/HEVC 在 RDP GFX 协议里没有对应编码，不支持也不可协商；音频（PCM 回放）已完成并听验通过；压缩音频的解码路径已就绪（AAC 等可解码格式），但实测 Windows 仍偏好 PCM，AAC/ADPCM 未被协商到。
- **Unicode 直接输入 / X509**：未实现或未验证。IME 组合输入不在客户端处理：按键在输入法之前被拦截并转发，中文由远端输入法完成（见「键盘」）；被系统在应用之前消费的快捷键与硬件键无法拦截。
