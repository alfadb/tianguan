# 天关 · 提交 AGC（AppGallery Connect）上架操作清单

适用版本：**1.0.0 / versionCode 100** · 包名 `cn.alfadb.tianguan` · APP ID `691761958035659012`
最后核对：2026-10-28（首发范围改为仅 PC/2in1，用户当日拍板）· 维护者：项目维护者（账号、证书、备案相关操作只能由账号持有人执行）

本清单按 AGC 的实际操作顺序排列，每一项都写清「填什么 / 材料在哪个文件 / 没有会卡在哪」。可勾选项写成 `- [ ]`，照着从上往下打勾即可。
凡本清单没写死的页面位置、字数上下限与按钮名称，一律**以 AGC 页面实际为准**。

| 状态图例 | 含义 |
| --- | --- |
| ✅ | 仓库里已有材料，可直接用 |
| ⚠️ | 材料已有但有已知缺陷，提交前必须处理 |
| ⬜ | 需要账号持有人在本机/AGC 侧完成，仓库里没有 |

相关文档：[上架差距清单](../appgallery-readiness.md)、[发布链路与签名验证](../release-pipeline.md)、[上架文案与素材说明](appgallery-copy.md)、[AGC 多设备素材要求](agc-media-requirements.md)、[只发 PC/2in1 的上架要求](pc-only-publishing.md)、[隐私政策正文](../privacy-policy.md)、[最终用户协议](eula.md)

---

## 1. 提交前的前置依赖

这四项是提交前的前置依赖：前三项（发布证书、发布 Profile、APP 备案）缺任何一项，后面的签名、上传或审核都走不通；第 4 项软著在**首发只发 PC/2in1** 时不需要（见 1.5）。它们的材料都要在**代码之外**取得（本地生成密钥 → AGC 网页申请 / 接入商备案系统 / 版权登记机构），无法靠改代码解决。

### 1.1 一览

| # | 依赖项 | 在哪做 | 要多久 | 没有会卡在哪一步 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 1 | **发布证书**（`.p12` + `.csr` → `.cer`） | 本机 DevEco Studio 生成密钥与 CSR → AGC「证书管理」上传 CSR 换 `.cer` | 生成本地几分钟；AGC 签发一般即时（以页面为准） | 无法签名 `.app` → 第 2 节的签名步骤出不了包 → 上传阶段直接被拒（调试证书/调试 Profile 不可上架） | ⬜ |
| 2 | **发布 Profile**（类型「发布」，勾 `READ_PASTEBOARD` 受限 ACL，绑定设备形态） | AGC「Profile → 添加」，类型**发布**，选发布证书，权限里勾「受限 ACL 权限」并勾 `ohos.permission.READ_PASTEBOARD`，提交业务申请 → 审批通过后勾选并下载 `.p7b` | 受限权限业务申请**约 3 个工作日**（官方口径），审批后下载即时 | 包内声明了 `READ_PASTEBOARD` 而 Profile 的 ACL 不许 → 安装失败 `9568289`；无发布 Profile → 包无法上架 | ⬜ |
| 3 | **APP 备案**（工信部 APP 备案） | 在选定的接入商（华为云/阿里云/腾讯云等）APP 备案系统办理，平台**必须勾「鸿蒙」**并填鸿蒙包名 `cn.alfadb.tianguan` | 以接入商与管局审核时长为准，通常数个工作日到数周，属**关键路径** | AGC 版本信息「核准（备案）信息」无法通过校验；未备案不得从事 APP 互联网信息服务 | ⬜ |
| 4 | **软著 / 电子版权证书**（首发不需要） | 中国版权保护中心（或华为认可的电子版权认证渠道）申请，上架时在「版权信息 → 应用版权证书或代理证书」上传 | 常规登记**数十个工作日**，加急另计；决定加**手机**之前再启动 | 只有发布中国大陆且含**手机**时才必选 → 首发只勾 PC/2in1，不卡首发 | ✅ 首发不办（以后加手机再办） |

> 前置的前置：确认**发布主体**（企业账号「湖南新捷智能科技有限公司」）就是最终著作权人/备案主体。账号类型不支持个人↔企业直接转换；名称、包名、主体三者必须全程一致，否则软著/备案会对不上。

### 1.2 发布证书（`.p12` / `.csr` → `.cer`）

- [ ] DevEco Studio → Build → Generate Key and CSR，生成 `.p12`（密钥库）与 `.csr`；记下**密码**与 **key alias**
- [ ] 密钥材料只存在仓外（例如 `/path/to/release.p12`、`/path/to/release.csr`），**不入库**（仓库 `.gitignore` 已忽略 `*.p12/*.cer/*.p7b/*.csr`）
- [ ] AGC → 证书管理 → 新增证书 → 类型「发布证书」→ 上传 `.csr` → 下载 `.cer`（例如 `/path/to/release.cer`）
- [ ] 记录证书名称、有效期（每账号最多 3 个发布证书，有效期 3 年；换证书必须同步换 Profile）

