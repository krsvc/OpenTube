#include "downloads/download_manager.hpp"
#include <algorithm>
#include <cstdio>
#include <limits>
#include <unistd.h>

namespace downloads {

const char *item_state_text(ItemState state) {
	switch (state) {
	case ItemState::QUEUED:
		return "Waiting to start";
	case ItemState::RESOLVING:
		return "Getting stream info";
	case ItemState::DOWNLOADING:
		return "Downloading";
	case ItemState::PAUSED_FOR_PLAYBACK:
		return "Paused for playback";
	case ItemState::READY:
		return "Ready";
	case ItemState::FAILED:
		return "Failed";
	case ItemState::CANCELLED:
		return "Cancelled";
	case ItemState::INTERRUPTED:
		return "Interrupted";
	case ItemState::DAMAGED:
		return "Damaged";
	case ItemState::DELETING:
		return "Deleting";
	}
	return "?";
}
bool item_state_active(ItemState state) {
	return state == ItemState::QUEUED || state == ItemState::RESOLVING || state == ItemState::DOWNLOADING ||
	       state == ItemState::PAUSED_FOR_PLAYBACK;
}
std::string format_bytes(uint64_t bytes) {
	char buf[32];
	if (bytes >= 1000ULL * 1000 * 1000) {
		snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1e9);
	} else if (bytes >= 1000ULL * 1000) {
		snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1e6);
	} else {
		snprintf(buf, sizeof(buf), "%u KB", (unsigned)((bytes + 999) / 1000));
	}
	return buf;
}

DownloadManager::DownloadManager(DlFs &fs, const std::string &root, const ManagerConfig &config)
    : store(fs, root), cfg(config), thumbs(fs, config.thumb_dir) {}

