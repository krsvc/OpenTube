// Host test of the v0.16.2 data root admission through the REAL start-up glue: main() extracted verbatim from
// source/main.cpp and everything from `namespace SceneSwitcher {` to the end of Menu_init() extracted verbatim from
// source/scene_switcher.cpp (the gate, its failure screen and Menu_init_abort() included), run against recording mocks
// (tests/host/data_root_glue_stubs/headers.hpp) and the REAL data_io/data_root.cpp on a real fixture SD card. The only
// textual change to the extracted code: the Wi-Fi shared-memory byte read becomes a mock variable, and main() is
// renamed app_main(). Built against the candidate tree (GREEN) and the preserved baseline tree (RED: no gate).
// Each scenario runs in a forked child (fresh static state).
// APT (APT_BOOTSTRAP_FIX_BRIEF.md): the REAL system/apt_handler.cpp (the normal application callback), the REAL
// system/cpu_limit.cpp and the REAL Menu_exit() (extracted) are linked. aptHook / aptUnhook keep libctru's hook list
// and aptMainLoop() can press HOME: like aptJumpToHomeMenu() it calls every registered hook with APTHOOK_ONSUSPEND,
// then APTHOOK_ONRESTORE. Safe invariant observer instead of undefined behavior: on an Old 3DS the callback's restore
// runs remove_cpu_limit(30), which reads the smallest remaining limit; without VideoPlayer_init()'s base limit the set
// is empty there (*begin() of an empty multiset). The observer records that violation and does not dispatch that
// restore.
#include "headers.hpp"
#include "scene_switcher.hpp"
#include "util/freeze_diag.hpp"
#if __has_include("data_io/data_root.hpp")
#include "data_io/data_root.hpp"
#endif

#include <algorithm>
#include <dirent.h>
#include <functional>
#include <map>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

HostLogger logger;
std::string g_sd;
bool var_is_new3ds = false;
u8 var_model = 2;
bool var_core2_available = false, var_core3_available = false;
u8 var_wifi_state = 0;
u8 mock_wifi_byte = 2; // connected: Menu_init's Wi-Fi wait loop is skipped
SceneType global_current_scene;
Intent global_intent;

// ------------------------------------------------------------------ event log and scenario knobs
static std::vector<std::string> events;
void ev(const std::string &e) { events.push_back(e); }
static int apt_calls = 0, apt_close_after = -1; // aptMainLoop() false from this call on (-1: never)
static int keys_calls = 0, press_a_at = -1;     // hidKeysDown() reports A on this call (-1: never)
static u64 fake_ms = 1000;
static std::vector<std::string> drawn;
bool mock_new3ds = false;                               // what APT_CheckNew3DS() reports
static bool home_in_gate = false, home_in_main = false; // press HOME once at the next aptMainLoop() of that phase
static bool base_cpu_limit = false;                     // VideoPlayer_init()'s add_cpu_limit(CPU_LIMIT) is in effect
static int violations = 0;
bool var_app_suspended = false;
float var_afk_time = 0;
static std::vector<aptHookCookie *> hooks; // libctru's registered hook list
void aptHook(aptHookCookie *cookie, aptHookFn callback, void *param) {
	ev("aptHook");
	cookie->callback = callback, cookie->param = param;
	hooks.push_back(cookie);
}
void aptUnhook(aptHookCookie *cookie) {
	auto it = std::find(hooks.begin(), hooks.end(), cookie);
	ev(it == hooks.end() ? "aptUnhook:unregistered" : "aptUnhook");
	if (it != hooks.end()) {
		hooks.erase(it);
	}
}
static u32 app_cpu_limit = 0;
Result APT_SetAppCpuTimeLimit(u32 percent) {
	app_cpu_limit = percent;
	ev("cpu_limit:" + std::to_string(percent));
	return 0;
}
Result APT_GetAppCpuTimeLimit(u32 *percent) { return *percent = app_cpu_limit, 0; }
void video_set_should_suspend_decoding(bool on) { ev(std::string("video_suspend_decoding:") + (on ? "1" : "0")); }
// HOME pressed: what libctru's aptJumpToHomeMenu() does with the registered hooks
static void press_home(const char *phase) {
	ev(std::string("HOME:") + phase);
	if (hooks.empty()) {
		return;
	}
	ev("hook:suspend");
	for (auto *h : std::vector<aptHookCookie *>(hooks)) {
		h->callback(APTHOOK_ONSUSPEND, h->param);
	}
	ev(std::string("suspended:") + (var_app_suspended ? "1" : "0"));
	if (!var_is_new3ds && !base_cpu_limit) {
		violations++;
		ev("VIOLATION:restore-without-base-cpu-limit");
		return; // the real restore would dereference begin() of the now empty CPU limit set
	}
	ev("hook:restore");
	for (auto *h : std::vector<aptHookCookie *>(hooks)) {
		h->callback(APTHOOK_ONRESTORE, h->param);
	}
	ev(std::string("suspended:") + (var_app_suspended ? "1" : "0"));
}

