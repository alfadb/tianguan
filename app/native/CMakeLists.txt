# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(tianguan_diag LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_TESTING OFF CACHE BOOL "Disable upstream experiment tests" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../experiments/vnc-core"
  "${CMAKE_CURRENT_BINARY_DIR}/vnc-core" EXCLUDE_FROM_ALL)
set_target_properties(tigervnc_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  # FreeRDP 3.9.0 (RDP client) static prefix built by
  # `scripts/build-rdp-deps.sh ohos`. The default is the repository-local
  # .work prefix that script installs to; override with
  # -DRDP_DEPS_PREFIX=<prefix> for a different location. The prefix provides
  # AArch64 static PIC archives and headers only (no .so): the RDP stack is
  # linked into libtianguan_diag.so without adding any shared dependency.
  set(RDP_DEPS_PREFIX
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.work/rdp-deps/ohos/prefix"
    CACHE PATH "FreeRDP 3.9.0 / OpenSSL / zlib static prefix for OHOS")
  add_library(tianguan_diag SHARED
    diagnostics.cxx napi_bridge.cxx render.cxx rdp_backend.cxx)
  target_compile_definitions(tianguan_diag PRIVATE TIANGUAN_WITH_RDP=1)
  target_include_directories(tianguan_diag PRIVATE
    "${RDP_DEPS_PREFIX}/include/freerdp3"
    "${RDP_DEPS_PREFIX}/include/winpr3"
    "${RDP_DEPS_PREFIX}/include")
  target_link_directories(tianguan_diag PRIVATE "${RDP_DEPS_PREFIX}/lib")
  target_link_libraries(tianguan_diag PRIVATE tigervnc_client ace_napi.z hilog_ndk.z
    EGL GLESv3 native_window
    # Static FreeRDP stack, in link order: client-common, core, WinPR, then the
    # static OpenSSL/zlib and libc dependencies. No shared FreeRDP/OpenSSL.
    freerdp-client3 freerdp3 winpr3 ssl crypto z pthread dl m)
  target_link_options(tianguan_diag PRIVATE "-Wl,-z,defs")
else()
  # Host build deliberately excludes rdp_backend.cxx: the OHOS FreeRDP prefix
  # is AArch64-only and its static OpenSSL is incompatible with the host's
  # shared system OpenSSL. diagnostics.cxx compiles its RDP entry point out
  # unless TIANGUAN_WITH_RDP is defined.
  add_executable(tianguan_diag_host diagnostics.cxx host_main.cxx)
  target_link_libraries(tianguan_diag_host PRIVATE tigervnc_client)
endif()
