#!/usr/bin/env bash
# Build + run tests/compat_test with a given render configuration and print
# its [T] results. Usage: tools/perf-variant.sh <label> [resolution] [impeller] [swap0] [switches]
#   resolution: e.g. 1920x1080 (empty = embedder default)
#   impeller:   true|false (default false)
#   switches:   space-separated webos/meta switch files to add for this run,
#               e.g. "compat-egl-damage compat-dual-surface"
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="$1"; RES="${2:-}"; IMP="${3:-false}"; SWAP0="${4:-false}"; SWITCHES="${5:-}"
# The legacy packager turns on two surfaces by default; measure one unless asked.
case " $SWITCHES " in *" compat-dual-surface "*) ;; *) SWITCHES="$SWITCHES compat-single-surface" ;; esac
APP="$FW/tests/compat_test"
. "$FW/env.sh"

python3 - "$APP/webos/meta/appinfo.json" "$RES" <<'EOF'
import json, sys
p, res = sys.argv[1], sys.argv[2]
d = json.load(open(p))
if res: d["resolution"] = res
else: d.pop("resolution", None)
json.dump(d, open(p, "w"), indent=4)
EOF
python3 - "$APP/webos/meta/flutter-conf.json" "$IMP" <<'EOF'
import json, sys
p, imp = sys.argv[1], sys.argv[2] == "true"
d = json.load(open(p))
d.pop("enable-impeller", None)
sw = d.setdefault("engine-switches", {})
sw["enable-impeller"] = imp
json.dump(d, open(p, "w"), indent=2)
EOF

(cd "$APP" && flutter-webos build webos --release >/dev/null 2>&1)
: > "$APP/webos/meta/compat-perf"
for sw in $SWITCHES; do : > "$APP/webos/meta/$sw"; done
cleanup() {
  rm -f "$APP/webos/meta/compat-perf" "$APP/webos/meta/compat-swap0"
  for sw in $SWITCHES; do rm -f "$APP/webos/meta/$sw"; done
}
trap cleanup EXIT
if [ "$SWAP0" = true ]; then : > "$APP/webos/meta/compat-swap0"; else rm -f "$APP/webos/meta/compat-swap0"; fi
(cd "$APP" && flutter-webos build webos --release >/dev/null 2>&1)
"$FW/tools/dev-cycle.sh" "$APP" 2 --luna >/dev/null 2>&1 || true
SSH=(ssh -o LogLevel=ERROR -i "${TV_SSH_KEY:-$HOME/.ssh/lg_webos}" "root@${TV_HOST:?set TV_HOST to the TV IP address}")
D=/media/developer/apps/usr/palm/applications/com.flutter.app.compat-test
for _ in $(seq 1 30); do
  sleep 5
  "${SSH[@]}" "grep -q 'run INFO done' $D/compat_results.log 2>/dev/null" && break
done
echo "=== $LABEL (resolution=${RES:-default}, impeller=$IMP, swap0=$SWAP0, switches=${SWITCHES:-none})"
"${SSH[@]}" "grep -E 'perf|FAIL|rss|done' $D/compat_results.log; grep -iE 'impeller|skia|size|resolution|SwapInterval|first frame|EGL extensions|compat-egl-damage|damage \\(' $D/compat.log | grep -v egl_unified | cut -c1-400 | head -14"
