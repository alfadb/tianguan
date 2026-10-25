#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Static PIC dependencies for the FreeRDP 3.9.0 RDP client on OHOS aarch64.
# Usage: scripts/build-rdp-deps.sh ohos [jobs: 1..4]
#        scripts/build-rdp-deps.sh host
# Builds zlib 1.3.2, OpenSSL 3.5.9 (libssl/libcrypto) and FreeRDP 3.9.0
# (libfreerdp3/libfreerdp-client3/libwinpr3) as AArch64 ELF static archives and
# installs them into .work/rdp-deps/ohos/prefix. Installs nothing system-wide and
# does not modify app/ or the pinned FreeRDP checkout.
# The host mode only reports the existing host spike; it does not rebuild it.
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
root="$repo/.work/rdp-deps"
spike="$repo/.work/freerdp-spike"
freerdp_src="$spike/src"
freerdp_commit=4ae5b6c25452211f01f370d3e6e481553e72778a

if [[ $mode == host ]]; then
  # The x86_64 spike already exists and is documented in .work/freerdp-spike.
  for lib in libfreerdp3.a libfreerdp-client3.a libwinpr3.a; do
    [[ -s $spike/prefix/lib/$lib ]] || {
      echo "host mode: missing $spike/prefix/lib/$lib; run the host spike first" >&2
      exit 1
    }
  done
  cat <<EOF
host mode: no build performed.
Existing host x86_64 spike prefix: $spike/prefix
It links the system shared OpenSSL/zlib; see $spike/README.md for the build.
The OHOS target prefix is: $root/ohos/prefix
EOF
  exit 0
fi

for tool in curl sha256sum tar xz perl make cmake git patch awk; do
  command -v "$tool" >/dev/null || { echo "Missing existing tool: $tool" >&2; exit 1; }
done

# Pinned OHOS SDK (same 26.0.0.821 SDK used by the repository's other scripts).
sdk=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony
native="$sdk/native"
sysroot="$native/sysroot"
test -f "$native/oh-uni-package.json" || { echo "Missing OHOS SDK: $native" >&2; exit 1; }
grep -Eq '"apiVersion": "26"' "$native/oh-uni-package.json"
grep -Eq '"version": "26.0.0.105"' "$native/oh-uni-package.json"
llvm_cc="$native/llvm/bin/clang"
llvm_cxx="$native/llvm/bin/clang++"
llvm_ar="$native/llvm/bin/llvm-ar"
llvm_ranlib="$native/llvm/bin/llvm-ranlib"
llvm_nm="$native/llvm/bin/llvm-nm"
llvm_readelf="$native/llvm/bin/llvm-readelf"
llvm_strip="$native/llvm/bin/llvm-strip"
for tool in "$llvm_cc" "$llvm_cxx" "$llvm_ar" "$llvm_ranlib" "$llvm_nm" "$llvm_readelf" \
  "$llvm_strip" "$sysroot/usr/include/zlib.h" \
  "$sysroot/usr/lib/aarch64-linux-ohos/libc.so"; do
  test -e "$tool" || { echo "Missing OHOS SDK file: $tool" >&2; exit 1; }
done

# Validate the pinned FreeRDP checkout before building anything.
test -d "$freerdp_src/.git" || {
  echo "Missing FreeRDP 3.9.0 source checkout: $freerdp_src" >&2
  echo "It is the .work/freerdp-spike checkout described in its README.md." >&2
  exit 1
}
actual_commit=$(git -C "$freerdp_src" rev-parse HEAD)
[[ $actual_commit == "$freerdp_commit" ]] || {
  echo "FreeRDP commit mismatch: $actual_commit != $freerdp_commit" >&2
  exit 1
}
grep -q 'set(RAW_VERSION_STRING "3.9.0")' "$freerdp_src/CMakeLists.txt" || {
  echo "FreeRDP source is not 3.9.0" >&2
  exit 1
}

