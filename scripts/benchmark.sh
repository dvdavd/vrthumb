#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 VIDEO [COUNT]" >&2
  exit 2
fi

video=$1
count=${2:-100}
project_dir=$(cd "$(dirname "$0")/.." && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

duration=$(ffprobe -v error -show_entries format=duration -of default=nk=1:nw=1 "$video")

run_timed() {
  if [[ -x /usr/bin/time ]]; then
    /usr/bin/time -f 'wall=%e sec peak_rss=%M KiB' "$@"
  else
    TIMEFORMAT='wall=%R sec (peak RSS unavailable: install GNU time)'
    time "$@"
  fi
}

echo "Seek-per-frame FFmpeg (optimized)"
seek_dir="$work_dir/seek"
mkdir -p "$seek_dir"
run_timed bash -c '
  set -euo pipefail
  video=$1; count=$2; duration=$3; seek_dir=$4
  for ((i = 0; i < count; i++)); do
    ts=$(awk -v i="$i" -v n="$count" -v d="$duration" \
              "BEGIN { printf \"%.6f\", d * (i + 1) / (n + 1) }")
    ffmpeg -hide_banner -loglevel error -ss "$ts" -i "$video" \
      -vf "scale=320:180:force_original_aspect_ratio=decrease" \
      -frames:v 1 "$seek_dir/$(printf "seek-%04d.jpg" "$((i + 1))")"
  done
' _ "$video" "$count" "$duration" "$seek_dir"

echo "vrthumb software"
run_timed \
  "$project_dir/build/vrthumb" "$video" --output "$work_dir/software" \
  --count "$count" --size 320x180 --decoder software --json

echo "vrthumb automatic"
run_timed \
  "$project_dir/build/vrthumb" "$video" --output "$work_dir/automatic" \
  --count "$count" --size 320x180 --decoder auto --json
