#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# 天关真机冒烟：安装（可选）→ 启动 → 找连接卡片 → 点连接 → 等首帧 →
# 断言无失败 → 截图 → 断开 → 断言回到未连接。
#
# 用法：
#   bash scripts/smoke-device.sh <device-serial> [--hap <签名包路径>]
#       [--name <连接名>] [--timeout <秒>] [--keep]
#
# 依赖：hdc（优先 $HDC，缺省用固定 SDK 的 hdc）、设备已解锁且已保存目标连接。
# 不改 connections.json、不读密钥库、不卸载应用；结束时清理设备临时文件并恢复
# 临时延长的息屏时间。
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_HDC="/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/toolchains/hdc"
HDC="${HDC:-$DEFAULT_HDC}"
if [ ! -x "$HDC" ]; then
  HDC_RESOLVED=$(command -v "$HDC" 2>/dev/null || true)
  [ -n "$HDC_RESOLVED" ] && HDC="$HDC_RESOLVED"
fi
BUNDLE="cn.alfadb.tianguan"
ABILITY="EntryAbility"

SERIAL=""
HAP=""
NAME=""
TIMEOUT=60
KEEP=0
LIST_WIN_ID=""

usage() {
  cat <<'EOF'
用法: bash scripts/smoke-device.sh <device-serial> [选项]

  <device-serial>      hdc 目标标识（无线设备如 192.168.50.199:37193）
  --hap <路径>         先安装该签名包（hdc install -r，绝不卸载）
  --name <连接名>      要连接的卡片名；缺省选第一张可连接的卡片
  --timeout <秒>       等待首帧的上限，默认 60
  --keep               保留设备 /data/local/tmp 下的本次临时文件
  -h, --help           显示本帮助

示例:
  bash scripts/smoke-device.sh 192.168.50.199:37193 --name 62
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --hap)
      [ "$#" -ge 2 ] || { printf '%s\n' "--hap 需要一个路径" >&2; exit 2; }
      HAP="$2"; shift 2 ;;
    --name)
      [ "$#" -ge 2 ] || { printf '%s\n' "--name 需要一个连接名" >&2; exit 2; }
      NAME="$2"; shift 2 ;;
    --timeout)
      [ "$#" -ge 2 ] || { printf '%s\n' "--timeout 需要一个秒数" >&2; exit 2; }
      TIMEOUT="$2"; shift 2 ;;
    --keep)
      KEEP=1; shift ;;
    -h|--help)
      usage; exit 0 ;;
    --)
      shift
      while [ "$#" -gt 0 ]; do
        if [ -z "$SERIAL" ]; then SERIAL="$1"; else
          printf '多余的参数: %s\n' "$1" >&2; usage >&2; exit 2
        fi
        shift
      done ;;
    -*)
      printf '未知参数: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    *)
      if [ -z "$SERIAL" ]; then
        SERIAL="$1"
      else
        printf '只接受一个设备标识，多余: %s\n' "$1" >&2; usage >&2; exit 2
      fi
      shift ;;
  esac
done

if [ -z "$SERIAL" ]; then
  printf '缺少设备标识\n' >&2
  usage >&2
  exit 2
fi
case "$TIMEOUT" in
  ''|*[!0-9]*) printf '无效的 --timeout: %s\n' "$TIMEOUT" >&2; exit 2 ;;
esac
if [ "$TIMEOUT" -lt 1 ]; then
  printf '%s\n' "--timeout 必须大于 0" >&2
  exit 2
fi
if [ -n "$HAP" ] && [ ! -f "$HAP" ]; then
  printf '签名包不存在: %s\n' "$HAP" >&2
  exit 2
fi

RUN_TS="$(date +%Y%m%d-%H%M%S)"
EVIDENCE_DIR="${REPO_ROOT}/.work/smoke/${RUN_TS}"
n=2
while [ -e "$EVIDENCE_DIR" ]; do
  EVIDENCE_DIR="${REPO_ROOT}/.work/smoke/${RUN_TS}-${n}"
  n=$((n + 1))
done
mkdir -p "$EVIDENCE_DIR"
LOG_FILE="${EVIDENCE_DIR}/smoke.log"
REMOTE_DIR="/data/local/tmp/tianguan-smoke-${RUN_TS}-$$"
REMOTE_CREATED=0
TIMEOUT_OVERRIDDEN=0
HILOG_TAG_SET=0

log() {
  printf '%s\n' "$*" | tee -a "$LOG_FILE"
}

