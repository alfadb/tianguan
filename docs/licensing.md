# 许可说明

## 自有代码的授权

天关 / Tianguan（TG）的自有代码采用 GNU General Public License v3.0 or later，SPDX 标识为 `GPL-3.0-or-later`。接收者可以选择遵循 GPL 第 3 版，或自由软件基金会发布的任何后续版本。

选择 GPLv3 是为了让分发的衍生作品继续提供相应源码与修改自由，并为评估可能采用不同许可的协议内核保留兼容空间。GPLv3 的强著佐权义务须在实际组合与分发方式明确后落实。

根目录 [LICENSE](../LICENSE) 是从 SPDX 官方维护的许可数据仓库下载的 GPLv3 完整原文，未修改许可正文；项目的“or later”选择由本文件和 [README](../README.md) 明确。实际下载来源：<https://raw.githubusercontent.com/spdx/license-list-data/main/text/GPL-3.0-or-later.txt>。GNU 原文入口：<https://www.gnu.org/licenses/gpl-3.0.txt>。

## 候选上游与兼容边界

TigerVNC 已进入隔离实验评估，使用官方 v1.15.0，固定 commit 为 `8bdf8734ce12c8ed3c232496fc8a4ccdb04636b8`。[源码获取脚本](../scripts/fetch-tigervnc.sh)将原始源码保留在 `.work/sources` 外部 checkout，未将上游代码 vendor 入仓库；依赖与构建产物置于 `.work` 本地缓存。原始 checkout 保持 clean，host 正式目标使用原源码；OHOS 目标编译的是含明确变更的 `Thread.cxx` 构建副本，不能称为完全未修改上游实现。此评估尚不构成最终内核选型或 HarmonyOS 应用集成。FreeRDP 已集成，版本 3.9.0，以静态链接方式编译进应用。

唯一平台补丁为 [experiments/vnc-core/ohos-thread.patch](../experiments/vnc-core/ohos-thread.patch)：官方 `Thread` 析构引用 SDK 缺失的 `pthread_cancel`，构建副本以 `TG_OHOS_NO_PTHREAD_CANCEL` 将仍在运行时的析构分支改为 trap；正常 `DecodeThread` 的 `stop()` / `wait()` 协作退出保持。该宏仅用于 OHOS 正式构建及 host 专用适配测试，原版权声明保留。分发相应二进制时，须按适用许可提供与实际编译内容对应的源码，包括此补丁、编译源变更及所需构建材料，并保留适用的版权、许可与 NOTICE 声明。

