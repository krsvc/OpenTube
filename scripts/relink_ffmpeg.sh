#!/bin/sh
# Relink OpenTube against a (possibly modified) FFmpeg: rebuild the FFmpeg archives from library/FFmpeg/FFmpeg,
# then build the app from source against them. Run it in a fresh copy of the repository.
# Same environment variables as scripts/build_ffmpeg.sh and scripts/build_release.sh (DRY_RUN=1 for a check only).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build_ffmpeg.sh"
"$ROOT/scripts/build_release.sh"
