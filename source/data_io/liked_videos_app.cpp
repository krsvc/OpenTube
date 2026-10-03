#include "headers.hpp"
#include "data_io/liked_videos_app.hpp"
#include "util/misc_tasks.hpp"

static Mutex liked_lock;
static liked::LikedStore *store = NULL;

static liked::LikedStore &the_store() {
	if (!store) {
		store = new liked::LikedStore(downloads::posix_fs(), DEF_MAIN_DIR);
	}
	return *store;
}

void liked_init() {
	liked_lock.lock();
	the_store().load();
	liked::LoadState st = the_store().state();
	std::string reason = the_store().lock_reason();
	size_t n = the_store().items().size();
	liked_lock.unlock();
	if (st == liked::LoadState::LOCKED) {
		logger.error("liked/load", "locked (file kept untouched): " + reason);
	} else {
		logger.info("liked/load", "loaded " + std::to_string(n) + " liked videos" +
		                              (st == liked::LoadState::RECOVERED ? " (recovered from the interrupted save)" : ""));
	}
}
bool liked_available(std::string *reason) {
	liked_lock.lock();
	bool ok = !the_store().locked();
	if (!ok && reason) {
		*reason = the_store().lock_reason();
	}
	liked_lock.unlock();
	return ok;
}
bool liked_contains(const std::string &video_id) {
	liked_lock.lock();
	bool res = the_store().contains(video_id);
	liked_lock.unlock();
	return res;
}
bool liked_set(const liked::LikedVideo &video, bool like, std::string *message) {
	liked_lock.lock();
	bool ok = like ? the_store().add(video, message) : the_store().remove(video.id, message);
	bool dirty = the_store().dirty();
	liked_lock.unlock();
	if (ok) {
		*message = like ? "Saved to Liked videos on this console" : "Removed from Liked videos";
		if (dirty) {
			misc_tasks_request(TASK_SAVE_LIKED);
		}
	}
	return ok;
}
std::vector<liked::LikedVideo> liked_snapshot(unsigned *generation) {
	liked_lock.lock();
	std::vector<liked::LikedVideo> res = the_store().items();
	if (generation) {
		*generation = the_store().generation();
	}
	liked_lock.unlock();
	return res;
}
unsigned liked_generation() {
	liked_lock.lock();
	unsigned g = the_store().generation();
	liked_lock.unlock();
	return g;
}
void liked_save_pending() {
	for (int round = 0; round < 4; round++) { // edits made during a write are saved right after it
		std::string text;
		unsigned gen = 0;
		liked_lock.lock();
		bool begun = the_store().save_begin(&text, &gen);
		liked_lock.unlock();
		if (!begun) {
			return;
		}
		std::string error;
		bool ok = the_store().save_write(text, &error); // const: file I/O only, no store state
		liked_lock.lock();
		the_store().save_end(gen, ok, error);
		liked_lock.unlock();
		if (!ok) {
			logger.error("liked/save", "not saved (reverted): " + error);
			var_need_refresh = true;
			return;
		}
		logger.info("liked/save", "saved");
	}
}
std::string liked_take_error() {
	liked_lock.lock();
	std::string e = the_store().take_error();
	liked_lock.unlock();
	return e;
}
