#!/bin/bash
# Builds the part of FFmpeg the Bink videos need (platform/src/bink.cpp: the
# file protocol, the bink demuxer, the bink and binkaudio decoders, swscale,
# swresample) as static libraries for iOS, which has no FFmpeg of its own.
#
#   tools/build_ffmpeg_ios.sh [iphoneos|iphonesimulator] [PREFIX]
#
# The source is FFmpeg 9.0.2 (Homebrew's version on the Mac), downloaded once
# into $FFMPEG_IOS_WORK (default /tmp/ffmpeg-ios) and checked against its
# SHA-256. The libraries and headers go to PREFIX, by default
# ~/Library/Developer/scimitar/ffmpeg-<sdk>, where games/ac2/ios/ios.cmake looks
# (SCIMITAR_FFMPEG_IOS). Nothing is written into the repository.
set -euo pipefail

SDK=${1:-iphoneos}
PREFIX=${2:-$HOME/Library/Developer/scimitar/ffmpeg-$SDK}
VERSION=9.0.2
SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
WORK=${FFMPEG_IOS_WORK:-/tmp/ffmpeg-ios}
MIN_IOS=17.0

case "$SDK" in
    iphoneos) TARGET="-target arm64-apple-ios$MIN_IOS" ;;
    iphonesimulator) TARGET="-target arm64-apple-ios$MIN_IOS-simulator" ;;
    *) echo "usage: $0 [iphoneos|iphonesimulator] [PREFIX]" >&2; exit 2 ;;
esac

mkdir -p "$WORK"
TARBALL="$WORK/ffmpeg-$VERSION.tar.xz"
if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL.part" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | shasum -a 256 -c -

SRC="$WORK/ffmpeg-$VERSION"
[ -d "$SRC" ] || tar -xJf "$TARBALL" -C "$WORK"

SYSROOT=$(xcrun --sdk "$SDK" --show-sdk-path)
CC=$(xcrun --sdk "$SDK" --find clang)
BUILD="$WORK/build-$SDK"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --prefix="$PREFIX" \
    --enable-cross-compile --target-os=darwin --arch=aarch64 \
    --cc="$CC" --sysroot="$SYSROOT" \
    --extra-cflags="$TARGET" --extra-ldflags="$TARGET" \
    --enable-static --disable-shared --enable-pic \
    --disable-autodetect --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avfilter --disable-network \
    --disable-everything \
    --enable-protocol=file \
    --enable-demuxer=bink \
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct \
    --enable-swscale --enable-swresample
make -j"$(sysctl -n hw.ncpu)"
rm -rf "$PREFIX"
make install
echo "FFmpeg $VERSION for $SDK in $PREFIX"
