# Building OpenTube

Release binaries are built with the Makefile's `diag` target. `scripts/build_release.sh` runs it without needing
git and then checks the result. The plain `make` target is the upstream FourthTube build; releases do not use it.

## Requirements

The v0.16.1 and v0.16.2 releases were built with these devkitPro pacman packages:

- devkitARM r68-1: devkitarm-gcc 16.1.0-1, devkitarm-binutils 2.46.0-1, devkitarm-newlib 4.6.0.20260123-5,
  devkitarm-crtls 1.2.6-1, devkitarm-rules 1.6.0-4
- libctru 2.7.0-1, citro2d 1.7.0-1, citro3d 1.7.1-2
- 3ds-mbedtls 2.28.8-1, 3ds-zlib 1.3.1-1
- tex3ds 2.3.0-4, 3dstools 1.3.1-3, picasso 2.7.2-3, general-tools 1.4.4-1

It also needs two packaging tools that devkitPro does not ship. Build them for your host from source:

- makerom v0.19.0 (tag `makerom-v0.19.0` of https://github.com/3DSGuy/Project_CTR)
- bannertool 1.2.1 (tag `1.2.1` of https://github.com/carstene1ns/3ds-bannertool)

Put both on `PATH`, or pass `MAKEROM=/path/to/makerom` and `BANNERTOOL=/path/to/bannertool`. They only package
the CIA. Their code does not end up in the app.

You also need Python 3 for the binary checks, and GNU make. For the optional libcurl rebuild you need `gpg`, `xz`,
`curl` and `patch`.

The scripts use `DEVKITPRO=/opt/devkitpro` and `DEVKITARM=$DEVKITPRO/devkitARM` unless you set them yourself.
They install nothing, never run `make clean`, and run at most `JOBS` (default 3) compile jobs. Each script accepts
`DRY_RUN=1`, which checks the prerequisites and prints the commands without building anything.

## 1. Build the app

    scripts/build_release.sh

This checks the prerequisites and runs `make diag` in a fresh object directory (`DIAG_BUILD`, which must not exist
yet). The results go to `release-out/<date>-<build id>/`: `OpenTube.cia`, `.3dsx`, `.elf`, `.smdh`, the link map,
`BUILD_INPUTS.sha256` (the hashes of everything the app build reads from this tree) and `SHA256SUMS`. The script then
runs four checks on the binaries and exits non-zero if any of them fails:

- `scripts/check_abi_pairing.py`: libctru and newlib come from the same installed toolchain
- `scripts/inspect_cia.py`: title ID `000400000BF74E00`, version and package structure
- `scripts/check_heap_split.py`: the heap split
- `scripts/check_linear_lock.py`: the linear-heap lock wrappers

The app's own objects are built with `-fmacro-prefix-map=<repository>=/src/opentube` (in the Makefile), so the
local build path does not end up in the binary.

The build id compiled into the app is `nogit-` plus the first 12 hex digits of the SHA-256 of `BUILD_INPUTS.sha256`.
You can override it with `OPENTUBE_BUILD_ID`. The v0.16.1 release has the build id `nogit-c5013471390c`, which is the
SHA-256 of `Documentation/v0.16.1/SOURCE_MANIFEST.sha256` (see the README in that folder).

The CIA title version comes from `OPENTUBE_VERSION_MINOR` / `OPENTUBE_VERSION_MICRO` in the Makefile
(title version = MINOR << 10 | MICRO; v0.16.1 = 16385, v0.16.2 = 16386).

Host tests for the v0.16.2 data folder move (clang++ only, no devkitARM) are described in
[Data root migration.md](Data%20root%20migration.md):

    TEST_OUT_DIR=<fresh folder> sh tests/host/run_data_root.sh

Keep the test output path short because the migration helper limits complete paths to 250 bytes. The comment-icon
regression builds the real shared PostView painter against a draw recorder:

    TEST_OUT_DIR=<another fresh folder> sh tests/host/run_post_like.sh

The test binary reports its own exit status. To check it directly after the runner:

    <another fresh folder>/green/test_post_like

For the exact v0.16.2 compiled-input hashes and verification limits, see [v0.16.2/README.md](v0.16.2/README.md).

## 2. FFmpeg (included as source and as built archives)

OpenTube links FFmpeg statically from `library/FFmpeg/lib`. These six archives (`libavcodec`, `libavfilter`,
`libavformat`, `libavutil`, `libswresample`, `libswscale`) were built from the source in `library/FFmpeg/FFmpeg` with:

    scripts/build_ffmpeg.sh

The script runs FFmpeg's `./configure` with the options in `library/FFmpeg/build.txt` (using `--prefix=..`), then
`make` and `make install`. That replaces `library/FFmpeg/lib` and `library/FFmpeg/include`. It builds inside the
FFmpeg source tree, so use a fresh copy of the repository. For v0.16.1, two separate builds with devkitARM r68 on
macOS arm64 produced byte-identical archives, and those are the archives in this repository. Their SHA-256 hashes are
in `Documentation/v0.16.1/LINKED_ARCHIVES.txt`, and the files `configure` generated are in
`Documentation/v0.16.1/ffmpeg-generated/`.

FFmpeg is configured without `--enable-gpl`, `--enable-version3` and `--enable-nonfree`. Each archive reports the
license "LGPL version 2.1 or later".

## 3. Relinking OpenTube with a modified FFmpeg

1. Change the FFmpeg source in `library/FFmpeg/FFmpeg`.
2. Run `scripts/relink_ffmpeg.sh` in a fresh copy of the repository. It runs `scripts/build_ffmpeg.sh` and then
   `scripts/build_release.sh`.

All of OpenTube's own code is compiled from this repository and linked against your archives. The result is a new
`OpenTube.cia` / `.3dsx`. This route was run end to end with the unmodified source when v0.16.1 was prepared. The
result passed all four checks. It is not byte-identical to the release (the build time is compiled in).

## 4. Optional: libcurl

`deps/curl-7.82.0-mbedtls2.28.8/lib/libcurl.a` is the libcurl archive that was linked into v0.16.1. To rebuild it:

    scripts/build_curl_diag.sh

This script downloads the official `curl-7.82.0.tar.xz` and its signature from curl.se and checks the signature
against Daniel Stenberg's key (`27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2`). It then applies
`library/libcurl/3ds.patch` and builds against the installed 3ds-mbedtls, the bundled nghttp2 and libbrotli, and the
headers of `library/libctru`. Expected tarball SHA-256:
`0aaa12d7bd04b0966254f2703ce80dd5c38dbbd76af0297d3d690cdce58a583c`.

libbrotli 1.0.9 and nghttp2 1.64.0 are linked from the archives in `library/`. Their upstream sources and recipes are
described in `third_party/README.md`.

## Reproducibility

Builds are not bit-identical: the build time is compiled in. Rebuilding from this source gives the same title ID,
version, RomFS, icon, banner and boot logo as the release. The app code differs by the build time and, unless you
set `OPENTUBE_BUILD_ID`, by the build id.

## Other scripts

- `scripts/build_diag.sh`: the older, git-based build wrapper (build id from `git rev-parse`)
- `scripts/make_opentube_branding.py`: HOME Menu icon and banner in `resource/opentube/` (needs Pillow and the
  Urbanist font, which is not included)
- `scripts/make_hungarian_glyphs.py`: `romfs/gfx/font/latin_extended_a_font.t3x`, made from the bundled Latin-1 glyphs
- `scripts/make_update_roots.py`: the updater's certificate bundle `romfs/cert/update_roots.pem`
