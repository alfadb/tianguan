# 许可说明

## 自有代码的授权

天关 / Tianguan（TG）的自有代码采用 GNU General Public License v3.0 or later，SPDX 标识为 `GPL-3.0-or-later`。接收者可以选择遵循 GPL 第 3 版，或自由软件基金会发布的任何后续版本。

选择 GPLv3 是为了让分发的衍生作品继续提供相应源码与修改自由，并为评估可能采用不同许可的协议内核保留兼容空间。GPLv3 的强著佐权义务须在实际组合与分发方式明确后落实。

根目录 [LICENSE](../LICENSE) 是从 SPDX 官方维护的许可数据仓库下载的 GPLv3 完整原文，未修改许可正文；项目的“or later”选择由本文件和 [README](../README.md) 明确。实际下载来源：<https://raw.githubusercontent.com/spdx/license-list-data/main/text/GPL-3.0-or-later.txt>。GNU 原文入口：<https://www.gnu.org/licenses/gpl-3.0.txt>。

## 候选上游与兼容边界

TigerVNC 和 FreeRDP 目前只是候选方案，本仓库尚未包含其代码或依赖。本文不构成对其全部文件许可或最终组合方式的审核结论。

Apache License 2.0 与 GPLv3 兼容：在满足 Apache-2.0 的版权、许可、NOTICE 等要求的前提下，可以将相关代码与 GPLv3 代码组合并按 GPLv3 的适用条件分发。Apache-2.0 与 GPL-2.0-only 不兼容；不能据此将仅限 GPL 第 2 版的代码直接纳入这样的组合。带有“or later”选项的代码须按具体授权判断是否可选择兼容版本。

上述兼容性不意味着上游文件被统一改成 GPL-3.0-or-later。实际引入任何上游实现前，必须按选定版本开展文件级许可审计，核对例外、双重授权、第三方嵌入代码和传递依赖；保留各自的版权声明、许可文本、NOTICE/NOTICES 及其他要求，并记录来源与修改。未来引入时再按实际内容建立相应清单。

可参考 GNU 的[许可兼容说明](https://www.gnu.org/licenses/license-list.html#apache2)及 Apache 的[GPL 兼容说明](https://www.apache.org/licenses/GPL-compatibility.html)。

## 尚待核对的分发条件

在采用鸿蒙 SDK、设备能力或应用商店分发之前，还需核对届时适用的 SDK 授权、签名与商店条款是否能与 GPL 的源码提供、再分发等义务同时满足。此项审核尚未完成，当前文档不表示已经获得任何平台或商店的合规确认。
