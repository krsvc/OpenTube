# Third-party sources and license notices

This directory holds upstream sources for components that the release build links but that are not built from the
rest of this tree: two libraries linked from prebuilt archives in `library/`, the devkitARM 3DS start file, the
devkitPro 3DS libraries the release links (libctru, citro2d, citro3d, Mbed TLS, zlib) with their package recipes, and
the devkitPro patches and recipes of the devkitARM r68 runtime (newlib, GCC). License notices of all linked
third-party components are in `../licenses/` and summarized in `../NOTICE.md`. Every file here is an unmodified
upstream download; the digests below were checked when the files were added (2026-09-30 and 2026-10-03).

## Source archives (`sources/`)

| File | Upstream | Identity | SHA-256 |
| --- | --- | --- | --- |
| `brotli-1.0.9-e61745a6b7add50d380cfd7d3883dd6c62fc2c71.tar.gz` | GitHub archive of google/brotli commit `e61745a6b7add50d380cfd7d3883dd6c62fc2c71` (the commit tag `v1.0.9` points to; the v1.0.9 release has no downloadable assets) | all 135 files match that commit's git blobs; the archive leaves out what upstream's `.gitattributes` marks `export-ignore` (tests, other language bindings, fuzzers, `c/common/dictionary.bin*`) | `280a7d0963ff6302f003c0bb4f414405924e1393ca70d5c61cc30d98ddaf2f49` |
| `nghttp2-1.64.0.tar.xz` | release asset of nghttp2/nghttp2 v1.64.0 (tag object `6315808d…`, commit `526ff38e0249acbcc4d0e8958c12cdeae9960cfe`) | digest equals the release's `checksums.txt`; the OpenPGP signature verifies with key `F4F3 B914 74D1 EB29 889B D0EF 7E84 03D5 D673 C366` (Tatsuhiro Tsujikawa, signing subkey `5339A2BE82E07DEC`) | `88bb94c9e4fd1c499967f83dece36a78122af7d5fb40da2019c56b9ccc6eb9dd` |
| `nghttp2-1.64.0.tar.xz.asc` | release asset, detached signature of the tarball | | `f3656742bc9ac56a0f31de10516b994397362f67f11a99b798d62e52b416ee34` |
| `nghttp2-1.64.0-checksums.txt` | release asset `checksums.txt` of v1.64.0, renamed | | `bb7b5b27996c68854f8e050a00b2f2ad7286b9839d3c5d7ea34cfc37be27c1f3` |
| `devkitarm-crtls-1.2.6/3dsx_crt0.s` | devkitPro/devkitarm-crtls tag `v1.2.6` (lightweight tag, commit `9788af74f0f3cfd903cffdc903d514fc073a0f95`) | git blob `c1adc143585352025a1bffd9e8385d5844daaed0`; MPL-2.0 header | `0cd0169dd5b711a2adf5efefe57134afff22a70b54e51cc4a87e608fd11071a8` |
| `devkitarm-crtls-1.2.6/3dsx.ld` | same tag; linker script used through `3dsx.specs` | git blob `3c9a6fc135a29c357801c5af7dd2bc1b6e780076`; MPL-2.0 header | `930b274d551cb4b61718c09d8bfbc76212f5f574152f43937687984b916508aa` |
| `devkitarm-crtls-1.2.6/3dsx.specs` | same tag; GCC specs selected by `-specs=3dsx.specs` | git blob `a3a9e117ebdf46948e74e6859b2ffbbb8e27a8c8` | `2663fca222c4a97bd51fae61a19f95bdabc068000685997ecbe7b11a6675f84b` |
| `devkitarm-crtls-1.2.6/Makefile` | same tag; build recipe of the start files | git blob `b4d977f155792f7e61c0c39784665bf649f3439a` | `cf0bbc61387a4ac7cb5eeb7536be8e0caeefa21e8d062922c42d348c19a73109` |

Neither archive contains absolute paths, `..` entries or links. The nghttp2 tarball includes upstream's own test TLS
keys (`tests/testdata/privkey.pem`, `integration-tests/server.key`, `integration-tests/alt-server.key`); they are
public test fixtures of that project, not OpenTube secrets, and stay in so the archive matches its signature.

### How they relate to the prebuilt archives

- `library/libbrotli/lib/libbrotli.a` was built by the FourthTube project from libbrotli v1.0.9 with devkitARM
  release 57 (`library/libbrotli/build.txt`). The four headers in `library/libbrotli/include/brotli` are identical to
  `c/include/brotli` in this archive. OpenTube links 9 decoder members (bit_reader, constants, context, decode,
  dictionary, huffman, platform, state, transform).