**没有会卡在**：第 2 节签名步骤（`scripts/build-release.sh` 认不出发布材料就只出未签名包）→ AGC 上传的包不是发布证书签名，无法分发。

### 1.3 发布 Profile（含 `READ_PASTEBOARD` ACL）

- [ ] AGC → Profile → 添加 → 选应用/包名 `cn.alfadb.tianguan` → 类型选**「发布」** → 选 1.2 的发布证书
- [ ] 设备形态与 AGC「支持设备」一致：首发只勾 **PC/2in1**；包内 `deviceTypes` 仍为 `['phone','tablet','2in1']`（官方要求包内声明范围 ≥ 勾选范围，见 3.6）。Profile 页面若按包内声明列出全部形态，以页面提示为准
- [ ] 「申请权限」里勾「受限 ACL 权限（HarmonyOS API9 及以上）」→ 勾 `ohos.permission.READ_PASTEBOARD` → 填使用场景与理由（可直接用 4.1 的模板）→ 提交
- [ ] 审批通过后回到 Profile，「已获取权限」里勾上该权限 → 确定 → 下载 `.p7b`（例如 `/path/to/release.p7b`）
- [ ] 核对 Profile 覆盖包内**全部**权限：`INTERNET`、`PREPARE_APP_TERMINATE`、`READ_PASTEBOARD`

**没有会卡在**：带受限权限声明的 HAP 安装阶段报 `grant request permissions failed`（错误码 `9568289`）；审核阶段也可能因「声明了受限权限但 Profile 未包含」驳回。调试 ACL Profile（`TianGuanDebugDevicesACLDebug.p7b`）只能用于本地验证，**不能上架**。

### 1.4 APP 备案

- [ ] 在接入商 APP 备案系统新增备案，平台选「鸿蒙」，包名填 `cn.alfadb.tianguan`
- [ ] 备案的应用名称（天关）、包名、主体须与 AGC 在架信息完全一致
- [ ] 拿到备案号后，在 AGC 版本信息「核准（备案）信息」按要求填写（主体一致可直接校验，不一致需手填主办单位与证件号）
- [ ] 天关是联网客户端（连用户自备的 VNC/RDP 服务器），**不能按「单机应用」豁免**；开发者没有自有服务端时如何填接入信息，需向接入商确认
- [ ] 向华为确认「只发 PC/2in1 的 HarmonyOS 应用是否仍属 APP 备案范围」并保留书面答复——官方**没有** PC 豁免，只有「单机应用」「境外应用」两类例外；确认前按仍需备案准备（详见 [pc-only-publishing.md](pc-only-publishing.md) 第 2 节）

**没有会卡在**：AGC 版本信息的备案校验；未备案影响搜索展示与审核通过。

### 1.5 软著 / 电子版权证书（首发只发 PC/2in1，不需要）

- [x] 首发**不申请软著、不上传版权证书**：官方只对含**手机**的中国大陆应用把「应用版权证书或代理证书」列为必选，PC/2in1 不是手机应用；`80301` 本身也把软著列为「非必选资质」（依据与原话见 [pc-only-publishing.md](pc-only-publishing.md) 第 1 节）
- [ ] 版权声明照写：`Copyright (C) 2026 天关开源项目`，许可 GPL-3.0-or-later；应用名称与图标仍须自有权利、不侵权
- [ ] **以后决定加手机之前**再启动：软件名称须为「天关」或含「天关」的规范全称，著作权人须与开发者主体一致；若著作权人不同，需另附授权书

**没有会卡在**：首发不卡。一旦勾选手机，版本信息「版权信息」页该栏立即变必选，必须先办完软著才能提交。

---

## 2. 本地产出上架包

### 2.1 构建命令

```bash
cd /path/to/tianguan
bash scripts/build-release.sh
```

脚本做的事：`ohpm install` → `hvigorw --no-daemon clean assembleApp --mode project -p product=default -p buildMode=release` → 定位 `.app` → 校验归档里有 `pack.info` 与 `entry-*.hap` → 打印大小与 SHA-256 → （材料齐全时）签名。

前置：HarmonyOS 命令行工具（默认 `$TIANGUAN_SDK`，脚本内有默认路径）、Node/ohpm 由脚本从 SDK 注入。首次构建会下载依赖，耗时较长属正常。

### 2.2 五个签名环境变量

**五个全部给齐**才会签名；缺任意一个只产出未签名包并提示缺哪个；一个都不给就直接出未签名包。变量里只放**路径**，口令只放文件。

| 变量 | 含义 | 占位示例 |
| --- | --- | --- |
| `TIANGUAN_RELEASE_P12` | 发布证书私钥 keystore（仓外） | `/path/to/release.p12` |
| `TIANGUAN_RELEASE_CER` | 发布证书（AGC 下载的 `.cer`） | `/path/to/release.cer` |
| `TIANGUAN_RELEASE_PROFILE` | 发布 Profile（AGC 下载的 `.p7b`，类型「发布」） | `/path/to/release.p7b` |
| `TIANGUAN_RELEASE_P12_PASS_FILE` | 口令文件（单行明文，权限 `0600`，仓外） | `/path/to/release.p12.pass` |
| `TIANGUAN_RELEASE_ALIAS` | keystore 内的 key alias | `tianguan-release` |

