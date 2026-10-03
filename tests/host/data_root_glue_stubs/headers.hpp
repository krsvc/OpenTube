#pragma once
// host stand-in for source/headers.hpp used by tests/host/test_data_root_glue.cpp: recording mocks of the services,
// drawing, threads, scenes and APT / HID that the REAL main() and Menu_init() (extracted verbatim) call. The data root
// macros point into the test's fixture SD card (g_sd); the production values are checked by test_data_root_defs.cpp.
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;
typedef int32_t Result;
typedef u32 Handle;
#define R_SUCCEEDED(res) ((res) >= 0)
#define R_FAILED(res) ((res) < 0)

void ev(const std::string &e); // the test's event log

struct Result_with_string {
	std::string string = "[Success] ";
	std::string error_description = "";
	Result code = 0;
};

struct HostLogger {
	void init() { ev("logger.init"); }
	void info(const std::string &, const std::string &) {}
	void info(const std::string &, const std::string &, long) {}
	void caution(const std::string &, const std::string &) {}
	void error(const std::string &m, const std::string &s) { ev("log.error:" + m + ":" + s); }
};
extern HostLogger logger;

// threads: never run, only recorded
struct MockThread;
typedef MockThread *Thread;
typedef void (*ThreadFunc)(void *);
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
void threadExit(int rc);
#define CUR_THREAD_HANDLE 0xFFFF8000
#define U64_MAX UINT64_MAX
Result svcSetThreadPriority(Handle thread, s32 prio);
void osSetSpeedupEnable(bool enable);
u64 osGetTime();
void *memalign(size_t alignment, size_t size);

// services
Result fsInit();
Result acInit();
Result aptInit();
Result mcuHwcInit();
Result ptmuInit();
Result socInit(u32 *buf, u32 size);
Result sslcInit(Handle h);
Result httpcInit(u32 size);
Result romfsInit();
Result cfguInit();
Result amInit();
Result ndspInit();
Result APT_SetAppCpuTimeLimit(u32 percent);
void fsExit();
void acExit();
void aptExit();
void mcuHwcExit();
void ptmuExit();
void httpcExit();
Result romfsExit();
void cfguExit();
void amExit();
void ndspExit();
void sslcExit();
Result socExit();
// APT hooks as in libctru: the test keeps the registered list (aptHook / aptUnhook) and its aptMainLoop() dispatches a
// HOME suspend / restore to whatever is registered, i.e. the REAL application callback of system/apt_handler.cpp
typedef enum {
	APTHOOK_ONSUSPEND = 0,
	APTHOOK_ONRESTORE,
	APTHOOK_ONSLEEP,
	APTHOOK_ONWAKEUP,
	APTHOOK_ONEXIT,
	APTHOOK_COUNT,
} APT_HookType;
typedef void (*aptHookFn)(APT_HookType hook, void *param);
typedef struct tag_aptHookCookie {
	struct tag_aptHookCookie *next;
	aptHookFn callback;
	void *param;
} aptHookCookie;
void aptHook(aptHookCookie *cookie, aptHookFn callback, void *param);
void aptUnhook(aptHookCookie *cookie);
Result APT_GetAppCpuTimeLimit(u32 *percent);
class Mutex { // the REAL system/cpu_limit.cpp locks one
	std::mutex m;

  public:
	void lock() { m.lock(); }
	void unlock() { m.unlock(); }
};
extern bool var_app_suspended;
extern float var_afk_time;
#include "system/cpu_limit.hpp" // REAL: add_cpu_limit / remove_cpu_limit / CPU_LIMIT (uses var_is_new3ds)
// REAL Menu_exit() (extracted) needs these; recorded by the test
Handle threadGetHandle(Thread thread);
Result svcWaitSynchronization(Handle handle, s64 ns);
bool Downloads_stop();
bool Updater_stop();
void VideoPlayer_exit();
void Channel_exit();
void Search_exit();
void Sem_exit();
void About_exit();
void History_exit();
void Home_exit();
void Downloads_exit();
void Liked_exit();
void Util_expl_exit();
void Extfont_exit();
void thumbnail_downloader_thread_exit_request();
void async_task_thread_exit_request();
void misc_tasks_thread_exit_request();
void freeze_watchdog_stop();
struct NetworkSessionList {
	static void exit_request();
	static void at_exit();
};
void lock_network_state();
void unlock_network_state();
void aptSetSleepAllowed(bool allowed);
void set_apt_callback();
void remove_apt_callback();
bool aptMainLoop();
Result APT_CheckNew3DS(bool *out);
#define CFG_MODEL_2DS 3
Result CFGU_GetSystemModel(u8 *model);
void hidScanInput();
u32 hidKeysDown();
#define KEY_A (1u << 0)
#define KEY_B (1u << 1)
#define KEY_START (1u << 3)

extern bool var_is_new3ds;
extern u8 var_model;
extern bool var_core2_available, var_core3_available;
extern u8 var_wifi_state;
extern u8 mock_wifi_byte; // stands in for the shared-memory Wi-Fi byte read in Menu_init
void Util_cset_set_wifi_state(bool on);

// drawing
#define DEF_DRAW_BLACK 0xFF000000
#define POCKET_DARK_BACKGROUND 0xFF251E18
#define POCKET_DARK_TEXT 0xFFF4F1EE
Result_with_string Draw_init(bool wide);
void Draw_frame_ready();
void Draw_screen_ready(int screen, u32 color);
void Draw_apply_draw();
void Draw_xy_centered(std::string text, float x0, float x1, float y0, float y1, float sx, float sy, int color);
void Draw_exit();
void Util_expl_init();
void Extfont_init();
#define FONT_BLOCK_NUM 2
#define SYSTEM_FONT_NUM 2
void Extfont_request_extfont_status(int i, bool load);
void Extfont_request_sysfont_status(int i, bool load);

// scenes, workers, consumers
void Sem_init();
void Sem_suspend();
void VideoPlayer_init();
void VideoPlayer_suspend();
void Channel_init();
void Channel_suspend();
void About_init();
void About_suspend();
void History_init();
void History_suspend();
void Search_init();
void Search_suspend();
void Downloads_start();
void Downloads_init();
void Downloads_suspend();
void Liked_init();
void Liked_suspend();
void Home_init();
void thumbnail_downloader_thread_func(void *);
void async_task_thread_func(void *);
void misc_tasks_thread_func(void *);
void Updater_start();
void Menu_get_system_info();
void freeze_watchdog_start();
bool Menu_main();
void Menu_exit_request();
bool Menu_exit_ready();
void Menu_stopping();
void Menu_exit();

// definitions used by the extracted code (data root macros point into the fixture SD card)
extern std::string g_sd;
#define DEF_MAIN_DIR (g_sd + std::string("/3ds/opentube/"))
#define DEF_PREDECESSOR_MAIN_DIR (g_sd + std::string("/3ds/FourthTubeTest/"))
#define DEF_DATA_ROOT_STAGE_DIR (g_sd + std::string("/3ds/opentube.partial/"))
#define DEF_CURRENT_APP_VER (std::string) "Beta 34.1 Test"
#define DEF_MENU_INIT_STR (std::string) "Menu/Init"
#define DEF_MENU_EXIT_STR (std::string) "Menu/Exit"
#define DEF_STACKSIZE (64 * 1024)
#define DEF_THREAD_PRIORITY_NORMAL 0x24
#define DEF_THREAD_PRIORITY_HIGH 0x23
#define DEF_THREAD_PRIORITY_REALTIME 0x18
