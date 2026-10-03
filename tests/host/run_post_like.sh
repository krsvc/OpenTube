#!/bin/sh
# Comment like icon regression (COMMENT_LIKE_FIX_BRIEF.md): REAL post.cpp + modern / theme / shell / view against a
# draw recorder (tests/host/test_post_like.cpp; stand-in headers in tests/host/post_like_stubs are copies of the
# original host suite's pocket_ui_stubs). clang++ only.
#   TEST_OUT_DIR=<fresh short dir> sh tests/host/run_post_like.sh [<pre-fix tree>]
# GREEN on this tree (captures written as [fixture] JSON; rendered to PNG when RENDER=<render_draw_capture.py> and a
# Pillow-capable PYTHON are given). With a pre-fix tree the same test must fail in the like slot checks only, with an
# identical layout. Nothing is ever deleted: TEST_OUT_DIR must be new.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${TEST_OUT_DIR:?set TEST_OUT_DIR to a fresh directory}"
[ ! -e "$OUT" ] || { echo "refusing to reuse $OUT (no deletion): set a fresh TEST_OUT_DIR" >&2; exit 1; }
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
PYTHON="${PYTHON:-python3}"
STUBS="$ROOT/tests/host/post_like_stubs"

# $1 tree, $2 output dir: the tree's own copy of each file when it has one, else this tree's
build() {
	mkdir -p "$2"
	objs=""
	for f in ui/views/specialized/post.cpp ui/modern.cpp ui/theme.cpp ui/shell.cpp ui/views/view.cpp; do
		src="$1/source/$f"
		[ -f "$src" ] || src="$ROOT/source/$f"
		echo "  $f <- $src" >> "$2/sources.txt"
		"$CXX" -std=gnu++14 -Wall -Wextra -Wno-unused -Wno-unused-parameter -O1 -g -I"$STUBS" -I"$1/source" \
			-I"$ROOT/source" -I"$ROOT/library" -c "$src" -o "$2/$(basename "$f").o"
		objs="$objs $2/$(basename "$f").o"
	done
	"$CXX" -std=gnu++14 -Wall -Wextra -Wno-unused -Wno-unused-parameter -O1 -g -I"$STUBS" -I"$1/source" \
		-I"$ROOT/source" -I"$ROOT/library" -c "$ROOT/tests/host/test_post_like.cpp" -o "$2/test.o"
	# shellcheck disable=SC2086
	"$CXX" "$2/test.o" $objs -o "$2/test_post_like"
}

echo "== candidate: comment like icon (REAL PostView)"
build "$ROOT" "$OUT/green"
mkdir -p "$OUT/captures"
"$OUT/green/test_post_like" "$OUT/captures" | tee "$OUT/green.log"
grep '^LAYOUT' "$OUT/green.log" > "$OUT/green.layout"
if [ -n "${RENDER:-}" ]; then
	PYTHONDONTWRITEBYTECODE=1 "$PYTHON" "$RENDER" "$OUT/captures"
	echo "captures: $(ls "$OUT/captures"/*.png | wc -l | tr -d ' ') PNG in $OUT/captures"
fi

if [ $# -ge 1 ]; then
	PRE="$(cd "$1" && pwd)"
	echo "== pre-fix RED: comment like icon ($PRE)"
	build "$PRE" "$OUT/red"
	rc=0; "$OUT/red/test_post_like" > "$OUT/red.log" 2>&1 || rc=$?
	cat "$OUT/red.log"
	grep '^LAYOUT' "$OUT/red.log" > "$OUT/red.layout" || true
	all=$(grep -c '^FAIL' "$OUT/red.log" || true)
	slot=$(grep '^FAIL' "$OUT/red.log" | grep -cE 'legacy thumb_up texture drawn|opaque square backing in the 16x16 like slot|vector like icon drawn in the slot' || true)
	legacy=$(grep '^FAIL' "$OUT/red.log" | grep -c 'legacy thumb_up texture drawn' || true)
	[ "$rc" -ne 0 ] && [ "$all" -gt 0 ] && [ "$all" -eq "$slot" ] && [ "$legacy" -eq 4 ] \
		|| { echo "pre-fix RED expected: like-slot failures only, legacy texture in all 4 themes (exit $rc, $all failures, $slot slot, $legacy legacy)" >&2; exit 1; }
	cmp "$OUT/green.layout" "$OUT/red.layout" \
		|| { echo "layout differs between the pre-fix and the corrected PostView" >&2; exit 1; }
	echo "pre-fix: RED as expected ($slot like-slot failures, legacy texture in 4 themes, identical layout)"
fi
echo "post like tests: all passed"