已完成的有界核对及证据见 [VNC 实验说明](../experiments/vnc-core/README.md#有界许可核对)：检查 TigerVNC 的 `LICENCE.TXT`，并依据实际编译依赖检查 131 个上游源码/头文件的文件头，其中 126 个明确为 GPL-2.0-or-later；其余五个输入涉及 The Open Group / Digital Equipment Corporation 条款、生成表及 DES 来源与授权证据，须保留各自声明并补核。实际链接依赖的核对限于本机 Debian copyright 元数据及相关许可段落，未逐文件审计这些依赖的完整源码。pixman 构建另保留上游 `COPYING`，也不代表完整文件级审计。

这些记录仅支持本地可行性实验，尚未发布二进制，不是完整产品分发审计，也不构成对全部上游文件、最终组合方式或 SDK / 商店条件的审核结论。正式引入与分发前的文件级审查要求仍须落实。

Apache License 2.0 与 GPLv3 兼容：在满足 Apache-2.0 的版权、许可、NOTICE 等要求的前提下，可以将相关代码与 GPLv3 代码组合并按 GPLv3 的适用条件分发。Apache-2.0 与 GPL-2.0-only 不兼容；不能据此将仅限 GPL 第 2 版的代码直接纳入这样的组合。带有“or later”选项的代码须按具体授权判断是否可选择兼容版本。

上述兼容性不意味着上游文件被统一改成 GPL-3.0-or-later。实际引入任何上游实现前，必须按选定版本开展文件级许可审计，核对例外、双重授权、第三方嵌入代码和传递依赖；保留各自的版权声明、许可文本、NOTICE/NOTICES 及其他要求，并记录来源与修改。未来引入时再按实际内容建立相应清单。

可参考 GNU 的[许可兼容说明](https://www.gnu.org/licenses/license-list.html#apache2)及 Apache 的[GPL 兼容说明](https://www.apache.org/licenses/GPL-compatibility.html)。

## Sunshine / Moonlight 上游组件

Sunshine 串流（应用里的第三种协议）引入两个上游：moonlight-common-c 与 libopus。两者都由 [scripts/build-sunshine-deps.sh](../scripts/build-sunshine-deps.sh) 用固定 OHOS SDK 交叉编译成 AArch64 静态 PIC 归档，安装到 `.work/sunshine-deps/ohos/prefix` 后静态链入 `libtianguan_diag.so`；上游源码不 vendor 入仓库，由脚本按固定提交拉到 `.work` 外部检出并校验。

- **moonlight-common-c**：许可为 GPL-3.0（仓库内只有 `LICENSE.txt` 的 GPLv3 全文，未见「or later」声明；文件末尾另附 GPLv3 第 7 条下的附加许可「App Store Distribution Exception」——允许通过与 GPL 不兼容的应用商店分发，条件是同一源码也通过无此限制的渠道按 GPL 提供），固定提交 `b4ee1cea3d19e13c0b37c26585e6b1ee174effcb`（2026-11-22）。负责 RTSP 握手、RTP 收流与音视频解包、FEC 纠错、控制通道与输入通道；HTTP 控制面由本项目自己实现。源码获取：[scripts/fetch-moonlight.sh](../scripts/fetch-moonlight.sh) 从公开上游检出到 `.work/moonlight-common-c`，并校验提交里锁定的两个子模块：enet（MIT，cgutman/enet fork，`98dbb897ac4a7208038e0782cddedf89dcf0723a`）与 nanors（MIT，`a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29`）。
- **libopus**：v1.6.1，BSD-3-Clause（tag `v1.6.1`，commit `22244de5a79bd1d6d623c32e72bf1954b56235be`），只用于 Sunshine 音频的 Opus 多流解码。源码缓存在 `.work/sunshine-deps/src/opus-1.6.1`，同样不 vendor。
- **与项目 GPL-3.0-or-later 的兼容性**：moonlight-common-c 的 GPL-3.0 可与 GPL-3.0-or-later 代码组合分发，但因为它只给到第 3 版，**含它的组合整体只能按 GPL-3.0 分发，不能按 GPL 的更高版本**；本项目自有代码的 or-later 选择不变。enet / nanors 的 MIT 与 libopus 的 BSD-3-Clause 都与 GPL 兼容，不改变其原有版权、许可与免责声明。
- **保留义务与源码获取**：分发二进制时须保留各组件版权与许可声明——moonlight-common-c 的 `LICENSE.txt`、`enet/LICENSE`、`nanors/LICENSE`、libopus 的 `COPYING` 与 `LICENSE_PLEASE_READ.txt`，以及 opus 内 KISS FFT 文件（`celt/mini_kfft.c`）自带的版权头——并按 GPL 第 6 条向接收者提供与实际二进制对应的完整源码与构建材料（固定提交的 moonlight-common-c、enet、nanors、libopus 源码与 [scripts/build-sunshine-deps.sh](../scripts/build-sunshine-deps.sh)）。源码获取方式与天关自有代码相同：公开仓库 <https://github.com/alfadb/tianguan>，或发邮件至 support@alfadb.cn 索取对应版本。

### 文件级核对结果（2026-11-27）

范围与方法：在固定提交上核对（`git rev-parse HEAD` 与 `git submodule status --recursive` 分别为上文三个提交，libopus checkout 的 HEAD 为 `22244de5…`；四个工作区均无本地改动）。先读各仓库根的许可文件，再对指定目录的每个 `*.c` / `*.h` 逐文件检索许可声明文本（GPL 头、MIT 的 `Permission is hereby granted`、BSD-3 的 `Redistribution and use in source and binary forms`）与版权行。核对对象是源码树里的文件头；不构成法律意见，也不覆盖编译选项、工具链、SDK 或商店条件。

| 组件 | 文件数 | 许可 | 例外 |
| --- | --- | --- | --- |
| moonlight-common-c | `src/` 36 个 `.c` / `.h` | GPL-3.0（根 `LICENSE.txt`，含第 7 条 App Store 附加许可） | 未发现许可例外（没有其它许可或第三方代码声明）；但 36 个文件都没有文件头许可或版权行，`moonlight-common-c is free software` 头 0/36，许可依据只有仓库根 `LICENSE.txt` |
| enet | 18 个 `.c` / `.h` | MIT（`enet/LICENSE`） | 未发现许可例外；18 个源文件均无文件头，MIT 文本另见 `enet/docs/license.dox` |
| nanors | 23 个 `.c` / `.h`（库 16、测试 7） | MIT（`nanors/LICENSE`） | 未发现许可例外；23 个源文件均无文件头；`nanors/deps/obl` 无独立许可文件，随 nanors 仓库按 MIT |
| libopus 1.6.1 | 199 个 `.c` / `.h`（`include/` 6、`src/` 23、`celt/` 98、`dnn/` 72） | BSD-3-Clause（`COPYING`；无 `LICENSE` 文件，另有 `LICENSE_PLEASE_READ.txt`） | 183 个文件带 BSD-3 条款头，16 个没有（见下）；未发现非 BSD-3 的许可文件 |

`libopus` 的 16 个无 BSD-3 文件头的文件：

- **同一许可、独立来源（1 个）**：`celt/mini_kfft.c` 是 KISS FFT（Copyright (c) 2003-2010 Mark Borgerding），文件头自带 `SPDX-License-Identifier: BSD-3-Clause` 并指向其自身上游；分发时保留该文件头即可，许可类型仍是 BSD-3-Clause。
- **自动生成的表（8 个）**：`src/mlp_data.c`、`src/tansig_table.h`、`celt/static_modes_fixed.h`、`celt/static_modes_fixed_arm_ne10.h`、`celt/static_modes_float.h`、`celt/static_modes_float_arm_ne10.h`、`dnn/lpcnet_tables.c`、`dnn/tansig_table.h`。文件头只有「automatically generated」说明，没有许可正文；作为发行包的一部分受 `COPYING` 约束，没有单独许可。
- **手写头、测试与示例（7 个）**：`dnn/pitchdnn.h`、`dnn/pitchdnn.c`、`dnn/lpcnet_private.h`、`dnn/common.h`、`dnn/test_vec.c`、`dnn/lossgen_demo.c`、`dnn/adaconvtest.c`。这些文件没有文件头声明，同样按发行包整体受 `COPYING` 约束；其中 `dnn/` 目录在本构建里没有编入 `libopus.a`（`OPUS_OSCE` / `OPUS_DRED` 关闭），但仍按源码树做了核对。

补充核对：`silk/` 目录（不在最初指定的四个目录内）188 个 `.c` / `.h` 全部带 BSD-3 条款头，未发现例外；`COPYING` 末尾附有 Xiph.Org、Microsoft、Broadcom 的专利许可指引，`LICENSE_PLEASE_READ.txt` 记录贡献者授权与 IETF IPR 声明，这些是保留 notice 的一部分，不是额外的版权许可条件。

以上只是 2026-11-27 在固定提交上的源码文件头核对：未审查编译选项对许可的影响，未逐文件核对编译清单之外的文件，也未完成工具链、SDK、商店与签名条件的审核。正式分发前仍需按实际二进制复核实际编译的文件清单与 notice 组合。

## 尚待核对的分发条件

在采用鸿蒙 SDK、设备能力或应用商店分发之前，还需核对届时适用的 SDK 授权、签名与商店条款是否能与 GPL 的源码提供、再分发等义务同时满足。此项审核尚未完成，当前文档不表示已经获得任何平台或商店的合规确认。
