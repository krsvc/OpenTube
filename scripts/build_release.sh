#!/bin/sh
# Git-free release build of OpenTube: `make diag` (the target the releases use), then the four binary gates.
# Installs nothing, never runs `make clean`, refuses an existing object directory.
#
# Environment (all optional):
#   DEVKITPRO        devkitPro root (default /opt/devkitpro); DEVKITARM defaults to $DEVKITPRO/devkitARM
#   MAKEROM          makerom binary (default: `makerom` on PATH, then $DEVKITPRO/tools/bin/makerom)
#   BANNERTOOL       bannertool binary (default: `bannertool` on PATH, then $DEVKITPRO/tools/bin/bannertool)
#   OPENTUBE_BUILD_ID  build id compiled into the app; default nogit-<first 12 hex of sha256(BUILD_INPUTS.sha256)>.
#                    v0.16.1 was built with nogit-c5013471390c (see Documentation/v0.16.1/).
#   DIAG_BUILD       object directory, must not exist (default build_release_<timestamp>)
#   OUT_DIR          where release folders go (default release-out)
#   JOBS             parallel jobs (default 3)
#   DRY_RUN=1        only check prerequisites and print the make command
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export DEVKITPRO DEVKITARM PYTHONDONTWRITEBYTECODE=1
STAMP="$(date +%Y%m%d-%H%M%S)"
JOBS="${JOBS:-3}"
TARGET=OpenTube
DIAG_BUILD="${DIAG_BUILD:-build_release_$STAMP}"
OUT_DIR="${OUT_DIR:-$ROOT/release-out}"
CURL_LIB="deps/curl-7.82.0-mbedtls2.28.8/lib/libcurl.a"

missing=""
[ -x "$DEVKITARM/bin/arm-none-eabi-gcc" ] || missing="$missing\n - devkitARM (arm-none-eabi-gcc) at $DEVKITARM"
[ -f "$DEVKITPRO/libctru/lib/libctru.a" ] || missing="$missing\n - libctru in $DEVKITPRO/libctru"
for l in libcitro2d.a libcitro3d.a; do [ -f "$DEVKITPRO/libctru/lib/$l" ] || missing="$missing\n - $l in $DEVKITPRO/libctru/lib"; done
for l in libmbedtls.a libmbedx509.a libmbedcrypto.a libz.a; do
	[ -f "$DEVKITPRO/portlibs/3ds/lib/$l" ] || missing="$missing\n - $l in $DEVKITPRO/portlibs/3ds/lib (3ds-mbedtls / 3ds-zlib)"
done
for t in tex3ds picasso bin2s 3dsxtool smdhtool; do [ -x "$DEVKITPRO/tools/bin/$t" ] || missing="$missing\n - $t in $DEVKITPRO/tools/bin"; done
[ -f "$CURL_LIB" ] || missing="$missing\n - $CURL_LIB (run scripts/build_curl_diag.sh)"
for a in libavcodec libavfilter libavformat libavutil libswresample libswscale; do
	[ -f "library/FFmpeg/lib/$a.a" ] || missing="$missing\n - library/FFmpeg/lib/$a.a (run scripts/build_ffmpeg.sh)"
done
find_tool() { # $1 = env value, $2 = name
	if [ -n "$1" ]; then echo "$1"; return; fi
	if command -v "$2" >/dev/null 2>&1; then command -v "$2"; return; fi
	if [ -x "$DEVKITPRO/tools/bin/$2" ]; then echo "$DEVKITPRO/tools/bin/$2"; return; fi
	echo ""
}
MAKEROM="$(find_tool "${MAKEROM:-}" makerom)"
BANNERTOOL="$(find_tool "${BANNERTOOL:-}" bannertool)"
# runnable tools print their usage with a non-zero exit code
if [ -z "$MAKEROM" ] || ! "$MAKEROM" -help 2>&1 | grep -qi usage; then
	missing="$missing\n - makerom (v0.19.0 used for v0.16.1, https://github.com/3DSGuy/Project_CTR): put it on PATH or set MAKEROM"
fi
if [ -z "$BANNERTOOL" ] || ! "$BANNERTOOL" 2>&1 | grep -qi usage; then
	missing="$missing\n - bannertool (v1.2.1 used for v0.16.1, https://github.com/carstene1ns/3ds-bannertool): put it on PATH or set BANNERTOOL"