// ------------------------------------------------------------------ in-memory API (any thread)
ItemView *DownloadManager::find_locked(const std::string &id) {
	for (auto &item : items) {
		if (item.id == id) {
			return &item;
		}
	}
	return NULL;
}
bool DownloadManager::busy(std::string *active_id) {
	lock.lock();
	bool res = has_job;
	if (res && active_id) {
		*active_id = job.id;
	}
	lock.unlock();
	return res;
}
bool DownloadManager::hold_work_locked(std::string *message) {
	if (work_held) {
		return true;
	}
	if (cfg.work_admit && !cfg.work_admit()) {
		if (message) {
			*message = "An OpenTube update is being installed. Try again when it has finished.";
		}
		return false;
	}
	work_held = true;
	return true;
}
void DownloadManager::release_work_if_idle_locked() {
	if (work_held && !has_job && delete_queue.empty() && !deleting && !thumb_busy && !scanning) {
		work_held = false;
		if (cfg.work_release) {
			cfg.work_release();
		}
	}
}
bool DownloadManager::accept_job_locked(const std::string &id, const DownloadRequest &request, bool claim,
                                        std::string *message) {
	if (exit_flag) {
		*message = "The app is closing.";
		return false;
	}
	if (!loaded) {
		*message = "The download library is still loading.";
		return false;
	}
	if (has_job) {
		ItemView *active = find_locked(job.id);
		*message = "Another download is in progress (" + (active ? active->meta.title : job.id) +
		           "). Wait for it or cancel it in Downloads.";
		return false;
	}
	if (!hold_work_locked(message)) {
		return false;
	}
	has_job = true;
	job_taken = false;
	cancel_flag = false;
	job.id = id;
	job.request = request;
	job.claim = claim;
	return true;
}
bool DownloadManager::request_download(const DownloadRequest &request_in, std::string *message) {
	DownloadRequest request = request_in;
	request.title = sanitize_title(request.title);
	std::string id = make_item_id(request.video_id, request.quality);
	if (id.empty()) {
		*message = "This video or quality cannot be downloaded.";
		return false;
	}
	bool formats_ok = request.video.itag > 0 && (request.layout == Layout::COMBINED || request.audio.itag > 0);
	if (!formats_ok) {
		*message = "No supported format was selected.";
		return false;
	}
	lock.lock();
	ItemView *existing = find_locked(id);
	if (existing) {
		if (existing->state == ItemState::READY) {
			*message = "Already downloaded in " + std::to_string(request.quality) +
			           "p. Delete it in Downloads to download it again.";
			lock.unlock();
			return false;
		}
		if (existing->state == ItemState::DELETING || item_state_active(existing->state)) {
			*message = "This item is busy (" + std::string(item_state_text(existing->state)) + ").";
			lock.unlock();
			return false;
		}
		if (existing->on_card && !existing->owned) {
			*message = "A folder for this download exists but was not created by this app (or its record is "
			           "unreadable). Nothing will be changed; remove it with a computer if needed.";
			lock.unlock();
			return false;
		}
	}
	if (!accept_job_locked(id, request, !(existing && existing->owned), message)) {
		lock.unlock();
		return false;
	}
	if (!existing) {
		items.insert(items.begin(), ItemView());
		existing = &items.front();
		existing->id = id;
	}
	existing->meta_ok = true;
	existing->meta.id = id;
	existing->meta.video_id = request.video_id;
	existing->meta.title = request.title;
	existing->meta.quality = request.quality;
	existing->meta.layout = request.layout;
	existing->meta.video = request.video;
	existing->meta.audio = request.layout == Layout::SEPARATE ? request.audio : FormatIdentity();
	existing->meta.duration_ms = request.duration_ms;
	existing->meta.state = PersistedState::DOWNLOADING;
	existing->state = ItemState::QUEUED;
	existing->message = "";
	existing->done = 0;
	existing->total = 0;
	gen++;
	*message = "Download started (" + std::to_string(request.quality) + "p).";
	lock.unlock();
	return true;
}
bool DownloadManager::request_cancel(const std::string &id, std::string *message) {
	lock.lock();
	bool ok = has_job && job.id == id;
	if (ok) {
		cancel_flag = true;
		*message = "Cancelling...";
	} else {
		*message = "This item is not downloading.";
	}
	lock.unlock();
	return ok;
}
bool DownloadManager::request_retry(const std::string &id, std::string *message) {
	lock.lock();
	ItemView *item = find_locked(id);
	if (!item) {
		*message = "Item not found.";
		lock.unlock();
		return false;
	}
	if (!item->meta_ok || (item->on_card && !item->owned)) {
		*message = "Not a download of this app, or its record is unreadable: nothing will be changed.";
		lock.unlock();
		return false;
	}
	if (item->state != ItemState::FAILED && item->state != ItemState::CANCELLED &&
	    item->state != ItemState::INTERRUPTED && item->state != ItemState::DAMAGED) {
		*message = std::string("Retry is not available while ") + item_state_text(item->state) + ".";
		lock.unlock();
		return false;
	}
	DownloadRequest request;
	request.video_id = item->meta.video_id;
	request.title = item->meta.title;
	request.quality = item->meta.quality;
	request.layout = item->meta.layout;
	request.video = item->meta.video;
	request.audio = item->meta.audio;
	request.duration_ms = item->meta.duration_ms;
	if (!accept_job_locked(id, request, !item->owned, message)) {
		lock.unlock();
		return false;
	}
	item->state = ItemState::QUEUED;
	item->message = "";
	item->done = item->total = 0;
	gen++;
	*message = "Retrying from the start (stream links are fetched again).";
	lock.unlock();
	return true;
}
bool DownloadManager::request_delete(const std::string &id, std::string *message) {
	lock.lock();
	ItemView *item = find_locked(id);
	bool ok = false;
	if (exit_flag) {
		*message = "The app is closing.";
	} else if (!loaded) {
		*message = "The download library is still loading.";
	} else if (!item) {
		*message = "Item not found.";
	} else if (has_job && job.id == id) {
		*message = "Cancel the download first.";
	} else if (item->in_use) {
		*message = "This item is playing. Stop playback first.";
	} else if (item->state == ItemState::DELETING) {
		*message = "Already deleting.";
	} else if (!item->owned && item->on_card) {
		*message = "Not a download of this app, or its record is unreadable: nothing will be deleted.";
	} else if (!item->owned) { // a request that never claimed storage: only in memory
		items.erase(std::remove_if(items.begin(), items.end(), [&](const ItemView &v) { return v.id == id; }),
		            items.end());
		gen++;
		*message = "Removed from the list (nothing on the SD card was changed).";
		ok = true;
	} else if (!hold_work_locked(message)) {
		// an update installation is in progress: nothing queued
	} else {
		item->state = ItemState::DELETING;
		delete_queue.push_back(id);
		gen++;
		*message = "Deleting...";
		ok = true;
	}
	lock.unlock();
	return ok;
}
bool DownloadManager::acquire_playback(const std::string &id, Playable *out, std::string *message) {
	lock.lock();
	ItemView *item = find_locked(id);
	bool ok = item && item->state == ItemState::READY && item->meta_ok;
	if (ok) {
		out->id = id;
		out->title = item->meta.title;
		out->layout = item->meta.layout;
		out->quality = item->meta.quality;
		out->duration_ms = item->meta.duration_ms;
		auto names = media_files(item->meta.layout);
		out->video_path = store.file_path(id, names[0]);
		out->video_size = item->meta.video.size;
		out->audio_path = names.size() > 1 ? store.file_path(id, names[1]) : "";
		out->audio_size = names.size() > 1 ? item->meta.audio.size : 0;
		out->lease = next_lease++;
		if (!next_lease) {
			next_lease = 1;
		}
		leases[out->lease] = id;
		item->in_use = true;
		gen++;
	} else if (message) {
		*message = item ? std::string("This download is not playable (") + item_state_text(item->state) + ")."
		                : "This download no longer exists.";
	}
	lock.unlock();
	return ok;
}
void DownloadManager::release_playback(unsigned lease) {
	lock.lock();
	auto it = leases.find(lease);
	if (it != leases.end()) {
		std::string id = it->second;
		leases.erase(it);
		bool still = false;
		for (auto &l : leases) {
			still |= l.second == id;
		}
		ItemView *item = find_locked(id);
		if (item) {
			item->in_use = still;
		}
		gen++;
	}
	lock.unlock();
}
std::vector<ItemView> DownloadManager::snapshot(unsigned *generation) {
	lock.lock();
	std::vector<ItemView> res = items;
	if (generation) {
		*generation = gen;
	}
	lock.unlock();
	return res;
}
bool DownloadManager::find(const std::string &id, ItemView *out) {
	lock.lock();
	ItemView *item = find_locked(id);
	if (item) {
		*out = *item;
	}
	lock.unlock();
	return item != NULL;
}
std::string DownloadManager::take_notice() {
	lock.lock();
	std::string res = notice;
	notice.clear();
	lock.unlock();
	return res;
}

