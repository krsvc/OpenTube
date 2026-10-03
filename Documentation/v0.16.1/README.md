# OpenTube v0.16.1: build record

This folder records what the released v0.16.1 binaries were built from. Release asset hashes:

    0228444a589afbe725dd165e159b6f44b8fdeef161b074e4fe5ecebe5b28856a  OpenTube.cia   (5125056 bytes)
    d8b0c947c490bb11a53092808444c0369bded84a3fa9235048b939ae7ef85dad  OpenTube.3dsx  (7018836 bytes)

- `SOURCE_MANIFEST.sha256`: SHA-256 of every file in the source tree the release was built from (8836 files,
  FFmpeg's in-tree build intermediates excluded). Its own SHA-256 is
  `c5013471390cd92b3e98c387c635431320a6e500c7ef0f41afef630f5d9e23c9`, and its first 12 hex digits are the build id
  compiled into the app (`nogit-c5013471390c`).
- `COMPILED_INPUTS.sha256`: every file the compilers, the linker and the packager actually read, taken from the
  dependency files of the app and FFmpeg builds, the link map and the packaging step. `work/` is the root of the
  source tree. `/opt/devkitpro/` paths are the installed devkitPro packages.
- `LINKED_ARCHIVES.txt`: the archives in the link map, with SHA-256 and toolchain.
- `IDENTITY.txt`: title ID, version and structure of the released CIA and 3DSX (`scripts/inspect_cia.py`).
- `ffmpeg-generated/`: the 11 files that FFmpeg's `configure` generated for this build (`config.h`, the component
  lists, `avconfig.h`, `ffversion.h`). `scripts/build_ffmpeg.sh` regenerates them.
- `PUBLICATION_CHANGES.tsv`: how this published tree differs from `SOURCE_MANIFEST.sha256`. Every compiled, linked
  and packaged input is unchanged, except the comment-only FFmpeg modification notices described below. The other
  differences are documentation, license notices, third-party source archives, new build scripts, and four groups of
  removed files that the build does not read: old bundled makerom / bannertool executables, the `pkgconfig` and
  `share/` output of FFmpeg's `make install`, and two libtool `.la` files.

## Comment-only FFmpeg modification notices (LGPL 2.1 section 2(b))

Five FFmpeg files differ from the fork's base commit (see `third_party/README.md`). For publication, each of them
got a comment block appended at the end of the file that names the public commit, author and date of the change.
The installed copies of `pixfmt.h` and `tx.h` in `library/FFmpeg/include` got the same block. Nothing before the
appended block changed, so all existing line numbers stay the same.

`SOURCE_MANIFEST.sha256` and `COMPILED_INPUTS.sha256` keep the digests of the files as they were compiled. The
published files are those exact bytes followed by the comment block. Removing the appended bytes (truncating the
file to the original size below) gives back the compiled file.

| File | Compiled into v0.16.1 | Original SHA-256 (as compiled), bytes | Published SHA-256, bytes |
| --- | --- | --- | --- |
| `library/FFmpeg/FFmpeg/configure` | no (build recipe) | `369ff413c81a66cbcb678c08ba7796c3535a46d592e4c8aee5f3e3e2aaacfbee`, 277612 | `403544b4ed42b1ee6045e3f6900918cbd79a90674f10457fadd9a4927b7569ca`, 278047 |
| `library/FFmpeg/FFmpeg/doc/t2h.pm` | no (documentation tool) | `cdc6046c84d5ef00764a1cb47ba17812491f6564724f58e3ae1e8c15f81c77a7`, 14713 | `16be3c68cd57883107733695509cd0f4b3d66ee796c0360c292d413a05cfcee7`, 15190 |
| `library/FFmpeg/FFmpeg/libavutil/pixfmt.h` | yes | `dcf362faa0b7c9e63aff3b51d0717b81f7350a776f16397ff6fc99af9f4e5ac8`, 41608 | `0a65cd0a5291de3a4f4297fea3857a4372109430343e91022c5456d0b7846ae2`, 42065 |
| `library/FFmpeg/FFmpeg/libavutil/tx.h` | yes | `28e2e254fb798a5cca996e15a920e1df188fae9e34c7891d3629d2fe92a41e2b`, 7174 | `eaaa5c0f91779dbbcfbf970758dc69c350ddc746c7928fb44120a4fccd716577`, 7626 |
| `library/FFmpeg/FFmpeg/libavutil/tx_template.c` | yes | `76469fac82beaa30bab76fce61de2de7ff0b6609dc3a095cf93d4edc2717254e`, 90617 | `ef1f164891c25c6c0bf232e1016b8d8874754e98a1f5ee127537ee23e70b71d3`, 91094 |
| `library/FFmpeg/include/libavutil/pixfmt.h` | yes (read by the app) | `dcf362faa0b7c9e63aff3b51d0717b81f7350a776f16397ff6fc99af9f4e5ac8`, 41608 | `0a65cd0a5291de3a4f4297fea3857a4372109430343e91022c5456d0b7846ae2`, 42065 |
| `library/FFmpeg/include/libavutil/tx.h` | no (installed, not read by the app) | `28e2e254fb798a5cca996e15a920e1df188fae9e34c7891d3629d2fe92a41e2b`, 7174 | `eaaa5c0f91779dbbcfbf970758dc69c350ddc746c7928fb44120a4fccd716577`, 7626 |

These comments do not change the build output. With the annotated files, `scripts/build_ffmpeg.sh` produced the
same six archives byte for byte (the SHA-256 in `LINKED_ARCHIVES.txt`), and `scripts/build_release.sh` produced the
same app objects as a build from the original files, except `scenes/about.o`, which contains the build time and differs between any two builds. The
v0.16.1 release binaries were built from the original files and were not rebuilt.
