#!/usr/bin/env bash
# Install an IPK on the rooted TV over SSH and (optionally) launch it.
# Usage: tools/tv-install.sh <file.ipk> [--launch]
set -euo pipefail
IPK="$1"; LAUNCH="${2:-}"
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"
SSH=(ssh -o ConnectTimeout=8 -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST")
APP_ID=$(basename "$IPK" | sed -E 's/_[^_]+_[^_]+\.ipk$//')

scp -q -o ConnectTimeout=8 -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "$IPK" "root@$HOST:/tmp/install.ipk"
"${SSH[@]}" "luna-send -w 30000 -i 'luna://com.webos.appInstallService/dev/install' \
  '{\"id\":\"$APP_ID\",\"ipkUrl\":\"/tmp/install.ipk\",\"subscribe\":true}' | grep -m1 -E 'installed|failed|error'"
"${SSH[@]}" "rm -f /tmp/install.ipk /tmp/$APP_ID.log"

if [ "$LAUNCH" = "--launch" ]; then
  "${SSH[@]}" "luna-send -n 1 luna://com.webos.applicationManager/launch '{\"id\":\"$APP_ID\"}'"
fi
