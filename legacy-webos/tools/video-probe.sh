#!/usr/bin/env bash
# Probe one URL with the installed video_test app: tools/video-probe.sh <url>
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"; A=com.flutter.app.video-test
D=/media/developer/apps/usr/palm/applications/$A
ssh -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST" "
  luna-send -i luna://com.webos.applicationManager/closeByAppId '{\"id\":\"$A\"}' >/dev/null 2>&1 & C=\$!; sleep 2; kill \$C
  echo '$1' > $D/video_url.txt; touch $D/video_probe; rm -f $D/compat_results.log
  luna-send -i luna://com.webos.applicationManager/launch '{\"id\":\"$A\"}' >/dev/null 2>&1 & L=\$!; sleep 2; kill \$L
  i=0; while [ \$i -lt 40 ] && ! grep -q '\[V\] done' $D/compat_results.log 2>/dev/null; do sleep 1; i=\$((i+1)); done
  grep -E 'url|init|probe|error' $D/compat_results.log | sort -u"
