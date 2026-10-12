#!/bin/sh
# Transcode the game's Sorenson Video 3 movies (which modern macOS can't decode;
# the rest play as they are) to H.264/AAC MP4 under build/movies/, mirroring
# their paths. (The app does the same itself: tools/setup/Movies.swift.)
# The runtime's QuickTime emulation plays these through AVFoundation.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GAME="$ROOT/build/game"
OUT="$ROOT/build/movies"
cd "$GAME" || exit 1
TOTAL=$(find . -iname "*.mov" | wc -l | tr -d ' ')
N=0
find . -iname "*.mov" | while read -r f; do
    N=$((N + 1))
    dst="$OUT/${f#./}.mp4"
    mkdir -p "$(dirname "$dst")"
    # Only Sorenson Video needs it; the rest play as they are.
    ffprobe -v error -select_streams v -show_entries stream=codec_name -of csv=p=0 "$f" | grep -q '^svq' || continue
    if [ ! -s "$dst" ]; then
        ffmpeg -nostdin -loglevel error -y -i "$f" -c:v h264_videotoolbox -b:v 5M -pix_fmt yuv420p \
            -c:a aac -b:a 192k -movflags +faststart "$dst.tmp.mp4" && mv "$dst.tmp.mp4" "$dst" || echo "failed: $f"
    fi
    echo "@@progress {\"done\": $N, \"total\": $TOTAL, \"msg\": \"$(basename "$f")\"}"
done
echo "movies in $OUT"
