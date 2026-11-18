#!/usr/bin/env bash
# Repackage a flutter-webos (webOS 26) build so it runs on older webOS TVs
# ("option B"): bundle a newer glibc + the NDK's C++ runtime and a
# Wayland >=1.20 shim, and start the app through the bundled dynamic loader.
#
# Usage: tools/make-compat-ipk.sh <flutter-app-dir> [release|profile|debug]
set -euo pipefail

FW="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$(cd "$1" && pwd)"
MODE="${2:-release}"
# Runtime to bundle.
# - glibc: LG's NDK glibc is built for kernel >= 5.15 (uses statx etc.) and
#   fails on older TVs with ENOSYS, so use Debian 13 armel glibc 2.41
#   (same soft-float ABI, min kernel 3.2).
# - C++ runtime: from the LG NDK (GCC 13, ARMv7), the build LG's binaries were
#   linked against. Debian's armel libstdc++ targets ARMv5TE and lacks part of
#   std::future (e.g. __future_base::_Result_base), which plugins use.
GLIBC_LIB="${COMPAT_GLIBC_LIB:-$FW/downloads/debian-armel/root/usr/lib/arm-linux-gnueabi}"
CXX_LIB="${COMPAT_CXX_LIB:-$FW/toolchains/lg-ndk-11.2.0/sysroots/ca9v1-webosmllib32-linux-gnueabi/usr/lib}"
SHIM="$FW/compat/wayland-shim/out/libwl-marshal-flags-shim.so"
PATCHELF="$FW/toolchains/pyenv/bin/patchelf"
ARES_PACKAGE="$FW/toolchains/ares/node_modules/.bin/ares-package"

"$FW/tools/build-shims.sh"

BUNDLE="$APP_DIR/build/webos/arm/$MODE/bundle"
[ -f "$BUNDLE/appinfo.json" ] || { echo "No bundle at $BUNDLE; run flutter-webos build webos --$MODE first" >&2; exit 1; }

APP_ID=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["id"])' "$BUNDLE/appinfo.json")
MAIN=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["main"])' "$BUNDLE/appinfo.json")
INSTALL_DIR="/media/developer/apps/usr/palm/applications/$APP_ID"

OUT="$APP_DIR/build/webos-compat/$MODE"
STAGE="$OUT/stage/$APP_ID"
rm -rf "$OUT"; mkdir -p "$STAGE"
cp -a "$BUNDLE/." "$STAGE/"
mkdir -p "$STAGE/lib/compat"

# glibc + C++ runtime (OSS components).
for f in ld-linux.so.3 libc.so.6 libm.so.6 libpthread.so.0 libdl.so.2 librt.so.1 \
         libresolv.so.2 libutil.so.1 libanl.so.1 libnss_files.so.2 libnss_dns.so.2; do
  cp -L "$GLIBC_LIB/$f" "$STAGE/lib/compat/$f"
done
[ -e "$CXX_LIB/libstdc++.so.6" ] || { echo "LG NDK not found ($CXX_LIB); run WEBOS_NDK_ACCEPT_EULA=1 ./setup.sh" >&2; exit 1; }
for f in libstdc++.so.6 libgcc_s.so.1 libatomic.so.1; do
  cp -L "$CXX_LIB/$f" "$STAGE/lib/compat/$f"
done
cp "$SHIM" "$STAGE/lib/compat/"

# webOS firmware lists /lib/libSegFault.so (built against the TV's old glibc)
# in /etc/ld.so.preload, which every process loads. Inside our newer glibc it
# is a foreign library whose crash handler can misbehave. ld.so has no switch
# to skip the preload file, so point our bundled loader at a path that never
# exists (same length, so the binary layout is unchanged).
python3 - "$STAGE/lib/compat/ld-linux.so.3" <<'EOF'
import sys
p = sys.argv[1]
b = open(p, "rb").read()
old, new = b"/etc/ld.so.preload\0", b"/etc/ld.so.nopreld\0"
assert len(old) == len(new)
n = b.count(old)
if n != 1:
    sys.exit(f"expected one {old!r} in {p}, found {n}")
open(p, "wb").write(b.replace(old, new))
EOF