cache="$root/cache"
srcdir="$root/src"
build="$root/build/ohos"
prefix="$root/ohos/prefix"
[[ $prefix == "$root/ohos/prefix" ]] || { echo 'Refusing unexpected prefix path' >&2; exit 1; }
mkdir -p "$cache" "$srcdir" "$root/build"
# The build directory and prefix are this script's own outputs: reset them so a
# rerun is a real from-scratch build and stale archives cannot survive.
rm -rf -- "$build" "$prefix"
mkdir -p "$build" "$prefix" "$srcdir"
exec > >(tee "$build/run.log") 2>&1
trap 'rc=$?; if ((rc)); then echo "FAILED exit=$rc mode=$mode; log=$build/run.log" >&2; fi' EXIT
printf 'MODE=%s\nBUILD=%s\nPREFIX=%s\nCACHE=%s\n' "$mode" "$build" "$prefix" "$cache"

# SHA256 provenance:
# OpenSSL 3.5.9 is the current LTS release; checksum from the official release
# asset openssl-3.5.9.tar.gz.sha256:
# https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz.sha256
# zlib 1.3.2 checksum is the SHA256 published at https://zlib.net/ ; the download
# below uses the official GitHub release mirror because zlib.net was slow in the
# implementation environment. Both archives are verified on every run, including
# cache reuse.
packages=(openssl-3.5.9 zlib-1.3.2)
archives=(openssl-3.5.9.tar.gz zlib-1.3.2.tar.gz)
urls=(
  https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz
  https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
)
sha256=(
  603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
  bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
)
for i in "${!packages[@]}"; do
  file="$cache/${archives[i]}"
  if [[ ! -f $file ]]; then
    part=$(mktemp "$cache/${archives[i]}.part.XXXXXXXX")
    curl --fail --location --show-error --silent --retry 3 \
      --connect-timeout 20 --max-time 900 "${urls[i]}" --output "$part"
    printf '%s  %s\n' "${sha256[i]}" "$part" | sha256sum --check --status
    ln "$part" "$file" || { [[ -f $file ]] || exit 1; }
    rm -f -- "$part"
  fi
  printf '%s  %s\n' "${sha256[i]}" "$file" | sha256sum --check
done

for package in "${packages[@]}"; do
  rm -rf -- "$srcdir/$package"
  tar -xf "$cache/$package.tar.gz" -C "$srcdir"
done

# Compiler wrappers: OpenSSL's Configure and zlib's configure take CC as a
# command name, so target/sysroot flags must live in the wrapper rather than in
# a CC value containing spaces.
toolchain="$build/toolchain"
mkdir -p "$toolchain"
cat > "$toolchain/ohos-cc" <<EOF
#!/bin/sh
exec '$llvm_cc' --target=aarch64-linux-ohos --sysroot='$sysroot' "\$@"
EOF
cat > "$toolchain/ohos-cxx" <<EOF
#!/bin/sh
exec '$llvm_cxx' --target=aarch64-linux-ohos --sysroot='$sysroot' "\$@"
EOF
chmod +x "$toolchain/ohos-cc" "$toolchain/ohos-cxx"
"$toolchain/ohos-cc" -dM -E -x c /dev/null > "$build/predefined-macros.txt"
grep -q '__aarch64__' "$build/predefined-macros.txt"
grep -q '__OHOS__' "$build/predefined-macros.txt"

# ---------------------------------------------------------------------------
# 1. zlib 1.3.2: static, PIC, into the shared prefix.
# ---------------------------------------------------------------------------
cp -a "$srcdir/zlib-1.3.2" "$build/zlib-src"
(cd "$build/zlib-src"
  CC="$toolchain/ohos-cc" CFLAGS="-O2 -fPIC" ./configure --static "--prefix=$prefix"
  make -j"$jobs"
  make install) > "$build/zlib.log" 2>&1 || {
    echo 'zlib build failed' >&2; tail -40 "$build/zlib.log" >&2; exit 1
  }

