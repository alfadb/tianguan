# 天关上架图标素材

本目录存放提交华为应用市场（AppGallery / AGC）时使用的平面图标。安装包内的应用图标是分层资源（`background.png` + `foreground.png` + `layered_image.json`，由系统裁切成圆角），这里的平面图是同一套图形合成后的上架素材。

| 文件 | 尺寸 | 用途 |
| --- | --- | --- |
| `icon-1024.png` | 1024×1024，不透明 | AGC「应用信息 → 应用图标」上传用（1024×1024 档）。也可用于官网、宣传素材。 |
| `icon-216.png` | 216×216，不透明 | AGC 图标上传的另一档规格（216×216）。与 1024 档二选一，AGC 页面另有尺寸要求时以页面提示为准。 |

## 规格说明

- 两张都是**正方形、无圆角、无透明像素**的 RGB PNG：圆角与形状遮罩由应用市场/系统绘制，素材本身不要预先切圆角，也不要有空白透明边。
- 背景色 `#17365D`（深蓝），中心带极轻微的径向暗角（角部比中心暗约 6%），肉眼几乎不可见。
- 前景为白色描边图形，线宽 64px（按 1024 画布计）、圆头圆角；内容外接框约 640×544，落在画布中心，位于系统裁剪安全区（居中约 2/3）内。
- `icon-216.png` 由 `icon-1024.png` 用 Lanczos 重采样缩小，没有单独绘制。

## 与安装包内图标的关系

安装包内不是单张平面图，而是分层图标；为避免同名资源重复，只保留一份：

- `app/AppScope/resources/base/media/` 下有
  `background.png`（1024×1024，不透明）、`foreground.png`（1024×1024，透明背景、白色前景）与
  `layered_image.json`（`{"layered-image": {"background": "$media:background", "foreground": "$media:foreground"}}`）；
- `app/entry/src/main/resources/base/media/` 下不再放同名三件套；
- `app/AppScope/app.json5` 与 `app/entry/src/main/module.json5`（两个 ability 的 `icon`/`startWindowIcon`）统一引用 `$media:layered_image`。

AppScope 资源在构建时会合入模块的同路径资源目录，模块清单可以直接引用；两处各放一份同名文件会触发 restool 的 `'...' is defined repeatedly` 警告，且官方说明重名时 AppScope 会覆盖模块副本，所以保留 AppScope 一份即可。

`icon-1024.png` 就是这两层直接叠加的结果，与设备上显示的图标同源同形（设备端由系统加圆角遮罩）。改动图标时三层素材要一起改，避免上架素材与安装后图标不一致。

## 为什么 HAP 里是 512×512（restool 行为）

源资源是 1024×1024，但构建产物（`unzip -l` / 解包后的 `resources/base/media/`）里两张图层都是 **512×512 RGBA**，背景的 alpha 为 254。这是 SDK 内置 restool 的固定行为，不是素材缺失，也不是本项目配置错误。

### 触发规则

1. **尺寸检查只告警**：hvigor 对 `bundleType=app`、非免安装的 entry/feature 模块自动在 `resConfig.json` 里置 `iconCheck: true`（等价于命令行 `--icon-check`；本工程 `deviceTypes: ['phone','tablet','2in1']` 不是 default/phone/tablet 全集，命中该逻辑）。restool 会检查 `icon` 与 `startWindowIcon` 引用到的 PNG，分层图标的 `background`/`foreground` 也通过 `layered_image.json` 被计入 `icon`。检查阈值不是 512，而是按资源目录的屏幕密度/设备档位取「常规尺寸」；`base` 目录等价于 `sdpi-phone`，对应 `icon=41 px`、`startWindowIcon=144 px`。所以 1024 的 base 图层必然打印：
   `Warning: The width or height of the png file referenced by the icon exceeds the limit (41 pixels)`。
   该检查只输出警告，不返回错误；hvigor 默认日志级别（非 DEBUG）不会显示这条警告。
