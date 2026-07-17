#!/bin/zsh
# Convert a normal source video into the two files used by the QR museum lamp.
# Usage: ./tools/convert-video.sh input.mp4 output-base

set -euo pipefail

if (( $# != 2 )); then
  print -u2 "Gebruik: $0 bronvideo uitvoer-zonder-extensie"
  exit 64
fi

source_video="$1"
output_base="$2"

if [[ ! -f "$source_video" ]]; then
  print -u2 "Bestand niet gevonden: $source_video"
  exit 66
fi

# ESP32-S3 software JPEG decoding is most reliable at a modest frame rate.
# The video is raw concatenated JPEG frames; it is deliberately not an AVI/MOV.
ffmpeg -y -i "$source_video" \
  -an \
  -vf "scale=480:272:force_original_aspect_ratio=decrease,pad=480:272:(ow-iw)/2:(oh-ih):black,format=yuvj420p" \
  -r 10 -c:v mjpeg -q:v 7 -f mjpeg "${output_base}.mjpeg"

# A separate WAV track is created for later synchronized video-audio support.
# The '?' makes videos without audio succeed as well.
ffmpeg -y -i "$source_video" \
  -map '0:a:0?' -ac 1 -ar 22050 -c:a pcm_s16le "${output_base}.wav"

print "Gemaakt: ${output_base}.mjpeg"
print "Gemaakt: ${output_base}.wav"
