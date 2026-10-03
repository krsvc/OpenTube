# Data folder move (v0.16.2)

OpenTube 0.16.2 keeps all of its data in `sd:/3ds/opentube/`. Earlier OpenTube releases (0.16.1 and older test
builds) used `sd:/3ds/FourthTubeTest/`. The first start of 0.16.2 copies that folder once and never changes it.
The plain upstream FourthTube build (`make`) still uses `sd:/3ds/FourthTube/` and neither reads nor copies anything.

Status: implemented and tested on a computer (host tests, see below). It has not yet been tested on a console.

## Folders

| Folder | Role |
| --- | --- |
| `sd:/3ds/opentube/` | OpenTube's data folder. Used only when it holds a valid `opentube_data_root.txt`. |
| `sd:/3ds/FourthTubeTest/` | The earlier folder. Only read, and only by the one-time copy. Never changed, moved or deleted. |
| `sd:/3ds/opentube.partial/` | The unfinished copy while it runs. Never used as data. |

`opentube_data_root.txt` is a short text file that OpenTube writes once: format line, `origin=fresh` or
`origin=migrated`, the number of copied files and bytes, and a checksum of those lines. Don't edit or delete it.
Without a valid marker file, OpenTube treats the folder as someone else's and won't start.

## What happens at start-up

The check runs before OpenTube reads any settings, starts any background work or writes any log file:

1. `sd:/3ds/opentube/` has a valid marker: OpenTube starts normally. The old folder isn't looked at again, so a later
   start never copies old data a second time.
2. Neither folder exists: OpenTube creates `sd:/3ds/opentube/` (with its marker) and starts. This is a new install.
3. Only the old folder exists: OpenTube checks it, works out the free space it needs, copies it into
   `sd:/3ds/opentube.partial/` and shows progress. Once every file is written and checked, it renames that folder
   to `sd:/3ds/opentube/`. Only then does the app start. This happens once.
4. Otherwise OpenTube shows a short message saying what's wrong and what to do, and closes when you press A. It
   doesn't change, delete or merge anything, and it never writes into the old folder.

The copy includes settings (with the theme), watch history, subscriptions, local likes, your YouTube login,
downloads with their metadata and media, saved download thumbnails, error reports and diagnostic logs. It also
includes any unrelated files in that folder. It skips one thing: `update/`, which holds the in-app updater's
temporary download. A leftover copy of that file would block later updates.

## Messages and what to do

| Message | Cause | What to do |
| --- | --- | --- |
| `sd:/3ds/opentube/ already exists, but OpenTube did not create it` | A folder or file with that name exists without a valid marker (including an empty folder). | Rename or move it on a computer, then start OpenTube again. |
| `A folder sd:/3ds/opentube.partial/ exists` | That folder exists: an interrupted copy (power loss, the app was closed during the copy, an earlier error) or someone else's folder with the same name. | On a computer, rename it (for example to `opentube.partial.old`) and keep its contents. The next start copies everything again. Remove the renamed folder yourself only once you know what it holds. |
| `Not enough free space ...` | The copy needs more space than is free (the needed and free MB are shown). | Free up space on the SD card. Nothing was written. |
| `... contains something OpenTube can't copy safely` | The old folder holds a link, a special file, an invalid name, or folders nested too deep or too long. | Move that item (named in the message) out of the old folder. |
| `... could not be read` | The SD card returned an error. | Check the SD card on a computer. An unreadable old folder is never treated as missing. |
| `Setting up sd:/3ds/opentube/ failed: ...` | A read, write, flush or close failed during the copy, or the folder couldn't be renamed. | Check the card and its free space. If the message asks you to, rename `sd:/3ds/opentube.partial/` on a computer and keep its contents. |

OpenTube never deletes anything by itself. These messages never lead to an empty or half-filled library.

## Going back to 0.16.1

To roll back, reinstall the 0.16.1 CIA. It uses `sd:/3ds/FourthTubeTest/`, which is exactly as it was before
0.16.2 first started.

Changes you make in 0.16.2 (settings, history, likes, logins, downloads) are **not** copied back to that folder.
Changes made in 0.16.1 after a rollback are not copied forward either. Once `sd:/3ds/opentube/` exists, 0.16.2 always
uses it as it is. The app never removes `sd:/3ds/FourthTubeTest/`; keeping it is what makes the rollback possible.

## Design notes and assumptions (3DS SD card, FAT)

- The copy runs on the main thread before any worker exists. It streams files through one 128 KiB buffer, so memory
  use doesn't depend on file size. Playback and network scheduling are unchanged.
- Each file is created exclusively (`O_CREAT|O_EXCL`), written in full, flushed (`fsync`, which is `FSFILE_Flush` on
  the SD card), closed, and its size is checked. Every return value is checked. The source is re-checked before each
  file. A file that changed size during the copy fails it.
- The copy is published with a single directory rename that never replaces an existing destination:
  `FSUSER_RenameDirectory`. libctru's newlib `rename()` deletes an existing destination and retries, so it isn't
  used here.
- Free space needed = every file rounded up to the card's cluster size, one cluster per folder plus two more,
  plus 1 MiB.
- FAT has no journal. The design assumes flushed and closed files are on the card before the rename, and that the
  rename updates one directory entry. If power is lost before the rename, only `opentube.partial/` remains. A marker
  that's damaged after a crash is refused. **This still has to be confirmed on a console.**
- Limits that block the copy (with a message): more than 10000 entries, folders nested more than 8 deep, a path
  longer than 250 bytes, a file over 2 GiB - 1.

## Tests

Host tests (clang++ only, real files in a fresh folder plus a wrapper that injects faults; nothing is ever deleted):

    TEST_OUT_DIR=<fresh folder> sh tests/host/run_data_root.sh [<previous source tree for RED>]

- `test_data_root.cpp`: new install, full migration with the real download, thumbnail and liked-videos stores loading
  the new folder, no second copy, foreign or unfinished destinations, an interrupted copy and relaunch, the space
  estimate and a full card, read / write / flush / close / open failures, links, special files and invalid names,
  an unreadable vs a missing old folder, a collision when publishing, a large file with bounded memory, rollback
  reads, and message bounds.
- `test_data_root_glue.cpp`: the real `main()` and `Menu_init()` (with the check, its message screen and the early
  exit) extracted from the source and run against recording stand-ins. Nothing that reads app data and no worker
  starts before the check passes. When the check fails, there's no normal shutdown.
- `test_data_root_defs.cpp`: the real `definitions.hpp` for the OpenTube build and the upstream build.

Still to do on a console: a first start with a real 0.16.1 folder (including large downloads), the progress screen,
HOME and power button during the copy, each message screen and exit, all data loading afterwards, rollback to 0.16.1,
and copy durability on FAT.
