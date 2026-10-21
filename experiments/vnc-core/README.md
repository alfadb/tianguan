# TigerVNC 1.15.0 headless 客户端实验

本目录自有代码为 GPL-3.0-or-later；先按根目录[贡献说明](../../CONTRIBUTING.md)及[许可说明](../../docs/licensing.md)核对授权。无需 CLA。这里只交付独立最小客户端实验，未连接用户 `:1/5901`、未读取用户凭据。原 host CTest 9/9 与独立真实 TigerVNC 1.15.0 RA2_256 harness 3/3 已通过；OHOS API26 ARM64 最新 main 已交叉编译、链接，目标 PC 尚未运行成功。独立诊断 HAP 已构建、未签名未装机，未签包不可直接作为可安装交付；设备尚无任何 native 执行成功，不代表最终内核选型或设备验收通过。

来源为官方 <https://github.com/TigerVNC/tigervnc>，tag v1.15.0，commit `8bdf8734ce12c8ed3c232496fc8a4ccdb04636b8`。CMake 同时检查 HEAD 和顶层 VERSION；使用外部只读 checkout，不下载、不修改、不 vendoring 上游。请使用该 commit 的未修改 checkout；HEAD 检查不代表整个工作树内容哈希验证。

## Host 构建与实测

工作目录为仓库根。需要 CMake >=3.20、C/C++11 编译器、Ninja、pkg-config、Python3 标准库，以及 jpeg/zlib/nettle/hogweed/gmp **和 pixman** 开发文件。此 tag 的 DecodeManager 使用 Region，Region 使用 pixman；这项依赖不能由原先列举的五个库替代。未执行上游顶层，因此无需 X11、FLTK、PAM、GNUTLS。TLS/X509 明确未启用。

默认从仓库根获取固定源码并构建全部六项静态 PIC 依赖，无需安装系统开发包。五依赖脚本每次创建 unique run，**必须使用本次输出的 `PREFIX=`，不可假设存在固定的 `.work/vnc-deps/host/prefix`**。下例在脚本完成后手工填入它打印的绝对路径；pixman 使用稳定的独立 prefix：

```sh
bash scripts/fetch-tigervnc.sh
bash scripts/build-vnc-deps.sh host 4
bash scripts/build-vnc-pixman.sh host 4
# 将下一行替换为上面五依赖脚本本次打印的 PREFIX
HOST_DEPS_PREFIX=/absolute/path/printed/by/build-vnc-deps
cmake -S experiments/vnc-core -B .work/vnc-core-host-pinned -G Ninja \
  -DTIGERVNC_SOURCE_DIR="$PWD/.work/sources/tigervnc-v1.15.0" \
  -DVNC_DEPS_PREFIX="$HOST_DEPS_PREFIX" \
  -DVNC_PIXMAN_PREFIX="$PWD/.work/vnc-pixman/host/prefix"
cmake --build .work/vnc-core-host-pinned -j 4 --verbose
ctest --test-dir .work/vnc-core-host-pinned --output-on-failure
```

显式 `VNC_DEPS_PREFIX` 在 host/cross 均清空 `PKG_CONFIG_PATH`，将 `PKG_CONFIG_LIBDIR` 限于 prefix 的 `lib/pkgconfig` 与 `share/pkgconfig`，关闭 pkg-config 自动添加 CMake prefix，并校验解析出的绝对头/库路径仍在 prefix 内。不存在的 prefix 立即报错；没有指定 prefix 的 host 仍允许本机默认依赖。切换依赖或架构应使用全新构建目录。

`VNC_PIXMAN_PREFIX` 可用于 host 或 cross：显式指定时仅在该 prefix 的 `include/pixman-1`、`lib`/`lib64` 查找 pixman，并校验真实路径（包括符号链接解析后）仍位于该 prefix，旧 pixman 查找缓存也不会覆盖此选择。不指定时保留 `VNC_DEPS_PREFIX` 或本机的默认查找；cross 仍使用目标 root-only 查找并校验 pixman 位于 `VNC_DEPS_PREFIX`，不能回退 host。

