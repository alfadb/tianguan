#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fixed-source static PIC dependencies for the TigerVNC 1.15.0 RA2 experiment.
# Usage: scripts/build-vnc-deps.sh host|ohos [jobs: 1..4]
# Requires existing Linux build tools; installs nothing system-wide.
# Each invocation keeps a fresh build/source tree and prefix under .work/vnc-deps.
# No cleanup is performed. The final PREFIX= line is the consumer's install root.
set -euo pipefail
export LC_ALL=C
mode=${1:-}
jobs=${2:-4}
case "$mode" in host|ohos) ;; *) echo "Usage: $0 host|ohos [jobs: 1..4]" >&2; exit 2;; esac
[[ $# -le 2 && $jobs =~ ^[1-4]$ ]] || { echo 'jobs must be 1..4' >&2; exit 2; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
root="$repo/.work/vnc-deps"
cache="$root/cache"
for tool in curl sha256sum tar xz cmake make gcc g++ ar ranlib nm readelf mktemp; do
  command -v "$tool" >/dev/null || { echo "Missing existing tool: $tool" >&2; exit 1; }
done
# Avoid inheriting another target's compiler, include search paths, or make flags.
unset CC CXX CPP AR AS LD NM RANLIB STRIP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH PKG_CONFIG_PATH MAKEFLAGS
export CONFIG_SITE=/dev/null
mkdir -p "$cache" "$root/runs"
run=$(mktemp -d "$root/runs/$mode.XXXXXXXX")
prefix="$run/prefix"
src="$run/source"
mkdir -p "$prefix" "$src"
exec > >(tee "$run/build.log") 2>&1
trap 'rc=$?; if ((rc)); then echo "FAILED exit=$rc mode=$mode run=$run" >&2; fi' EXIT
printf 'MODE=%s\nRUN=%s\nSOURCE_CACHE=%s\nPREFIX=%s\n' "$mode" "$run" "$cache" "$prefix"

# SHA256 provenance (verified when this entry point was implemented):
# zlib: published SHA256 at https://zlib.net/ (1.3.2 release).
# JPEG: calculated from official tarball after official detached signature verified;
# fingerprint 0338C8D8D9FDA62CF9C421BD7EC2DBB6F4DBF434, see
# https://libjpeg-turbo.org/Downloads/DigitalSignatures . GitHub asset digest is null.
# nettle: calculated after detached signature verified with official distribution key
# 343C2FF0FBEE5EC2EDBEF399F3599FF828C67298, published at
# https://www.lysator.liu.se/~nisse/nettle/ .
# GMP: calculated after its detached signature verified with the same Niels Moeller
# key above. Download uses a mirror listed at https://www.gnu.org/prep/ftp.html
# because ftp.gnu.org and gmplib.org failed TLS in the implementation environment.
# Canonical release: https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz .
# SHA256 is always checked on cache reuse; no GPG keyring is needed to run this script.
packages=(zlib-1.3.2 libjpeg-turbo-3.1.0 gmp-6.3.0 nettle-3.10.1)
archives=(zlib-1.3.2.tar.gz libjpeg-turbo-3.1.0.tar.gz gmp-6.3.0.tar.xz nettle-3.10.1.tar.gz)
urls=(
  https://zlib.net/zlib-1.3.2.tar.gz
  https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.1.0/libjpeg-turbo-3.1.0.tar.gz
  https://mirrors.kernel.org/gnu/gmp/gmp-6.3.0.tar.xz
  https://www.lysator.liu.se/~nisse/archive/nettle-3.10.1.tar.gz
)
sha256=(
  bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
  9564c72b1dfd1d6fe6274c5f95a8d989b59854575d4bbee44ade7bc17aa9bc93
  a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
  b0fcdd7fc0cdea6e80dcf1dd85ba794af0d5b4a57e26397eee3bc193272d9132
)
for i in "${!packages[@]}"; do
  file="$cache/${archives[i]}"
  if [[ ! -f $file ]]; then
    # Unique incomplete file: interruptions cannot poison the canonical cache.
    part=$(mktemp "$cache/${archives[i]}.part.XXXXXXXX")
    curl --fail --location --show-error --silent --retry 2 \
      --connect-timeout 20 --max-time 600 "${urls[i]}" --output "$part"
    printf '%s  %s\n' "${sha256[i]}" "$part" | sha256sum --check --status
    # The destination was just checked; do not overwrite a concurrently populated file.
    ln "$part" "$file" || { [[ -f $file ]] || exit 1; }
  fi
  printf '%s  %s\n' "${sha256[i]}" "$file" | sha256sum --check
  tar -xf "$file" -C "$src"
done

cc=$(command -v gcc)
cxx=$(command -v g++)
export AR=$(command -v ar) RANLIB=$(command -v ranlib) NM=$(command -v nm)
readelf=$(command -v readelf)
flags='-O2 -fPIC'
cross=()
cmake_target=()
if [[ $mode == ohos ]]; then
  sdk=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/native
  # Deliberately do not use current or DEVECO_SDK_HOME.
  test -f "$sdk/oh-uni-package.json"
  grep -Eq '"apiVersion": "26"' "$sdk/oh-uni-package.json"
  grep -Eq '"version": "26.0.0.105"' "$sdk/oh-uni-package.json"
  cc="$sdk/llvm/bin/clang"
  cxx="$sdk/llvm/bin/clang++"
  export AR="$sdk/llvm/bin/llvm-ar" RANLIB="$sdk/llvm/bin/llvm-ranlib" NM="$sdk/llvm/bin/llvm-nm"
  readelf="$sdk/llvm/bin/llvm-readelf"
  target_flags="--target=aarch64-linux-ohos --sysroot=$sdk/sysroot"
  flags="$flags $target_flags"
  # Autoconf's platform classification uses Linux/musl; clang's real target remains
  # aarch64-linux-ohos. Explicit --build/--host prevents running target probes.
  build_triple=$("$src/nettle-3.10.1/config.guess")
  cross=("--build=$build_triple" --host=aarch64-unknown-linux-musl)
  cmake_target=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
    -DCMAKE_C_COMPILER_TARGET=aarch64-linux-ohos
    -DCMAKE_CXX_COMPILER_TARGET=aarch64-linux-ohos
    "-DCMAKE_SYSROOT=$sdk/sysroot")
fi
export CC="$cc" CXX="$cxx" CFLAGS="$flags" CXXFLAGS="$flags" CC_FOR_BUILD=gcc
"$CC" $CFLAGS --version
cmake --version
"$CC" $CFLAGS -dM -E -x c /dev/null > "$run/compiler-macros.txt"
if [[ $mode == ohos ]]; then
  grep -q '__aarch64__' "$run/compiler-macros.txt"
  grep -q '__OHOS__' "$run/compiler-macros.txt"
fi

(cd "$src/zlib-1.3.2"; ./configure --static "--prefix=$prefix"; make -j"$jobs";
  if [[ $mode == host ]]; then make -j"$jobs" check; fi
  make install)
cmake -S "$src/libjpeg-turbo-3.1.0" -B "$run/jpeg" \
  -G 'Unix Makefiles' "-DCMAKE_INSTALL_PREFIX=$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_BUILD_TYPE=Release -DBUILD=20241212 "-DCMAKE_C_COMPILER=$CC" "-DCMAKE_C_FLAGS=$CFLAGS" \
  "-DCMAKE_AR=$AR" "-DCMAKE_RANLIB=$RANLIB" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_SIMD=OFF \
  "${cmake_target[@]}"
cmake --build "$run/jpeg" --parallel "$jobs"
if [[ $mode == host ]]; then (cd "$run/jpeg"; ctest --parallel "$jobs" --output-on-failure); fi
cmake --install "$run/jpeg"
mkdir "$run/gmp"
(cd "$run/gmp"; "$src/gmp-6.3.0/configure" "--prefix=$prefix" --libdir="$prefix/lib" \
  --disable-shared --enable-static --with-pic --disable-assembly "${cross[@]}";
  make -j"$jobs"; if [[ $mode == host ]]; then make -j"$jobs" check; fi; make install)
mkdir "$run/nettle"
(cd "$run/nettle"; "$src/nettle-3.10.1/configure" "--prefix=$prefix" --libdir="$prefix/lib" \
  --disable-shared --enable-static --enable-pic --disable-assembler \
  --disable-documentation --disable-openssl "--with-include-path=$prefix/include" \
  "--with-lib-path=$prefix/lib" "${cross[@]}";
  make -j"$jobs"; if [[ $mode == host ]]; then make -j"$jobs" check; fi; make install)

# Preserve release license/notice files as well as the untouched source archives.
licenses="$prefix/share/licenses/vnc-deps"
mkdir -p "$licenses"
for package in "${packages[@]}"; do
  mkdir -p "$licenses/$package"
  while IFS= read -r -d '' path; do
    relative=${path#"$src/$package/"}
    mkdir -p "$licenses/$package/$(dirname "$relative")"
    cp -p "$path" "$licenses/$package/$relative"
  done < <(find "$src/$package" -type f \( -iname 'copying*' -o -iname 'license*' \
    -o -iname 'licence*' -o -iname 'notice*' -o -name README.ijg \) -print0)
done
manifest="$prefix/share/vnc-deps-manifest.txt"
{
  printf 'Purpose: TigerVNC 1.15.0 RA2 minimal experiment dependencies (no GnuTLS)\n'
  printf 'Mode: %s\nPrefix: %s\nSource archives: %s\nSources and build log: %s\n' "$mode" "$prefix" "$cache" "$run"
  printf 'Build: static PIC; GMP/nettle assembly disabled; JPEG SIMD/TurboJPEG disabled.\n'
  printf 'Source modifications: none.\n'
  for i in "${!packages[@]}"; do
    printf '\n%s\nURL: %s\nSHA256: %s\nLicense files: %s/%s\n' \
      "${packages[i]}" "${urls[i]}" "${sha256[i]}" "$licenses" "${packages[i]}"
  done
  cat <<'LICENSE_SCOPE'

Checksum provenance established while implementing this entry point:
zlib: SHA256 published at https://zlib.net/ for release 1.3.2.
JPEG: computed SHA256 after verifying official .tar.gz.sig; signing fingerprint
0338C8D8D9FDA62CF9C421BD7EC2DBB6F4DBF434, published at
https://libjpeg-turbo.org/Downloads/DigitalSignatures .
GMP and nettle: computed SHA256 after verifying official .sig files; fingerprint
343C2FF0FBEE5EC2EDBEF399F3599FF828C67298, official distribution key at
https://www.lysator.liu.se/~nisse/archive/distribution-key.gpg .
GMP download mirror is listed at https://www.gnu.org/prep/ftp.html .
The script rechecks pinned SHA256 on every run; it does not redo GPG verification.

Declared license overview (consult preserved upstream files):
zlib: Zlib.
libjpeg-turbo: IJG / BSD-3-Clause / Zlib; see LICENSE.md and README.ijg.
GMP: LGPL-3.0-or-later OR GPL-2.0-or-later (see COPYING*).
nettle/hogweed: LGPL-3.0-or-later OR GPL-2.0-or-later, with upstream per-file exceptions.
Boundary: release license/notice filenames copied recursively; no full per-file
license audit, dependency-wide compliance conclusion, or SDK licensing review.
This software is based in part on the work of the Independent JPEG Group.
LICENSE_SCOPE
  if [[ $mode == ohos ]]; then
    printf '\nSDK: %s\nTarget: aarch64-linux-ohos\nSysroot: %s/sysroot\n' "$sdk" "$sdk"
    cat "$sdk/oh-uni-package.json"
    printf '\n'
  fi
  "$CC" $CFLAGS --version
} > "$manifest"

# Verify every archive member's ELF machine and required public symbols.
for lib in z jpeg gmp nettle hogweed; do
  archive="$prefix/lib/lib$lib.a"
  test -s "$archive"
  "$readelf" -h "$archive" > "$run/$lib.elf.txt"
  if [[ $mode == ohos ]]; then
    awk '/Machine:/{n++; if ($0 !~ /AArch64/) bad=1} END{exit (!n || bad)}' "$run/$lib.elf.txt"
  fi
  "$NM" -g --defined-only "$archive" > "$run/$lib.symbols.txt"
done
for pair in 'z deflate' 'jpeg jpeg_std_error' 'gmp __gmpz_init' \
  'nettle nettle_aes128_encrypt' 'nettle nettle_sha1_digest' 'hogweed nettle_rsa_public_key_init'; do
  read -r lib symbol <<< "$pair"
  grep -Eq "[[:space:]]$symbol$" "$run/$lib.symbols.txt"
done
# Whole-archive shared linking tests PIC and closure across all five static libs.
"$CC" $CFLAGS -shared -Wl,-z,defs -Wl,--whole-archive \
  "$prefix/lib/libz.a" "$prefix/lib/libjpeg.a" "$prefix/lib/libhogweed.a" \
  "$prefix/lib/libnettle.a" "$prefix/lib/libgmp.a" -Wl,--no-whole-archive \
  -lm -o "$run/libdeps-pic-check.so"
"$readelf" -h -d "$run/libdeps-pic-check.so" > "$run/pic-check.elf.txt"
if grep -q TEXTREL "$run/pic-check.elf.txt"; then echo 'Unexpected text relocations' >&2; exit 1; fi
printf '\nValidation: all five archives have symbols; whole-archive PIC shared link passed.\n' >> "$manifest"
printf 'Evidence: %s/*.elf.txt and %s/*.symbols.txt\n' "$run" "$run" >> "$manifest"
printf '\nSUCCESS mode=%s\nPREFIX=%s\nSOURCE_CACHE=%s\nMANIFEST=%s\nBUILD_LOG=%s/build.log\n' \
  "$mode" "$prefix" "$cache" "$manifest" "$run"
