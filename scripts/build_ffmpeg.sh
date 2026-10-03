#!/bin/sh
# Rebuild the six static FFmpeg archives and headers in library/FFmpeg/{lib,include} from the in-tree source
# library/FFmpeg/FFmpeg, with the configure line recorded in library/FFmpeg/build.txt.
#
# The only difference to that line is --prefix=.. (relative): `make install` then writes into library/FFmpeg/lib and
# library/FFmpeg/include, and no absolute host path ends up in the configuration string embedded in every archive.
# For OpenTube v0.16.1 this exact recipe (devkitARM r68, arm-none-eabi-gcc 16.1.0) produced the shipped archives.
#
# The build is in-tree (FFmpeg's own recipe): it leaves objects, config.h etc. inside library/FFmpeg/FFmpeg
# (FFmpeg's .gitignore covers them). Run it in a fresh copy of the source. Installs nothing outside this tree.
#
# Usage: scripts/build_ffmpeg.sh            environment: DEVKITPRO (default /opt/devkitpro), JOBS (default 3),
#        DRY_RUN=1 scripts/build_ffmpeg.sh   LOGDIR (default build-logs/ffmpeg)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/library/FFmpeg/FFmpeg"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export DEVKITPRO DEVKITARM
export PATH="$DEVKITARM/bin:$PATH"
JOBS="${JOBS:-3}"
LOGDIR="${LOGDIR:-$ROOT/build-logs/ffmpeg}"

[ -f "$SRC/configure" ] || { echo "FFmpeg source not found at $SRC"; exit 2; }
command -v arm-none-eabi-gcc >/dev/null 2>&1 || { echo "arm-none-eabi-gcc not found (set DEVKITPRO / DEVKITARM)"; exit 2; }
command -v make >/dev/null 2>&1 || { echo "make not found"; exit 2; }
echo "compiler: $(arm-none-eabi-gcc --version | head -1)  (v0.16.1 used: arm-none-eabi-gcc (devkitARM) 16.1.0)"
if [ -e "$SRC/config.h" ] || [ -e "$SRC/ffbuild/config.mak" ]; then
	echo "warning: $SRC already contains configure output; use a fresh copy for a clean, comparable build"
fi

set -- --enable-cross-compile --cross-prefix=arm-none-eabi- --prefix=.. --cpu=armv6k --arch=arm --target-os=linux \
	"--extra-cflags=-mfloat-abi=hard -mtune=mpcore -mtp=cp15 -D_POSIX_THREADS" "--extra-ldflags=-mfloat-abi=hard" \
	--disable-filters --disable-devices --disable-bsfs --disable-parsers --disable-hwaccels --disable-debug \
	--disable-programs --disable-avdevice --disable-postproc --disable-decoders --disable-demuxers --disable-encoders \
	--disable-muxers --disable-asm --disable-protocols --enable-pthreads --enable-inline-asm --enable-vfp \
	--enable-armv5te --enable-armv6 "--enable-filter=chorus,superequalizer,volume,asetrate,aformat,aresample,atempo,aecho,anull" \
	"--enable-decoder=aac,h264,opus" "--enable-demuxer=mov" "--enable-protocol=file"

if [ "${DRY_RUN:-0}" = 1 ]; then
	echo "DRY RUN, would run in $SRC:"
	printf '  ./configure'; printf " '%s'" "$@"; echo
	echo "  make -j$JOBS"
	echo "  make install"
	exit 0
fi

mkdir -p "$LOGDIR"
cd "$SRC"
./configure "$@" > "$LOGDIR/configure.log" 2>&1 || { echo "configure failed, see $LOGDIR/configure.log"; tail -20 "$LOGDIR/configure.log"; exit 3; }
echo "configure: ok"
make -j"$JOBS" > "$LOGDIR/make.log" 2>&1 || { echo "make failed, see $LOGDIR/make.log"; tail -20 "$LOGDIR/make.log"; exit 4; }
echo "make: ok"
make install > "$LOGDIR/install.log" 2>&1 || { echo "make install failed, see $LOGDIR/install.log"; exit 5; }
echo "make install: ok"
cd "$ROOT/library/FFmpeg/lib"
shasum -a 256 libavcodec.a libavfilter.a libavformat.a libavutil.a libswresample.a libswscale.a | tee "$LOGDIR/ARCHIVES.sha256"
echo "Compare with Documentation/v0.16.1/LINKED_ARCHIVES.txt (the archives linked into the v0.16.1 release)."
