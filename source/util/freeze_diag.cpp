#include "freeze_diag.hpp"
#include <cstdio>

namespace freeze_diag {

Board::Board() {
	for (int i = 0; i < LOCK_NUM; i++) {
		lock_owner[i].store(NO_OWNER, std::memory_order_relaxed);
		lock_since[i].store(0, std::memory_order_relaxed);
	}
}
Board &board() {
	static Board b;
	return b;
}

static uint32_t zero_clock() { return 0; }
static std::atomic<ClockFn> clock_fn{zero_clock};
void set_clock(ClockFn clock) { clock_fn.store(clock ? clock : zero_clock, std::memory_order_relaxed); }
uint32_t now_ms() { return clock_fn.load(std::memory_order_relaxed)(); }

static thread_local int tls_slot = UNREGISTERED;
void register_thread(Slot slot) { tls_slot = (slot >= 0 && slot < SLOT_NUM) ? slot : UNREGISTERED; }
int current_slot() { return tls_slot; }

static ThreadCrumb *mine() { return tls_slot < SLOT_NUM ? &board().threads[tls_slot] : nullptr; }

void phase(const char *literal) {
	if (ThreadCrumb *c = mine()) {
		c->phase_since.store(now_ms(), std::memory_order_relaxed);
		c->phase.store(literal, std::memory_order_relaxed);
	}
}
const char *current_phase() {
	ThreadCrumb *c = mine();
	return c ? c->phase.load(std::memory_order_relaxed) : nullptr;
}
void wait_begin(const char *lock_literal) {
	if (ThreadCrumb *c = mine()) {
		c->wait_since.store(now_ms(), std::memory_order_relaxed);
		c->waiting_on.store(lock_literal, std::memory_order_relaxed);
	}
}
void wait_end() {
	if (ThreadCrumb *c = mine()) {
		c->waiting_on.store(nullptr, std::memory_order_relaxed);
	}
}
void counter_reset() {
	if (ThreadCrumb *c = mine()) {
		c->counter.store(0, std::memory_order_relaxed);
	}
}
void counter_bump() {
	if (ThreadCrumb *c = mine()) {
		c->counter.store(c->counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
	}
}
void lock_acquired(LockId id) {
	board().lock_since[id].store(now_ms(), std::memory_order_relaxed);
	board().lock_owner[id].store(tls_slot, std::memory_order_relaxed);
}
void lock_released(LockId id) { board().lock_owner[id].store(NO_OWNER, std::memory_order_relaxed); }
void heartbeat() {
	board().heartbeat.store(board().heartbeat.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

StallDetector::Event StallDetector::tick(uint32_t now, uint32_t hb, uint32_t *stall_ms) {
	if (!started || hb != last_hb) {
		bool was_reported = started && in_stall && reported;
		uint32_t frozen = now - last_change;
		started = true;
		last_hb = hb;
		last_change = now;
		in_stall = false;
		if (was_reported && lines_ < cfg.max_lines) {
			lines_++;
			*stall_ms = frozen;
			return Event::RESUMED;
		}
		return Event::NONE;
	}
	uint32_t elapsed = now - last_change;
	if (!in_stall && elapsed >= cfg.stall_ms) {
		in_stall = true;
		reported = reported_again = false;
		if (lines_ < cfg.max_lines) {
			lines_++;
			reported = true;
			*stall_ms = elapsed;
			return Event::STALL;
		}
	} else if (in_stall && reported && !reported_again && elapsed >= cfg.again_ms) {
		reported_again = true;
		if (lines_ < cfg.max_lines) {
			lines_++;
			*stall_ms = elapsed;
			return Event::STALL_AGAIN;
		}
	}
	return Event::NONE;
}
const char *event_name(StallDetector::Event e) {
	switch (e) {
	case StallDetector::Event::STALL:
		return "STALL";
	case StallDetector::Event::STALL_AGAIN:
		return "STALL_AGAIN";
	case StallDetector::Event::RESUMED:
		return "RESUMED";
	default:
		return "NONE";
	}
}

static const char *slot_name(int slot) {
	static const char *const names[SLOT_NUM] = {"main", "dec", "conv", "dl", "async", "thumb"};
	if (slot == NO_OWNER) {
		return "-";
	}
	return slot >= 0 && slot < SLOT_NUM ? names[slot] : "other";
}
// literals only, but never trust a pointer blindly for length / content: bounded, printable, no spaces
static void put_literal(char *&p, char *end, const char *s) {
	if (!s) {
		s = "?";
	}
	for (int i = 0; s[i] && i < 24 && p < end; i++) {
		char c = s[i];
		*p++ = (c > ' ' && c < 127) ? c : '_';
	}
}
static void put_fmt(char *&p, char *end, const char *fmt, unsigned a, unsigned b = 0) {
	if (p >= end) {
		return;
	}
	int n = snprintf(p, end - p + 1, fmt, a, b);
	if (n > 0) {
		p += (size_t)n > (size_t)(end - p) ? (size_t)(end - p) : (size_t)n;
	}
}

size_t format_snapshot(char *buf, size_t cap, StallDetector::Event event, uint32_t now, uint32_t stall_ms,
                       bool app_suspended, const Board &b) {
	if (!buf || cap == 0) {
		return 0;
	}
	char *p = buf;
	char *end = buf + cap - 1; // room for NUL
	put_fmt(p, end, "t=%u ev=", now);
	put_literal(p, end, event_name(event));
	put_fmt(p, end, " frozen=%ums hb=%u", stall_ms, b.heartbeat.load(std::memory_order_relaxed));
	put_fmt(p, end, " susp=%u", app_suspended ? 1u : 0u);
	static const char *const lock_names[LOCK_NUM] = {"small", "crit"};
	for (int i = 0; i < LOCK_NUM; i++) {
		put_fmt(p, end, " ", 0);
		put_literal(p, end, lock_names[i]);
		put_fmt(p, end, "=", 0);
		int owner = b.lock_owner[i].load(std::memory_order_relaxed);
		put_literal(p, end, slot_name(owner));
		if (owner != NO_OWNER) {
			put_fmt(p, end, "@%ums", now - b.lock_since[i].load(std::memory_order_relaxed));
		}
	}
	for (int s = 0; s < SLOT_NUM; s++) {
		const ThreadCrumb &c = b.threads[s];
		const char *ph = c.phase.load(std::memory_order_relaxed);
		if (!ph) {
			continue; // thread never reported (e.g. not started yet)
		}
		put_fmt(p, end, " ", 0);
		put_literal(p, end, slot_name(s));
		put_fmt(p, end, ":", 0);
		put_literal(p, end, ph);
		put_fmt(p, end, "@%ums", now - c.phase_since.load(std::memory_order_relaxed));
		const char *w = c.waiting_on.load(std::memory_order_relaxed);
		if (w) {
			put_fmt(p, end, "[w:", 0);
			put_literal(p, end, w);
			put_fmt(p, end, "@%ums]", now - c.wait_since.load(std::memory_order_relaxed));
		}
		uint32_t n = c.counter.load(std::memory_order_relaxed);
		if (n) {
			put_fmt(p, end, "#%u", n);
		}
	}
	*p = '\0';
	return p - buf;
}

} // namespace freeze_diag
