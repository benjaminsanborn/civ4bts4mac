#!/bin/sh
# Transcode the game's QuickTime movies (Sorenson Video 3, which modern macOS
# can't decode) to H.264/AAC MP4 under build/movies/, mirroring their paths.
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
    if [ ! -s "$dst" ]; then
        ffmpeg -nostdin -loglevel error -y -i "$f" -c:v h264_videotoolbox -b:v 8M -pix_fmt yuv420p \
            -c:a aac -b:a 192k -movflags +faststart "$dst.tmp.mp4" && mv "$dst.tmp.mp4" "$dst" || echo "failed: $f"
    fi
    echo "@@progress {\"done\": $N, \"total\": $TOTAL, \"msg\": \"$(basename "$f")\"}"
done
echo "movies in $OUT"
