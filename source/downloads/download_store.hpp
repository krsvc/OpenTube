#pragma once
// SD-card download library: on-disk layout, strict metadata and crash-safe publication (DOWNLOADS_OFFLINE_BRIEF.md).
//
// Layout (all names generated here, never derived from a title or URL):
//   <root>/<item id>/meta.txt     metadata of the item (see serialize_meta)
//   <root>/<item id>/meta.tmp     next metadata, written completely and synced before it replaces meta.txt
//   <root>/<item id>/video.mp4    separate layout: H.264 video stream
//   <root>/<item id>/audio.m4a    separate layout: AAC audio stream
//   <root>/<item id>/media.mp4    combined layout: the single muxed stream (itag 18)
// An item id is "<11 character video id>-<quality>p" (e.g. "dQw4w9WgXcQ-360p").
//
// Ownership: the app owns an item folder only if it created that folder itself (claim: the folder must not exist
// before, whatever it would contain) or if the folder holds this item's valid, checksummed metadata. A known media
// file name, an item-shaped folder name or malformed / foreign metadata never counts: such folders are never written
// to, claimed or deleted (fail closed).
//
// Publication: an item is READY only when its metadata says so AND every media file exists with the recorded size.
// The READY metadata is written (tmp + sync + close, then replacing meta.txt) only after every stream was fully
// written, synced, closed and size-verified, so a crash at any earlier point leaves a non-ready item.
//
// Metadata holds title, video id, format identity (itag, base mime type, size), duration and state only: no URL,
// signature, cookie or token ever reaches it.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace downloads {

// newlib's fseek()/ftell() take a 32-bit long on the 3DS (FAT32 itself allows 4 GiB - 1): larger files are refused
constexpr uint64_t MAX_FILE_BYTES = 0x7FFFFFFFULL;
constexpr size_t MAX_TITLE_BYTES = 200;
constexpr size_t MAX_META_BYTES = 4096;
constexpr int64_t MAX_DURATION_MS = 48LL * 3600 * 1000;

enum class Layout { SEPARATE, COMBINED };
enum class PersistedState { DOWNLOADING, READY, FAILED, CANCELLED };

struct FormatIdentity {
	int itag = 0;
	std::string mime; // base type only, e.g. "video/mp4"
	uint64_t size = 0; // 0 = not known
};

struct ItemMeta {
	std::string id;
	std::string video_id;
	std::string title;
	int quality = 0;
	Layout layout = Layout::SEPARATE;
	FormatIdentity video; // combined layout: the single stream
	FormatIdentity audio; // separate layout only
	int64_t duration_ms = 0;
	PersistedState state = PersistedState::DOWNLOADING;
};

bool is_valid_video_id(const std::string &video_id);
bool is_supported_quality(int quality);
std::string make_item_id(const std::string &video_id, int quality); // "" when either part is invalid
bool parse_item_id(const std::string &id, std::string *video_id, int *quality);
bool is_valid_mime(const std::string &mime);
std::string base_mime(const std::string &mime_type); // "video/mp4; codecs=..." -> "video/mp4" ("" if unusable)
std::string sanitize_title(const std::string &title);  // control characters -> space, bounded, valid UTF-8 cut
std::string serialize_meta(const ItemMeta &meta);
bool parse_meta(const std::string &text, ItemMeta *out, std::string *why);
const char *state_name(PersistedState state);

extern const char *const FILE_VIDEO;
extern const char *const FILE_AUDIO;
extern const char *const FILE_COMBINED;
extern const char *const FILE_META;
extern const char *const FILE_META_TMP;
std::vector<const char *> media_files(Layout layout); // in stream order (video first)

// ------------------------------------------------------------------ file system seam (host tests inject faults)
struct DlStat {
	bool exists = false;
	bool is_dir = false;
	bool is_file = false;
	bool is_link = false;
	uint64_t size = 0;
};
struct DlFile {
	virtual ~DlFile() {}
	virtual size_t write(const void *data, size_t len) = 0; // bytes written; fewer = error (see error())
	virtual bool seek(uint64_t pos) = 0;
	virtual bool sync() = 0;  // flush the stdio buffer and ask the file system to persist it
	virtual bool close() = 0; // exactly once; false = the data may not have been written
	virtual int error() = 0;  // errno of the last failure
};
struct DlFs {
	virtual ~DlFs() {}
	virtual DlFile *open_truncate(const std::string &path) = 0; // NULL on failure (last_errno())
	virtual bool read_small(const std::string &path, size_t max_len, std::string *out) = 0;
	virtual int remove_file(const std::string &path) = 0; // 0 or errno
	virtual int rename_file(const std::string &from, const std::string &to) = 0;
	virtual int make_dir(const std::string &path) = 0; // 0 or errno (EEXIST included)
	virtual int remove_dir(const std::string &path) = 0; // only succeeds on an empty directory
	virtual bool list_dir(const std::string &path, std::vector<std::string> *names) = 0;
	virtual DlStat stat_path(const std::string &path) = 0; // never follows a symlink where the platform has them
	virtual uint64_t free_bytes(const std::string &path) = 0; // UINT64_MAX when unknown
	virtual int last_errno() = 0;
};
DlFs &posix_fs(); // stdio / POSIX implementation (3DS: newlib + sdmc devoptab)

// ------------------------------------------------------------------ store
struct ScannedItem {
	std::string id;
	bool meta_ok = false;
	ItemMeta meta;
	bool files_ok = false; // READY metadata AND every media file present with the recorded size
	std::string problem;   // why meta_ok / files_ok is false
};

class DownloadStore {
	DlFs &fs;
	std::string root; // ends with '/'

	bool write_durable(const std::string &path, const std::string &data, std::string *err);

  public:
	DownloadStore(DlFs &fs, const std::string &root);
	DlFs &file_system() { return fs; }
	const std::string &root_dir() const { return root; }
	std::string item_dir(const std::string &id) const;
	std::string file_path(const std::string &id, const char *name) const;

	// claim (new item): creates root as needed and the item folder, which must not exist yet (nothing is merged into
	// or taken over). !claim (restart of an owned item): the folder must be owned (owns_item) and this app's file
	// names in it must be regular files.
	bool prepare_item_dir(const std::string &id, bool claim, std::string *err);
	bool owns_item(const std::string &id, std::string *why); // real folder holding this item's valid metadata
	// undo a claim whose first metadata write failed: only the metadata file of that write and the (then empty) folder
	void release_claim(const std::string &id);
	bool write_meta(const ItemMeta &meta, std::string *err); // meta.tmp durable, then replaces meta.txt
	bool load_meta(const std::string &id, ItemMeta *out, std::string *why);
	bool verify_media(const ItemMeta &meta, std::string *why); // sizes known and equal on disk
	std::vector<ScannedItem> scan(); // only valid item ids that are real directories; nothing is modified
	// owned items only: removes this app's own file names inside the item folder, then the folder if it is empty
	bool delete_item(const std::string &id, std::string *err);
};

} // namespace downloads
