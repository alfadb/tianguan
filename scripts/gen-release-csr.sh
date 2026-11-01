#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 天关发布证书请求（CSR）生成脚本：在本机生成发布密钥库（.p12）与证书请求（.csr），
# 之后把 .csr 上传 AGC「证书管理」换回发布证书 .cer，再创建「发布」类型 Profile。
#
# 用法：
#   bash scripts/gen-release-csr.sh
#
# 环境变量（都可省略）：
#   TIANGUAN_RELEASE_DIR            输出目录，必须在仓库外
#                                   默认 $HOME/harmonyos-signing/tianguan/release（0700）
#   TIANGUAN_RELEASE_P12_PASS_FILE  口令文件（单行明文，0600，仓外）
#                                   不设置则在本终端交互输入两次（不回显）
#   ALIAS                            key alias（默认 tianguan-release）
#   DN                              证书主体（默认 CN=tianguan release,O=AlfaDB,C=CN）
#   VALIDITY                         有效期天数（默认 3650，约 10 年）
#
# 安全约定：本脚本不生成、不保存、不打印任何口令；口令只经 keytool 的
# -storepass:file（口令文件）或标准输入（交互输入）传入，不进命令行参数、
# 不进环境变量、不写入日志。私钥只在 .p12 里，脚本不读取也不输出其内容。
set -euo pipefail

OUT_DIR="${TIANGUAN_RELEASE_DIR:-$HOME/harmonyos-signing/tianguan/release}"
ALIAS="${ALIAS:-tianguan-release}"
DN="${DN:-CN=tianguan release,O=AlfaDB,C=CN}"
VALIDITY="${VALIDITY:-3650}"
PASS_FILE="${TIANGUAN_RELEASE_P12_PASS_FILE:-}"

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
info() { printf '\n==> %s\n' "$*"; }

# --- 前置检查 -------------------------------------------------------------
command -v keytool >/dev/null 2>&1 || fail "未找到 keytool（需要 JDK）"
command -v openssl >/dev/null 2>&1 || fail "未找到 openssl（校验 CSR 需要）"

[[ "$ALIAS" =~ ^[A-Za-z0-9._-]+$ ]] \
  || fail "ALIAS 只允许字母、数字、点、下划线、连字符（它同时是文件名）：$ALIAS"
[[ "$VALIDITY" =~ ^[1-9][0-9]*$ ]] || fail "VALIDITY 必须是正整数天数：$VALIDITY"
[[ -n "$DN" ]] || fail "DN 不能为空"

# keytool 对 EC 密钥的参数因 JDK 版本而异：优先 -groupname，不支持时回退 -keysize。
if keytool -genkeypair -help 2>&1 | grep -q -- '-groupname'; then
  EC_PARAM=(-groupname secp256r1)
else
  EC_PARAM=(-keysize 256)
  printf '提示：当前 keytool 不支持 -groupname，回退 -keysize 256（secp256r1 / P-256）\n'
fi

# --- 输出目录与覆盖检查 ---------------------------------------------------
mkdir -p "$OUT_DIR"
chmod 700 "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
P12="$OUT_DIR/$ALIAS.p12"
CSR="$OUT_DIR/$ALIAS.csr"

[[ ! -e "$P12" ]] || fail "已存在同名密钥库，拒绝覆盖：$P12
发布证书私钥丢失后无法更换签名，确需重建请先手工移走旧 .p12，并同步重新申请发布证书与 Profile。"
[[ ! -e "$CSR" ]] || fail "已存在同名 CSR，拒绝覆盖：$CSR"

# --- 口令 -----------------------------------------------------------------
# 两种模式都保证口令不出现在 keytool 的 argv 里。
if [[ -n "$PASS_FILE" ]]; then
  [[ -f "$PASS_FILE" ]] || fail "口令文件不存在：$PASS_FILE"
  [[ -r "$PASS_FILE" ]] || fail "口令文件不可读：$PASS_FILE"
  printf '口令来源：文件 %s（内容不读取、不回显）\n' "$PASS_FILE"
else
  printf '请输入参与签名的 keystore 口令（keystore 与 key 共用，至少 6 位，不回显）\n'
  read -rs -p '  口令        : ' PASS1 || fail "未读到口令（标准输入已结束）"
  printf '\n'
  read -rs -p '  再次输入口令: ' PASS2 || fail "未读到第二次口令（标准输入已结束）"
  printf '\n'
  [[ "$PASS1" == "$PASS2" ]] || fail "两次输入的口令不一致，未生成任何文件"
  [[ "${#PASS1}" -ge 6 ]] || fail "口令至少 6 个字符（keytool 要求）"
  PASS="$PASS1"
  unset PASS1 PASS2
