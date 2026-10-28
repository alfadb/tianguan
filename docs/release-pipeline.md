# 天关 AppGallery 发布构建与签名方案（首发 PC/2in1）

> 状态：2026-10-28。本文只写方案、配置片段与命令，**不包含任何密钥或口令**；所有签名材料必须留在仓库外。
>
> 适用对象：`cn.alfadb.tianguan`，API 26。**AGC「支持设备」首发只勾 PC/2in1**（2026-10-28 用户决定，取代 2026-10-27 的「手机 + 平板 + PC/2in1」）；包内 `deviceTypes` 保持 `['phone','tablet','2in1']`（包内范围 ≥ 勾选范围），不做 2in1 裁剪、不拆包；以后可加设备、不能删设备。
>
> 本轮已落地版本号 `1.0.0 / 100` 与 `scripts/build-release.sh`；2026-10-27 设备声明改为 `['phone','tablet','2in1']` 并重新构建核对（2026-10-28 决定不改包）；真实发布证书签名与 AGC 上传仍需等待外部材料。

## 0. 现状核查

| 项 | 现状 | 结论 |
| --- | --- | --- |
| 构建入口 | `app/build.sh` 只跑 `assembleHap --mode module`，产物 `entry-default-unsigned.hap`；`scripts/build-release.sh` 跑 `assembleApp --mode project`，产物 `app-default-unsigned.app` | 已有 `.app` 构建入口 |
| 签名配置 | `app/build-profile.json5` 无 `signingConfigs`，产品上无 `signingConfig` | 构建产物未签名；靠仓外 `HapSignLauncher.java` 手工签名 |
| 签名材料 | 只有调试证书 + 调试 ACL Profile（在 `$HOME/harmonyos-signing/tianguan/`） | 无发布证书 / 发布 Profile |
| 设备类型 | `module.json5` 与 `entry/build-profile.json5` 都是 `['phone', 'tablet', '2in1']` | 2026-10-27 调整；包内声明三项，AGC 首发只勾 PC/2in1（2026-10-28） |
| 版本 | `app.json5` 与 `build-profile.json5` 的 `default` product 均为 `versionCode 100` / `versionName 1.0.0` | 已同步，见第 4 节 |
| 上架包 | `scripts/build-release.sh` 已能产出未签名 `.app` | AGC 要签名的 `.app`，见第 1、2 节 |

## 1. 发布包格式：`.app`（App Pack），不是 `.hap`

**结论**：HarmonyOS 应用在 AppGallery 上架时提交 **`.app`（App Pack）**。`.hap` 是安装/分发单位，AGC 收到 `.app` 后会拆包并对内部 HAP 重新签名；`.hap` 不能作为上架包单独提交。

- 官方 FAQ 原文：「HAP 是用于本地调试的，APP 包是用于上架发布的。根据不同的设备类型，一个 APP 包可以包含多个 HAP 包。」
- 同一个 FAQ 还明确：「APP 包能本地安装吗？不能，APP 包只能用于发布，即便使用调试证书和调试 Profile 进行签名，也无法安装。」因此**安装到设备上验证必须用 HAP**，`.app` 只能验证结构与签名。
- PC/2in1 没有单独的包格式，仍然是 `.app`；2in1 是通过「AGC 创建应用时勾选支持设备 PC/2in1」+「包内 `module.json5` 的 `deviceTypes` 范围覆盖它」来区分的。
- 上架包必须是 Release 类型：用 `buildMode=release` 构建；AGC 读取包内 `pack.info` 的 `releaseType` 与 `app.debug`。

### 1.1 纯命令行构建 `.app`（无 IDE）

本工程没有 DevEco Studio，使用命令行工具链即可。发布构建已固化为脚本，直接运行：

```bash
cd /home/worker/work/base/tianguan
bash scripts/build-release.sh
```

脚本内部（在 `app/` 下）实际执行：

```bash
<SDK>/bin/hvigorw --no-daemon clean assembleApp --mode project \
  -p product=default -p buildMode=release
```

默认 `SDK=/home/worker/harmonyos/command-line-tools/26.0.0.821`，可用 `TIANGUAN_SDK` 覆盖；脚本同时设置 `DEVECO_SDK_HOME` / `DEVECO_NODE_HOME` 并执行 `ohpm install`。产物为 `app/build/outputs/default/app-default-unsigned.app`，脚本会校验包内含 `pack.info` 与 `entry-*.hap`，并打印路径、大小和 SHA-256。

要点：

- `assembleApp` 必须配 `--mode project`；`-p module=entry@<target>` 是 **模块模式** 参数，打包 APP 时传入会报错（本工具链在 `PreBuildApp.checkConfigModuleStatus()` 中直接拒绝 `module` 参数）。
- 工程目录名是 `app`，hvigor 的工程名取执行目录的 basename，所以 APP 文件名是 `app-<product>-<signed|unsigned>.app`；当前 product 为 `default`，产物即 `app-default-unsigned.app`。以 `ls` 实际输出为准。
- 不配签名时 `assembleApp` 会打印 `No signingConfig found for product default` 并继续，产出 **`-unsigned.app`**；这是本仓库推荐的起点（签名在外部完成，见第 2 节）。
- 产物：

