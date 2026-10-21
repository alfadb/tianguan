# 天关：原生内核诊断 HAP

这是独立 Stage ArkTS 应用，bundle `cn.alfadb.tianguan`，版本 `0.0.1` / code `1`，API 26，`2in1` 与 `phone` 双设备。它用于通过正式 HAP 加载原生内核诊断库；当前界面提供内存内核诊断与带指纹确认的受控 VNC 连接测试。仅申请 `ohos.permission.INTERNET`（普通权限，无需 ACL），不申请后台或 VPN 权限。

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

- **多连接管理**：主界面列出保存的连接（名称、`host:port`），可「使用」「删除」；表单填写名称/地址/端口/密码后可「保存连接」。列表存在应用沙箱 `filesDir/connections.json`，只含名称、地址、端口和 `savePassword` 标志，**不含密码**。
- **密码保存**：勾选「保存密码」时，密码写入系统密钥库（`@ohos.security.asset`，`asset.add` / `asset.query` / `asset.remove`，别名 `tianguan:<host>:<port>`，可访问性 `DEVICE_FIRST_UNLOCKED`），不写入 `connections.json`、不写日志、不回显。若系统密钥库不可用，保存会失败并在状态栏提示，此时连接仍可用但密码不落盘。选择已保存的连接且勾选保存密码时，会自动从密钥库取回密码填入输入框。
- **远程画面页**：连接启动后为该会话打开独立子窗口 `remote-<sessionId>`，页面 `pages/Remote` 承载远程画面、鼠标键盘、光标与剪贴板；同一会话再次「打开」只激活已有窗口。子窗口创建失败时降级到主窗口内打开同一页面并提示。详见「多会话与窗口（2026-10-21）」。

冒烟状态（早于多会话改造）：签名包已安装并启动到手机 PLA-AL10（进程存活、无应用错误日志），但连接管理与子窗口的交互尚未人工点验；PC 当晚关机，尚未部署。

边界：`connections.json` 明文保存地址与名称（不含密码）；子窗口尺寸不持久记忆；同一会话同时只有一个远程窗口（再次打开是激活），多个会话各自独立窗口。

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

## 多会话与窗口（2026-10-21）

本节描述 2026-10-21 多会话改造后的行为，接口以 [docs/napi-contract.md](../docs/napi-contract.md) 为准（含 `sessionId`、`pendingCloses`、`takeClipboardOrNull`、`requestFullFrame`、`copyCursor(sessionId, …)`）。

### 原生侧：每个会话独立

- 每个运行期会话有独立 `sessionId`（`createSession` 返回），所有运行期接口都带 `sessionId`。socket、会话线程、输入队列、帧缓冲、光标、`generation` 与剪贴板状态均按会话隔离。
- `generation` 每次重连加一；迟到的帧、迟到的输入和针对旧连接的身份确认都会被拒绝。会话状态以 JSON 返回，界面不解析英文错误串。
- `closeSession(sessionId)` 非阻塞：立即摘除会话、停止读、清输入，线程回收交给后台；`pendingCloses()` 返回尚未回收完的会话数。

### 子窗口：每会话一个

- 每个会话一个独立子窗口，窗口名为 `remote-<sessionId>`，页面为 `pages/Remote`。同一会话再次点「打开」只会 `show()` 并把焦点转回已有窗口，不会创建第二个窗口；窗口关闭后再次打开才新建。
- 创建顺序固定为 `resize` → `show` → `setUIContent` → 设焦点：先按主窗口 `0.92×0.9`（最小 320×240）调整尺寸，`show()` 之后加载页面，再 `setWindowFocusable(true)` 并 `window.shiftAppWindowFocus` 把键盘焦点转到子窗口。子窗口禁用标题栏拖动，避免在远程桌面里拖动时移动整个窗口。
- 创建失败或加载页面失败时销毁半成品窗口；拿不到 `WindowStage` 时没有窗口可建。两种情况都降级到主窗口内打开同一页面，经路由参数传 `sessionId`，界面提示「窗口不可用，已在主窗口打开远程画面」。

### 会话身份：不猜

- 页面只从两处取会话 id：本页所在窗口名 `remote-<sessionId>` 反解，或显式路由参数 `sessionId`（主窗口降级路径）。两处都没有或互相冲突时报错，显示「无法确定本页所属的会话」，**禁用全部输入与断开操作**，只留「返回连接列表」。
- 子窗口路径在 `setUIContent` 之前把 `sessionId` 写入按窗口名派生的 AppStorage 键，页面用它与窗口名交叉校验；不一致同样拒绝挂载。没有 FIFO、最近窗口或「取注册表第一个会话」之类的兜底。

### 关窗与退出

