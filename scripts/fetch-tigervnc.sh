#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SOURCE_DIR="$ROOT/.work/sources/tigervnc-v1.15.0"
UPSTREAM=https://github.com/TigerVNC/tigervnc.git
TAG=v1.15.0
COMMIT=8bdf8734ce12c8ed3c232496fc8a4ccdb04636b8

fail() { printf 'fetch-tigervnc: %s\n' "$*" >&2; exit 1; }

# Use public HTTPS without reading user credentials or changing remote settings.
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
export GIT_ASKPASS=/bin/false

git_public() { git -c credential.helper= -c core.askPass=/bin/false "$@"; }

if [[ -e "$SOURCE_DIR" || -L "$SOURCE_DIR" ]]; then
    [[ -d "$SOURCE_DIR" && ! -L "$SOURCE_DIR" && -d "$SOURCE_DIR/.git" && ! -L "$SOURCE_DIR/.git" ]] \
        || fail "Refusing unknown directory: $SOURCE_DIR"
    [[ "$(git_public -C "$SOURCE_DIR" rev-parse --show-toplevel)" == "$SOURCE_DIR" ]] \
        || fail "Refusing unexpected repository: $SOURCE_DIR"
    [[ "$(git_public -C "$SOURCE_DIR" rev-parse HEAD)" == "$COMMIT" ]] \
        || fail "Refusing repository with incorrect commit: $SOURCE_DIR"
    [[ -z "$(git_public -C "$SOURCE_DIR" status --porcelain --untracked-files=all --ignored)" ]] \
        || fail "Refusing repository with local changes or extra files: $SOURCE_DIR"
else
    mkdir -p -- "$(dirname -- "$SOURCE_DIR")"
    # Claim a new destination; never reuse a directory created concurrently.
    mkdir -- "$SOURCE_DIR"
    git_public -C "$SOURCE_DIR" init --quiet
    git_public -C "$SOURCE_DIR" fetch --depth=1 "$UPSTREAM" "refs/tags/$TAG:refs/tags/$TAG"
    [[ "$(git_public -C "$SOURCE_DIR" rev-parse "refs/tags/$TAG^{commit}")" == "$COMMIT" ]] \
        || fail "Upstream tag $TAG does not match pinned commit $COMMIT; leaving fetched files untouched"
    git_public -C "$SOURCE_DIR" -c advice.detachedHead=false checkout --quiet --detach "$COMMIT"
fi

[[ "$(git_public -C "$SOURCE_DIR" rev-parse HEAD)" == "$COMMIT" ]] \
    || fail "Source HEAD does not match pinned commit"
[[ -z "$(git_public -C "$SOURCE_DIR" status --porcelain --untracked-files=all --ignored)" ]] \
    || fail "Source worktree is not clean"
printf 'TIGERVNC_SOURCE_DIR=%s\n' "$SOURCE_DIR"