| 产物 | 路径（相对仓库根） |
| --- | --- |
| App Pack（未签名） | `app/build/outputs/default/app-default-unsigned.app` |
| 单模块 HAP（可安装验证） | `app/entry/build/default/outputs/default/entry-default-unsigned.hap` |
| 符号表（可选上传） | `app/build/outputs/default/symbol/`、`app/entry/build/default/outputs/default/symbol/` |

- 上传不要在命令行里做：`Build > Upload Product` 是 DevEco Studio 的功能，纯命令行没有等价入口。纯 CLI 流程是「本地构建并签名 → 浏览器登录 AGC → 应用上架 > 软件包管理 > 上传」。

### 1.2 关于 AGC 的重签名

- 26.0.0 及以上，DevEco Studio 的 `Upload Product` 会在上传时重新签名（可用 AGC 云管理证书或开发者自己的证书），所以文档说「只要编译构建后上传」；这条路径依赖 IDE。
- 纯 CLI 走 AGC 网页上传时，按传统发布流程应事先用**发布证书 + 发布 Profile** 签好 `.app`；AGC 上传后仍会拆包并重签内部 HAP。
- 「AGC 网页上传是否接受未签名 `.app`」在 26.0.0 的云端重签能力下**需向华为确认**；本方案按「本地用发布材料签名」执行，不赌未签名可上传。

## 2. 签名配置：字段、密码注入方式与真实限制

### 2.1 `signingConfigs` 的完整字段

`app/build-profile.json5` 的 `app.signingConfigs` 是对象数组，`material` 七个字段全部必填。下面是**字段格式示意**；直接照抄会因缺少 DevEco 生成的 `material/` 目录而签名失败（原因见 2.2）：

```json5
{
  app: {
    signingConfigs: [
      {
        name: 'release',            // 1~64 个字母/数字，产品通过 signingConfig 引用它
        type: 'HarmonyOS',          // HarmonyOS | OpenHarmony
        material: {
          storeFile: '/home/worker/harmonyos-signing/tianguan-release/private/tianguan-release.p12',
          storePassword: '<DevEco 生成的密文，不能是明文>',
          keyAlias: 'tianguan-release',
          keyPassword: '<DevEco 生成的密文，不能是明文>',
          signAlg: 'SHA256withECDSA',   // 唯一可选值
          profile: '/home/worker/harmonyos-signing/tianguan-release/profiles/TianGuanRelease.p7b',
          certpath: '/home/worker/harmonyos-signing/tianguan-release/cert/TianGuanRelease.cer'
        }
      },
      {
        name: 'debug',
        type: 'HarmonyOS',
        material: {
          storeFile: '/home/worker/harmonyos-signing/tianguan/private/tianguan-debug.p12',
          storePassword: '<DevEco 生成的密文，不能是明文>',
          keyAlias: 'tianguan-debug',
          keyPassword: '<DevEco 生成的密文，不能是明文>',
          signAlg: 'SHA256withECDSA',
          profile: '/home/worker/harmonyos-signing/tianguan/profiles/TianGuanDebugDevicesACLDebug.p7b',
          certpath: '/home/worker/harmonyos-signing/tianguan/cert/TianGuanDebug.cer'
        }
      }
    ],
    products: [
      // 不写 signingConfig 就是不签名；当前仓库维持这种方式，签名在外部做。
      { name: 'default' /* ... */ }
    ]
  }
}
```

> 上面 `products` 只示意 `signingConfig` 的关联方式，不是完整可编译配置；当前仓库的完整 product 见第 3 节。

注意事项：

- 路径支持绝对路径，也支持相对路径；相对路径以**工程根目录**（`app/`）为起点。签名材料必须放在仓库外，或至少被 `.gitignore` 覆盖（仓库现有 `.gitignore` 已忽略 `*.p12`、`*.cer`、`*.p7b` 等，但**不会**忽略 `build-profile.json5` 里的密文）。
- `signingConfigs` 与 product 是「产品级」关联，与 `buildMode` 无关：同一个 product 的 debug/release 构建都会用同一个 `signingConfig`。`buildMode=release` 只影响 `debuggable`/混淆等编译选项，不会自动切换到发布签名材料。因此如果以后配置两套材料，建议拆成两个 product。
- 推论：发布包必须**同时**指定 `-p product=default` 和 `-p buildMode=release`。只改 `buildMode` 不会把构建模式变成 release；当前只有 `default` 一个 product。
- `signAlg` 只能写 `SHA256withECDSA`，不是 OpenSSL 风格的任意算法。
- 不要把 `.p12` 口令写进仓库、提交信息、命令行 argv 或 CI 日志。

### 2.2 密码不能靠「环境变量直接注入」——hvigor 的签名链路只接受 DevEco 密文

这是本方案最重要的一个现实约束，和网上很多「用环境变量覆盖 `storePassword`」的写法不同：

- 本机工具链（26.0.0.821）的 `hvigor-ohos-plugin` 在签名前会调用 `CommonSignCommandBuilder.getKeyStorePwd()/getKeyPwd()`，无条件进入 `DecipherUtil.decryptPwd()`；
- `decryptPwd()` 要求口令是 **至少 32 字符的十六进制密文**，并且要在 `.p12` 同级目录读取 `material/{fd,ac,ce}` 三个子目录里的密钥材料，然后做 AES-128-GCM 解密；
- 因此把 `storePassword` 写成明文、写成 `process.env.XXX` 的返回值、或用 `hvigorfile.ts` 的 `overrides`/hook 换成明文，都会在签名阶段报 `INVALID_DATA` / `INVALID_PASSWORD_LENGTH` / `KEY_PASSWORD_VALID_FAILED`。这不是配置格式问题，是 hvigor 的签名实现只认 DevEco 生成的「密文 + material 目录」。

