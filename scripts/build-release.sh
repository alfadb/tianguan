#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 天关 release 构建入口：产出未签名 .app（App Pack），可选用外部发布材料签名。
#
# 用法：
#   bash scripts/build-release.sh
#
# 默认只构建未签名包。在 app/ 下实际执行的命令：
#   <SDK>/bin/hvigorw --no-daemon clean assembleApp --mode project \
#     -p product=default -p buildMode=release
# 产物：
#   app/build/outputs/default/app-default-unsigned.app
#
# 可选签名：下面 5 个变量全部给出时才启用；缺任意一个就只产出未签名包并提示。
#   TIANGUAN_RELEASE_P12            发布证书私钥 keystore（仓外，.p12）
#   TIANGUAN_RELEASE_CER            发布证书（仓外，.cer）
#   TIANGUAN_RELEASE_PROFILE        发布 Profile（仓外，已签名 .p7b）
#   TIANGUAN_RELEASE_P12_PASS_FILE  口令文件（单行明文，权限 0600，仓外）
#   TIANGUAN_RELEASE_ALIAS          keystore 内的 key alias
# 签名产物：.work/release/app-default-signed.app
# 口令只经文件读入 char[]，不出现在命令行参数、环境变量、仓库或日志里。
# 签名通过仓外的 HapSignLauncher.java 调用 SDK 的 hap-sign-tool（官方 CLI 只支持
# argv 或交互式口令，无法安全地使用口令文件）；可用 TIANGUAN_HAP_SIGN_LAUNCHER 指定其路径。
#
# 其它环境变量：
#   TIANGUAN_SDK       HarmonyOS 命令行工具根目录
#                      （默认 /home/worker/harmonyos/command-line-tools/26.0.0.821）
#   TIANGUAN_HVIGORW   指定 hvigorw 可执行文件（默认 $TIANGUAN_SDK/bin/hvigorw，其次 PATH）
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_ROOT="$REPO_ROOT/app"
OUT_ROOT="$REPO_ROOT/.work/release"
SDK="${TIANGUAN_SDK:-/home/worker/harmonyos/command-line-tools/26.0.0.821}"
JAR="$SDK/sdk/default/openharmony/toolchains/lib/hap-sign-tool.jar"
LAUNCHER="${TIANGUAN_HAP_SIGN_LAUNCHER:-$HOME/harmonyos-signing/HapSignLauncher.java}"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '\n==> %s\n' "$*"; }

# --- hvigorw 检查 ---------------------------------------------------------
HVIGORW="${TIANGUAN_HVIGORW:-}"
if [[ -z "$HVIGORW" ]]; then
  if [[ -x "$SDK/bin/hvigorw" ]]; then
    HVIGORW="$SDK/bin/hvigorw"
  elif command -v hvigorw >/dev/null 2>&1; then
    HVIGORW="$(command -v hvigorw)"
  else
    fail "未找到 hvigorw：请设置 TIANGUAN_SDK 指向 HarmonyOS 命令行工具，或用 TIANGUAN_HVIGORW 指定可执行文件"
  fi
fi
[[ -x "$HVIGORW" ]] || fail "hvigorw 不存在或不可执行：$HVIGORW"
[[ -d "$APP_ROOT" ]] || fail "缺少应用目录：$APP_ROOT"
OHPM="$SDK/ohpm/bin/ohpm"
[[ -x "$OHPM" ]] || fail "缺少 ohpm：$OHPM（用 TIANGUAN_SDK 指定命令行工具目录）"

export DEVECO_SDK_HOME="$SDK"
export DEVECO_NODE_HOME="$SDK/tool/node"
export PATH="$SDK/tool/node/bin:$SDK/bin:$SDK/ohpm/bin:$PATH"

printf '天关 release 构建（未签名 .app）\n'
printf '仓库  : %s\n' "$REPO_ROOT"
printf 'SDK   : %s\n' "$SDK"
printf 'hvigor: %s\n' "$HVIGORW"

cd "$APP_ROOT"

info "安装 ohpm 依赖"
"$OHPM" install

info "构建 release 未签名 .app"
"$HVIGORW" --no-daemon clean assembleApp --mode project -p product=default -p buildMode=release

# --- 产物定位与校验 -------------------------------------------------------
info "定位 .app 产物"
EXPECTED_APP="$APP_ROOT/build/outputs/default/app-default-unsigned.app"
APP_FILE=""
if [[ -f "$EXPECTED_APP" ]]; then
  APP_FILE="$EXPECTED_APP"
else
  APP_FILE="$(find "$APP_ROOT/build/outputs" -type f -name '*.app' -printf '%T@ %p\n' 2>/dev/null \
    | sort -nr | head -n 1 | cut -d' ' -f2-)" || true
