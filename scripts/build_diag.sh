#!/bin/sh
# Repeatable diagnostic build of "FourthTube Test" (make diag).
# - Installs NOTHING. If a prerequisite is missing it prints what is needed and exits 2.
# - Never runs `make clean`, never touches build/ or the tracked FourthTube.cia.
# Usage: scripts/build_diag.sh            (optionally MAKEROM=/path BANNERTOOL=/path)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

missing=""
if [ -z "${DEVKITARM:-}" ] || [ ! -x "$DEVKITARM/bin/arm-none-eabi-gcc" ]; then
	missing="$missing\n - devkitARM (r63+): DEVKITARM must point to it (arm-none-eabi-gcc missing)"
fi
if [ -z "${DEVKITPRO:-}" ] || [ ! -d "$DEVKITPRO/libctru" ]; then
	missing="$missing\n - devkitPro root: DEVKITPRO must point to it (libctru dir missing)"
fi
for tool in tex3ds picasso bin2s 3dsxtool; do
	if [ -z "${DEVKITPRO:-}" ] || [ ! -x "$DEVKITPRO/tools/bin/$tool" ]; then
		missing="$missing\n - $tool (devkitPro 3ds-dev tools)"
	fi
done
if [ -n "${DEVKITPRO:-}" ]; then
	[ -f "$DEVKITPRO/portlibs/3ds/lib/libmbedtls.a" ] || missing="$missing\n - 3ds-mbedtls 2.16.6-1 in $DEVKITPRO/portlibs/3ds"
	[ -f "$DEVKITPRO/portlibs/3ds/lib/libz.a" ] || missing="$missing\n - 3ds-zlib in $DEVKITPRO/portlibs/3ds"
fi
MAKEROM="${MAKEROM:-resource/tools/makerom}"
BANNERTOOL="${BANNERTOOL:-resource/tools/bannertool}"
# A runnable tool prints its usage even though its exit code is non-zero (makerom -help exits 254,
# bannertool without args exits 1). Only a missing / non-executable / wrong-arch binary prints nothing usable.
tool_runs() { # $1 = binary, $2 = args, $3 = word expected in the usage text
	[ -x "$1" ] || return 1
	# shellcheck disable=SC2086
	"$1" $2 2>&1 | grep -qi "$3"
}
if ! tool_runs "$MAKEROM" "-help" "usage"; then
	missing="$missing\n - makerom runnable on this host (MAKEROM=$MAKEROM; the bundled one is a Linux x86_64 ELF)"
fi
if ! tool_runs "$BANNERTOOL" "" "usage"; then
	missing="$missing\n - bannertool runnable on this host (BANNERTOOL=$BANNERTOOL; the bundled one is a Linux x86_64 ELF)"
fi
if [ -n "$missing" ]; then
	printf 'Diagnostic build NOT started. Missing prerequisites (nothing was installed):%b\n' "$missing"
	printf '\nOfficial requirements: devkitARM r63+, 3ds-zlib, 3ds-mbedtls 2.16.6-1 (Documentation/Build Instructions.md).\n'
	exit 2
fi

JOBS="${JOBS:-3}"                       # bounded parallelism
DIAG_BUILD="${DIAG_BUILD:-build_diag}"   # object dir; use a fresh name for a full (non-incremental) build
DIAG_TARGET="${DIAG_TARGET:-FourthTubeTest}"  # output base name; use a fresh name to keep earlier outputs intact
DIAG_FPS="${DIAG_FPS:-1}"               # 0 = same tree without frame-pacing instrumentation (fresh DIAG_BUILD)
[ -f "$DEVKITPRO/libctru/lib/libctru.a" ] || { printf 'Missing %s/libctru/lib/libctru.a (diag builds link the installed libctru, not the bundled fork).\n' "$DEVKITPRO"; exit 2; }
CURL_LIB="${DIAG_CURL_LIBDIR:-deps/curl-7.82.0-mbedtls2.28.8/lib}"
if [ ! -f "$CURL_LIB/libcurl.a" ] && [ -z "${DIAG_ALLOW_BUNDLED_CURL:-}" ]; then
	printf 'Missing %s/libcurl.a: run scripts/build_curl_diag.sh first (or DIAG_ALLOW_BUNDLED_CURL=1).\n' "$CURL_LIB"
	exit 2