```bash
# 口令只经文件传入，不进命令行、不进环境变量、不进仓库
read -rsp 'keystore 口令: ' P && printf '%s' "$P" > /path/to/release.p12.pass && chmod 600 /path/to/release.p12.pass; unset P

export TIANGUAN_RELEASE_P12=/path/to/release.p12
export TIANGUAN_RELEASE_CER=/path/to/release.cer
export TIANGUAN_RELEASE_PROFILE=/path/to/release.p7b
export TIANGUAN_RELEASE_P12_PASS_FILE=/path/to/release.p12.pass
export TIANGUAN_RELEASE_ALIAS=tianguan-release
bash scripts/build-release.sh
```

签名额外依赖两项（脚本会报缺失并中止，不会静默出未签名包）：

- 仓外签名启动器 `HapSignLauncher.java`（默认 `$HOME/harmonyos-signing/HapSignLauncher.java`，可用 `TIANGUAN_HAP_SIGN_LAUNCHER` 指定）——因为官方 CLI 只能用 argv 或交互式口令，无法安全地用口令文件；
- `javac`（JDK）。

### 2.3 产物与校验

| 产物 | 路径 | 说明 |
| --- | --- | --- |
| 未签名包 | `app/build/outputs/default/app-default-unsigned.app` | 2026-10-26 构建实测 **6,803,892 B** |
| 已签名包 | `.work/release/app-default-signed.app` | 仅当 2.2 五个变量齐全时生成；脚本拒绝覆盖已存在的同名输出 |
| 归档清单 | 脚本 stdout | 应含 `pack.info` 与 `entry-*.hap` |
| 大小 / SHA-256 | 脚本 stdout | 上传前记下来，便于与 AGC 侧比对 |

- [ ] 跑 `bash scripts/build-release.sh`，确认末尾打印 `.app` 路径 + 大小 + SHA-256
- [ ] 用 `unzip -l` 看一眼结构（可选）：
  ```bash
  unzip -l app/build/outputs/default/app-default-unsigned.app | grep -E '\.hap$|pack\.info'
  ```
- [ ] 核对包内版本与设备形态，必须与 AGC 填写一致：
  ```bash
  unzip -p .work/release/app-default-signed.app pack.info
  # 期望：version.code=100、version.name=1.0.0、deviceType=["phone","tablet","2in1"]、releaseType="Release"
  ```
- [ ] 验签（签名包）：
  ```bash
  JAR=/path/to/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar
  mkdir -p .work/release/verify
  java -jar "$JAR" verify-app -inFile .work/release/app-default-signed.app \
    -outCertChain .work/release/verify/app-chain.cer \
    -outProfile  .work/release/verify/app-profile.p7b
  ```
  期望输出含 `Verify success` / `Digest verify result: true`。
- [ ] 核对包内 Profile 就是发布 Profile，而不是调试 Profile：
  ```bash
  cmp .work/release/verify/app-profile.p7b /path/to/release.p7b && echo "profile identical"
  # Profile 类型必须是 release，不是 debug：
  grep -ao '"type"[[:space:]]*:[[:space:]]*"[a-z]*"' .work/release/verify/app-profile.p7b
  ```
  期望 `"type":"release"`；若看到 `"type":"debug"`，说明签的是调试 Profile，上传会被拒。

> ⚠️ 当前仓里那份 `.work/release/app-default-signed.app`（2026-10-26 生成）是用**调试** Profile 签的本地验证产物（内嵌 Profile `"type":"debug"`，ACL 含 `READ_PASTEBOARD`），**不能上架**。发布时必须用 1.3 的发布 Profile 重新签出，并按上面的命令确认类型为 release。

**必须记住的约束**：`.app`（App Pack）**不能本地安装**，`verify-app` 通过 + 包内 `pack.info` 是 Release + 包内 Profile 与发布 `.p7b` 一致，就是本机能做到的全部验证。发布 Profile 签的包也不能用 `hdc install` / `bm` 装到本机（错误码 `9568402`）；真正的安装验证走 AGC 内部测试/邀请测试或商店安装，**不要**把「hdc 装不上」当成包坏了。

---

## 3. AGC 逐字段填写对照表

### 3.1 主表