官方文档也印证了这一点：`signingConfigs.material.storePassword` 被描述为「以密文形式呈现」；命令行构建文档要求流水线中存在 `.cer`、`.p7b`、`.p12` **和 material 文件夹**（默认与 `.p12` 同路径）。

各注入方式的结论：

| 方式 | 口令形态 | 是否需 DevEco 生成 | 仓库内是否留密 | 结论 |
| --- | --- | --- | --- | --- |
| 外部 `hap-sign-tool` + 0600 口令文件 | 明文（只存在仓外文件） | 否 | 否 | **推荐**，见 2.3 |
| `signingConfigs` + 环境变量/仓外 JSON | 必须是 DevEco 密文 | 是 | 否 | 可用，见 2.4 |
| `signingConfigs` 直接写明文 | 明文 | 否 | 会泄露 | **不可用**（解密报错） |
| 构建脚本临时改 `build-profile.json5` | 必须是 DevEco 密文 | 是 | 临时 | CI 一次性容器可考虑，本地不推荐 |

**所以在本仓库（没有 IDE）里，推荐做法是：不让 hvigor 签名。**

### 2.3 推荐路径：hvigor 只构建未签名包，仓外工具签名

这与仓库现状一致（`HapSignLauncher.java` 已经这样用了很久），只是把签名对象从 HAP 扩展到 `.app`：

```bash
set -euo pipefail
SIGN_ROOT=/home/worker/harmonyos-signing
JAR=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar
MAT=/home/worker/harmonyos-signing/tianguan          # 发布时换成 tianguan-release

IN=/home/worker/work/base/tianguan/app/build/outputs/default/app-default-unsigned.app
OUT=/home/worker/work/base/tianguan/.work/release/app-default-signed.app

mkdir -p "$(dirname "$OUT")"
test -f "$IN"
test ! -e "$OUT"

(cd "$SIGN_ROOT" && sha256sum -c HapSignLauncher.java.sha256)
CLASSES=$(mktemp -d /tmp/hap-sign-classes.XXXXXX)
trap 'rm -rf -- "$CLASSES"' EXIT
javac -cp "$JAR" -d "$CLASSES" "$SIGN_ROOT/HapSignLauncher.java"

java -cp "$JAR:$CLASSES" HapSignLauncher \
  "$MAT/private/tianguan-debug.p12" \
  "$MAT/private/tianguan-debug.p12.pass" \
  "$MAT/cert/TianGuanDebug.cer" \
  "$MAT/profiles/TianGuanDebugDevicesACLDebug.p7b" \
  "$IN" "$OUT" tianguan-debug 26
```

- 口令只以文件路径传入，`HapSignLauncher` 把口令读成 `char[]`，不经过 argv/环境变量/日志；这是仓库既有的安全约定。
- `scripts/build-release.sh` 已按同样方式内置可选签名：环境变量 `TIANGUAN_RELEASE_P12`、`TIANGUAN_RELEASE_CER`、`TIANGUAN_RELEASE_PROFILE`、`TIANGUAN_RELEASE_P12_PASS_FILE`、`TIANGUAN_RELEASE_ALIAS` 全部给出时，调用仓外 `HapSignLauncher.java`（可用 `TIANGUAN_HAP_SIGN_LAUNCHER` 指定路径）把未签名 `.app` 签到 `.work/release/`；缺任意一个就只产出未签名包。
- 正式发布时只替换材料目录、证书、Profile、alias：`tianguan-release.p12`、发布 `.cer`、发布 `.p7b`、release alias；`compatibleVersion` 仍为 `26`。
- 官方文档明确「如果要对 APP 进行签名，只需将 `inFile` 和 `outFile` 参数修改为 APP 包即可」，即 `hap-sign-tool` 的 `sign-app` 同样可签 `.app`。本仓库的启动器封装的是同一个 `HapSignTool.signApp` API；本轮已实际构建未签名 `.app`，签名路径尚未用发布材料实测，首次使用时应按第 6 节做一次 `verify-app`。
- 如果启动器在 `.app` 上因 `signCode` 参数报错，退回官方 CLI 的交互式口令模式（口令不落 argv）：

```bash
java -jar "$JAR" sign-app -mode localSign \
  -keyAlias tianguan-debug -signAlg SHA256withECDSA \
  -appCertFile "$MAT/cert/TianGuanDebug.cer" \
  -profileFile "$MAT/profiles/TianGuanDebugDevicesACLDebug.p7b" \
  -inFile "$IN" -outFile "$OUT" -compatibleVersion 26 -pwdInputMode 1
```

### 2.4 如果坚持让 hvigor 一步签名（可选，需要 DevEco 生成材料）

1. 在任意装有 DevEco Studio 的机器上用 `Project Structure > Signing Configs` 配好发布材料并保存；DevEco 会在 `.p12` 同级生成 `material/` 目录，并把密文口令写入 `build-profile.json5`。
2. 把 `.p12 + material/ + .cer + .p7b` 整体拷到仓库外的 `/home/worker/harmonyos-signing/tianguan-release/`。
3. 仓库内只保留**路径**，密文口令用环境变量/仓外 JSON 注入，例如工程级 `app/hvigorfile.ts`：

