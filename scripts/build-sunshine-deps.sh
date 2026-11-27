#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static PIC dependencies for the Sunshine/Moonlight stream core on OHOS aarch64:
# moonlight-common-c (with its enet and nanors submodules) and libopus, installed
# into .work/sunshine-deps/ohos/prefix as AArch64 ELF static archives.
# Usage: scripts/build-sunshine-deps.sh ohos [jobs: 1..4]
#        scripts/build-sunshine-deps.sh host   (reserved; not implemented)
# Installs nothing system-wide, never builds inside the pinned moonlight-common-c
# checkout, and only reads the OpenSSL static prefix built by build-rdp-deps.sh.
# The host mode is a placeholder for a future x86_64 prefix; it errors out
# instead of pretending to have built something.
set -euo pipefail
export LC_ALL=C
mode=${1:-}
jobs=${2:-4}
case "$mode" in
  ohos|host) ;;
  *) echo "Usage: $0 ohos [jobs: 1..4] | $0 host" >&2; exit 2 ;;
esac
[[ $# -le 2 && $jobs =~ ^[1-4]$ ]] || { echo 'jobs must be 1..4' >&2; exit 2; }

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
root="$repo/.work/sunshine-deps"
moon_src="$repo/.work/moonlight-common-c"
# Static OpenSSL 3.5.9 for OHOS, built by scripts/build-rdp-deps.sh. It is only
# read here; the prefix fingerprint below fails the build if anything writes to it.
rdp_prefix="$repo/.work/rdp-deps/ohos/prefix"

if [[ $mode == host ]]; then
  cat >&2 <<'EOF'
host mode is not implemented by this script.
The x86_64 host prefix used by experiments/sunshine-core was assembled by hand
(see experiments/sunshine-core/README.md); this script only builds the OHOS
(aarch64-linux-ohos) prefix. Refusing to claim a host build.
EOF
  exit 1
fi

for tool in git cmake make; do
  command -v "$tool" >/dev/null || { echo "Missing existing tool: $tool" >&2; exit 1; }
done
test -x "$repo/scripts/fetch-moonlight.sh" || {
  echo "Missing $repo/scripts/fetch-moonlight.sh" >&2
  exit 1
}

# Pinned OHOS SDK (same 26.0.0.821 SDK used by the repository's other scripts).
sdk=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony
native="$sdk/native"
sysroot="$native/sysroot"
test -f "$native/oh-uni-package.json" || { echo "Missing OHOS SDK: $native" >&2; exit 1; }
grep -Eq '"apiVersion": "26"' "$native/oh-uni-package.json"
grep -Eq '"version": "26.0.0.105"' "$native/oh-uni-package.json"
llvm_cc="$native/llvm/bin/clang"
llvm_ar="$native/llvm/bin/llvm-ar"
llvm_ranlib="$native/llvm/bin/llvm-ranlib"
llvm_nm="$native/llvm/bin/llvm-nm"
llvm_readelf="$native/llvm/bin/llvm-readelf"
llvm_strip="$native/llvm/bin/llvm-strip"
for tool in "$llvm_cc" "$llvm_ar" "$llvm_ranlib" "$llvm_nm" "$llvm_readelf" \
  "$llvm_strip" "$sysroot/usr/include/sys/socket.h" \
  "$sysroot/usr/lib/aarch64-linux-ohos/libc.so"; do
  test -e "$tool" || { echo "Missing OHOS SDK file: $tool" >&2; exit 1; }
done

# moonlight-common-c's own CMake only needs libcrypto; the OpenSSL prefix must
# already exist (scripts/build-rdp-deps.sh produces it). libssl is never linked
# into moonlight-common-c; both library paths are passed to find_package(OpenSSL)
# only so the package check can resolve, and the CMake target links crypto only.
for file in "$rdp_prefix/include/openssl/opensslv.h" "$rdp_prefix/include/openssl/evp.h" \
  "$rdp_prefix/lib/libcrypto.a" "$rdp_prefix/lib/libssl.a"; do
  test -f "$file" || {
    echo "Missing OHOS OpenSSL file: $file (run scripts/build-rdp-deps.sh ohos first)" >&2
    exit 1
  }
done
grep -q 'define OPENSSL_VERSION_STR "3\.5\.9"' "$rdp_prefix/include/openssl/opensslv.h" || {
  echo "Expected OpenSSL 3.5.9 headers in $rdp_prefix" >&2
  exit 1
}

# Upstream pins: moonlight-common-c and its two submodules (the same commits
# scripts/fetch-moonlight.sh enforces), plus libopus.
moon_commit=b4ee1cea3d19e13c0b37c26585e6b1ee174effcb
enet_commit=98dbb897ac4a7208038e0782cddedf89dcf0723a
nanors_commit=a9b1636f9c98dbfdd0daeaf6982ccd24d1c30f29
moon_url=https://github.com/moonlight-stream/moonlight-common-c.git

# libopus: v1.6.1 was the newest stable (non-rc) tag when this script was
# written. Resolved with
#   git ls-remote --tags https://github.com/xiph/opus.git
# where refs/tags/v1.6.1 (annotated tag object a5d6c1b6f4e582df97390f9ac5c6e7c51cbffffe)
# dereferences to commit 22244de5a79bd1d6d623c32e72bf1954b56235be. The commit is
# pinned, not the tag, so a moved/retagged tag cannot change the source.
# The source is cached under .work/sunshine-deps/src/opus-<version> and only
# fetched when that cache is absent; an existing cache is verified, never reset.
opus_version=1.6.1
opus_tag=v1.6.1
opus_commit=22244de5a79bd1d6d623c32e72bf1954b56235be
opus_url=https://github.com/xiph/opus.git

cache="$root/cache"
srcdir="$root/src"
build="$root/build"
prefix="$root/ohos/prefix"
[[ $prefix == "$root/ohos/prefix" ]] || { echo 'Refusing unexpected prefix path' >&2; exit 1; }
# cache/ is kept for layout parity with build-rdp-deps.sh; the two sources here
# are git checkouts and live in src/, so no tarball is downloaded into cache/.
mkdir -p "$cache" "$srcdir"
# The build directory and prefix are this script's own outputs: reset them so a
# rerun is a real from-scratch build and stale archives cannot survive. The
# source cache (src/) is deliberately not reset. Both paths are checked against
# an explicit whitelist before anything is deleted.
reset_owned_dir() {
  local dir=$1
  case $dir in
    "$root/build"|"$root/ohos/prefix") ;;
    *) echo "Refusing to delete unexpected path: $dir" >&2; exit 1 ;;
  esac
  rm -rf -- "$dir"
}
reset_owned_dir "$build"
reset_owned_dir "$prefix"
mkdir -p "$build" "$prefix"
exec > >(tee "$build/run.log") 2>&1
trap 'rc=$?; if ((rc)); then echo "FAILED exit=$rc mode=$mode; log=$build/run.log" >&2; fi' EXIT
printf 'MODE=%s\nBUILD=%s\nPREFIX=%s\nCACHE=%s\n' "$mode" "$build" "$prefix" "$cache"