void Menu_worker_thread(void *) {}
void thumbnail_downloader_thread_func(void *) {}
void async_task_thread_func(void *) {}
void misc_tasks_thread_func(void *) {}
Thread threadCreate(ThreadFunc entry, void *, size_t, int, int, bool) {
	ev(entry == Menu_worker_thread                 ? "thread:menu_worker"
	   : entry == thumbnail_downloader_thread_func ? "thread:thumbnail"
	   : entry == async_task_thread_func           ? "thread:async"
	   : entry == misc_tasks_thread_func           ? "thread:misc"
	                                               : "thread:other");
	static char dummy;
	return (Thread)&dummy;
}
Result threadJoin(Thread, u64) { return 0; }
void threadFree(Thread) {}
void threadExit(int) {}
Result svcSetThreadPriority(Handle, s32) { return 0; }
void osSetSpeedupEnable(bool) {}
u64 osGetTime() { return fake_ms += 1000; } // one simulated second per call
void *memalign(size_t, size_t size) { return malloc(size); }
#define SVC(name)                                                                                                      \
	Result name() {                                                                                                    \
		ev(#name);                                                                                                     \
		return 0;                                                                                                      \
	}
SVC(fsInit)
SVC(acInit)
SVC(aptInit)
SVC(mcuHwcInit)
SVC(ptmuInit)
SVC(romfsInit)
SVC(cfguInit)
SVC(amInit)
SVC(ndspInit)
Result socInit(u32 *, u32) { return ev("socInit"), 0; }
Result sslcInit(Handle) { return ev("sslcInit"), 0; }
Result httpcInit(u32) { return ev("httpcInit"), 0; }
#define VOID(name)                                                                                                     \
	void name() { ev(#name); }
VOID(fsExit)
VOID(acExit)
VOID(aptExit)
VOID(mcuHwcExit)
VOID(ptmuExit)
VOID(httpcExit)
VOID(cfguExit)
VOID(amExit)
VOID(ndspExit)
VOID(sslcExit)
VOID(lock_network_state)
VOID(unlock_network_state)
VOID(Draw_exit)
VOID(Util_expl_init)
VOID(Extfont_init)
VOID(Sem_init)
VOID(Sem_suspend)
void VideoPlayer_init() { // hardware-free stand-in keeping the real CPU limit contract of video_player.cpp:668
	ev("VideoPlayer_init");
	add_cpu_limit(CPU_LIMIT);
	base_cpu_limit = true;
}
void VideoPlayer_exit() {   // video_player.cpp:841 removes CPU_LIMIT again; not replayed here: it would empty the
	ev("VideoPlayer_exit"); // set (a separate, pre-existing exit-time condition outside this brief)
	base_cpu_limit = false;
}
VOID(VideoPlayer_suspend)
VOID(Channel_init)
VOID(Channel_suspend)
VOID(About_init)
VOID(About_suspend)
VOID(History_init)
VOID(History_suspend)
VOID(Search_init)
VOID(Search_suspend)
VOID(Downloads_start)
VOID(Downloads_init)
VOID(Downloads_suspend)
VOID(Liked_init)
VOID(Liked_suspend)
VOID(Home_init)
VOID(Updater_start)
VOID(Menu_get_system_info)
VOID(freeze_watchdog_start)
VOID(Menu_exit_request)
VOID(Menu_stopping)
VOID(Channel_exit)
VOID(Search_exit)
VOID(Sem_exit)
VOID(About_exit)
VOID(History_exit)
VOID(Home_exit)
VOID(Downloads_exit)
VOID(Liked_exit)
VOID(Util_expl_exit)
VOID(Extfont_exit)
VOID(thumbnail_downloader_thread_exit_request)
VOID(async_task_thread_exit_request)
VOID(misc_tasks_thread_exit_request)
VOID(freeze_watchdog_stop)
bool Downloads_stop() { return ev("Downloads_stop"), true; }
bool Updater_stop() { return ev("Updater_stop"), true; }
void NetworkSessionList::exit_request() {}
void NetworkSessionList::at_exit() { ev("NetworkSessionList::at_exit"); }
Handle threadGetHandle(Thread) { return 1; }
Result svcWaitSynchronization(Handle, s64) { return 0; }
Result romfsExit() { return ev("romfsExit"), 0; }
Result socExit() { return ev("socExit"), 0; }
void aptSetSleepAllowed(bool) {}
static int count_ev(const std::string &e) { return (int)std::count(events.begin(), events.end(), e); }
bool aptMainLoop() {
	apt_calls++;
	bool after_init = count_ev("Home_init") > 0;
	if (home_in_gate && !after_init && count_ev("Draw_init") > 0) {
		home_in_gate = false;
		press_home("gate");
	} else if (home_in_main && after_init) {
		home_in_main = false;
		press_home("main");
	}
	return apt_close_after < 0 || apt_calls < apt_close_after;
}
Result APT_CheckNew3DS(bool *out) { return *out = mock_new3ds, 0; }
Result CFGU_GetSystemModel(u8 *m) { return *m = 2, 0; }
void hidScanInput() {}
u32 hidKeysDown() { return ++keys_calls == press_a_at ? KEY_A : 0; }
void Util_cset_set_wifi_state(bool) {}
Result_with_string Draw_init(bool) { return ev("Draw_init"), Result_with_string(); }
void Draw_frame_ready() {}
void Draw_screen_ready(int, u32) {}
void Draw_apply_draw() { ev("frame"); }
void Draw_xy_centered(std::string text, float, float, float, float, float, float, int) { drawn.push_back(text); }
void Extfont_request_extfont_status(int, bool) {}
void Extfont_request_sysfont_status(int, bool) {}
bool Menu_main() { return ev("Menu_main"), false; }
bool Menu_exit_ready() { return true; }

// ------------------------------------------------------------------ the REAL code under test
#include "menu_init.inc"
#include "menu_exit.inc"
#include "main.inc"

// ------------------------------------------------------------------ helpers
namespace {
int passed = 0, failed = 0;
#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (cond) {                                                                                                    \
			passed++;                                                                                                  \
		} else {                                                                                                       \
			failed++;                                                                                                  \
			printf("    FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

int idx(const std::string &e) {
	auto it = std::find(events.begin(), events.end(), e);
	return it == events.end() ? -1 : (int)(it - events.begin());
}
int count(const std::string &e) { return (int)std::count(events.begin(), events.end(), e); }
bool exists(const std::string &p) {
	struct stat st;
	return lstat(p.c_str(), &st) == 0;
}
void mkdirs(const std::string &path) {
	for (size_t i = 1; i <= path.size(); i++) {
		if (i == path.size() || path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0777);
		}
	}
}
void put(const std::string &path, const std::string &data) {
	FILE *fp = fopen(path.c_str(), "wb");
	fwrite(data.data(), 1, data.size(), fp);
	fclose(fp);
}
std::string get(const std::string &path) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return "<missing>";
	}
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		s.append(buf, n);
	}
	fclose(fp);
	return s;
}
void listing(const std::string &root, const std::string &rel, std::map<std::string, std::string> *out) {
	struct stat st;
	if (lstat((root + rel).c_str(), &st) != 0) {
		return;
	}
	(*out)[rel] = S_ISDIR(st.st_mode) ? "D" : std::to_string((long long)st.st_size) + ":" + get(root + rel);
	if (S_ISDIR(st.st_mode)) {
		if (DIR *d = opendir((root + rel).c_str())) {
			while (struct dirent *e = readdir(d)) {
				if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
					listing(root, rel + "/" + e->d_name, out);
				}
			}
			closedir(d);
		}
	}
}
std::map<std::string, std::string> tree(const std::string &p) {
	std::map<std::string, std::string> m;
	listing(p, "", &m);
	return m;
}
void old_fixture() {
	std::string o = g_sd + "/3ds/FourthTubeTest/";
	mkdirs(o + "downloads/dQw4w9WgXcQ-144p");
	put(o + "settings.txt", "<theme>coral</theme>\n");
	put(o + "oauth_tokens", std::string("\x01\x02secret-ish\x00\xff", 14));
	put(o + "downloads/dQw4w9WgXcQ-144p/media.mp4", std::string(300000, 'v'));
}