- 关闭远程窗口（页面里的「断开」）= 结束该会话：先结束该会话（`closeSession(sessionId)`），再销毁对应子窗口。
- 关闭主窗口 = 转入后台，不退出、不断开会话：拦截主窗口的关闭事件并调用 `hideAbility()`（失败退回 `minimize()`）；托盘未启动时直接 `minimize()`。窗口隐藏后进程、托盘与会话继续运行，托盘或热启动用 `showAbility()` 恢复。
- 只有「退出」才清理：非阻塞关闭所有会话 → 轮询 `pendingCloses()` 最多 3 秒 → 移除托盘 → `terminateSelf()`；超时仍强退并记录未回收数，可能留下未优雅关闭的会话线程。托盘自带的「退出」行由 `onPrepareToTerminate` 接管：首次请求取消并异步清理，5 秒内再次请求直接放行，避免退不掉。

边界：当前只有页面里的「断开」会结束会话；若从系统标题栏直接关闭子窗口，代码路径只在窗口销毁事件里删除会话身份键，`aboutToDisappear` 并不结束会话，该会话可能变成列表/托盘可接管的孤儿。该路径的实际表现是否与产品约定一致，**需真机验证**。

## 系统托盘（2026-10-21）

- 仅 `2in1`（PC）启用：先按 `deviceInfo.deviceType === '2in1'`、`canIUse('SystemCapability.PCService.StatusBarManager')` 和平台 capability 探测三重判断，再动态加载 `@kit.DeskTopExtensionKit` 的 `statusBarManager`（`@hms.pcService.statusBarManager`，无权限要求）。手机/平板或缺少该能力的系统静默降级，只记一条日志，不加载该 kit。
- 左键点击托盘图标打开/激活主窗口（`showAbility()` 加自身 `startAbility` 热启动，覆盖「已隐藏」和「仅后台」两种情况）。
- 右键菜单：第一组「打开主窗口」，第二组为每个活跃会话一行「打开 <名称>」，最多 18 行；活跃会话超过 18 个时追加「更多会话…」，该行只打开主窗口。已断开或失败的会话不出现在菜单里。
- 系统自带的「退出」行保留，由 `onPrepareToTerminate` 执行完整清理（见上一节）。
- 平台/运行期失败只降级、不影响会话：模块缺失或 capability 不支持时永久关闭托盘功能；图标/能力探测的瞬时失败不会锁死功能；菜单与悬停提示更新失败保留旧值，下次刷新重试；点击处理失败只记日志。菜单更新做 500 ms 合并，避免平台限流。
- 悬停提示为「天关 · N 个会话」，无活跃会话时为「天关 · 未连接」。

## 自动剪贴板（2026-10-21）

- 双向自动同步，仅纯文本；`\r\n` 与 `\r` 统一归一化为 `\n`。每个会话有独立剪贴板版本，来自状态 JSON 的 `clipboard.version`。
- 远端 → 本机：状态里的版本变化时按会话取一次（`takeClipboardOrNull(sessionId)`），把文本写入本机系统剪贴板；同一版本只取一次。
- 本机 → 远端：本机剪贴板出现新的纯文本时，按会话调用 `setClipboard(sessionId, text)`，并记录每个会话已投递的本地版本。只有返回 `QUEUED`/`COALESCED` 才算投递成功；断连或当时未接受的会话在之后的轮询里补投，不要求重新复制。
- 防回环：每次写本机剪贴板**之前**先在 `ClipboardLoopGuard` 记账，剪贴板 `update` 事件与 500 ms 轮询读到这段文本时按「自己写的」跳过，不再回发；本机到远端与远端到本机两个方向共用一个 Promise 串行队列，避免交错。
- 读取失败：`takeClipboardOrNull` 返回 `null` 表示读取失败，**不消费版本**，保留待处理项，下一轮重试；返回空串才表示「确实没有新内容」，可以消费版本。写本机剪贴板失败同样保留内容重试。
- 超过 1 MiB 的文本按 UTF-8 码点边界截断到 1 MiB 以内，并记录截断计数；原生侧对服务端文本同样限长，超出时在状态 JSON 标 `clipboardTruncated: true`。
- 后台限制：应用在后台时读取系统剪贴板受系统权限限制，「本机 → 远端」会暂停；回到前台后下一轮轮询自动续上。「远端 → 本机」的写入不受此限制。
- 远端复制的内容只写入本机剪贴板，不会自动转发给其它会话；只有用户在本机真实复制的新文本才会广播给各会话。

## 输入与显示（2026-10-21）

本节是远程画面与输入链路的当前实现说明，面向使用者与后续接手者。更早的 JPEG 整帧快照、在主窗口内打开远程页与按 `keyText` 映射的写法已不代表现状，上文对应小节已就地订正。

### 画面