| AGC 字段 | 填什么 | 来源 / 文件 |
| --- | --- | --- |
| **应用名称** | `天关`（2 个汉字，创建时已填） | 包内 `label`（`app/entry/src/main/resources/base/element/string.json` 的 `app_name`）。提交前确认与商店展示名一致；以后办软著时名称/简称须与此一致 |
| **应用图标** | 优先 1024×1024 档 `icon-1024.png`；页面只要 216 档时用 `icon-216.png`（两档二选一，以页面提示为准） | ✅ `docs/store/icon-1024.png`（1024×1024 RGB）、`docs/store/icon-216.png`（216×216 RGB），说明见 [`docs/store/README.md`](README.md) |
| **应用简介（一句话）** | 「开源的 VNC / RDP 远程桌面客户端，用鸿蒙电脑或手机直连你自己的远程主机，多会话、高清画质、声音与剪贴板俱全；名取自《晋书》星官。」（69 字符 / 50 汉字） | ✅ [`appgallery-copy.md`](appgallery-copy.md) 第 1 节（另有一条备选文案） |
| **应用介绍（详细介绍）** | 第 2 节整段（约 473 汉字）：双协议、多会话、GFX/H.264、音频、剪贴板、TOFU 指纹、手机/PC 适配、**适用范围（需自备服务器）**、GPL-3.0-or-later + 仓库地址、support@alfadb.cn | ✅ [`appgallery-copy.md`](appgallery-copy.md) 第 2 节 |
| **截图（2in1 / PC）** | 5 张，1920×1080，顺序见 3.2：`pc-01-main` → `pc-02-rdp` → `pc-03-vnc` → `pc-05-form` → `pc-04-about` | ⚠️ `docs/store/screenshots/pc-*.png`；其中 `pc-01-main`（顶部对齐前的布局）与 `pc-04-about`（版本号仍显示 **0.0.1**）须用 1.0.0 重拍（见 3.3） |
| **截图（手机）** | 首发不需要；以后加手机时再补 3–10 张 1080×1920 或 1920×1080 | 现有 `phone-*.png` 保留在仓库作参考，**不作为首发上架素材**（见 3.2） |
| **截图（平板）** | 首发不需要；以后加平板时再补 3 张 1280×1920 或 1920×1280 | 当前无平板真机，后续加设备再拍（见 3.6） |
| **最终用户协议（EULA）** | 填 [`eula.md`](eula.md) 的正文，或托管为链接（GPL-3.0-or-later 兼容，含第三方组件与源码获取方式，生效日期 2026-10-28） | ✅ [`eula.md`](eula.md) |
| **隐私政策 URL** | `https://tianguan.alfadb.cn/privacy-policy.html`（已 200） | ✅ 正文同源：[`docs/privacy-policy.md`](../privacy-policy.md)；应用内入口为「关于」面板 |
| **开发者联系方式** | `support@alfadb.cn` | ✅ 已写入隐私政策、关于面板与上架文案；AGC 侧按页面要求另填主体名称、客服邮箱/电话（有则填） |
| **应用分类与标签** | 一级「应用」（非游戏）；二级建议「商务办公」，被指类目不符再改「工具」；标签选 5–8 个：远程桌面、VNC、RDP、远程办公、多会话、开源、服务器运维、HarmonyOS PC | ✅ [`appgallery-copy.md`](appgallery-copy.md) 第 5 节（含三个候选类目的取舍）。注意二级分类只能选一个，设置后不能自行修改 |
| **内容分级问卷** | 按 3.4 的答案建议逐条如实勾选 | ✅ 应用事实见 [`appgallery-copy.md`](appgallery-copy.md)、[`../privacy-policy.md`](../privacy-policy.md) |
| **支持设备** | 首发**只勾 PC/2in1**，不勾手机、平板、智慧屏；包内 `deviceTypes` 保持 `['phone','tablet','2in1']`（包内范围 ≥ 勾选范围），见 3.6 | ✅ `app/entry/src/main/module.json5:8`、包内 `pack.info` 的 `deviceType`；AGC 页面提交前复核实际勾选 |
| **版权信息** | 首发**不上传版权证书**（PC/2in1 非手机应用，非必选）；只填著作权人与版权声明 `Copyright (C) 2026 天关开源项目`、许可 GPL-3.0-or-later | [pc-only-publishing.md](pc-only-publishing.md) 第 1 节；许可与仓库信息见 [`../../LICENSE`](../../LICENSE) 与 [`../licensing.md`](../licensing.md) |
| **演示视频** | 60–90 秒真实录屏，横屏 16:9 为主，另出 9:16 竖屏；8 个分镜按脚本拍 | ⬜ 尚未拍摄；脚本 ✅ [`appgallery-copy.md`](appgallery-copy.md) 第 4 节 |
| **版本更新说明** | 用 3.5 的摘录（首版说明，约 200 字） | ✅ [`../../CHANGELOG.md`](../../CHANGELOG.md) `[1.0.0]` 条目 |

### 3.2 截图分组与顺序

AGC 的截图按设备形态分组上传，每组的张数上下限、是否必须给横屏/竖屏，**以页面提示为准**。首发只勾 PC/2in1，因此**只提交 2in1 / PC 一组**；手机与平板分组是以后加设备时才需要，手机 3 张保留在仓库作参考：