cleanup() {
  local rc=$?
  trap - EXIT INT TERM HUP
  if [ "$REMOTE_CREATED" -eq 1 ] && [ -x "$HDC" ] && [ -n "$SERIAL" ]; then
    if [ "$KEEP" -eq 1 ]; then
      printf '设备临时目录（--keep 保留）: %s\n' "$REMOTE_DIR" >>"$LOG_FILE"
    else
      "$HDC" -t "$SERIAL" shell "rm -rf '$REMOTE_DIR'" >/dev/null 2>&1 || true
      printf '已清理设备临时目录: %s\n' "$REMOTE_DIR" >>"$LOG_FILE"
    fi
  fi
  if [ "$TIMEOUT_OVERRIDDEN" -eq 1 ] && [ -x "$HDC" ] && [ -n "$SERIAL" ]; then
    "$HDC" -t "$SERIAL" shell "power-shell timeout -r" >/dev/null 2>&1 || true
    printf '已恢复设备息屏时间设置\n' >>"$LOG_FILE"
  fi
  if [ "$HILOG_TAG_SET" -eq 1 ] && [ -x "$HDC" ] && [ -n "$SERIAL" ]; then
    "$HDC" -t "$SERIAL" shell "hilog -b I -T TianguanDiag" >/dev/null 2>&1 || true
    printf '已恢复 TianguanDiag 日志级别\n' >>"$LOG_FILE"
  fi
  exit "$rc"
}
trap cleanup EXIT INT TERM HUP

hdc_sh() {
  local cmd="$1" out="" rc=0
  out=$("$HDC" -t "$SERIAL" shell "$cmd" 2>&1) || rc=$?
  printf '\n$ hdc -t %s shell %s\n%s\n' "$SERIAL" "$cmd" "$out" >>"$LOG_FILE"
  printf '%s' "$out"
  return "$rc"
}

hdc_exec() {
  local out="" rc=0
  out=$("$HDC" -t "$SERIAL" "$@" 2>&1) || rc=$?
  printf '\n$ hdc -t %s %s\n%s\n' "$SERIAL" "$*" "$out" >>"$LOG_FILE"
  printf '%s' "$out"
  return "$rc"
}

hdc_plain() {
  local out="" rc=0
  out=$("$HDC" "$@" 2>&1) || rc=$?
  printf '\n$ hdc %s\n%s\n' "$*" "$out" >>"$LOG_FILE"
  printf '%s' "$out"
  return "$rc"
}

