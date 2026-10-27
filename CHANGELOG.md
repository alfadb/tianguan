# 更新日志

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 的风格，版本号采用[语义化版本](https://semver.org/lang/zh-CN/)。

## [1.0.0] - 2026-10-26

首个公开版本。HarmonyOS 手机 / 平板 / PC / 2in1 发布，API 26（HarmonyOS 7.0+）。

### 新增

- VNC（TigerVNC 1.15.0 内核）与 RDP（FreeRDP 3.9.0 内核）双协议远程桌面：VNC 支持 RA2_256 / RA2 / VncAuth，RDP 支持 TLS/NLA 登录、画面、键鼠与中文输入（由远端 Windows 输入法完成）。
- 多会话与多窗口：每个会话独立 socket、线程、帧缓冲、光标与剪贴板；每个会话启动独立窗口实例，Dock 窗口选择器可直接激活，系统托盘常驻（2in1）。
- RDP GFX 图形管线与 H.264/AVC 解码（FFmpeg 7.1.5 静态链接），服务端按内容在 ClearCodec 与 AVC420 之间切换。
- RDP 音频回放（rdpsnd，PCM 与 FFmpeg 可解码的压缩格式）。
- 双向剪贴板：远端 → 本机已验证；本机 → 远端需要 `ohos.permission.READ_PASTEBOARD` 授权，首次打开会话时申请，未授权时停用读取。
- 服务端证书 TOFU：VNC 与 RDP 首次连接展示指纹并要求确认，确认后指纹变化即拒绝连接。
- 手机适配：窄屏布局断点、自动旋转、双指缩放、单指长按映射右键，并与鼠标输入共存。
- 诊断日志分级：周期性统计降为 debug，默认 hilog 级别下稳态不再刷屏。
- 隐私政策与首次启动同意流程；关于面板可查看版本与隐私政策。
- 自动化测试：`scripts/test.sh`（原生几何 CTest + ArkTS 连接逻辑 node 单测）与 `scripts/smoke-device.sh`（真机安装/启动冒烟）。

### 构建

- 新增 `scripts/build-release.sh`，默认产出未签名 `.app`（`app/build/outputs/default/app-default-unsigned.app`）；提供仓外发布材料时可选择签名到 `.work/release/`。
