#!/usr/bin/env bash
# Build the compat shims with the webosbrew toolchain (glibc 2.12 baseline, so
# the shims themselves load on every webOS release).
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
CC="$FW/toolchains/webosbrew/arm-webos-linux-gnueabi_sdk-buildroot/bin/arm-webos-linux-gnueabi-gcc"
WB_SYSROOT="$FW/toolchains/webosbrew/arm-webos-linux-gnueabi_sdk-buildroot/arm-webos-linux-gnueabi/sysroot"
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
# libcurl.so.4 -> firmware libcurl.so.5 forwarder (webOS 4); see compat/fallback/curl_forward.c.
# The toolchain has no libcurl, so link against an empty stand-in with SONAME
# libcurl.so.5 to record the DT_NEEDED entry; the stand-in is not shipped.
mkdir -p "$FW/compat/fallback/out/link"
"$CC" -shared -nostdlib -Wl,-soname,libcurl.so.5 -o "$FW/compat/fallback/out/link/libcurl.so.5" -x c /dev/null
"$CC" -O2 -fPIC -shared -Wall -nostdlib -Wl,-soname,libcurl.so.4 -Wl,--no-as-needed \
  -o "$FW/compat/fallback/out/libcurl.so.4" "$FW/compat/fallback/curl_forward.c" \
  "$FW/compat/fallback/out/link/libcurl.so.5"
# webOS 4: wl_webos_foreign protocol descriptions its Wayland library lacks.
mkdir -p "$FW/compat/webos4/out"
"$CC" -O2 -fPIC -shared -Wall -Wextra -fvisibility=hidden -Wl,-soname,libwebos-foreign-compat.so \
  -o "$FW/compat/webos4/out/libwebos-foreign-compat.so" \
  "$FW/compat/webos4/wl_webos_foreign_protocol.c" -lwayland-client
# webOS 3: libudev.so.1 stand-in (forwards to libudev.so.0 or the firmware's .so.1).
mkdir -p "$FW/compat/webos3/out"
"$CC" -O2 -fPIC -shared -Wall -Wextra -fvisibility=hidden -Wl,-soname,libudev.so.1 \
  -o "$FW/compat/webos3/out/libudev.so.1" "$FW/compat/webos3/udev_compat.c" -ldl -lpthread
"$CC" -O2 -Wall -o "$FW/tools/tv-key/tv-key" "$FW/tools/tv-key/tv-key.c"

# C++ parts, with the NDK's g++ (the CDM's and LG's libstdc++ ABI).
NDK_ENV="$FW/toolchains/lg-ndk-11.2.0/environment-setup-ca9v1-webosmllib32-linux-gnueabi"
if [ -e "$NDK_ENV" ]; then
  # Opt-in hardware Widevine (secure video path): see compat/widevine-l1/NOTICE.
  L1="$FW/compat/widevine-l1"
  mkdir -p "$L1/out"
  # shellcheck source=/dev/null
  ( . "$NDK_ENV" >/dev/null 2>&1
    $CXX -std=c++17 -O2 -Wall -Wextra -Wno-psabi -fPIC -shared -fvisibility=hidden \
      -Wl,-soname,libwv-l1.so -o "$L1/out/libwv-l1.so" \
      "$L1/wv_l1_player.cpp" "$L1/l1_cdm.cpp" "$L1/fmp4.cpp" "$L1/dash.cpp" "$L1/http.cpp" -ldl -lpthread )

  # webOS 3 (COMPAT_WEBOS3=1): see compat/webos3/NOTICE. C++ with the NDK's g++
  # so std::string and LS::Call match LG's binaries.
  W3="$FW/compat/webos3"
  LS2="$W3/luna-service2pp"
  mkdir -p "$W3/out/obj" "$W3/out/link"
  # shellcheck source=/dev/null
  ( . "$NDK_ENV" >/dev/null 2>&1
    # libwebos3-compat.so: functions webOS 3's firmware lacks. wayland-util.h
    # (plain structs) comes from the webosbrew sysroot; the NDK has none.
    $CC -O2 -fPIC -Wall -Wextra -idirafter "$WB_SYSROOT/usr/include" \
      -c -o "$W3/out/obj/xinput.o" "$W3/wl_webos_xinput_protocol.c"
    read -r -a GLIB_CFLAGS <<<"$(pkg-config --cflags glib-2.0)"
    read -r -a GLIB_LIBS <<<"$(pkg-config --libs glib-2.0)"
    $CXX -std=c++17 -O2 -fPIC -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden \
      "${GLIB_CFLAGS[@]}" -c -o "$W3/out/obj/webos3_compat.o" "$W3/webos3_compat.cpp"
    $CXX -shared -Wl,-soname,libwebos3-compat.so -o "$W3/out/libwebos3-compat.so" \
      "$W3/out/obj/webos3_compat.o" "$W3/out/obj/xinput.o" -ldl "${GLIB_LIBS[@]}"
    # libluna-service2++.so.3 from webOS OSE sources, linked against the
    # firmware's C libluna-service2 (empty stand-in with its SONAME, not shipped).
    $CC -shared -nostdlib -Wl,-soname,libluna-service2.so.3 \
      -o "$W3/out/link/libluna-service2.so.3" -x c /dev/null
    LS2_FLAGS=(-std=c++14 -fPIC -Wall -DUSE_PMLOG_DECLARATION -I"$LS2/include/private"
               -I"$LS2/include/public" -I"$LS2/include/public/luna-service2++")
    for src in call subscription condition_variable; do
      $CXX "${LS2_FLAGS[@]}" "${GLIB_CFLAGS[@]}" -O2 -c -o "$W3/out/obj/$src.o" "$LS2/src/$src.cpp"
    done
    $CXX "${LS2_FLAGS[@]}" "${GLIB_CFLAGS[@]}" -O0 -c -o "$W3/out/obj/call_export.o" "$LS2/src/call_export.cpp"
    $CXX -shared -Wl,-soname,libluna-service2++.so.3 -Wl,--no-as-needed \
      -o "$W3/out/libluna-service2++.so.3" \
      "$W3/out/obj/call.o" "$W3/out/obj/subscription.o" "$W3/out/obj/condition_variable.o" \
      "$W3/out/obj/call_export.o" "$W3/out/link/libluna-service2.so.3" -lPmLogLib "${GLIB_LIBS[@]}" )
fi