fi
[[ -n "$APP_FILE" && -f "$APP_FILE" ]] || fail "assembleApp 成功但未找到 .app 产物（查找范围：$APP_ROOT/build/outputs）"

ARCHIVE_LIST="$(unzip -l "$APP_FILE")" || fail "无法读取 .app 归档：$APP_FILE"
grep -qE '[[:space:]]pack\.info$' <<<"$ARCHIVE_LIST" \
  || fail ".app 内缺少 pack.info：$APP_FILE"
grep -qE '[[:space:]]entry-[^/[:space:]]*\.hap$' <<<"$ARCHIVE_LIST" \
  || fail ".app 内缺少 entry-*.hap：$APP_FILE"

APP_SIZE="$(stat -c '%s' "$APP_FILE")"
APP_SHA256="$(sha256sum "$APP_FILE" | cut -d' ' -f1)"

printf '%s\n' "$ARCHIVE_LIST"
printf '\nApp Pack : %s\n' "$APP_FILE"
printf '大小     : %s bytes\n' "$APP_SIZE"
printf 'SHA-256  : %s\n' "$APP_SHA256"

# --- 可选签名 -------------------------------------------------------------
SIGN_VARS=(TIANGUAN_RELEASE_P12 TIANGUAN_RELEASE_CER TIANGUAN_RELEASE_PROFILE \
  TIANGUAN_RELEASE_P12_PASS_FILE TIANGUAN_RELEASE_ALIAS)
MISSING=()
PRESENT=0
for var in "${SIGN_VARS[@]}"; do
  if [[ -n "${!var:-}" ]]; then
    PRESENT=$((PRESENT + 1))
  else
    MISSING+=("$var")
  fi
done

if [[ "$PRESENT" -eq 0 ]]; then
  printf '\n提示：未设置发布签名变量，只产出未签名 .app。设置 %s 后会自动签名。\n' \
    "TIANGUAN_RELEASE_P12 / _CER / _PROFILE / _P12_PASS_FILE / _ALIAS"
  exit 0
fi
if [[ "${#MISSING[@]}" -gt 0 ]]; then
  printf '\n警告：签名变量不完整，跳过签名（缺少：%s）。仅产出未签名 .app。\n' \
    "${MISSING[*]}"
  exit 0
fi

info "签名 .app"
for f in "$TIANGUAN_RELEASE_P12" "$TIANGUAN_RELEASE_CER" "$TIANGUAN_RELEASE_PROFILE" \
  "$TIANGUAN_RELEASE_P12_PASS_FILE"; do
  [[ -f "$f" ]] || fail "签名输入文件不存在：$f"
done
[[ -f "$JAR" ]] || fail "缺少 hap-sign-tool：$JAR"
[[ -f "$LAUNCHER" ]] || fail "缺少签名启动器：$LAUNCHER（用 TIANGUAN_HAP_SIGN_LAUNCHER 指定 HapSignLauncher.java 路径）"
command -v javac >/dev/null 2>&1 || fail "未找到 javac（编译仓外签名启动器需要 JDK）"

mkdir -p "$OUT_ROOT"
BASE_NAME="$(basename "$APP_FILE" .app)"
SIGNED_APP="$OUT_ROOT/${BASE_NAME%-unsigned}-signed.app"
[[ ! -e "$SIGNED_APP" ]] || fail "签名输出已存在，拒绝覆盖：$SIGNED_APP"

CLASSES="$(mktemp -d "${TMPDIR:-/tmp}/hap-sign-classes.XXXXXX")"
trap 'rm -rf -- "$CLASSES"' EXIT
javac -cp "$JAR" -d "$CLASSES" "$LAUNCHER"
java -cp "$JAR:$CLASSES" HapSignLauncher \
  "$TIANGUAN_RELEASE_P12" \
  "$TIANGUAN_RELEASE_P12_PASS_FILE" \
  "$TIANGUAN_RELEASE_CER" \
  "$TIANGUAN_RELEASE_PROFILE" \
  "$APP_FILE" "$SIGNED_APP" "$TIANGUAN_RELEASE_ALIAS" 26
rm -rf -- "$CLASSES"
trap - EXIT

[[ -f "$SIGNED_APP" ]] || fail "签名命令结束但未生成：$SIGNED_APP"
SIGNED_SIZE="$(stat -c '%s' "$SIGNED_APP")"
SIGNED_SHA256="$(sha256sum "$SIGNED_APP" | cut -d' ' -f1)"
printf '\nSigned App Pack: %s\n' "$SIGNED_APP"
printf '大小          : %s bytes\n' "$SIGNED_SIZE"
printf 'SHA-256       : %s\n' "$SIGNED_SHA256"
printf '提示：.app 不能本地安装，只能验证结构与签名；安装验证请使用 .hap。\n'
