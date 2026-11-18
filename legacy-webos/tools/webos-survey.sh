#!/bin/sh
# webOS device survey: read-only. Records what a Flutter runtime would need.
# Runs under busybox sh on the TV. Usage (from the host):
#   ssh -i "$TV_SSH_KEY" root@"$TV_HOST" 'sh -s' < tools/webos-survey.sh > survey-<name>.txt

sec() { echo; echo "=== $1"; }
try() { "$@" 2>/dev/null; }

sec "webOS / firmware"
try cat /var/run/nyx/os_info.json
try cat /var/run/nyx/device_info.json
try cat /etc/starfish-release
try cat /etc/os-release
try luna-send -n 1 -f luna://com.webos.service.tv.systemproperty/getSystemInfo \
  '{"keys":["modelName","firmwareVersion","sdkVersion","boardType","UHD"]}'

sec "kernel / cpu / memory"
uname -a
try grep -m1 -E "model name|Processor" /proc/cpuinfo
try grep -m1 Features /proc/cpuinfo
try grep -m1 "CPU architecture" /proc/cpuinfo
grep -c ^processor /proc/cpuinfo
try head -3 /proc/meminfo

sec "userspace bitness / loader"
try ls -l /lib/ld-linux*.so* /lib/ld-linux-armhf.so* /lib64/ld-linux* /lib/ld-*.so*
try file /bin/busybox /usr/bin/luna-send

sec "glibc"
for l in /lib/libc.so.6 /lib32/libc.so.6 /usr/lib/libc.so.6; do
  [ -e "$l" ] || continue
  echo "$l"; "$l" 2>/dev/null | head -1
  strings "$l" 2>/dev/null | grep -oE "^GLIBC_[0-9.]+$" | sort -t. -k2,2n -k3,3n | tail -1
done
try ldd --version | head -1

sec "libstdc++ / libgcc"
for l in /usr/lib/libstdc++.so.6* /lib/libstdc++.so.6*; do
  [ -e "$l" ] || continue
  echo "$l"
  strings "$l" 2>/dev/null | grep -oE "^GLIBCXX_[0-9.]+$" | sort -t. -k3,3n | tail -1
  strings "$l" 2>/dev/null | grep -oE "^CXXABI_[0-9.]+$" | sort -t. -k3,3n | tail -1
done

sec "graphics: EGL / GLES / Wayland / DRM"
for p in libEGL libGLESv2 libGLESv1_CM libwayland-client libwayland-egl libwayland-cursor \
         libwayland-webos-client libxkbcommon libmali libgbm libdrm libvulkan; do
  ls -l /usr/lib/$p.so* /lib/$p.so* 2>/dev/null
done
try ls /dev/dri /dev/mali* /dev/galcore
try ls -d /tmp/xdg* /var/run/xdg* /run/xdg* ; try ls /tmp/xdg /run/user/*/ 2>/dev/null
echo "WAYLAND_DISPLAY=$WAYLAND_DISPLAY XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"
try strings /usr/lib/libGLESv2.so* | grep -m3 -E "OpenGL ES [0-9]|Mali|PowerVR"

sec "webOS system libraries used by the LG Flutter embedder"
for p in libluna-service2 libluna-service2++ libPmLogLib libwebosi18n libglib-2.0 \
         libsystemd libfontconfig libpbnjson_cpp libpbnjson_c; do
  f=$(ls /usr/lib/$p.so* /lib/$p.so* 2>/dev/null)
  if [ -n "$f" ]; then ls -l $f; else echo "MISSING $p"; fi
done

sec "native app runtime"
try ls /media/developer/apps/usr/palm/applications | head
try ls -ld /media/developer /media/developer/apps
try df -h /media/developer /tmp
try cat /proc/self/status | grep -E "^(Uid|Gid|CapEff)"
echo; echo "=== done"