```ts
import { appTasks } from '@ohos/hvigor-ohos-plugin';
import { readFileSync } from 'node:fs';

// 仓外 JSON：{ name, type, material: { storeFile, storePassword, keyAlias,
//   keyPassword, signAlg, profile, certpath } }，口令字段是 DevEco 密文。
const file = process.env.TIANGUAN_RELEASE_SIGNING;
const signing = file ? JSON.parse(readFileSync(file, 'utf8')) : undefined;

export default {
  system: appTasks,
  plugins: [],
  ...(signing ? { config: { ohos: { overrides: { signingConfig: signing } } } } : {})
};
```

- 这是官方文档里 `config.ohos.overrides.signingConfig` 的用法（官方把它标为「不推荐使用」，推荐 hook，但功能可用）；`overrides` 是**工程级**的，设置后对所有 product 生效，所以必须像上面这样用环境变量控制「只在发布环境出现」。
- 再次强调：即使走这条路径，`storePassword`/`keyPassword` 仍必须是 DevEco 密文并伴随 `material/` 目录；环境变量里放明文不行。
- 如果不想改 `hvigorfile.ts`，可以退化为「发布脚本临时生成 `build-profile.json5`，构建后还原」，但这会修改受版本控制的文件，失败中断时容易留下脏工作区；只有在 CI 一次性容器里才建议使用，本仓库不推荐。

### 2.5 安全与仓库卫生

- 签名材料统一放 `/home/worker/harmonyos-signing/tianguan-release/`（发布）与现有 `.../tianguan/`（调试）两套，互不复用；Profile 绑定应用与设备，不能跨应用。
- 仓库 `.gitignore` 已覆盖密钥/证书扩展名；仍需保证 `build-profile.json5` 中不出现密文口令。若采用 2.4，`TIANGUAN_RELEASE_SIGNING` 指向的 JSON 必须在仓库外且权限 0600。
- 发布证书的私钥丢失后**无法更换签名**（官方 FAQ：HarmonyOS 应用不支持更换签名），`.p12` 要长期备份；换证书只能重新申请发布证书与 Profile，且更新版本时要用同一个 CSR 生成的证书。

## 3. 发布范围：AGC 首发只勾 PC/2in1，包内声明维持三项

**结论（用户决策 2026-10-28）**：AGC「支持设备」**首发只勾 PC/2in1**，不勾手机、平板、智慧屏；包内 `deviceTypes` 保持 `['phone','tablet','2in1']`。**不需要改包、不拆包、不做 2in1 裁剪**。

- `module.json5` 的 `deviceTypes` 与 `entry/build-profile.json5` 的 `config.deviceType` 均为 `['phone', 'tablet', '2in1']`（2026-10-27 由四项改为三项，2026-10-28 决定保持不变）。
- 当前只有 `default` 一个 product；发布构建用 `-p product=default`，包内 `module.json` 与 `pack.info` 声明 `phone` + `tablet` + `2in1`。
- AGC 的「支持设备」只勾 PC/2in1；官方要求包内声明范围 ≥ AGC 勾选范围（3 ≥ 1，满足）。分发范围由 AGC 勾选决定，手机/平板用户不会拿到应用。
- `READ_PASTEBOARD`：PC/2in1 上均可申请、首次默认允许，手机侧「场景不符」风险首发不涉及；发布 Profile 仍要勾该 ACL，AGC 隐私说明仍要理由 + 使用场景视频（约 3 个工作日）。

### 3.1 为什么不再走 target 收窄

上一轮方案曾计划新增只含 `2in1` 的 target 和 `appstore2in1` product，用 `targets[].config.deviceType` 把包内设备收窄为 `2in1`。现在首发只勾 PC/2in1、包内保留三项即可满足「包内范围 ≥ 勾选范围」，因此该方案**搁置不实施**：

- `module.json5` 的 `deviceTypes` 是模块级唯一声明，不能按 product 直接覆盖；
- target 的 `config.deviceType` 只能是 `module.json5` 的子集；
- 这些机制仍然有效，但只有将来真的要按设备拆包时才需要，当前不引入额外 product/target。

### 3.2 当前配置（本轮未改）

`app/build-profile.json5` 只有 `default` 一个 product，版本与 `app.json5` 同步为 `100 / '1.0.0'`；`modules[].targets` 只有 `default`：

```json5
app: {
  products: [{
    name: 'default',
    bundleName: 'cn.alfadb.tianguan',
    // ... compile/compatible/targetSdkVersion 均为 26.0.0
    versionCode: 100,
    versionName: '1.0.0'
  }],
  buildModeSet: [{ name: 'debug' }, { name: 'release' }]
},
modules: [{ name: 'entry', srcPath: './entry', targets: [{ name: 'default', applyToProducts: ['default'] }] }]
```

`app/entry/build-profile.json5` 的 `config.deviceType` 为 `['phone', 'tablet', '2in1']`；`app/entry/src/main/module.json5` 的 `deviceTypes` 也是 `['phone', 'tablet', '2in1']`，两者一致。

### 3.3 构建与验收