// consumers / workers / diagnostics that must never start before (or without) an admitted data root
const char *const AFTER_GATE[] = {"Util_expl_init",       "Extfont_init",    "thread:menu_worker", "Sem_init",
                                  "History_init",         "Downloads_start", "Liked_init",         "Home_init",
                                  "thread:thumbnail",     "thread:async",    "thread:misc",        "Updater_start",
                                  "freeze_watchdog_start"};

// one start of the app (the REAL main()) in this process
int start_app() {
	events.clear();
	drawn.clear();
	apt_calls = keys_calls = 0;
	return app_main();
}

// ------------------------------------------------------------------ scenarios (each in its own child)
// a complete start registers the normal APT callback once, after its prerequisites (VideoPlayer_init()'s base CPU
// limit, the whole app) and before the main loop; the normal shutdown removes exactly that registration once
void expect_apt_started(const char *name) {
	CHECK(count("aptHook") == 1 && idx("aptHook") > idx("VideoPlayer_init") && idx("aptHook") > idx("Updater_start") &&
	          idx("aptHook") < idx("Menu_main"),
	      "apt: %s: normal callback registered once after its prerequisites, before the main loop (hook %d, video %d, "
	      "main %d)",
	      name, idx("aptHook"), idx("VideoPlayer_init"), idx("Menu_main"));
	CHECK(count("aptUnhook") == 1 && count("aptUnhook:unregistered") == 0 && idx("aptUnhook") > idx("Menu_main"),
	      "apt: %s: normal shutdown unhooks the registered callback exactly once (%d / %d)", name, count("aptUnhook"),
	      count("aptUnhook:unregistered"));
	int first_hook = -1;
	for (size_t i = 0; i < events.size(); i++) {
		if (events[i] == "hook:suspend") {
			first_hook = (int)i;
			break;
		}
	}
	CHECK(violations == 0 && (first_hook < 0 || first_hook > idx("VideoPlayer_init")),
	      "apt: %s: normal callback dispatched before its prerequisites (violations %d)", name, violations);
}
void expect_started(const char *name, int rc) {
	CHECK(rc == 0, "%s: main returned %d", name, rc);
	for (const char *e : AFTER_GATE) {
		CHECK(count(e) == 1, "%s: %s started once (got %d)", name, e, count(e));
	}
	CHECK(idx("Menu_main") > idx("Home_init") && count("freeze_watchdog_stop") == 1, "%s: normal run and exit", name);
	CHECK(count("fsExit") == 1 && idx("fsExit") > idx("Menu_main"), "%s: services end only in the normal shutdown",
	      name);
	expect_apt_started(name);
}
void expect_refused(const char *name, int rc) {
	CHECK(rc == 0, "%s: main returned %d", name, rc);
	for (const char *e : AFTER_GATE) {
		CHECK(count(e) == 0, "%s: %s must not start (got %d)", name, e, count(e));
	}
	CHECK(count("Menu_main") == 0 && count("Menu_exit_request") == 0 && count("freeze_watchdog_stop") == 0,
	      "%s: no main loop, no normal shutdown", name);
	CHECK(count("aptHook") == 0, "apt: %s: refused start registered the normal APT callback (%d)", name,
	      count("aptHook"));
	CHECK(count("aptUnhook") + count("aptUnhook:unregistered") == 0, "apt: %s: refused start unhooked (%d / %d)", name,
	      count("aptUnhook"), count("aptUnhook:unregistered"));
	CHECK(violations == 0 && count("hook:suspend") == 0, "apt: %s: normal callback dispatched during bootstrap", name);
	// what Menu_init started up to the gate, and only that, is ended
	for (const char *e : {"unlock_network_state", "fsExit", "acExit", "aptExit", "mcuHwcExit", "ptmuExit", "httpcExit",
	                      "romfsExit", "cfguExit", "amExit", "ndspExit", "sslcExit", "socExit", "Draw_exit"}) {
		CHECK(count(e) == 1, "%s: teardown %s (got %d)", name, e, count(e));
	}
	CHECK(idx("Draw_exit") > idx("fsExit"), "%s: drawing ends last", name);
}