- `library/nghttp2/lib/libnghttp2.a` was built from nghttp2 v1.64.0 with devkitARM release 65 and
  `library/nghttp2/3ds-configure.sh` (`library/nghttp2/build.txt`). `nghttp2.h` and `nghttp2ver.h` in
  `library/nghttp2/include` are identical to the release tarball's. OpenTube links 22 of its 26 members.
- A rebuild from these archives with devkitARM r68 and the recipes above produced archives with the same members and
  the same exported symbols per member. The prebuilt binaries themselves were **not** reproduced bit for bit, so
  treat these archives as the upstream source of those versions, not as proof of the exact binary build.
- Recipe notes: set `FOURTHTUBE_PATH` in `3ds-configure.sh` to this repository; on macOS pass
  `AR=arm-none-eabi-ar` to the brotli `make` (the host `ar` cannot index ARM objects).

### devkitARM start file (`sources/devkitarm-crtls-1.2.6/`)

The release build links `3dsx_crt0.o` from the devkitPro package `devkitarm-crtls` 1.2.6-1 (its `.crt0` section,
148 bytes at 0x100000, plus `initSystem` / `exit` references) and uses that package's `3dsx.specs` and `3dsx.ld`. No
other object of that package is linked; `crti`, `crtn`, `crtbegin` and `crtend` come from GCC. The installed files
match the package's recorded digests; `3dsx.ld` and `3dsx.specs` are byte-identical to the tag's files, and
assembling the tag's `3dsx_crt0.s` with the Makefile's rule (`arm-none-eabi-gcc -march=armv6k -mfloat-abi=hard -c`,
devkitARM r68) gives an object bit-identical to the installed `3dsx_crt0.o`. In the release ELF the 148 bytes differ
from that object only in its 7 relocated words. The tag has no license file; the files carry an MPL-2.0 header, and
the full MPL 2.0 text is `../licenses/MPL-2.0.txt`.

### FFmpeg (in-tree, `library/FFmpeg/`)

FFmpeg is not in this directory: its source is `library/FFmpeg/FFmpeg`, its license texts are in that directory
(`COPYING.LGPLv2.1`, `LICENSE.md`; copy in `../licenses/FFmpeg-COPYING.LGPLv2.1`), and the archives linked by the
release are `library/FFmpeg/lib`. Checked 2026-10-03:

- The in-tree source is windows-server-2003/FFmpeg (the FFmpeg fork used by ThirdTube / FourthTube) at commit
  `12b6c739b408cc10ae42ed9231a6085841cbcbbf`, except 5 FATE test reference files under `tests/ref/` that are not
  present (they are not used by the build). Compared with that fork's base `125da06c6a2cbc35945782b76097f45d0472cfaf`
  ("Patch to enable pthread", 2023-11-02) five files differ, each from a public commit of that repository:

  | File | Commit | Author, date | Change |
  | --- | --- | --- | --- |
  | `libavutil/pixfmt.h`, `libavutil/tx.h` | `525e9d7d85c1` "More fixes regarding enum size" | windows-server-2003, 2024-03-12 | adds `AV_PIX_FMT_TERMINATING` / `AV_TX_TERMINATING = 0x7FFFFFFF` (32-bit enums) |
  | `libavutil/tx_template.c` | `60fdd4d55f64` "Reduce memory usage" | windows-server-2003, 2024-03-12 | removes the FFT tables and codelets above 131072 points (reverts FFmpeg commit 8f48a62) |
  | `configure` | `e00d60de28b8` "Hack to fix building FFmpeg on devkitARM r64+." | Smu1zel, 2024-12-21 | adds `check_cflags -Wno-error=incompatible-pointer-types` |
  | `doc/t2h.pm` | `12b6c739b408` "doc/html: support texinfo 7.0" | Frank Plowman, 2023-11-08 | Texinfo 7.0 support for the HTML documentation |

  Each of these five files (and the installed copies of `pixfmt.h` and `tx.h` in `library/FFmpeg/include`) ends
  with a comment that records this change, as LGPL 2.1 section 2(b) asks. The comment was added for publication and
  does not change the build output; the digests of the files as compiled are in `Documentation/v0.16.1/README.md`.
  OpenTube made no further change to the FFmpeg source.
- The six archives in `library/FFmpeg/lib` were built from this source by `scripts/build_ffmpeg.sh` (configure line of
  `library/FFmpeg/build.txt` with `--prefix=..`) with devkitARM r68. Three separate builds (two while preparing the
  release, one from this published tree with the published script) produced byte-identical archives; their SHA-256
  are in `Documentation/v0.16.1/LINKED_ARCHIVES.txt`. The headers in `library/FFmpeg/include` are the matching
  `make install` output, byte-identical to the source files.