| 分组 | 张数 | 建议顺序 | 文件 | 尺寸 |
| --- | --- | --- | --- | --- |
| 2in1 / PC（首发唯一一组） | 5 | 首图=主界面（宽屏双栏，最能说明产品）→ RDP 会话 → VNC 会话 → 新建连接表单 → 关于面板 | `pc-01-main.png`、`pc-02-rdp.png`、`pc-03-vnc.png`、`pc-05-form.png`、`pc-04-about.png` | 1920×1080 |
| 手机（后续扩展） | — | 首发不提交 | `phone-01-main.png`、`phone-02-session.png`、`phone-03-about.png` | 1080×1920 |
| 平板（后续扩展） | — | 首发不提交 | 待拍：`tablet-01-main.png` 等 | 1280×1920 或 1920×1280 |

顺序理由：首图决定列表页观感，必须一眼看出「这是远程桌面客户端」；会话图证明功能真实可用；表单图证明能自己配置；关于图提供版本、许可与联系方式（审核指南要求应用内有真实有效的联系信息）。

- [ ] 首发只上传 2in1 / PC 组 5 张；AGC 页面只应出现 PC/2in1 页签（若出现其它页签，以页面提示为准）
- [ ] 每张图的 IP、用户名、主机名已打码为演示值（打码对照见 [`appgallery-copy.md`](appgallery-copy.md) 附录 A）
- [ ] 截图内容与应用实际界面一致，不改文案、不加演示账号暗示

### 3.3 ⚠️ 提交前必须处理的素材缺陷（只涉 PC 两张）

`pc-04-about.png` 的版本号显示为 **0.0.1**（已目视复核），而源码与 AGC 都是 1.0.0 / 100；`pc-01-main.png` 是主界面布局改为顶部对齐之前拍的。两张都要用 1.0.0 重拍。这类不一致属「提交信息与安装后不一致」的典型驳回点。

- [ ] 用 1.0.0（versionCode 100）的包在目标 PC 上覆盖安装（`hdc install -r`，调试 ACL 签名包即可，不必等发布证书）
- [ ] 重拍 `pc-01-main.png`（顶部对齐后的主界面）与 `pc-04-about.png`（版本号 1.0.0），保持 1920×1080 与原有打码方式
- [ ] 逐张确认版本号显示 `1.0.0`、界面与安装后一致，再上传

其余 3 张 PC 截图（`pc-02-rdp`、`pc-03-vnc`、`pc-05-form`）不受影响。手机 3 张不再作为上架必需，其中 `phone-03-about` 的 0.0.1 问题**不用处理**；截图保留在仓库，以后加手机时再重拍。

### 3.4 内容分级问卷答案建议

问卷选项名称以页面实际为准，答案须与应用真实功能一致（虚假填写会被驳回）：

| 问卷维度 | 建议答案 | 依据 |
| --- | --- | --- |
| 暴力 / 血腥 / 恐怖 | 无 | 应用只做远程画面投射，不生产内容 |
| 色情 / 裸露 / 性暗示 | 无 | 同上 |
| 赌博 / 真实货币交易 | 无 | 无内购、无支付、无广告 |
| 烟酒 / 毒品 / 药品 | 无 | — |
| 脏话 / 粗俗语言 | 无 | 应用内自带文案不含 |
| 用户互动 / 社交 / 用户生成内容 | 无 | 无账号、无评论、无分享、无 UGC 服务器 |
| 位置共享 / 收集位置 | 无 | 不申请位置权限 |
| 收集个人信息 | 无 | 见 [`../privacy-policy.md`](../privacy-policy.md) 第二节 |

预期结果为最低龄档（如「全年龄 / 3+」，以问卷算出的结果为准）。

### 3.5 版本更新说明（可直接粘贴）

```text
首个公开版本 1.0.0。
新增：VNC / RDP 双协议远程桌面；多会话多窗口与系统托盘；RDP GFX 与 H.264 解码、远端音频；文本剪贴板双向同步（本机→远端需剪贴板授权）；首次连接指纹确认与变化拒连；手机双指缩放、单指长按右键；隐私政策与首启同意流程。
修复：诊断日志分级，默认日志级别下不再刷屏。
说明：本应用为客户端，需自备 VNC / RDP 服务器；GPL-3.0-or-later 开源，源码见 https://github.com/alfadb/tianguan 。
```

### 3.6 支持设备：首发只勾 PC/2in1

| 事实 | 证据 |
| --- | --- |
| AGC 应用信息里支持设备**首发只勾 PC/2in1**，不勾手机、平板、智慧屏 | 2026-10-28 用户拍板（依据见 [pc-only-publishing.md](pc-only-publishing.md)）；提交前请在 AGC 页面上复核当前实际勾选状态 |
| 包内声明三种形态 | `app/entry/src/main/module.json5:8` → `deviceTypes: ['phone', 'tablet', '2in1']`；`app/entry/build-profile.json5` 的 `config.deviceType` 同为三项；包内 `pack.info` / `module.json` 的 `deviceType` 亦为三项（2026-10-27 构建核对） |

官方《配置支持设备》要求包内声明范围 ≥ AGC 勾选范围；三项 ≥ 一项，满足。**不用改包、不用重新构建**：分发范围由 AGC 勾选决定，不由包内声明决定，手机/平板用户不会因此拿到应用；保留三项便于以后加设备与侧载调试。官方还规定「应用一旦发布，升级版本只能增加设备、无法删除」，所以首发形态发布后不能再收窄。

