#!/usr/bin/env bash
# One iteration: rebuild shims -> repackage compat IPK -> install on TV ->
# run the app inside its devmode jail (as root, via chroot) -> print compat.log.
# Usage: tools/dev-cycle.sh <flutter-app-dir> [run-seconds] [--luna]
#   --luna  launch through the application manager (real jailer path) instead.
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$1"; SECS="${2:-12}"; HOW="${3:-}"
HOST="${TV_HOST:?set TV_HOST to the TV IP address}"
SSH=(ssh -o ConnectTimeout=8 -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@$HOST")

IPK=$("$FW/tools/make-compat-ipk.sh" "$APP_DIR" release)
APP_ID=$(basename "$IPK" | sed -E 's/_[^_]+_[^_]+\.ipk$//')
scp -q -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "$IPK" "root@$HOST:/tmp/install.ipk"
"${SSH[@]}" "APP_ID=$APP_ID RUN_SECS=0 VIA_LUNA=2 sh -s" < "$FW/tools/remote/install-and-run.sh" | grep '^install'

if [ "$HOW" = "--luna" ]; then
  "${SSH[@]}" "A=/media/developer/apps/usr/palm/applications/$APP_ID; rm -f \$A/compat.log \$A/compat_results.log
    luna-send -i luna://com.webos.applicationManager/launch '{\"id\":\"$APP_ID\"}' >/tmp/l.out 2>&1 & P=\$!
    sleep $SECS; kill \$P 2>/dev/null
    echo \"--- state: \$(ps -ef | grep -v grep | grep -q \"\$A/\" && echo RUNNING || echo NOT RUNNING)\"
    echo '--- compat.log'; tail -60 \$A/compat.log 2>&1"
else
  "${SSH[@]}" "J=/var/palm/jail/$APP_ID; A=/media/developer/apps/usr/palm/applications/$APP_ID
    rm -f \$A/compat.log \$A/compat_results.log
    XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 chroot \$J \$A/\$(sed -n 's/.*\"main\" *: *\"\\([^\"]*\\)\".*/\\1/p' \$A/appinfo.json) >/tmp/jailrun.out 2>&1 & P=\$!
    sleep $SECS
    echo \"--- state: \$(kill -0 \$P 2>/dev/null && echo RUNNING || echo EXITED)\"
    kill \$P 2>/dev/null
    echo '--- compat.log'; tail -60 \$A/compat.log 2>&1"
fi