- Each archive reports its license as "LGPL version 2.1 or later" and embeds the configure line (no `--enable-gpl`,
  `--enable-nonfree` or `--enable-version3`).

### devkitPro 3DS libraries (`sources/devkitpro-3ds-libraries/`)

The release links these installed devkitPro packages. The archives here are the exact source downloads named by the
packages' recipes (`recipes/*.PKGBUILD`, from https://github.com/devkitPro/pacman-packages at commit
`f103fe88e37180ecd0b7a9173b52b7580a54f71a`); each matches the recipe's SHA-256.

| Package (installed) | Source | SHA-256 | Recipe last changed | Correspondence checked |
| --- | --- | --- | --- | --- |
| libctru 2.7.0-1 | `libctru-2.7.0.tar.gz` (github.com/devkitPro/libctru tag `v2.7.0`) | `c0b8a5048e15a0ae02772e6db0188f5571e8c26d064d7962212a40681f0d18b3` | `f0075bc1f978` | 89/89 installed headers identical to the tag |
| citro2d 1.7.0-1 | `citro2d-1.7.0.tar.gz` (tag `v1.7.0`) | `f311d1decb64668980b7a7a917a9b936a40025265df921e251f5f0537b94b86b` | `5305c7004e1c` | 6/6 installed headers identical |
| citro3d 1.7.1-2 | `citro3d-1.7.1.tar.gz` (tag `v1.7.1`) | `c920515a791442e81e0c2c8867dde81bf14aca17eacb2709b4a3a3c61699250c` | `bd937347d7e4` | 17/17 installed headers identical |
| 3ds-mbedtls 2.28.8-1 | `mbedtls-2.28.8.tar.gz` (github.com/Mbed-TLS/mbedtls tag `v2.28.8`) + devkitPro's `mbedtls-2.28.8.patch` (3DS entropy source) | `4fef7de0d8d542510d726d643350acb3cdb9dc76ad45611b59c9aa08372b4213`, patch `84892ec63ab68804a582364d8c4bd6c8dfd0d697ed175bddcd5b7339cce35563` | `eb53bd49cb75` | 96/96 installed headers match: 95 identical to the patched tag, `config.h` reproduced byte for byte by the recipe's `config.pl` steps |
| 3ds-zlib 1.3.1-1 | `zlib-1.3.1.tar.xz` (+ upstream `.asc` signature; github.com/madler/zlib release v1.3.1) | `38ef96b8dfe510d42707d9c781877914792541133e1870841463bfa73f883e32` | `3d222f5a92c7` | `zlib.h` identical; `zconf.h` differs only in the two lines zlib's `./configure` rewrites (`HAVE_UNISTD_H`, `HAVE_STDARG_H`) |

The bundled libctru fork in `library/libctru` is a different, older libctru; it is not linked into the release.

### devkitARM r68 runtime (`sources/devkitarm-r68-runtime/`)

The release links newlib (`libc`, `libm`, `libsysbase`) from devkitarm-newlib 4.6.0.20260123-5 and the GCC runtime
(`libgcc`, `libstdc++`, `crti`, `crtn`, `crtbegin`, `crtend`) from devkitarm-gcc 16.1.0-1. devkitPro builds them from
the official upstream releases plus its own patches:

- newlib: `newlib-4.6.0.20260123.tar.gz` from https://sourceware.org/pub/newlib/ (SHA-256
  `6ff27e3bf022666f43f7802255be680eeff722ac181b1725d21e2e8318604ee3`, matches sourceware's `sha512.sum` and the
  recipe) plus `newlib-4.6.0.20260123-5.patch` (adds devkitPro's `libsysbase`; SHA-256
  `44edc33674524010c3e8469b6b0cbbc41dbe4c5f00b2a648950b2ab91c3d13fb`, identical at buildscripts tags `devkitARM_r67.2`
  and `devkitARM_r68` and in the recipe `devkitarm-newlib.PKGBUILD`).
- GCC: `gcc-16.1.0.tar.xz` from https://ftp.gnu.org/gnu/gcc/gcc-16.1.0/ (102 MB with its `.sig`, too large for this
  repository; git tag `releases/gcc-16.1.0` of https://gcc.gnu.org/git/gcc.git) plus `gcc-16.1.0-1.patch` (SHA-256
  `7b39aeadf373327bf6671e1bc3596c864eeb38a92a0e3dd553ddd7f385869981`).