- [ ] AGC「支持设备」只勾 **PC/2in1**，不勾手机、平板、智慧屏
- [ ] 上传软件包后确认 AGC 解析出的设备形态含 `phone` + `tablet` + `2in1`（三项都在包内，属正常，不是缺口），而勾选范围只有 PC/2in1
- [ ] 提交前复核勾选范围：以后只能加设备、不能删设备

---

## 4. 权限与合规问答准备

审核可能直接问，也可能在「受限权限业务申请」「隐私说明」框里让你写。下面每段都可以**整段复制**。

### 4.1 为什么需要 `READ_PASTEBOARD`（申请理由，可直接粘贴）

```text
天关是一款 VNC / RDP 远程桌面客户端，用户在本机复制文本后需要在远程会话中粘贴（例如把口令、命令、代码片段送到远端主机），这是远程桌面类工具的核心操作之一。为实现「本机复制 → 远端粘贴」，应用需要读取本机剪贴板中的纯文本，通过已建立的远程会话转发给用户自己指定的服务器。

使用范围严格受限：
1) 仅在远程会话窗口处于前台、且用户首次打开会话时系统弹窗授权之后读取；未授权时应用完全不调用剪贴板读取接口（已做门控，日志可证）。
2) 只读取纯文本，不读取图片、文件或富文本；不落盘、不上传开发者服务器或任何第三方，只发往用户当前连接的远端主机。
3) 用户可在「设置 > 隐私与安全」随时关闭该权限，关闭后远程画面、鼠标键盘均不受影响，仅失去自动同步。
4) 应用无账号、无广告、无统计，不收集任何个人信息。

随附材料：权限使用场景录屏（本机复制→远端粘贴全过程、授权弹窗、拒绝授权后的降级表现）。
```

**首发只发 PC/2in1**：该受限权限在 PC/2in1 上「应用均可申请」、首次申请系统默认授予「允许」，不存在手机侧「场景不符就驳回」的门槛；但发布 Profile 的 ACL、「应用隐私说明」的使用理由与使用场景视频仍要交（业务申请约 3 个工作日）。手机侧场景风险首发不涉及；若以后加手机，按第 6 节的预案处理。

### 4.2 常见问题与建议答复

| 审核可能问 | 建议答复（可直接粘贴） |
| --- | --- |
| 为什么需要读取剪贴板？ | 见 4.1。核心诉求是「本机复制 → 远端粘贴」，只在会话前台、用户授权后读取纯文本，不落盘不上传；未授权时完全不读。 |
| 是否收集个人信息？ | 不收集。应用没有账号体系，不采集设备信息、位置、通讯录、相册、麦克风、摄像头，不含统计/广告/崩溃上报 SDK，不向开发者或任何第三方发送数据。连接配置、密码（可选保存，存系统密钥库）、证书指纹只保存在本机应用沙箱。 |
| 是否包含第三方 SDK？ | 不含广告、统计、推送、社交或分析类 SDK。仅为协议实现静态链接开源库：TigerVNC（GPL-2.0-or-later）、FreeRDP（Apache-2.0）、FFmpeg（LGPL/GPL 依配置）等，源码与许可清单见仓库。 |
| 是否需要注册/登录账号？ | 不需要。应用无账号体系，安装后即可使用。 |
| 适用范围是什么？ | 面向需要访问远程主机的用户：连接**用户自备**的 VNC / RDP 服务器（局域网可达或已做端口映射/VPN 的公网主机）。应用是客户端，不提供服务器、不提供演示账号或试用主机。 |
| 是否提供服务器 / 是否中转数据？ | 不提供、不中转。连接由用户设备直连用户指定地址，开发者没有中转或托管服务器，也看不到连接内容。 |
| 收集的剪贴板内容是否上传？ | 不上传开发者或第三方，只发往用户当前连接的远端主机；不落盘、不做统计。 |
| 联网与 IPv6？ | 应用通过系统网络能力与用户指定的主机建立 TCP/TLS 连接，不经过开发者服务器；IPv6 目标地址未单独实测，若审核要求可补测后答复。 |
| 开源许可与商店分发是否冲突？ | 应用以 GPL-3.0-or-later 发布，源码公开在 https://github.com/alfadb/tianguan ，用户可获取完整对应源码，符合 GPL 义务；商店分发属版权人授予的非独占分发授权，不冲突。 |
| 应用功能需要演示视频吗？ | 需要：应用依赖用户自备的远程主机，审核侧无现成环境，因此提供 60–90 秒真实录屏，覆盖连接建立、会话操作、剪贴板授权弹窗、拒绝授权后的降级表现。 |

### 4.3 提交前自问自答