// ------------------------------------------------------------------ worker
void DownloadManager::set_state(const std::string &id, ItemState state, const std::string &message) {
	lock.lock();
	ItemView *item = find_locked(id);
	if (item && (item->state != state || item->message != message)) {
		item->state = state;
		item->message = message;
		gen++;
	}
	lock.unlock();
}
void DownloadManager::set_progress(const std::string &id, uint64_t done, uint64_t total) {
	lock.lock();
	ItemView *item = find_locked(id);
	if (item) {
		item->done = done;
		item->total = total;
	}
	lock.unlock();
}
void DownloadManager::finish_job(const std::string &id, ItemState state, const std::string &message,
                                 const ItemMeta *meta) {
	lock.lock();
	ItemView *item = find_locked(id);
	if (item) {
		item->state = state;
		item->message = message;
		if (meta) {
			item->meta = *meta;
			item->meta_ok = true;
		}
	}
	has_job = false;
	job_taken = false;
	cancel_flag = false;
	release_work_if_idle_locked();
	notice = (item ? item->meta.title : id) + ": " + item_state_text(state) + (message.size() ? " - " + message : "");
	gen++;
	lock.unlock();
}

struct DownloadManager::Control : TransferControl {
	DownloadManager &m;
	std::string id;
	ItemState running_state = ItemState::DOWNLOADING;
	uint64_t base = 0, total = 0;
	Control(DownloadManager &m, const std::string &id) : m(m), id(id) {}
	StopKind stop_requested() override {
		if (m.exit_flag) {
			return StopKind::EXIT;
		}
		if (m.cancel_flag) {
			return StopKind::CANCEL;
		}
		if (m.cfg.playback_active && m.cfg.playback_active()) {
			return StopKind::PAUSE;
		}
		return StopKind::NONE;
	}
	StopKind wait_while_paused() override {
		StopKind k = stop_requested();
		if (k == StopKind::PAUSE) {
			m.set_state(id, ItemState::PAUSED_FOR_PLAYBACK);
			while ((k = stop_requested()) == StopKind::PAUSE) {
				usleep(m.cfg.pause_poll_us);
			}
			if (k == StopKind::NONE) {
				m.set_state(id, running_state);
			}
		}
		return k;
	}
	void progress(uint64_t stream_done, uint64_t stream_total) override {
		(void)stream_total;
		m.set_progress(id, base + stream_done, total);
	}
};

