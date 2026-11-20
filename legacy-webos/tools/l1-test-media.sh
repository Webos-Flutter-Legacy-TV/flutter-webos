#!/usr/bin/env bash
# Make clear DASH test streams for the L1 player's stream handling, for the
# codecs and features no public stream covers well: VP9 + Opus, AC-3 5.1,
# HEVC HDR10 with mastering metadata (mdcv/clli) and three H.264 qualities
# for adaptive switching. Written to downloads/media/l1/ (git-ignored) and
# served by tools/media-server.sh; drm_test's l1local_* cases play them with
# the test aid <app dir>/compat-l1-clear.
#
# Needs ffmpeg with libvpx-vp9, libopus, libx264 and libx265.
# Usage: tools/l1-test-media.sh [seconds]   (default 60; existing streams are kept)
set -euo pipefail
FW="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$FW/downloads/media/l1"
SECS="${1:-60}"
mkdir -p "$OUT"

# 30 fps, a key frame every 2 s, 2 s segments.
SRC=(-f lavfi -i "testsrc=size=1920x1080:rate=30:duration=$SECS"
     -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$SECS")
GOP=(-g 60 -keyint_min 60 -sc_threshold 0)
DASH=(-f dash -seg_duration 2 -use_template 1 -dash_segment_type mp4
      -init_seg_name 'init-$RepresentationID$.mp4'
      -media_seg_name 'seg-$RepresentationID$-$Number%05d$.m4s')

make() {  # <name> <ffmpeg output args...>
  local name="$1"; shift
  [ -s "$OUT/$name/manifest.mpd" ] && { echo "  $name (kept)"; return 0; }
  rm -rf "$OUT/$name"; mkdir -p "$OUT/$name"
  echo "  $name"
  ffmpeg -hide_banner -loglevel error -y "${SRC[@]}" "$@" "$OUT/$name/manifest.mpd"
}

echo "== L1 test streams in $OUT ($SECS s each)"

# VP9 profile 0 in MP4 (vp09 + vpcC) with Opus; SegmentTimeline.
make vp9_opus -map 0:v -map 1:a \
  -pix_fmt yuv420p -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -b:v 4M "${GOP[@]}" \
  -c:a libopus -b:a 128k -ac 2 \
  "${DASH[@]}" -use_timeline 1 -adaptation_sets "id=0,streams=v id=1,streams=a"

# H.264 with AC-3 5.1; $Number$ template with fixed duration.
make h264_ac3 -map 0:v -map 1:a \
  -pix_fmt yuv420p -c:v libx264 -preset veryfast -b:v 5M "${GOP[@]}" \
  -c:a ac3 -b:a 448k -ac 6 \
  "${DASH[@]}" -use_timeline 0 -adaptation_sets "id=0,streams=v id=1,streams=a"

# HEVC Main 10, BT.2020 PQ, with HDR10 mastering display and light level
# (mdcv/clli and colr added by mp4-hdr-boxes.py: ffmpeg's DASH muxer omits them).
make hevc_hdr10 -map 0:v -map 1:a \
  -vf format=yuv420p10le -c:v libx265 -preset fast -tag:v hvc1 -b:v 5M \
  -x265-params "log-level=error:keyint=60:min-keyint=60:scenecut=0:hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=1000,400" \
  -color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc \
  -c:a aac -b:a 128k -ac 2 \
  "${DASH[@]}" -use_timeline 0 -adaptation_sets "id=0,streams=v id=1,streams=a"

# Three H.264 qualities in one AdaptationSet (aligned key frames), each with
# its height drawn on the picture, for adaptive switching.
make abr_h264 -filter_complex \
  "[0:v]split=3[a][b][c];[a]drawtext=text='1080p':fontsize=96:fontcolor=white:box=1:boxcolor=black:x=40:y=40[v0];[b]scale=1280:720,drawtext=text='720p':fontsize=64:fontcolor=white:box=1:boxcolor=black:x=27:y=27[v1];[c]scale=854:480,drawtext=text='480p':fontsize=43:fontcolor=white:box=1:boxcolor=black:x=18:y=18[v2]" \
  -map '[v0]' -map '[v1]' -map '[v2]' -map 1:a \
  -pix_fmt yuv420p -c:v libx264 -preset veryfast "${GOP[@]}" \
  -b:v:0 6M -b:v:1 2500k -b:v:2 1M \
  -c:a aac -b:a 128k -ac 2 \
  "${DASH[@]}" -use_timeline 0 -adaptation_sets "id=0,streams=v id=1,streams=a"

python3 "$FW/tools/mp4-hdr-boxes.py" "$OUT/hevc_hdr10/init-0.mp4"

echo "== done; serve with tools/media-server.sh (paths l1/<name>/manifest.mpd)"