```bash
# 仓库根：构建未签名 .app
bash scripts/build-release.sh

# 包内设备与 release 标记（在 app/ 下）
cd app
unzip -p build/outputs/default/app-default-unsigned.app entry-default.hap > /tmp/entry-default.hap
unzip -p /tmp/entry-default.hap module.json \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["module"]["deviceTypes"]); print("debug:", d["app"]["debug"], "buildMode:", d["app"]["buildMode"])'
# 期望：['phone', 'tablet', '2in1']、debug False、buildMode Release
unzip -p build/outputs/default/app-default-unsigned.app pack.info
# 期望：version 1.0.0/100，deviceType 为 phone / tablet / 2in1（无 tv），releaseType Release
```

### 3.4 风险与边界

- **AGC 设备勾选不能超过包内声明**：官方要求「软件包内声明的设备范围 ≥ AGC 最终勾选的设备范围」。当前包声明 `phone` + `tablet` + `2in1`，AGC 只勾 PC/2in1，满足。
- **`READ_PASTEBOARD` 首发不涉手机侧风险**：只勾 PC/2in1 时该权限在 PC 上「均可申请」，但仍须发布 Profile 的 ACL、隐私说明理由与使用场景视频；以后加手机时才会回到「手机场景不符」的审核风险（见 [appgallery-readiness.md](appgallery-readiness.md) 4.2）。
- **首发无平板截图缺口**：平板不勾选，不需要平板素材；以后加平板时再补 3 张 1280×1920 / 1920×1280。
- **版本号两处同步**：`app.json5` 与 `default` product 都写了版本；只改一处不生效（product 会覆盖）。当前两处均为 `100 / '1.0.0'`。
- **构建命令已固化**：`scripts/build-release.sh` 用 `product=default` + `buildMode=release`；`app/build.sh` 仍只出未签名 HAP，不受影响。

## 4. 版本号：规则、取值与同步位置

### 4.1 规则

- `versionCode`：整数，官方范围 `0 ~ 2147483647`；官方要求新版本必须使用比旧版本**更大的值**（AGC 会按版本号判重，重复提交同一版本会失败，具体以 AGC 提示为准）。
- `versionName`：长度不超过 127 字节；只允许数字和点（推荐 `A.B.C`），或带 `{}` 的模板字符串；它只用于展示，不参与新旧比较。
- API 23 起可选 `buildVersion`（1~18 字节，数字和点）；AGC 用它区分同一主版本下的测试子版本。首版可留空。

### 4.2 关键坑：product 会覆盖 `app.json5`

官方字段说明：product 里配置了 `label/icon/versionCode/versionName/resource` 后，编译时会用 product 的值替换 `app.json5`。当前 `app/build-profile.json5` 的 `default` product 与 `AppScope/app.json5` 均已写 `versionCode: 100, versionName: '1.0.0'`；只改其中一处不会生效。

当前首发取值（已落地）：

| 位置 | 值 |
| --- | --- |
| `app/AppScope/app.json5` | `versionCode: 100`、`versionName: '1.0.0'` |
| `app/build-profile.json5` 的 `default` product | `100` / `'1.0.0'` |

后续每发一版：`101 / 1.0.1`、`102 / 1.0.2`……只增不减。若决定删掉 product 里的版本字段，需确认 `app.json5` 是唯一来源，避免一处生效一处不生效。

### 4.3 同步位置（本轮已落地）

- `README.md` 与 `app/README.md` 的版本描述已改为 `1.0.0 / code 100`；README 没有版本徽章，不存在徽章同步问题。
- `README.md` 里的 `READ_PASTEBOARD` 描述已按「ACL 已放行、声明已恢复」订正，与 `module.json5` 一致。
- 新增 `CHANGELOG.md`，首个条目为 `## [1.0.0] - 2026-10-26`，供 AGC 更新说明与仓库历史对照。
- `app/oh-package.json5` 与 `app/entry/src/main/cpp/types/libtianguan_diag/oh-package.json5` 里的 `version` 是 ohpm 包版本，不是应用版本，**不需要**跟 `versionCode` 同步。
- 应用内「关于」面板从 `bundleManager` 读版本（`PrivacyStore`），不用改代码；构建后实际显示值由上面的配置决定。
- `docs/appgallery-readiness.md` 里的 `0.0.1` 是当时审计快照，不必回改历史结论，但新增的发布记录应写清首发版本。

## 5. Release 构建的其它必做项

### 5.1 混淆（ArkGuard）：当前是关闭的

- 官方说明：从 DevEco Studio 5.0.3.600 起，新建工程/模块**默认关闭**源码混淆；只有显式配置 `arkOptions.obfuscation.ruleOptions.enable: true` 才在 **release** 模式生效（debug 模式一律不混淆）。
- 本仓库的两个 `build-profile.json5` 都没有 `obfuscation` 配置，因此**当前 release 构建不混淆**。本工具链的实现还要求显式写 `enable: true`，只写 `files` 不会开启。
- 如要开启（可选，建议不要和首发一起做）：

```json5
// app/entry/build-profile.json5 —— buildOption
{
  buildOption: {
    arkOptions: {
      obfuscation: {
        ruleOptions: {
          enable: true,
          files: ['./obfuscation-rules.txt']
        }
      }
    }
  }
}
```