void scenario_fresh() {
	int rc = start_app();
	expect_started("fresh", rc);
	CHECK(exists(g_sd + "/3ds/opentube/opentube_data_root.txt") && !exists(g_sd + "/3ds/opentube.partial"),
	      "fresh: data root created through the start-up glue");
	CHECK(idx("Draw_init") < idx("Util_expl_init"), "fresh: drawing is up before the first worker");
	auto before = tree(g_sd);
	rc = start_app();
	expect_started("fresh relaunch", rc);
	CHECK(tree(g_sd) == before, "fresh relaunch: nothing on the card changes");
}
void scenario_migrate() {
	old_fixture();
	auto old_before = tree(g_sd + "/3ds/FourthTubeTest");
	int rc = start_app();
	expect_started("migrate", rc);
	auto copied = tree(g_sd + "/3ds/opentube");
	copied.erase("/opentube_data_root.txt");
	CHECK(copied == old_before, "migrate: new root holds the old files byte for byte");
	CHECK(tree(g_sd + "/3ds/FourthTubeTest") == old_before, "migrate: old root unchanged");
	bool progress = false;
	for (auto &d : drawn) {
		progress |= d.find("Copying your OpenTube data") != std::string::npos;
	}
	CHECK(progress, "migrate: progress screen drawn");
	int first_frame_after_gate = -1;
	for (size_t i = 0; i < events.size(); i++) {
		if (events[i] == "frame" && (int)i > idx("Draw_init") + 1) {
			first_frame_after_gate = (int)i;
			break;
		}
	}
	CHECK(first_frame_after_gate >= 0 && first_frame_after_gate < idx("Util_expl_init"),
	      "migrate: the copy (progress frames) runs before the first worker");
	put(g_sd + "/3ds/FourthTubeTest/settings.txt", "<theme>slate</theme>\n"); // rollback use of v0.16.1
	auto new_before = tree(g_sd + "/3ds/opentube");
	rc = start_app();
	expect_started("migrate relaunch", rc);
	CHECK(tree(g_sd + "/3ds/opentube") == new_before, "migrate relaunch: no remigration");
}
void scenario_foreign() {
	old_fixture();
	mkdirs(g_sd + "/3ds/opentube"); // an empty foreign folder
	put(g_sd + "/3ds/opentube/notes.txt", "mine");
	auto before = tree(g_sd);
	press_a_at = 3;
	int rc = start_app();
	expect_refused("foreign", rc);
	CHECK(tree(g_sd) == before, "foreign: nothing on the card changed");
	bool msg = false;
	for (auto &d : drawn) {
		msg |= d.find("already exists, but OpenTube") != std::string::npos;
	}
	CHECK(msg && count("frame") >= 3, "foreign: failure screen shown until A");
}
void scenario_unreadable_source() {
	old_fixture();
	std::string o = g_sd + "/3ds/FourthTubeTest";
	chmod(o.c_str(), 0000);
	press_a_at = 1;
	int rc = start_app();
	chmod(o.c_str(), 0755);
	expect_refused("unreadable source", rc);
	CHECK(!exists(g_sd + "/3ds/opentube") && !exists(g_sd + "/3ds/opentube.partial"),
	      "unreadable source: not treated as absent (no fresh root)");
}
void scenario_leftover() {
	old_fixture();
	mkdirs(g_sd + "/3ds/opentube.partial/downloads");
	auto before = tree(g_sd);
	press_a_at = 2;
	int rc = start_app();
	expect_refused("leftover", rc);
	CHECK(tree(g_sd) == before, "leftover: kept, nothing written");
}
void scenario_close_during_copy() {
	old_fixture();
	// APT calls: 3 scan reports (root, downloads/, the item folder), the copy's first report (before the staging folder
	// exists), then one per chunk: the 6th call is inside media.mp4 (300000 bytes, three chunks)
	apt_close_after = 6;
	int rc = start_app();
	expect_refused("close during copy", rc);
	CHECK(!exists(g_sd + "/3ds/opentube") && exists(g_sd + "/3ds/opentube.partial"),
	      "close during copy: partial output kept apart, no new root");
	bool failure_screen = false;
	for (auto &d : drawn) {
		failure_screen |= d.find("can't start") != std::string::npos;
	}
	CHECK(!failure_screen, "close during copy: no failure screen loop after a close request");
}
void scenario_bounded_screen() {
	old_fixture();
	mkdirs(g_sd + "/3ds/opentube");
	int rc = start_app(); // no button, no close: the failure screen ends on its own bound
	expect_refused("bounded", rc);
	int frames = count("frame");
	CHECK(frames > 10 && frames < 400, "bounded: failure screen ended by its time bound (%d frames)", frames);
}

