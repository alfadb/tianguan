#!/usr/bin/env bash
set -euo pipefail
APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$APP_ROOT")"
SDK=/home/worker/harmonyos/command-line-tools/26.0.0.821
NATIVE="$SDK/sdk/default/openharmony/native"
SO_SOURCE="${1:-$REPO_ROOT/.work/hap-native-ohos/libtianguan_diag.so}"
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
[[ -f "$SO_SOURCE" ]] || fail "缺少原生诊断库：$SO_SOURCE；请先完成 native 构建，或将库路径作为第一个参数传入。"
READELF="$NATIVE/llvm/bin/llvm-readelf"
HEADER="$("$READELF" -h "$SO_SOURCE")"
[[ "$HEADER" == *AArch64* && "$HEADER" == *DYN* ]] || fail '原生库必须为 AArch64 ELF shared object'
DEPENDENCIES="$("$READELF" -d "$SO_SOURCE")"
printf '%s\n' "$DEPENDENCIES"
mkdir -p "$APP_ROOT/entry/libs/arm64-v8a"
cp "$SO_SOURCE" "$APP_ROOT/entry/libs/arm64-v8a/libtianguan_diag.so"
if [[ "$DEPENDENCIES" == *libc++_shared.so* ]]; then
  CXX="$NATIVE/llvm/lib/aarch64-linux-ohos/libc++_shared.so"
  [[ -f "$CXX" ]] || fail "缺少 SDK C++ runtime：$CXX"
  cp "$CXX" "$APP_ROOT/entry/libs/arm64-v8a/libc++_shared.so"
fi
export DEVECO_SDK_HOME="$SDK"
export DEVECO_NODE_HOME="$SDK/tool/node"
export PATH="$SDK/tool/node/bin:$SDK/ohpm/bin:$PATH"
cd "$APP_ROOT"
"$SDK/ohpm/bin/ohpm" install
"$SDK/bin/hvigorw" --no-daemon assembleHap --mode module -p product=default -p buildMode=release
HAP="$APP_ROOT/entry/build/default/outputs/default/entry-default-unsigned.hap"
[[ -f "$HAP" ]] || fail "未找到 unsigned HAP：$HAP"
printf 'Unsigned HAP: %s\n' "$HAP"
sha256sum "$HAP"