fail() { echo "$*" >&2; exit 1; }
tail_log() { tail -40 "$1" >&2; }

# Public-HTTP-only git: ignore system/global config, never prompt for credentials.
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null GIT_TERMINAL_PROMPT=0
export GIT_ASKPASS=/bin/false
git_public() { git -c credential.helper= -c core.askPass=/bin/false "$@"; }

# Snapshot the RDP dependency prefix (path, size, mtime of every file) so the end
# of the build can prove this script did not modify it.
rdp_fingerprint() {
  find "$rdp_prefix" -type f -printf '%p %s %T@\n' | LC_ALL=C sort | sha256sum | cut -d' ' -f1
}
rdp_before=$(rdp_fingerprint)
printf 'RDP_PREFIX_BEFORE=%s\n' "$rdp_before"

# ---------------------------------------------------------------------------
# 0. Sources. moonlight-common-c is fetched/verified by scripts/fetch-moonlight.sh
#    (idempotent: an existing checkout is only verified). libopus has no fetch
#    script; first use initialises a small git checkout at the pinned commit and
#    later runs only verify HEAD and a clean tree.
# ---------------------------------------------------------------------------
printf '\n[1/4] moonlight-common-c source (pinned %s)\n' "$moon_commit"
bash "$repo/scripts/fetch-moonlight.sh" || fail 'fetch-moonlight.sh failed'
[[ -d $moon_src/.git && ! -L $moon_src/.git ]] || fail "not a git checkout: $moon_src"
[[ $(git_public -C "$moon_src" rev-parse HEAD) == "$moon_commit" ]] || \
  fail "moonlight-common-c HEAD is not $moon_commit"