# ---------------------------------------------------------------------------
# 2. OpenSSL 3.5.9: static, no tests/apps/docs, no zstd, no dynamic modules.
#    AArch64 assembly is attempted first; if the integrated assembler rejects
#    it, the build is retried with no-asm.
# ---------------------------------------------------------------------------
openssl_src="$srcdir/openssl-3.5.9"
openssl_build="$build/openssl-build"
openssl_asm=yes
build_openssl() {
  local asm=$1
  local configure_opts=(
    linux-aarch64 no-shared no-tests no-apps no-docs no-zstd no-module
    "--prefix=$prefix" "--openssldir=$prefix/ssl" --libdir=lib -fPIC
  )
  if [[ $asm == no ]]; then configure_opts+=(no-asm); fi
  rm -rf -- "$openssl_build"
  mkdir -p "$openssl_build"
  (cd "$openssl_build"
    CC="$toolchain/ohos-cc" CXX="$toolchain/ohos-cxx" \
      AR="$llvm_ar" RANLIB="$llvm_ranlib" NM="$llvm_nm" \
      "$openssl_src/Configure" "${configure_opts[@]}"
    make -j"$jobs"
    make install_sw) > "$build/openssl.log" 2>&1
}
if ! build_openssl yes; then
  if grep -Eq '(^|[[:space:]])[^[:space:]]+\.S:[0-9]+:[0-9]+: error:|unknown (instruction|directive)|integrated assembler' \
    "$build/openssl.log"; then
    echo 'OpenSSL AArch64 assembly failed; retrying with no-asm' >&2
    openssl_asm=no
    build_openssl no || {
      echo 'OpenSSL no-asm build failed' >&2; tail -60 "$build/openssl.log" >&2; exit 1
    }
  else
    echo 'OpenSSL build failed (not an assembly error)' >&2
    tail -60 "$build/openssl.log" >&2
    exit 1
  fi
fi

# ---------------------------------------------------------------------------
# 3. FreeRDP 3.9.0: minimal client + winpr static PIC, OpenSSL/zlib from prefix.
#    OHOS musl libc exposes no pthread_cancel declaration or dynamic symbol, so
#    WinPR's TerminateThread must take the "not supported" branch. This is the
#    only source change and it is applied to a disposable copy of the checkout.
# ---------------------------------------------------------------------------
freerdp_copy="$build/freerdp-src"
freerdp_build="$build/freerdp-build"
cp -a "$freerdp_src" "$freerdp_copy"
cat > "$build/freerdp-ohos-pthread-cancel.patch" <<'PATCH'
diff --git a/winpr/libwinpr/thread/thread.c b/winpr/libwinpr/thread/thread.c
index f1b239f..fb77332 100644
--- a/winpr/libwinpr/thread/thread.c
+++ b/winpr/libwinpr/thread/thread.c
@@ -1035,7 +1035,7 @@ BOOL TerminateThread(HANDLE hThread, DWORD dwExitCode)
 	if (!run_mutex_fkt(pthread_mutex_lock, &thread->mutex))
 		return FALSE;
 
-#ifndef ANDROID
+#if !defined(ANDROID) && !defined(__OHOS__)
 	pthread_cancel(thread->thread);
 #else
 	WLog_ERR(TAG, "Function not supported on this platform!");
PATCH
patch -d "$freerdp_copy" -p1 --batch --forward \
  < "$build/freerdp-ohos-pthread-cancel.patch" > "$build/freerdp-patch.log" 2>&1 || {
    echo 'FreeRDP OHOS patch failed to apply' >&2
    cat "$build/freerdp-patch.log" >&2
    exit 1
  }

