#!/usr/bin/env bash
# Install an IPK on the rooted TV over SSH and (optionally) launch it.
# Usage: tools/tv-install.sh <file.ipk> [--launch]
set -euo pipefail
IPK="$1"; LAUNCH="${2:-}"
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"
SSH=(ssh -o ConnectTimeout=8 -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST")
APP_ID=$(basename "$IPK" | sed -E 's/_[^_]+_[^_]+\.ipk$//')

scp -q -o ConnectTimeout=8 -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "$IPK" "root@$HOST:/tmp/install.ipk"
# Large packages take well over 30 s to unpack, and luna-send -w can return
# before the final status. Subscribe in the background and wait for it.
"${SSH[@]}" "luna-send -i 'luna://com.webos.appInstallService/dev/install' \
  '{\"id\":\"$APP_ID\",\"ipkUrl\":\"/tmp/install.ipk\",\"subscribe\":true}' > /tmp/install.out 2>&1 & P=\$!
for i in \$(seq 1 ${TV_INSTALL_TIMEOUT:-180}); do
  grep -qE '\"state\":\"(installed|failed)|errorText|\"returnValue\":false' /tmp/install.out && break; sleep 1
done
kill \$P 2>/dev/null
R=\$(grep -m1 -oE '\"state\":\"(installed|failed)\"|\"errorText\":\"[^\"]*\"' /tmp/install.out)
rm -f /tmp/install.ipk /tmp/install.out /tmp/$APP_ID.log
echo \"install: \${R:-timed out}\"; case \"\$R\" in *installed*) ;; *) exit 1;; esac"

if [ "$LAUNCH" = "--launch" ]; then
  # luna-send -n 1 gets no reply on some firmware; use -i and stop it.
  "${SSH[@]}" "luna-send -i luna://com.webos.applicationManager/launch '{\"id\":\"$APP_ID\"}' & P=\$!; sleep 4; kill \$P 2>/dev/null"
fi