# uitest dumpLayout 的 JSON 解析。所有模式都只读本地文件：
#   find-card <file> <name>  找卡片；name 为空时选第一张可连接卡片
#   state <file> <name>      等待首帧/断开阶段的状态汇总
#   texts <file>             打印所有 tianguan 窗口文本
#   find-text <file> <text>  找一个文本节点的点击坐标
layout_py() {
  python3 - "$@" <<'PY'
import json
import re
import sys

MODE = sys.argv[1]
PATH = sys.argv[2]
NAME = sys.argv[3] if len(sys.argv) > 3 else ""

PRIMARY = {"连接", "打开", "重连"}
STATUS_RE = re.compile(r"^(未连接|连接中|已连接·\d+个|重连中|已断开|需要确认身份|失败(：.*)?)$")
ENDPOINT_RE = re.compile(r"^(VNC|RDP)\s{2}")
TIME_RE = re.compile(r"^\d{2}-\d{2} \d{2}:\d{2}$")
BOUNDS_RE = re.compile(r"^\[(-?\d+),(-?\d+)\]\[(-?\d+),(-?\d+)\]$")


def attrs(node):
    return node.get("attributes") or {}


def text(node):
    value = attrs(node).get("text") or ""
    return value.replace("\r", " ").replace("\n", " ")


def ntype(node):
    return attrs(node).get("type") or ""


def bundle(node):
    return attrs(node).get("bundleName") or ""


def clickable(node):
    return attrs(node).get("clickable") == "true"


def bounds(node):
    match = BOUNDS_RE.match(attrs(node).get("bounds") or "")
    if not match:
        return None
    return tuple(int(value) for value in match.groups())


def center(box):
    return ((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)


def children(node):
    return node.get("children") or []


def walk(node):
    yield node
    for child in children(node):
        yield from walk(child)


def roots_of_tree(tree):
    found = []

    def visit(node):
        if bundle(node) == "cn.alfadb.tianguan":
            found.append(node)
            return
        for child in children(node):
            visit(child)

    visit(tree)
    return found


def parent_map(tree):
    parents = {}

    def visit(node):
        for child in children(node):
            parents[id(child)] = node
            visit(child)

    visit(tree)
    return parents


def is_primary(node):
    return text(node) in PRIMARY and (ntype(node) == "Button" or clickable(node))


def is_meta(value):
    return (not value or value in PRIMARY or value == "⋯" or
            bool(STATUS_RE.match(value)) or bool(ENDPOINT_RE.match(value)) or
            bool(TIME_RE.match(value)) or value == "未连接过")


def is_name_like(node):
    # 卡片名是 Text；工具栏的「新建连接」也是文本按钮，不能当名字。
    return ntype(node) != "Button" and not is_meta(text(node))


def card_of(node, parents):
    current = node
    while current is not None:
        descendants = list(walk(current))
        primaries = [item for item in descendants if is_primary(item)]
        menus = [item for item in descendants if text(item) == "⋯"]
        # 真正的卡片一定带状态标签；缺少状态时宁可不认，避免把整页
        # （含「新建连接」工具栏按钮）误当成一张卡片。
        has_status = any(STATUS_RE.match(text(item)) for item in descendants)
        if primaries and menus and has_status and any(is_name_like(item) for item in descendants):
            return current, primaries[0], menus[0]
        current = parents.get(id(current))
    return None


def build_card(card, primary, menu):
    values = [(text(item), item) for item in walk(card) if text(item)]
    names = [value for value, item in values if is_name_like(item)]
    status = ""
    for value, _ in values:
        if STATUS_RE.match(value):
            status = value
            break
    return {
        "name": names[0] if names else "",
        "status": status,
        "button": text(primary),
        "primary": primary,
        "menu": menu,
        "bounds": bounds(card),
    }


def collect_cards(tree):
    cards = []
    seen = set()
    parents = parent_map(tree)
    for root in roots_of_tree(tree):
        for node in walk(root):
            if not is_primary(node):
                continue
            found = card_of(node, parents)
            if found is None:
                continue
            card, primary, menu = found
            if id(card) in seen:
                continue
            seen.add(id(card))
            cards.append(build_card(card, primary, menu))
    return cards


def card_line(card):
    print("FOUND=1")
    print("CHOSEN=" + card["name"])
    print("STATUS=" + card["status"])
    print("BUTTON=" + card["button"])
    primary_box = bounds(card["primary"])
    if primary_box:
        px, py = center(primary_box)
        print("BX=%d" % px)
        print("BY=%d" % py)
    menu_box = bounds(card["menu"])
    if menu_box:
        mx, my = center(menu_box)
        print("MX=%d" % mx)
        print("MY=%d" % my)


def all_texts(tree):
    values = []
    for root in roots_of_tree(tree):
        for node in walk(root):
            value = text(node)
            if value:
                values.append(value)
    return values


def find_card(tree):
    cards = collect_cards(tree)
    if NAME:
        matches = [card for card in cards if card["name"] == NAME]
    else:
        connectable = [card for card in cards if card["button"] == "连接"]
        matches = connectable[:1] if connectable else cards[:1]
    if not matches:
        print("FOUND=0")
        for card in cards:
            print("VISIBLE=" + card["name"])
        return 1
    card_line(matches[0])
    for other in cards:
        print("VISIBLE=" + other["name"])
    return 0


def state(tree):
    cards = collect_cards(tree)
    card = None
    if NAME:
        for item in cards:
            if item["name"] == NAME:
                card = item
                break
    if card is None and not NAME and cards:
        card = cards[0]
    values = all_texts(tree)
    print("HAS_WAIT=%d" % (1 if any("等待首帧" in value for value in values) else 0))
    print("HAS_FAIL=%d" % (1 if any("失败" in value or "重连中" in value for value in values) else 0))
    print("HAS_NAME=%d" % (1 if any(value == NAME for value in values) else 0))
    print("CARD_FOUND=%d" % (1 if card is not None else 0))
    print("CARD_STATUS=" + (card["status"] if card else ""))
    print("CARD_BUTTON=" + (card["button"] if card else ""))
    print("CONNECTED=%d" % (1 if card and card["status"].startswith("已连接") else 0))
    print("REMOTE_WINDOWS=%d" % sum(
        1 for root in roots_of_tree(tree)
        if not any(is_primary(node) for node in walk(root)) and
        (not NAME or any(text(node) == NAME for node in walk(root)))))
    print("REMOTE_PAGE=%d" % (
        1 if any(attrs(root).get("pagePath") == "pages/Remote" for root in roots_of_tree(tree)) else 0))
    for value in values:
        print("TEXT=" + value)
    return 0


def find_text(tree, target):
    for root in roots_of_tree(tree):
        for node in walk(root):
            if text(node) != target:
                continue
            box = bounds(node)
            if box is None:
                continue
            x, y = center(box)
            print("FOUND=1")
            print("X=%d" % x)
            print("Y=%d" % y)
            return 0
    print("FOUND=0")
    return 1


try:
    with open(PATH, "r", encoding="utf-8") as handle:
        tree = json.load(handle)
except Exception as error:  # noqa: BLE001 - dump files are external input
    print("PARSE_ERROR=" + str(error))
    sys.exit(1)

if MODE == "find-card":
    sys.exit(find_card(tree))
if MODE == "state":
    sys.exit(state(tree))
if MODE == "texts":
    for item in all_texts(tree):
        print("TEXT=" + item)
    sys.exit(0)
if MODE == "find-text":
    sys.exit(find_text(tree, sys.argv[3] if len(sys.argv) > 3 else ""))
print("PARSE_ERROR=unknown mode " + MODE)
sys.exit(1)
PY
}

get_kv() {
  local blob="$1" key="$2"
  printf '%s\n' "$blob" | awk -v k="$key" 'index($0, k "=") == 1 { print substr($0, length(k) + 2); exit }'
}

get_all_kv() {
  local blob="$1" key="$2"
  printf '%s\n' "$blob" | awk -v k="$key" 'index($0, k "=") == 1 { print substr($0, length(k) + 2) }'
}

wms_focus_is_locked() {
  local wms="$1" focus_id focus_name
  focus_id=$(printf '%s\n' "$wms" | sed -n 's/^Focus window: *\([0-9][0-9]*\).*/\1/p' | head -n 1)
  [ -n "$focus_id" ] || return 1
  focus_name=$(printf '%s\n' "$wms" | awk -v id="$focus_id" '$4 == id { print $1; exit }')
  case "$focus_name" in
    SCBScreenLock*) return 0 ;;
  esac
  return 1
}

