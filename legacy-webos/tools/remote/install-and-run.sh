#!/bin/sh
# Runs ON THE TV (piped over ssh). Expects APP_ID; installs /tmp/install.ipk,
# then starts the app's launcher directly (as root) and prints its log.
# Env: APP_ID (required), RUN_SECS (default 10), VIA_LUNA=1 to launch via appmanager.
set -u
RUN_SECS=${RUN_SECS:-10}
APPDIR=/media/developer/apps/usr/palm/applications/$APP_ID
LOG=/tmp/$APP_ID.log

# Close the running app first, or appinstalld fails with "Close package failed".
luna-send -i luna://com.webos.applicationManager/closeByAppId "{\"id\":\"$APP_ID\"}" >/dev/null 2>&1 &
CL=$!; sleep 2; kill $CL 2>/dev/null
pkill -9 -f "$APPDIR/" 2>/dev/null; sleep 1
luna-send -i "luna://com.webos.appInstallService/dev/install" \
  "{\"id\":\"$APP_ID\",\"ipkUrl\":\"/tmp/install.ipk\",\"subscribe\":true}" > /tmp/inst.out 2>&1 &
LS=$!
i=0
while [ $i -lt 60 ]; do
  grep -q '"state":"installed"\|"state":"failed"\|errorCode' /tmp/inst.out && break
  sleep 1; i=$((i+1))
done
kill $LS 2>/dev/null
echo "install: $(tail -1 /tmp/inst.out | grep -oE '"state":"[^"]*"|"errorCode":[^,]*|"reason":"[^"]*"' | tr '\n' ' ')"
rm -f /tmp/install.ipk "$LOG"

if [ "${VIA_LUNA:-0}" = 2 ]; then exit 0
fi
if [ "${VIA_LUNA:-0}" = 1 ]; then
  echo "launch: $(luna-send -n 1 -a com.webos.surfacemanager luna://com.webos.applicationManager/launch "{\"id\":\"$APP_ID\"}" 2>&1)"
else
  export XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0
  ( cd "$APPDIR" && sh "./$(sed -n 's/.*"main" *: *"\([^"]*\)".*/\1/p' appinfo.json)" & )
fi
sleep "$RUN_SECS"
echo "--- running:"; ps -ef | grep -v grep | grep "$APPDIR/" || echo "(not running)"
echo "--- log:"; tail -60 "$LOG" 2>/dev/null || echo "(no log)"
