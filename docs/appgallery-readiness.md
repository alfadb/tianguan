# 天关（cn.alfadb.tianguan）鸿蒙应用市场上架差距清单

- 审计日期：2026-10-26
- 审计基线：`main` @ `b452b67`（工作区干净）
- 审计方式：只读阅读仓库配置/资源/构建产物 + 查阅华为官方文档（2026-10 更新版本）。**审计当时未修改任何代码或配置，未提交 git。**
- 图标补齐记录（2026-10-26）：分层图标与 AGC 平面素材已入库，`app.json5`/`module.json5` 已改为 `$media:layered_image`；包内实际图层为 **512×512**（restool 固定上限缩放），推导与实测见 3.3 与 `docs/store/README.md`。
- 目标：HarmonyOS 应用（.app / HAP），设备 phone + 2in1，发布中国大陆，免费应用。
- 发布决策（2026-10-26 更新）：用户已决定**全量发布**（手机 / 平板 / 智慧屏 / PC / 2in1），版本 `1.0.0 / 100`，API 保持 26；`READ_PASTEBOARD` 在手机侧为已知风险，若审核被拒再拆包或改用系统粘贴控件。本文其余「现状」是审计快照，最新落地见 [release-pipeline.md](release-pipeline.md)。
- 状态图例：✅ 满足 ｜ ❌ 缺失 ｜ ⚠️ 需确认/需实测 ｜ — 不适用

> 重要前提：本文的「官方依据」只覆盖能查到的华为官方页面；查不到的项会明确写「未找到官方依据，需向华为确认」，不替华为下结论。所有「现状」均可在附录 A 的文件/行号或构建产物中核对。

---

## 0. 结论速览

1. **发布链路已补齐本地构建**：`scripts/build-release.sh` 可产出未签名 `.app`（`app/build/outputs/default/app-default-unsigned.app`）；仓库仍只有调试证书/调试 Profile（签名材料不在仓库），`app/build-profile.json5` 无 `signingConfigs`，AGC 要求的是用**发布证书 + 发布 Profile** 签名的 **.app**（App Pack）。
2. **图标已补齐**：AppScope 下已有 1024×1024 分层图标（`background.png` 不透明 + `foreground.png` 透明 + `layered_image.json`），`app.json5` 与 `module.json5` 均引用 `$media:layered_image`；AGC 详情页 216/1024 平面图在 `docs/store/`。包内实际图层为 **512×512**（restool 固定上限，属编译期优化，非素材缺失），详见 3.3 与 `docs/store/README.md`。
3. **隐私合规是最大缺口**：仓库和应用内**都没有隐私政策**，也没有首启隐私弹窗/显著提示；官方明确要求在开发者中心**和应用内**都提供可便捷查看的隐私政策链接，且收集/使用前须取得用户同意。
4. **`READ_PASTEBOARD` 在手机上大概率被拒**：该受限权限对 PC/2in1「应用均可申请」，但手机只允许列举场景（银行/口令/文档/输入法/开源框架），**远程桌面不在列**；发布 Profile 还必须包含该权限、并提供权限使用理由 + 使用场景视频。用户已决定全量发布并接受该风险；若被拒再拆包或改用系统粘贴控件。
5. **`compatibleSdkVersion: 26.0.0` 把可安装范围锁到 HarmonyOS 7.0+**（官方定义：低于该版本的设备不能安装），与「适配华为主流终端」的审核要求存在张力；当前主流设备仍在 6.1.x（API 23/24）。
6. **远程桌面类目本身不是禁区**：官方资质清单、审核指南「部分类型应用」、不收录清单均未列远程桌面/远程控制；AppGallery 在架有 ToDesk、向日葵、UU远程、时代云远程桌面（RDP/SSH/VNC）等先例，分类均为 Tools。但官方鸿蒙分类表中**没有**「远程桌面/远程控制」二级类目，需在 AGC 实选或问华为。
7. **备案与软著是长周期外部依赖**：APP 备案（工信部）为强制的上架前置；发布中国大陆且含手机时，「应用版权证书或代理证书」在版权信息页是**手机应用必选**（软著三种形式之一即可）。
8. **GPL 与商店分发未发现冲突**：华为开发者协议/AGC 服务协议未禁止 GPL/开源应用，且明确开源组件受其许可证约束；但必须履行 GPLv3 源码提供义务、EULA 不得附加限制、软著/版权主体与开发者一致。商店要求提交 EULA 副本（AGC 服务协议 3.4）。
9. **必须有演示视频**：应用依赖用户自备 VNC/RDP 服务器才能体现功能，审核指南 1.21 要求此类应用提供演示视频（同时 12.8 要求在描述中写明适用范围）。
10. **版本号/变更记录/应用内「关于」缺失**：`versionName 0.0.1 / versionCode 1`、无 CHANGELOG、应用内无关于页（版本、联系方式、隐私政策、GPL 许可与源码地址都无处展示）。

---

## 1. 准入清单表

