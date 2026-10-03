#pragma once
// DIAGNOSTIC ONLY (NETWORK_FREEZE_DELIVERY.md): per-thread phase / lock-wait breadcrumbs plus a stall detector,
// used by the watchdog thread (system/freeze_watchdog.cpp) to write a bounded, redacted snapshot to the SD card when
// the UI (main) thread stops looping. This changes no behaviour and fixes nothing by itself.
//
// Rules: phase and lock names are string LITERALS (never URLs, ids or user data); every field is one word, written
// by its owner thread and read by the watchdog through relaxed atomics, so a snapshot may mix instants but never
// reads freed memory or takes a lock. Pure C++ (host-tested by tests/host/test_freeze_diag.cpp).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace freeze_diag {

enum Slot : int { MAIN = 0, DECODE, CONVERT, DOWNLOADER, ASYNC, THUMBNAIL, SLOT_NUM };
enum LockId : int { SMALL_RESOURCE = 0, DECODER_CRITICAL, LOCK_NUM };
static const int NO_OWNER = -1;
static const int UNREGISTERED = SLOT_NUM; // a thread that never called register_thread()

struct ThreadCrumb {
	std::atomic<const char *> phase{nullptr};
	std::atomic<uint32_t> phase_since{0};
	std::atomic<const char *> waiting_on{nullptr}; // lock name while blocked in TracedMutex::lock()
	std::atomic<uint32_t> wait_since{0};
	std::atomic<uint32_t> counter{0}; // libcurl poll iterations of the current perform()
};
struct Board {
	ThreadCrumb threads[SLOT_NUM];
	std::atomic<uint32_t> heartbeat{0}; // main loop iterations
	std::atomic<int> lock_owner[LOCK_NUM];
	std::atomic<uint32_t> lock_since[LOCK_NUM];
	Board();
};
Board &board();

typedef uint32_t (*ClockFn)(); // milliseconds (wraps; only differences are used)
void set_clock(ClockFn clock);
uint32_t now_ms();

// calling thread
void register_thread(Slot slot);
int current_slot(); // UNREGISTERED if the thread never registered
void phase(const char *literal);
const char *current_phase();
void wait_begin(const char *lock_literal);
void wait_end();
void counter_reset();
void counter_bump();
void lock_acquired(LockId id);
void lock_released(LockId id);
void heartbeat();

class StallDetector {
  public:
	struct Config {
		uint32_t stall_ms = 5000;   // main loop frozen this long -> first snapshot
		uint32_t again_ms = 20000;  // still frozen -> second (last) snapshot of this episode
		int max_lines = 8;          // per app run
	};
	enum class Event { NONE, STALL, STALL_AGAIN, RESUMED };
	explicit StallDetector(const Config &cfg) : cfg(cfg) {}
	// *stall_ms = how long the heartbeat has not moved (STALL*) / how long it was frozen (RESUMED)
	Event tick(uint32_t now, uint32_t heartbeat, uint32_t *stall_ms);
	int lines() const { return lines_; }

  private:
	Config cfg;
	bool started = false;
	uint32_t last_hb = 0;
	uint32_t last_change = 0;
	bool in_stall = false;
	bool reported = false;
	bool reported_again = false;
	int lines_ = 0;
};
const char *event_name(StallDetector::Event e);

// One line (no trailing newline), always NUL-terminated, never longer than cap - 1. Only literals + integers.
size_t format_snapshot(char *buf, size_t cap, StallDetector::Event event, uint32_t now, uint32_t stall_ms,
                       bool app_suspended, const Board &b);

} // namespace freeze_diag
