# OpenTube v0.16.2

## Changes

- Moved app data to `sd:/3ds/opentube/`. The first launch copies the earlier OpenTube data folder and keeps the original. See [the migration notes](../Data%20root%20migration.md) for space requirements, interrupted copies and rollback.
- Delayed normal HOME/suspend callback registration until startup and the base CPU limit are initialized. This fixes an Old 3DS startup lifecycle defect during the data-folder check.
- Replaced the old like texture in the shared comment/reply/community-post painter with the existing vector icon. Counts and interaction behavior are unchanged.

## Exact release files

- `OpenTube.cia`: 5170112 bytes, SHA-256 `2fc7fd967b6a549a13863268af51394b6dbd1db207bfb0854b9f7ec14f2875b2`
- `OpenTube.3dsx`: 7096500 bytes, SHA-256 `76afd3f44f67df143ae154173f6bc29ed1765e1876977fb65bbb1830813fd35d`
- Build ID: `nogit-5b4bac8bb94c`
- Title ID and HOME jump ID: `000400000BF74E00`
- Title version: `16386`
- Product code: `CTR-TYTD`

The files have the same title ID as the earlier OpenTube releases, so installing the CIA updates that title.
The previous release remains available at [v0.16.1](https://github.com/krsvc/OpenTube/releases/tag/v0.16.1).
Reinstalling it restores the earlier app code, not changes made to your data after upgrading.

## Source and build route

[BUILD_INPUTS.sha256](BUILD_INPUTS.sha256) records all 472 project-local build inputs for these exact binaries.
Every recorded input matches this tag. README, screenshots, release documentation and host tests are outside
that compiled-input set. The build-time string is compiled in, so a fresh rebuild need not be byte-identical.

The FFmpeg archives, their annotated source, other dependencies, packaged media and toolchain are unchanged from
v0.16.1. Its [dependency inventory](../v0.16.1/LINKED_ARCHIVES.txt), [source publication notes](../v0.16.1/README.md),
[NOTICE.md](../../NOTICE.md) and included licenses still apply. Use the [build instructions](../Build%20Instructions.md)
to compile the app or rebuild FFmpeg and relink OpenTube. That library rebuild/relink route was exercised during
v0.16.1 preparation; v0.16.2 adds a fresh full application build with the same library inputs.

## Verification and limits

Focused host checks passed:

- Data-folder helper: 278 checks, no failures, using real filesystem fixtures and injected errors.
- Startup and HOME/suspend lifecycle glue: 366 checks across 11 scenarios, no failures.
- Shared comment painter: 88 checks across all four themes, no failures.
- The preserved pre-fix source fails the expected icon and startup regression assertions.
- The fresh target build and package identity, application binary interface, heap split and linear-lock checks passed.

These results do not establish a full historical host-suite pass. Older runner stub/driver gaps and an inherited
normal-exit CPU-limit issue remain outside this repair. The migration's FAT durability, HOME/suspend behavior,
new icons and update between public releases still require real-console verification. Version 0.16.1 had the
maintainer's bounded hardware acceptance; it does not automatically carry over to this binary.

The README gallery uses real console captures taken before these fixes. It documents the interface, not hardware
acceptance of this release. Placeholder host renders were not used in that gallery.
