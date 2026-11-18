#!/usr/bin/env bash
# Download the test apps' media once into downloads/media/ (git-ignored) and
# serve it on the local network, so video/audio tests do not depend on
# third-party hosts. Build test apps against it with:
#   flutter-webos build webos --release --dart-define=MEDIA_BASE=http://<this host>:8099
# (video-scenario.sh does this when MEDIA_BASE is set). DRM tests stay online:
# their licence servers are external anyway.
#
# Usage: tools/media-server.sh [--fetch-only] [port]
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$FW/downloads/media"
FETCH_ONLY=0
[ "${1:-}" = "--fetch-only" ] && { FETCH_ONLY=1; shift; }
PORT="${1:-8099}"
mkdir -p "$DIR"

# <file> <url>... (first URL that works wins). Add your own MP4 under the same
# name if a host is down.
fetch() {
  local name="$1"; shift
  [ -s "$DIR/$name" ] && return 0
  for url in "$@"; do
    if curl -fsSL --retry 2 "$url" -o "$DIR/$name.part"; then
      mv "$DIR/$name.part" "$DIR/$name"; echo "  $name"; return 0
    fi
  done
  rm -f "$DIR/$name.part"; echo "  $name: download failed; place a file there by hand" >&2
}
echo "== media in $DIR"
fetch sintel_trailer-720p.mp4 https://download.blender.org/durian/trailer/sintel_trailer-720p.mp4
fetch butterfly.mp4 https://flutter.github.io/assets-for-api-docs/assets/videos/butterfly.mp4
fetch sample-15s.mp3 https://download.samplelib.com/mp3/sample-15s.mp3
fetch sample-15s.wav https://download.samplelib.com/wav/sample-15s.wav
fetch owl.jpg https://flutter.github.io/assets-for-api-docs/assets/widgets/owl.jpg
[ "$FETCH_ONLY" = 1 ] && exit 0

# The address the TV reaches us on: the source address of our route to it.
IP=$(ip route get "${TV_HOST:-1.1.1.1}" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p')
echo "== MEDIA_BASE=http://${IP:-<this host>}:$PORT"
exec python3 "$FW/tools/media-server.py" "$DIR" --port "$PORT"