// APT: the events of one HOME cycle, from `marker` to the next main loop step
std::vector<std::string> home_trace(const char *marker) {
	std::vector<std::string> res;
	int from = idx(marker);
	for (int i = from < 0 ? (int)events.size() : from + 1; i < (int)events.size() && events[i] != "Menu_main"; i++) {
		const std::string &e = events[i];
		if (e.compare(0, 10, "cpu_limit:") == 0 || e.compare(0, 10, "suspended:") == 0 ||
		    e.compare(0, 23, "video_suspend_decoding:") == 0) {
			res.push_back(e);
		}
	}
	return res;
}
std::string joined(const std::vector<std::string> &v) {
	std::string r;
	for (auto &e : v) {
		r += e + " ";
	}
	return r;
}
void scenario_apt_old3ds_progress() { // Old 3DS: HOME during the one-time copy, then HOME in the normal app
	old_fixture();
	mock_new3ds = false;
	home_in_gate = home_in_main = true;
	int rc = start_app();
	expect_started("apt old3ds progress", rc);
	CHECK(count("HOME:gate") == 1 && idx("HOME:gate") < idx("VideoPlayer_init") && count("HOME:main") == 1,
	      "apt: old3ds progress: HOME pressed during the copy and in the app");
	CHECK(exists(g_sd + "/3ds/opentube/opentube_data_root.txt") && !exists(g_sd + "/3ds/opentube.partial"),
	      "apt: old3ds progress: the copy completed across HOME");
	std::vector<std::string> want = {"video_suspend_decoding:1", "cpu_limit:30", "suspended:1",
	                                 "video_suspend_decoding:0", "cpu_limit:65", "suspended:0"};
	CHECK(home_trace("HOME:main") == want, "apt: old3ds progress: REAL callback in the app: %s",
	      joined(home_trace("HOME:main")).c_str());
}
void scenario_apt_old3ds_failure() { // Old 3DS: HOME on the refused-start screen
	old_fixture();
	mkdirs(g_sd + "/3ds/opentube"); // foreign: the start is refused
	mock_new3ds = false;
	home_in_gate = true;
	press_a_at = 3;
	int rc = start_app();
	expect_refused("apt old3ds failure", rc);
	CHECK(count("HOME:gate") == 1 && drawn.size() > 0, "apt: old3ds failure: HOME pressed on the failure screen");
}
void scenario_apt_new3ds_progress() { // New 3DS control: the callback never touches CPU limits there
	old_fixture();
	mock_new3ds = true;
	home_in_gate = home_in_main = true;
	int rc = start_app();
	expect_started("apt new3ds progress", rc);
	std::vector<std::string> want = {"suspended:1", "suspended:0"};
	CHECK(home_trace("HOME:main") == want, "apt: new3ds progress: REAL callback in the app: %s",
	      joined(home_trace("HOME:main")).c_str());
}
void scenario_apt_new3ds_failure() {
	old_fixture();
	mkdirs(g_sd + "/3ds/opentube.partial");
	mock_new3ds = true;
	home_in_gate = true;
	press_a_at = 2;
	int rc = start_app();
	expect_refused("apt new3ds failure", rc);
	CHECK(count("HOME:gate") == 1, "apt: new3ds failure: HOME pressed on the failure screen");
}