- [ ] 应用内能找到真实有效的联系方式与应用名（「关于」面板已有版本、包名、support@alfadb.cn、许可与仓库地址）
- [ ] 隐私政策 URL 与应用内展示内容**完全一致**（同一份正文：`docs/privacy-policy.md`）
- [ ] 隐私政策页面是静态 HTML、中文、无需登录/点击/跳转即可阅读
- [ ] 权限清单与用途与实际声明一致（`INTERNET`、`PREPARE_APP_TERMINATE`、`READ_PASTEBOARD`）
- [ ] 受限权限的场景说明 + 录屏已备好（4.1）
- [ ] 演示视频已拍（自备服务器类应用的必备材料）

---

## 5. 提交与审核

### 5.1 提交动作清单

- [ ] 软件包管理上传 2.3 的 `.app`（签名包），等待合法性检测
- [ ] 合法性检测「已达标」，页面能解析出版本名/版本号/设备形态（`1.0.0` / `100` / 包内三项 `phone` + `tablet` + `2in1`；AGC 勾选只有 PC/2in1）
- [ ] 跑一遍上架自检，逐条处理红色项
- [ ] 版本信息：应用名称、图标、简介、介绍、截图、隐私政策 URL、联系方式、分类标签、版权信息、内容分级、备案信息、版本更新说明（对照第 3 节逐项填）
- [ ] 最终用户协议已填写或托管为链接（3.1，用 `docs/store/eula.md` 正文；确认无与 GPL 冲突的限制）
- [ ] 支持设备只勾 PC/2in1，包内声明三项（3.6）
- [ ] 用 1.0.0 重拍 `pc-01-main` 与 `pc-04-about` 两张（3.3）
- [ ] 上传演示视频
- [ ] 提交审核，记录提交时间与包 SHA-256

### 5.2 常见驳回原因与应对

| 驳回表述（大意） | 真实原因 | 应对 |
| --- | --- | --- |
| 受限权限使用场景不符 | 手机侧 `READ_PASTEBOARD` 不在允许场景内（首发只勾 PC/2in1 时不涉及）；或提交的场景说明没讲清「前台、授权后、纯文本、不落盘」 | 首发按 4.1 提交 PC 侧理由 + 录屏；若以后加手机被拒，走第 6 节的 A/B/C 三条路 |
| 应用类目与实际功能不符 | 二级分类选了「效率」等不够贴合的类目 | 改「商务办公」，不行再改「工具」；同时在描述里写明「自备 VNC/RDP 服务器的远程桌面客户端，非 VPN、非代理、无内置服务器」 |
| 截图信息不全 / 与实际不符 | 截图少于页面下限、缺关键界面，或截图里版本号是 0.0.1 | 按 3.2 补齐张数与顺序；按 3.3 用 1.0.0 重拍 |
| 隐私政策与实际不符 / 打不开 | URL 打不开、内容与权限声明不一致、含登录墙 | 用 `https://tianguan.alfadb.cn/privacy-policy.html`；正文与包内 `privacy_policy.md` 保持同一版本；确认 200 且无跳转 |
| 应用功能无法验证，需提供演示视频 | 应用依赖用户自备的远程主机，审核侧无环境 | 上传按 [`appgallery-copy.md`](appgallery-copy.md) 第 4 节脚本拍的真实录屏（连接→操作→剪贴板授权→拒绝后降级） |
| 未备案 / 备案信息不一致 | 备案号缺失，或名称/包名/主体与在架不一致 | 完成 1.4，备案名称、包名、主体三处与 AGC 完全一致 |
| 版权证书缺失 / 名称不一致 | 含手机的国内发布，版权证书必选；软著名称与「天关」不一致 | 首发只勾 PC/2in1，不涉及；一旦加手机，按 1.5 补证书；名称不一致时先办名称变更或附授权书 |
| 图标与安装后不一致 | 上传的平面图与包内分层图标图形不同源 | 用 `docs/store/icon-1024.png`（与包内 `background.png` + `foreground.png` 叠加结果逐像素一致） |

### 5.3 隐私政策 URL 的一个隐患

该页面托管在 GitHub Pages 上、用自定义域 `tianguan.alfadb.cn` 提供。形式上满足「静态 HTML、独立页面、可直接读取」，但如审核以「托管在 GitHub」为由质疑（官方对博客/代码托管平台有排除口径），可把 `docs/privacy-policy.md` 渲染后放到自有主机同名路径，再改 AGC 里的 URL。

- [ ] 提交前实测：`curl -sSI https://tianguan.alfadb.cn/privacy-policy.html` 返回 200
- [ ] 页面正文与包内 `app/entry/src/main/resources/rawfile/privacy_policy.md` 一字不差

---

## 6. 被拒预案：`READ_PASTEBOARD` 被拒时的三条路

前提：首发只勾 PC/2in1，手机侧受限权限场景风险**不在首发范围**；本节保留给「以后加手机」时参考。真被拒时按下面的代价从低到高选。