- 原生端按会话把「自上次成功交付以来累积的变化」合并成一个包围盒，经 `takeFrame(sessionId, buffer, afterVersion, forceFull)` **在同一把锁里**原子返回「矩形 + 像素」，避免先要矩形再要像素造成的错位与条纹。像素格式固定 `BGRA_8888`，`stride = w * 4`；没有新内容时返回 `NONE`。
- 交付基线：`afterVersion` 必须等于上一次成功交付的版本（原生记录的 `takenVersion`，且 `pixEpoch` 一致），否则原生返回整帧而不是局部块。基线只在像素 `memcpy` 成功后推进；`NONE` 与 `ERROR:BUFFER_TOO_SMALL` 都不推进。界面把 `takeFrame` 返回的 `version` 记为下一轮的 `afterVersion`。
- 整帧请求：空闲桌面不会主动推全帧，Canvas 就绪、窗口由隐藏转可见、画面尺寸变化或 `generation` 变化后，界面调用 `requestFullFrame(sessionId)` 请求一次非增量整屏更新，同时把本地 `forceFull` 置真。
- 动态分辨率：服务端可以中途改变分辨率。状态 JSON 的 `width`/`height` 一变，界面就丢弃旧 `PixelMap` 与在途渲染，按新尺寸重建帧缓冲并强制全帧；`generation` 变化（重连）同理，避免把新画面贴进旧布局。
- 界面维护一份整屏 `PixelMap` 镜像：拿到矩形后用 `writePixels` 只贴变化区，再把整幅镜像 `drawImage` 到 Canvas。分块重绘实验过，实测比整幅 blit 更差，已回退为整幅重绘。同一时刻只允许一个渲染在途，旧代次帧会被丢弃且不推进版本。
- 前台每 30 ms 轮询 `sessionStatus(sessionId)`（JSON）；页面或子窗口不可见时降为 1 s 只读状态，停止取帧并释放按键/按钮。像素字节序是 BGRA 而不是 RGBA，按 RGBA 解释会红蓝互换。

### 光标

- 光标形状按会话取：`copyCursor(sessionId, buffer)` 把该会话当前光标图像以 `RGBA_8888` 拷入 `buffer`；状态 JSON 的 `cursor.version` 不变就不重复取，版本递增时再取一次。
- 客户端声明支持本地光标（`supportsLocalCursor` / `supportsCursorPosition`），光标由客户端按物理指针位置绘制，移动鼠标不产生画面更新，也不需要网络往返。
- 服务端首次会下发 `0x0` 空光标（TigerVNC 的 `emptyCursor`），语义是「这次服务端自己把光标画进了画面」：客户端此时隐藏自己的光标图层；客户端发过指针事件后，服务端改发真实形状，客户端再按热点与视图缩放比绘制。

### 鼠标

- 指针事件按队列发送：界面只入队，由会话线程写 socket（会话线程可能正卡在阻塞读上，直接写会丢掉抬起事件）。纯移动事件合并为最新位置。
- 按键掩码：左键 `1`、中键 `2`、右键 `4`；滚轮用按钮 4/5，掩码 `8`/`16`，经 `onAxisEvent` + `getVerticalAxisValue()` 取值后按「按下 + 抬起」成对发送。

### 键盘

- 键盘事件只在页面根组件上统一处理（`Remote.ets` 根 Column 的 `onKeyEvent`）。**不要**再把 `onKeyEvent` 同时挂在画面容器与根组件上：key 事件会冒泡，两个处理器会让每个键转发两次、按下抬起顺序错乱。
- HarmonyOS 的 `KeyEvent.keyText` 对字母/数字返回的是常量名（如 `"KEYCODE_A"`），不能当字符用；必须按 `event.keyCode` 映射：
  - 字母 `KEYCODE_A..Z`（`2017..2042`）→ `0x61 + 偏移`；
  - 数字 `KEYCODE_0..9`（`2000..2009`）→ `0x30 + 偏移`；
  - 标点查表：空格/减号/等号/方括号/反斜杠/分号/引号/逗号/句点/斜杠/反引号。
- Shift 组合：目标服务端不按修饰键状态处理（单独发 Shift 按下再发 `a`，远端仍是小写），因此由客户端维护 Shift 状态，按下时直接把字符换成上档 keysym（`A`、`!`、`@` 等），并保证同一物理键的按下与抬起使用同一个 keysym。

### 已知限制

- 子窗口里的键盘事件是否真能到达远程会话**需真机验证**：本轮已把焦点转到子窗口（`setWindowFocusable(true)` + `window.shiftAppWindowFocus`），并保留页面根组件的 `onKeyEvent`，但尚无真机证据；上一版子窗口方案曾出现「焦点正常、一个键都收不到」。
- 中文输入法（IME 组合输入）未处理，键盘按物理按键逐个转发；Unicode 直接输入、粘贴与部分快捷键组合未验证。
- 多会话、独立子窗口、系统托盘与自动剪贴板均未完成真机验收；托盘点击/菜单、剪贴板双端互通、关窗与退出的实际表现需在 PC 与手机上逐项验证。
- 子窗口尺寸不持久记忆，每次按主窗口比例重新计算。
