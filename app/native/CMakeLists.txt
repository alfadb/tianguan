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
  add_library(tianguan_diag SHARED diagnostics.cxx napi_bridge.cxx render.cxx)
  target_link_libraries(tianguan_diag PRIVATE tigervnc_client ace_napi.z hilog_ndk.z
    EGL GLESv3 native_window)
  target_link_options(tianguan_diag PRIVATE "-Wl,-z,defs")
else()
  add_executable(tianguan_diag_host diagnostics.cxx host_main.cxx)
  target_link_libraries(tianguan_diag_host PRIVATE tigervnc_client)
endif()