wake_displays() {
  local wms="${1:-}" display
  if [ -n "$wms" ]; then
    for display in $(printf '%s\n' "$wms" | sed -n 's/^ScreenGroup \([0-9][0-9]*\).*/\1/p'); do
      hdc_sh "uitest uiInput keyEvent 2070 $display" >/dev/null 2>&1 || true
    done
  else
    hdc_sh "uitest uiInput keyEvent 2070 0" >/dev/null 2>&1 || true
  fi
}

RESOLVE_ERROR=""
resolve_display_id() {
  local attempt=1 wms display tianguan0_id any_id
  while [ "$attempt" -le 6 ]; do
    wms=$(hdc_sh "hidumper -s WindowManagerService -a '-a'" || true)
    if [ -z "$wms" ] || ! printf '%s\n' "$wms" | grep -q 'WindowManagerService'; then
      if [ "$attempt" -ge 4 ]; then
        RESOLVE_ERROR="hidumper WindowManagerService 无有效输出（服务可能拒绝 dump）"
      fi
      sleep 2
      attempt=$((attempt + 1))
      continue
    fi
    tianguan0_id=$(printf '%s\n' "$wms" | awk '$1 == "tianguan0" { print $2; exit }')
    if [ -n "$tianguan0_id" ]; then
      printf '%s\n' "$tianguan0_id"
      return 0
    fi
    any_id=$(printf '%s\n' "$wms" | awk '$1 ~ /^tianguan[0-9]+$/ { print $2; exit }')
    if [ -n "$any_id" ]; then
      printf '%s\n' "$any_id"
      return 0
    fi
    if wms_focus_is_locked "$wms"; then
      RESOLVE_ERROR="设备处于锁屏（焦点窗口为 SCBScreenLock*），请先解锁并保持亮屏"
      return 1
    fi
    case "$attempt" in
      1|4)
        hdc_sh "aa start -a $ABILITY -b $BUNDLE" >/dev/null 2>&1 || true ;;
      2)
        wake_displays "$wms" ;;
    esac
    sleep 2
    attempt=$((attempt + 1))
  done
  RESOLVE_ERROR="未找到 tianguan 窗口（hidumper WindowManagerService 无 tianguan*）"
  return 1
}

ALL_STEPS=(preflight install launch find-card wait-frame screenshot disconnect)
declare -A RESULT_OF=()
FINISHED=0
FAILED=0

record() {
  RESULT_OF["$1"]="$2|${3:-}"
}

step_pass() {
  record "$1" PASS "${2:-}"
  log "[PASS] $1${2:+ — $2}"
}

step_skip() {
  record "$1" SKIP "${2:-}"
  log "[SKIP] $1${2:+ — $2}"
}

finish() {
  [ "$FINISHED" -eq 1 ] && return 0
  FINISHED=1
  local step entry status detail
  log ""
  log "===== 汇总 ====="
  for step in "${ALL_STEPS[@]}"; do
    entry="${RESULT_OF[$step]:-SKIP|未执行}"
    status="${entry%%|*}"
    detail="${entry#*|}"
    if [ "$detail" = "$entry" ]; then detail=""; fi
    if [ -n "$detail" ]; then
      log "$status  $step — $detail"
    else
      log "$status  $step"
    fi
  done
  log "证据目录: $EVIDENCE_DIR"
  if [ "$FAILED" -eq 0 ]; then
    log "SMOKE PASS"
    exit 0
  fi
  log "SMOKE FAIL"
  exit 1
}

step_fail() {
  record "$1" FAIL "${2:-}"
  log "[FAIL] $1${2:+ — $2}"
  FAILED=1
  finish
}

on_error() {
  local rc="$1" line="$2"
  [ "$FINISHED" -eq 1 ] && exit "$rc"
  log "[FAIL] internal — 第 ${line} 行命令失败（exit ${rc}）"
  FAILED=1
  record internal FAIL "第 ${line} 行 exit ${rc}"
  finish
}
trap 'on_error "$?" "$LINENO"' ERR

show_texts() {
  local dump="$1" line value found=0
  log "界面文本（$(basename "$dump")）:"
  if [ -z "$dump" ] || [ ! -s "$dump" ]; then
    log "  (无 dump)"
    return 0
  fi
  while IFS= read -r line; do
    value="${line#TEXT=}"
    log "  $value"
    found=1
  done < <(layout_py texts "$dump" 2>/dev/null || true)
  if [ "$found" -eq 0 ]; then
    log "  (无 tianguan 文本)"
  fi
}

