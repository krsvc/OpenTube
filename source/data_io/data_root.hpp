#pragma once
// OpenTube v0.16.2 app data root admission and one-time preservation migration (Documentation/Data root migration.md).
//
// Roots (diag / OpenTube build, definitions.hpp): the new root /3ds/opentube/ is authoritative once it holds a valid
// marker file (MARKER_NAME). Without one, it is created from the predecessor root /3ds/FourthTubeTest/ (read only,
// never changed) through an app-owned staging folder /3ds/opentube.partial/. The staging folder becomes the new
// root by a single non-replacing directory rename, and only after every file and the marker were written, synced,
// closed and checked.
//
// Never: overwriting, merging into or adopting anything that already exists at the new root, following a link,
// treating an unreadable source as absent, removing anything (old root, staging leftovers, foreign data), or reading
// file contents into memory beyond one fixed copy buffer. Log details carry relative paths and errno values only.
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace data_root {

extern const char *const MARKER_NAME;     // "opentube_data_root.txt"
extern const char *const SKIPPED_TOP_DIR; // "update": the updater's session staging, never copied
constexpr size_t MAX_MARKER_BYTES = 512;
constexpr size_t DEFAULT_CHUNK_BYTES = 128 * 1024;
constexpr size_t MAX_ENTRIES = 10000;
constexpr int MAX_DEPTH = 8;
constexpr size_t MAX_PATH_BYTES = 250;             // staging path of any copied entry
constexpr uint64_t MAX_FILE_BYTES = 0x7FFFFFFFULL; // newlib's 32-bit off_t on the 3DS (downloads use the same bound)
constexpr uint64_t RESERVE_BYTES = 1024 * 1024;    // free space left after the copy

// ------------------------------------------------------------------ file system seam (host tests inject faults)
struct Probe {
	enum Kind { ABSENT, DIR, FILE, OTHER, ERROR }; // OTHER: link, device, FIFO, socket. ERROR: not ENOENT
	Kind kind = ERROR;
	uint64_t size = 0;
	int err = 0;
};
struct InFile {
	virtual ~InFile() {}
	virtual long read(void *buf, size_t len) = 0; // bytes read, 0 at the end, -1 on error (error())
	virtual int close() = 0;                      // 0 or errno; exactly once
	virtual int error() = 0;
};
struct OutFile {
	virtual ~OutFile() {}
	virtual long write(const void *buf, size_t len) = 0; // bytes written; fewer than len or -1 = error (error())
	virtual int sync() = 0;                              // 0 or errno
	virtual int close() = 0;                             // 0 or errno; exactly once
	virtual int error() = 0;
};
struct RootFs {
	virtual ~RootFs() {}
	virtual Probe probe(const std::string &path) = 0;                                   // never follows a link
	virtual int list_dir(const std::string &path, std::vector<std::string> *names) = 0; // 0 or errno
	virtual int make_dir(const std::string &path) = 0;                                  // 0 or errno (EEXIST)
	virtual InFile *open_read(const std::string &path, int *err) = 0;
	virtual OutFile *create_new(const std::string &path, int *err) = 0; // exclusive: fails if anything exists
	virtual int rename_dir_noreplace(const std::string &from, const std::string &to) = 0; // never replaces `to`
	virtual int space(const std::string &path, uint64_t *free_bytes, uint64_t *cluster_bytes) = 0;
};
RootFs &posix_root_fs(); // 3DS: newlib + sdmc devoptab, FSUSER_RenameDirectory; host: POSIX, renamex_np/renameat2

// ------------------------------------------------------------------ marker
struct Marker {
	bool migrated = false;
	uint64_t files = 0;
	uint64_t bytes = 0;
};
std::string format_marker(const Marker &marker);
bool parse_marker(const std::string &text, Marker *out); // exact format and checksum only

// ------------------------------------------------------------------ admission
struct Layout {
	std::string old_root;   // predecessor, read only
	std::string stage_root; // app-owned partial output
	std::string new_root;   // authoritative once established
};
enum class Status { READY, FRESH, MIGRATED, BLOCKED };
enum class Block {
	NONE,
	NEW_ROOT_FOREIGN,    // something without a valid marker is at the new root
	NEW_ROOT_UNREADABLE, // the new root cannot be checked
	PARTIAL_LEFTOVER,    // an earlier copy was interrupted: the staging folder exists
	STAGE_UNREADABLE,
	SOURCE_UNREADABLE,  // the old root exists but cannot be read (never treated as absent)
	SOURCE_UNSUPPORTED, // link, special file, invalid name or limit in the old root
	SPACE_UNKNOWN,
	NO_SPACE,
	NO_MEMORY,
	COPY_FAILED,
	PUBLISH_FAILED,
	CANCELLED, // the system asked the app to close during the copy
};
struct Outcome {
	Status status = Status::BLOCKED;
	Block block = Block::NONE;
	std::string detail; // for the log: relative path / errno, never file contents
	uint64_t need_bytes = 0, free_bytes = 0;
	uint64_t total_bytes = 0, copied_bytes = 0;
	uint64_t total_files = 0, copied_files = 0;
	bool stage_written = false; // blocked after creating the staging folder: it stays, the user removes it
	bool ready() const { return status != Status::BLOCKED; }
};
struct Progress {
	enum Phase { SCANNING, COPYING, PUBLISHING } phase = SCANNING;
	uint64_t done_bytes = 0, total_bytes = 0;
	uint64_t done_files = 0, total_files = 0;
};
typedef std::function<bool(const Progress &)> ProgressFn; // false = stop as soon as possible (system close)

Outcome prepare(RootFs &fs, const Layout &layout, const ProgressFn &progress, size_t chunk_bytes = DEFAULT_CHUNK_BYTES);
const char *block_name(Block block);
// bounded plain-ASCII text for the startup failure screen (at most 12 lines of at most 56 characters)
std::string user_message(const Outcome &outcome, const Layout &layout);

} // namespace data_root