[[ $(git_public -C "$moon_src/enet" rev-parse HEAD) == "$enet_commit" ]] || \
  fail "enet submodule is not $enet_commit"
[[ $(git_public -C "$moon_src/nanors" rev-parse HEAD) == "$nanors_commit" ]] || \
  fail "nanors submodule is not $nanors_commit"

printf '\n[2/4] libopus %s source (pinned %s)\n' "$opus_tag" "$opus_commit"
opus_src="$srcdir/opus-$opus_version"
if [[ -e $opus_src || -L $opus_src ]]; then
  [[ -d $opus_src/.git && ! -L $opus_src/.git ]] || fail "refusing unknown directory: $opus_src"
  head=$(git_public -C "$opus_src" rev-parse HEAD)
  [[ $head == "$opus_commit" ]] || \
    fail "existing libopus checkout HEAD $head != pinned $opus_commit; remove or fix $opus_src by hand"
  [[ -z $(git_public -C "$opus_src" status --porcelain --untracked-files=all --ignored) ]] || \
    fail "existing libopus checkout has local changes or extra files: $opus_src"
  printf 'already pinned, nothing to fetch: %s\n' "$opus_src"
else
  mkdir -p "$srcdir"
  git_public -C "$srcdir" init --quiet "opus-$opus_version" || fail "cannot init $opus_src"
  git_public -C "$opus_src" remote add origin "$opus_url"
  # GitHub accepts fetching a reachable commit by SHA. Resolve and verify it
  # before checking out; never fall back to another commit.
  git_public -C "$opus_src" fetch --quiet --depth=1 --no-tags origin "$opus_commit" || \
    fail "cannot fetch libopus commit $opus_commit"
  resolved=$(git_public -C "$opus_src" rev-parse --verify --quiet "$opus_commit^{commit}" || true)
  [[ $resolved == "$opus_commit" ]] || \
    fail "cannot resolve pinned libopus commit $opus_commit (got '$resolved'); not switching to another commit"
  git_public -C "$opus_src" -c advice.detachedHead=false checkout --quiet --detach "$opus_commit"
  printf 'fetched: %s\n' "$opus_src"
fi
printf 'opus HEAD: %s\n' "$(git_public -C "$opus_src" rev-parse HEAD)"

# Compiler wrapper: target/sysroot flags live in the wrapper (as in
# build-rdp-deps.sh) so CMake's compiler tests and both projects use the same
# OHOS clang invocation.
toolchain="$build/toolchain"
mkdir -p "$toolchain"
cat > "$toolchain/ohos-cc" <<EOF
#!/bin/sh
exec '$llvm_cc' --target=aarch64-linux-ohos --sysroot='$sysroot' "\$@"
EOF
chmod +x "$toolchain/ohos-cc"
"$toolchain/ohos-cc" -dM -E -x c /dev/null > "$build/predefined-macros.txt"
grep -q '__aarch64__' "$build/predefined-macros.txt"
grep -q '__OHOS__' "$build/predefined-macros.txt"

common_cmake_flags=(
  -G "Unix Makefiles"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_SYSROOT="$sysroot"
  -DCMAKE_C_COMPILER="$toolchain/ohos-cc"
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-ohos
  -DCMAKE_AR="$llvm_ar" -DCMAKE_RANLIB="$llvm_ranlib"
  -DCMAKE_NM="$llvm_nm" -DCMAKE_STRIP="$llvm_strip"
  -DCMAKE_FIND_ROOT_PATH="$rdp_prefix;$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
)

