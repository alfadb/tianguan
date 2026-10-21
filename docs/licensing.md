# 许可说明

## 自有代码的授权

天关 / Tianguan（TG）的自有代码采用 GNU General Public License v3.0 or later，SPDX 标识为 `GPL-3.0-or-later`。接收者可以选择遵循 GPL 第 3 版，或自由软件基金会发布的任何后续版本。

选择 GPLv3 是为了让分发的衍生作品继续提供相应源码与修改自由，并为评估可能采用不同许可的协议内核保留兼容空间。GPLv3 的强著佐权义务须在实际组合与分发方式明确后落实。

根目录 [LICENSE](../LICENSE) 是从 SPDX 官方维护的许可数据仓库下载的 GPLv3 完整原文，未修改许可正文；项目的“or later”选择由本文件和 [README](../README.md) 明确。实际下载来源：<https://raw.githubusercontent.com/spdx/license-list-data/main/text/GPL-3.0-or-later.txt>。GNU 原文入口：<https://www.gnu.org/licenses/gpl-3.0.txt>。

## 候选上游与兼容边界

TigerVNC 已进入隔离实验评估，使用官方 v1.15.0，固定 commit 为 `8bdf8734ce12c8ed3c232496fc8a4ccdb04636b8`。[源码获取脚本](../scripts/fetch-tigervnc.sh)将原始源码保留在 `.work/sources` 外部 checkout，未将上游代码 vendor 入仓库；依赖与构建产物置于 `.work` 本地缓存。原始 checkout 保持 clean，host 正式目标使用原源码；OHOS 目标编译的是含明确变更的 `Thread.cxx` 构建副本，不能称为完全未修改上游实现。此评估尚不构成最终内核选型或 HarmonyOS 应用集成。FreeRDP 仍为候选，尚未引入。

唯一平台补丁为 [experiments/vnc-core/ohos-thread.patch](../experiments/vnc-core/ohos-thread.patch)：官方 `Thread` 析构引用 SDK 缺失的 `pthread_cancel`，构建副本以 `TG_OHOS_NO_PTHREAD_CANCEL` 将仍在运行时的析构分支改为 trap；正常 `DecodeThread` 的 `stop()` / `wait()` 协作退出保持。该宏仅用于 OHOS 正式构建及 host 专用适配测试，原版权声明保留。分发相应二进制时，须按适用许可提供与实际编译内容对应的源码，包括此补丁、编译源变更及所需构建材料，并保留适用的版权、许可与 NOTICE 声明。

已完成的有界核对及证据见 [VNC 实验说明](../experiments/vnc-core/README.md#有界许可核对)：检查 TigerVNC 的 `LICENCE.TXT`，并依据实际编译依赖检查 131 个上游源码/头文件的文件头，其中 126 个明确为 GPL-2.0-or-later；其余五个输入涉及 The Open Group / Digital Equipment Corporation 条款、生成表及 DES 来源与授权证据，须保留各自声明并补核。实际链接依赖的核对限于本机 Debian copyright 元数据及相关许可段落，未逐文件审计这些依赖的完整源码。pixman 构建另保留上游 `COPYING`，也不代表完整文件级审计。

这些记录仅支持本地可行性实验，尚未发布二进制，不是完整产品分发审计，也不构成对全部上游文件、最终组合方式或 SDK / 商店条件的审核结论。正式引入与分发前的文件级审查要求仍须落实。

Apache License 2.0 与 GPLv3 兼容：在满足 Apache-2.0 的版权、许可、NOTICE 等要求的前提下，可以将相关代码与 GPLv3 代码组合并按 GPLv3 的适用条件分发。Apache-2.0 与 GPL-2.0-only 不兼容；不能据此将仅限 GPL 第 2 版的代码直接纳入这样的组合。带有“or later”选项的代码须按具体授权判断是否可选择兼容版本。

上述兼容性不意味着上游文件被统一改成 GPL-3.0-or-later。实际引入任何上游实现前，必须按选定版本开展文件级许可审计，核对例外、双重授权、第三方嵌入代码和传递依赖；保留各自的版权声明、许可文本、NOTICE/NOTICES 及其他要求，并记录来源与修改。未来引入时再按实际内容建立相应清单。

可参考 GNU 的[许可兼容说明](https://www.gnu.org/licenses/license-list.html#apache2)及 Apache 的[GPL 兼容说明](https://www.apache.org/licenses/GPL-compatibility.html)。

## 尚待核对的分发条件

在采用鸿蒙 SDK、设备能力或应用商店分发之前，还需核对届时适用的 SDK 授权、签名与商店条款是否能与 GPL 的源码提供、再分发等义务同时满足。此项审核尚未完成，当前文档不表示已经获得任何平台或商店的合规确认。
