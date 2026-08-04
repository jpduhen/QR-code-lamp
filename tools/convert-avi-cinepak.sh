#!/usr/bin/env zsh
# Convert a normal source video to the experimental AVI/Cinepak format.
# Usage: VIDEO_FPS=10 ./tools/convert-avi-cinepak.sh input.mp4 output-base

set -euo pipefail

if [[ $# -ne 2 ]]; then
  print -u2 "Gebruik: $0 bronvideo uitvoer-zonder-extensie"
  exit 2
fi

source_video="$1"
output_base="${2%.*}"
video_fps="${VIDEO_FPS:-10}"

if [[ ! -f "$source_video" ]]; then
  print -u2 "Bestand niet gevonden: $source_video"
  exit 1
fi

mkdir -p "$(dirname "$output_base")"

ffmpeg -y -i "$source_video" \
  -map 0:v:0 \
  -vf "fps=${video_fps},scale=480:272:force_original_aspect_ratio=decrease,pad=480:272:(ow-iw)/2:(oh-ih)/2:color=black,format=yuv420p" \
  -c:v cinepak -q:v 7 -an \
  "${output_base}.avi"

ffmpeg -y -i "$source_video" \
  -map "0:a:0?" -ac 1 -ar 44100 -c:a libmp3lame -b:a 96k \
  "${output_base}.mp3"

print "Gemaakt: ${output_base}.avi"
print "Gemaakt: ${output_base}.mp3"