# Channel notes: 剪贴板通道（RDP 双向文本剪贴板）is enabled as
#   -DCHANNEL_CLIPRDR=ON -DCHANNEL_CLIPRDR_CLIENT=ON -DCHANNEL_CLIPRDR_SERVER=OFF.
# cliprdr is a static channel: its client OBJECT library is aggregated into the
# generated freerdp-channels-client sources and registered through
# CLIENT_STATIC_ADDIN_TABLE, so the code is archived into libfreerdp-client3.a
# (not just a -DCHANNEL_CLIPRDR define). The server side stays OFF.
# GFX（RemoteFX/progressive）动态通道；H.264 需要 ffmpeg，本构建未启用。
# It is a dynamic channel and therefore requires CHANNEL_DRDYNVC=ON (already
# enabled above). CHANNEL_VIDEO stays OFF: the RDPSND/VIDEO H.264 path needs
# WITH_FFMPEG, which this target build deliberately leaves off.
cmake -S "$freerdp_copy" -B "$freerdp_build" -G "Unix Makefiles" \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_SYSROOT="$sysroot" \
  -DCMAKE_C_COMPILER="$toolchain/ohos-cc" \
  -DCMAKE_C_COMPILER_TARGET=aarch64-linux-ohos \
  -DCMAKE_AR="$llvm_ar" -DCMAKE_RANLIB="$llvm_ranlib" \
  -DCMAKE_NM="$llvm_nm" -DCMAKE_STRIP="$llvm_strip" \
  -DCMAKE_FIND_ROOT_PATH="$prefix;$sysroot" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
  -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
  -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_PREFIX_PATH="$prefix" \
  -DOPENSSL_ROOT_DIR="$prefix" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DZLIB_ROOT="$prefix" -DZLIB_USE_STATIC_LIBS=ON \
  -DWITH_CLIENT=ON -DWITH_SERVER=OFF -DWITH_CLIENT_SDL=OFF -DWITH_X11=OFF -DWITH_WAYLAND=OFF \
  -DWITH_FFMPEG=OFF -DWITH_SWSCALE=OFF -DWITH_ALSA=OFF -DWITH_PULSE=OFF -DWITH_CUPS=OFF \
  -DWITH_PCSC=OFF -DWITH_FUSE=OFF -DWITH_KRB5=OFF -DWITH_SAMPLE=OFF -DWITH_MANPAGES=OFF \
  -DWITH_UNICODE_BUILTIN=ON -DWITH_CHANNELS=ON -DWITH_CLIENT_CHANNELS=ON \
  -DCHANNEL_DRDYNVC=ON -DCHANNEL_DRDYNVC_CLIENT=ON -DCHANNEL_DRDYNVC_SERVER=OFF \
  -DCHANNEL_RDPEI=ON -DCHANNEL_RDPEI_CLIENT=ON -DCHANNEL_RDPEI_SERVER=OFF \
  -DCHANNEL_CLIPRDR=ON -DCHANNEL_CLIPRDR_CLIENT=ON -DCHANNEL_CLIPRDR_SERVER=OFF \
  -DCHANNEL_AINPUT=OFF -DCHANNEL_AUDIN=OFF -DCHANNEL_DISP=OFF \
  -DCHANNEL_DRIVE=OFF -DCHANNEL_ECHO=OFF -DCHANNEL_ENCOMSP=OFF -DCHANNEL_GEOMETRY=OFF \
  -DCHANNEL_LOCATION=OFF -DCHANNEL_PARALLEL=OFF -DCHANNEL_PRINTER=OFF -DCHANNEL_RAIL=OFF \
  -DCHANNEL_RDPDR=OFF -DCHANNEL_RDPECAM=OFF -DCHANNEL_RDPEMSC=OFF \
  -DCHANNEL_RDPGFX=ON -DCHANNEL_RDPGFX_CLIENT=ON -DCHANNEL_RDPGFX_SERVER=OFF \
  -DCHANNEL_RDPSND=OFF -DCHANNEL_REMDESK=OFF -DCHANNEL_RDP2TCP=OFF -DCHANNEL_SERIAL=OFF \
  -DCHANNEL_SMARTCARD=OFF -DCHANNEL_SSHAGENT=OFF -DCHANNEL_TELEMETRY=OFF -DCHANNEL_TSMF=OFF \
  -DCHANNEL_URBDRC=OFF -DCHANNEL_VIDEO=OFF > "$build/freerdp-configure.log" 2>&1 || {
    echo 'FreeRDP configure failed' >&2; tail -60 "$build/freerdp-configure.log" >&2; exit 1
  }