- Recipes: `buildscripts-*.sh` from https://github.com/devkitPro/buildscripts tag `devkitARM_r68`
  (commit `5be6f7a1214ec8d68054b2d0b6023aeb4ea56667`); `select_toolchain.sh` there pins GCC 16.1.0-1 and newlib
  4.6.0.20260123-5, the installed versions.

Notices: `../licenses/newlib-4.6.0.20260123-COPYING.NEWLIB`, `../licenses/newlib-4.6.0.20260123-COPYING.LIBGLOSS`
(libsysbase is added under newlib's `libgloss/`; most of its files carry no license header, `fnmatch.c` is
BSD-licensed), `../licenses/gcc-16.1.0-COPYING.RUNTIME` (GCC Runtime Library Exception 3.1) and `../LICENSE` (the
GPLv3 text, byte-identical to GCC's `COPYING3`).

## License notices (`../licenses/`)

| File | Component and version linked by the release build | Origin | License |
| --- | --- | --- | --- |
| `brotli-1.0.9-LICENSE` | libbrotli 1.0.9 | `LICENSE` in the brotli archive above | MIT |
| `nghttp2-1.64.0-COPYING` | nghttp2 1.64.0 | `COPYING` in the nghttp2 tarball above (`lib/sfparse.c` carries its own MIT header) | MIT |
| `curl-7.82.0-COPYING` | libcurl 7.82.0 | `COPYING` in the signed curl-7.82.0 release tarball | curl |
| `mbedtls-2.28.8-LICENSE` | Mbed TLS 2.28.8 (devkitPro `3ds-mbedtls` 2.28.8-1) | `LICENSE` at Mbed-TLS/mbedtls tag `v2.28.8` (commit `5a764e5555c64337ed17444410269ff21cb617b1`) | Apache-2.0 OR GPL-2.0-or-later |
| `zlib-1.3.1-LICENSE` | zlib 1.3.1 (devkitPro `3ds-zlib` 1.3.1-1) | `LICENSE` at madler/zlib tag `v1.3.1` (commit `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf`) | zlib |
| `citro2d-1.7.0-LICENSE` | citro2d 1.7.0 | `LICENSE` at devkitPro/citro2d tag `v1.7.0` (commit `147b02aae021da61b1f620446ad2892ecc45411e`) | zlib |
| `citro3d-1.7.1-LICENSE` | citro3d 1.7.1 | `LICENSE` at devkitPro/citro3d tag `v1.7.1` (commit `9f21cf7b380ce6f9e01a0420f19f0763e5443ca7`) | zlib |
| `MPL-2.0.txt` | devkitarm-crtls 1.2.6 (`3dsx_crt0.o`, `3dsx.ld`) | plain-text MPL 2.0 from mozilla.org (`https://www.mozilla.org/media/MPL/2.0/index.txt`), same wording as the license at https://www.mozilla.org/en-US/MPL/2.0/ apart from list markers | MPL-2.0 |
| `libctru-2.7.0-README.md` | libctru 2.7.0 (the release build links the devkitPro package, not `library/libctru`) | `README.md` at devkitPro/libctru tag `v2.7.0` (commit `36fe1ada5b7ebe53ba4decda36d764a55f8fefb6`); that tag has no separate license file, the license is in the README | zlib |

| `newlib-4.6.0.20260123-COPYING.NEWLIB`, `newlib-4.6.0.20260123-COPYING.LIBGLOSS` | newlib 4.6.0.20260123 (devkitarm-newlib 4.6.0.20260123-5: `libc`, `libm`, `libsysbase`) | `COPYING.NEWLIB` / `COPYING.LIBGLOSS` in the official newlib-4.6.0.20260123 tarball | newlib licenses |
| `gcc-16.1.0-COPYING.RUNTIME` | GCC 16.1.0 runtime (`libgcc`, `libstdc++`, crt objects) | `COPYING.RUNTIME` at gcc.gnu.org tag `releases/gcc-16.1.0` | GCC Runtime Library Exception 3.1 (with GPLv3, `../LICENSE`) |
| `FFmpeg-COPYING.LGPLv2.1` | FFmpeg | copy of `library/FFmpeg/FFmpeg/COPYING.LGPLv2.1` | LGPL 2.1 |

In-tree notices: FFmpeg (`library/FFmpeg/FFmpeg/COPYING.LGPLv2.1`, `LICENSE.md`), rapidjson
(`library/rapidjson/license.txt`), stb_image (`library/stb_image/LICENSE`), the bundled libctru fork
(`library/libctru/LICENSE.txt`).