# ---------------------------------------------------------------------------
# 3. moonlight-common-c (static PIC) + enet, built out of tree. Upstream has no
#    install rule for the common-c library (only enet installs), so the library
#    and headers are copied into the prefix below, exactly like the host spike.
#    OpenSSL comes from the RDP dependency prefix; only libcrypto is linked.
#    nanors is compiled straight into libmoonlight-common-c.a by upstream's
#    CMakeLists (nanors/rs.c + nanors/deps/obl), so there is no separate archive.
# ---------------------------------------------------------------------------
printf '\n[3/4] moonlight-common-c + enet (AArch64 static)\n'
moon_build="$build/moonlight-common-c-build"
cmake -S "$moon_src" -B "$moon_build" "${common_cmake_flags[@]}" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DBUILD_SHARED_LIBS=OFF -DENET_NO_INSTALL=ON \
  -DCMAKE_PREFIX_PATH="$rdp_prefix" \
  -DOPENSSL_ROOT_DIR="$rdp_prefix" \
  -DOPENSSL_INCLUDE_DIR="$rdp_prefix/include" \
  -DOPENSSL_CRYPTO_LIBRARY="$rdp_prefix/lib/libcrypto.a" \
  -DOPENSSL_SSL_LIBRARY="$rdp_prefix/lib/libssl.a" \
  -DOPENSSL_USE_STATIC_LIBS=ON > "$build/moonlight-configure.log" 2>&1 || {
    fail "moonlight-common-c configure failed; see $build/moonlight-configure.log"
  }
cmake --build "$moon_build" --parallel "$jobs" > "$build/moonlight-build.log" 2>&1 || {
  echo 'moonlight-common-c build failed' >&2
  tail_log "$build/moonlight-build.log"
  exit 1
}
for archive in "$moon_build/libmoonlight-common-c.a" "$moon_build/enet/libenet.a"; do
  [[ -s $archive ]] || fail "missing build output: $archive"
done
mkdir -p "$prefix/lib" "$prefix/include/enet"
install -m 644 "$moon_build/libmoonlight-common-c.a" "$prefix/lib/libmoonlight-common-c.a"
install -m 644 "$moon_build/enet/libenet.a" "$prefix/lib/libenet.a"
# Public common-c headers (Limelight.h, Video.h, Input.h, ...) live flat in
# include/; enet headers keep their own enet/ directory.
cp -a "$moon_src/src/"*.h "$prefix/include/"
cp -a "$moon_src/enet/include/enet/"*.h "$prefix/include/enet/"

# ---------------------------------------------------------------------------
# 4. libopus 1.6.1 (static PIC) via its CMake build, out of tree.
#    Chosen over autotools because the pinned source is a git checkout without a
#    generated configure script: CMake is fully supported upstream and needs no
#    ./autogen.sh (autoconf/automake/libtool are then not build dependencies),
#    it generates its config from cmake/config.h.cmake.in, and it builds only the
#    library with OPUS_BUILD_PROGRAMS/TESTING off. Upstream's install rule puts
#    the public headers in include/opus/, but the library and headers are copied
#    explicitly so the prefix layout stays identical to the RDP prefix (no
#    pkg-config/CMake export files with host-inappropriate paths).
# ---------------------------------------------------------------------------
printf '\n[4/4] libopus %s (AArch64 static)\n' "$opus_version"
opus_build="$build/opus-build"
cmake -S "$opus_src" -B "$opus_build" "${common_cmake_flags[@]}" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF \
  -DBUILD_TESTING=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
  > "$build/opus-configure.log" 2>&1 || {
    fail "libopus configure failed; see $build/opus-configure.log"
  }
cmake --build "$opus_build" --parallel "$jobs" > "$build/opus-build.log" 2>&1 || {
  echo 'libopus build failed' >&2
  tail_log "$build/opus-build.log"
  exit 1
}
[[ -s $opus_build/libopus.a ]] || fail "missing build output: $opus_build/libopus.a"
install -m 644 "$opus_build/libopus.a" "$prefix/lib/libopus.a"
mkdir -p "$prefix/include/opus"
cp -a "$opus_src/include/"*.h "$prefix/include/opus/"