本次全新 host 构建使用五依赖 prefix `$PWD/.work/vnc-deps/runs/host.sg5kmiqd/prefix` 与 pixman prefix `$PWD/.work/vnc-pixman/host/prefix`：jpeg 3.1.0、zlib 1.3.2、nettle/hogweed 3.10.1、GMP 6.3.0、pixman 0.42.2。CMake 3.31.6、Ninja 1.12.1、GCC 14.2.0、Python 3.13.5；配置、verbose 构建及 ctest 均退出 0，六项全部通过。完整日志保存在 `.work/vnc-core-host-pinned/{configure.log,build-initial.log,ctest.log}`；`commands.txt` 保留实际命令，最终依赖链接顺序为：

```text
libtigervnc_client.a
.work/vnc-deps/runs/host.sg5kmiqd/prefix/lib/libjpeg.a
.work/vnc-deps/runs/host.sg5kmiqd/prefix/lib/libz.a
.work/vnc-deps/runs/host.sg5kmiqd/prefix/lib/libhogweed.a
.work/vnc-deps/runs/host.sg5kmiqd/prefix/lib/libnettle.a
.work/vnc-deps/runs/host.sg5kmiqd/prefix/lib/libgmp.a
.work/vnc-pixman/host/prefix/lib/libpixman-1.a
```

实际命令使用上述库的绝对路径，没有使用系统 jpeg/zlib/nettle/hogweed/GMP/pixman 共享库。历史 `.work/vnc-core-host-repro` 仅固定 pixman、其余使用系统共享库；本次全固定依赖六项复验结果如下：

```text
1/6 ra2_256_factory Passed
2/6 raw            Passed
3/6 truncated      Passed
4/6 unsupported    Passed
5/6 none_rejected  Passed
6/6 timeout        Passed
100% tests passed, 0 tests failed out of 6
```

## 验证内容及边界

[客户端](main.cxx)继承真正的 `rfb::CConnection`，使用 `network::TcpSocket`、上游输入输出流、`ManagedPixelBuffer` 与解码器。回调包括初始化、光标、颜色表、铃声、认证及帧结束。旧 fixture 为固定 2×1、32bpp、little-endian true-colour；下述独立真实 RA2_256 case 为 64×48，两者不可混称。不支持任意桌面尺寸、索引颜色或桌面重配。

[fixture](fixture.py)只绑定 `127.0.0.1:0` 随机端口。默认 ctest 不连接任何用户服务。可执行文件不带参数只打印用法，不发起连接。只有显式 `--fixture-loopback-none PORT` 才允许 None，地址不能从命令行覆盖。`--fixture-loopback-strict PORT` 只启用 RA2_256/RA2，并被用于证明拒绝 None。另有显式 `--ra2-test PORT --credential-file PATH --server-fingerprint PIN`，仅允许 RA2_256，固定 loopback、端口 1024..65535 且拒绝 5901，供隔离真实服务测试。

- `raw`：断言 RFB 3.8 双向版本、None 选择、安全结果和共享 ClientInit；发送 ServerInit 与一帧 Raw。客户端从上游真实 pixelbuffer 读回字节，核对像素 `112233,445566`；通过上游键盘/鼠标 API 发出事件，fixture 断言 `a` 按下/释放及 `(1,0)` 左键事件。
- `ra2_256_factory`：调用真实 `SecurityClient::GetCSecurity`，检查返回对象是 `CSecurityRSAAES` 且类型为 RA2_256（129），证明实现被编译、链接和实例化。没有执行 RSA 握手、身份确认、凭据交换或认证，不能称为认证成功。
- `truncated`：ServerInit 只发送前 10 字节即关闭，客户端退出 1，输出 `End of stream`。
- `unsupported`：只公布安全类型 127，客户端退出 1，输出 `No matching security types`。
- `none_rejected`：向严格模式公布 None，客户端退出 1，同样拒绝协商。
- `timeout`：服务端接入后不发送版本，客户端在 2 秒协议期限退出 1，输出 `Protocol deadline exceeded`。

旧 fixture 模式协议期限 2 秒，进程 alarm 4 秒覆盖构造和上游潜在阻塞；fixture socket 5 秒、子进程 6 秒、CTest 8 秒。正常超时测试必须捕获客户端自己的期限错误，不能以外层强杀当成功。旧模式 RSA 消息确认及凭据回调仍抛错。

### 独立真实 TigerVNC RA2_256 harness（host 3/3 PASS）

