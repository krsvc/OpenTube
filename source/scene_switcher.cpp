#include "headers.hpp"

#include "scenes/video_player.hpp"
#include "scenes/search.hpp"
#include "scenes/channel.hpp"
#include "scenes/about.hpp"
#include "scenes/setting_menu.hpp"
#include "scenes/watch_history.hpp"
#include "scenes/home.hpp"
#include "scenes/downloads.hpp"
#include "downloads/downloads_app.hpp"
#include "scenes/liked_videos.hpp"
#include "updater/update_app.hpp"
#include "network_decoder/network_io.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "util/async_task.hpp"
#include "util/misc_tasks.hpp"
#include "ui/ui.hpp"
#include "util/freeze_diag.hpp"
#include "system/freeze_watchdog.hpp"
// add here

SceneType global_current_scene;
Intent global_intent;

namespace SceneSwitcher {
static bool menu_thread_run = false;
static bool menu_check_exit_request = false;
static Thread menu_worker_thread, thumbnail_downloader_thread, async_task_thread, misc_tasks_thread;
static Handle async_task_handle = 0; // OS handle of async_task_thread (the stream resolver's owner), see below

static void empty_thread(void *arg) { threadExit(0); }

bool manual_screen_off = false;

static Result sound_init_result;
}; // namespace SceneSwitcher
using namespace SceneSwitcher;