# ---------------------------------------------------------------------------
# 5. Validation: every archive member is AArch64 ELF64, the required symbols are
#    defined, the common-c archive uses libcrypto but not libssl, the prefix has
#    no shared objects, and the RDP dependency prefix was not modified.
# ---------------------------------------------------------------------------
evidence="$build/evidence"
mkdir -p "$evidence"
printf '\nValidation\n'
libs=(libmoonlight-common-c.a libenet.a libopus.a)
for lib in "${libs[@]}"; do
  archive="$prefix/lib/$lib"
  [[ -s $archive ]] || fail "Missing static library: $archive"
  "$llvm_ar" t "$archive" > "$evidence/$lib.members.txt" || fail "llvm-ar t failed on $archive"
  members=$(wc -l < "$evidence/$lib.members.txt")
  ((members > 0)) || fail "$lib has no archive members"
  # Every member, not just the first, must be AArch64 ELF64.
  "$llvm_readelf" -h "$archive" > "$evidence/$lib.elf.txt" 2>/dev/null || \
    fail "llvm-readelf failed on $archive (not an ELF archive?)"
  bad_machine=$(grep 'Machine:' "$evidence/$lib.elf.txt" | grep -vc 'AArch64' || true)
  bad_class=$(grep 'Class:' "$evidence/$lib.elf.txt" | grep -vc 'ELF64' || true)
  machines=$(grep -c 'Machine:' "$evidence/$lib.elf.txt" || true)
  ((machines == members && bad_machine == 0 && bad_class == 0)) || \
    fail "$lib has non-AArch64/ELF64 members (members=$members headers=$machines bad_machine=$bad_machine bad_class=$bad_class)"
  # Extract the first member and print its ELF header excerpt as evidence.
  first=$(head -n 1 "$evidence/$lib.members.txt")
  "$llvm_ar" p "$archive" "$first" > "$evidence/$lib.first.o" || \
    fail "cannot extract $first from $archive"
  "$llvm_readelf" -h "$evidence/$lib.first.o" > "$evidence/$lib.first.elf.txt"
  class=$(sed -n 's/^ *Class: *//p' "$evidence/$lib.first.elf.txt" | head -n 1)
  machine=$(sed -n 's/^ *Machine: *//p' "$evidence/$lib.first.elf.txt" | head -n 1)
  printf '  %-24s %4d members  first=%-28s %s, %s\n' "$lib" "$members" "$first" "$class" "$machine"
  grep -E '^( *Class:| *Machine:)' "$evidence/$lib.first.elf.txt" | sed 's/^/      /'
done