fi

# keytool 读口令的两种方式：文件模式用 -storepass:file / -keypass:file；
# 交互模式经标准输入喂入（重复若干次以兼容不同 JDK 的重复确认提示）。
keytool_genkey() {
  if [[ -n "$PASS_FILE" ]]; then
    keytool "${GEN_ARGS[@]}" -storepass:file "$PASS_FILE" -keypass:file "$PASS_FILE"
  else
    for _ in 1 2 3 4; do printf '%s\n' "$PASS"; done | keytool "${GEN_ARGS[@]}"
  fi
}
keytool_certreq() {
  if [[ -n "$PASS_FILE" ]]; then
    keytool "${REQ_ARGS[@]}" -storepass:file "$PASS_FILE"
  else
    for _ in 1 2; do printf '%s\n' "$PASS"; done | keytool "${REQ_ARGS[@]}"
  fi
}

# --- 生成密钥对 -----------------------------------------------------------
GEN_ARGS=(-genkeypair -keystore "$P12" -storetype PKCS12 -alias "$ALIAS"
  -keyalg EC "${EC_PARAM[@]}" -sigalg SHA256withECDSA -validity "$VALIDITY"
  -dname "$DN")

info "生成 EC 密钥对（PKCS#12）"
keytool_genkey

[[ -f "$P12" ]] || fail "keytool 结束但未生成密钥库：$P12"
chmod 600 "$P12"

# --- 生成 CSR -------------------------------------------------------------
REQ_ARGS=(-certreq -keystore "$P12" -alias "$ALIAS" -sigalg SHA256withECDSA
  -file "$CSR")

info "生成证书请求（CSR）"
keytool_certreq

[[ -f "$CSR" ]] || fail "keytool 结束但未生成 CSR：$CSR"
chmod 644 "$CSR"

# --- 校验 CSR -------------------------------------------------------------
info "校验 CSR（openssl，只含公钥与签名，不含私钥）"
CSR_TEXT="$(openssl req -in "$CSR" -noout -verify -text 2>&1)" \
  || fail "openssl 读取 CSR 失败：$CSR"
printf '%s\n' "$CSR_TEXT"
grep -q 'verify OK' <<<"$CSR_TEXT" || fail "CSR 自签名校验未通过：$CSR"

# 交互模式的口令只在生成期间需要，校验完成后立即丢弃。
if [[ -z "$PASS_FILE" ]]; then
  unset PASS || true
fi

# --- 结果与后续步骤 -------------------------------------------------------
P12_SHA="$(sha256sum "$P12" | cut -d' ' -f1)"
CSR_SHA="$(sha256sum "$CSR" | cut -d' ' -f1)"

printf '\n发布密钥库 : %s\n' "$P12"
printf 'SHA-256    : %s\n' "$P12_SHA"
printf 'CSR        : %s\n' "$CSR"
printf 'SHA-256    : %s\n' "$CSR_SHA"
printf 'key alias  : %s\n' "$ALIAS"

cat <<EOF

下一步（在 AGC 网页完成，脚本不会联网上传任何东西）：
  1. AGC → 证书管理 → 新增证书 → 类型「发布证书」→ 上传上面的 .csr → 下载 .cer
  2. AGC → Profile → 添加 → 类型「发布」→ 选刚签发的发布证书
     → 勾「受限 ACL 权限」中的 ohos.permission.READ_PASTEBOARD
     → 设备形态勾 PC/2in1 → 审批后下载 .p7b
  3. 回到本机，用 scripts/build-release.sh 签名（五个变量全部给出才会签）：
       export TIANGUAN_RELEASE_P12=$P12
       export TIANGUAN_RELEASE_CER=/path/to/release.cer
       export TIANGUAN_RELEASE_PROFILE=/path/to/release.p7b
       export TIANGUAN_RELEASE_P12_PASS_FILE=/path/to/release.p12.pass
       export TIANGUAN_RELEASE_ALIAS=$ALIAS
       bash scripts/build-release.sh

注意：.p12 是发布签名私钥，HarmonyOS 应用不支持更换签名，请长期备份并留在仓库外；
口令不要写进仓库、命令历史或任何文档。
EOF
