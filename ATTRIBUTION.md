# Attribution, modifications and third-party licenses

## Modification notice (GPLv3 section 5a)

OpenTube is a modified version of FourthTube (https://github.com/erievs/FourthTube, base commit
`75a6143a356c5c65af1768e13cd1ccd0d6be775b`, 2026-07-25), which is a fork of ThirdTube
(https://github.com/windows-server-2003/ThirdTube). The OpenTube changes were made in 2026 (v0.16.1 source prepared
2026-09-30). They include a redesigned interface, offline downloads, local liked videos, an updater for this
repository's releases, playback and stability fixes, and the build changes described in
`Documentation/Build Instructions.md`. Files carry no per-file change markers; the upstream history above is the
reference for what changed.

The combined work is licensed under the GNU General Public License v3 or later, as upstream (see `LICENSE`).

## Upstream credits (from FourthTube and ThirdTube)

- [WindowsServer2003](https://github.com/windows-server-2003): creator of ThirdTube.
- [Smu1zel](https://github.com/Smu1zel): found and tested the fix that made the app work again in 2024.
- [NCP 3.0 / erievs](https://github.com/erievs): fixed the app after it stopped working in 2024; FourthTube maintainer.
- [5GBurrito](https://github.com/5GBurrito): minor changes.
- [2b-zipper](https://github.com/2b-zipper): banner, watch history fix, icon, 480p support and more.
- [ItsFrocat](https://github.com/ItsFrocat) and [Dragontwo14](https://github.com/Dragontwo14): German strings.
- [cooolgamer](https://github.com/cooolgamer): French strings and a custom boot screen.
- [Dxni](https://github.com/Icee666): Italian strings.
- [Return YouTube Dislike](https://returnyoutubedislike.com/install): dislike counts.
- Core 2 Extreme: [Video player for 3DS](https://github.com/Core-2-Extreme/Video_player_for_3DS), the basis of the
  playback engine.
- dixy52-beep: in-app textures. [PokéTube](https://github.com/Poketubepoggu): original ThirdTube icon and banner.
- The contributors of [youtube-dl](https://github.com/ytdl-org/youtube-dl) and
  [pytube](https://github.com/pytube/pytube): references for YouTube page parsing.

The boot animation packed into the CIA (`resource/logo.bin`) is the FourthTube one, unchanged.

## Third-party components

Summary for binary users: `NOTICE.md`. License texts: `licenses/`. Upstream source archives and provenance of the
libraries linked from prebuilt archives: `third_party/README.md`. What v0.16.1 was built and linked from:
`Documentation/v0.16.1/`.

| Component | Where | License | Notice |
| --- | --- | --- | --- |
| FFmpeg (3DS fork: windows-server-2003/FFmpeg `125da06c` plus local changes, see `third_party/README.md`) | source `library/FFmpeg/FFmpeg`; the linked archives `library/FFmpeg/lib` were built from that source with `scripts/build_ffmpeg.sh` (configure line in `library/FFmpeg/build.txt`) | LGPL 2.1 or later (configured without `--enable-gpl`, `--enable-version3`, `--enable-nonfree`) | `licenses/FFmpeg-COPYING.LGPLv2.1`, `library/FFmpeg/FFmpeg/LICENSE.md` |
| libcurl 7.82.0 + `library/libcurl/3ds.patch` | linked archive `deps/curl-7.82.0-mbedtls2.28.8/lib/libcurl.a`, built by `scripts/build_curl_diag.sh` from the signed curl.se tarball | curl license | `licenses/curl-7.82.0-COPYING` |
| libbrotli 1.0.9 | prebuilt `library/libbrotli`, source `third_party/sources` | MIT | `licenses/brotli-1.0.9-LICENSE` |
| nghttp2 1.64.0 | prebuilt `library/nghttp2`, source `third_party/sources` | MIT | `licenses/nghttp2-1.64.0-COPYING` |
| rapidjson | `library/rapidjson` | MIT | `library/rapidjson/license.txt` |
| stb_image | `library/stb_image` | MIT / public domain | `library/stb_image/LICENSE` |
| libctru 2.7.0, citro2d 1.7.0, citro3d 1.7.1 | devkitPro packages (the bundled libctru fork in `library/libctru` is not linked into the app; its headers are used by the libcurl recipe) | zlib | `licenses/` |
| Mbed TLS 2.28.8, zlib 1.3.1 | devkitPro portlibs `3ds-mbedtls` 2.28.8-1, `3ds-zlib` 1.3.1-1 | Apache-2.0 OR GPL-2.0-or-later / zlib | `licenses/` |
| 3DS start file `3dsx_crt0` and linker script `3dsx.ld` (devkitarm-crtls 1.2.6) | source `third_party/sources/devkitarm-crtls-1.2.6` | MPL-2.0 | `licenses/MPL-2.0.txt` |
| newlib C library (`libc`, `libm`) and devkitPro `libsysbase` | devkitPro package devkitarm-newlib 4.6.0.20260123-5 (devkitARM r68) | newlib licenses (BSD-style and similar permissive terms) | `licenses/newlib-4.6.0.20260123-COPYING.NEWLIB`, `licenses/newlib-4.6.0.20260123-COPYING.LIBGLOSS` |
| GCC runtime (`libgcc`, `libstdc++`, `crti`, `crtn`, `crtbegin`, `crtend`) | devkitPro package devkitarm-gcc 16.1.0-1 (devkitARM r68) | GPL-3.0-or-later with the GCC Runtime Library Exception 3.1 | `LICENSE` (GPLv3 text), `licenses/gcc-16.1.0-COPYING.RUNTIME` |

Build tools that are not part of the app: makerom v0.19.0 (3DSGuy/Project_CTR) and bannertool 1.2.1
(carstene1ns/3ds-bannertool) package the CIA; they are not included in this repository.

## Art, fonts and sounds

- `romfs/gfx/font/*.t3x`: the glyph textures are inherited unchanged from ThirdTube / FourthTube (same files as in
  both upstream repositories), except `latin_extended_a_font.t3x`, which OpenTube composed from the bundled Latin-1
  glyphs (`scripts/make_hungarian_glyphs.py`). The upstream projects do not name the typeface the glyphs were
  rendered from.
- `romfs/gfx/draw/*`: in-app textures inherited from ThirdTube / FourthTube (ThirdTube credits dixy52-beep for the
  in-app textures); `romfs/gfx/draw/video_player/banner.t3x` comes from FourthTube.
- `resource/banner.cgfx`, `resource/banner.wav`, `resource/banner_legacy.png`, `resource/icon.png` and the boot
  animation `resource/logo.bin` come from FourthTube unchanged (FourthTube credits 2b-zipper for its banner and icon
  and cooolgamer for a custom boot screen). The release CIA packs `resource/logo.bin` and `resource/banner.wav`.
- The HOME Menu icon and banner in `resource/opentube/` were drawn for OpenTube; the wordmark was rendered with the
  Urbanist typeface (SIL Open Font License), which is not included in this repository.