2. **缩放到 512 是硬编码上限**：独立于上面的检查，restool 对所有被 `icon` 引用的图片（含分层图标的两个图层）调用图像转码器，目标尺寸硬编码为 `{512, 512}`；大于 512 的缩到 512，≤512 的保持原尺寸。`layered_image.json` 原样复制，所以 HAP 里仍是分层结构。
3. **alpha 254 是转码产物**：转码器把 RGB 背景输出为 RGBA，纯不透明像素的 alpha 为 254（254/255 ≈ 99.6%，肉眼不可见）。源 `background.png` 本身是 RGB、没有 alpha 通道，符合「背景不允许含透明像素」的素材要求。

### 是否影响上架

没有找到任何官方规则说 AGC 会校验 HAP 内图层的原始像素尺寸。官方图标规范要求 1024×1024 分层素材，指向的是提交/设计源资源；而标准工具链在打包时一定会把 `icon` 图层压到 512。若 AGC 以「HAP 内必须 1024」否决，所有用该 SDK 正常构建的应用都会被拒，这与事实不符。因此把 512 视为**编译期正常优化**，不作为上架阻塞项；上架素材仍以本目录的 1024/216 平面图为准，并与安装后 Dock/桌面效果核对一致。保留 1024 源文件是必须的（设计规范、后续换 SDK/工具链、以及商店素材都依赖它）。

> 残余风险：AGC 的「合法性检测 / 上架自检」只能在真实上传后看到结果，本地无法完全复现；如果 AGC 对包内图层有额外校验，以 AGC 的检测结论为准。

### 依据与复现

- 官方图标资源规范（1024×1024、分层、PNG、无圆角、背景不透明）：<https://developer.huawei.com/consumer/cn/doc/doccenter-ux-design/application-icon-0000001953444009>
- 官方分层图标配置说明（AppScope 资源合入模块、重名时 AppScope 覆盖模块）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/layered-image>
- restool 源码：`src/resource_check.cpp`（尺寸告警）、`include/resource_data.h`（`g_normalIconMap` 的 41/144 等档位）、`src/compression_parser.cpp`（`ScaleImage(..., {512, 512})`）、`src/config_parser.cpp`（把分层图层加入 `icon` 检查）——<https://gitee.com/openharmony/developtools_global_resource_tool>
- 本机实测（SDK 26.0.0.821）：对当前资源直接运行 restool，1024 图层输出 512×512；对 1024/600/513/512/511/256/145/144/42/41/40 的尺寸矩阵测试，大于 512 缩到 512、≤512 保持，告警阈值为 41/144。原始输出见 `.work/icon-restool-behavior-20261026.txt`（未入库）。

### 复现命令

```bash
# 以构建中间产物里的 resConfig.json 为输入，直接跑 restool（输出到 /tmp）
mkdir -p /tmp/icon-check/out /tmp/icon-check/ids
python3 - <<'EOF'
import json
c = json.load(open('app/entry/build/default/intermediates/res/default/resConfig.json'))
c['output'] = '/tmp/icon-check/out'
c['ResourceTable'] = ['/tmp/icon-check/ResourceTable.h', '/tmp/icon-check/ResourceTable.ts']
c['ids'] = '/tmp/icon-check/ids'
c['definedIds'] = '/tmp/icon-check/ids/id_defined.json'
json.dump(c, open('/tmp/icon-check/resConfig.json', 'w'))
EOF
export LD_LIBRARY_PATH="$HOME/harmonyos/command-line-tools/26.0.0.821/sdk/default/hms/toolchains/lib:$HOME/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/previewer/common/bin"
"$HOME/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/toolchains/restool" -l /tmp/icon-check/resConfig.json
file /tmp/icon-check/out/resources/base/media/*.png
```

## 生成方式

图形沿用旧 `128×128` 单层 `app_icon.svg` 的路径，按 ×8 放大到 1024 画布：

- 横线 `(224,272)–(800,272)`
- 横线 `(320,464)–(704,464)`
- 竖线 `(512,272)–(512,736)`
- 折线 `(272,752)→(512,464)→(752,752)`
- 线宽 64px，圆头圆角，白色

用 python3 + Pillow 以 4 倍超采样绘制前景再降采样到 1024（抗锯齿），背景为 `#17365D` 加轻微径向暗角，二者叠加得到平面图标。