在安装 TigerVNC 1.15.0 `Xtigervnc` 与 X11 开发依赖的 host 上，从仓库根运行[真实服务 harness](ra2-fixture.py)；[X observer](ra2-observer.c)由 harness 在私有 run 目录编译。它独立于默认 CTest：

```sh
python3 experiments/vnc-core/ra2-fixture.py --client .work/vnc-core-host-pinned/vnc-core
```

每 case 使用独立 loopback、高位 display、非 5901 RFB port 与 owned server，未触碰用户 `:1/5901`。本地 run 目录权限 0700，秘密文件 0600，密码与私钥不进入日志；最终所有 owned 进程均已回收。

| case | 已证实结果 | 最终本地证据 |
| --- | --- | --- |
| success | 真实 `authSuccess` 后完成认证；基类解码并 flush 后读回首帧中心 RGB `112233`；server X observer 收到 `a` 按下/释放和 `(16,12)` button1 按下/释放 | `.work/ra2-runs/success-9rjp66wi/result.json` |
| wrong-pin | identity rejected，且在 `getUserPasswd` 前拒绝，无 credential callback、认证成功或首帧 | `.work/ra2-runs/wrong-pin-4pp5u1o5/result.json` |
| wrong-password | exit 1、有 callback、无 AUTH/首帧；owned 服务端唯一连接按顺序出现 RA2_256(129)、精确 `SConnection: Authentication error: Authentication failed` 及同端点 `VNCSConnST: Closing 127.0.0.1::<端口>: Authentication failed`，`server_authentication_rejected=true` | `.work/ra2-runs/wrong-password-r53gfz6v/result.json` |

wrong-password 的服务端核对在 owned 进程退出、日志句柄关闭后进行，不以 exit 1、耗时或泛化的断连消息代替密码拒绝证据。只打印 callback 后 exit 1、从不连接的假 client 反例被判 **FAIL（预期）**：服务端接受连接、RA2 请求、认证错误与匹配关闭计数全为 0；证据为 `.work/ra2-runs/wrong-password-nejaylfh/result.json`。这是防假通过反例，不加入正式 3/3 通过数。

真实模式协议期限 15 秒、alarm 20 秒；凭据仅在身份确认成功后从 `--credential-file` 指定的 0600 普通单行文件读取，不经 argv/env 传密码。`getUserPasswd` 入口仅输出无秘密固定标记 `RA2_256 credential callback`。pin 是上游 **64-bit 截断指纹**（八组小写十六进制字节），不是完整公钥 pin；本隔离实验不是生产身份校验方案。上述结果仅证明 host 真实 RA2_256 链路，不证明 RA2 的独立互通、目标 PC 运行或产品验收。

## 实际上游源码清单

[CMake](CMakeLists.txt)显式编译下列 55 个 translation units 为实验私有静态库 `tigervnc_client`。这不是上游不存在的 rfbclient/core target，也不是上游混合 client/server 的完整 rfb 库。配置时在构建目录生成 `upstream-sources.txt` 作为绝对路径清单。所有名称均相对上游 `common/`：

```text
os/: Mutex.cxx Thread.cxx os.cxx
network/: Socket.cxx TcpSocket.cxx
rdr/: AESInStream.cxx AESOutStream.cxx BufferedInStream.cxx
  BufferedOutStream.cxx Exception.cxx FdInStream.cxx FdOutStream.cxx
  FileInStream.cxx HexInStream.cxx HexOutStream.cxx RandomStream.cxx
  ZlibInStream.cxx ZlibOutStream.cxx
rfb/: KeysymStr.c Region.cxx CConnection.cxx CMsgHandler.cxx CMsgReader.cxx
  CMsgWriter.cxx CSecurityPlain.cxx CSecurityStack.cxx CSecurityVeNCrypt.cxx
  CSecurityVncAuth.cxx CSecurityDH.cxx CSecurityMSLogonII.cxx
  CSecurityRSAAES.cxx Configuration.cxx CopyRectDecoder.cxx Cursor.cxx
  DecodeManager.cxx Decoder.cxx d3des.c HextileDecoder.cxx
  JpegDecompressor.cxx LogWriter.cxx Logger.cxx Logger_file.cxx
  Logger_stdio.cxx PixelBuffer.cxx PixelFormat.cxx RREDecoder.cxx
  RawDecoder.cxx ServerParams.cxx Security.cxx SecurityClient.cxx
  Timer.cxx TightDecoder.cxx ZRLEDecoder.cxx encodings.cxx util.cxx
```

