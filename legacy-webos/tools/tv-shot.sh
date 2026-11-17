#!/usr/bin/env bash
# Take a screenshot of the TV and copy it to the host.
# Usage: tools/tv-shot.sh <out.png> [width height]
set -euo pipefail
OUT="$1"; W="${2:-1920}"; H="${3:-1080}"
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"
KEY=(-o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}")
ssh "${KEY[@]}" "root@$HOST" "rm -f /tmp/cap.png
  luna-send -i luna://com.webos.service.capture/executeOneShot \
    '{\"path\":\"/tmp/cap.png\",\"method\":\"DISPLAY\",\"format\":\"PNG\",\"width\":$W,\"height\":$H}' >/tmp/cap.out 2>&1 & P=\$!
  i=0; while [ \$i -lt 10 ] && [ ! -s /tmp/cap.png ]; do sleep 1; i=\$((i+1)); done; sleep 1; kill \$P 2>/dev/null; true"
scp -q "${KEY[@]}" "root@$HOST:/tmp/cap.png" "$OUT"
echo "$OUT"