# 手机上 Remote 页面没有标题栏，dumpLayout 里看不到连接名/等待文案；
# 用应用自己的 TianguanPerf 渲染日志补齐首帧证据：
#   render={state:running,...,frames:N,...}，N>0 即已有渲染帧。
# 只读当前这次启动的 pid，force-stop 后旧会话日志不会混入。
latest_perf_line() {
  local pid="$1" line=""
  line=$("$HDC" -t "$SERIAL" shell "hilog -x -T TianguanDiag -P $pid" 2>/dev/null |
    grep 'TianguanPerf' | tail -n 1 || true)
  [ -n "$line" ] || return 1
  printf '%s\n' "$line" >>"${EVIDENCE_DIR}/hilog-perf.txt"
  printf '%s\n' "$line"
}

perf_frames() {
  printf '%s\n' "$1" | sed -n 's/.*render={[^}]*frames:\([0-9][0-9]*\).*/\1/p' | tail -n 1
}

perf_session() {
  printf '%s\n' "$1" | sed -n 's/.*session=\([^ ]*\).*/\1/p' | head -n 1
}

perf_state() {
  printf '%s\n' "$1" | sed -n 's/.*render={state:\([a-zA-Z_-]*\).*/\1/p' | head -n 1
}

dump_layout() {
  local label="$1" win_id="${2:-}" attempt=1 remote local_path window_arg=""
  [ -n "$win_id" ] && window_arg="-w $win_id"
  while [ "$attempt" -le 2 ]; do
    remote="${REMOTE_DIR}/${label}.json"
    local_path="${EVIDENCE_DIR}/${label}.json"
    hdc_sh "uitest dumpLayout -d ${DISPLAY_ID:-0} $window_arg -p '$remote'" >/dev/null 2>&1 || true
    case "$local_path" in
      "$EVIDENCE_DIR"/*) rm -f -- "$local_path" ;;
    esac
    hdc_exec file recv "$remote" "$local_path" >/dev/null 2>&1 || true
    if [ -s "$local_path" ] && python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$local_path" >/dev/null 2>&1; then
      printf '%s\n' "$local_path"
      return 0
    fi
    if [ "$attempt" -eq 1 ]; then
      log "[WARN] ${label}: dumpLayout 失败，唤醒屏幕后重试"
      wake_displays
      sleep 1
    fi
    attempt=$((attempt + 1))
  done
  return 1
}

log "天关真机冒烟"
log "设备: $SERIAL"
log "bundle: $BUNDLE"
log "连接名: ${NAME:-<自动选择>}"
log "等待首帧上限: ${TIMEOUT}s"
log "证据目录: $EVIDENCE_DIR"

# ---------------------------------------------------------------------------
# 1. preflight：hdc、tconn、list targets、displayId（含锁屏检查）
# ---------------------------------------------------------------------------
if [ ! -x "$HDC" ]; then
  step_fail preflight "HDC 不存在或不可执行: $HDC"
fi

if [[ "$SERIAL" == *:* ]]; then
  tconn_out=$(hdc_plain tconn "$SERIAL" || true)
  log "hdc tconn: $(printf '%s' "$tconn_out" | tr '\n' ' ')"
  if ! printf '%s' "$tconn_out" | grep -qE 'Connect OK|Target is connected'; then
    log "[WARN] tconn 未明确返回 Connect OK，继续用 list targets 判断"
  fi
fi

targets=$(hdc_plain list targets || true)
if ! printf '%s\n' "$targets" | grep -Fxq "$SERIAL"; then
  step_fail preflight "hdc list targets 不含 $SERIAL（实际: $(printf '%s' "$targets" | tr '\n' ' ')）"
fi

if ! hdc_sh "mkdir -p '$REMOTE_DIR'" >/dev/null 2>&1; then
  step_fail preflight "无法创建设备临时目录 $REMOTE_DIR"
fi
REMOTE_CREATED=1

AWAKE_MS=$(( (TIMEOUT + 180) * 1000 ))
if hdc_sh "power-shell timeout -o $AWAKE_MS" >/dev/null 2>&1; then
  TIMEOUT_OVERRIDDEN=1
  log "已临时延长息屏时间到 $((AWAKE_MS / 1000)) 秒（结束时恢复）"
else
  log "[WARN] 无法临时延长息屏时间，继续"
fi

# 当前 Remote 页的 TianguanPerf（含 render frames）是 debug 级日志；
# 只把应用自己的 tag 临时调到 D，结束时恢复 I。
if hdc_sh "hilog -b D -T TianguanDiag" >/dev/null 2>&1; then
  HILOG_TAG_SET=1
  log "已临时把 TianguanDiag 日志级别调到 D（结束时恢复 I）"
else
  log "[WARN] 无法调整 TianguanDiag 日志级别，首帧判定将依赖界面文本"
fi

wms=$(hdc_sh "hidumper -s WindowManagerService -a '-a'" || true)
wake_displays "$wms"
sleep 1
wms=$(hdc_sh "hidumper -s WindowManagerService -a '-a'" || true)
if [ -n "$wms" ] && wms_focus_is_locked "$wms"; then
  step_fail preflight "设备处于锁屏，无法自动解锁；请先解锁手机并保持亮屏后重试"
fi

if ! DISPLAY_ID=$(resolve_display_id); then
  step_fail preflight "${RESOLVE_ERROR:-无法确定 tianguan 窗口的 displayId}"
fi
step_pass preflight "displayId=$DISPLAY_ID"

# ---------------------------------------------------------------------------
# 2. install：给了 --hap 才做；只 install -r，绝不卸载
# ---------------------------------------------------------------------------
if [ -z "$HAP" ]; then
  step_skip install "未指定 --hap"
else
  hdc_sh "aa force-stop $BUNDLE" >/dev/null 2>&1 || true
  install_out=$(hdc_exec install -r "$HAP" || true)
  if printf '%s' "$install_out" | grep -q 'install bundle successfully'; then
    step_pass install "$HAP"
  else
    log "install 输出:"
    printf '%s\n' "$install_out" | while IFS= read -r line; do log "  $line"; done
    step_fail install "hdc install -r 输出不含 install bundle successfully"
  fi
fi

# ---------------------------------------------------------------------------
# 3. launch：force-stop → start → pidof 非空
# ---------------------------------------------------------------------------
hdc_sh "aa force-stop $BUNDLE" >/dev/null 2>&1 || true
sleep 1
launch_out=$(hdc_sh "aa start -a $ABILITY -b $BUNDLE" || true)
if printf '%s' "$launch_out" | grep -qi 'error'; then
  if printf '%s' "$launch_out" | grep -qi 'locked'; then
    step_fail launch "设备锁屏，aa start 被拒绝；请解锁后重试"
  fi
  log "aa start 输出:"
  printf '%s\n' "$launch_out" | while IFS= read -r line; do log "  $line"; done
  step_fail launch "aa start 失败"
fi
sleep 3
pid_out=$(hdc_sh "pidof $BUNDLE" || true)
PID=$(printf '%s\n' "$pid_out" | awk 'NF { print $1; exit }')
if [ -z "$PID" ]; then
  step_fail launch "启动 3 秒后 pidof $BUNDLE 为空"
fi
# force-stop + start 可能把主窗口换到另一个显示器，重新取一次。
if new_display=$(resolve_display_id); then
  if [ "$new_display" != "$DISPLAY_ID" ]; then
    log "[INFO] 启动后 displayId 由 $DISPLAY_ID 变为 $new_display"
    DISPLAY_ID="$new_display"
  fi
else
  step_fail launch "启动后无法确定 tianguan displayId: ${RESOLVE_ERROR:-未知原因}"
fi
# 主列表窗口的 WinId：远程窗口在前台时，用它按窗口 dump 出卡片状态。
wms_list=$(hdc_sh "hidumper -s WindowManagerService -a '-a'" || true)
LIST_WIN_ID=$(printf '%s\n' "$wms_list" | awk '$1 == "tianguan0" { print $4; exit }')
if [ -n "$LIST_WIN_ID" ]; then
  log "列表窗口 WinId=$LIST_WIN_ID"
else
  log "[WARN] 未从 WMS 取得列表窗口 WinId，首帧判定只依赖远程窗口 dump 与渲染日志"
fi
step_pass launch "pid=$PID displayId=$DISPLAY_ID"

# ---------------------------------------------------------------------------
# 4. find-card：dumpLayout 找卡片，点「连接/打开/重连」按钮中心
# ---------------------------------------------------------------------------
find_dump=""
find_info=""
attempt=0
while [ "$attempt" -lt 6 ]; do
  attempt=$((attempt + 1))
  if find_dump=$(dump_layout "find-card-${attempt}"); then
    if find_info=$(layout_py find-card "$find_dump" "$NAME"); then
      break
    fi
    find_info=""
  fi
  sleep 1
done

if [ -z "$find_info" ]; then
  if [ -z "$find_dump" ]; then
    step_fail find-card "dumpLayout 连续失败，无法读取界面"
  fi
  log "可见卡片:"
  visible_count=0
  while IFS= read -r visible_name; do
    log "  $visible_name"
    visible_count=$((visible_count + 1))
  done < <(get_all_kv "$(layout_py find-card "$find_dump" "$NAME" 2>/dev/null || true)" VISIBLE)
  if [ "$visible_count" -eq 0 ]; then log "  (无)"; fi
  show_texts "$find_dump"
  if [ -n "$NAME" ]; then
    step_fail find-card "未找到连接卡片「$NAME」"
  fi
  step_fail find-card "未找到可连接的卡片"
fi

CHOSEN=$(get_kv "$find_info" CHOSEN)
if [ -z "$CHOSEN" ]; then
  step_fail find-card "卡片没有可读名称"
fi
CARD_STATUS=$(get_kv "$find_info" STATUS)
CARD_BUTTON=$(get_kv "$find_info" BUTTON)
BUTTON_X=$(get_kv "$find_info" BX)
BUTTON_Y=$(get_kv "$find_info" BY)
MENU_X=$(get_kv "$find_info" MX)
MENU_Y=$(get_kv "$find_info" MY)
if [ -z "$NAME" ]; then
  NAME="$CHOSEN"
  log "[INFO] 未指定 --name，自动选择卡片「$NAME」"
fi
if [ -z "$BUTTON_X" ] || [ -z "$BUTTON_Y" ]; then
  step_fail find-card "卡片「$NAME」没有可点击的「$CARD_BUTTON」按钮坐标"
fi
step_pass find-card "「$NAME」status=$CARD_STATUS button=$CARD_BUTTON click=$BUTTON_X,$BUTTON_Y"

# ---------------------------------------------------------------------------
# 5. wait-frame：每 2 秒 dump；失败/重连中即 FAIL；首帧消失/渲染帧>0 即 PASS
# ---------------------------------------------------------------------------
PERF_BEFORE=$(latest_perf_line "$PID" || true)
BASE_SESSION=$(perf_session "$PERF_BEFORE")
hdc_sh "uitest uiInput click $BUTTON_X $BUTTON_Y $DISPLAY_ID" >/dev/null 2>&1 || true
log "已点击「$CARD_BUTTON」，等待首帧（最多 ${TIMEOUT}s）"
WAIT_START=$SECONDS
WAIT_OK=0
WAIT_INDEX=0
WAIT_LAST_DUMP=""
WAIT_SAW_WAIT=0
WAIT_LAST_FRAMES=0
while [ $((SECONDS - WAIT_START)) -lt "$TIMEOUT" ]; do
  sleep 2
  WAIT_INDEX=$((WAIT_INDEX + 1))
  label=$(printf 'wait-%02d' "$WAIT_INDEX")
  if ! wait_dump=$(dump_layout "$label"); then
    log "[WARN] ${label}: dumpLayout 失败，继续等待"
    continue
  fi
  WAIT_LAST_DUMP="$wait_dump"
  if ! wait_state=$(layout_py state "$wait_dump" "$NAME"); then
    log "[WARN] ${label}: 布局解析失败，继续等待"
    continue
  fi
  has_wait=$(get_kv "$wait_state" HAS_WAIT)
  has_fail=$(get_kv "$wait_state" HAS_FAIL)
  has_name=$(get_kv "$wait_state" HAS_NAME)
  connected=$(get_kv "$wait_state" CONNECTED)
  wait_status=$(get_kv "$wait_state" CARD_STATUS)
  # 手机上远程窗口在前台，默认 dump 只有 Remote 页，看不到列表卡片；
  # 用列表窗口 WinId 定向 dump 一次，补上连接名与卡片状态。
  list_has_name=0
  list_connected=0
  list_has_fail=0
  list_status=""
  if [ "$has_name" -eq 0 ] && [ -n "$LIST_WIN_ID" ]; then
    if list_dump=$(dump_layout "${label}-list" "$LIST_WIN_ID"); then
      if list_state=$(layout_py state "$list_dump" "$NAME"); then
        list_has_name=$(get_kv "$list_state" HAS_NAME)
        list_connected=$(get_kv "$list_state" CONNECTED)
        list_has_fail=$(get_kv "$list_state" HAS_FAIL)
        list_status=$(get_kv "$list_state" CARD_STATUS)
      fi
    fi
  fi
  perf_line=$(latest_perf_line "$PID" || true)
  frames=$(perf_frames "$perf_line")
  [ -n "$frames" ] || frames=0
  perf_sid=$(perf_session "$perf_line")
  render_state=$(perf_state "$perf_line")
  if [ -n "$perf_sid" ] && [ "$perf_sid" = "$BASE_SESSION" ]; then
    frames=0
  fi
  WAIT_LAST_FRAMES="$frames"
  [ "$has_wait" -eq 1 ] && WAIT_SAW_WAIT=1
  if [ "$has_fail" -eq 1 ] || [ "$list_has_fail" -eq 1 ] ||
     [ "$render_state" = "failed" ] || [ "$render_state" = "reconnecting" ]; then
    show_texts "$wait_dump"
    step_fail wait-frame "界面或渲染日志出现「失败」/「重连中」（render state=${render_state:-unknown}）"
  fi
  if [ "$has_wait" -eq 0 ] && [ "$frames" -gt 0 ]; then
    WAIT_OK=1
    step_pass wait-frame "首帧已显示（约 $((SECONDS - WAIT_START))s，render frames=$frames，卡片状态=${list_status:-${wait_status:-未知}}）"
    break
  fi
  if [ "$has_wait" -eq 0 ] && [ "$WAIT_SAW_WAIT" -eq 1 ] &&
     { { [ "$has_name" -eq 1 ] && [ "$connected" -eq 1 ]; } ||
       { [ "$list_has_name" -eq 1 ] && [ "$list_connected" -eq 1 ]; } }; then
    WAIT_OK=1
    step_pass wait-frame "首帧已显示（约 $((SECONDS - WAIT_START))s，卡片状态=${list_status:-${wait_status:-未知}}）"
    break
  fi
done
if [ "$WAIT_OK" -ne 1 ]; then
  [ -n "$WAIT_LAST_DUMP" ] && show_texts "$WAIT_LAST_DUMP"
  step_fail wait-frame "超时 ${TIMEOUT}s 仍未看到首帧（曾出现等待首帧=${WAIT_SAW_WAIT}，最后渲染帧=${WAIT_LAST_FRAMES}）"
fi

# ---------------------------------------------------------------------------
# 6. screenshot：snapshot_display 存到证据目录
# ---------------------------------------------------------------------------
shot_remote="${REMOTE_DIR}/screenshot.jpeg"
shot_local="${EVIDENCE_DIR}/screenshot.jpeg"
hdc_sh "snapshot_display -i $DISPLAY_ID -f '$shot_remote'" >/dev/null 2>&1 || true
case "$shot_local" in
  "$EVIDENCE_DIR"/*) rm -f -- "$shot_local" ;;
esac
hdc_exec file recv "$shot_remote" "$shot_local" >/dev/null 2>&1 || true
if [ ! -s "$shot_local" ]; then
  step_fail screenshot "截图未取回: $shot_local"
fi
step_pass screenshot "$shot_local"

# ---------------------------------------------------------------------------
# 7. disconnect：先返回键；3 秒未回到未连接则「⋯」→「断开」
# ---------------------------------------------------------------------------
hdc_sh "uitest uiInput keyEvent 2 $DISPLAY_ID" >/dev/null 2>&1 || true
BACK_OK=0
DISC_DUMP=""
for i in 1 2 3 4 5 6; do
  sleep 0.5
  if ! back_dump=$(dump_layout "disconnect-back-${i}"); then
    continue
  fi
  if ! back_state=$(layout_py state "$back_dump" "$NAME"); then
    continue
  fi
  back_status=$(get_kv "$back_state" CARD_STATUS)
  back_button=$(get_kv "$back_state" CARD_BUTTON)
  if [ "$back_status" = "未连接" ] && [ "$back_button" = "连接" ]; then
    BACK_OK=1
    DISC_DUMP="$back_dump"
    break
  fi
done

if [ "$BACK_OK" -ne 1 ]; then
  log "[INFO] 返回键 3 秒内未回到未连接，改用卡片「⋯」→「断开」"
  # 把连接列表抬到前台；远程窗口还开着时，直接点列表坐标会点到远程画面。
  hdc_sh "aa start -a $ABILITY -b $BUNDLE" >/dev/null 2>&1 || true
  sleep 1
  if ! list_dump=$(dump_layout "disconnect-list"); then
    step_fail disconnect "无法读取连接列表（dumpLayout 失败）"
  fi
  if ! list_info=$(layout_py find-card "$list_dump" "$NAME"); then
    show_texts "$list_dump"
    step_fail disconnect "回到列表后找不到卡片「$NAME」"
  fi
  MENU_X=$(get_kv "$list_info" MX)
  MENU_Y=$(get_kv "$list_info" MY)
  if [ -z "$MENU_X" ] || [ -z "$MENU_Y" ]; then
    step_fail disconnect "卡片「$NAME」缺少「⋯」按钮坐标"
  fi
  hdc_sh "uitest uiInput click $MENU_X $MENU_Y $DISPLAY_ID" >/dev/null 2>&1 || true
  MENU_OK=0
  menu_dump=""
  for i in 1 2 3 4; do
    sleep 0.5
    if ! menu_dump=$(dump_layout "disconnect-menu-${i}"); then
      continue
    fi
    if menu_pos=$(layout_py find-text "$menu_dump" "断开"); then
      DISC_X=$(get_kv "$menu_pos" X)
      DISC_Y=$(get_kv "$menu_pos" Y)
      MENU_OK=1
      break
    fi
  done
  if [ "$MENU_OK" -ne 1 ]; then
    [ -n "$menu_dump" ] && show_texts "$menu_dump"
    step_fail disconnect "菜单中未找到「断开」"
  fi
  hdc_sh "uitest uiInput click $DISC_X $DISC_Y $DISPLAY_ID" >/dev/null 2>&1 || true
  DISC_OK=0
  for i in $(seq 1 10); do
    sleep 0.5
    if ! post_dump=$(dump_layout "disconnect-after-${i}"); then
      continue
    fi
    if ! post_state=$(layout_py state "$post_dump" "$NAME"); then
      continue
    fi
    post_status=$(get_kv "$post_state" CARD_STATUS)
    post_button=$(get_kv "$post_state" CARD_BUTTON)
    if [ "$post_status" = "未连接" ] && [ "$post_button" = "连接" ]; then
      DISC_OK=1
      DISC_DUMP="$post_dump"
      break
    fi
  done
  if [ "$DISC_OK" -ne 1 ]; then
    [ -n "${post_dump:-}" ] && show_texts "${post_dump:-}"
    step_fail disconnect "菜单断开后卡片仍未显示「未连接/连接」"
  fi
fi

step_pass disconnect "卡片「$NAME」已回到未连接/连接"
finish
