#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 天关纯逻辑单测统一入口。不需要设备、不需要网络、不修改源码：
#   native: app/native/tests/view_geometry_test.cxx (CTest)
#   ArkTS : app/tests/arkts/connection_codec.test.mjs (node + SDK tsc)
#
# 用法：
#   bash scripts/test.sh            # 全部
#   bash scripts/test.sh --native   # 只跑原生几何单测
#   bash scripts/test.sh --arkts    # 只跑 ArkTS 连接逻辑单测
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NATIVE_BUILD="${REPO_ROOT}/.work/test-native-host"
JOBS="$(nproc 2>/dev/null || echo 2)"

RUN_NATIVE=1
RUN_ARKTS=1

usage() {
  cat <<'EOF'
用法: bash scripts/test.sh [--native | --arkts]

  --native  只跑原生几何单测（cmake + ctest，host 构建）
  --arkts   只跑 ArkTS 连接逻辑单测（node + SDK 自带 tsc）
  无参数    依次跑以上全部
EOF
}

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi
case "${1:-}" in
  "") ;;
  --native) RUN_ARKTS=0 ;;
  --arkts) RUN_NATIVE=0 ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

RESULTS=()
FAILED=0

run_stage() {
  local name="$1"
  shift
  printf '\n== %s ==\n' "$name"
  if "$@"; then
    RESULTS+=("PASS ${name}")
    printf '[PASS] %s\n' "$name"
  else
    RESULTS+=("FAIL ${name}")
    FAILED=1
    printf '[FAIL] %s\n' "$name"
  fi
}

native_stage() {
  if [[ ! -f "${NATIVE_BUILD}/CMakeCache.txt" ]]; then
    # Test-only configure: no TigerVNC checkout / dependency prefixes needed.
    # The geometry test includes app/native/view_geometry.h only.
    cmake -S "${REPO_ROOT}/app/native" -B "${NATIVE_BUILD}" \
      -DCMAKE_BUILD_TYPE=Release -DTIANGUAN_GEOMETRY_TEST_ONLY=ON || return 1
  fi
  cmake --build "${NATIVE_BUILD}" --target tianguan_geometry_test -j "${JOBS}" || return 1
  ctest --test-dir "${NATIVE_BUILD}" --output-on-failure || return 1
}

arkts_stage() {
  node "${REPO_ROOT}/app/tests/arkts/connection_codec.test.mjs" || return 1
}

printf '天关纯逻辑单测（无设备、无网络）\n'
printf '仓库: %s\n' "${REPO_ROOT}"

if [[ "${RUN_NATIVE}" -eq 1 ]]; then
  run_stage "native view_geometry (ctest)" native_stage
fi
if [[ "${RUN_ARKTS}" -eq 1 ]]; then
  run_stage "arkts connection_codec (node)" arkts_stage
fi

printf '\n===== 汇总 =====\n'
for result in "${RESULTS[@]}"; do
  printf '%s\n' "${result}"
done
if [[ "${FAILED}" -ne 0 ]]; then
  printf 'FAILED: 至少一个阶段失败\n'
  exit 1
fi
printf 'ALL PASS\n'
exit 0
