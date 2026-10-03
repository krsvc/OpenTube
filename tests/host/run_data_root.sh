#!/bin/sh
# Builds and runs the v0.16.2 data root tests on the host (clang++ only, no devkitARM).
#   TEST_OUT_DIR=<fresh dir> sh tests/host/run_data_root.sh [<baseline tree> [<pre-APT-fix tree>]]
# With a baseline tree (a checkout of the previous source), the definitions and start-up glue tests are also built
# against it and must fail behaviorally (RED). With a pre-APT-fix tree (APT_BOOTSTRAP_FIX_BRIEF.md), the start-up glue
# test is built against it and must fail in its APT assertions only. Keep TEST_OUT_DIR short: the helper caps complete
# staging paths at 250 bytes. Nothing is ever deleted: every output directory must be new.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TEST_OUT_DIR:?set TEST_OUT_DIR to a fresh directory}"
[ ! -e "$OUT" ] || { echo "refusing to reuse $OUT (no deletion): set a fresh TEST_OUT_DIR" >&2; exit 1; }
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
FLAGS="-std=gnu++14 -Wall -Wextra -Wno-unused -Wno-unused-parameter -O1 -g"
STUBS="$ROOT/tests/host/data_root_glue_stubs"

# $1 tree, $2 output dir: definitions probe, both build kinds
build_defs() {
	mkdir -p "$2"
	"$CXX" $FLAGS -DDEF_DIAG_BUILD -I"$1/source" "$ROOT/tests/host/test_data_root_defs.cpp" -o "$2/defs_opentube"
	"$CXX" $FLAGS -I"$1/source" "$ROOT/tests/host/test_data_root_defs.cpp" -o "$2/defs_upstream"
}
# $1 tree, $2 output dir: REAL main(), Menu_init() (+ gate) and Menu_exit() of the tree, the tree's REAL
# system/apt_handler.cpp + system/cpu_limit.cpp, REAL data_root.cpp of the candidate
build_glue() {
	mkdir -p "$2"
	awk '/^int main\(\) \{/ {f=1} f {print} f && /^}/ {exit}' "$1/source/main.cpp" | sed 's/^int main() {/int app_main() {/' \
		> "$2/main.inc"
	grep -q 'int app_main() {' "$2/main.inc" || { echo "extraction failed: main" >&2; exit 1; }
	awk '/^namespace SceneSwitcher \{/ {f=1} f {print} f && /^(void|bool) Menu_init\(void\) \{/ {m=1} m && /^}/ {exit}' \
		"$1/source/scene_switcher.cpp" | sed 's/\*(u8 \*)0x1FF81067/mock_wifi_byte/g' > "$2/menu_init.inc"
	grep -q 'Menu_init(void) {' "$2/menu_init.inc" || { echo "extraction failed: Menu_init" >&2; exit 1; }
	grep -q 'mock_wifi_byte' "$2/menu_init.inc" || { echo "extraction failed: Wi-Fi byte" >&2; exit 1; }
	awk '/^(static )?void Menu_exit(_track_async)?\(void\) \{/ {f=1} f {print} f && /^}/ {f=0}' \
		"$1/source/scene_switcher.cpp" > "$2/menu_exit.inc"
	grep -q '^void Menu_exit(void) {' "$2/menu_exit.inc" || { echo "extraction failed: Menu_exit" >&2; exit 1; }
	"$CXX" $FLAGS -DDEF_DIAG_BUILD -I"$2" -I"$STUBS" -I"$1/source" -I"$ROOT/source" \
		"$ROOT/tests/host/test_data_root_glue.cpp" "$ROOT/source/data_io/data_root.cpp" \
		"$1/source/system/apt_handler.cpp" "$1/source/system/cpu_limit.cpp" \
		"$ROOT/source/util/freeze_diag.cpp" -lpthread -o "$2/test_data_root_glue"
}

echo "== candidate: production definitions"
build_defs "$ROOT" "$OUT/defs"
"$OUT/defs/defs_opentube"
"$OUT/defs/defs_upstream"

echo "== candidate: data root helper (real files + fault injection)"
"$CXX" $FLAGS -I"$ROOT/source" -I"$ROOT/library" \
	"$ROOT/source/data_io/data_root.cpp" "$ROOT/source/data_io/liked_videos.cpp" \
	"$ROOT/source/downloads/download_store.cpp" "$ROOT/source/downloads/thumb_cache.cpp" \
	"$ROOT/tests/host/test_data_root.cpp" -o "$OUT/test_data_root"
"$OUT/test_data_root" "$OUT/helper_fixtures"

echo "== candidate: start-up glue (REAL main + Menu_init + gate)"
build_glue "$ROOT" "$OUT/glue"
"$OUT/glue/test_data_root_glue" "$OUT/glue_fixtures"

if [ $# -ge 1 ]; then
	BASE_TREE="$(cd "$1" && pwd)"
	echo "== baseline RED: production definitions ($BASE_TREE)"
	build_defs "$BASE_TREE" "$OUT/red_defs"
	rc=0; "$OUT/red_defs/defs_opentube" || rc=$?
	[ $rc -ne 0 ] || { echo "RED expected: the baseline OpenTube build must not save under /3ds/opentube/" >&2; exit 1; }
	echo "baseline definitions: RED as expected (exit $rc)"
	"$OUT/red_defs/defs_upstream" # the upstream build must be identical before and after
	echo "== baseline RED: start-up glue"
	build_glue "$BASE_TREE" "$OUT/red_glue"
	"$OUT/red_glue/test_data_root_glue" "$OUT/red_glue_fixtures" red
	echo "baseline glue: RED as expected"
fi
if [ $# -ge 2 ]; then
	PRE_TREE="$(cd "$2" && pwd)"
	echo "== pre-APT-fix RED: start-up glue ($PRE_TREE)"
	build_glue "$PRE_TREE" "$OUT/prefix_glue"
	rc=0; "$OUT/prefix_glue/test_data_root_glue" "$OUT/prefix_glue_fixtures" > "$OUT/prefix_glue.log" 2>&1 || rc=$?
	cat "$OUT/prefix_glue.log"
	all=$(grep -c '^    FAIL' "$OUT/prefix_glue.log" || true)
	apt=$(grep '^    FAIL' "$OUT/prefix_glue.log" | grep -c ': apt: ' || true)
	[ "$rc" -ne 0 ] && [ "$all" -gt 0 ] && [ "$all" -eq "$apt" ] \
		|| { echo "pre-APT-fix RED expected: APT-only failures (exit $rc, $all failures, $apt APT)" >&2; exit 1; }
	echo "pre-APT-fix glue: RED as expected ($apt APT failures, no other failure)"
fi
echo "data root tests: all passed"
