#!/usr/bin/env bash
# 拉取 Moonlight 流媒体内核 moonlight-common-c 到 .work/（上游不 vendor，按提交固定）。
# 上游没有 tag，只能按提交固定；该提交带有 git 子模块，必须用提交里锁定的子模块版本。
# 只走公开 HTTPS：不读用户凭据、不改动 git 配置、不交互；不在源码树里构建。
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SOURCE_DIR="$ROOT/.work/moonlight-common-c"
UPSTREAM=https://github.com/moonlight-stream/moonlight-common-c.git
# 固定提交：上游 master 在 2026-11-22 的提交，前缀 b4ee1ce。
# 完整 SHA 由 `git ls-remote $UPSTREAM HEAD` 解析得到（本次解析时 HEAD 即该提交），
# 解析不到就停止，绝不替换成别的提交。
COMMIT=b4ee1cea3d19e13c0b37c26585e6b1ee174effcb
COMMIT_PREFIX=b4ee1ce

fail() { printf 'fetch-moonlight: %s\n' "$*" >&2; exit 1; }

# 只读公网：禁用系统/全局 git 配置，禁止交互式凭据提示。
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
export GIT_ASKPASS=/bin/false

git_public() { git -c credential.helper= -c core.askPass=/bin/false "$@"; }

# .gitmodules 里声明的子模块数量（用于确认 submodule status 没有漏项）。
declared_submodule_count() {
    git_public -C "$SOURCE_DIR" config --file .gitmodules --get-regexp '^submodule\..*\.path$' | wc -l
}

# 打印主仓库与每个子模块的路径和完整 SHA。
print_pins() {
    local line sha path
    printf 'MOONLIGHT_SOURCE_DIR=%s\n' "$SOURCE_DIR"
    printf 'main %s %s\n' "$SOURCE_DIR" "$(git_public -C "$SOURCE_DIR" rev-parse HEAD)"
    while IFS= read -r line; do
        [[ -n $line ]] || continue
        # 行格式：<状态字符><40 位 SHA><空格><路径>[ (<describe>)]
        sha=${line:1:40}
        path=${line:42}
        path=${path%% (*}
        printf 'submodule %s/%s %s\n' "$SOURCE_DIR" "$path" "$sha"
    done < <(git_public -C "$SOURCE_DIR" submodule status --recursive)
}

# 校验已有 checkout：HEAD、工作区、每个子模块都必须与固定提交一致。
# 不一致直接报错退出，不自动删除已有目录（由人工判断后处理）。
verify_existing() {
    [[ -d "$SOURCE_DIR/.git" && ! -L "$SOURCE_DIR/.git" ]] \
        || fail "refusing unknown directory: $SOURCE_DIR"
    [[ "$(git_public -C "$SOURCE_DIR" rev-parse --show-toplevel)" == "$SOURCE_DIR" ]] \
        || fail "refusing unexpected repository: $SOURCE_DIR"
    local head
    head=$(git_public -C "$SOURCE_DIR" rev-parse HEAD)
    [[ $head == "$COMMIT" ]] \
        || fail "existing checkout HEAD $head != pinned $COMMIT; remove or fix $SOURCE_DIR by hand"
    [[ -z "$(git_public -C "$SOURCE_DIR" status --porcelain --untracked-files=all --ignored)" ]] \
        || fail "existing checkout has local changes or extra files: $SOURCE_DIR"
    local line count=0
    while IFS= read -r line; do
        [[ -n $line ]] || continue
        count=$((count + 1))
        # 首字符为空格表示子模块已初始化且正好在该提交上；- + U 都算不匹配。
        [[ ${line:0:1} == ' ' ]] \
            || fail "submodule not at pinned commit: $line"
    done < <(git_public -C "$SOURCE_DIR" submodule status --recursive)
    [[ $count -eq $(declared_submodule_count) ]] \
        || fail "submodule count mismatch: $count checked out, $(declared_submodule_count) declared"
}

if [[ -e "$SOURCE_DIR" || -L "$SOURCE_DIR" ]]; then
    # 目录已存在：只校验，不删除、不重置。
    verify_existing
    printf 'already pinned, nothing to fetch: %s\n' "$SOURCE_DIR"
    print_pins
    exit 0
fi

# 新建目录前先解析固定提交，解析不到就不产生任何文件。
mkdir -p -- "$(dirname -- "$SOURCE_DIR")"
mkdir -- "$SOURCE_DIR"
trap 'rc=$?; if ((rc)); then printf "fetch-moonlight: failed at %s (partial files left in %s)\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$SOURCE_DIR" >&2; fi' EXIT
git_public -C "$SOURCE_DIR" init --quiet
git_public -C "$SOURCE_DIR" remote add origin "$UPSTREAM"
[[ $(git_public -C "$SOURCE_DIR" rev-parse --show-toplevel) == "$SOURCE_DIR" ]] \
    || fail "unexpected repository: $SOURCE_DIR"

# 先直接按完整 SHA 浅取（GitHub 允许取可达提交）；被拒则逐步加深 master 历史再解析。
if ! git_public -C "$SOURCE_DIR" fetch --quiet --depth=1 --no-tags origin "$COMMIT"; then
    depth=50
    while ((depth <= 6400)); do
        git_public -C "$SOURCE_DIR" fetch --quiet --depth="$depth" --no-tags origin master || true
        if git_public -C "$SOURCE_DIR" rev-parse --verify --quiet "$COMMIT^{commit}" >/dev/null; then
            break
        fi
        depth=$((depth * 2))
    done
fi
resolved=$(git_public -C "$SOURCE_DIR" rev-parse --verify --quiet "$COMMIT^{commit}" || true)
[[ $resolved == "$COMMIT" ]] \
    || fail "cannot resolve pinned commit $COMMIT_PREFIX... ($COMMIT); not switching to another commit"
git_public -C "$SOURCE_DIR" -c advice.detachedHead=false checkout --quiet --detach "$COMMIT"

# 子模块必须用该提交锁定的版本（递归，含嵌套子模块）。
git_public -C "$SOURCE_DIR" submodule update --init --recursive

verify_existing
printf 'fetched: %s\n' "$SOURCE_DIR"
print_pins