# Required symbols. llvm-nm lists all archive members.
printf '\nSymbols\n'
"$llvm_nm" --defined-only "$prefix/lib/libmoonlight-common-c.a" > "$evidence/libmoonlight-common-c.symbols.txt"
"$llvm_nm" --defined-only "$prefix/lib/libopus.a" > "$evidence/libopus.symbols.txt"
for pair in 'libmoonlight-common-c.a:LiStartConnection' 'libopus.a:opus_decoder_create' \
  'libopus.a:opus_multistream_decode'; do
  lib=${pair%%:*}
  symbol=${pair#*:}
  hit=$(grep -E "[[:space:]]T $symbol$" "$evidence/${lib%.a}.symbols.txt" | head -n 1 || true)
  [[ -n $hit ]] || fail "$lib does not define $symbol"
  printf '  %-24s %s\n' "$lib" "$hit"
done
# libssl must not be linked into common-c: it is not passed on the link line and
# its archive must not show undefined SSL_* references. libcrypto is required.
"$llvm_nm" --undefined-only "$prefix/lib/libmoonlight-common-c.a" > "$evidence/libmoonlight-common-c.undefined.txt"
if grep -Eq '[[:space:]]U SSL_' "$evidence/libmoonlight-common-c.undefined.txt"; then
  fail 'libmoonlight-common-c.a references libssl symbols (SSL_*)'
fi
grep -Eq '[[:space:]]U EVP_EncryptInit_ex$' "$evidence/libmoonlight-common-c.undefined.txt" || \
  fail 'libmoonlight-common-c.a does not reference libcrypto (EVP_EncryptInit_ex)'
printf '  %-24s crypto-only: EVP_EncryptInit_ex undefined, no SSL_* references\n' 'libmoonlight-common-c.a'

# Header layout required by consumers.
for header in Limelight.h Video.h Input.h enet/enet.h opus/opus.h \
  opus/opus_defines.h opus/opus_types.h opus/opus_multistream.h; do
  [[ -s $prefix/include/$header ]] || fail "missing installed header: $prefix/include/$header"
done
printf '  headers: %s/include/{Limelight,Video,Input}.h enet/ opus/\n' "$prefix"

if find "$prefix" -type f \( -name '*.so' -o -name '*.so.*' -o -name '*.dylib' \) | grep -q .; then
  fail 'Unexpected shared object installed in the static prefix'
fi

# Static closure probe: one symbol from each archive plus libcrypto must link
# into a shared object with -Wl,-z,defs (no undefined symbols). This also proves
# the archives are PIC-linkable and that libssl is not needed by common-c.
linktest="$build/linktest"
mkdir -p "$linktest"
cat > "$linktest/probe.c" <<'EOF'
/* Static closure probe: reference one symbol from each archive. */
extern void LiStartConnection(void);
extern void *opus_decoder_create(void);
extern void *opus_multistream_decode(void);
void *tg_sunshine_deps_probe(void)
{
    LiStartConnection();
    return opus_decoder_create() ? opus_multistream_decode() : 0;
}
EOF
"$toolchain/ohos-cc" -O2 -fPIC -shared -Wl,-z,defs \
  -o "$linktest/libsunshine-deps-linktest.so" "$linktest/probe.c" \
  -L"$prefix/lib" -lmoonlight-common-c -lenet -lopus \
  -L"$rdp_prefix/lib" -lcrypto -lpthread -lm -ldl \
  > "$linktest/link.log" 2>&1 || {
    fail "static closure link test failed; see $linktest/link.log"
  }
"$llvm_readelf" -h -d "$linktest/libsunshine-deps-linktest.so" > "$evidence/linktest.elf.txt"
grep -q 'Machine:.*AArch64' "$evidence/linktest.elf.txt" || fail 'link test is not AArch64'
if grep -q 'TEXTREL' "$evidence/linktest.elf.txt"; then
  fail 'link test has text relocations (non-PIC code)'
fi
mapfile -t needed < <(sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' "$evidence/linktest.elf.txt")
(( ${#needed[@]} > 0 )) || fail 'link test has no NEEDED entries'
for entry in "${needed[@]}"; do
  case $entry in
    libc.so|libm.so|libdl.so|libpthread.so) ;;
    *) fail "Unexpected NEEDED in link test: $entry" ;;
  esac
done
printf '\nLink test (-shared -Wl,-z,defs): PASS, NEEDED=%s\n' "${needed[*]}"

# Platform branch evidence for the notes below: OHOS clang predefines __OHOS__,
# so Platform.h takes the POSIX (LC_POSIX) branch; the OHOS libc headers define
# SO_PRIORITY, so the Linux QoS setsockopt path compiles; common-c's CMake adds
# -DHAS_SOCKLEN_T, so SOCKADDR_LEN is socklen_t.
cat > "$build/platform-probe.c" <<'EOF'
#include <sys/socket.h>
#include "Platform.h"
EOF
"$toolchain/ohos-cc" -E -dM -I"$moon_src/src" "$build/platform-probe.c" \
  > "$build/platform-macros.txt" 2> "$build/platform-probe.err" || {
    fail "platform macro probe failed; see $build/platform-probe.err"
  }
printf '\nPlatform macros\n'
grep -E '^#define (LC_POSIX|__OHOS__|SO_PRIORITY|SO_NET_SERVICE_TYPE)( |$)' \
  "$build/platform-macros.txt" | tee "$evidence/platform-macros.txt" | sed 's/^/  /'
grep -qE '^#define LC_POSIX( |$)' "$build/platform-macros.txt" || \
  fail 'Platform.h did not select the POSIX branch'
grep -q '^#define SO_PRIORITY ' "$build/platform-macros.txt" || \
  fail 'SO_PRIORITY is not defined by the OHOS libc headers'
grep -q 'DHAS_SOCKLEN_T' "$moon_build/CMakeFiles/moonlight-common-c.dir/flags.make" || \
  fail 'common-c was not compiled with -DHAS_SOCKLEN_T'
printf '  common-c flags: %s\n' \
  "$(grep -o -- '-DHAS_SOCKLEN_T' "$moon_build/CMakeFiles/moonlight-common-c.dir/flags.make" | head -n 1)"
printf '  Platform.h branch: LC_POSIX (non-Windows); SOCKADDR_LEN=socklen_t\n'

# CMake caches must resolve OpenSSL to the RDP static prefix, never to a host copy.
grep -E '^(OPENSSL_(CRYPTO|SSL)_LIBRARY|OPENSSL_INCLUDE_DIR|OPENSSL_ROOT_DIR|BUILD_SHARED_LIBS):' \
  "$moon_build/CMakeCache.txt" > "$evidence/moonlight-openssl-cache.txt"
grep -qx "OPENSSL_CRYPTO_LIBRARY:FILEPATH=$rdp_prefix/lib/libcrypto.a" "$moon_build/CMakeCache.txt" || \
  fail 'common-c did not resolve libcrypto to the RDP static prefix'
grep -qE '^BUILD_SHARED_LIBS:[A-Z]+=OFF$' "$moon_build/CMakeCache.txt" || \
  fail 'common-c BUILD_SHARED_LIBS is not OFF'

rdp_after=$(rdp_fingerprint)
printf '\nRDP_PREFIX_BEFORE=%s\nRDP_PREFIX_AFTER=%s\n' "$rdp_before" "$rdp_after"
[[ $rdp_before == "$rdp_after" ]] || fail "the RDP dependency prefix changed during this build: $rdp_prefix"

# ---------------------------------------------------------------------------
# 6. Manifest and report.
# ---------------------------------------------------------------------------
manifest="$prefix/share/sunshine-deps-manifest.txt"
mkdir -p "$prefix/share"
{
  printf 'Purpose: static PIC dependencies for the Sunshine/Moonlight stream core on OHOS aarch64\n'
  printf 'Mode: %s\nPrefix: %s\nBuilt: %s\nBuild log: %s/run.log\n' \
    "$mode" "$prefix" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$build"
  printf 'SDK: %s\nTarget: aarch64-linux-ohos\nSysroot: %s\n' "$sdk" "$sysroot"
  printf '\nmoonlight-common-c: %s\n  Source: %s\n  enet submodule: %s\n  nanors submodule: %s\n' \
    "$moon_commit" "$moon_url" "$enet_commit" "$nanors_commit"
  printf '  CMake: BUILD_SHARED_LIBS=OFF, CMAKE_POSITION_INDEPENDENT_CODE=ON,\n'
  printf '         OPENSSL_ROOT_DIR=%s (libcrypto only; libssl not linked),\n' "$rdp_prefix"
  printf '         ENET_NO_INSTALL=ON; library and src/*.h + enet headers copied by this script.\n'
  printf '  nanors/rs.c and nanors/deps/obl/*.c are part of libmoonlight-common-c.a.\n'
  printf '\nlibopus: %s (tag %s)\n  Commit: %s\n  Source: %s\n' \
    "$opus_version" "$opus_tag" "$opus_commit" "$opus_url"
  printf '  CMake: BUILD_SHARED_LIBS=OFF, OPUS_BUILD_SHARED_LIBRARY=OFF,\n'
  printf '         OPUS_BUILD_TESTING=OFF, OPUS_BUILD_PROGRAMS=OFF, BUILD_TESTING=OFF;\n'
  printf '         built from the git checkout without ./autogen.sh (CMake is upstream-supported\n'
  printf '         and generates its config from cmake/config.h.cmake.in).\n'
  printf '\nLink order for consumers (static):\n'
  printf '  -lmoonlight-common-c -lenet -lopus -lcrypto -lpthread -lm -ldl\n'
  printf '  Include dirs: %s/include %s/include/enet %s/include/opus\n' "$prefix" "$prefix" "$prefix"
  printf '  libssl is deliberately NOT part of this set; common-c only needs libcrypto.\n'
  printf '\nEvidence: %s/*.elf.txt, %s/*.first.elf.txt, %s/*.symbols.txt,\n' \
    "$evidence" "$evidence" "$evidence"
  printf '          %s/moonlight-openssl-cache.txt, %s/platform-macros.txt\n' "$evidence" "$evidence"
} > "$manifest"

printf '\nArchive sizes and layout:\n'
ls -la "$prefix/lib" | tee "$evidence/layout.txt"
printf '\nSUCCESS mode=%s\nPREFIX=%s\nMANIFEST=%s\nBUILD_LOG=%s/run.log\n' \
  "$mode" "$prefix" "$manifest" "$build"