## 有界许可核对

本次检查上游 `LICENCE.TXT`（GPLv2 原文），并按 Ninja 实际依赖记录检查 131 个被编译器使用的上游源码/头文件的文件头。55 个 translation units 中 53 个具有 GPL-2.0-or-later 明示；全部 131 个输入中 126 个有同样明示。版权包括 RealVNC、Cendio、TigerVNC Team、Red Hat 等，仍保存在外部原始 checkout，未重写其授权。

例外单列，不能把整个上游粗略统一标成一种许可：

- `rfb/KeysymStr.c`：The Open Group 许可，须保留版权、许可、免责声明及名称使用约束。
- `rfb/keysymdef.h`：The Open Group 与 Digital Equipment Corporation 的宽松许可及名称使用约束。
- `rfb/ks_tables.h`：声明由 keysymdef.h 生成，没有独立完整许可正文，保留生成来源与对应上游声明；分发前需进一步确认其归属及 notice 组合。
- `rfb/d3des.c`：Richard Outerbridge 的 public-domain 原实现声明，及 AT&T 1999 修改版权/免责声明；`rfb/d3des.h` 有对应版权及来源但无独立完整授权正文。本实验保留原文、依上游组合使用，不把其文件头擅自改写成 GPL 声明；正式分发需补核这部分授权证据。

本机依赖核对范围限于实际链接库的 Debian copyright 元数据和相关许可段落，未逐文件审计这些库的完整源码：

| 库 | 本机许可证据 | 记录 |
| --- | --- | --- |
| libjpeg-turbo | `/usr/share/doc/libjpeg62-turbo/copyright` | IJG 条款、BSD、zlib 等按文件划分；this software is based in part on the work of the Independent JPEG Group |
| zlib | `/usr/share/doc/zlib1g/copyright` | Zlib，保留来源、许可及修改标识要求 |
| nettle / hogweed | `/usr/share/doc/libnettle8t64/copyright`、`/usr/share/doc/libhogweed6t64/copyright` | LGPL-3.0-or-later 或 GPL-2.0-or-later；个别文件可有宽松例外 |
| GMP | `/usr/share/doc/libgmp10/copyright` | LGPL-3.0-or-later 或 GPL-2.0-or-later；此处不含其文档分发结论 |
| pixman | `/usr/share/doc/libpixman-1-0/copyright` | MIT 风格，保留版权及许可声明 |

此记录仅支持本地可行性实验。没有发布二进制、没有复制上游代码到仓库，不是完整产品分发审计；后续分发须提供相应源码及所有适用许可/版权/NOTICE，补查依赖构建选项、文件例外、工具链运行时、SDK、商店与签名条件。根目录候选状态文档未被本实验改写。

## OHOS ARM64 API26 真实构建记录（PASS，实验适配）

先生成同 ABI 的目标依赖；五依赖脚本同样每次生成 unique run，采用本次打印的 `PREFIX=`。不要复用 host 缓存。下面是本轮实际执行并成功完成链接的配置/构建参数：

```sh
bash scripts/build-vnc-deps.sh ohos 4
bash scripts/build-vnc-pixman.sh ohos-arm64 4
# 将下一行替换为五依赖脚本本次打印的 PREFIX
OHOS_DEPS_PREFIX=/absolute/path/printed/by/build-vnc-deps
cmake -S experiments/vnc-core -B .work/vnc-core-ohos-api26 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/native/build/cmake/ohos.toolchain.cmake \
  -DOHOS_ARCH=arm64-v8a -DOHOS_PLATFORM=OHOS \
  -DOHOS_COMPATIBLE_SDK_VERSION=26 \
  -DTIGERVNC_SOURCE_DIR="$PWD/.work/sources/tigervnc-v1.15.0" \
  -DVNC_DEPS_PREFIX="$OHOS_DEPS_PREFIX" \
  -DVNC_PIXMAN_PREFIX="$PWD/.work/vnc-pixman/ohos-arm64/prefix" \
  -DBUILD_TESTING=OFF
cmake --build .work/vnc-core-ohos-api26 -j 4 --verbose
```