void DownloadManager::run_delete(const std::string &id) {
	std::string err, why;
	if (!store.owns_item(id, &why)) { // ownership is re-checked on the card right before anything is removed
		lock.lock();
		if (ItemView *item = find_locked(id)) {
			item->state = ItemState::DAMAGED;
			item->owned = false;
			item->message = "Delete refused: not a download of this app (" + why + "); nothing was deleted";
		}
		notice = "Delete refused: nothing was deleted (" + why + ")";
		gen++;
		lock.unlock();
		return;
	}
	bool ok = store.delete_item(id, &err);
	DlFs &fs = store.file_system();
	bool meta_left = fs.stat_path(store.file_path(id, FILE_META)).exists ||
	                 fs.stat_path(store.file_path(id, FILE_META_TMP)).exists;
	lock.lock();
	if (!meta_left) {
		ItemView *item = find_locked(id);
		std::string title = item ? item->meta.title : id;
		items.erase(std::remove_if(items.begin(), items.end(), [&](const ItemView &v) { return v.id == id; }),
		            items.end());
		notice = ok ? "Deleted: " + title : "Deleted: " + title + " (" + err + ")";
	} else {
		ItemView *item = find_locked(id);
		if (item) {
			item->state = ItemState::DAMAGED;
			item->message = "Delete failed: " + err;
		}
		notice = "Delete failed: " + err;
	}
	gen++;
	lock.unlock();
	if (!meta_left && thumbs.enabled()) { // the item is gone: its thumbnail too (a failure only leaves a cache file)
		std::string e;
		thumbs.remove(id, &e);
		thumb_checked.erase(id);
		thumb_attempts.erase(id);
	}
}