void Menu_worker_thread(void *arg);
#define LOG_IF_ERROR(expr)                                                                                             \
	do {                                                                                                               \
		auto res = expr;                                                                                               \
		if (res != 0)                                                                                                  \
			logger.error(DEF_MENU_INIT_STR, #expr ": " + std::to_string(res));                                         \
	} while (0)

void Menu_init(void) {
	Result_with_string result;

	logger.init();

	logger.info(DEF_MENU_INIT_STR, "Initializing...");
	logger.info(DEF_MENU_INIT_STR, "Version is \"" + DEF_CURRENT_APP_VER + "\"");

	osSetSpeedupEnable(true);
	svcSetThreadPriority(CUR_THREAD_HANDLE, DEF_THREAD_PRIORITY_HIGH - 1);
	freeze_diag::register_thread(freeze_diag::MAIN); // diagnostic breadcrumbs only
	freeze_diag::phase("main:init");

	logger.info(DEF_MENU_INIT_STR, "Initializing services...");
	LOG_IF_ERROR(fsInit());
	LOG_IF_ERROR(acInit());
	LOG_IF_ERROR(aptInit());
	LOG_IF_ERROR(mcuHwcInit());
	LOG_IF_ERROR(ptmuInit());
	{
		constexpr int SOC_BUFFERSIZE = 0x100000;
		u32 *soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFERSIZE);
		if (!soc_buffer) {
			logger.error(DEF_MENU_INIT_STR, "soc buffer out of memory");
		} else {
			LOG_IF_ERROR(socInit(soc_buffer, SOC_BUFFERSIZE));
		}
	}
	LOG_IF_ERROR(sslcInit(0));
	LOG_IF_ERROR(httpcInit(0x200000));
	LOG_IF_ERROR(romfsInit());
	LOG_IF_ERROR(cfguInit());
	LOG_IF_ERROR(amInit());
	LOG_IF_ERROR((sound_init_result = ndspInit())); // 0xd880A7FA if ndsp firmware is lacking
	LOG_IF_ERROR(APT_SetAppCpuTimeLimit(30));
	lock_network_state();

	aptSetSleepAllowed(false);
	set_apt_callback();

	logger.info(DEF_MENU_INIT_STR, "Services initialized.");

	APT_CheckNew3DS(&var_is_new3ds);
	CFGU_GetSystemModel(&var_model);

	if (var_is_new3ds) { // check core availability
		Thread core_2 = threadCreate(empty_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 2, false);
		var_core2_available = (bool)core_2;
		if (core_2) {
			threadJoin(core_2, U64_MAX);
		}
		threadFree(core_2);

		Thread core_3 = threadCreate(empty_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 3, false);
		var_core3_available = (bool)core_3;
		if (core_3) {
			threadJoin(core_3, U64_MAX);
		}
		threadFree(core_3);
	}

	LOG_IF_ERROR(Draw_init(var_model != CFG_MODEL_2DS).code);
	Draw_frame_ready();
	Draw_screen_ready(0, DEF_DRAW_BLACK); // Black prevents flashing.
	Draw_screen_ready(1, DEF_DRAW_BLACK); // Same here
	Draw_apply_draw();

	Util_expl_init();
	Extfont_init();
	for (int i = 0; i < FONT_BLOCK_NUM; i++) {
		Extfont_request_extfont_status(i, true);
	}
	for (int i = 0; i < SYSTEM_FONT_NUM; i++) {
		Extfont_request_sysfont_status(i, true);
	}

	menu_thread_run = true;
	menu_worker_thread =
	    threadCreate(Menu_worker_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_REALTIME, 1, false);

	// Get Wi-Fi state from shared memory #0x1FF81067
	var_wifi_state = *(u8 *)0x1FF81067;
	// Wait for the 3DS's Wi-Fi to (hopefully) hurry up.
	int tries = 0;
	while (var_wifi_state != 2) {
		tries++;
		if (tries > 20) {
			break;
		}
		// Enable Wi-Fi in case the user disabled it for whatever reason.
		if (tries < 2) {
			Util_cset_set_wifi_state(true);
		}
		// Sleep for one second. This will keep running until the console finally connects, or if we exceed 20 tries.
		usleep(1000000);
		// Fetch the updated Wi-Fi state
		var_wifi_state = *(u8 *)0x1FF81067;
		if (var_wifi_state == 2) {
			// Sometimes takes a bit longer to actually have connectivity for some reason.
			usleep(2500000);
		}
	}

	Sem_init();
	Sem_suspend();
	VideoPlayer_init();
	VideoPlayer_suspend();
	Channel_init();
	Channel_suspend();
	About_init();
	About_suspend();
	History_init();
	History_suspend();
	Search_init();
	Search_suspend();
	Downloads_start(); // library scan + worker thread (all SD / network work happens there)
	Downloads_init();
	Downloads_suspend();
	Liked_init(); // r15: loads the liked videos list
	Liked_suspend();
	// add here
	Home_init(); // first running
	global_current_scene = SceneType::HOME;

	thumbnail_downloader_thread = threadCreate(thumbnail_downloader_thread_func, (void *)(""), DEF_STACKSIZE,
	                                           DEF_THREAD_PRIORITY_NORMAL, 0, false);
	async_task_thread = threadCreate(async_task_thread_func, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	misc_tasks_thread = threadCreate(misc_tasks_thread_func, NULL, DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	Updater_start(); // r15: idle worker; no network until a check is requested (never at boot)

	Menu_get_system_info();
#ifdef DEF_DIAG_BUILD
	freeze_watchdog_start(); // diagnostic only: snapshots thread phases if the main loop stops
#endif

	logger.info(DEF_MENU_INIT_STR, "Initialized.");
}

// Taken before the async thread is asked to exit (it cannot have finished before that; threadGetHandle() returns ~0
// once libctru's "finished" flag is set). Completion is only this handle's kernel signal: threadJoin() returns 0 on the
// flag alone, which threadExit() sets before its last write to the thread object and svcExitThread().
static void Menu_exit_track_async(void) {
	if (async_task_thread && !async_task_handle) {
		async_task_handle = threadGetHandle(async_task_thread);
	}
}
void Menu_exit_request(void) {
	// r15c: the FIRST state change of the exit, before any log, phase, flag or other subsystem stop. Linearization point:
	// Updater_request_stop()'s critical section on the updater lock (atomic with the worker's job claim and its guarded
	// AM_StartCiaInstall). A claim / start that held that lock first runs to its end (the exit waits for it); none
	// happens after it. Idempotent: a repeated request finds the updater already stopping and changes nothing.
	Updater_request_stop();
	static bool requested = false;
	if (requested) {
		return; // a second request (e.g. the system's close after a menu exit) changes nothing
	}
	requested = true;
	logger.info(DEF_MENU_EXIT_STR, "Exit requested: stopping downloads and network work...");
	freeze_diag::phase("main:exit_request");
	Downloads_request_stop();           // no new jobs; aborts the transfer / pause / resolve wait; drops a queued resolve
	Menu_exit_track_async();
	async_task_thread_exit_request();   // no further async task starts; the running one (a page load) is awaited
	NetworkSessionList::exit_request(); // in-flight requests abort at their next progress callback
}
// what the exit still waits for; "" once the kernel has ended the download worker and the async thread (stream
// resolution). A zero-timeout wait only checks, it never blocks; only 0 counts as ended.
static std::string Menu_exit_pending(void) {
	if (!Downloads_finished()) {
		return "the download";
	}
	if (async_task_thread && svcWaitSynchronization(async_task_handle, 0) != 0) {
		return "a network request";
	}
	if (!Updater_finished()) {
		return Updater_install_running() ? "the update installation" : "the update check";
	}
	return "";
}
bool Menu_exit_ready(void) { return Menu_exit_pending().empty(); }
void Menu_stopping(void) {
	static bool closing = false; // the system asked the app to close: the screens are no longer ours to draw
	static u64 since = osGetTime(), next_log_s = 0;
	freeze_diag::heartbeat();
	freeze_diag::phase("main:stopping");
	std::string pending = Menu_exit_pending();
	if (pending.empty()) {
		return;
	}
	u64 waited_s = (osGetTime() - since) / 1000;
	if (waited_s >= next_log_s) {
		logger.info(DEF_MENU_EXIT_STR, "waiting for " + pending + " to stop (" + std::to_string(waited_s) + " s)");
		next_log_s = waited_s + 5;
	}
	if (!closing && !aptMainLoop()) {
		closing = true;
	}
	if (closing) {
		usleep(50000);
		return;
	}
	Draw_frame_ready();
	Draw_screen_ready(0, POCKET_DARK_BACKGROUND);
	Draw_xy_centered("Closing...\n\nWaiting for " + pending + " to stop (" + std::to_string(waited_s) +
	                     " s).\nA network lookup in progress cannot be interrupted;\n"
	                     "the app closes as soon as it returns.",
	                 0, 400, 0, 240, 0.5, 0.5, POCKET_DARK_TEXT);
	Draw_screen_ready(1, POCKET_DARK_BACKGROUND);
	Draw_apply_draw();
}

void Menu_exit(void) {
	logger.info(DEF_MENU_EXIT_STR, "Exiting...");
	u64 time_out = 10000000000;
	Result_with_string result;
	freeze_diag::phase("main:exit");
	freeze_watchdog_stop();

	menu_thread_run = false;

	// main() commits the exit only after Menu_exit_ready(): the download worker and the async thread have already
	// ended, so these joins return at once. The live-after-bound handling below stays as a defensive fallback.
	bool downloads_stopped = Downloads_stop();
	bool updater_stopped = Updater_stop(); // r15: joined by its kernel handle before any service below ends
	VideoPlayer_exit();
	Channel_exit();
	Search_exit();
	Sem_exit();
	About_exit();
	History_exit();
	Home_exit();
	Downloads_exit();
	Liked_exit();
	// add here

	Util_expl_exit();
	Extfont_exit();

	thumbnail_downloader_thread_exit_request();
	Menu_exit_track_async(); // no-op after Menu_exit_request()
	async_task_thread_exit_request();
	misc_tasks_thread_exit_request();
	NetworkSessionList::exit_request();
	unlock_network_state();

	remove_apt_callback();

	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(menu_worker_thread, time_out));
	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(thumbnail_downloader_thread, time_out));
	// may be inside a page load / stream resolution; the kernel wait, not threadJoin()'s "finished" fast path
	Result async_join = async_task_thread ? svcWaitSynchronization(async_task_handle, time_out) : 0;
	logger.info(DEF_MENU_EXIT_STR, "svcWaitSynchronization()...", async_join);
	logger.info(DEF_MENU_EXIT_STR, "threadJoin()...", threadJoin(misc_tasks_thread, time_out));
	threadFree(menu_worker_thread);
	threadFree(thumbnail_downloader_thread);
	if (async_join == 0) { // a timeout is 0x09401BFE, which R_FAILED() does not report
		threadFree(async_task_thread);
	}
	threadFree(misc_tasks_thread);

	// a thread still live after its bound may still use the network sessions, the sockets and the SD card: those
	// are left to the process exit instead of being freed under it
	bool network_users_live = !downloads_stopped || async_join != 0 || !updater_stopped;
	if (network_users_live) {
		logger.error(DEF_MENU_EXIT_STR, "a network / SD user is still live: sessions, sockets and fs kept");
	}
	if (!network_users_live) {
		NetworkSessionList::at_exit();
		fsExit();
	}
	acExit();
	aptExit();
	mcuHwcExit();
	ptmuExit();
	httpcExit();
	romfsExit();
	cfguExit();
	amExit();
	ndspExit();
	sslcExit();
	if (!network_users_live) {
		socExit();
	}
	Draw_exit();

	logger.info(DEF_MENU_EXIT_STR, "Exited.");
}