### 1.1 账号与主体

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 1 | 注册华为开发者联盟并完成实名认证（个人或企业）；账号主体即应用分发主体 | [个人开发者实名认证](https://developer.huawei.com/consumer/cn/doc/start/ht-idrna-0000001200848143)；[审核指南 11.4](https://developer.huawei.com/consumer/cn/doc/app/50104)（金融/医疗等须企业主体，远程桌面不在此列） | 仓库记录显示已用「企业账号调试证书」签名并安装真机（`README.md:21`、`app/README.md:9`），说明账号存在；账号类型/主体是否即最终发布主体**未在仓库中体现** | ⚠️ 需确认 |
| 2 | 个人/企业主体限制 | 审核指南 11.4：银行/金融、医疗健康等须由相应法律实体提交；[审核指南 12](https://developer.huawei.com/consumer/cn/doc/app/50104) 的特殊类目限制不含远程桌面 | 远程桌面客户端不属于受限类目 | ✅（个人或企业均可，以实际审核为准） |
| 3 | 在 AGC 创建 HarmonyOS 应用（App ID）：包名全局唯一、设备类型、默认语言 | [发布应用（DevEco）](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-publish-app)；[配置本地化基础信息](https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-nameicon-0000002306265009) | 仓库无任何 AGC 应用记录/截图；`cn.alfadb.tianguan` 仅存在于包内 | ❌ 缺失 |

### 1.2 包格式与签名

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 4 | 上架包格式为 **.app（App Pack）**，由 HAP 打包而成 | [发布应用（DevEco）：打包成 .app 后上架](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-publish-app) | 只有 `entry-default-unsigned.hap`；`build.sh` 无 `assembleApp` 步骤 | ❌ 缺失 |
| 5 | 包内 `bundleName` 与 AGC 创建时一致 | [发布准备工作](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-prepare-0000002306311921) | `cn.alfadb.tianguan`（`app/AppScope/app.json5:2`、`app/build-profile.json5:3`、`app/entry/src/main/module.json5`） | ✅ 满足（待 AGC 侧创建） |
| 6 | 使用**发布证书 + 发布 Profile** 签名（调试证书/调试 Profile 不可上架） | [申请发布证书](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-cert-0000002283336729)；[申请发布Profile](https://developer.huawei.com/consumer/cn/doc/app/agc-help-add-releaseprofile-0000001914714796) | 无 `signingConfigs`（`app/build-profile.json5` 全文仅 4 行）；构建产物 unsigned | ❌ 缺失 |
| 7 | 每个账号最多 3 个发布证书；发布证书有效期 3 年（实名认证开发者） | [申请发布证书](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-cert-0000002283336729) | 未申请 | ❌ 缺失 |
| 8 | 发布 Profile 中申请的权限必须覆盖软件包声明的受限权限；类型选「发布」 | [申请发布Profile](https://developer.huawei.com/consumer/cn/doc/app/agc-help-add-releaseprofile-0000001914714796) | 仅有调试 ACL Profile（`TianGuanDebugDevicesACLDebug.p7b`，见 `app/README.md:374`），无发布 Profile | ❌ 缺失 |
| 9 | APP 包 ≤4GB；单 HAP（手机/PC/2in1）≤4GB；所有 HAP 为非免安装；`bundleType=app`；`label` 非空；`debug≠true`；API Level ≥10 | [发布准备工作](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-prepare-0000002306311921) | 当前 HAP 15.26MB；`installationFree:false`；`bundleType:app`；label 非空；包内 `module.json` 无 `debug`、`app.debug=false`；`apiVersion` 26 | ✅ 满足（仅格式层面；签名后需复核） |
| 10 | 发布版本不得为可调试版本 | [审核指南 3.8](https://developer.huawei.com/consumer/cn/doc/app/50104) | 当前 release 构建 `pack.info` 为 `releaseType: Release`、`app.debug=false` | ✅ 满足（构建层面） |
| 11 | 上传后通过「合法性检测」与（建议）「上架自检」 | [上传软件包](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-upload-pkg-0000002277983368) | 从未上传，未做检测 | ❌ 待做 |

### 1.3 应用信息与素材

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 12 | 应用名称 ≤15 个汉字或 30 个其他语言字符；不得为泛词；不得与他人重名；须与软著名称/简称一致 | [审核指南 1.1–1.5](https://developer.huawei.com/consumer/cn/doc/app/50104)；[软著证书说明 Q3](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111-02) | 「天关」2 字，非泛词；AppGallery 站内搜索未见同名应用（[检索页](https://appgallery.huawei.com/search/%E5%A4%A9%E5%85%B3)，2026-10-26）；无软著 | ⚠️ 名称本身可，软著一致性待办 |
| 13 | AGC 详情页应用图标 216×216 或 1024×1024 PNG（≤3MB），须与安装后图标一致 | [素材规范](https://developer.huawei.com/consumer/cn/doc/app/agc-help-app-visual-asset-spec-0000002277607976)；[配置名称/图标](https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-nameicon-0000002306265009) | 已补齐：`docs/store/icon-1024.png`（1024×1024 RGB，20,709 B）与 `docs/store/icon-216.png`（216×216，7,443 B）；1024 与源两层合成结果逐像素一致 | ✅ 已补齐 |
| 14 | 包内应用图标必须是 **1024×1024 分层图标**：前景图 + 背景图，PNG，正方形，无圆角，背景不得含透明像素 | [图标资源规范](https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/application-icon-0000001953444009)；[配置应用图标和名称](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/layered-image) | 已补齐：`app/AppScope/resources/base/media/{background.png,foreground.png,layered_image.json}`，源 1024×1024；entry 不再重复放置。构建后 HAP 内两图层为 512×512（restool 固定上限），背景源文件为 RGB、无 alpha 通道 | ✅ 已补齐（包内 512 属编译期优化，见 3.3） |
| 15 | 应用截图：手机 3–10 张（16:9 1920×1080 或 9:16 1080×1920）；PC/2in1 3–10 张（16:9 1920×1080）；不得重复/模糊/拉伸 | [素材规范](https://developer.huawei.com/consumer/cn/doc/app/agc-help-app-visual-asset-spec-0000002277607976)；[审核指南 1.9](https://developer.huawei.com/consumer/cn/doc/app/50104) | 仓库无上架截图（`.work` 下的设备截图不是素材） | ❌ 缺失 |
| 16 | 应用介绍视频（可选）；但本应用依赖外部服务器，须提供**演示视频** | [审核指南 1.21](https://developer.huawei.com/consumer/cn/doc/app/50104)：需连接设备/特殊运行环境才可使用的应用须提供演示视频 | 无演示视频、无公开测试服务器 | ❌ 缺失 |
| 17 | 应用分类/标签须与实际功能相符 | [鸿蒙应用分类](https://developer.huawei.com/consumer/cn/doc/app/classify-0000001960172909)；[配置分类、标签](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-class-tag-0000002271695234) | 官方分类表无「远程桌面/远程控制」二级类目；先例（ToDesk/向日葵/时代云远程桌面）归入 Tools | ⚠️ 需确认具体二级类目 |
| 18 | 内容分级（年龄分级问卷） | [配置内容分级](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-rating-0000002313511005)；[审核指南 1.10](https://developer.huawei.com/consumer/cn/doc/app/50104) | 未做 | ❌ 缺失 |
| 19 | 一句话简介、应用介绍、新版本特性，须与实际一致，不得含「测试/demo/beta」等词 | [审核指南 1.14](https://developer.huawei.com/consumer/cn/doc/app/50104)；[审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170) | 无上架文案；README 中「计划/未实现」等表述不能直接当介绍 | ❌ 缺失 |
| 20 | 应用内须有真实有效的开发者联系信息 | [审核指南 11.2](https://developer.huawei.com/consumer/cn/doc/app/50104) | 应用内只有连接列表与远程页（`pages/Index.ets`、`pages/Remote.ets`），无关于/联系入口 | ❌ 缺失 |
| 21 | 提交的名称、图标须与安装后一致 | [配置名称/图标](https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-nameicon-0000002306265009)；[审核指南 1.20](https://developer.huawei.com/consumer/cn/doc/app/50104) | 包内 label=天关、`icon=$media:layered_image`（`bm dump` iconId=16777237）；PC `install -r` 后 Dock 图标与 `docs/store/icon-1024.png` 同形，证据 `.work/icon-evidence/` | ✅ 已核对（上架前用最终 AGC 素材再复核一次） |

### 1.4 隐私与权限

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 22 | 必须在 AGC 提供隐私政策（URL 或隐私托管） | [配置隐私声明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-state-0000002278878296)；[隐私政策链接 FAQ](https://developer.huawei.com/consumer/cn/doc/app/50128) | 无隐私政策文件；`docs/` 仅 `licensing.md`、`napi-contract.md` | ❌ 缺失 |
| 23 | 必须在**应用内**提供可便捷查看的隐私政策链接，且与应用在 AGC 提交的 URL 内容完全一致 | [审核指南 7.1](https://developer.huawei.com/consumer/cn/doc/app/50104) | 应用内无任何隐私入口 | ❌ 缺失 |
| 24 | 首启须以弹窗等显著方式提示阅读隐私政策；同意/拒绝明确、不得默认勾选；未同意前不得收集个人信息/申请权限 | [审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170)；[审核指南 7.5](https://developer.huawei.com/consumer/cn/doc/app/50104) | 无弹窗；当前不申请剪贴板权限（打开会话时才申请），这点符合「不提前申请」 | ❌ 缺失（弹窗） |
| 25 | 隐私政策内容须含开发者名称、应用名称、收集/处理目的方式范围、用户权利、联系方式；剪切板须按标准表述写明 | [审核指南 7.1/7.3](https://developer.huawei.com/consumer/cn/doc/app/50104)；[50128 标准表述](https://developer.huawei.com/consumer/cn/doc/app/50128) | 无 | ❌ 缺失 |
| 26 | 若采集个人信息：应用内二级菜单展示「已收集个人信息清单 / 第三方共享清单」 | [审核指南 7.25](https://developer.huawei.com/consumer/cn/doc/app/50104) | 未判定是否构成「收集」（见 4.6） | ⚠️ 需确认 |
| 27 | 涉及受限权限：需在 AGC 填「应用隐私说明」——权限使用理由 + **使用场景视频**（.mp4/.mov，h.264，≤500MB） | [配置隐私说明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-desc-0000002313477969) | 未填；`module.json5:17-19` 只有 `reason`（复用 `module_desc`）与 `usedScene` | ❌ 缺失 |
| 28 | 软件包声明的受限权限必须同时出现在发布 Profile 中，否则审核驳回；声明的权限若不在 Profile 内会安装失败（9568289） | [配置隐私说明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-desc-0000002313477969)；[申请发布Profile](https://developer.huawei.com/consumer/cn/doc/app/agc-help-add-releaseprofile-0000001914714796) | 调试 ACL 已批（`app/README.md:374`）；发布 Profile 未建 | ❌ 缺失 |
| 29 | `READ_PASTEBOARD` 受限权限：PC/2in1 应用均可申请；手机仅限列出的特殊场景（银行卡号/口令/文档编辑/系统输入法/使用开源框架且无法用粘贴控件） | [受限开放权限](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/restricted-permissions#ohospermissionread_pasteboard) | 同一 HAP 对 phone + 2in1 都声明了该权限（`module.json5:8,17`） | ⚠️ 高风险（见 4.2） |
| 30 | 权限最小化、按业务场景动态申请、拒绝后不得退出/循环弹窗 | [审核指南 7.17/7.19](https://developer.huawei.com/consumer/cn/doc/app/50104) | `INTERNET`/`PREPARE_APP_TERMINATE` 为 system_grant；`READ_PASTEBOARD` 在打开会话时申请一次（`pages/Remote.ets:474`），未授权则停用读取 | ✅ 满足 |
| 31 | `PREPARE_APP_TERMINATE` 属开放权限（normal / system_grant，起始 API 10），无需 ACL | [开放权限（系统授权）](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/permissions-for-all) | 已声明（`module.json5:14`） | ✅ 满足 |
| 32 | 隐私标签：应用详情页展示；不收集个人信息可填「否」 | [配置隐私标签](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-tag-0000002316420993) | 未填 | ❌ 缺失/待判定 |

### 1.5 备案、版权与资质

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 33 | APP 备案（工信部）：未备案不得从事 APP 互联网信息服务；上架时 AGC 校验，未备案影响搜索展示 | [APP核准（备案）指引](https://developer.huawei.com/consumer/cn/doc/app/50130)；[审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170) | 无备案号 | ❌ 缺失 |
| 34 | 鸿蒙版备案须在接入商系统选择「鸿蒙」平台并填写鸿蒙包名（多个包名均需备案） | [APP核准（备案）指引 2.2](https://developer.huawei.com/consumer/cn/doc/app/50130) | 无 | ❌ 缺失 |
| 35 | 备案的应用名称/包名/主体须与在架信息一致 | [APP核准（备案）指引 3.3](https://developer.huawei.com/consumer/cn/doc/app/50130) | 无 | ❌ 缺失 |
| 36 | 版权信息：发布中国大陆且含手机时，「应用版权证书或代理证书」为**手机应用必选**；电子版权证书为可选（PDF ≤5MB） | [配置版权信息](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-copyright-0000002278981450) | 无软著/版权证书 | ❌ 缺失 |
| 37 | 软著三种形式：计算机软件著作权登记证书 / APP 电子版权认证证书 / 软件著作权认证证书；名称须与应用名一致；著作权人须与开发者一致（否则需授权书） | [软著证书说明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111-02)；[审核指南 1.1](https://developer.huawei.com/consumer/cn/doc/app/50104) | 无 | ❌ 缺失 |
| 38 | 特殊类目资质 | [应用资质审核要求](https://developer.huawei.com/consumer/cn/doc/app/80301) | 清单中无远程桌面/远程控制类目 | — 不适用 |
| 39 | 增值电信业务经营许可证（仅从事增值电信业务时） | [资质：ICP备案/许可证](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111-03)；[审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170) | 免费工具，无自有付费电信业务 | — 不适用（需确认） |
| 40 | 安全评估报告（仅特定情形） | [资质文件法规依据](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111) | 不涉及舆论属性/社交 | — 不适用（需确认） |
| 41 | 上架后默认授权华为应用市场为华为终端官方分发渠道（非独占） | [审核指南 概述](https://developer.huawei.com/consumer/cn/doc/app/50104) | 无异议（需开发者接受） | ⚠️ 条款确认 |

### 1.6 发布配置与审核

| # | 要求 | 官方依据 | 天关现状 | 判定 |
|---|---|---|---|---|
| 42 | 支持设备勾选范围 ≤ `deviceTypes` 声明范围 | [配置支持设备](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-devicetype-0000002271592112) | `deviceTypes: ['2in1','phone']`（`module.json5:8`、`entry/build-profile.json5:1`） | ✅ 满足 |
| 43 | 选择发布国家/地区（境外当前仅手表类可发） | [发布HarmonyOS应用](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-0000002271695230) | 未配置；本项目目标为中国大陆 | ❌ 待做 |
| 44 | 应用内资费类型（免费/付费） | 同上 | 未配置；项目为免费 | ❌ 待做 |
| 45 | 审核信息：联系方式、测试说明；需账号的应用提供测试账号 | [发布HarmonyOS应用](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-0000002271695230)；[审核指南 1.21](https://developer.huawei.com/consumer/cn/doc/app/50104) | 未配置；本应用无账号体系，但需说明「须自备 VNC/RDP 服务器」 | ❌ 待做 |
| 46 | 常规审核时效约 24 小时；复杂应用可能延长 | [审核指南 概述](https://developer.huawei.com/consumer/cn/doc/app/50104) | — | 信息项 |
| 47 | 应用须支持 IPv6 网络环境正常运行 | [审核指南 3.14](https://developer.huawei.com/consumer/cn/doc/app/50104) | 未找到 IPv6 字面量/双栈处理与实测记录 | ⚠️ 需实测 |
| 48 | 应用不得含隐藏功能/测试数据；不得为测试版本 | [审核指南 2.16/3.4](https://developer.huawei.com/consumer/cn/doc/app/50104)；[审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170) | 启动不再跑诊断、无 `vnc-test.json` 读取（grep 无命中）；但 `README` 仍描述旧诊断流程 | ✅ 代码层面（文档需同步） |
| 49 | 应用不得绕过审核自动更新、不得动态下发可执行代码 | [审核指南 3.13/2.5](https://developer.huawei.com/consumer/cn/doc/app/50104) | 原生库随包分发，无热更新 | ✅ 满足 |
| 50 | UX 规范（深色模式、对比度等）需通过上架自检/云测试 | [审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170)；[应用审核指南 常见问题 5](https://developer.huawei.com/consumer/cn/doc/app/50104) | 有 dark 配色（`resources/dark/element/color.json`）；未做 DevEco Testing/云测试 | ⚠️ 待检 |

---

## 2. 必须由用户（开发者账号持有人）完成的项

> 以下每项都写明「在 AGC 哪里做 / 需要什么材料」。凡涉及账号、证书私钥、备案主体的操作，只能由账号持有人完成。

### 2.1 实名认证与主体确认

- **位置**：华为开发者联盟 → 账号中心 → 实名认证（个人：人脸/银行卡/身份证人工/华为云授权；企业：营业执照、法人信息、对公验证，以页面为准）。
- **材料**：个人身份证；或企业营业执照、法人证件、对公账户信息。
- **依据**：[个人开发者实名认证](https://developer.huawei.com/consumer/cn/doc/start/ht-idrna-0000001200848143)。
- **天关相关**：需确认仓库记录中的「企业账号」是否就是最终发布主体；它同时决定软著著作权人、备案主体、开发者展示名称。**账号类型不支持个人↔企业直接变更**（[账号 FAQ](https://developer.huawei.com/consumer/cn/doc/start/account-faq-0000001953714830)），务必先定主体。

### 2.2 在 AGC 创建 HarmonyOS 应用

- **位置**：AGC → 我的项目 → 添加项目/应用 → HarmonyOS 应用（也可在 DevEco「发布应用」向导中创建并同步到 AGC）。
- **材料/信息**：应用名称「天关」、包名 `cn.alfadb.tianguan`（创建后不可改）、设备类型（手机 + PC/2in1）、默认语言（简体中文）、应用分类（工具）。
- **依据**：[发布应用（DevEco）](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-publish-app)；[配置名称/图标](https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-nameicon-0000002306265009)。
- **注意**：包名是证书 Profile、备案、软著、后续所有上传的锚点；发布后升级只能增加设备、不能删除设备（[配置支持设备](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-devicetype-0000002271592112)）。

### 2.3 软著 / 应用版权证书（长周期，建议立即启动）

- **位置**：中国版权保护中心（或华为认可的电子版权认证渠道）办理；上架时在 AGC「应用上架 > 版本信息 > 版权信息 > 应用版权证书或代理证书」上传。
- **材料**：软件名称须为「天关」或含「天关」的规范全称（须与应用名/简称一致）；著作权人须与开发者账号主体一致；源代码/文档样本、申请表等。
- **依据**：[配置版权信息](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-copyright-0000002278981450)（手机应用必选）；[软著证书说明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111-02)（名称/著作权人一致性、授权书）。
- **注意**：`80301` 把软著写成「非必选资质、建议申请」，但版权信息页对**手机应用**把「应用版权证书或代理证书」列为必选；实操按必选准备。本次为全量发布（含手机），按手机应用的必选要求准备。
- **周期**：常规登记数十个工作日，加急另计；这是上架关键路径之一。

### 2.4 申请发布证书

- **本地**：DevEco Studio → Build → Generate Key and CSR，生成 `.p12`（密钥库）与 `.csr`；记住密码与别名，建议有效期 25 年；**密钥材料不得入库**（仓库 `.gitignore` 已忽略 `*.p12/*.cer/*.p7b/*.csr` 等）。
- **AGC**：开发者管理 → ID、证书和 Profile → 证书 → 新增证书 → 类型「发布证书」→ 上传 CSR → 下载 `.cer`。
- **材料**：CSR、证书名称；账号需具备「访问发布类证书」权限。
- **依据**：[申请发布证书](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-cert-0000002283336729)。
- **限制**：每账号最多 3 个发布证书；有效期 3 年；换证书须同步换 Profile；证书页可查看公钥/指纹供备案使用。

### 2.5 申请发布 Profile（含 READ_PASTEBOARD 受限 ACL）

- **位置**：开发者管理 → ID、证书和 Profile → Profile → 添加 → 应用/包名 → **类型「发布」** → 选择发布证书 → 「申请权限」栏勾选「受限 ACL 权限（HarmonyOS API9 及以上）」→ 勾选 `ohos.permission.READ_PASTEBOARD` → 提交业务申请（填写场景/理由，必要时附件）→ 审批通过后到「已获取权限」勾选并确定 → 下载 `.p7b`。
- **材料/信息**：权限使用场景说明（远程桌面中把本机剪贴板**纯文本**转发到当前会话；仅前台、仅会话打开时读取；不采集、不上传第三方、不持久化；可在系统设置关闭）；必要时提供录屏/截图证据。
- **依据**：[申请发布Profile](https://developer.huawei.com/consumer/cn/doc/app/agc-help-add-releaseprofile-0000001914714796)；[受限开放权限 READ_PASTEBOARD](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/restricted-permissions#ohospermissionread_pasteboard)（AGC 审核约 3 个工作日；权限与**应用**绑定，调试 ACL 已批的权限在新建 Profile 时可直接勾选）。
- **硬约束**：软件包声明了受限权限、Profile 却未包含 → 审核驳回或安装失败（9568289）；Profile 申请的权限必须覆盖包内所有权限。

### 2.6 APP 备案

- **位置**：在所选择的接入商（华为云/阿里云/腾讯云/移动云/天翼云/联通云等）的 APP 备案系统办理；材料中平台**必须勾选「鸿蒙」**并填写鸿蒙包名 `cn.alfadb.tianguan`。
- **材料（以接入商要求为准）**：主体证件、APP 名称/图标、包名、以及接入服务信息（域名/服务器等）。具体清单请在接入商页面确认（[50130](https://developer.huawei.com/consumer/cn/doc/app/50130) 只给路径，未列明细）。
- **AGC 填写**：应用上架 → 版本信息 → 「核准（备案）信息」，按「服务器在中国大陆 / 不在中国大陆 / 单机」三选一；主体一致可直接校验，不一致需手填主办单位名称与证件号。
- **依据**：[APP核准（备案）指引](https://developer.huawei.com/consumer/cn/doc/app/50130)；[审核 Checklist](https://developer.huawei.com/consumer/cn/doc/app/50170)。
- **天关相关**：应用是联网客户端（连接用户自备的 VNC/RDP 服务器），**不能按「单机应用」豁免**；但开发者没有自有服务端，如何填写接入信息需向接入商确认（见 4.5）。备案名称/包名/主体必须与在架完全一致。

### 2.7 隐私政策：二选一

**方案 A（推荐，最省事）：AGC 标准化隐私声明托管**
- **位置**：AGC → 应用上架 → 版本信息 → 隐私声明 → 隐私托管；先基于模板生成隐私政策与用户协议，再在此选择。
- **适用**：发布地区含中国大陆、设备为手机/平板/PC/2in1 的 HarmonyOS 应用。
- **依据**：[配置隐私声明](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-state-0000002278878296)。
- **巨大好处**：接入托管后，**首次打开应用会由系统弹出标准化隐私弹窗**，官方明确要求**不要再自行实现弹窗**（[隐私管理服务](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/store-privacy)）；应用内通过 `privacyManager.getAppPrivacyMgmtInfo()` 取链接展示即可。

**方案 B：自建隐私政策 URL**
- **位置**：版本信息 → 隐私声明 → 自定义隐私政策（填 URL + 隐私权利 URL）。
- **硬要求**：静态 HTML、中文、无需登录/点击/跳转、可被自动化读取；**不能是** PDF/Word/图片、博客/微博/GitHub/云笔记（[50128](https://developer.huawei.com/consumer/cn/doc/app/50128)）。
- **内容**：开发者名称、应用名称、剪贴板（标准表述「剪切板」）、连接信息与凭据的本地存储、网络目标由用户指定、无第三方 SDK/广告/统计、权限清单与用途、用户权利、联系方式、更新日期；须与应用内展示内容**完全一致**（[审核指南 7.1](https://developer.huawei.com/consumer/cn/doc/app/50104)）。

### 2.8 素材与文案

- **位置**：AGC → 应用上架 → 应用信息（名称/图标/支持设备）与版本信息（介绍/截图/视频/分级/隐私）。
- **材料**：
  - 应用图标 1024×1024 PNG（与包内分层图标合成效果一致；或 216×216）；
  - 手机截图 3–10 张（1080×1920 或 1920×1080）、PC/2in1 截图 3–10 张（1920×1080），不得重复/拉伸/模糊；
  - 演示视频 1 个（15 秒–2 分钟，h.264，≤500MB，展示连接 VNC/RDP、画面、键鼠、剪贴板）；
  - 一句话简介、应用介绍、新版本特性（不得出现 test/demo/beta、国家级/最佳等词）；
  - 分类（工具）、内容分级问卷、隐私标签、资费（免费）、发布地区（中国大陆）。
- **依据**：[素材规范](https://developer.huawei.com/consumer/cn/doc/app/agc-help-app-visual-asset-spec-0000002277607976)；[审核指南 1.9–1.14](https://developer.huawei.com/consumer/cn/doc/app/50104)；[配置内容分级](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-rating-0000002313511005)；[配置隐私标签](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-tag-0000002316420993)。

### 2.9 演示环境/审核说明

- **位置**：版本信息 → 审核信息与联系方式。
- **内容**：说明「本应用是 VNC/RDP 客户端，须用户自备服务器」；提供可复现的测试服务器地址或演示视频；写明无需账号登录。
- **依据**：[审核指南 1.21](https://developer.huawei.com/consumer/cn/doc/app/50104)（需特殊环境须提供演示视频）；[审核指南 12.8](https://developer.huawei.com/consumer/cn/doc/app/50104)（需与指定软件/设备配合的应用须在描述中写明适用范围）。

### 2.10 上传与自检

- **位置**：应用上架 → 软件包管理 → 上传 `.app`（选「测试和正式上架」）→ 查看「合法性检测」→ 建议「启动自检」（兼容性/稳定性/性能/功耗/UX/隐私）→ 提交审核。
- **依据**：[上传软件包](https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-upload-pkg-0000002277983368)；[发布HarmonyOS应用](https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-0000002271695230)。

---

## 3. 我可以直接改代码/配置完成的项

> 以下只列改动点与验收标准，本次未实施。行号以当前 `b452b67` 为准。

### 3.1 版本号与变更记录

- **改动（已完成）**：`app/AppScope/app.json5` 与 `app/build-profile.json5` 的 `versionName`/`versionCode` 已同步改为 `1.0.0` / `100`；已新增 `CHANGELOG.md`（首个条目 `1.0.0 / 2026-10-26`）并在 `README.md` 链接。
- **验收**：`assembleApp` 后 `pack.info`/`module.json`/AGC 软件包记录三处版本一致；CHANGELOG 有对应条目；AGC 首次上传不报版本冲突。

### 3.2 release 签名 + .app 构建

- **改动**：
  - `app/build-profile.json5` 增加 `app.signingConfigs`（字段：`name/type: HarmonyOS/material.storeFile|certpath|profile|storePassword|keyAlias|keyPassword|signAlg: SHA256withECDSA`，官方 schema 见 [build-profile 字段说明](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile-app)），并在 `products[0]` 加 `signingConfig: 'release'`；
  - `app/build.sh:27` 从 `assembleHap --mode module` 改为 `assembleApp`（DevEco 对应 Build > Build APP(s)），产物改为 `.app`；
  - 签名材料放仓库外（如 `~/.tianguan-signing/`），密码不写入仓库（`.gitignore` 已覆盖 `*.p12/*.cer/*.p7b/*.csr`）。
- **验收**：`hap-sign-tool verify-app` 通过且 Profile 类型为「发布」；AGC 合法性检测「已达标」；AGC 能解析出 `versionName/versionCode/deviceTypes`。
- **注意**：Profile 分发类型为 `app_gallery` 且证书为 release 的包**不能用 `bm` 安装到本地设备**（官方 bm 文档错误码 9568402，[bm 工具](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/bm-tool)）；release 包的真机验证只能走 AGC 内部测试/邀请测试或商店安装，不能把「hdc 装不上」当成失败。

### 3.3 分层图标（已补齐，2026-10-26）

- **已落地**：
  - `app/AppScope/resources/base/media/background.png`（1024×1024 RGB，不透明）、`foreground.png`（1024×1024 RGBA，前景透明）、`layered_image.json`（`{"layered-image":{"background":"$media:background","foreground":"$media:foreground"}}`）；
  - `app/AppScope/app.json5` 的 `icon` 与 `app/entry/src/main/module.json5` 两个 ability 的 `icon`/`startWindowIcon` 均为 `$media:layered_image`；
  - entry 下不再放同名三件套：AppScope 资源构建时会合入模块资源目录，模块清单可直接引用，同时消除 restool 的 `'...' is defined repeatedly` 警告（官方说明重名时 AppScope 覆盖模块副本）；
  - AGC 平面素材：`docs/store/icon-1024.png`（1024×1024 不透明 RGB）、`docs/store/icon-216.png`（216×216，由 1024 用 Lanczos 缩小），两张都与源两层合成结果逐像素一致。
- **包内实际尺寸**：源图层 1024×1024；`bash app/build.sh` 后 HAP 内 `resources/base/media/background.png`、`foreground.png` 均为 **512×512 RGBA**（背景 alpha 254）。这是 restool 对 `icon` 引用资源的固定 512 上限缩放（`CompressionParser::ScaleImage` 硬编码 `{512,512}`），属编译期正常优化；`layered_image.json` 与引用关系保留。
- **告警判读**：restool 的 `iconCheck` 对 base 目录按 sdpi-phone 的常规尺寸（`icon=41`、`startWindowIcon=144`）告警，1024 源图必然触发；该检查仅告警不失败，hvigor 默认日志级别不显示。另有一条与图标无关的 `app_name conflict`（AppScope/entry 各有一份 `string.json`），属既存字符串重复，不在本项处理。
- **依据**：[图标资源规范](https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/application-icon-0000001953444009)（1024×1024、分层、PNG、无圆角、背景不透明）；[配置应用图标和名称](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/layered-image)（AppScope 资源合入模块、重名覆盖）；restool 源码 [developtools_global_resource_tool](https://gitee.com/openharmony/developtools_global_resource_tool) 的 `resource_check.cpp`、`resource_data.h`、`compression_parser.cpp`、`config_parser.cpp`。本地实测与复现命令见 `docs/store/README.md`。
- **验收**：`bash app/build.sh` 退出 0；解包确认 2 张 PNG + `layered_image.json`；`bm dump -n cn.alfadb.tianguan` 的 `iconId=16777237`、`iconPath=$media:layered_image`；PC `hdc install -r` 后 Dock 图标正常（`.work/icon-evidence/desktop-dock-appscope-d1-20261026.jpeg`、`dock-icon-appscope-zoom-20261026.png`）。AGC 合法性检测/上架自检仍需真实上传后确认。

### 3.4 应用内隐私政策与「关于」入口

- **改动**：
  - 新增 `pages/About.ets`（注册进 `resources/base/profile/main_pages.json`），在 `pages/Index.ets` 顶部/设置入口进入；
  - 内容：版本号、开发者/主体名称、联系邮箱、**隐私政策链接**、GPLv3 许可声明与源码地址、第三方组件许可（TigerVNC GPL-2.0-or-later、FreeRDP Apache-2.0、libc++ 等）、备案号（拿到后）；
  - 隐私链接二选一：托管方案调 `privacyManager.getAppPrivacyMgmtInfo()` 取 URL 打开；自建方案直接打开 URL 或内置同文页面。
- **验收**：从首页 ≤2 次点击可达；链接可打开且与 AGC 提交内容一致；满足 [审核指南 7.1](https://developer.huawei.com/consumer/cn/doc/app/50104)「应用内可便捷查看」；同时覆盖 11.2 联系信息要求。

### 3.5 首启隐私同意

- **改动**：
  - 若用 AGC 隐私托管：**不要自实现弹窗**（官方明确会由系统弹出，自实现会被驳回，见 [隐私管理服务](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/store-privacy)）；仅在关于页展示链接；
  - 若自建 URL：实现首启弹窗，含「同意 / 不同意」，不默认勾选，不同意可退出；弹窗内链接到完整政策。
- **验收**：[审核指南 7.5](https://developer.huawei.com/consumer/cn/doc/app/50104) 三条合规要求（有同意选项、政策声明权限与目的、未同意前不收集/不申请权限）；未同意前不触发 `READ_PASTEBOARD`（当前只在打开会话时申请，已满足）。

### 3.6 READ_PASTEBOARD 用途文案规范化

- **改动**：`app/entry/src/main/resources/base/element/string.json:3` 新增专用 `clipboard_reason`（不要复用 `module_desc`），`module.json5:18` 改为 `reason: '$string:clipboard_reason'`；文案 ≤500 字符，写明：仅纯文本、仅前台、仅会话打开时读取、不采集不上传第三方、不持久化、可在系统设置关闭、拒绝后仅影响本机→远端同步。
- **验收**：AGC 隐私说明自动填充/手填与该文案一致；权限使用场景视频脚本覆盖同一表述；中文表述统一。

### 3.7 隐私政策文档初稿

- **改动**：新增 `docs/privacy-policy.md`（可由静态 HTML 直接发布或作为托管文案底稿）。建议章节：开发者信息与联系方式、我们处理哪些信息（剪切板、连接配置与凭据的本地存储、网络目标）、处理目的与方式、权限清单与用途、共享/第三方 SDK（无）、存储与安全（asset 密钥库、应用沙箱）、用户权利与撤回同意、未成年人、变更与生效日期、GPL/开源声明。
- **验收**：按 [50128 结构](https://developer.huawei.com/consumer/cn/doc/app/50128) 自检；覆盖 [审核指南 7.1/7.2/7.3](https://developer.huawei.com/consumer/cn/doc/app/50104)；与应用内展示逐字一致；不含「最佳/官方」等违规词。

### 3.8 日志收敛

- **改动**：检查所有 `hilog.info/warn` 是否可能在会话期间周期打印；`TianguanPerf`/`TianguanPoll` 已降为 debug（`pages/Remote.ets`），建议 release 下用统一开关（或 `hilog.isLoggable`）彻底关闭性能打点；确认主机、端口、用户名、密码、剪贴板内容不进入日志（当前 native 侧有 `%{public}s` 的普通告警，需逐条确认不含敏感值）。
- **验收**：`hilog -L I,W,E,F` 抓 12 秒 0 行周期日志；全链路（连接/首帧/输入/剪贴板/RDP）日志中无凭据与剪贴板明文。

### 3.9 代码混淆（可选）

- **改动**：`app/entry/build-profile.json5` 的 `buildOption` 增加 `arkOptions.obfuscation.ruleOptions.enable: true`  + `obfuscation-rules.txt`；**必须保留**与原生 JSON 契约相关的属性名（`state/error/errorCode/width/height/generation/cursor/clipboard/perf/pf/...`），否则会破坏 `SessionApi` 的解析。
- **验收**：release 构建成功；连接、首帧、输入、双向剪贴板、RDP 全链路真机回归通过；`mapping` 文件留存。
- **说明**：官方只规定混淆能力与限制（[ArkGuard](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/source-obfuscation)），**未找到**应用市场强制混淆的依据；GPL 项目源码公开，混淆收益有限，可不做——但需在发布说明里记录决定。

### 3.10 `compatibleSdkVersion`（产品决策，影响面最大）

- **改动**：`app/build-profile.json5:3` 的 `compatibleSdkVersion`（及必要时 `targetSdkVersion`）从 `26.0.0` 下调到实际可支持的最低版本（例如 `24` / `23`，对应 HarmonyOS 6.1.1/6.1.0）；需逐项核对 API 使用（`@kit.AppGalleryKit` privacyManager 19+、multiton、XComponent/EGL 等）。
- **验收**：在 API 23/24 真机安装启动，VNC + RDP 主链路通过；若坚持 26.0.0，则在 AGC/审核说明中明确「仅支持 HarmonyOS 7.0+」，并接受可安装设备极少、审核可能质疑「未适配主流终端」的后果。
- **依据**：[build-profile 字段说明](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile-app)（`compatibleSdkVersion` = 可安装的最低 SDK，低于该版本的设备不能安装）；[审核指南 3.1](https://developer.huawei.com/consumer/cn/doc/app/50104)（需适配华为主流终端）。

### 3.11 IPv6 支持

- **改动**：连接表单与 native 连接路径支持 IPv6 字面量（`[::1]:5901` 或自动解析），避免只按 IPv4 解析 host；如使用系统 `getaddrinfo` 则重点在 UI 校验与显示。
- **验收**：[审核指南 3.14](https://developer.huawei.com/consumer/cn/doc/app/50104)：在 IPv6-only 环境或 IPv6 字面量下完成 VNC 连接与首帧。

### 3.12 文档同步

- **改动**：`README.md:9,11`、`app/README.md:3,5` 仍有「RDP 未实现」「ACL 声明保持移除」「启动自动跑诊断」等过期描述；按 HEAD 实际状态更新，并补充隐私/许可/上架章节。
- **验收**：README 与代码/包内容一致；不再出现与当前 `module.json5` 冲突的权限说明。

### 3.13 （可选）按设备拆分 HAP，规避手机 ACL 风险

- **改动**：把 `READ_PASTEBOARD` 只放进声明 `2in1` 的 HAP；手机 HAP 不声明该受限权限，手机端本机→远端改为「提示使用远端粘贴/暂不支持」；同一 App Pack 内多 HAP 按设备分发。
- **验收**：手机 HAP 的 `requestPermissions` 不含 `READ_PASTEBOARD`；PC/2in1 HAP 保留；两端安装均不报 9568289；AGC 审核不再要求手机侧剪贴板场景视频。
- **代价**：构建/资源/模块结构调整较大。用户已决定全量发布；若手机侧 `READ_PASTEBOARD` 审核被拒，再评估本方案（见 4.2）。

---

## 4. 风险与不确定项

### 4.1 远程桌面/远程控制是否需额外资质？——未发现要求，风险低

- **官方资质清单**：逐项检索 [应用资质审核要求](https://developer.huawei.com/consumer/cn/doc/app/80301)，**没有**远程桌面/远程控制/远程协助类目。
- **审核指南「部分类型应用注意事项」**：列了直播、保健、盲盒、定位、数据恢复、插件/遥控器、网赚、贷款、地图等 28 类，**没有**远程桌面（[50104 第 12 章](https://developer.huawei.com/consumer/cn/doc/app/50104)）。
- **不收录清单**：46 条中无远程桌面相关（[50104 第 13 章](https://developer.huawei.com/consumer/cn/doc/app/50104)）。
- **在架先例**：AppGallery 搜索「远程桌面」可见 ToDesk、向日葵远程控制、UU远程、时代云远程桌面（支持 RDP/SSH/VNC）等，均归 Tools（[检索页](https://appgallery.huawei.com/search/%E8%BF%9C%E7%A8%8B%E6%A1%8C%E9%9D%A2)，2026-10-26）。
- **结论**：类目本身不构成准入障碍；但官方分类表中**没有**远程桌面二级类目，最终二级类目需在 AGC 下拉中确认或问华为。
- **需注意的两条通用规则**：审核指南 2.3 禁止「监听剪切板」式恶意行为、2.4 禁止「被远程攻击者控制手机」；天关是**主控端客户端**（不控制本机、不做后台隐藏监听），但剪贴板读取必须有明确用户授权、前台、可见，文案与演示视频要把这一点讲清楚。
- **未来若做被控端**：会触发 `ohos.permission.CONTROL_DEVICE`（26.0.0，仅 PC/2in1，面向远程登录器被控端）等受限权限，门槛显著提高（[受限开放权限](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/restricted-permissions)）。

### 4.2 READ_PASTEBOARD 审核风险——最高优先级不确定项

- **官方规则**（[受限开放权限](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/restricted-permissions#ohospermissionread_pasteboard)）：
  - PC/2in1：**应用均可申请**；首次申请系统默认授予「允许」，用户可在设置修改；
  - 其他设备：仅限银行卡号、口令、文档编辑、系统输入法、使用开源框架且无法用粘贴控件等场景；
  - AGC 审核约 3 个工作日；上架时按使用场景复核，不符则驳回；
  - 支持设备含 Phone，但「可申请场景」不含远程桌面。
- **天关风险**：同一 HAP 声明 phone+2in1（`module.json5:8,17`），若手机上架，审核员按手机场景判断 → **大概率不符合**；即使调试 ACL 已批，发布审核仍会按场景重审。
- **缓解方案（用户已决定全量发布，按此排序）**：
  1. **全量发布（已决定）**：单包声明 phone + 2in1，AGC 支持设备全选；`READ_PASTEBOARD` 手机侧是已知风险，被拒再拆包或改粘贴控件。发布后只能增加设备、不能删除，因此审核驳回后仍可在原包基础上调整。
  2. **拆 HAP**：2in1 HAP 声明权限，手机 HAP 不声明（见 3.13），作为被拒后的第一退路。
  3. 手机上架但放弃自动剪贴板：移除手机侧读取，仅保留「远端→本机」写入（写入不需要该权限），本机→远端改为远端粘贴/手动输入。
  4. 坚持手机上架 + 自动剪贴板：提交详尽场景说明 + 使用场景视频，尝试以「使用开源框架（TigerVNC/FreeRDP），无法用粘贴控件实现该转发」为理由申请——**这是解释性尝试，未找到官方明确支持远程桌面场景的依据，不保证通过**。

### 4.3 compatibleSdkVersion=26.0.0 的兼容性风险

- 官方定义：低于 `compatibleSdkVersion` 的设备**不能安装**（[build-profile 字段说明](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile-app)）；API 26 对应 HarmonyOS 7.0（[HarmonyOS 7（API 26）](https://developer.huawei.com/consumer/cn/features/)）。
- 现状：`compile/compatible/target` 全为 `26.0.0`（`app/build-profile.json5:3`），意味着只有 HarmonyOS 7.0+ 能装。第三方媒体引用官方数据称 2026-09 时 6.1.1（API 24）占比约 86.8%、7.0 尚在 Beta（[报道](https://finance.sina.com.cn/tech/digi/2026-09-09/doc-inircrns9288016.shtml)，非官方页面，仅作量级参考）。
- 风险：审核指南 3.1 要求适配华为主流终端；仅支持最新系统的应用可能被质疑，且实际可分发用户极少。
- **未找到**官方「上架应用最低 compatibleSdkVersion」的硬性数值（发布准备只要求 API Level ≥10），所以这是产品/审核判断，不是格式硬门槛。

### 4.4 GPL-3.0-or-later 与商店分发是否冲突？——未发现冲突，但有条件

**结论：GPL 允许通过 AppGallery 分发；未找到华为禁止 GPL/开源应用的条款；真正的义务在 GPL 侧而不是商店侧。**

- **依据与推理**：
  1. [华为开发者服务协议 4.5](https://developer.huawei.com/consumer/cn/doc/start/agreement-0000001052728169)：华为提供的开源组件受其许可证约束；协议未禁止开发者提交开源应用。
  2. [AGC 服务协议 3.4](https://developer.huawei.com/consumer/cn/doc/20214)：要求每个产品提供最终用户协议（EULA）并提交副本，华为不审批其内容 → 可以把 GPL 作为许可声明，不写与 GPL 冲突的限制。
  3. AGC 服务协议 3.1：开发者授权华为在华为平台展示/分发产品——这是**版权人授予的非独占分发授权**，与 GPL 不冲突（GPL 不禁止通过商店分发）。
  4. 审核指南 9 只要求知识产权权属清晰、不得侵权，未禁止 copyleft（[50104 第 9 章](https://developer.huawei.com/consumer/cn/doc/app/50104)）。
  5. 未找到华为针对 GPL/开源上架的专门规则 → 若审核提出疑问，需向华为确认（标注为「未找到官方依据」）。
- **必须同时满足的条件**：
  - **源码提供**：GPLv3 分发目标代码须提供对应源码（含修改与构建材料）。当前仓库有源码，但应用内/详情页没有源码获取入口；TigerVNC 为 GPL-2.0-or-later 且带 `ohos-thread.patch`，FreeRDP 为 Apache-2.0（需保留 NOTICE），libc++ 等也要保留声明（见 `docs/licensing.md`）。建议在关于页放源码仓库地址 + 许可清单。
  - **EULA 不得附加限制**：最终用户协议不能写「禁止反向工程/禁止再分发」等与 GPL 冲突的条款；GPL 本身就是许可。
  - **权属**：软著/版权证书的著作权人须与开发者一致；GPL 不阻止登记软著，但若有多位贡献者且无 CLA，登记与「完整权利或充分授权」的声明需谨慎（审核指南 9、[AGC 3.4](https://developer.huawei.com/consumer/cn/doc/20214)）。
  - **不得暗示华为官方**：审核指南 9.4/9.5；`README.md:27` 已声明「独立社区项目，非华为官方」——上架文案也要保持同样口径。
- **未找到官方依据的延伸问题**（需向华为确认）：华为是否接受 GPL 作为 EULA 主体；是否需要额外提交开源许可清单；「电子版权证书」对 GPL 项目是否有特殊要求。

### 4.5 APP 备案的执行不确定性

- 官方只说「在接入商备案、平台选鸿蒙、填鸿蒙包名」（[50130](https://developer.huawei.com/consumer/cn/doc/app/50130)），没有说明**没有自有服务端的联网客户端**如何备案。
- 天关不属于「单机应用」（需要联网连接 VNC/RDP），也不属于「境外应用」（假设主体/服务器在境内），因此不能简单豁免。
- 实操上备案通常需要接入服务（域名/服务器）信息，可能要求开发者先有一个可备案的域名与云服务器；这与「纯客户端」的产品形态不匹配。
- **建议**：第一步就向拟用接入商（如华为云备案客服）确认「无自有服务端的远程桌面客户端如何备案/是否可备案」；同时准备一个项目官网域名作为备案载体。未确认前不要把备案排在最后。

### 4.6 隐私标签与「剪切板是否算收集」

- [50128](https://developer.huawei.com/consumer/cn/doc/app/50128) 把「剪切板」列入个人信息标准表述；天关读取剪贴板并发送到**用户指定的远端服务器**，不发送给开发者、不落盘。
- 「是否涉及个人信息收集」在 AGC 隐私标签里填「否」还是「是」，官方没有针对「本地处理 + 用户自有服务端」的明文判定 → **需向华为确认**；保守做法是按「是」披露剪切板（目的：用户主动发起的剪贴板同步），但可能引出「双清单」要求（[审核指南 7.25](https://developer.huawei.com/consumer/cn/doc/app/50104)）。
- 无论标签怎么填，隐私政策里都必须写剪切板（[50104 7.2](https://developer.huawei.com/consumer/cn/doc/app/50104)）。

### 4.7 演示视频/测试环境的可用性

- 审核指南 1.21 要求依赖特殊环境的应用提供演示视频；12.8 要求在描述中写明适用范围。
- 需要一台审核期间可访问的 VNC 与 RDP 服务器（公网或可复现的录屏）。这是**用户侧资源**，不是代码能解决的；如果只给录屏，须确保视频能覆盖连接、认证、首帧、键鼠、剪贴板。

### 4.8 release 包无法本地安装验证

- Profile 分发类型为 `app_gallery` 且证书为 release 的 HAP，官方 `bm` 明确禁止安装（错误码 9568402，[bm 工具](https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/bm-tool)）。
- 因此「release 签名 → hdc 安装 → 真机跑通」这条路走不通；验收必须设计成：`verify-app` 签名校验 + AGC 合法性检测 + AGC 内部测试/邀请测试分发 + 商店安装验证。

### 4.9 其他「未找到官方依据」项

- 鸿蒙应用是否有「最低 targetSdkVersion」硬性数值（发布准备只写 API Level ≥10）。
- 远程桌面类目在 AGC 下拉中的确切二级分类。
- 华为对 GPL 作为 EULA 的明确接受度。
- 启动页 `startWindowIcon` 的官方尺寸规范（设计指南只规定「图标 + 背景色」，未找到像素要求）；仅从 restool 源码查到 base 目录的告警阈值为 144 px，不是硬性上限。
- HAP 是否必须提供 x86_64 版本（当前只打 arm64-v8a；未找到 2in1 x86 设备的官方要求）。
- 个人开发者上架远程桌面类应用是否有额外限制（资质清单未列，但账号 FAQ 提到部分类目限制，未见远程桌面）。

---

## 5. 下一步建议

**第 1 步已完成（2026-10-26）**：AGC 应用已创建，APP ID `691761958035659012`，包名固定 `cn.alfadb.tianguan`，支持设备全选，默认语言简体中文；首发版本定为 `1.0.0 / 100`，本地发布构建入口 `scripts/build-release.sh` 已落地（详见 [release-pipeline.md](release-pipeline.md)）。

- **紧接着并行启动（长周期，越早越好）**：
  1. 发布 Profile 的 `READ_PASTEBOARD` 受限 ACL 申请（约 3 个工作日）；
  2. 软著登记（手机必选版权证书，数十个工作日）；
  3. APP 备案（先向接入商确认「无自有服务端的联网客户端」怎么备案）。
- **已确定的产品决策**（影响 ACL、软著、HAP 拆分、compatibleSdkVersion）：
  - 全量发布：手机 / 平板 / 智慧屏 / PC / 2in1；不做 2in1 裁剪。
  - `compatibleSdkVersion` 保持 API 26。
  - `READ_PASTEBOARD` 手机侧为已知风险，若审核被拒再拆包或改用系统粘贴控件。

---

## 附录 A：本次审计读取的证据

| 证据 | 位置 | 结论 |
|---|---|---|
| 应用元信息 | `app/AppScope/app.json5:2` | bundleName `cn.alfadb.tianguan`；`versionCode 1`；`versionName 0.0.1`；icon `$media:layered_image`；label `$string:app_name` |
| 工程构建配置 | `app/build-profile.json5:3-4` | 无 `signingConfigs`；compile/compatible/target 均 `26.0.0`；`buildModeSet: debug/release` |
| 模块构建配置 | `app/entry/build-profile.json5:1` | `buildOption: {}`（无 arkOptions/混淆）；deviceType `['2in1','phone']` |
| 模块清单 | `app/entry/src/main/module.json5:8-19` | deviceTypes 2in1+phone；INTERNET / PREPARE_APP_TERMINATE / READ_PASTEBOARD（reason 复用 `module_desc`，usedScene inuse） |
| 能力/启动图 | `module.json5` 两个 ability 的 `icon`/`startWindowIcon`、`:35,54`（orientation auto_rotation）、`:56`（multiton） | 两个 UIAbility 的 `icon` 与 `startWindowIcon` 均引用 `$media:layered_image` |
| 字符串 | `app/entry/src/main/resources/base/element/string.json:2-3` | 仅 `app_name=天关`、`module_desc=用于在本机复制内容后自动同步到远程会话` |
| 图标资源 | `app/AppScope/resources/base/media/{background.png,foreground.png,layered_image.json}` | 背景/前景 1024×1024；背景为 RGB 无 alpha；entry 不再重复放置 |
| 图标构建产物 | `app/entry/build/default/outputs/default/entry-default-unsigned.hap` 内 `resources/base/media/*` | background/foreground 均 512×512 RGBA（背景 alpha 254）；`layered_image.json` 保留；restool 固定 512 上限缩放 |
| 设备图标 | `bm dump -n cn.alfadb.tianguan` | `iconId=16777237`（0x01000015）、`iconPath=$media:layered_image`；PC `install -r` 后 Dock 图标正常 |
| 构建脚本 | `app/build.sh:27-29` | 只跑 `assembleHap --mode module -p buildMode=release`，输出 unsigned HAP |
| 构建产物 | `app/entry/build/default/outputs/default/entry-default-unsigned.hap` | 15,263,559 字节；`module.json`：deviceTypes 2in1/phone、installationFree false、无 debug；`pack.info`：releaseType Release、apiVersion 26 |
| 原生库 | HAP 内 `libs/arm64-v8a/libtianguan_diag.so` | 打包后 13.7MB，`readelf -S` 无 `.debug_*`（已 strip）；源 `app/entry/libs/...` 为 15.9MB 含调试段，未被打入 |
| 隐私政策 | 全仓库搜索 `privacy/隐私/隐私政策` | 无任何隐私政策文件；`docs/` 仅 licensing/napi-contract |
| 版本记录 | 搜索 `CHANGELOG` | 无 |
| 应用内入口 | `pages/` 仅 `Index.ets`、`Remote.ets`（`main_pages.json`） | 无关于/隐私/许可/联系方式入口 |
| 权限申请时机 | `pages/Remote.ets:474` | 打开远程会话时才申请剪贴板，未提前申请（符合 7.17） |
| 凭据存储 | `model/ConnectionStore.ets` | 密码存系统 asset（`asset.add/query`），非明文文件；未发现密码日志 |
| 测试钩子 | 全仓库 grep `runDiagnostics/diag-result/vnc-test` | 无命中；README 里描述的旧诊断流程已不在代码中（文档过期） |
| 许可声明 | `README.md:27,50-52`；`docs/licensing.md`；`LICENSE` | GPL-3.0-or-later；已声明非华为官方；已记录 TigerVNC/FreeRDP 许可边界 |
| 忽略规则 | `.gitignore` | 已忽略 `*.p12/*.cer/*.p7b/*.csr/signing/` 等签名材料 |

## 附录 B：官方文档索引（本次引用）

1. 应用审核指南：<https://developer.huawei.com/consumer/cn/doc/app/50104>
2. 应用审核 Checklist：<https://developer.huawei.com/consumer/cn/doc/app/50170>
3. 应用资质审核要求：<https://developer.huawei.com/consumer/cn/doc/app/80301>
4. APP 核准（APP 备案）指引：<https://developer.huawei.com/consumer/cn/doc/app/50130>
5. 发布 HarmonyOS 应用（总览）：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-0000002271695230>
6. 发布准备工作：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-prepare-0000002306311921>
7. 上传软件包 / 合法性检测 / 上架自检：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-upload-pkg-0000002277983368>
8. 配置支持设备：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-devicetype-0000002271592112>
9. 配置本地化基础信息（名称、图标）：<https://developer.huawei.com/consumer/cn/doc/App/agc-help-release-app-nameicon-0000002306265009>
10. 配置应用分类、标签：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-class-tag-0000002271695234>
11. 配置内容分级：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-rating-0000002313511005>
12. 配置隐私说明：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-desc-0000002313477969>
13. 配置隐私声明：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-state-0000002278878296>
14. 配置隐私标签：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-privacy-tag-0000002316420993>
15. 配置版权信息：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-copyright-0000002278981450>
16. 素材规范：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-app-visual-asset-spec-0000002277607976>
17. 图标资源规范：<https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/application-icon-0000001953444009>
18. 配置应用图标和名称（分层图标）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/layered-image>
19. 受限开放权限：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/restricted-permissions>
20. 开放权限（系统授权）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/permissions-for-all>
21. 申请发布证书：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-cert-0000002283336729>
22. 申请发布 Profile：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-add-releaseprofile-0000001914714796>
23. 发布应用（DevEco / .app）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-publish-app>
24. 隐私管理服务（托管弹窗/查询链接）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/store-privacy>
25. 隐私政策链接提交及内容规范 FAQ：<https://developer.huawei.com/consumer/cn/doc/app/50128>
26. 个人开发者实名认证：<https://developer.huawei.com/consumer/cn/doc/start/ht-idrna-0000001200848143>
27. 鸿蒙应用分类：<https://developer.huawei.com/consumer/cn/doc/app/classify-0000001960172909>
28. 工程级 build-profile.json5 字段：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile-app>
29. 计算机软件著作权证书说明：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/50111-02>
30. ArkGuard 混淆：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/source-obfuscation>
31. 华为开发者服务协议：<https://developer.huawei.com/consumer/cn/doc/start/agreement-0000001052728169>
32. 华为 AppGallery Connect 服务协议：<https://developer.huawei.com/consumer/cn/doc/20214>
33. bm 工具（release + app_gallery 禁止安装）：<https://developer.huawei.com/consumer/cn/doc/doccenter-capabilities/bm-tool>
34. HarmonyOS 7（API 26）特性页：<https://developer.huawei.com/consumer/cn/features/>
