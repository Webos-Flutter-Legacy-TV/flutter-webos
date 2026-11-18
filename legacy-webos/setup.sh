#!/usr/bin/env bash
# Fetch everything legacy-webos needs into toolchains/ and downloads/
# (both git-ignored). Nothing is installed system-wide.
#
#   WEBOS_NDK_ACCEPT_EULA=1 ./setup.sh
#
# The LG webOS NDK is licensed under the LG SDK License Agreement; the
# installer shows it, and setting WEBOS_NDK_ACCEPT_EULA=1 means you accept it.
set -euo pipefail
FW="$(cd "$(dirname "$0")" && pwd)"
TC="$FW/toolchains"; DL="$FW/downloads"
mkdir -p "$TC" "$DL"

BUILDROOT_URL="https://github.com/openlgtv/buildroot-nc4/releases/download/2026.08-webos/arm-webos-linux-gnueabi_sdk-buildroot-x86_64.tar.gz"
NDK_VERSION=11.2.0
NDK_URL="https://github.com/lg-flutter-webos/ndk/releases/download/$NDK_VERSION/webos-ndk-flutter-starfish-x86_64-ca9v1-$NDK_VERSION.sh"
DEBIAN_MIRROR="https://deb.debian.org/debian"
DEBIAN_SUITE=trixie
# libsystemd0: fallback for webOS 4, whose firmware has no libsystemd.so.0
# (LG's media library uses its sd_event loop). Needs only glibc and libcap2.
DEBIAN_PACKAGES="libc6 libsystemd0"

step() { printf '\n== %s\n' "$*"; }

step "webosbrew toolchain (buildroot-nc4): builds the shims against glibc 2.12"
if [ ! -x "$TC/webosbrew/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc" ]; then
  curl -fL "$BUILDROOT_URL" -o "$DL/buildroot-nc4.tar.gz"
  mkdir -p "$TC/webosbrew"
  tar -xzf "$DL/buildroot-nc4.tar.gz" -C "$TC/webosbrew"
  (cd "$TC/webosbrew/arm-webos-linux-gnueabi_sdk-buildroot" && ./relocate-sdk.sh >/dev/null)
fi

step "Debian $DEBIAN_SUITE armel runtime: bundled into compat IPKs"
# armel = the same soft-float ABI as webOS userspace; Debian's glibc is built
# for kernel >= 3.2 (LG's NDK glibc needs >= 5.15, which old TVs do not have).
mkdir -p "$DL/debian-armel/root"
[ -s "$DL/debian-armel/Packages" ] ||
  curl -fsSL "$DEBIAN_MIRROR/dists/$DEBIAN_SUITE/main/binary-armel/Packages.xz" | xz -d > "$DL/debian-armel/Packages"
for p in $DEBIAN_PACKAGES; do
  [ -e "$DL/debian-armel/.installed-$p" ] && continue
  read -r file sha < <(awk -v p="$p" '$0=="Package: "p{f=1} f&&/^Filename:/{fn=$2} f&&/^SHA256:/{print fn, $2; exit}' "$DL/debian-armel/Packages")
  deb="$DL/debian-armel/$(basename "$file")"
  curl -fsSL "$DEBIAN_MIRROR/$file" -o "$deb"
  echo "$sha  $deb" | sha256sum -c --quiet
  dpkg-deb -x "$deb" "$DL/debian-armel/root"
  touch "$DL/debian-armel/.installed-$p"
  echo "  $(basename "$file")"
done

step "patchelf (Python venv)"
[ -x "$TC/pyenv/bin/patchelf" ] || { python3 -m venv "$TC/pyenv" && "$TC/pyenv/bin/pip" install -q patchelf; }

step "webOS CLI (ares), local npm install"
[ -x "$TC/ares/node_modules/.bin/ares-package" ] || npm install --prefix "$TC/ares" @webos-tools/cli --no-fund --no-audit >/dev/null

step "LG webOS NDK $NDK_VERSION (needed by flutter-webos to build apps)"
if [ ! -e "$TC/lg-ndk-$NDK_VERSION/environment-setup-ca9v1-webosmllib32-linux-gnueabi" ]; then
  if [ "${WEBOS_NDK_ACCEPT_EULA:-0}" != 1 ]; then
    echo "  skipped: set WEBOS_NDK_ACCEPT_EULA=1 to accept the LG SDK License Agreement and install it"
  else
    curl -fL "$NDK_URL" -o "$DL/webos-ndk-$NDK_VERSION.sh"
    sh "$DL/webos-ndk-$NDK_VERSION.sh" -y -d "$TC/lg-ndk-$NDK_VERSION"
  fi
fi

step "shims"
"$FW/tools/build-shims.sh"
echo; echo "Done. Next:  . $FW/env.sh"