- 开启后，DevEco 新建工程自带的 `obfuscation-rules.txt` 默认会启用属性/顶层/文件名/导出名混淆；本仓库没有这个文件，需要自己创建并按需显式写选项。这对天关有两个现实风险：
  1. NAPI：`import diag from 'libtianguan_diag.so'` 后调用的 `diag.runDiagnostics` 是**按属性名跨语言查找**的；属性混淆会改掉这个名字，native 侧找不到。需要 `-keep-property-name runDiagnostics` 之类的保留规则。
  2. JSON：连接配置、隐私偏好等用 `JSON.parse`/`JSON.stringify` 的对象属性会被属性混淆破坏，需要把对应属性加进 `-keep-property-name` 白名单。
- 建议：**首发不开启混淆**（保持当前行为），先保证上架与稳定性；等有完整真机回归和保留白名单后再单独做一个「开启混淆」的版本。如果开启，必须重新跑 `scripts/smoke-device.sh` 与全部手工验证。

### 5.2 debug 信息移除

- `buildMode=release` 默认 `debuggable=false`：
  - 包内 `app.debug=false`，`app.buildMode='release'`，`pack.info` 的 `releaseType='Release'`；
  - 不打包 `sourceMaps.map`，`ets/` 下只有 `modules.abc`（已用现有 release HAP 核对）；
  - native `.so` 在打包阶段被 strip：现有 release HAP 内的 `libtianguan_diag.so` 已无 `.debug_*` 段（仓库 `app/entry/libs/` 下未打包的副本仍带调试段，属正常）；符号表单独输出到 `outputs/<target>/symbol`，可按需上传 AGC。
- release **不会自动删除** `console.log`/`hilog` 调用；天关的周期性诊断日志已在代码里降为 debug 级别（见 `app/README.md`），发版前仍建议在真机 `hdc hilog` 确认没有 info/warn 级周期日志刷屏。
- 不要在 release 包里保留调试开关、测试入口或明文测试凭据；`vnc-test.json` 等测试模式是运行时文件，不进包。

### 5.3 产物与可选打包开关

- 默认 `packOptions.fastBuildApp=false`：`assembleApp` 会同时保留模块下的 HAP，正好可以拿来做安装验证。
- 只想要 APP 时可设 `buildOption.packOptions.fastBuildApp=true`，但此时模块下不再生成 HAP，安装验证就得另跑 `assembleHap`；不建议首发阶段改。
- `buildAppSkipSignHap=true` 可跳过「生成签名 HAP」以提速；本方案本来就不在 hvigor 内签名，没有收益，保持默认即可。

## 6. 不依赖 AGC 的验证路径（上线前自测）

目标：用现有**调试证书 + 调试 ACL Profile**，验证「release 模式 + 三项设备声明包 + 打包路径 + 签名流程」都正确。发布证书/发布 Profile 的真实签名只能等 AGC 下发后再验。

> 为什么必须用 `TianGuanDebugDevicesACLDebug.p7b`：包内声明了 `ohos.permission.READ_PASTEBOARD`，如果 Profile 的 ACL 列表不含它，安装会失败（9568289）。不带 ACL 的 `TianGuanDebugDevicesDebug.p7b` 只适用于不声明该权限的包。已核对：ACL Profile 的 `allowed-acls` 为 `["ohos.permission.READ_PASTEBOARD"]`，且绑定 `cn.alfadb.tianguan` 与两台设备。

### 6.1 步骤

```bash
set -euo pipefail
SDK=/home/worker/harmonyos/command-line-tools/26.0.0.821
APP_ROOT=/home/worker/work/base/tianguan/app
JAR="$SDK/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar"
SIGN_ROOT=/home/worker/harmonyos-signing
MAT="$SIGN_ROOT/tianguan"
HDC="$SDK/sdk/default/openharmony/toolchains/hdc"
OUTDIR=/home/worker/work/base/tianguan/.work/release-verify

export DEVECO_SDK_HOME="$SDK"
export DEVECO_NODE_HOME="$SDK/tool/node"
export PATH="$SDK/tool/node/bin:$SDK/bin:$SDK/ohpm/bin:$PATH"

mkdir -p "$OUTDIR"
cd "$APP_ROOT"

# 1) 构建 release 模式的三项设备声明 HAP（不签名）
"$SDK/bin/hvigorw" --no-daemon assembleHap --mode module \
  -p product=default -p buildMode=release

HAP="$APP_ROOT/entry/build/default/outputs/default/entry-default-unsigned.hap"
test -f "$HAP"

# 2) 检查包内 deviceTypes 与 release 标记
unzip -p "$HAP" module.json \
  | python3 -c 'import json,sys; d=json.load(sys.stdin); print("deviceTypes:", d["module"]["deviceTypes"]); print("debug:", d["app"]["debug"], "buildMode:", d["app"]["buildMode"])'
unzip -p "$HAP" pack.info

# 3) 用调试 ACL Profile 签名（release 产物 + 调试签名，仅本地验证用）
(cd "$SIGN_ROOT" && sha256sum -c HapSignLauncher.java.sha256)
CLASSES=$(mktemp -d /tmp/hap-sign-classes.XXXXXX)
trap 'rm -rf -- "$CLASSES"' EXIT
javac -cp "$JAR" -d "$CLASSES" "$SIGN_ROOT/HapSignLauncher.java"
SIGNED_HAP="$OUTDIR/entry-default-release-debugsigned.hap"
java -cp "$JAR:$CLASSES" HapSignLauncher \
  "$MAT/private/tianguan-debug.p12" \
  "$MAT/private/tianguan-debug.p12.pass" \
  "$MAT/cert/TianGuanDebug.cer" \
  "$MAT/profiles/TianGuanDebugDevicesACLDebug.p7b" \
  "$HAP" "$SIGNED_HAP" tianguan-debug 26

# 4) 验签并核对 Profile 与源文件一致
mkdir -p "$OUTDIR/verify"
java -jar "$JAR" verify-app -inFile "$SIGNED_HAP" \
  -outCertChain "$OUTDIR/verify/chain.cer" \
  -outProfile "$OUTDIR/verify/profile.p7b"
cmp "$OUTDIR/verify/profile.p7b" "$MAT/profiles/TianGuanDebugDevicesACLDebug.p7b" \
  && echo "profile identical"

# 5) 安装到 PC 并启动（.app 不能装，必须用 HAP）
"$HDC" list targets
if [ -z "${SERIAL:-}" ]; then
  read -r -p '输入 SLM-W32 的 hdc 目标（hdc list targets 中的一项）: ' SERIAL
fi
"$HDC" -t "$SERIAL" install -r "$SIGNED_HAP"
"$HDC" -t "$SERIAL" shell aa start -a EntryAbility -b cn.alfadb.tianguan

# 6) 真机冒烟（脚本自己会带 --hap 再安装一次，覆盖安装属预期）
cd /home/worker/work/base/tianguan
bash scripts/smoke-device.sh "$SERIAL" --hap "$SIGNED_HAP"
```

