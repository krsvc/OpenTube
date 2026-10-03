# Third-party notices for OpenTube binaries

`OpenTube.cia` and `OpenTube.3dsx` contain OpenTube (GPL-3.0-or-later, see `LICENSE`) statically linked with the
components below. The complete corresponding source of the release is this repository at its release tag; see
`ATTRIBUTION.md` for upstream credits and `Documentation/v0.16.1/` for what v0.16.1 was built and linked from.

## FFmpeg (LGPL 2.1 or later)

This software uses libraries from the FFmpeg project (https://ffmpeg.org) under the GNU Lesser General Public License
version 2.1 or later: libavcodec, libavfilter, libavformat, libavutil, libswresample and libswscale. The complete
FFmpeg source used to build them is in `library/FFmpeg/FFmpeg`, the configure options in `library/FFmpeg/build.txt`,
and the steps to rebuild them and relink OpenTube against a modified version in `Documentation/Build Instructions.md`
(`scripts/build_ffmpeg.sh`, `scripts/relink_ffmpeg.sh`). License text: `licenses/FFmpeg-COPYING.LGPLv2.1`.

## Other linked components

| Component | License | Notice |
| --- | --- | --- |
| libcurl 7.82.0 | curl license | `licenses/curl-7.82.0-COPYING` |
| nghttp2 1.64.0 | MIT | `licenses/nghttp2-1.64.0-COPYING` |
| libbrotli 1.0.9 (decoder) | MIT | `licenses/brotli-1.0.9-LICENSE` |
| Mbed TLS 2.28.8 | Apache-2.0 OR GPL-2.0-or-later | `licenses/mbedtls-2.28.8-LICENSE` |
| zlib 1.3.1 | zlib | `licenses/zlib-1.3.1-LICENSE` |
| libctru 2.7.0 | zlib | `licenses/libctru-2.7.0-README.md` |
| citro2d 1.7.0, citro3d 1.7.1 | zlib | `licenses/citro2d-1.7.0-LICENSE`, `licenses/citro3d-1.7.1-LICENSE` |
| rapidjson | MIT | `library/rapidjson/license.txt` |
| stb_image | MIT / public domain | `library/stb_image/LICENSE` |
| devkitarm-crtls 1.2.6 (`3dsx_crt0`, `3dsx.ld`) | MPL-2.0 | `licenses/MPL-2.0.txt`, source in `third_party/sources/devkitarm-crtls-1.2.6` |
| newlib 4.6.0.20260123 (devkitPro build: `libc`, `libm`, `libsysbase`) | newlib licenses | `licenses/newlib-4.6.0.20260123-COPYING.NEWLIB`, `licenses/newlib-4.6.0.20260123-COPYING.LIBGLOSS` |
| GCC 16.1.0 runtime (`libgcc`, `libstdc++`, crt objects) | GPL-3.0-or-later with GCC Runtime Library Exception 3.1 | `LICENSE`, `licenses/gcc-16.1.0-COPYING.RUNTIME` |

Source of the devkitARM r68 runtime: GCC 16.1.0 (https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/) and newlib 4.6.0.20260123
(https://sourceware.org/pub/newlib/), each with devkitPro's patch from https://github.com/devkitPro/buildscripts
(`patches/gcc-16.1.0-1.patch`, `patches/newlib-4.6.0.20260123-5.patch`, commit
`9b57a022648cbd971c2df3652671cc9deaa2482a`). Source of the libbrotli and nghttp2 versions: `third_party/sources/`.
