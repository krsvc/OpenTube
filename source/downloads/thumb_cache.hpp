#pragma once
// OpenTube r14: thumbnails of downloaded videos, kept OUTSIDE the media item folders in a separate app-owned cache.
//
// Layout (all names generated here):
//   <app data root>/download_thumbs/                    the cache folder (DEF_MAIN_DIR + "download_thumbs/")
//   <app data root>/download_thumbs/owner.txt           ownership marker, exact content OWNER_TEXT
//   <app data root>/download_thumbs/<item id>.<m>.jpg   thumbnail of one library item (validated JPEG, bounded)
//   <app data root>/download_thumbs/<item id>.<m>.tmp   next thumbnail, written completely and synced before the rename
// <m> is the 3 hex digit mask of the upper-case letters of the video id, so two ids that differ only in case never
// share a file on the case-insensitive FAT card.
//
// Ownership: the cache folder is this app's only if the app created it itself (claim: it must not exist before; the
// marker is written right after) or it is a real folder holding the exact marker. A folder name, a file name or a
// valid JPEG never counts. A cache folder that is not owned (a file, a symlink, a folder without / with another marker)
// disables thumbnails for the run: nothing in it is read, written or deleted. Inside an owned folder only this app's
// generated names are written / replaced / removed, and only when they are regular files; other entries are never
// touched (they only count towards the entry bound).
//
// Bounds: at most MAX_THUMB_BYTES per image and MAX_THUMB_ENTRIES entries in the folder (about 16 MiB). Thumbnails are
// never part of an item's state: a missing / corrupt / refused one only means the neutral tile. The media library
// (download_store) is not changed by any of this, so older builds read, delete and re-download the same library; a
// cache file left behind by an older build's delete never blocks a new media folder (it lives elsewhere).
#include <cstddef>
#include <string>
#include "downloads/download_store.hpp"

namespace downloads {

constexpr size_t MAX_THUMB_BYTES = 64 * 1024;
constexpr int MAX_THUMB_W = 480, MAX_THUMB_H = 360;
constexpr size_t MAX_THUMB_ENTRIES = 256;
extern const char *const THUMB_OWNER_FILE;
extern const char *const THUMB_OWNER_TEXT;

bool jpeg_dimensions(const std::string &data, int *width, int *height); // from the frame header (SOFn)
bool valid_thumbnail(const std::string &data, std::string *why); // JPEG, complete (EOI), within the bounds above
std::string thumb_file_name(const std::string &item_id, bool tmp = false); // "" for an invalid item id
// any thread, no DlFs state: a regular file of at most MAX_THUMB_BYTES holding a valid thumbnail
bool read_thumbnail_file(const std::string &path, std::string *out, std::string *why);

class ThumbCache {
	DlFs &fs;
	std::string dir;     // ends with '/', "" = thumbnails off
	bool refused = false; // the folder exists but is not this app's: nothing in it is touched for the rest of the run
	bool verified = false; // ownership was proven on the card during this run (read-only checks rely on it)

	bool write_durable(const std::string &path, const std::string &data, std::string *err);

  public:
	ThumbCache(DlFs &fs, const std::string &dir);
	bool enabled() const { return dir.size() && !refused; }
	const std::string &dir_path() const { return dir; }
	std::string path(const std::string &item_id) const { return dir + thumb_file_name(item_id); }
	// proves ownership on the card now; create: claim the folder (and write the marker) when it does not exist yet
	bool owned(bool create, std::string *why);
	bool present(const std::string &item_id); // owned folder, a regular file of plausible size (content not checked)
	bool read(const std::string &item_id, std::string *out, std::string *why); // validated bytes
	// valid bytes only; ownership re-checked on the card; tmp durable, then it replaces the old file; no partial file
	bool write(const std::string &item_id, const std::string &jpeg, std::string *err);
	// removes this item's generated names (regular files only) from an owned folder; nothing else
	bool remove(const std::string &item_id, std::string *err);
};

} // namespace downloads