static std::vector<Intent> scene_stack = {{SceneType::HOME, ""}};

bool Menu_main(void) {
	freeze_diag::heartbeat();
	freeze_diag::phase("main:scene");
	Updater_idle_tick(); // r15: the once-per-session update check (idle, connected) and the You badge
#ifdef DEF_FPS_DIAG
	video_fps_diag_tick();
#endif
	Util_hid_update_key_state();

	Hid_info key;
	Util_hid_query_key_state(&key);

	if (sound_init_result != 0) {
		std::string error_msg = "Could not initialize NDSP (sound service)\n"
		                        "This is probably because you haven't run DSP1.\n"
		                        "You can download it from the link below:\n\n"
		                        "https://github.com/zoogie/DSP1/releases/\n\n"
		                        "If you are on Luma3DS v11.0 or later, you\n"
		                        "can open Rosalina menu, go to Misc. options\n"
		                        " -> Dump DSP firmware instead.\n";
		error_msg += "\n\nPress A to close the app";

		Draw_frame_ready();
		Draw_screen_ready(0, POCKET_DARK_BACKGROUND);

		Draw_xy_centered(error_msg, 0, 400, 0, 240, 0.5, 0.5, POCKET_DARK_TEXT);
		Draw_top_ui();

		Draw_screen_ready(1, POCKET_DARK_BACKGROUND);

		Draw_apply_draw();

		static int frame_cnt = 0;
		if (++frame_cnt >= 30) {
			frame_cnt = 0;
			logger.info(DEF_MENU_INIT_STR, "ndspInit() retry...", (sound_init_result = ndspInit())); // 0xd880A7FA
		}

		if (key.h_a) {
			return false;
		}
		return true;
	}

	if (var_show_fps) {
		sprintf(var_status, "%02dFPS %04d/%02d/%02d %02d:%02d:%02d ", (int)Draw_query_fps(), var_years, var_months,
		        var_days, var_hours, var_minutes, var_seconds);
	} else {
		sprintf(var_status, "%04d/%02d/%02d %02d:%02d:%02d ", var_years, var_months, var_days, var_hours, var_minutes,
		        var_seconds);
	}

	if (var_debug_mode) {
		var_need_refresh = true;
	}

	global_intent = Intent();
	if (global_current_scene == SceneType::VIDEO_PLAYER) {
		VideoPlayer_draw();
	} else if (global_current_scene == SceneType::SEARCH) {
		Search_draw();
	} else if (global_current_scene == SceneType::CHANNEL) {
		Channel_draw();
	} else if (global_current_scene == SceneType::SETTINGS) {
		Sem_draw();
	} else if (global_current_scene == SceneType::ABOUT) {
		About_draw();
	} else if (global_current_scene == SceneType::HISTORY) {
		History_draw();
	} else if (global_current_scene == SceneType::HOME) {
		Home_draw();
	} else if (global_current_scene == SceneType::DOWNLOADS) {
		Downloads_draw();
	} else if (global_current_scene == SceneType::LIKED) {
		Liked_draw();
	}
	// add here

	if (global_intent.next_scene != SceneType::NO_CHANGE) {
		if (global_current_scene == SceneType::VIDEO_PLAYER) {
			VideoPlayer_suspend();
		} else if (global_current_scene == SceneType::SEARCH) {
			Search_suspend();
		} else if (global_current_scene == SceneType::CHANNEL) {
			Channel_suspend();
		} else if (global_current_scene == SceneType::SETTINGS) {
			Sem_suspend();
		} else if (global_current_scene == SceneType::ABOUT) {
			About_suspend();
		} else if (global_current_scene == SceneType::HISTORY) {
			History_suspend();
		} else if (global_current_scene == SceneType::HOME) {
			Home_suspend();
		} else if (global_current_scene == SceneType::DOWNLOADS) {
			Downloads_suspend();
		} else if (global_current_scene == SceneType::LIKED) {
			Liked_suspend();
		}
		// add here
	}

	// common updates
	if (key.h_select && key.p_y) {
		var_debug_mode = !var_debug_mode;
	}
	if (key.h_select && key.h_r && key.p_a) {
		var_show_fps = !var_show_fps;
	}
	if (key.h_select && key.p_x) {
		logger.draw_enabled ^= 1, var_need_refresh = true; // toggle log drawing
	}
	logger.update(key);
	if (key.h_touch || key.p_touch) {
		var_need_refresh = true;
	}
	if (((key.h_select && key.p_start) || (key.h_start && key.p_select)) && var_model != CFG_MODEL_2DS) {
		manual_screen_off = !manual_screen_off;
	}

	if (global_intent.next_scene == SceneType::EXIT) {
		return false;
	} else if (global_intent.next_scene != SceneType::NO_CHANGE && global_intent != scene_stack.back()) {
		if (scene_stack.size() >= 2 && global_intent == scene_stack[scene_stack.size() - 2]) {
			global_intent.next_scene = SceneType::BACK;
		}
		if (global_intent.next_scene == SceneType::BACK) {
			if (scene_stack.size() >= 2) {
				scene_stack.pop_back();
			}
		} else {
			scene_stack.push_back(global_intent);
		}

		global_current_scene = scene_stack.back().next_scene;
		std::string arg = scene_stack.back().arg;

		if (global_current_scene == SceneType::VIDEO_PLAYER) {
			VideoPlayer_resume(arg);
		} else if (global_current_scene == SceneType::SEARCH) {
			Search_resume(arg);
		} else if (global_current_scene == SceneType::CHANNEL) {
			Channel_resume(arg);
		} else if (global_current_scene == SceneType::SETTINGS) {
			Sem_resume(arg);
		} else if (global_current_scene == SceneType::ABOUT) {
			About_resume(arg);
		} else if (global_current_scene == SceneType::HISTORY) {
			History_resume(arg);
		} else if (global_current_scene == SceneType::HOME) {
			Home_resume(arg);
		} else if (global_current_scene == SceneType::DOWNLOADS) {
			Downloads_resume(arg);
		} else if (global_current_scene == SceneType::LIKED) {
			Liked_resume(arg);
		}
		// add here
	}

	return true;
}