cmake --build "$freerdp_build" --parallel "$jobs" > "$build/freerdp-build.log" 2>&1 || {
  echo 'FreeRDP build failed' >&2; tail -60 "$build/freerdp-build.log" >&2; exit 1
}
cmake --install "$freerdp_build" > "$build/freerdp-install.log" 2>&1 || {
  echo 'FreeRDP install failed' >&2; tail -60 "$build/freerdp-install.log" >&2; exit 1
}

# ---------------------------------------------------------------------------
# 4. Validation: archives contain only AArch64 ELF64 objects, the prefix has no
#    shared objects, and a -shared -Wl,-z,defs link closes over all static libs.
# ---------------------------------------------------------------------------
evidence="$build/evidence"
mkdir -p "$evidence"
libs=(libz.a libssl.a libcrypto.a libfreerdp3.a libfreerdp-client3.a libwinpr3.a)
if [[ -s $prefix/lib/libwinpr-tools3.a ]]; then libs+=(libwinpr-tools3.a); fi
for lib in "${libs[@]}"; do
  archive="$prefix/lib/$lib"
  [[ -s $archive ]] || { echo "Missing static library: $archive" >&2; exit 1; }
  "$llvm_readelf" -h "$archive" > "$evidence/$lib.elf.txt" 2>/dev/null || {
    echo "llvm-readelf failed on $archive (not an ELF archive?)" >&2; exit 1
  }
  objects=$(grep -c 'Machine:' "$evidence/$lib.elf.txt" || true)
  bad_machine=$(grep 'Machine:' "$evidence/$lib.elf.txt" | grep -vc 'AArch64' || true)
  bad_class=$(grep 'Class:' "$evidence/$lib.elf.txt" | grep -vc 'ELF64' || true)
  (( objects > 0 && bad_machine == 0 && bad_class == 0 )) || {
    echo "$lib has non-AArch64/ELF64 members (objects=$objects bad_machine=$bad_machine bad_class=$bad_class)" >&2
    exit 1
  }
  printf '  %-22s %5d objects  AArch64 ELF64\n' "$lib" "$objects"
done
if find "$prefix" -type f \( -name '*.so' -o -name '*.so.*' -o -name '*.dylib' \) | grep -q .; then
  echo 'Unexpected shared object installed in the static prefix:' >&2
  find "$prefix" -type f \( -name '*.so' -o -name '*.so.*' \) >&2
  exit 1
fi

linktest="$build/linktest"
mkdir -p "$linktest"
cat > "$linktest/linktest.c" <<'EOF'
/* Closure probe: one symbol from each required static library. */
extern void *freerdp_client_context_new(const void *entry_points);
extern void *SSL_CTX_new(const void *method);
extern int deflate(void *strm, int flush);
int rdp_deps_link_probe(void)
{
    void *ctx = freerdp_client_context_new(0);
    void *ssl = SSL_CTX_new(0);
    int z = deflate(0, 0);
    return (ctx != 0) || (ssl != 0) || (z != 0);
}
EOF
"$toolchain/ohos-cc" -O2 -fPIC -shared -Wl,-z,defs \
  -o "$linktest/librdp-deps-linktest.so" "$linktest/linktest.c" \
  -L"$prefix/lib" -lfreerdp-client3 -lfreerdp3 -lwinpr3 -lssl -lcrypto -lz \
  -lpthread -ldl -lm > "$linktest/link.log" 2>&1 || {
    echo 'Static closure link test failed' >&2; tail -40 "$linktest/link.log" >&2; exit 1
  }
"$llvm_readelf" -h -d "$linktest/librdp-deps-linktest.so" > "$evidence/linktest.elf.txt"
grep -q 'Machine:.*AArch64' "$evidence/linktest.elf.txt" || { echo 'Link test is not AArch64' >&2; exit 1; }
if grep -q 'TEXTREL' "$evidence/linktest.elf.txt"; then
  echo 'Link test has text relocations (non-PIC code)' >&2; exit 1