// ------------------------------------------------------------------ r14 thumbnails (worker thread, idle only)
bool DownloadManager::thumbnail_should_stop() {
	if (exit_flag || (cfg.playback_active && cfg.playback_active()) ||
	    (cfg.network_available && !cfg.network_available())) {
		return true;
	}
	lock.lock();
	bool busy_now = has_job || delete_queue.size();
	lock.unlock();
	return busy_now;
}
void DownloadManager::thumbnail_step() {
	if (!thumbs.enabled() || !cfg.thumbnail_url || !cfg.session || thumbnail_should_stop()) {
		return;
	}
	std::string id, video_id;
	bool check = false;
	lock.lock();
	for (auto &item : items) { // first what is on the card (no network), then one missing image
		if (item.owned && item.state == ItemState::READY && !thumb_checked.count(item.id)) {
			id = item.id;
			check = true;
			break;
		}
	}
	for (size_t i = 0; !check && i < items.size(); i++) {
		ItemView &item = items[i];
		auto it = thumb_attempts.find(item.id);
		if (item.owned && item.state == ItemState::READY && !item.thumb &&
		    (it == thumb_attempts.end() || it->second < MAX_THUMB_ATTEMPTS)) {
			id = item.id;
			video_id = item.meta.video_id;
			break;
		}
	}
	if (!id.empty()) {
		if (hold_work_locked(NULL)) {
			thumb_busy = true;
		} else {
			id.clear(); // an update installation is in progress: no thumbnail work
		}
	}
	lock.unlock();
	if (id.empty()) {
		return;
	}
	struct ThumbDone { // every return below ends the thumbnail work and drops the hold if the manager is idle
		DownloadManager *m;
		~ThumbDone() {
			m->lock.lock();
			m->thumb_busy = false;
			m->release_work_if_idle_locked();
			m->lock.unlock();
		}
	} thumb_done{this};
	auto publish = [&](bool thumb) {
		lock.lock();
		ItemView *item = find_locked(id);
		if (item && item->thumb != thumb) {
			item->thumb = thumb;
			gen++;
		}
		lock.unlock();
	};
	std::string data, why;
	if (check) {
		thumb_checked.insert(id);
		publish(thumbs.read(id, &data, &why));
		return;
	}
	FetchResult r = fetch_small(*cfg.session, cfg.thumbnail_url(video_id), "image/jpeg", MAX_THUMB_BYTES,
	                            [this]() { return thumbnail_should_stop(); }, &data);
	if (r.error == FetchError::ABORTED) {
		return; // not counted: tried again once the worker is idle
	}
	thumb_attempts[id]++;
	if (r.error == FetchError::NONE && thumbs.write(id, data, &why)) {
		publish(true);
	}
}

