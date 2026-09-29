#!/usr/bin/env bash
# Build FastPlay's FFmpeg: audio only (the demuxers, decoders and parsers of the
# formats FastPlay plays, the network protocols for streams, and the FLAC encoder
# for recording), as static libraries, with no outside libraries. LGPL. Our
# few changes to FFmpeg are in ci/ffmpeg/patches.
#
#   ci/ffmpeg/build.sh windows <out-dir>   (in MSYS2, with the MSVC tools on PATH)
#   ci/ffmpeg/build.sh macos <out-dir>     (a universal arm64 + x86_64 build)
#
# <out-dir> receives include/, lib/ and BUILDINFO.txt. The version to build is in
# ci/ffmpeg/VERSION (an FFmpeg tag).
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
platform="$1"
out="$(mkdir -p "$2" && cd "$2" && pwd)"
version="$(tr -d ' \r\n' < "$here/VERSION")"
work="${FFMPEG_WORK:-$here/work}"

demuxers=(
    aac ac3 ac4 aiff amr ape asf au caf dsf dts dtshd eac3 flac hls iff loas
    matroska mlp mov mp3 mpc mpc8 mpegts ogg oma rm shorten tak truehd tta voc
    w64 wav wv xwma 'pcm_*'
)
decoders=(
    aac aac_latm ac3 eac3 alac als amrnb amrwb ape atrac1 atrac3 atrac3p atrac9
    cook dca dolby_e 'dsd_*' dst flac g723_1 g729 gsm gsm_ms mace3 mace6 mlp
    truehd mp1 mp1float mp2 mp2float mp3 mp3float mp3adu mp3adufloat mp3on4
    mp3on4float mpc7 mpc8 nellymoser opus qdm2 qdmc ra_144 ra_288 ralf shorten
    sipr speex tak truespeech tta twinvq vorbis wavpack wmalossless wmapro wmav1
    wmav2 wmavoice xma1 xma2 'pcm_*' 'adpcm_*'
)
parsers=(aac aac_latm ac3 amr cook dca dolby_e flac gsm mlp mpegaudio opus sipr tak vorbis)
protocols=(file http https httpproxy tcp tls hls crypto data)

common=(
    --disable-everything --disable-autodetect
    --disable-programs --disable-doc --disable-debug
    --disable-avdevice --disable-avfilter --disable-swscale
    --enable-static --disable-shared --enable-pic
    --enable-avformat --enable-avcodec --enable-swresample
    --enable-network
    --enable-encoder=flac --enable-muxer=flac
)
for d in "${demuxers[@]}"; do common+=(--enable-demuxer="$d"); done
for d in "${decoders[@]}"; do common+=(--enable-decoder="$d"); done
for p in "${parsers[@]}"; do common+=(--enable-parser="$p"); done
for p in "${protocols[@]}"; do common+=(--enable-protocol="$p"); done

fetch() {
    mkdir -p "$work"
    if [ ! -d "$work/src" ]; then
        git clone --depth 1 --branch "$version" https://github.com/FFmpeg/FFmpeg.git "$work/src"
    fi
    # FastPlay's changes to FFmpeg (ci/ffmpeg/patches), applied once
    if [ ! -f "$work/src/.fastplay-patched" ]; then
        for p in "$here"/patches/*.patch; do
            [ -e "$p" ] || continue
            # The files it changes with LF line endings, as the patch has them,
            # whichever git checked them out
            for t in $(sed -n 's|^+++ b/\([^[:space:]]*\).*|\1|p' "$p"); do
                tr -d '\r' < "$work/src/$t" > "$work/src/$t.lf" && mv "$work/src/$t.lf" "$work/src/$t"
            done
            git -C "$work/src" apply "$p"
        done
        touch "$work/src/.fastplay-patched"
    fi
}

# Configure and build one architecture into $2, from a copy of the source.
build_one() {
    local name="$1" prefix="$2"
    shift 2
    rm -rf "$work/build-$name"
    mkdir -p "$work/build-$name"
    (
        cd "$work/build-$name"
        "$work/src/configure" --prefix="$prefix" "${common[@]}" "$@" \
            || { tail -n 60 ffbuild/config.log; exit 1; }
        make -j"${JOBS:-4}"
        make install
    )
}

fetch
rm -rf "$out/include" "$out/lib"

case "$platform" in
    windows)
        # Static runtime (/MT), as FastPlay; Windows 7 (0x0601) is FFmpeg's own floor.
        build_one x64 "$work/install-x64" \
            --toolchain=msvc --arch=x86_64 --target-os=win64 \
            --extra-cflags="-MT -D_WIN32_WINNT=0x0601" \
            --enable-schannel
        cp -r "$work/install-x64/include" "$out/include"
        mkdir -p "$out/lib"
        # Static libraries come out as X.lib (or, from older FFmpeg, libX.a)
        for f in "$work/install-x64/lib"/*.lib "$work/install-x64/lib"/lib*.a; do
            [ -e "$f" ] || continue
            base="$(basename "$f")"
            base="${base%.lib}"
            base="${base%.a}"
            cp "$f" "$out/lib/${base#lib}.lib"
        done
        ls "$out/lib"/*.lib > /dev/null
        ;;
    macos)
        min="-mmacosx-version-min=11.0"
        for arch in arm64 x86_64; do
            extra=()
            # Intel slice without the x86 assembly (it needs nasm; audio decoding is cheap)
            [ "$arch" = x86_64 ] && extra+=(--disable-x86asm)
            build_one "$arch" "$work/install-$arch" \
                --enable-cross-compile --target-os=darwin --arch="$arch" \
                --cc="clang -arch $arch" \
                --extra-cflags="$min" --extra-ldflags="$min -arch $arch" \
                --enable-securetransport --enable-zlib \
                ${extra[@]+"${extra[@]}"}
        done
        cp -r "$work/install-arm64/include" "$out/include"
        mkdir -p "$out/lib"
        for f in "$work/install-arm64/lib"/*.a; do
            base="$(basename "$f")"
            lipo -create "$f" "$work/install-x86_64/lib/$base" -output "$out/lib/$base"
        done
        ;;
    *)
        echo "usage: $0 windows|macos <out-dir>" >&2
        exit 2
        ;;
esac

{
    echo "FFmpeg $version, audio only, built for FastPlay by ci/ffmpeg/build.sh ($platform)"
    echo "LGPL 2.1 or later; source: https://github.com/FFmpeg/FFmpeg/tree/$version"
} > "$out/BUILDINFO.txt"
cp "$work/src/COPYING.LGPLv2.1" "$out/COPYING.LGPLv2.1"
echo "FFmpeg $version built into $out"
