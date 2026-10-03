#include "headers.hpp"
#include "updater/update_app.hpp"
#include "updater/update_net.hpp"
#include "scenes/video_player.hpp"
#include "downloads/downloads_app.hpp"
#include "ui/shell.hpp"
#include "util/activity_gate.hpp"

// OpenTube r15 updater glue (update_app.hpp). The model is only touched under `lock`; the procedures run on the worker
// without it (their progress / abort callbacks take it briefly). r15b: the installation is admitted by the activity
// gate's reservation (util/activity_gate.hpp), taken before the INSTALL job is queued and released by the worker
// after the system installer finished / aborted (or by the exit when the job was dropped unclaimed). Lock order:
// `lock` -> gate (the gate is a leaf); the gate is never held while `lock` is taken.

using namespace updater;

namespace {
Mutex lock;
Model model;
bool is_3dsx = false;
volatile bool stop_requested = false;
Thread worker = NULL;
Handle worker_handle = 0;
u64 started_ms = 0;
bool badge_shown = false;

const Version CURRENT = {OPENTUBE_VERSION_MAJOR, OPENTUBE_VERSION_MINOR, OPENTUBE_VERSION_MICRO};
const char *const TRUST_BUNDLE = "romfs:/cert/update_roots.pem";
const std::string STAGE_DIR = DEF_MAIN_DIR + "update"; // app data root: an app-owned folder, fixed names only
const char *const STAGE_FILE = "OpenTube.cia.part";

struct AmInstaller : Installer {
	Handle h = 0;
	static std::string hex(Result r) {
		char buf[16];
		snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)r);
		return buf;
	}
	bool begin(std::string *err) override {
		Result r = AM_StartCiaInstall(MEDIATYPE_SD, &h);
		if (R_FAILED(r)) {
			h = 0;
			*err = "AM_StartCiaInstall " + hex(r);
			return false;
		}
		return true;
	}
	bool write(uint64_t offset, const uint8_t *p, size_t n, std::string *err) override {
		u32 written = 0;
		Result r = FSFILE_Write(h, &written, offset, p, (u32)n, FS_WRITE_FLUSH);
		if (R_FAILED(r)) {
			*err = "FSFILE_Write " + hex(r);
			return false;
		}
		if (written != n) { // a short (or zero) write is an error: never retried in a loop
			*err = "wrote " + std::to_string(written) + " of " + std::to_string(n) + " bytes";
			return false;
		}
		return true;
	}
	bool finish(std::string *err) override {
		Result r = AM_FinishCiaInstall(h);
		h = 0;
		if (R_FAILED(r)) {
			*err = "AM_FinishCiaInstall " + hex(r);
			return false;
		}
		return true;
	}
	bool abort(std::string *err) override {
		Result r = AM_CancelCIAInstall(h);
		h = 0;
		if (R_FAILED(r)) {
			*err = "AM_CancelCIAInstall " + hex(r);
			return false;
		}
		return true;
	}
};
struct AppEnv : Environment {
	bool online() override { return var_wifi_state == 2; }
	bool install_reserved() override { return activity_gate().install_reserved(); }
	bool exit_requested() override { return stop_requested; }
	bool start_guarded(const std::function<bool()> &start, bool *exit_refused) override {
		lock.lock(); // AM_StartCiaInstall runs inside: an exit request is ordered before it (refused) or after it
		bool ok = model.start_install(start, exit_refused);
		lock.unlock();
		return ok;
	}
};

bool cancel_or_stop() {
	if (stop_requested) {
		return true;
	}
	lock.lock();
	bool c = model.cancel_requested;
	lock.unlock();
	return c;
}
Progress progress_reporter() {
	Progress p;
	p.report = [](uint64_t done, uint64_t total) {
		lock.lock();
		model.done = done, model.total = total;
		lock.unlock();
		var_need_refresh = true;
	};
	return p;
}

struct AppWorker : WorkerHost {
	CurlTransport *transport = NULL;
	std::string trust_error;
	StageFs *stage = new_posix_stage_fs(STAGE_DIR, STAGE_FILE);
	AmInstaller installer;
	AppEnv env;
	~AppWorker() {
		delete stage;
		delete transport;
	}
	void lock() override { ::lock.lock(); }
	void unlock() override { ::lock.unlock(); }
	Model &model() override { return ::model; }
	void idle() override { usleep(100000); }
	bool ensure_transport() {
		if (!transport) {
			std::string pem;
			if (load_ca_bundle(TRUST_BUNDLE, &pem, &trust_error)) { // no bundle: no connection at all
				transport = new CurlTransport(pem, "OpenTube/" + version_string(CURRENT) +
				                                       " (Nintendo 3DS; +https://github.com/krsvc/OpenTube)");
			}
		}
		return transport != NULL;
	}
	CheckResult check() override {
		CheckResult r;
		if (!ensure_transport()) {
			r.message = "Update checks are unavailable: " + trust_error + ".";
		} else {
			r = check_latest(*transport, CURRENT, cancel_or_stop);
		}
		logger.info("updater", "check: " + r.message);
		return r;
	}
	Staged download(const Release &release) override {
		Staged s;
		if (!ensure_transport()) {
			s.message = "Update downloads are unavailable: " + trust_error + ".";
		} else {
			s = download_and_verify(*transport, *stage, release, CURRENT, cancel_or_stop, progress_reporter());
		}
		logger.info("updater", "download: " + s.message);
		return s;
	}
	InstallResult install(const Staged &staged) override {
		// irrevocable part: no HOME menu, no cancel; the app's own exit waits for this thread
		aptSetHomeAllowed(false);
		InstallResult r = install_staged(*stage, installer, env, staged, progress_reporter());
		aptSetHomeAllowed(true);
		logger.info("updater", "install: " + r.message);
		return r;
	}
	void job_finished(Job job) override {
		if (job == Job::INSTALL) {
			activity_gate().release_install(); // the system installer finished or aborted (or never started)
		}
		var_need_refresh = true;
	}
};