void DownloadManager::run_job(const Job &j) {
	const std::string &id = j.id;
	const DownloadRequest &req = j.request;
	Control control(*this, id);
	std::string err;

	ItemMeta meta;
	meta.id = id;
	meta.video_id = req.video_id;
	meta.title = req.title;
	meta.quality = req.quality;
	meta.layout = req.layout;
	meta.video = req.video;
	meta.audio = req.layout == Layout::SEPARATE ? req.audio : FormatIdentity();
	meta.duration_ms = req.duration_ms;
	meta.state = PersistedState::DOWNLOADING;
	if (!store.prepare_item_dir(id, j.claim, &err)) {
		finish_job(id, ItemState::FAILED, "SD card: " + err, NULL);
		return;
	}
	// persisted before any media byte: from here on a crash leaves a visible, non-ready (interrupted) item, and the
	// valid metadata is what proves this app's ownership of the folder after a restart
	if (!store.write_meta(meta, &err)) {
		if (j.claim) {
			store.release_claim(id); // the folder was created by this job moments ago and holds nothing else
		}
		finish_job(id, ItemState::FAILED, "SD card: " + err, NULL);
		return;
	}
	lock.lock();
	if (ItemView *item = find_locked(id)) {
		item->owned = true;
		item->on_card = true;
	}
	lock.unlock();
	auto stopped = [&](StopKind k) {
		if (k == StopKind::EXIT) {
			finish_job(id, ItemState::INTERRUPTED, "App was closed during the download.", &meta);
		} else {
			meta.state = PersistedState::CANCELLED;
			std::string e;
			bool saved = store.write_meta(meta, &e);
			finish_job(id, ItemState::CANCELLED, saved ? "" : "(state not saved: " + e + ")", &meta);
		}
	};
	auto failed = [&](const std::string &message) {
		meta.state = PersistedState::FAILED;
		std::string e;
		store.write_meta(meta, &e); // best effort: an unsaved "failed" still loads as interrupted (non-ready)
		finish_job(id, ItemState::FAILED, message, &meta);
	};

	control.running_state = ItemState::RESOLVING;
	StopKind k = control.wait_while_paused();
	if (k == StopKind::CANCEL || k == StopKind::EXIT) {
		stopped(k);
		return;
	}
	set_state(id, ItemState::RESOLVING);
	ResolveResult r = cfg.resolve ? cfg.resolve(req.video_id, req.quality, req.layout,
	                                            [&]() {
		                                            StopKind s = control.stop_requested();
		                                            return s == StopKind::CANCEL || s == StopKind::EXIT;
	                                            })
	                              : ResolveResult();
	k = control.stop_requested();
	if (k == StopKind::CANCEL || k == StopKind::EXIT) {
		stopped(k);
		return;
	}
	if (!r.ok) {
		failed(r.error.size() ? r.error : "Could not get the stream links.");
		return;
	}
	if (r.is_livestream || r.is_upcoming) {
		failed(r.is_livestream ? "Livestreams cannot be downloaded." : "Upcoming videos cannot be downloaded.");
		return;
	}
	// the representation actually served now is the one recorded and downloaded (restart from zero: no mixing)
	std::vector<ResolvedStream *> streams = {&r.video};
	if (req.layout == Layout::SEPARATE) {
		streams.push_back(&r.audio);
	}
	for (size_t i = 0; i < streams.size(); i++) {
		FormatIdentity &f = streams[i]->format;
		const char *want_kind = i == 0 ? "video/" : "audio/";
		if (streams[i]->url.empty() || f.itag <= 0 || !is_valid_mime(f.mime) || f.mime.compare(0, 6, want_kind) != 0) {
			failed("The selected format is no longer offered.");
			return;
		}
		if (f.size > MAX_FILE_BYTES) {
			failed("File larger than 2 GiB (SD file limit).");
			return;
		}
	}
	meta.video = r.video.format;
	if (req.layout == Layout::SEPARATE) {
		meta.audio = r.audio.format;
	}
	if (r.duration_ms > 0 && r.duration_ms <= MAX_DURATION_MS) {
		meta.duration_ms = r.duration_ms;
	}
	if (meta.title.empty() && r.title.size()) {
		meta.title = sanitize_title(r.title);
	}
	if (!store.write_meta(meta, &err)) {
		finish_job(id, ItemState::FAILED, "SD card: " + err, NULL);
		return;
	}
	lock.lock();
	if (ItemView *item = find_locked(id)) {
		item->meta = meta;
		gen++;
	}
	lock.unlock();

	uint64_t total = 0;
	bool sizes_known = true;
	for (auto s : streams) {
		sizes_known &= s->format.size > 0;
		total += s->format.size;
	}
	if (sizes_known) { // preflight; unknown sizes are bounded per chunk by transfer_stream
		uint64_t free_now = store.file_system().free_bytes(store.root_dir());
		if (free_now != std::numeric_limits<uint64_t>::max() && free_now < total + SPACE_MARGIN_BYTES) {
			failed("Not enough space: needs " + format_bytes(total) + ", " + format_bytes(free_now) + " free.");
			return;
		}
	}
	control.running_state = ItemState::DOWNLOADING;
	control.total = sizes_known ? total : 0;
	set_state(id, ItemState::DOWNLOADING);
	set_progress(id, 0, control.total);
	auto names = media_files(meta.layout);
	FormatIdentity *formats[2] = {&meta.video, &meta.audio};
	for (size_t i = 0; i < streams.size(); i++) {
		StreamPlan plan;
		plan.url = streams[i]->url;
		plan.expected_len = streams[i]->format.size;
		plan.kind = i == 0 ? "video" : "audio";
		plan.path = store.file_path(id, names[i]);
		TransferResult tr = transfer_stream(*cfg.session, store.file_system(), plan, control, cfg.chunk_bytes);
		if (tr.error == TransferError::CANCELLED || tr.error == TransferError::EXITING) {
			stopped(tr.error == TransferError::EXITING ? StopKind::EXIT : StopKind::CANCEL);
			return;
		}
		if (tr.error != TransferError::NONE) {
			failed(std::string(plan.kind) + ": " + transfer_error_text(tr.error) +
			       (tr.detail.size() ? " (" + tr.detail + ")" : ""));
			return;
		}
		formats[i]->size = tr.total; // learned when the page did not state it
		control.base += tr.total;
	}
	// publication: every stream is synced, closed and size-checked; now (and only now) the READY metadata
	meta.state = PersistedState::READY;
	std::string why;
	if (!store.verify_media(meta, &why)) {
		failed("Verification failed: " + why);
		return;
	}
	if (!store.write_meta(meta, &err)) {
		meta.state = PersistedState::DOWNLOADING;
		finish_job(id, ItemState::FAILED, "Could not save the finished item: " + err, &meta);
		return;
	}
	set_progress(id, control.base, control.base);
	thumb_checked.erase(id); // a new READY item: its thumbnail is looked up (and fetched) afresh
	thumb_attempts.erase(id);
	finish_job(id, ItemState::READY, "", &meta);
}