fi
BUILD_ID="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo -dirty)"
echo "Building FourthTube Test (make diag, JOBS=$JOBS, DIAG_TARGET=$DIAG_TARGET, DIAG_BUILD=$DIAG_BUILD, DIAG_FPS=$DIAG_FPS, curl=$CURL_LIB, libctru=$DEVKITPRO/libctru)"
make diag JOBS="$JOBS" DIAG_TARGET="$DIAG_TARGET" DIAG_BUILD="$DIAG_BUILD" DIAG_FPS="$DIAG_FPS" MAKEROM="$MAKEROM" BANNERTOOL="$BANNERTOOL"
echo
# versioned release directory: every delivered artifact is tied to its build id and linked libraries
STAMP="$(date +%Y%m%d-%H%M%S)"
REL="release/$STAMP-$BUILD_ID"
mkdir -p "$REL"
cp "$DIAG_TARGET.3dsx" "$DIAG_TARGET.cia" "$DIAG_TARGET.elf" "$DIAG_TARGET.smdh" "$REL/"
cp "$DIAG_BUILD/$DIAG_TARGET.map" "$REL/$DIAG_TARGET.map"
# exact source identity: HEAD + every tracked file + every untracked (non-ignored) file, hashed
{
	echo "head=$(git rev-parse HEAD)"
	echo "build_id=$BUILD_ID"
	echo "tracked_diff_sha256=$(git diff HEAD | shasum -a 256 | cut -d' ' -f1)"
	git status --short | sed 's/^/status: /'
	{ git ls-files -z; git ls-files --others --exclude-standard -z; } | xargs -0 shasum -a 256 | sort -k2
} > "$REL/SOURCE_MANIFEST.txt"
{
	echo "build_id=$BUILD_ID"
	echo "date=$STAMP"
	echo "target=$DIAG_TARGET jobs=$JOBS objdir=$DIAG_BUILD fps_instrumentation=$DIAG_FPS"
	echo "curl_lib=$CURL_LIB"
	echo "libctru=$DEVKITPRO/libctru (installed; bundled library/libctru is NOT linked in diag builds)"
	echo "makerom=$MAKEROM"
	echo "bannertool=$BANNERTOOL"
	echo "devkitARM=$("$DEVKITARM/bin/arm-none-eabi-gcc" --version | head -1)"
	if command -v pacman >/dev/null 2>&1; then pacman -Q 2>/dev/null | grep -Ei "3ds|devkitarm|libctru|citro|tex3ds" | sed 's/^/pkg=/'; fi
	echo "linked_archives (sha256, producer):"
	for a in $(grep -oE "(/[^ ()]+|[^ ()]*library/[^ ()]+)\.a\(" "$REL/$DIAG_TARGET.map" | sed 's/($//' | sort -u); do
		printf '  %s\n    sha256=%s\n    producer=%s\n' "$a" "$(shasum -a 256 "$a" | cut -d' ' -f1)" \
			"$(strings -n 8 "$a" | grep -E '^GCC: ' | sort | uniq -c | sort -rn | head -1 | sed 's/^ *//')"
	done
} > "$REL/MANIFEST.txt"
(cd "$REL" && shasum -a 256 "$DIAG_TARGET.cia" "$DIAG_TARGET.3dsx" "$DIAG_TARGET.elf" "$DIAG_TARGET.smdh" "$DIAG_TARGET.map" > SHA256SUMS)
echo "Release directory: $REL"
cat "$REL/SHA256SUMS"
echo "Build id: $BUILD_ID"
echo
# regression gates on the produced artifacts (recorded next to them); a FAIL fails the build script
gate_rc=0; python3 scripts/check_abi_pairing.py "$REL/$DIAG_TARGET.elf" "$REL/$DIAG_TARGET.map" > "$REL/ABI_GATE.txt" 2>&1 || gate_rc=$?
cat "$REL/ABI_GATE.txt"; echo "abi gate exit=$gate_rc"
id_rc=0; python3 scripts/inspect_cia.py "$REL/$DIAG_TARGET.cia" "$REL/$DIAG_TARGET.3dsx" > "$REL/IDENTITY.txt" 2>&1 || id_rc=$?
cat "$REL/IDENTITY.txt"; echo "identity exit=$id_rc"
heap_rc=0; python3 scripts/check_heap_split.py "$REL/$DIAG_TARGET.elf" > "$REL/HEAP_GATE.txt" 2>&1 || heap_rc=$?
cat "$REL/HEAP_GATE.txt"; echo "heap split gate exit=$heap_rc"
lin_rc=0; python3 scripts/check_linear_lock.py "$REL/$DIAG_TARGET.elf" > "$REL/LINEAR_LOCK_GATE.txt" 2>&1 || lin_rc=$?
cat "$REL/LINEAR_LOCK_GATE.txt"; echo "linear lock gate exit=$lin_rc"
[ "$gate_rc" -eq 0 ] && [ "$id_rc" -eq 0 ] && [ "$heap_rc" -eq 0 ] && [ "$lin_rc" -eq 0 ] || { echo "GATE FAILED (abi=$gate_rc identity=$id_rc heap=$heap_rc linear=$lin_rc): do not deliver $REL"; exit 1; }
