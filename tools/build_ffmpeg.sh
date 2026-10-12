#!/bin/sh
# A minimal ffmpeg for the app, to convert the game's Sorenson Video 3 movies
# (which modern macOS can't decode) to H.264 MP4: the QuickTime demuxer, the
# decoders the movies use, Apple's hardware H.264 encoder and AAC. LGPL build
# (no GPL parts), shipped as a separate program.
#   tools/build_ffmpeg.sh  ->  build/ffmpeg/ffmpeg
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=7.1
SHA256=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
SRC="$ROOT/third_party/ffmpeg-$VERSION"
OUT="$ROOT/build/ffmpeg"
[ -x "$OUT/ffmpeg" ] && [ "$OUT/ffmpeg" -nt "$0" ] && { echo "build/ffmpeg/ffmpeg is up to date"; exit 0; }
if [ ! -d "$SRC" ]; then
    TMP=$(mktemp -d)
    curl -sSfL -o "$TMP/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    echo "$SHA256  $TMP/ffmpeg.tar.xz" | shasum -a 256 -c - >/dev/null
    tar -xJf "$TMP/ffmpeg.tar.xz" -C "$ROOT/third_party"
    rm -rf "$TMP"
fi
mkdir -p "$OUT/obj"
cd "$OUT/obj"
"$SRC/configure" --prefix="$OUT/install" --cc=clang --arch=arm64 \
    --extra-cflags=-mmacosx-version-min=13.0 --extra-ldflags=-mmacosx-version-min=13.0 \
    --disable-everything --disable-autodetect --disable-doc --disable-debug --disable-network \
    --disable-ffplay --disable-ffprobe --enable-static --disable-shared \
    --enable-videotoolbox --enable-swscale --enable-swresample \
    --enable-protocol=file --enable-demuxer=mov \
    --enable-decoder=svq3,svq1,pcm_s16le,pcm_s16be,adpcm_ima_qt,alac,aac,qdm2,mp3 \
    --enable-encoder=h264_videotoolbox,aac --enable-muxer=mp4 \
    --enable-filter=scale,format,aformat,aresample,null,anull >/dev/null
make -j"$(sysctl -n hw.ncpu)" ffmpeg >/dev/null
cp ffmpeg "$OUT/ffmpeg"
strip "$OUT/ffmpeg"
echo "built $OUT/ffmpeg ($(du -h "$OUT/ffmpeg" | cut -f1))"