void DownloadManager::run() {
	for (;;) { // the start-up scan is download-library work too (admitted like any other)
		lock.lock();
		scanning = hold_work_locked(NULL);
		lock.unlock();
		if (scanning || exit_flag) {
			break;
		}
		usleep(cfg.idle_sleep_us);
	}
	if (!scanning) {
		return; // exit before the library was scanned: nothing was accepted (not loaded)
	}
	std::vector<ScannedItem> scanned = store.scan();
	lock.lock();
	for (auto &s : scanned) {
		if (find_locked(s.id)) {
			continue;
		}
		ItemView v;
		v.id = s.id;
		v.meta_ok = s.meta_ok;
		v.owned = s.meta_ok; // only a valid record of this item proves ownership (never a file name)
		v.on_card = true;
		v.thumb = s.meta_ok && thumbs.present(s.id);
		if (s.meta_ok) {
			v.meta = s.meta;
		} else {
			parse_item_id(s.id, &v.meta.video_id, &v.meta.quality);
			v.meta.id = s.id;
			v.meta.title = "(unreadable item " + s.id + ")";
		}
		if (!s.meta_ok) {
			v.state = ItemState::DAMAGED;
			v.message = "not a download of this app or its record is unreadable (" + s.problem +
			            "); nothing will be changed, remove it with a computer if needed";
		} else if (s.meta.state == PersistedState::READY) {
			v.state = s.files_ok ? ItemState::READY : ItemState::DAMAGED;
			v.message = s.files_ok ? "" : s.problem;
			if (s.files_ok) {
				v.done = v.total = s.meta.video.size + s.meta.audio.size;
			}
		} else if (s.meta.state == PersistedState::DOWNLOADING) {
			v.state = ItemState::INTERRUPTED;
		} else if (s.meta.state == PersistedState::FAILED) {
			v.state = ItemState::FAILED;
		} else {
			v.state = ItemState::CANCELLED;
		}
		items.push_back(v);
	}
	std::stable_sort(items.begin(), items.end(), [](const ItemView &a, const ItemView &b) {
		return a.meta.title < b.meta.title;
	});
	loaded = true;
	gen++;
	scanning = false;
	release_work_if_idle_locked();
	lock.unlock();

	while (!exit_flag) {
		std::string delete_id;
		bool run_it = false;
		Job j;
		lock.lock();
		if (delete_queue.size()) {
			delete_id = delete_queue.front();
			delete_queue.erase(delete_queue.begin());
			deleting = true;
		} else if (has_job && !job_taken) {
			job_taken = true;
			j = job;
			run_it = true;
		}
		lock.unlock();
		if (delete_id.size()) {
			run_delete(delete_id);
			lock.lock();
			deleting = false;
			release_work_if_idle_locked();
			lock.unlock();
		} else if (run_it) {
			run_job(j);
		} else {
			thumbnail_step();
			usleep(cfg.idle_sleep_us);
		}
	}
	// a job accepted but never started stays visible as non-ready; nothing is written during exit
	lock.lock();
	if (has_job && !job_taken) {
		ItemView *item = find_locked(job.id);
		if (item && item->state == ItemState::QUEUED) {
			item->state = ItemState::INTERRUPTED;
		}
		has_job = false;
	}
	release_work_if_idle_locked();
	lock.unlock();
}

} // namespace downloads