void Menu_get_system_info(void) {
	u8 battery_level = -1;
	u8 battery_voltage = -1;
	char *ssid = (char *)malloc(512);
	Result_with_string result;

	PTMU_GetBatteryChargeState(&var_battery_charge);      // battery charge
	result.code = MCUHWC_GetBatteryLevel(&battery_level); // battery level(%)
	if (result.code == 0) {
		MCUHWC_GetBatteryVoltage(&battery_voltage);
		var_battery_voltage = 5.0 * (battery_voltage / 256);
		var_battery_level_raw = battery_level;
		var_battery_level_exact = true;
	} else {
		var_battery_level_exact = false; // r16: coarse steps below, the status bar shows the icon only
		PTMU_GetBatteryLevel(&battery_level);
		if ((int)battery_level == 0) {
			var_battery_level_raw = 0;
		} else if ((int)battery_level == 1) {
			var_battery_level_raw = 5;
		} else if ((int)battery_level == 2) {
			var_battery_level_raw = 10;
		} else if ((int)battery_level == 3) {
			var_battery_level_raw = 30;
		} else if ((int)battery_level == 4) {
			var_battery_level_raw = 60;
		} else if ((int)battery_level == 5) {
			var_battery_level_raw = 100;
		}
	}

	// ssid
	result.code = ACU_GetSSID(ssid);
	if (result.code == 0) {
		var_connected_ssid = ssid;
	} else {
		var_connected_ssid = "";
	}

	free(ssid);
	ssid = NULL;

	var_wifi_signal = osGetWifiStrength();
	var_wifi_state = *(u8 *)0x1FF81067;
	if (var_wifi_state != 2) {
		var_wifi_signal = 8;
	}

	// Get time
	time_t unixTime = time(NULL);
	struct tm *timeStruct = gmtime((const time_t *)&unixTime);
	var_years = timeStruct->tm_year + 1900;
	var_months = timeStruct->tm_mon + 1;
	var_days = timeStruct->tm_mday;
	var_hours = timeStruct->tm_hour;
	var_minutes = timeStruct->tm_min;
	var_seconds = timeStruct->tm_sec;

	if (var_debug_mode) {
		// check free RAM
		var_free_ram = Menu_check_free_ram();
		var_free_linear_ram = linearSpaceFree();
	}
}