fi
mapfile -t needed < <(sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' "$evidence/linktest.elf.txt")
(( ${#needed[@]} > 0 )) || { echo 'Link test has no NEEDED entries' >&2; exit 1; }
for entry in "${needed[@]}"; do
  case $entry in
    libc.so|libm.so|libdl.so|libpthread.so|libc++_shared.so) ;;
    *) echo "Unexpected NEEDED in link test: $entry" >&2; exit 1 ;;
  esac
done
"$llvm_nm" --defined-only "$linktest/librdp-deps-linktest.so" > "$evidence/linktest.symbols.txt"
for symbol in freerdp_client_context_new SSL_CTX_new deflate; do
  grep -Eq "[[:space:]]$symbol$" "$evidence/linktest.symbols.txt" || {
    echo "Link test is missing $symbol" >&2; exit 1
  }
done

# ---------------------------------------------------------------------------
# 5. Manifest and report.
# ---------------------------------------------------------------------------
manifest="$prefix/share/rdp-deps-manifest.txt"
mkdir -p "$prefix/share"
{
  printf 'Purpose: static PIC dependencies for the FreeRDP 3.9.0 RDP client on OHOS aarch64\n'
  printf 'Mode: %s\nPrefix: %s\nBuilt: %s\nBuild log: %s/run.log\n' \
    "$mode" "$prefix" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$build"
  printf 'SDK: %s\nTarget: aarch64-linux-ohos\nSysroot: %s\n' "$sdk" "$sysroot"
  printf '\nOpenSSL: 3.5.9 (LTS)\n  Configure: linux-aarch64 no-shared no-tests no-apps no-docs no-zstd no-module -fPIC\n'
  printf '  AArch64 assembly: %s\n  URL: %s\n  SHA256: %s\n' \
    "$openssl_asm" "${urls[0]}" "${sha256[0]}"
  printf '\nzlib: 1.3.2\n  configure: --static, CFLAGS="-O2 -fPIC"\n  URL: %s\n  SHA256: %s\n' \
    "${urls[1]}" "${sha256[1]}"
  printf '\nFreeRDP: 3.9.0\n  Commit: %s\n  Source: %s\n' "$freerdp_commit" "$freerdp_src"
  printf '  CMake: WITH_CLIENT=ON WITH_SERVER=OFF, SDL/X11/Wayland/FFmpeg/ALSA/Pulse/CUPS/PCSC/FUSE/KRB5/SAMPLE/MANPAGES off,\n'
  printf '         WITH_UNICODE_BUILTIN=ON, WITH_CHANNELS=ON, only drdynvc+rdpei+cliprdr+rdpgfx client channels on,\n'
  printf '         BUILD_SHARED_LIBS=OFF, CMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF, CMAKE_POSITION_INDEPENDENT_CODE=ON,\n'
  printf '         OPENSSL_ROOT_DIR/ZLIB_ROOT=%s, OPENSSL_USE_STATIC_LIBS=ON.\n' "$prefix"
  printf '  Source patch: winpr/libwinpr/thread/thread.c TerminateThread uses the "not supported"\n'
  printf '  branch on __OHOS__ as well as ANDROID, because OHOS musl has no pthread_cancel\n'
  printf '  declaration or dynamic symbol. No other source changes.\n'
  printf '\nLink order for consumers (static):\n'
  printf '  -lfreerdp-client3 -lfreerdp3 -lwinpr3 -lwinpr-tools3 -lssl -lcrypto -lz -lpthread -ldl -lm\n'
  printf '  Include dirs: %s/include/freerdp3 %s/include/winpr3 %s/include\n' "$prefix" "$prefix" "$prefix"
  printf '  The link test used -shared -Wl,-z,defs and produced NEEDED: %s\n' "${needed[*]}"
  printf '\nEvidence: %s/*.elf.txt, %s/linktest.symbols.txt\n' "$evidence" "$evidence"
} > "$manifest"

printf '\nArchive sizes:\n'
ls -l "$prefix/lib/"*.a | tee "$evidence/sizes.txt"
printf '\nLink-test NEEDED: %s\n' "${needed[*]}"
printf '\nSUCCESS mode=%s\nPREFIX=%s\nMANIFEST=%s\nBUILD_LOG=%s/run.log\n' \
  "$mode" "$prefix" "$manifest" "$build"
