#!/usr/bin/env bash
# Build the compat shims with the webosbrew toolchain (glibc 2.12 baseline, so
# the shims themselves load on every webOS release).
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
CC="$FW/toolchains/webosbrew/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc"
[ -x "$CC" ] || { echo "webosbrew toolchain missing; run ./setup.sh" >&2; exit 1; }
mkdir -p "$FW/compat/wayland-shim/out" "$FW/compat/ndl-sdl-shim/out"
"$CC" -O2 -fPIC -shared -Wall -Wextra \
  -o "$FW/compat/wayland-shim/out/libwl-marshal-flags-shim.so" \
  "$FW/compat/wayland-shim/wl_marshal_flags_shim.c" -lwayland-client -ldl
"$CC" -O2 -fPIC -shared -Wall -Wextra -Wl,-soname,libSDL2-2.0.so.0 \
  -o "$FW/compat/ndl-sdl-shim/out/libSDL2-2.0.so.0" \
  "$FW/compat/ndl-sdl-shim/ndl_sdl_compat.c" -ldl
mkdir -p "$FW/compat/wayland-egl-forward/out"
"$CC" -O2 -fPIC -shared -Wall -Wextra -fvisibility=hidden -Wl,-soname,libwayland-egl.so.1 \
  -o "$FW/compat/wayland-egl-forward/out/libwayland-egl.so.1" \
  "$FW/compat/wayland-egl-forward/wayland_egl_forward.c" -ldl
"$CC" -O2 -Wall -o "$FW/tools/tv-key/tv-key" "$FW/tools/tv-key/tv-key.c"

# Widevine CDM proxy (C++, needs the NDK's g++ for the CDM's libstdc++ ABI).
NDK_ENV="$FW/toolchains/lg-ndk-11.2.0/environment-setup-ca9v1-webosmllib32-linux-gnueabi"
if [ -e "$NDK_ENV" ]; then
  mkdir -p "$FW/compat/widevine/out"
  ( . "$NDK_ENV" >/dev/null 2>&1
    $CXX -std=c++17 -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden \
      -Wl,-soname,libwv-compat.so -o "$FW/compat/widevine/out/libwv-compat.so" \
      "$FW/compat/widevine/wv_proxy.cpp" "$FW/compat/widevine/cenc_mp4.cpp" \
      "$FW/compat/widevine/wv_cdm.cpp" -ldl -lpthread
    $CXX -std=c++17 -O2 -Wall -Wextra -o "$FW/compat/widevine/out/wv-selftest" \
      "$FW/compat/widevine/wv_selftest.cpp" "$FW/compat/widevine/wv_cdm.cpp" -ldl -lpthread )
fi