本轮复用五依赖 `$PWD/.work/vnc-deps/runs/ohos.1nGSC7hS/prefix` 和 pixman `$PWD/.work/vnc-pixman/ohos-arm64/prefix`，没有重建或修改依赖。SDK toolchain 读取 `OHOS_COMPATIBLE_SDK_VERSION=26`，实际生成 `--target=aarch64-linux-ohos26.0.0`。上述 configure 与 verbose build 均退出 **0**，最终生成 `.work/vnc-core-ohos-api26/vnc-core`。

历史阻塞是 SDK26 没有 `pthread_cancel`，而上游 `Thread::~Thread()` 的 `if (isRunning())` 分支引用它。[显式补丁](ohos-thread.patch)仅在这一分支以 `TG_OHOS_NO_PTHREAD_CANCEL` 选择 `__builtin_trap()`，否则保留原取消行为；原版权头、析构及正常清理保持。CMake 仅在 OHOS 正式目标和 host 专用适配测试启用此宏，不伪造 `__OHOS__`。每次配置用 `configure_file(COPYONLY)` 从只读上游刷新构建目录 `ohos-adaptation/Thread.cxx`，再调用 `patch --batch --fuzz=0`；失败立即终止配置，重复配置不会叠加补丁。正式源清单仍是 **55** 项，仅 OHOS 的 Thread 路径替换为构建副本，Decoder/RA2 均保留。

这是实验适配：**不支持析构仍在运行的线程，此时终止整个进程**，不是取消实现，不是空操作，也不增加无条件 join。真实 `DecodeThread` 析构先 `stop(); wait()`，由 worker 协作退出；其在目标设备上的正常退出仍待验证。

Host 原正式目标仍使用未补丁的上游 Thread，原六项全部通过。新增 [生命周期探针](thread-lifecycle.cxx)与[子进程检查](thread-lifecycle.py)使用同一补丁 Thread、真实 Mutex/Condition：worker 进入条件等待后，正常 `start → stop（标志+广播）→ wait → delete` 返回 0；运行中 `delete` 返回 `-SIGILL`（本机为 -4），禁 core dump，每个子进程 3 秒超时。外置同步状态保持到进程终止，避免派生析构先销毁 worker 的同步对象。host 专用 `vnc-core-ohos-test` 通过 OBJECT 链接补丁 Thread，复用原 Raw 和 truncated fixture，覆盖真实 DecodeThread 析构。CTest 总计 **9/9** 通过，命令 `cmake --build .work/vnc-core-host-pinned -j 4` 与 `ctest --test-dir .work/vnc-core-host-pinned --output-on-failure` 均退出 0；最初新增探针因缺少上游 `rfb::format` 链接失败（退出 1），已通过链接现有真实客户端库解决。`nm -u` 验证原 host 有 `pthread_cancel` 引用，专用适配二进制没有。

目标 `readelf -h -l -d` 和 `nm -C` 实查退出 0，完整证据分别保存在 `.work/vnc-core-ohos-api26/elf-adapted.txt`、`symbols-adapted.txt`：

```text
Machine: AArch64
Requesting program interpreter: /lib/ld-musl-aarch64.so.1
NEEDED: libdeviceinfo_ndk.z.so
NEEDED: libc++_shared.so
NEEDED: libc.so
00000000000676f4 T rfb::CSecurityRSAAES::processMsg()
000000000007bdc8 T rfb::RawDecoder::decodeRect(...)
```

无 host 动态库、无 RPATH/RUNPATH、无 `pthread_cancel` 符号（含未定义引用）。最终链接显式使用目标 prefix 的 jpeg/zlib/hogweed/nettle/GMP/pixman 六个静态库。上述历史构建轮次未访问设备；新增 `--ra2-test` 及 callback 标记后的最新 main 也已在 `.work/vnc-core-ohos-device` 交叉编译成功，但尚未在设备上运行成功。旧 `configure-initial.log`、`build-initial.log`、`configure-final.log`、`build-final.log` 是适配前失败记录，不能当作当前构建结论。

