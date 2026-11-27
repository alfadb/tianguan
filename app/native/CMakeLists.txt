# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(tianguan_diag LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_TESTING OFF CACHE BOOL "Disable upstream experiment tests" FORCE)
# scripts/test.sh configures with this ON to build ONLY the host unit test:
# the TigerVNC experiment subdirectory needs a pinned upstream checkout and
# prebuilt dependency prefixes, which the pure view-geometry test must not
# require. Default OFF keeps every normal host/OHOS build unchanged.
option(TIANGUAN_GEOMETRY_TEST_ONLY "Configure only the host-side unit test" OFF)
if(NOT TIANGUAN_GEOMETRY_TEST_ONLY)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../experiments/vnc-core"
    "${CMAKE_CURRENT_BINARY_DIR}/vnc-core" EXCLUDE_FROM_ALL)
  set_target_properties(tigervnc_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  # FreeRDP 3.9.0 (RDP client) static prefix built by
  # `scripts/build-rdp-deps.sh ohos`. The default is the repository-local
  # .work prefix that script installs to; override with
  # -DRDP_DEPS_PREFIX=<prefix> for a different location. The prefix provides
  # AArch64 static PIC archives and headers only (no .so): the RDP stack is
  # linked into libtianguan_diag.so without adding any shared dependency.
  set(RDP_DEPS_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.work/rdp-deps/ohos/prefix"
    CACHE PATH "FreeRDP 3.9.0 / FFmpeg 7.1.5 / OpenSSL / zlib static prefix for OHOS")
  # moonlight-common-c + enet + opus（Moonlight 串流运行库），由
  # `scripts/build-sunshine-deps.sh ohos` 构建到仓库内的 .work 前缀。
  # 同样是 AArch64 静态 PIC 归档，只提供头文件与 .a。
  set(SUNSHINE_DEPS_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.work/sunshine-deps/ohos/prefix"
    CACHE PATH "moonlight-common-c / enet / opus static prefix for OHOS")
  add_library(tianguan_diag SHARED
    diagnostics.cxx napi_bridge.cxx render.cxx rdp_backend.cxx
    # Sunshine/Moonlight 会话后端：nvhttp 控制面 -> pairing -> launch ->
    # common-c 串流；视频是 AVCodec Buffer 模式硬解 -> libswscale 转 BGRA ->
    # 宿主帧镜像，音频是 libopus 多流解码 -> OH_AudioRenderer 拉模式 + 预缓冲。
    # 全部源码为 gnu++11，异常保持开启（本目标没有 -fno-exceptions）。
    sunshine_backend.cxx sunshine_video.cxx sunshine_audio.cxx
    # Sunshine/Moonlight 控制面公共层（客户端身份、极简 XML、nvhttp 端点、
    # /pair 五阶段配对、服务端证书指纹固定）。
    sunshine/identity.cxx sunshine/xml_util.cxx sunshine/nv_http.cxx
    sunshine/pairing.cxx)
  target_compile_definitions(tianguan_diag PRIVATE TIANGUAN_WITH_RDP=1
    TIANGUAN_WITH_SUNSHINE=1)
  target_include_directories(tianguan_diag PRIVATE
    "${RDP_DEPS_PREFIX}/include/freerdp3"
    "${RDP_DEPS_PREFIX}/include/winpr3"
    "${RDP_DEPS_PREFIX}/include"
    "${SUNSHINE_DEPS_PREFIX}/include")
  target_link_directories(tianguan_diag PRIVATE "${RDP_DEPS_PREFIX}/lib")
  target_link_libraries(tianguan_diag PRIVATE tigervnc_client ace_napi.z hilog_ndk.z
    # ohaudio is the OHOS AudioRenderer NDK (libohaudio.so), used by
    # rdp_backend.cxx for RDP audio playback (rdpsnd). It is a platform system
    # library, so the shared object gains exactly one more system NEEDED entry.
    EGL GLESv3 native_window ohaudio
    # OHOS AVCodec 硬解（sunshine_video.cxx，Buffer 模式）。都是系统 NDK 库，
    # 因此共享对象按下面顺序新增四条 NEEDED：
    #   native_media_vdec        OH_VideoDecoder_*（CreateByMime/Configure/
    #                            RegisterCallback/Prepare/Start/PushInputBuffer/
    #                            FreeOutputBuffer/GetOutputDescription/Stop/Destroy）
    #   native_media_core        OH_AVBuffer_* / OH_AVFormat_*
    #   native_media_codecbase   OH_MD_KEY_* / OH_AVCODEC_MIMETYPE_* 常量，
    #                            以及 sunshine_backend.cxx 的 HEVC 能力探测
    #                            OH_AVCodec_GetCapabilityByCategory（原先用
    #                            dlopen/dlsym 动态解析，现在直链）
    #   native_buffer            OH_NativeBuffer_GetConfig / Unreference（输出
    #                            缓冲 stride 的兜底来源）
    native_media_vdec native_media_core native_media_codecbase native_buffer
    # Static FreeRDP stack, in link order: client-common, core, WinPR, then the
    # static OpenSSL/zlib dependencies. No shared FreeRDP/OpenSSL.
    freerdp-client3 freerdp3 winpr3
    # moonlight-common-c 与它的依赖（enet 提供控制流传输，opus 提供音频多流
    # 解码）。必须排在 ssl/crypto 之前：common-c 需要 libcrypto 的 EVP_*/RAND_*
    # 符号，-Wl,-z,defs 才能闭包。绝对路径同样按上面顺序展开为静态归档。
    "${SUNSHINE_DEPS_PREFIX}/lib/libmoonlight-common-c.a"
    "${SUNSHINE_DEPS_PREFIX}/lib/libenet.a"
    "${SUNSHINE_DEPS_PREFIX}/lib/libopus.a"
    ssl crypto z
    # Static FFmpeg 7.1.5 archives from the same prefix, last as the dependency
    # manifest orders them (FreeRDP was built WITH_FFMPEG/WITH_SWSCALE, but its
    # .pc files do not list the libav* archives). libavcodec provides the GFX
    # H.264 decoder (AVC420/AVC444) and the rdpsnd compressed-audio decoders
    # (AAC/AAC-LATM), libswscale the YUV->RGB conversion, libswresample the
    # decoder sample-rate/channel conversion, libavutil the common runtime.
    # They are archives only: -Wl,-z,defs must still close over them here.
    avcodec swscale swresample avutil
    # libc dependencies last (libavcodec needs pthread/dl/m too).
    pthread dl m)
  target_link_options(tianguan_diag PRIVATE "-Wl,-z,defs")
else()
  # Host build deliberately excludes rdp_backend.cxx: the OHOS FreeRDP prefix
  # is AArch64-only and its static OpenSSL is incompatible with the host's
  # shared system OpenSSL. diagnostics.cxx compiles its RDP entry point out
  # unless TIANGUAN_WITH_RDP is defined.
  # sunshine/ is OHOS-only for the same reason (it needs the OpenSSL headers
  # from that prefix). The host build has no TIANGUAN_WITH_SUNSHINE either:
  # parseSessionOptions() still accepts protocol=sunshine, but sessionMain()
  # fails it explicitly instead of running the VNC loop.
  # The diagnostics host binary links the TigerVNC experiment; in test-only
  # mode that subtree is not configured, so only the pure test is built.
  if(NOT TIANGUAN_GEOMETRY_TEST_ONLY)
    add_executable(tianguan_diag_host diagnostics.cxx host_main.cxx)
    target_link_libraries(tianguan_diag_host PRIVATE tigervnc_client)
  endif()

  # Pure-logic host unit test for the view transform geometry (no EGL/GLES, no
  # device). `ctest` in this build directory runs it; scripts/test.sh does.
  enable_testing()
  add_executable(tianguan_geometry_test tests/view_geometry_test.cxx)
  add_test(NAME tianguan_geometry_test COMMAND tianguan_geometry_test)
endif()