void worker_entry(void *) {
	logger.info("updater", "worker started (OpenTube " + version_string(CURRENT) + ")");
	{
		AppWorker w;
		worker_loop(w);
	}
	logger.info("updater", "worker exit");
	threadExit(0);
}
} // namespace

void Updater_start() {
	if (worker) {
		return;
	}
	is_3dsx = envIsHomebrew();
	started_ms = osGetTime();
	worker = threadCreate(worker_entry, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_LOW, 0, false);
	if (worker) {
		worker_handle = threadGetHandle(worker);
	} else {
		logger.error("updater", "worker thread creation failed: updates unavailable");
	}
}
void Updater_request_stop() {
	lock.lock();
	Job dropped = model.request_stop(); // atomic with the worker's claim: no job is taken after this
	lock.unlock();
	stop_requested = true; // abort polls of a running check / download
	if (dropped == Job::INSTALL) {
		activity_gate().release_install(); // queued, never claimed: nothing was started
	}
}
bool Updater_finished() { return !worker || svcWaitSynchronization(worker_handle, 0) == 0; }
bool Updater_stop() {
	if (!worker) {
		return true;
	}
	Updater_request_stop();
	Result join = svcWaitSynchronization(worker_handle, 10000000000ULL);
	logger.info("updater", "svcWaitSynchronization()...", join);
	if (join != 0) {
		logger.error("updater", "worker still running after the exit bound; its resources are kept");
		return false;
	}
	threadFree(worker);
	worker = NULL;
	worker_handle = 0;
	return true;
}

// once per session: after a minute, connected, nothing playing / downloading, not inside the player; never at boot
void Updater_idle_tick() {
	if (!worker || stop_requested) {
		return;
	}
	lock.lock();
	bool pending = !model.auto_checked;
	bool available = model.state == State::AVAILABLE || model.state == State::READY;
	lock.unlock();
	if (available != badge_shown) {
		badge_shown = shell::you_badge = available;
		var_need_refresh = true;
	}
	if (!pending || osGetTime() - started_ms < 60000 || var_wifi_state != 2 || video_playback_active() ||
	    (downloads_running() && downloads_manager().busy()) || global_current_scene == SceneType::VIDEO_PLAYER) {
		return;
	}
	std::string msg;
	lock.lock();
	model.request_check(true, &msg);
	lock.unlock();
}

UpdaterView Updater_snapshot() {
	UpdaterView v;
	lock.lock();
	v.state = model.state;
	v.message = model.message;
	v.release = model.release;
	v.staged = model.staged;
	v.done = model.done, v.total = model.total;
	lock.unlock();
	v.is_3dsx = is_3dsx;
	return v;
}
std::string Updater_current_version() { return version_string(CURRENT); }
bool Updater_check(std::string *msg) {
	lock.lock();
	bool ok = worker && model.request_check(false, msg);
	lock.unlock();
	if (!worker) {
		*msg = "Updates are unavailable.";
	}
	return ok;
}
bool Updater_download(const Release &shown, std::string *msg) {
	if (is_3dsx) {
		*msg = "The .3dsx version is updated manually.";
		return false;
	}
	lock.lock();
	bool ok = model.request_download(shown, msg);
	lock.unlock();
	return ok;
}
// advisory only (before the confirmation dialog); the admission itself is the reservation in Updater_install()
std::string Updater_install_blocker() {
	if (activity_gate().playback_admitted()) {
		return "Stop the video first (X + B in the player).";
	}
	if (activity_gate().downloads_held()) {
		return "The download library is busy. Wait for the download or cancel it first.";
	}
	return "";
}
bool Updater_install(const std::string &shown_sha256, std::string *msg) {
	if (is_3dsx) {
		*msg = "The .3dsx version is updated manually.";
		return false;
	}
	if (!worker) {
		*msg = "Updates are unavailable.";
		return false;
	}
	// the reservation is the admission: from here no playback and no download-library work can start until the
	// worker releases it after the system installer finished / aborted (or the exit drops the unclaimed job)
	if (!activity_gate().reserve_install(msg)) {
		return false;
	}
	lock.lock();
	bool ok = model.request_install(shown_sha256, msg);
	lock.unlock();
	if (!ok) {
		activity_gate().release_install();
	}
	return ok;
}
bool Updater_cancel(std::string *msg) {
	lock.lock();
	bool ok = model.request_cancel(msg);
	lock.unlock();
	return ok;
}
bool Updater_install_running() {
	lock.lock();
	bool r = model.install_running();
	lock.unlock();
	return r;
}