Cross 强制要求 `VNC_DEPS_PREFIX`，五依赖头/库/包采用 root-only 查找；显式 pixman prefix 只影响 pixman。目标 `.pc` 文件必须填写目标 prefix，不能保留 host `/usr` 路径。负向配置记录保存在 `.work/vnc-core-prefix-checks/`：`host-missing.log` 退出 1（`VNC_DEPS_PREFIX does not exist`），`cross-no-prefix.log` 退出 1（`Cross builds require VNC_DEPS_PREFIX`），`cross-empty.log` 退出 1（五个 pkg-config 包全部 not found），均未回退系统依赖。

| 项目 | 本轮状态 | 证据与边界 |
| --- | --- | --- |
| host 全固定依赖 | PASS | configure/build/ctest 退出 0；原 6/6，适配测试新增 3/3；六项依赖均为 prefix `.a` |
| OHOS ARM64 API26 build | PASS（实验适配） | configure/build 退出 0；最终 AArch64 ELF、解释器、动态依赖及符号已验收；未运行 |
| RA2_256 真实认证 | host PASS 3/3 | 独立真实 success / wrong-pin / wrong-password；服务端拒绝证据与防假通过反例见上文；不是设备验收 |
| OHOS 设备 | NOT RUN（未成功执行） | `/data/local/tmp` 原未签与 selfsigned 副本均 execution 126；Docs 在 chmod 被拒后停止，未执行 capabilities |
| 应用内诊断 | unsigned HAP 已构建 | 同源 host 3/3 PASS，AArch64 NAPI 共享库及包内真实库已校验；未签名、未安装，设备未执行 |

目标 PC 已确认是 MateBookFold SLM-W32、API26 ARM64。原未签 `vnc-core` 在 `/data/local/tmp` 返回 execution 126 / `Permission denied`；SDK 官方 `selfSign1` 副本经 `display-sign` 确认 selfsigned 后，同目录仍 execution 126，不能归因于缺签名。按[官方 Docs/hishell 说明](https://consumer.huawei.com/cn/support/content/zh-cn16078461/)已能新建/投递到 Docs，但 `chmod` 被拒（remote 1）后即停止，Docs 的 capabilities 根本未执行，不能记为 Docs exec126 或 hishell 失败。`hdc` 返回 0 不等于程序返回 0，具体策略原因未确定；未改变任何设备安全属性，PC 程序未运行成功。[官方二进制签名文档](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides/binary-sign-tool)及 Docs/hishell 支持不等于 hdc 身份具有执行权限。

为走正式应用加载路线，独立诊断 HAP `cn.alfadb.tianguan` 0.0.1 / code 1、API26、仅 2in1 已构建，见[应用说明](../../app/README.md)。`app/native` 真实共享库已构建为 `.work/hap-native-ohos/libtianguan_diag.so`（shared ELF AArch64），包含真正的 NAPI async + promise。同源 host 诊断 3/3 PASS：RA2 工厂、完整内存 RFB 3.8 Raw 2×1、真实 Thread 协作退出；无 socket/main/alarm。`bash app/build.sh` 已退出 0，包内真实 `libtianguan_diag.so` 与 `libc++_shared.so` 已校验，无 `requestPermissions`；启动自动运行纯诊断并保存 `filesDir/diag-result.txt`。这不是上述真实服务器 3/3，也不代表设备执行。HAP 未签名、未装机，未签包不可直接作为可安装交付；安装前仍需匹配 `cn.alfadb.tianguan` 与 MateBookFold 的 debug profile，不能默认使用 NetBird profile。UI、中文 IME、X509、RDP、PC native 与 HAP 安装均未通过，根文档四项产品验收目标仍未整体通过。

host 产物 `.work/vnc-core-host-pinned/vnc-core` 的 `readelf -h -l -d` 记录在 `elf.txt`：X86-64，interpreter `/lib64/ld-linux-x86-64.so.2`，NEEDED 仅 `libstdc++.so.6`、`libm.so.6`、`libgcc_s.so.1`、`libc.so.6`、`ld-linux-x86-64.so.2`，无 RPATH/RUNPATH。`nm -C --defined-only` 记录在 `symbols.txt`，含 `CSecurityRSAAES::processMsg()`、其构造函数及 `RawDecoder::decodeRect(...)` 实现。

本页汇总不同轮次的已证实结果；历史六项、线程适配后的九项、真实服务三项与应用同源诊断三项分别记录，不合并为产品通过数。文件级许可边界保持上述有界核对范围，实验进展不代表正式内核选型。