# The app binary stays the "main" executable: webOS registers the app's Luna
# bus permissions for that exact path, so no wrapper script may sit in front.
# Make it self-sufficient instead:
#  - ELF interpreter -> bundled loader
#  - DT_RPATH (not RUNPATH: jailer sets LD_LIBRARY_PATH, which beats RUNPATH) -> lib/compat first
#  - bundled libpthread/libdl/librt + the Wayland shim as direct NEEDED entries,
#    so system libraries (Mali, glib, Luna...) reuse them instead of loading the
#    TV's old glibc pieces, and the shim's wl_proxy_marshal_flags is in scope.
chmod -R u+w "$STAGE"
EXE="$STAGE/$MAIN"
"$PATCHELF" --set-interpreter "$INSTALL_DIR/lib/compat/ld-linux.so.3" "$EXE"
"$PATCHELF" --force-rpath --set-rpath '$ORIGIN/lib/compat:$ORIGIN/lib:$ORIGIN' "$EXE"
"$PATCHELF" --add-needed libwl-marshal-flags-shim.so --add-needed libpthread.so.0 \
            --add-needed libdl.so.2 --add-needed librt.so.1 "$EXE"
# Media: on webOS <= 25, NDL_MediaInit() asks SDL for an exported window, even
# for audio-only playback. If any bundled library links libNDL_media
# (video_player, audioplayers, ...), add the NDL/SDL compat library
# (SONAME libSDL2-2.0.so.0) so NDL does not load the system SDL.
if grep -l "libNDL_media.so" "$STAGE"/lib/*.so >/dev/null 2>&1; then
  cp "$FW/compat/ndl-sdl-shim/out/libSDL2-2.0.so.0" "$STAGE/lib/compat/"
  "$PATCHELF" --add-needed libSDL2-2.0.so.0 "$EXE"
fi
# Opt-in Widevine CDM proxy: only when the app ships webos/meta/compat-widevine-cdm.
if [ -e "$STAGE/compat-widevine-cdm" ]; then
  [ -e "$FW/compat/widevine/out/libwv-compat.so" ] || { echo "libwv-compat.so not built (needs the LG NDK)" >&2; exit 1; }
  cp "$FW/compat/widevine/out/libwv-compat.so" "$STAGE/lib/compat/"
fi
for f in "$STAGE"/lib/*.so; do
  "$PATCHELF" --force-rpath --set-rpath '$ORIGIN/compat:$ORIGIN' "$f"
done

# wayland-egl: firmware ships the vendor (Mali) library as libwayland-egl.so.1
# or only as the older libwayland-egl.so.0 (same API). Bundle a .so.1 that
# forwards to whichever the TV has, so LG's NEEDED entry resolves on both.
cp "$FW/compat/wayland-egl-forward/out/libwayland-egl.so.1" "$STAGE/lib/compat/"
# webOS 4 (opt-in, COMPAT_WEBOS4=1; untested on a TV): the firmware has no
# libsystemd.so.0 (LG's media library uses its sd_event loop) and ships curl
# only as libcurl.so.5. Bundle Debian's libsystemd and a libcurl.so.4 that
# forwards to the firmware's libcurl.so.5. Both sit in lib/compat, ahead of
# the firmware's libraries, so build webOS 5+ packages without the switch.
if [ "${COMPAT_WEBOS4:-0}" = 1 ]; then
  if grep -lq "libsystemd.so.0" "$EXE" "$STAGE"/lib/*.so 2>/dev/null; then
    [ -e "$GLIBC_LIB/libsystemd.so.0" ] || { echo "libsystemd0 not downloaded; re-run ./setup.sh" >&2; exit 1; }
    cp -L "$GLIBC_LIB/libsystemd.so.0" "$STAGE/lib/compat/"
  fi
  if grep -lq "libcurl.so.4" "$EXE" "$STAGE"/lib/*.so 2>/dev/null; then
    cp "$FW/compat/fallback/out/libcurl.so.4" "$STAGE/lib/compat/"
  fi
  # webOS 4's libwayland-webos-client lacks the wl_webos_foreign protocol, which
  # LG's media library references as data; provide the descriptions so the
  # app loads (the compositor does not offer the protocol: no video placement).
  if grep -lq "wl_webos_foreign_interface" "$EXE" "$STAGE"/lib/*.so 2>/dev/null; then
    cp "$FW/compat/webos4/out/libwebos-foreign-compat.so" "$STAGE/lib/compat/"
    "$PATCHELF" --add-needed libwebos-foreign-compat.so "$EXE"
  fi
  echo "COMPAT_WEBOS4=1: package for webOS 4 (bundled libsystemd, libcurl.so.4 -> libcurl.so.5, wl_webos_foreign)" >&2
fi
chmod 755 "$EXE" "$STAGE/lib/compat/"*

"$ARES_PACKAGE" "$STAGE" -o "$OUT" >/dev/null
IPK=$(ls "$OUT"/*.ipk)
echo "$IPK"
