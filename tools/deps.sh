#!/bin/sh
# Download the sources of qalam's three libraries into deps/, so qalam can be
# built with no system packages and linked statically (release builds,
# Docker, Windows):
#
#   tools/deps.sh            then   make VENDOR=1
#
#   deps/harfbuzz   HarfBuzz   text shaping and font subsetting   (MIT)
#   deps/fribidi    FriBidi    the Unicode bidi algorithm         (LGPL-2.1+)
#   deps/zlib       zlib       compression of PDF streams         (zlib)
#
# Versions and checksums are pinned below. Downloads are cached in
# deps/cache, so running it again is cheap. Needs curl or wget, tar, xz.
set -eu

HB_VERSION=10.1.0
HB_SHA256=6ce3520f2d089a33cef0fc48321334b8e0b72141f6a763719aaaecd2779ecb82
FRIBIDI_VERSION=1.0.16
FRIBIDI_SHA256=1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
ZLIB_VERSION=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

cd "$(dirname "$0")/.."
mkdir -p deps/cache

die() { printf 'deps.sh: %s\n' "$*" >&2; exit 1; }

fetch() {   # url file
    if command -v curl >/dev/null 2>&1; then curl -fsSL "$1" -o "$2"
    elif command -v wget >/dev/null 2>&1; then wget -q "$1" -O "$2"
    else die "needs curl or wget"; fi
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else echo unknown; fi
}

get() {   # name url sha256 dir-in-archive
    name=$1 url=$2 want=$3 inner=$4
    file=deps/cache/$(basename "$url")
    if [ ! -f "$file" ] || [ "$(sha256 "$file")" != "$want" ]; then
        echo "deps: downloading $(basename "$url")"
        fetch "$url" "$file.part" || die "download failed: $url"
        mv "$file.part" "$file"
    fi
    got=$(sha256 "$file")
    [ "$got" = unknown ] && echo "deps: warning: no sha256sum or shasum, $(basename "$file") not verified"
    [ "$got" = unknown ] || [ "$got" = "$want" ] || die "checksum mismatch for $file"
    if [ ! -f "deps/$name/.version" ] || [ "$(cat "deps/$name/.version")" != "$want" ]; then
        rm -rf "deps/$name" "deps/$inner"
        tar -xf "$file" -C deps
        mv "deps/$inner" "deps/$name"
        echo "$want" > "deps/$name/.version"
        echo "deps: $name ready"
    fi
}

get harfbuzz "https://github.com/harfbuzz/harfbuzz/releases/download/$HB_VERSION/harfbuzz-$HB_VERSION.tar.xz" \
    "$HB_SHA256" "harfbuzz-$HB_VERSION"
get fribidi "https://github.com/fribidi/fribidi/releases/download/v$FRIBIDI_VERSION/fribidi-$FRIBIDI_VERSION.tar.xz" \
    "$FRIBIDI_SHA256" "fribidi-$FRIBIDI_VERSION"
get zlib "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz" \
    "$ZLIB_SHA256" "zlib-$ZLIB_VERSION"

# HarfBuzz ships two "amalgamated" sources, harfbuzz.cc and harfbuzz-subset.cc,
# meant for two separate libraries; compiled into one program they define some
# things twice. Write a unit with only what harfbuzz-subset.cc adds.
(
    cd deps/harfbuzz/src
    grep -o '"[^"]*"' harfbuzz.cc | sort > ../../hb-main.list
    echo "/* written by tools/deps.sh: harfbuzz-subset.cc minus harfbuzz.cc */"
    grep -o '"[^"]*"' harfbuzz-subset.cc | sort | comm -13 ../../hb-main.list - | sed 's/^/#include /'
    rm -f ../../hb-main.list
) > deps/harfbuzz/src/qalam-hb-subset.cc

# FriBidi's configure step only produces this header; write it ourselves.
major=${FRIBIDI_VERSION%%.*}; rest=${FRIBIDI_VERSION#*.}; minor=${rest%%.*}; micro=${rest#*.}
cat > deps/fribidi/lib/fribidi-config.h <<EOF
/* written by tools/deps.sh */
#ifndef FRIBIDI_CONFIG_H
#define FRIBIDI_CONFIG_H
#define FRIBIDI "fribidi"
#define FRIBIDI_NAME "GNU FriBidi"
#define FRIBIDI_BUGREPORT "https://github.com/fribidi/fribidi/issues/new"
#define FRIBIDI_VERSION "$FRIBIDI_VERSION"
#define FRIBIDI_MAJOR_VERSION $major
#define FRIBIDI_MINOR_VERSION $minor
#define FRIBIDI_MICRO_VERSION $micro
#define FRIBIDI_INTERFACE_VERSION 4
#define FRIBIDI_INTERFACE_VERSION_STRING "4"
#define FRIBIDI_SIZEOF_INT 4
#endif
EOF
echo "deps: all ready; build with  make VENDOR=1"
