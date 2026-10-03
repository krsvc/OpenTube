# OpenTube

An unofficial homebrew YouTube client for the Nintendo 3DS family. v0.16.1 is a **public beta**.

OpenTube is an independent fork of [FourthTube](https://github.com/erievs/FourthTube), which is itself a fork of
[ThirdTube](https://github.com/windows-server-2003/ThirdTube). It keeps their playback engine and adds a redesigned
interface, offline downloads, local liked videos and an in-app updater. It is not affiliated with or endorsed by
YouTube, Google, Nintendo or the FourthTube / ThirdTube maintainers.

## Status: v0.16.1 public beta

This is beta software. Expect crashes and rough edges, and please report them in this repository's Issues. It comes
with no warranty (see [LICENSE](LICENSE)).

Testing on real hardware so far:

- The released CIA was installed on the maintainer's console and judged good in normal use. That was a user
  acceptance check, not a full regression test.
- The previous build was tested in more detail: login, video, audio, seeking and exit all worked. It differs from
  v0.16.1 only in compile-time file paths.
- **Old 3DS:** in one tested video, 144p played smoothly and 240p stuttered. Other videos may behave differently.
  There is no general frame-rate promise. **On Old 3DS / 2DS, use 144p.**
- **New 3DS:** the v0.16.1 interface has not been checked on hardware yet.
- **Updating from inside the app** has not been tested end to end yet (see Updates).

## Installation

Requirements: a 3DS or 2DS with [Luma3DS](https://github.com/LumaTeam/Luma3DS) custom firmware and the DSP firmware
dumped. Modern Luma3DS can [dump it from the Rosalina menu](https://3ds.hacks.guide/finalizing-setup.html);
otherwise use [DSP1](https://github.com/zoogie/DSP1).

1. Download `OpenTube.cia` and `SHA256SUMS` from this repository's
   [Releases](https://github.com/krsvc/OpenTube/releases) page.
2. Check that the SHA-256 of `OpenTube.cia` matches its line in `SHA256SUMS`.
3. Copy the CIA to your SD card and install it with a CIA installer such as FBI.
4. Launch **OpenTube** from the HOME Menu.

Good to know:

- When the app starts, the 3DS shows the **green FourthTube boot animation**. This is expected: this beta keeps the
  original, known-good boot logo.
- Title ID `000400000BF74E00`. Installing replaces earlier "FourthTube Test" / OpenTube development builds that use
  the same ID. It installs next to the original FourthTube app.
- App data lives in `sd:/3ds/FourthTubeTest/` (settings, watch history, subscriptions, liked videos, downloads,
  update staging). This is separate from FourthTube's `sd:/3ds/FourthTube/` folder. Uninstalling the title does not
  delete it.
- `OpenTube.3dsx` (if attached to the release) runs from the Homebrew Launcher. It cannot update itself, so the CIA is
  the recommended install.

## Updates

Settings -> Update checks the latest **stable** release of this repository (`krsvc/OpenTube`) over HTTPS. Before
installing anything, the app downloads the whole CIA, checks its SHA-256 against `SHA256SUMS`, checks the CIA's title
ID and version, and asks twice. It only offers strictly newer versions, so any installed v0.16.1 build (including
development builds that call themselves 0.16.1) reports "up to date" for this release. To move such a build to this
exact release, install the CIA manually. Updating from one published release to a newer one has not been tested end
to end yet. If an in-app update ever fails, install the newer CIA manually.

## Controls

In the video player (tap **?** at the end of the player's tab bar for the in-app list):

| Button | Action |
| --- | --- |
| A | Play / pause |
| D-pad left / right | Back / forward 10 s |
| ZL / ZR | Back / forward 5 s |
| X + B | Stop playback |
| B | Back, or close a sheet |
| Y | Full player (while browsing) |
| START | Menu |

Sheets can also be closed by dragging their top edge down. Everything else uses the touch screen.

## Known limitations

- Beta quality. Performance on Old 3DS depends heavily on the resolution (see above).
- Some characters outside the bundled font blocks (for example some symbols and emoji) are drawn as a `<?>` box.
- This build writes small, bounded diagnostic logs to its data folder (`playback_diag.log`, `fps_diag.log`,
  `freeze_diag.log`). They stay on the SD card and nothing is uploaded.
- The About screen still links to the upstream FourthTube repository, and some internal version strings still read
  "Beta 34.1 Test". The release version is v0.16.1.
- The optional YouTube login is inherited from FourthTube. If you use it, its tokens are stored in the data folder on
  the SD card.
- YouTube changes its private APIs from time to time, and playback can break until the app is updated.

## Building

See [Documentation/Build Instructions.md](Documentation/Build%20Instructions.md). In short: `scripts/build_release.sh`
builds the app, and `scripts/relink_ffmpeg.sh` rebuilds FFmpeg from the included source and relinks the app against
it.

## License

OpenTube is free software under the GNU General Public License, version 3 or (at your option) any later version. See
[LICENSE](LICENSE). Upstream credits and the modification notice are in [ATTRIBUTION.md](ATTRIBUTION.md).
Third-party notices are summarized in [NOTICE.md](NOTICE.md), and the full texts are in [licenses/](licenses).

This software uses libraries from the [FFmpeg](https://ffmpeg.org) project under the GNU Lesser General Public
License, version 2.1 or later. Their complete source and the steps to rebuild them and relink OpenTube are included
(`library/FFmpeg/`, `Documentation/Build Instructions.md`).