int Menu_check_free_ram(void) {
	void *ptr[10000];
	int head = 0;
	int res = 0;

	int cur_size = 1000 * 1000;
	while (head < 10000 && cur_size >= 10000) {
		ptr[head] = malloc(cur_size);
		if (ptr[head]) {
			res += cur_size, head++;
		} else {
			cur_size /= 10;
		}
	}
	for (int i = 0; i < head; i++) {
		free(ptr[i]);
	}
	return res / 1000;
}

void Menu_worker_thread(void *arg) {
	logger.info(DEF_MENU_WORKER_THREAD_STR, "Thread started.");
	int count = 0;
	Result_with_string result;

	while (menu_thread_run) {
		usleep(49000);
		count++;

		if (count >= 20) {
			Menu_get_system_info();
			var_need_refresh = true;
			count = 0;
		}

		if (var_time_to_turn_off_lcd != 0) {
			var_afk_time += 0.05;
		}

		static bool cur_screen_on = true;
		static bool cur_screen_dimmed = false;
		// track manual off state for top and bottom separately to detect changes
		static bool cur_top_manual_off = false;
		static bool cur_bot_manual_off = false;

		bool next_screen_dimmed = cur_screen_dimmed;

		// Determine which screens should be turned off based on mode
		// Mode 0: Bottom, Mode 1: Top, Mode 2: Both
		bool allowed_top_off = (var_screen_off_mode == 1 || var_screen_off_mode == 2);
		bool allowed_bot_off = (var_screen_off_mode == 0 || var_screen_off_mode == 2);

		// Manual off logic
		bool top_off_manual = manual_screen_off && (var_screen_off_mode == 1 || var_screen_off_mode == 2);
		bool bot_off_manual = manual_screen_off && (var_screen_off_mode == 0 || var_screen_off_mode == 2);

		// System handles sleep if app is suspended
		bool next_top_manual_off = top_off_manual && !var_app_suspended;
		bool next_bot_manual_off = bot_off_manual && !var_app_suspended;

		bool idle_off_requested = false;
		if (var_time_to_turn_off_lcd != 0 && var_afk_time > var_time_to_turn_off_lcd) {
			idle_off_requested = true;
			next_screen_dimmed = false;
		} else if (var_time_to_turn_off_lcd != 0 &&
		           var_afk_time > std::max<float>(var_time_to_turn_off_lcd * 0.5, (var_time_to_turn_off_lcd - 10))) {
			idle_off_requested = false;
			next_screen_dimmed = true;
		} else {
			idle_off_requested = false;
			next_screen_dimmed = false;
		}

		// Idle off logic (respects screen off mode)
		bool next_top_idle_off = idle_off_requested && allowed_top_off;
		bool next_bot_idle_off = idle_off_requested && allowed_bot_off;

		// Final screen state: ON if neither manually off nor idle off
		bool next_top_on = !next_top_manual_off && !next_top_idle_off;
		bool next_bot_on = !next_bot_manual_off && !next_bot_idle_off;

		// We track the applied state to detect changes
		static bool cur_top_applied_on = true;
		static bool cur_bot_applied_on = true;

		if (cur_top_applied_on != next_top_on || cur_bot_applied_on != next_bot_on ||
		    cur_screen_dimmed != next_screen_dimmed || cur_top_manual_off != next_top_manual_off ||
		    cur_bot_manual_off != next_bot_manual_off) {

			// Handle screen power
			if (cur_top_applied_on == cur_bot_applied_on && cur_top_applied_on != next_top_on &&
			    cur_bot_applied_on != next_bot_on) {
				Util_cset_set_screen_state(true, true, next_top_on);
			} else {
				if (cur_top_applied_on != next_top_on) {
					Util_cset_set_screen_state(true, false, next_top_on);
				}
				if (cur_bot_applied_on != next_bot_on) {
					Util_cset_set_screen_state(false, true, next_bot_on);
				}
			}

			// Handle dimming
			if (cur_screen_dimmed != next_screen_dimmed) {
				Util_cset_set_screen_brightness(allowed_top_off, allowed_bot_off,
				                                next_screen_dimmed ? 10 : var_lcd_brightness);
			}

			cur_top_applied_on = next_top_on;
			cur_bot_applied_on = next_bot_on;
			cur_screen_dimmed = next_screen_dimmed;
			cur_top_manual_off = next_top_manual_off;
			cur_bot_manual_off = next_bot_manual_off;
		}
	}
	logger.info(DEF_MENU_WORKER_THREAD_STR, "Thread exit.");
	threadExit(0);
}