| 方案 | 具体做法 | 代价 / 影响 | 何时选 |
| --- | --- | --- | --- |
| **C. 补充说明 + 申诉**（先试，成本最低） | 在驳回工单/申诉入口提交 4.1 的场景说明 + 权限使用录屏（本机复制→远端粘贴、授权弹窗、拒绝后降级），强调「前台、按需、用户授权、纯文本、不落盘、不上传第三方」，并说明远程桌面粘贴是产品刚需 | 只需写材料与录屏；等待一轮审核。**不保证通过**（场景清单不含远程桌面），且可能反复 | 首次被拒、且你判断审核员只是没看到授权与降级证据 |
| **A. 改用系统粘贴控件（`PasteButton`）** | 去掉 `READ_PASTEBOARD` 声明与后台读取逻辑，在远程窗口加一个系统粘贴安全控件：用户点一次，把剪贴板内容发送到远端一次 | 代码改动（声明、门控、UI、测试），失去「自动同步」，变成手动粘贴；需重新构建、重测、重拍受影响的截图；发布 Profile 可不再申请该受限权限（审核风险归零，PC 侧也退化为手动） | 申诉失败，且你更看重「一次过审」而不是自动同步体验 |
| **B. 拆包：手机包不声明该权限** | 做成按设备分发的两个包（2in1 包保留自动剪贴板；手机包不声明、改用粘贴控件或不做该功能） | 工程改动最大（多 product / 多 HAP 与资源配置、双份测试与发版流程）；商店要分别维护两套包与截图；用户已明确不考虑拆两个 HAP，需重新拍板 | A 也不想做、又必须保住 PC 侧自动同步 |

补充：**首发只上 PC/2in1 已是 2026-10-28 的现行决定**（见 [pc-only-publishing.md](pc-only-publishing.md)）：手机侧受限权限与软著问题在首发不会出现。以后加手机时，再按本节重新评估 A/B/C。

### 6.1 无论走哪条路，先确认这些没做错

- [ ] Profile 的 ACL 确实勾了 `READ_PASTEBOARD`（否则不是审核问题，是签名配置问题，安装会报 `9568289`）
- [ ] 包内 `module.json5` 的 `usedScene` 与 `reason` 合理（前台、`when: 'inuse'`，理由文案面向用户可读）
- [ ] 未授权时确实完全不读剪贴板（真机日志 `TianguanClip perm=0 ... gate=` 可证），这条是申诉最有力的证据
- [ ] 隐私政策里剪贴板条目与提交的场景说明口径一致（官方对剪贴板有标准表述「剪切板」）

---

## 附录 A：素材清单速查

首发只需 PC/2in1 一组；手机 3 张保留作参考，以后加设备时再补正式素材。

| 文件 | 规格 | 用途 |
| --- | --- | --- |
| [`icon-1024.png`](icon-1024.png) | 1024×1024 RGB，不透明 | AGC 应用图标（1024 档） |
| [`icon-216.png`](icon-216.png) | 216×216 RGB，不透明 | AGC 应用图标（216 档） |
| `screenshots/pc-01-main.png` | 1920×1080 | 2in1 首图：主界面（含 VNC/RDP 条目）⚠️ 布局更新后须重拍 |
| `screenshots/pc-02-rdp.png` | 1920×1080 | RDP 会话到 Windows 11 |
| `screenshots/pc-03-vnc.png` | 1920×1080 | VNC 会话到 Ubuntu |
| `screenshots/pc-04-about.png` | 1920×1080 | 关于面板 ⚠️ 版本号 0.0.1，须重拍 |
| `screenshots/pc-05-form.png` | 1920×1080 | 新建连接表单 |
| `screenshots/phone-01-main.png` | 1080×1920 | 后续扩展参考（首发不提交） |
| `screenshots/phone-02-session.png` | 1080×1920 | 后续扩展参考（首发不提交） |
| `screenshots/phone-03-about.png` | 1080×1920 | 后续扩展参考（版本号 0.0.1，加设备前须重拍） |
| [`appgallery-copy.md`](appgallery-copy.md) | 文案 + 脚本 | 简介、介绍、使用须知、演示视频脚本、分类标签、权限说明、打码对照 |
| [`../privacy-policy.md`](../privacy-policy.md) | Markdown | 隐私政策正文（与线上页面、包内 `rawfile/privacy_policy.md` 同源） |
| [`../../CHANGELOG.md`](../../CHANGELOG.md) | Markdown | 版本更新说明来源 |

## 附录 B：当前阻塞项（一句话版）

1. ⬜ 发布证书 + 发布 Profile（含 `READ_PASTEBOARD` ACL）——没有就签不出可上架的包；仓里现有的已签名 `.app` 是调试 Profile 签的，不能直接上传；
2. ⬜ APP 备案——没有就填不完版本信息；官方没有 PC 豁免，需向华为确认只发 PC/2in1 是否仍属备案范围，确认前按仍需备案准备（软著首发不办）；
3. ⚠️ `pc-01-main` 与 `pc-04-about` 两张截图须用 1.0.0 重拍（3.3）；
4. ⬜ 演示视频未拍——自备服务器类应用的必备材料。

上面列出的前置依赖与素材待办都清掉后，按第 5 节从上往下打勾即可提交。