fi
command -v python3 >/dev/null 2>&1 || missing="$missing\n - python3 (binary gates)"
if [ -n "$missing" ]; then printf "missing prerequisites:%b\n" "$missing"; exit 2; fi
[ ! -e "$DIAG_BUILD" ] || { echo "object directory $DIAG_BUILD exists: pass a fresh DIAG_BUILD"; exit 2; }

echo "compiler:   $("$DEVKITARM/bin/arm-none-eabi-gcc" --version | head -1)"
echo "makerom:    $MAKEROM ($("$MAKEROM" -help 2>&1 | head -1))"
echo "bannertool: $BANNERTOOL ($("$BANNERTOOL" 2>&1 | head -1))"

# Manifest of the app's build inputs (own sources, headers and archives it compiles / links, packaging assets).
# The FFmpeg source tree is not listed: its compiled result is the archives in library/FFmpeg/lib, which are.
build_inputs() {
	find Makefile source romfs resource library/FFmpeg/include library/FFmpeg/lib library/libbrotli library/nghttp2 \
		library/libcurl library/rapidjson library/stb_image deps/curl-7.82.0-mbedtls2.28.8/lib -type f -print0 \
		| LC_ALL=C sort -z | xargs -0 shasum -a 256
}
BI_SHA="$(build_inputs | shasum -a 256 | cut -c1-64)"
BUILD_ID="${OPENTUBE_BUILD_ID:-nogit-$(echo "$BI_SHA" | cut -c1-12)}"
echo "build inputs: $(build_inputs | wc -l | tr -d ' ') files, sha256 $BI_SHA, build id $BUILD_ID"

set -- diag JOBS="$JOBS" DIAG_TARGET="$TARGET" DIAG_BUILD="$DIAG_BUILD" DIAG_FPS=1 DIAG_BUILD_ID="$BUILD_ID" \
	MAKEROM="$MAKEROM" BANNERTOOL="$BANNERTOOL"
if [ "${DRY_RUN:-0}" = 1 ]; then
	printf 'DRY RUN, would run: make'; printf " '%s'" "$@"; echo
	exit 0
fi
mkdir -p "$OUT_DIR"
BI="$OUT_DIR/BUILD_INPUTS-$STAMP.sha256"
build_inputs > "$BI"
[ "$(shasum -a 256 "$BI" | cut -c1-64)" = "$BI_SHA" ] || { echo "build inputs changed while starting"; exit 2; }
make "$@"
echo "make diag: ok"

REL="$OUT_DIR/$STAMP-$BUILD_ID"
mkdir -p "$REL"
cp "$TARGET.cia" "$TARGET.3dsx" "$TARGET.elf" "$TARGET.smdh" "$REL/"
cp "$DIAG_BUILD/$TARGET.map" "$REL/"
mv "$BI" "$REL/BUILD_INPUTS.sha256"
(cd "$REL" && shasum -a 256 "$TARGET.cia" "$TARGET.3dsx" > SHA256SUMS)
echo "release folder: $REL"; cat "$REL/SHA256SUMS"

rc=0
python3 scripts/check_abi_pairing.py "$REL/$TARGET.elf" "$REL/$TARGET.map" > "$REL/ABI_GATE.txt" 2>&1 || rc=1
python3 scripts/inspect_cia.py "$REL/$TARGET.cia" "$REL/$TARGET.3dsx" > "$REL/IDENTITY.txt" 2>&1 || rc=1
python3 scripts/check_heap_split.py "$REL/$TARGET.elf" > "$REL/HEAP_GATE.txt" 2>&1 || rc=1
python3 scripts/check_linear_lock.py "$REL/$TARGET.elf" > "$REL/LINEAR_LOCK_GATE.txt" 2>&1 || rc=1
grep -h -E 'PASS|FAIL|title_id|title_version' "$REL"/ABI_GATE.txt "$REL"/HEAP_GATE.txt "$REL"/LINEAR_LOCK_GATE.txt "$REL"/IDENTITY.txt | head -20
[ $rc -eq 0 ] || { echo "GATE FAILED: see $REL/*_GATE.txt and IDENTITY.txt"; exit 1; }
echo "all 4 gates pass"
