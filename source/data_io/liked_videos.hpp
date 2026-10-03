#pragma once
// OpenTube r15 local liked videos (OPENTUBE_R15_PLAN.md A). A like is a local favorite on this console only: nothing
// is sent to YouTube and YouTube's own like count is never changed.
//
// Files (fixed names in the app data root, nothing else is touched):
//   liked_videos.json  durable list
//   liked_videos.tmp   next version: written completely and synced, then it replaces liked_videos.json
// Content: {"format":"opentube.liked_videos","version":1,"items":[{"id","title","channel","duration_ms",
// "liked_at"}]}, newest first. Only the exact 11 character video id, a bounded title / channel name, the duration and
// the time it was liked: no URL (the thumbnail is derived from the id), no signed media URL, no account data.
//
// Load: a present liked_videos.json that cannot be read or does not validate (wrong format / version, malformed,
// oversized, bad or duplicate id) LOCKS the store: it stays empty, every change is refused with a message and the
// file is never written, renamed or removed. A missing liked_videos.json with a valid .tmp (a save interrupted
// between its remove and its rename) is recovered; an invalid .tmp is this app's unfinished scratch file and is
// replaced by the next save.
//
// Threading: LikedStore is not thread safe; the owner serializes every call (liked_videos_app.cpp). Saving is split so
// the file I/O runs without that lock: save_begin() (lock held) -> save_write() (no lock) -> save_end() (lock held).
// A failed save reverts the list to the last saved state and keeps the error for the UI: success is never assumed.
#include <cstdint>
#include <string>
#include <vector>
#include "downloads/download_store.hpp"

namespace liked {

constexpr size_t MAX_ITEMS = 1000;
constexpr size_t MAX_FILE_BYTES = 512 * 1024;
constexpr size_t MAX_TITLE_BYTES = 200;
constexpr size_t MAX_CHANNEL_BYTES = 100;
constexpr int64_t MAX_DURATION_MS = 48LL * 3600 * 1000;
extern const char *const FILE_MAIN;
extern const char *const FILE_TMP;
extern const char *const FORMAT_NAME;
constexpr int FORMAT_VERSION = 1;

struct LikedVideo {
	std::string id;
	std::string title;
	std::string channel;
	int64_t duration_ms = 0;
	int64_t liked_at = 0; // seconds since 1970
	bool operator==(const LikedVideo &o) const {
		return id == o.id && title == o.title && channel == o.channel && duration_ms == o.duration_ms &&
		       liked_at == o.liked_at;
	}
};

// bounded, control characters removed, valid UTF-8 cut; false when the id is not a video id (out may be NULL)
bool make_entry(const std::string &id, const std::string &title, const std::string &channel, int64_t duration_ms,
                int64_t liked_at, LikedVideo *out);
std::string serialize(const std::vector<LikedVideo> &items);
bool parse(const std::string &text, std::vector<LikedVideo> *out, std::string *why);

// display / routing helpers of the Liked videos scene
std::string duration_text(int64_t duration_ms); // "4:13", "1:02:03", "" when unknown
// online: the normal YouTube route (even when an SD copy exists); offline: the READY SD copy, else nothing
enum class OpenRoute { ONLINE, LOCAL, UNAVAILABLE };
OpenRoute open_route(bool online, bool has_ready_copy);

enum class LoadState { NOT_LOADED, EMPTY, LOADED, RECOVERED, LOCKED };

class LikedStore {
	downloads::DlFs &fs;
	std::string dir; // ends with '/'
	LoadState load_state = LoadState::NOT_LOADED;
	std::string problem;          // why LOCKED
	std::vector<LikedVideo> list; // current (what the UI shows), newest first
	std::vector<LikedVideo> saved; // last state known to be on the card
	std::vector<LikedVideo> saving; // snapshot of the save in progress
	unsigned gen = 0, saved_gen = 0, saving_gen = 0;
	bool save_running = false;
	std::string last_error; // last failed save, for the UI (take_error)

  public:
	LikedStore(downloads::DlFs &fs, const std::string &dir);
	std::string path(const char *name) const { return dir + name; }

	void load(); // file I/O; call once before anything else
	LoadState state() const { return load_state; }
	bool locked() const { return load_state == LoadState::LOCKED || load_state == LoadState::NOT_LOADED; }
	const std::string &lock_reason() const { return problem; }

	const std::vector<LikedVideo> &items() const { return list; }
	bool contains(const std::string &id) const;
	// in memory; idempotent (already liked / not liked: true, nothing changes). false + message: refused
	bool add(const LikedVideo &video, std::string *message);
	bool remove(const std::string &id, std::string *message);
	unsigned generation() const { return gen; }
	bool dirty() const { return gen != saved_gen; }

	// save protocol (see the header comment); save_begin() false: nothing to save / locked / one already running
	bool save_begin(std::string *text, unsigned *snapshot_gen);
	bool save_write(const std::string &text, std::string *error) const;
	void save_end(unsigned snapshot_gen, bool ok, const std::string &error);
	std::string take_error();
};

} // namespace liked
