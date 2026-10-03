#include "headers.hpp"
#include "downloads/downloads_app.hpp"
#include "scenes/video_player.hpp"
#include "util/activity_gate.hpp"
#include "util/async_task.hpp"
#include "youtube_parser/parser.hpp"

#define DOWNLOADS_ROOT (DEF_MAIN_DIR + "downloads/")
#define DOWNLOAD_THUMBS_DIR (DEF_MAIN_DIR + "download_thumbs/") // r14: separate app-owned cache (thumb_cache.hpp)
#define RESOLVE_TIMEOUT_MS (150 * 1000) // > page load bounds (20 s connect + 30 s stall per request); DNS is not bounded

namespace {
NetworkSessionList worker_session; // owned by the worker thread: initialised and cleaned up there
downloads::DownloadManager *manager = NULL;
Thread worker_thread = NULL;
// the worker's OS thread handle. Completion is only its kernel signal: libctru's threadExit() sets the "finished" flag
// (which makes threadJoin() return 0 at once) before its last write to the thread object and svcExitThread(), and the
// object also holds the thread's stack. 0 is the only success; a timeout (0x09401BFE, not R_FAILED) or an error waits.
Handle worker_handle = 0;

// one resolution at a time, handed to the async task thread; a late result of an abandoned request is dropped
Mutex resolve_lock;
unsigned resolve_gen = 0;
bool resolve_done = false;
std::string resolve_url;
YouTubeVideoDetail resolve_result;

void resolve_task(void *arg) {
	unsigned gen = (unsigned)(uintptr_t)arg;
	resolve_lock.lock();
	bool current = gen == resolve_gen;
	std::string url = resolve_url;
	resolve_lock.unlock();
	if (!current) {
		return;
	}
	add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	YouTubeVideoDetail detail = youtube_load_video_page(url, false); // not a view: watch history untouched
	remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
	resolve_lock.lock();
	if (gen == resolve_gen) {
		resolve_result = detail;
		resolve_done = true;
	}
	resolve_lock.unlock();
}

downloads::ResolveResult resolve(const std::string &video_id, int quality, downloads::Layout layout,
                                 const std::function<bool()> &should_stop) {
	downloads::ResolveResult res;
	resolve_lock.lock();
	unsigned gen = ++resolve_gen;
	resolve_done = false;
	resolve_url = youtube_get_video_url_by_id(video_id);
	resolve_lock.unlock();
	queue_async_task(resolve_task, (void *)(uintptr_t)gen);

	u64 start = osGetTime();
	while (true) {
		usleep(50000);
		resolve_lock.lock();
		bool done = resolve_done && gen == resolve_gen;
		YouTubeVideoDetail detail;
		if (done) {
			detail = resolve_result;
			resolve_result = YouTubeVideoDetail();
		}
		resolve_lock.unlock();
		if (done) {
			return downloads::resolve_from_detail(detail, quality, layout);
		}
		bool timed_out = osGetTime() - start > RESOLVE_TIMEOUT_MS;
		if (should_stop() || timed_out) {
			resolve_lock.lock();
			resolve_gen++; // the task may still run: its result is dropped
			resolve_lock.unlock();
			res.error = timed_out ? "Getting stream info timed out. Check the connection and retry." : "Stopped.";
			return res;
		}
	}
}

void worker_entry(void *arg) {
	downloads::DownloadManager *m = (downloads::DownloadManager *)arg;
	worker_session.init_owned();
	m->run();
	worker_session.cleanup_owned(); // after the last perform(): no other thread ever frees these handles
	threadExit(0);
}
} // namespace

void Downloads_start() {
	if (manager) {
		return;
	}
	downloads::ManagerConfig cfg;
	cfg.resolve = resolve;
	cfg.playback_active = []() { return video_playback_active(); };
	cfg.session = &worker_session;
	cfg.thumb_dir = DOWNLOAD_THUMBS_DIR;
	cfg.thumbnail_url = [](const std::string &video_id) { return "https://i.ytimg.com/vi/" + video_id + "/mqdefault.jpg"; };
	cfg.network_available = []() { return var_wifi_state == 2; }; // offline: not a single thumbnail request
	cfg.work_admit = []() { return activity_gate().hold_downloads(); }; // r15b: never alongside an update install
	cfg.work_release = []() { activity_gate().release_downloads(); };
	manager = new downloads::DownloadManager(downloads::posix_fs(), DOWNLOADS_ROOT, cfg);
	worker_thread = threadCreate(worker_entry, manager, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	if (worker_thread) {
		worker_handle = threadGetHandle(worker_thread); // valid: run() only returns after request_exit()
	} else {
		logger.error("downloads", "worker thread creation failed: downloads unavailable");
		delete manager; // never ran and never published: downloads_running() is false, the UI says so
		manager = NULL;
	}
}
void Downloads_request_stop() {
	if (!manager || !worker_thread) {
		return;
	}
	manager->request_exit(); // aborts the in-flight chunk (libcurl progress callback) and any pause / resolve wait
	remove_all_async_tasks_with_type(resolve_task); // a queued stream resolution never starts during exit
}
bool Downloads_finished() { return !worker_thread || svcWaitSynchronization(worker_handle, 0) == 0; }
bool Downloads_stop() {
	if (!manager || !worker_thread) {
		return true;
	}
	Downloads_request_stop();
	u64 time_out = 10000000000;
	Result join = svcWaitSynchronization(worker_handle, time_out); // the kernel ended it, not just libctru's flag
	logger.info("downloads", "svcWaitSynchronization()...", join);
	if (join != 0) { // a timeout is 0x09401BFE, which R_FAILED() does not report
		// still live (a blocking call such as a DNS lookup): keep the handle, its session and the manager; the caller
		// keeps the services it may use, and the worker releases its session itself if it ever finishes
		logger.error("downloads", "worker still running after the exit bound; its resources are kept");
		return false;
	}
	threadFree(worker_thread); // closes worker_handle too
	worker_thread = NULL;
	worker_handle = 0;
	return true;
}
downloads::DownloadManager &downloads_manager() { return *manager; }
bool downloads_running() { return manager != NULL; }
std::string Downloads_thumbnail_key(const std::string &item_id, bool large) {
	return std::string("local-thumb:") + (large ? "L:" : "S:") + item_id;
}
bool Downloads_parse_thumbnail_key(const std::string &key, std::string *item_id, bool *large) {
	if (key.compare(0, 12, "local-thumb:") != 0 || key.size() < 15 || (key[12] != 'L' && key[12] != 'S') || key[13] != ':') {
		return false;
	}
	*item_id = key.substr(14);
	*large = key[12] == 'L';
	return downloads::parse_item_id(*item_id, NULL, NULL);
}
bool Downloads_read_thumbnail(const std::string &item_id, std::vector<u8> *out) {
	// only an item of this app's library for which the worker proved the cache folder and found a file
	downloads::ItemView item;
	if (!manager || !downloads::parse_item_id(item_id, NULL, NULL) || !manager->find(item_id, &item) || !item.owned ||
	    !item.thumb) {
		return false;
	}
	std::string data, why;
	if (!downloads::read_thumbnail_file(DOWNLOAD_THUMBS_DIR + downloads::thumb_file_name(item_id), &data, &why)) {
		return false;
	}
	out->assign(data.begin(), data.end());
	return true;
}
