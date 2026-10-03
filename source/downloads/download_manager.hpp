#pragma once
// Download library + single transfer worker (DOWNLOADS_OFFLINE_BRIEF.md).
//
// Ownership / threading:
//  - One worker thread runs run(): it alone touches the SD card (scan, metadata, media, delete) and the network
//    (through ManagerConfig::resolve and its own NetworkSessionList). The UI / player threads only call the request_*
//    / acquire / snapshot functions, which change in-memory state under `lock` and never wait on IO.
//  - One job at a time. A request while a job is queued or running is rejected with a visible message.
//  - While ManagerConfig::playback_active() is true the job is paused (PAUSED_FOR_PLAYBACK): the in-flight chunk is
//    aborted and fetched again from its first byte once playback stops.
//  - Playback leases: the player acquires a READY item before opening its files and releases it after the decoder
//    closed them. Delete is refused while any lease or the job refers to the item, so no file is removed under a
//    reader and no transfer races a delete.
//  - OpenTube r14 thumbnails (thumb_cache.hpp): only when the worker is idle (no job / delete queued, no playback, not
//    exiting) it validates saved thumbnails and fetches missing ones for READY items, one small request at a time,
//    at most MAX_THUMB_ATTEMPTS counted attempts per item per run; a job, delete, playback or exit aborts it. A
//    thumbnail never changes an item's state, metadata or media.
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "downloads/download_store.hpp"
#include "downloads/download_transfer.hpp"
#include "downloads/thumb_cache.hpp"
#include "system/libctru_wrapper.hpp"

namespace downloads {

enum class ItemState {
	QUEUED,
	RESOLVING,
	DOWNLOADING,
	PAUSED_FOR_PLAYBACK,
	READY,
	FAILED,
	CANCELLED,
	INTERRUPTED, // metadata still says "downloading" (app exit / crash / power loss during the transfer)
	DAMAGED,     // unreadable metadata, or READY metadata whose media is missing / has the wrong size
	DELETING,
};
const char *item_state_text(ItemState state);
bool item_state_active(ItemState state); // queued / resolving / downloading / paused

struct ItemView {
	std::string id;
	bool meta_ok = false;
	ItemMeta meta;
	ItemState state = ItemState::DAMAGED;
	uint64_t done = 0;  // bytes of all streams on the card
	uint64_t total = 0; // 0 = not known yet
	std::string message; // failure reason (never contains a URL)
	bool in_use = false; // leased for playback
	// storage ownership (download_store.hpp): `owned` = this app created the folder or it holds this item's valid
	// metadata; `on_card` = a folder for it exists on the card. An item that is on the card but not owned is never
	// written to, retried or deleted; a request that never claimed storage is only in memory.
	bool owned = false;
	bool on_card = false;
	bool thumb = false; // r14: the thumbnail cache holds a file for this item (validated when read)
};

struct DownloadRequest {
	std::string video_id;
	std::string title;
	int quality = 0;
	Layout layout = Layout::SEPARATE;
	FormatIdentity video; // identity shown to the user; the worker re-resolves and records the served identity
	FormatIdentity audio;
	int64_t duration_ms = 0;
};

struct ResolvedStream {
	std::string url; // signed, memory only
	FormatIdentity format;
};
struct ResolveResult {
	bool ok = false;
	std::string error; // user-facing, no URL
	bool is_livestream = false;
	bool is_upcoming = false;
	std::string title;
	int64_t duration_ms = 0;
	ResolvedStream video; // combined layout: the single stream
	ResolvedStream audio;
};
using ResolveFunc = std::function<ResolveResult(const std::string &video_id, int quality, Layout layout,
                                                const std::function<bool()> &should_stop)>;

struct Playable {
	std::string id;
	std::string title;
	Layout layout = Layout::SEPARATE;
	int quality = 0;
	int64_t duration_ms = 0;
	std::string video_path; // combined: the single file
	std::string audio_path;
	uint64_t video_size = 0;
	uint64_t audio_size = 0;
	unsigned lease = 0;
};

struct ManagerConfig {
	ResolveFunc resolve;
	std::function<bool()> playback_active;
	NetworkSessionList *session = NULL; // used by the worker thread only
	uint64_t chunk_bytes = CHUNK_BYTES;
	unsigned idle_sleep_us = 50000;
	unsigned pause_poll_us = 100000;
	// r14 thumbnails: cache folder ("" = off), the image url of a video id, and whether a network is up (unset = yes)
	std::string thumb_dir;
	std::function<std::string(const std::string &video_id)> thumbnail_url;
	std::function<bool()> network_available;
	// r15b: admission against an update installation (util/activity_gate.hpp). work_admit() is asked under the
	// manager lock before any SD / network work is accepted or started (job, delete, thumbnail, start-up scan);
	// false refuses it. work_release() once no such work is queued or running. Unset = always admitted.
	std::function<bool()> work_admit;
	std::function<void()> work_release;
};
constexpr int MAX_THUMB_ATTEMPTS = 2; // counted (not aborted) fetch attempts per item per run

class DownloadManager {
  public:
	DownloadManager(DlFs &fs, const std::string &root, const ManagerConfig &config);

