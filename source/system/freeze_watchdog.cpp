#include "headers.hpp"
#include "freeze_watchdog.hpp"
#include "util/freeze_diag.hpp"
#include "system/fps_diag_glue.hpp"

static uint32_t freeze_watchdog_clock() { return (uint32_t)osGetTime(); }

static Thread watchdog_thread = NULL;
static std::atomic<bool> watchdog_run{false}; // set / cleared by the main thread, polled by the watchdog
static char watchdog_path[64];
static char watchdog_header[160]; // built on the main thread at start: the watchdog itself never allocates
static size_t watchdog_header_len = 0;

// direct FS IPC append: no malloc / stdio / app locks, so it still works if another thread froze inside one
static void watchdog_append(const char *line, size_t len, bool with_header) {
	static const u64 MAX_FILE_SIZE = 32 * 1024;
	FS_Archive sdmc;
	if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
		return;
	}
	Handle file;
	if (R_SUCCEEDED(FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_ASCII, watchdog_path), FS_OPEN_WRITE | FS_OPEN_CREATE,
	                                0))) {
		u64 size = 0;
		FSFILE_GetSize(file, &size);
		if (size > MAX_FILE_SIZE) {
			FSFILE_SetSize(file, 0);
			size = 0;
		}
		u32 written = 0;
		if (with_header && watchdog_header_len) {
			FSFILE_Write(file, &written, size, watchdog_header, watchdog_header_len, 0);
			size += written;
		}
		FSFILE_Write(file, &written, size, line, len, FS_WRITE_FLUSH);
		FSFILE_Close(file);
	}
	FSUSER_CloseArchive(sdmc);
}

static void watchdog_thread_func(void *) {
	freeze_diag::StallDetector detector{freeze_diag::StallDetector::Config()};
	static char line[512];
	bool header_written = false;
	while (watchdog_run) {
		usleep(250000);
#ifdef DEF_FPS_DIAG
		fps_hooks::log_drain(); // frame-pacing lines queued by the main thread (at most every few seconds)
#endif
		uint32_t now = freeze_diag::now_ms();
		uint32_t stall_ms = 0;
		auto ev = detector.tick(now, freeze_diag::board().heartbeat.load(std::memory_order_relaxed), &stall_ms);
		if (ev == freeze_diag::StallDetector::Event::NONE) {
			continue;
		}
		size_t len = freeze_diag::format_snapshot(line, sizeof(line) - 1, ev, now, stall_ms, var_app_suspended,
		                                          freeze_diag::board());
		line[len++] = '\n';
		watchdog_append(line, len, !header_written);
		header_written = true;
	}
#ifdef DEF_FPS_DIAG
	fps_hooks::log_drain(); // lines queued by exit_flush()
#endif
	threadExit(0);
}

void freeze_watchdog_start() {
	snprintf(watchdog_path, sizeof(watchdog_path), "%sfreeze_diag.log", (DEF_MAIN_DIR).c_str());
	int n = snprintf(watchdog_header, sizeof(watchdog_header),
	                 "# FourthTube freeze breadcrumbs (diagnostic only) build=%s ver=%s\n", DEF_DIAG_BUILD_ID,
	                 (DEF_CURRENT_APP_VER).c_str());
	watchdog_header_len = (n > 0 && (size_t)n < sizeof(watchdog_header)) ? (size_t)n : 0;
	freeze_diag::set_clock(freeze_watchdog_clock);
#ifdef DEF_FPS_DIAG
	fps_hooks::log_init();
#endif
	watchdog_run = true;
	// core 1, above the app's worker threads (0x23-0x25) so a busy core 0 or a busy decoder cannot starve it; it
	// sleeps 250 ms between checks
	watchdog_thread = threadCreate(watchdog_thread_func, NULL, 16 * 1024, DEF_THREAD_PRIORITY_REALTIME + 1, 1, false);
}
void freeze_watchdog_stop() {
	if (!watchdog_thread) {
		return;
	}
#ifdef DEF_FPS_DIAG
	fps_hooks::exit_flush(); // main thread: hand the open frame-pacing interval to the writer before it stops
#endif
	watchdog_run = false;
	threadJoin(watchdog_thread, 2000000000ULL);
	threadFree(watchdog_thread);
	watchdog_thread = NULL;
}
