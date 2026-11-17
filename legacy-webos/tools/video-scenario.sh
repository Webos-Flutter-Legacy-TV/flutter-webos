#!/usr/bin/env bash
# Drive tests/video_test on the TV: install + launch, follow its STEP markers,
# take screenshots at the visual steps, press Home and relaunch, print results.
set -uo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"
SSH=(ssh -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST")
APP=com.flutter.app.video-test
D=/media/developer/apps/usr/palm/applications/$APP
OUT="$FW/out/video"
mkdir -p "$OUT"

shot() {  # screenshot including the video plane
  "${SSH[@]}" "rm -f /tmp/vshot.png; luna-send -i luna://com.webos.service.capture/executeOneShot \
    '{\"path\":\"/tmp/vshot.png\",\"method\":\"SCREEN_WITH_SOURCE_VIDEO\",\"format\":\"PNG\",\"width\":1280,\"height\":720}' >/dev/null 2>&1 & P=\$!
    i=0; while [ \$i -lt 8 ] && [ ! -s /tmp/vshot.png ]; do sleep 1; i=\$((i+1)); done; sleep 1; kill \$P 2>/dev/null; true"
  scp -q -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST:/tmp/vshot.png" "$OUT/$1.png" && echo "  screenshot $OUT/$1.png"
}
wait_step() {  # wait until the app logs STEP <name>
  for _ in $(seq 1 120); do
    "${SSH[@]}" "grep -q '\[V\] STEP $1\$' $D/compat_results.log 2>/dev/null" && return 0
    "${SSH[@]}" "grep -q '\[V\] done' $D/compat_results.log 2>/dev/null" && return 1
    sleep 1
  done
  return 1
}
launch() {
  "${SSH[@]}" "luna-send -i luna://com.webos.applicationManager/launch '{\"id\":\"$1\"}' >/dev/null 2>&1 & P=\$!; sleep 2; kill \$P"
}

(cd "$FW/tests/video_test" && . "$FW/env.sh" && flutter-webos build webos --release >/dev/null 2>&1) || { echo build failed; exit 1; }
"$FW/tools/dev-cycle.sh" "$FW/tests/video_test" 1 --luna | grep '^install'

if [ "${PASS2:-0}" = 1 ]; then
  "${SSH[@]}" "rm -f $D/video_probe $D/video_url.txt; touch $D/video_pass2; rm -f $D/video_skip_loop; rm -f $D/compat_results.log"
  launch "$APP"
  wait_step two_players_visible && { sleep 3; shot two_players; }
else
  "${SSH[@]}" "rm -f $D/video_pass2"
  wait_step small && { sleep 3; shot small; }
  wait_step full && { sleep 3; shot full; }
  wait_step two_players && { sleep 8; shot two_players; }
fi
if wait_step home; then
  sleep 3
  echo "  going Home (launching com.webos.app.home)"
  launch com.webos.app.home
  sleep 6; shot after_home
  "${SSH[@]}" "luna-send -i -f luna://com.webos.applicationManager/getForegroundAppInfo '{}' >/tmp/fg.out 2>&1 & P=\$!; sleep 2; kill \$P; grep appId /tmp/fg.out"
  sleep 6
  echo "  relaunching $APP"
  launch "$APP"
  sleep 5; shot after_relaunch
fi
for _ in $(seq 1 60); do
  "${SSH[@]}" "grep -q '\[V\] done' $D/compat_results.log" && break; sleep 2
done
echo "=== results"
"${SSH[@]}" "grep -vE 'tick position|^\[V\] STEP' $D/compat_results.log"
echo "=== lifecycle / ticks around Home"
"${SSH[@]}" "grep -E 'lifecycle|tick|timeline' $D/compat_results.log"
echo "=== ndl-compat log"
"${SSH[@]}" "grep -E 'ndl-compat' $D/compat.log | sort | uniq -c | sort -rn | head -20"
