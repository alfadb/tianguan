# 天关 / Tianguan（TG）

面向 HarmonyOS 的 VNC + RDP 远程桌面客户端计划，优先支持 HarmonyOS PC，关注友好的界面与连接管理。

Tianguan (TG) is a planned community VNC and RDP client for HarmonyOS, with HarmonyOS PC as the first target.

## 当前状态

**pre-implementation（实现前准备）**：本仓库目前只有文档和许可材料，没有实现代码、可运行 App 或可用安装包，也没有可执行的构建流程。以下内容均为计划与验收目标，不代表功能已实现或兼容性已验证。

项目名称为「天关」，英文名 Tianguan，简称 TG；GitHub 仓库为 [alfadb/tianguan](https://github.com/alfadb/tianguan)。

名称沿用「太一」体系的星官命名意象。[《晋书·卷十一》](https://zh.wikisource.org/zh-hans/晉書/卷011)记载：“天关一星，在五车南，亦曰天门，日月之所行也，主边事，主关闭。”本项目借「天关」表达通往远端桌面的入口；“远端入口”是现代寓意。

这是独立社区项目，非华为官方项目，与华为不存在官方隶属或背书关系。

## 产品方向与候选内核

- 平台：HarmonyOS PC 优先，其他设备形态留待后续评估。
- 体验：提供易用的连接管理、清晰的连接状态与友好的远程桌面界面。
- VNC：TigerVNC 是候选协议内核；首版目标包括 RA2、RA2_256 和 X509Vnc，实际可用性须经移植与互通验证。
- RDP：FreeRDP 是候选协议内核；目标包括 NLA/TLS 连接与 Unicode 输入。
- 中文输入：须对 VNC 与 RDP 分别验证输入链路，包括本地输入法提交、协议编码与远端应用接收；不能从某一协议的结果推断另一协议支持情况。

目前尚未引入 TigerVNC、FreeRDP 或其他第三方实现，候选方案不构成依赖已集成的声明。

## 分阶段验收目标

以下阶段均未完成，需保留设备、版本、配置与测试结果作为验收证据：

1. **OHOS 交叉编译与真机运行**：验证候选内核及最小调用链能够使用适用的 OHOS 工具链交叉编译，并在目标 HarmonyOS PC 真机运行。
2. **VNC RA2_256 主线**：连接现有 TigerVNC 服务端，完成 RA2_256 握手，显示远端画面并验证键盘、鼠标交互；同时记录 RA2 的互通验证结果。
3. **VNC X509Vnc**：验证 X509Vnc 连接与证书验证，包括可信证书成功连接，以及不可信、过期或身份不匹配证书的处理。
4. **RDP 安全连接与输入**：验证 NLA/TLS 连接、远端画面和键鼠交互，并验证 Unicode 与中文输入在远端应用中的实际结果。

## 许可与贡献

本项目自有代码采用 **GNU General Public License v3.0 or later（GPL-3.0-or-later）**：你可以依照 GPL 第 3 版，或自由软件基金会发布的任何后续版本，使用、修改和分发本项目自有代码。

[LICENSE](LICENSE) 保留 GPLv3 官方完整原文；“or later”的项目授权选择由本节与[许可说明](docs/licensing.md)明确。上游文件仍须遵循各自许可，不能据此推定所有第三方文件都采用本项目许可。

参与方式见 [CONTRIBUTING.md](CONTRIBUTING.md)。
