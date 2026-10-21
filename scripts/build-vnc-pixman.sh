#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: bash scripts/build-vnc-pixman.sh host|ohos-arm64 [jobs: 1..4]
# Existing tools only; no source patches, system installs, or SDK modifications.
set -euo pipefail
export LC_ALL=C
mode=${1:-}
jobs=${2:-4}
case "$mode" in host|ohos-arm64) ;; *) echo "Usage: $0 host|ohos-arm64 [jobs: 1..4]" >&2; exit 2;; esac
[[ $# -le 2 && $jobs =~ ^[1-4]$ ]] || { echo 'jobs must be 1..4' >&2; exit 2; }
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
root="$repo/.work/vnc-pixman"
version=0.42.2
sha=ea1480efada2fd948bc75366f7c349e1c96d3297d09a3fe62626e38e234a625e
url="https://cairographics.org/releases/pixman-$version.tar.gz"
announcement=https://lists.freedesktop.org/archives/pixman/2022-November/004994.html
sdk=/home/worker/harmonyos/command-line-tools/26.0.0.821/sdk/default/openharmony/native
for tool in curl sha256sum tar make gcc ar ranlib nm readelf flock mktemp tee awk; do
  command -v "$tool" >/dev/null || { echo "Missing existing tool: $tool" >&2; exit 1; }
done
mkdir -p "$root/cache" "$root/$mode"
exec 9>"$root/build.lock"
flock 9
prefix="$root/$mode/prefix"
source="$root/pixman-$version"
build="$root/$mode/build"
archive="$root/cache/pixman-$version.tar.gz"
exec > >(tee -a "$root/$mode/build.log") 2>&1
trap 'rc=$?; echo "EXIT=$rc MODE=$mode PREFIX=$prefix"' EXIT
printf '\nMODE=%s\nPREFIX=%s\n' "$mode" "$prefix"
if [[ ! -f $archive ]]; then
  part=$(mktemp "$root/cache/pixman-$version.tar.gz.part.XXXXXXXX")
  curl --fail --location --silent --show-error --retry 2 --connect-timeout 20 --max-time 600 "$url" --output "$part"
  printf '%s  %s\n' "$sha" "$part" | sha256sum --check
  # Fixed destination is absent under the lock; failed downloads are retained.
  test ! -e "$archive"
  mv -- "$part" "$archive"
fi
printf '%s  %s\n' "$sha" "$archive" | sha256sum --check
if [[ ! -d $source ]]; then tar -xzf "$archive" -C "$root"; fi
mkdir -p "$build" "$prefix"
unset CC CXX CPP AR AS LD NM RANLIB STRIP CFLAGS CXXFLAGS CPPFLAGS LDFLAGS LIBS
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH PKG_CONFIG_PATH
unset PKG_CONFIG_SYSROOT_DIR PKG_CONFIG_LIBDIR MAKEFLAGS MFLAGS CCAS CCASFLAGS
unset GTK_CFLAGS GTK_LIBS PNG_CFLAGS PNG_LIBS LT_SYS_LIBRARY_PATH
export CONFIG_SITE=/dev/null PKG_CONFIG=/bin/false
export CC=$(command -v gcc) AR=$(command -v ar) RANLIB=$(command -v ranlib) NM=$(command -v nm)
reader=$(command -v readelf)
export CFLAGS='-O2 -fPIC'
cross=()
if [[ $mode == ohos-arm64 ]]; then
  export CC="$sdk/llvm/bin/clang" AR="$sdk/llvm/bin/llvm-ar"
  export RANLIB="$sdk/llvm/bin/llvm-ranlib" NM="$sdk/llvm/bin/llvm-nm"
  reader="$sdk/llvm/bin/llvm-readelf"
  export CFLAGS="-O2 -fPIC --target=aarch64-linux-ohos --sysroot=$sdk/sysroot"
  # Autoconf recognizes Linux/musl; clang targets the actual OHOS ABI.
  # LLVM 15 cannot assemble this release's GNU altmacro ARM64 NEON code.
  cross=("--build=$("$source/config.guess")" --host=aarch64-unknown-linux-musl --disable-arm-a64-neon)
fi
"$CC" --version
"$CC" $CFLAGS -dM -E -x c /dev/null > "$root/$mode/compiler-macros.txt"
if [[ $mode == ohos-arm64 ]]; then
  grep -q '__OHOS__' "$root/$mode/compiler-macros.txt"
  grep -q '__aarch64__' "$root/$mode/compiler-macros.txt"
fi
(
  cd "$build"
  "$source/configure" "--prefix=$prefix" "--libdir=$prefix/lib" \
    --disable-shared --enable-static --with-pic \
    --disable-gtk --disable-libpng --disable-openmp "${cross[@]}"
  # No --disable-tests in this release. Never build test/demo directories.
  make -C pixman -j"$jobs"
  make -C pixman install
  make install-pkgconfigDATA
)
licenses="$prefix/share/licenses/pixman"
mkdir -p "$licenses"
cp -p "$source/COPYING" "$licenses/COPYING"
lib="$prefix/lib/libpixman-1.a"
test -s "$lib"
"$AR" t "$lib" > "$root/$mode/archive-members.txt"
"$reader" -h "$lib" > "$root/$mode/archive-headers.txt"
expected='Advanced Micro Devices X86-64'
if [[ $mode == ohos-arm64 ]]; then expected=AArch64; fi
awk -v expected="$expected" '/Machine:/{n++; if (index($0, expected)==0) bad=1} END{exit (!n || bad)}' "$root/$mode/archive-headers.txt"
"$NM" -g --defined-only "$lib" > "$root/$mode/archive-symbols.txt"
awk '$NF == "pixman_region_init" && $(NF-1) == "T" {found=1} END{exit !found}' "$root/$mode/archive-symbols.txt"
# Link the exact archive, bypassing host pkg-config; run host only.
printf '#include <pixman.h>\nint main(void) { pixman_region16_t r; pixman_region_init(&r); pixman_region_fini(&r); return 0; }\n' > "$root/$mode/link-smoke.c"
"$CC" $CFLAGS "-I$prefix/include/pixman-1" "$root/$mode/link-smoke.c" "$lib" -lm -o "$root/$mode/link-smoke"
"$reader" -h -d "$root/$mode/link-smoke" > "$root/$mode/link-smoke.elf.txt"
# Force every member into a shared link and reject non-PIC text relocations.
"$CC" $CFLAGS -shared -Wl,-z,text -Wl,--whole-archive "$lib" \
  -Wl,--no-whole-archive -lm -o "$root/$mode/pic-smoke.so"
"$reader" -h -d "$root/$mode/pic-smoke.so" > "$root/$mode/pic-smoke.elf.txt"
if [[ $mode == host ]]; then "$root/$mode/link-smoke"; fi
{
  printf 'pixman %s\nMode: %s\nPrefix: %s\nURL: %s\nSHA256: %s\nPublished SHA256: %s\n' "$version" "$mode" "$prefix" "$url" "$sha" "$announcement"
  printf 'SHA256 checked each run. No patches. Release signature not verified.\n'
  printf 'Static PIC (-fPIC/--with-pic); GTK/libpng/OpenMP disabled; tests/demos excluded by library-only make.\n'
  printf 'pkg-config disabled during configure; explicit archive used in link smoke.\n'
  printf 'License: upstream MIT-style terms; preserved %s/COPYING.\n' "$licenses"
  printf 'Boundary: release COPYING inspected; complete unchanged source retained for per-file notices. No full per-file audit, dependency compliance conclusion, or SDK license review.\n'
  printf 'Evidence beside prefix: archive-members.txt, archive-headers.txt, archive-symbols.txt, link-smoke.elf.txt, pic-smoke.elf.txt, build.log.\n'
  printf 'Whole-archive shared link with -z text passed (PIC smoke).\n'
  printf 'Archive members all match %s; pixman_region_init defined; smoke linked.\n' "$expected"
  if [[ $mode == ohos-arm64 ]]; then
    printf 'ARM64 NEON assembly disabled: release GNU altmacro code incompatible with SDK clang 15; generic C paths retained.\n'
    printf 'SDK: %s\nTarget: aarch64-linux-ohos; sysroot: %s/sysroot\nTarget binary not executed; no device/runtime validation.\n' "$sdk" "$sdk"
  else
    printf 'Host smoke executed successfully.\n'
  fi
  "$CC" --version
  sha256sum "$lib" "$licenses/COPYING"
} > "$prefix/share/vnc-pixman-manifest.txt"
printf 'PREFIX=%s\n' "$prefix"