	// ---- any thread: in-memory only
	bool request_download(const DownloadRequest &request, std::string *message);
	bool request_cancel(const std::string &id, std::string *message);
	bool request_retry(const std::string &id, std::string *message);
	bool request_delete(const std::string &id, std::string *message);
	bool acquire_playback(const std::string &id, Playable *out, std::string *message);
	void release_playback(unsigned lease);
	std::vector<ItemView> snapshot(unsigned *generation = NULL);
	bool find(const std::string &id, ItemView *out);
	bool library_loaded() { return loaded; }
	bool busy(std::string *active_id = NULL);
	std::string take_notice(); // one-shot message of the last finished delete / job (UI toast)

	// ---- worker thread
	void run(); // scans the library, then serves jobs until request_exit()
	void request_exit() { exit_flag = true; }
	bool exit_requested() const { return exit_flag; }

  private:
	struct Job {
		std::string id;
		DownloadRequest request;
		bool claim = true; // storage is not owned yet: the job must create the item folder itself
	};
	struct Control;

	Mutex lock;
	DownloadStore store;
	ManagerConfig cfg;
	std::vector<ItemView> items; // display order: newest first
	std::map<unsigned, std::string> leases;
	unsigned next_lease = 1;
	unsigned gen = 0;
	bool has_job = false;   // a job is queued or running (active_id)
	bool job_taken = false; // the worker picked it up
	Job job;
	std::vector<std::string> delete_queue;
	std::string notice;
	volatile bool loaded = false;
	volatile bool exit_flag = false;
	volatile bool cancel_flag = false;
	// r14 thumbnails, worker thread only
	ThumbCache thumbs;
	std::map<std::string, int> thumb_attempts;
	std::set<std::string> thumb_checked; // the saved file was validated (or found missing) during this run
	// r15b admission (ManagerConfig::work_admit), under `lock`: one hold while any of these is true
	bool work_held = false;
	bool scanning = false, deleting = false, thumb_busy = false;
	bool hold_work_locked(std::string *message);
	void release_work_if_idle_locked();

	ItemView *find_locked(const std::string &id);
	void set_state(const std::string &id, ItemState state, const std::string &message = "");
	void set_progress(const std::string &id, uint64_t done, uint64_t total);
	void finish_job(const std::string &id, ItemState state, const std::string &message, const ItemMeta *meta);
	void run_job(const Job &job);
	void run_delete(const std::string &id);
	bool accept_job_locked(const std::string &id, const DownloadRequest &request, bool claim, std::string *message);
	bool thumbnail_should_stop();
	void thumbnail_step();
};

std::string format_bytes(uint64_t bytes); // "12.3 MB"

} // namespace downloads