### 6.2 再验证 `.app` 的结构与签名

下面的命令**沿用 6.1 的 `$SDK`、`$APP_ROOT`、`$JAR`、`$MAT`、`$OUTDIR` 与已编译好的 `$CLASSES`**；单独执行时先跑 6.1 的变量段与第 3 步的 `javac`。

```bash
cd /home/worker/work/base/tianguan/app
bash ../scripts/build-release.sh

mkdir -p "$OUTDIR/verify"
UNSIGNED_APP="$APP_ROOT/build/outputs/default/app-default-unsigned.app"
test -f "$UNSIGNED_APP"
unzip -l "$UNSIGNED_APP" | grep -E '\.hap$|pack.info'   # 应看到 entry-default.hap 与 pack.info

SIGNED_APP="$OUTDIR/app-default-debugsigned.app"
java -cp "$JAR:$CLASSES" HapSignLauncher \
  "$MAT/private/tianguan-debug.p12" \
  "$MAT/private/tianguan-debug.p12.pass" \
  "$MAT/cert/TianGuanDebug.cer" \
  "$MAT/profiles/TianGuanDebugDevicesACLDebug.p7b" \
  "$UNSIGNED_APP" "$SIGNED_APP" tianguan-debug 26

java -jar "$JAR" verify-app -inFile "$SIGNED_APP" \
  -outCertChain "$OUTDIR/verify/app-chain.cer" \
  -outProfile "$OUTDIR/verify/app-profile.p7b"
```

说明：

- `.app` **不能安装**（官方 FAQ），所以 `verify-app` 通过、包内 HAP 正确、`pack.info` 是 Release，就是本机能做到的全部；安装与交互只能用 HAP。
- `HapSignLauncher` 的 8 个参数名是按 HAP 命名的（`in-hap`/`out-hap`），但底层是 `HapSignTool.signApp`；官方文档说明 APP 包同样支持。首次对 `.app` 执行时以 `verify-app` 结果为准；若报格式/参数错误，改用 2.3 末尾的官方 CLI `sign-app`（`-pwdInputMode 1`）。

### 6.3 这条路径证明什么、不证明什么

能证明：

- release 模式构建成功，包内 `debug=false`、`releaseType=Release`；
- 包内三项设备声明生效，`deviceTypes` 为 `phone` + `tablet` + `2in1`（AGC 首发只勾 PC/2in1，属包范围 ≥ 勾选范围的正常状态）；
- 未签名 HAP / `.app` 可被 `hap-sign-tool` 正确签名并通过 `verify-app`（HAP 路径已在既有流程中验证；`.app` 首次执行时按 6.2 验证）；
- 调试 ACL Profile 能覆盖包内受限权限，PC 真机可安装、启动、冒烟。

不能证明（必须等 AGC 发布材料/环境）：

- 发布证书 + 发布 Profile 的签名链是否被 AGC 接受；
- 发布 Profile 是否包含 `READ_PASTEBOARD` ACL（需要在 AGC 新建发布 Profile 时勾选「受限 ACL 权限」）；
- AGC 合法性检测/上架自检结果、云端重签、按设备类型分发；
- 审核对首发只发 PC/2in1 的判定（含 PC 侧 `READ_PASTEBOARD` 的理由与使用场景视频是否被接受）。

## 7. 上线顺序建议

