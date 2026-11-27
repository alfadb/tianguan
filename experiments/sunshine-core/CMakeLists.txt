# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(tianguan_sunshine_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# 客户端身份 / XML / nvhttp 端点：命令行工具与（未来的）拉流客户端共用。
set(SUNSHINE_CORE_COMMON_SOURCES
  src/common/identity.cxx
  src/common/nv_http.cxx
  src/common/pairing.cxx
  src/common/xml_util.cxx)

add_executable(tg-sunshine-ctl
  ${SUNSHINE_CORE_COMMON_SOURCES}
  src/ctl/main.cxx)
target_include_directories(tg-sunshine-ctl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tg-sunshine-ctl PRIVATE
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads)

# 仓库根：experiments/sunshine-core 往上两级。
get_filename_component(TIANGUAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SUNSHINE_DEPS_DIR "${TIANGUAN_ROOT}/.work/sunshine-deps/host")

# 拉流客户端由同事编写；目录为空时不生成目标，避免空源文件列表报错。
file(GLOB SUNSHINE_STREAM_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/stream/*.cxx")
if(SUNSHINE_STREAM_SOURCES)
  add_executable(tg-sunshine-stream ${SUNSHINE_STREAM_SOURCES})
  target_include_directories(tg-sunshine-stream PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SUNSHINE_DEPS_DIR}/include")
  target_link_libraries(tg-sunshine-stream PRIVATE
    "${SUNSHINE_DEPS_DIR}/lib/static/libmoonlight-common-c.a"
    "${SUNSHINE_DEPS_DIR}/lib/static/libenet.a"
    OpenSSL::Crypto
    Threads::Threads
    m)
else()
  message(STATUS "src/stream 下暂无 .cxx，跳过 tg-sunshine-stream")
endif()