typedef void (*Scenario)();
struct Named {
	const char *name;
	Scenario fn;
};
} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <fresh fixture dir> [red]\n", argv[0]);
		return 2;
	}
	std::string base = argv[1];
	bool red = argc > 2 && std::string(argv[2]) == "red";
	if (exists(base)) {
		fprintf(stderr, "refusing to reuse %s (no deletion)\n", base.c_str());
		return 2;
	}
	mkdirs(base);
	Named all[] = {{"fresh", scenario_fresh},
	               {"migrate", scenario_migrate},
	               {"foreign", scenario_foreign},
	               {"unreadable_source", scenario_unreadable_source},
	               {"leftover", scenario_leftover},
	               {"close_during_copy", scenario_close_during_copy},
	               {"bounded_screen", scenario_bounded_screen},
	               {"apt_old3ds_progress", scenario_apt_old3ds_progress},
	               {"apt_old3ds_failure", scenario_apt_old3ds_failure},
	               {"apt_new3ds_progress", scenario_apt_new3ds_progress},
	               {"apt_new3ds_failure", scenario_apt_new3ds_failure}};
	int total_passed = 0, total_failed = 0, red_scenarios = 0;
	for (auto &s : all) {
		printf("%s\n", s.name);
		fflush(stdout);
		int fd[2];
		pipe(fd);
		pid_t pid = fork();
		if (pid == 0) {
			close(fd[0]);
			g_sd = base + "/" + s.name + "/sd";
			mkdirs(g_sd + "/3ds");
			alarm(60);
			s.fn();
			fflush(stdout);
			char buf[64];
			int n = snprintf(buf, sizeof(buf), "%d %d", passed, failed);
			write(fd[1], buf, n);
			_exit(0);
		}
		close(fd[1]);
		char buf[64] = {0};
		read(fd[0], buf, sizeof(buf) - 1);
		close(fd[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		int p = 0, f = 0;
		if (sscanf(buf, "%d %d", &p, &f) != 2 || !WIFEXITED(status)) {
			printf("    FAIL %s: child ended abnormally (status %d)\n", s.name, status);
			f++;
		}
		total_passed += p, total_failed += f;
		red_scenarios += f > 0;
	}
	printf("data_root glue%s: %d checks passed, %d failed (%d of %zu scenarios failing)\n", red ? " (RED run)" : "",
	       total_passed, total_failed, red_scenarios, sizeof(all) / sizeof(all[0]));
	if (red) {
		return total_failed ? 0 : 1; // RED: the baseline must fail behaviorally
	}
	return total_failed ? 1 : 0;
}