1. **AGC 侧（已完成应用创建）**：应用已创建，APP ID `691761958035659012`，包名 `cn.alfadb.tianguan`，**支持设备首发只勾 PC/2in1**（2026-10-28 决定，不勾手机/平板/智慧屏）。准备发布证书（CSR 用长期保存的新 `.p12` 生成，发布证书有效期 3 年）；申请发布 Profile 时勾选「受限 ACL 权限（HarmonyOS API9 及以上）」中的 `ohos.permission.READ_PASTEBOARD`，下载 `.cer`/`.p7b`。
2. **配置落地（本轮已完成）**：`versionCode 100 / versionName 1.0.0` 已同步到 `app.json5` 与 `default` product；新增 `scripts/build-release.sh`、`CHANGELOG.md`，README 与发布文档已同步；不新增 `2in1` target/product，`deviceTypes` 为 `['phone', 'tablet', '2in1']`（2026-10-27 调整，2026-10-28 决定保持不变）；首发不开启混淆。
3. **本地验证**：`bash scripts/test.sh` 全通过；`bash scripts/build-release.sh` 产出未签名 `.app`；`bash app/build.sh` 仍产出未签名 HAP。需要装机时用第 6 节调试签名 HAP 路径。
4. **发布日**：构建 unsigned `.app` → 用发布材料签名 → `verify-app` → AGC 软件包管理「测试和正式上架」上传 → 合法性检测 → 上架自检 → 提交审核。
5. **每次发版**：`versionCode` 递增；复用同一 `.p12`（签名不能换）；如换证书需同步换 Profile；重新跑第 6 节。

### 7.1 配置改动清单（本轮已执行）

| 文件 | 改动 | 备注 |
| --- | --- | --- |
| `app/AppScope/app.json5` | `versionCode: 100`、`versionName: '1.0.0'` | 已完成 |
| `app/build-profile.json5` 的 `default` product | 同步为 `100` / `'1.0.0'` | product 覆盖 `app.json5`；已完成 |
| `scripts/build-release.sh` | 新增发布构建脚本（`assembleApp --mode project -p product=default -p buildMode=release`） | 默认未签名 `.app`；可选外部签名 |
| `CHANGELOG.md` | 新增，首个条目 `1.0.0 / 2026-10-26` | 已完成 |
| `README.md` / `app/README.md` | 版本、RDP / ACL / 启动诊断等过期描述订正，补「构建发布包」 | 已完成 |
| `docs/release-pipeline.md` | 首发只勾 PC/2in1、三项设备声明、版本号、脚本命令与产物路径 | 本文 |
| `app/entry/src/main/module.json5` | `deviceTypes` 改为 `['phone', 'tablet', '2in1']`（2026-10-27），权限保持现状 | 支持设备不含智慧屏 |
| `app/build.sh` | 不改，仍只跑 `assembleHap` 出未签名 HAP | 日常调试流程不受影响 |
| `app/hvigorfile.ts` | 不改（不配置 hvigor 签名） | 签名在仓外完成 |

## 8. 需要向华为确认 / 首次执行需实测的点

1. AGC 网页上传是否接受未签名 `.app`（26.0.0+ 的云端重签是否覆盖非 DevEco Studio 上传路径）。
2. 发布包（包内 `phone` + `tablet` + `2in1`，AGC 只勾 PC/2in1）在 AGC 合法性检测中的设备类型校验细节（官方文字是「包内声明范围 ≥ 勾选范围」，当前 3 ≥ 1）。
3. `HapSignLauncher` 对 `.app` 的 `signCode`/`compatibleVersion` 参数行为（官方说 APP 包可签；首次执行以 `verify-app` 为准）。
4. 发布 Profile 的 `READ_PASTEBOARD` ACL 在首发 PC/2in1 下是否会被要求「使用场景视频」等隐私说明材料（PC 侧权限「均可申请」不等于免材料；详见 [appgallery-readiness.md](appgallery-readiness.md) 的 2.5 与 3.6）。
5. 若以后加手机，手机侧 `READ_PASTEBOARD` 被拒时拆 HAP 或改用系统粘贴控件的成本（见 appgallery-readiness 4.2 与 3.13）。
6. `module.targets` 若将来新增 target 复用 `src/main` 的行为是否随工具链升级变化（本文按 26.0.0.821 核对；升级后重跑第 6 节的产物检查）。

## 9. 参考

- 发布应用（`.app`、26.0.0 重签、Release 要求）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-publish-app>
- 命令行搭建流水线（`assembleApp`、签名、`sign-app` 支持 APP 包、material 目录）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-command-line-building-app>
- 工程级 `build-profile.json5`（`signingConfigs` 字段、product 覆盖 `app.json5`）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile-app>
- 模块级 `build-profile.json5`（`targets.config.deviceType` 必须是 `module.json5` 子集）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-build-profile>
- 动态修改编译配置（`hvigorfile.ts` 的 `config.ohos.overrides`）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-hvigor-config-ohos-guide>
- 混淆加固（默认关闭、release 生效、规则与白名单）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/ide-build-obfuscation>
- `app.json5`（`versionCode` 0~2147483647、`versionName` 规则）：<https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/app-configuration-file>
- 创建 HarmonyOS 应用（支持设备含 PC/2in1）：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-create-app-0000002247955506>
- 配置支持设备（包内 `deviceTypes` 范围 ≥ AGC 勾选范围）：<https://developer.huawei.com/consumer/cn/doc/doccenter-submission/agc-help-release-app-devicetype-0000002271592112>
- 申请发布证书（有效期 3 年、CSR 要求）：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-cert-0000002283336729>
- 申请发布 Profile（受限 ACL 勾选）：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-profile-0000002248341090>
- 发布 FAQ（HAP vs APP、APP 不能本地安装、Profile/证书关系、签名不可更换）：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-releasefaq-0000001110342644>
- 上传软件包与合法性检测：<https://developer.huawei.com/consumer/cn/doc/app/agc-help-release-app-upload-pkg-0000002277983368>
