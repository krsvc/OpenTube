#pragma once
// DIAGNOSTIC ONLY (FPS_MEASUREMENT_PLAN.md): frame-pacing measurement of the video pipeline
// (decode thread -> convert thread -> texture publication -> main-thread draw / submit). Measures, changes nothing.
//
// Concurrency rules: every Counter / Hist bucket has exactly ONE writer thread (its probe's owner) and is a cumulative
// std::atomic<uint32_t> updated with a relaxed load + store (no RMW, no lock). Readers only take wrap-safe differences
// of cumulative values, so nothing is ever reset across threads. The publication sequence number is the only value with
// release / acquire ordering. No seqlock, no 64-bit shared state. Probe-local "previous" values are plain members
// touched only by the owner thread. Pure C++ (host-tested by tests/host/test_fps_diag.cpp).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fps_diag {

static const uint32_t VBLANK_US = 16713;         // 3DS LCD refresh ~59.83 Hz
static const uint32_t WINDOW_US = 1000000;       // analyzer window
static const uint32_t LONG_WINDOW_US = 3000000;  // longer = main loop blocked / suspended / clock discontinuity
static const int INTERVAL_MAX_WINDOWS = 30;      // one log line at least every 30 windows of playback
static const int INTERVAL_MIN_WINDOWS = 5;       // a state-key change splits the line only after this many windows
static const int HIST_N = 8;

struct Counter {
	std::atomic<uint32_t> v{0};
	void add(uint32_t d) { v.store(v.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }
	void set(uint32_t x) { v.store(x, std::memory_order_relaxed); }
	uint32_t get() const { return v.load(std::memory_order_relaxed); }
};
struct Hist {
	Counter b[HIST_N];
	void add(int bucket) { b[bucket].add(1); }
};

// interval in VBlank units, rounded: 0..6, 7 = 7 or more
int vblank_bucket(uint32_t interval_us);
// <1, <2, <4, <8, <16, <33, <66, >=66 ms
int duration_bucket(uint32_t us);
// video pts minus audio clock: <-100, <-50, <-17, <-3, -3..3, <=17, <=50, >50 ms
int skew_bucket(int32_t skew_us);
// smallest unique-submit interval that means at least one source frame was not shown (0 = rate unknown)
uint32_t hitch_threshold_us(uint32_t nominal_mfps, uint32_t tempo_milli);
// audio queued ahead of the DSP: <20, <50, <100, <200, <300, <500, <1000, >=1000 ms
int lead_bucket(uint32_t us);
// S16 values at -32768 / +32767 (what the saturating float -> S16 conversion produces for over-full-scale input)
uint32_t count_full_scale(const int16_t *pcm, uint32_t n_values);

enum DecoderKind : uint32_t { DEC_UNKNOWN = 0, DEC_HW, DEC_MT_FRAME, DEC_MT_SLICE, DEC_SINGLE };
enum SessionFlag : uint32_t { S_AUDIO_ONLY = 1, S_LIVE = 2 };

// Written by the decode thread only.
struct Session {
	Counter generation;  // play start, seek begin / end, play loop exit
	Counter play_starts; // play start only (UI steady totals restart here)
	Counter nominal_mfps; // stream frame rate x 1000, 0 = unknown
	Counter width, height, quality, decoder_kind, flags;
	std::atomic<const char *> client{nullptr}; // string literal (player API client actually used)
	void on_play_start() {
		play_starts.add(1);
		generation.add(1);
	}
	void bump() { generation.add(1); }
	void set_meta(uint32_t mfps, uint32_t w, uint32_t h, uint32_t kind, uint32_t q, uint32_t session_flags,
	              const char *client_literal);
};

// Written by the decode thread only.
struct DecoderProbe {
	Counter decoded;      // decode_video() returned 0 = one frame queued for the converter
	Counter decode_us;    // duration of that decode_video() call (includes packet refill, i.e. possibly network)
	Counter backpressure; // NEED_MORE_OUTPUT retries: converter queue full, decoder is ahead
	Hist decode_hist;
	void on_backpressure_wait() { backpressure.add(1); }
	void on_video_decode(bool frame_queued, uint32_t us);
};

// Written by the convert thread only (except `seq`, which the main thread also reads with acquire).
class ConverterProbe {
  public:
	Counter got;           // frames taken from the decoder queue
	Counter starve_events; // frames that needed >= 1 NEED_MORE_INPUT retry while not paused
	Counter starve_us;
	Counter convert_us;    // Y2R / software conversion (0 on the MVD path)
	Counter sync_us;       // time in the audio-sync wait loop (intended pacing)
	Counter no_audio;      // frames drawn without an audio clock
	Counter copy_us;       // texture upload + publication
	Counter skipped_draw;  // frames not published because video_skip_drawing (keyboard open)
	Counter media_us;      // sum of positive pts deltas <= 1 s between publications
	Counter pts_disc;      // other pts deltas (seek, loop, stream discontinuity)
	Counter tempo_milli;   // playback speed x 1000 at the last publication
	Hist convert_hist, sync_hist, copy_hist, skew_hist, pub_interval_hist;
	std::atomic<uint32_t> seq{0}; // publications so far; release store after texture_index_head toggled

	void on_frame_got(uint32_t wait_us, bool starved);
	void on_converted(uint32_t us);
	void on_synced(uint32_t wait_us, bool have_audio_clock, int32_t skew_us);
	void on_published(uint32_t now_us, uint32_t copy_us, int64_t pts_us, uint32_t tempo_milli, uint32_t generation);
	void on_skipped_draw() { skipped_draw.add(1); }
	uint32_t published() const { return seq.load(std::memory_order_relaxed); }

  private: // convert thread only
	bool have_last = false;
	uint32_t seen_gen = 0;
	uint32_t last_pub_us = 0;
	int64_t last_pts_us = 0;
};

// Written by the main thread only (Draw_frame_ready / video_draw_video_frame / Draw_apply_draw).
class RenderProbe {
  public:
	Counter submits;       // every C3D_FrameEnd
	Counter video_submits; // submits whose frame drew the video texture
	Counter unique;        // ... with a newer publication than the previous video submit
	Counter repeats;       // ... with the same publication again
	Counter missed;        // publications overwritten before any submit drew them
	Counter hitches;       // unique intervals >= hitch_threshold_us()
	Counter submit_us;     // C3D_FrameEnd CPU time (video submits)
	Counter begin_us;      // C3D_FrameBegin(SYNCDRAW) time: VBlank sync + previous GPU queue (video submits)
	Hist unique_interval_hist, submit_hist, begin_hist;

	void on_frame_begin(uint32_t wait_us);
	void on_video_drawn(uint32_t seq);
	void on_submitted(uint32_t now_us, uint32_t submit_us, uint32_t generation, uint32_t nominal_mfps,
	                  uint32_t tempo_milli);

  private: // main thread only
	bool begin_pending = false;
	uint32_t pending_begin_us = 0;
	bool video_pending = false;
	uint32_t pending_seq = 0;
	bool have_last = false;
	uint32_t seen_gen = 0;
	uint32_t last_seq = 0;
	uint32_t last_unique_us = 0;
};

// Written by the decode thread only (audio branch of decode_thread: decode_audio() -> Util_speaker_add_buffer()).
// ECO_AUDIO_DIAGNOSIS.md: audio health next to the video pipeline. Measures, changes nothing.
class AudioProbe {
  public:
	Counter src_rate, channels, speaker_rate; // decoder sample rate / channels, rate given to ndsp (session meta)
	Counter decoded;      // decode_audio() returned PCM
	Counter errors;       // decode_audio() failed (NEED_MORE_INPUT is not an error)
	Counter decode_us;    // successful decode_audio() calls (filter, swr and packet refill included)
	Counter queued;       // buffers handed to ndsp
	Counter samples;      // samples per channel handed to ndsp
	Counter full_retries; // "Queues are full" retries: the producer is far ahead
	Counter add_errors;   // other Util_speaker_add_buffer failures (out of linear memory), retried
	Counter dry;          // buffer added while the channel had stopped = the queue ran dry (first add of a generation
	                      // and a paused channel excluded)
	Counter full_scale;   // S16 values at full scale in the added PCM
	Hist decode_hist, lead_hist;

	void set_meta(uint32_t src, uint32_t ch, uint32_t spk);
	void on_decoded(bool ok, uint32_t us);
	// once per buffer, before the first add attempt: queue state and the converted PCM
	void on_add_begin(uint32_t queued_ahead_samples, bool running, uint32_t generation, const int16_t *pcm,
	                  uint32_t n_values);
	void on_add_retry(bool queue_full);
	void on_added(uint32_t nsamples);

  private: // decode thread only
	bool have_gen = false;
	uint32_t seen_gen = 0;
};

// Written by the main thread only: copies of libctru's ndspGetFrameCount() / ndspGetDroppedFrames() (cumulative
// counters of the ndsp thread: DSP frames serviced, frames the DSP reported dropped), taken before each analyzer tick.
struct DspProbe {
	Counter frames, dropped;
};

// Written by the log writer thread only (freeze watchdog).
struct LogStats {
	Counter lines, bytes, dropped, failures;
	std::atomic<uint32_t> state{0}; // LogState
};
enum LogState : uint32_t { LOG_IDLE = 0, LOG_OK, LOG_FULL, LOG_FAILED };

struct Probes {
	Session session;
	DecoderProbe dec;
	ConverterProbe conv;
	RenderProbe render;
	LogStats log;
	AudioProbe audio;
	DspProbe dsp;
};

// ---------------------------------------------------------------------------------------------------------------
// Snapshot / delta
// ---------------------------------------------------------------------------------------------------------------
enum Field : int {
	DEC_FRAMES = 0, DEC_US, DEC_BP,
	CONV_GOT, CONV_STARVE_EV, CONV_STARVE_US, CONV_US, CONV_SYNC_US, CONV_NO_AUDIO, CONV_COPY_US, CONV_SKIPPED,
	CONV_MEDIA_US, CONV_PTS_DISC, CONV_PUB,
	R_SUBMITS, R_VIDEO_SUBMITS, R_UNIQUE, R_REPEATS, R_MISSED, R_HITCHES, R_SUBMIT_US, R_BEGIN_US,
	H_DEC, H_CONV = H_DEC + HIST_N, H_SYNC = H_CONV + HIST_N, H_COPY = H_SYNC + HIST_N, H_SKEW = H_COPY + HIST_N,
	H_PUB_IV = H_SKEW + HIST_N, H_UNIQ_IV = H_PUB_IV + HIST_N, H_SUBMIT = H_UNIQ_IV + HIST_N,
	H_BEGIN = H_SUBMIT + HIST_N,
	A_DEC = H_BEGIN + HIST_N, A_ERR, A_DEC_US, A_QUEUED, A_SAMPLES, A_FULL, A_ADD_ERR, A_DRY, A_FS, DSP_FRAMES,
	DSP_DROPPED, H_ADEC, H_LEAD = H_ADEC + HIST_N, FIELD_N = H_LEAD + HIST_N
};
struct Counts {
	uint32_t v[FIELD_N] = {};
};
void take_snapshot(const Probes &p, Counts *out);
void delta(const Counts &before, const Counts &after, Counts *out); // wrap-safe

// Main-thread accumulator (64-bit, never shared).
struct Accum {
	uint64_t v[FIELD_N] = {};
	void clear();
	void add(const Counts &d);
};

// ---------------------------------------------------------------------------------------------------------------
// Main-thread window analyzer
// ---------------------------------------------------------------------------------------------------------------
enum StateFlag : uint32_t {
	F_NOT_PLAYING = 1u << 0, // no play request / decoder not ready
	F_PAUSED = 1u << 1,      // pause button or seek-bar grab
	F_SEEKING = 1u << 2,     // seek or video change requested
	F_WAITING = 1u << 3,     // player / decoder waiting status (reading stream, seeking, buffering, recovering)
	F_EOF = 1u << 4,
	F_SKIP_DRAW = 1u << 5,   // keyboard open: converter does not publish
	F_SUSPENDED = 1u << 6,   // HOME / background / decoding suspended
	F_AUDIO_ONLY = 1u << 7,
	DISQUALIFY_MASK = 0xffu,
	M_DEBUG_VIEW = 1u << 8,  // lower-screen debug view drawn in this window
	M_TOP_DEBUG = 1u << 9,   // var_debug_mode overlay
	M_ECO = 1u << 10,
	M_FULLSCREEN = 1u << 11,
	M_NEW3DS = 1u << 12,
	KEY_META_MASK = M_DEBUG_VIEW | M_TOP_DEBUG | M_ECO | M_FULLSCREEN,
	// analyzer-only window reasons
	R_GEN = 1u << 16,    // generation changed (play start / seek / stop) during the window
	R_LONG = 1u << 17,   // window longer than LONG_WINDOW_US
	R_NO_SRC = 1u << 18, // nominal source rate unknown
	R_SETTLE = 1u << 19, // previous window was not clean
};
const char *reason_name(int bit);

struct StateSample {
	uint32_t now_us = 0;
	uint32_t flags = 0;
	const char *scene = "?"; // string literal
};

class Analyzer {
  public:
	typedef bool (*Sink)(const char *line, size_t len, void *ctx); // false = not accepted (ring full)
	static const size_t LINE_CAP = 1024;
	static const int SUMMARY_LINES = 4;
	static const size_t SUMMARY_CAP = 72;

	Analyzer(const Probes &probes, Sink sink, void *sink_ctx) : p(probes), sink(sink), sink_ctx(sink_ctx) {}
	bool window_due(uint32_t now_us) const { return started && (uint32_t)(now_us - win_start_us) >= WINDOW_US; }
	void tick(const StateSample &s);
	void flush(); // emit the open interval (exit)

	// last closed window (for tests / UI)
	const Counts &last_delta() const { return last_d; }
	uint32_t last_reasons() const { return last_r; }
	uint32_t last_wall_us() const { return last_wall; }
	bool last_steady() const { return windows_closed && last_r == 0; }
	uint64_t windows_closed_count() const { return windows_closed; }
	// steady totals of the current playback session (reset at a play start)
	const Accum &session_steady() const { return sess_steady; }
	uint32_t session_steady_windows() const { return sess_steady_windows; }
	uint64_t session_steady_wall_us() const { return sess_steady_wall_us; }
	uint32_t session_stall_windows() const { return sess_stall_windows; }
	uint32_t lines_emitted() const { return emitted; }
	uint32_t lines_refused() const { return refused; }
	const char *summary(int i) const { return (i >= 0 && i < SUMMARY_LINES) ? ui[i] : ""; }
	// format one interval line (exposed for tests)
	size_t format_interval(char *buf, size_t cap) const;

  private:
	struct Key {
		uint32_t gen = 0;
		uint32_t meta = 0;
		const char *scene = nullptr;
		bool operator!=(const Key &o) const { return gen != o.gen || meta != o.meta || scene != o.scene; }
	};
	struct Interval {
		Key key;
		int windows = 0;
		int playing_windows = 0;
		int steady_windows = 0;
		int stall_windows = 0;
		uint64_t steady_wall_us = 0;
		uint64_t nonsteady_wall_us = 0;
		uint32_t reason_windows[20] = {};
		bool mixed = false;
		Accum steady;
		uint32_t flags_or = 0;
		const char *scene = "?";
		// session metadata captured at windows without a generation change (never from a later session)
		bool have_meta = false;
		uint32_t mfps = 0, width = 0, height = 0, quality = 0, decoder_kind = 0, session_flags = 0, tempo_milli = 0;
		uint32_t a_src_rate = 0, a_channels = 0, a_speaker_rate = 0;
		const char *client = nullptr;
		// audio problems in windows that are not steady (pause / seek / wait / eof ...)
		uint64_t ns_dry = 0, ns_dsp_dropped = 0;
	};

	void close_window(uint32_t now_us);
	void add_to_interval(const Counts &d, uint32_t reasons, uint32_t wall);
	void capture_meta();
	void emit_interval();
	void update_summary();

	const Probes &p;
	Sink sink;
	void *sink_ctx;
	bool started = false;
	uint32_t win_start_us = 0;
	uint32_t win_start_gen = 0;
	uint32_t win_flags = 0;
	const char *win_scene = "?";
	bool prev_clean = false;
	Counts snap;
	Counts last_d = {};
	uint32_t last_r = 0;
	uint32_t last_wall = 0;
	uint64_t windows_closed = 0;
	uint32_t sess_play_starts = 0;
	Accum sess_steady = {};
	uint32_t sess_steady_windows = 0;
	uint64_t sess_steady_wall_us = 0;
	uint32_t sess_stall_windows = 0;
	Interval iv;
	uint32_t line_no = 0;
	uint32_t emitted = 0;
	uint32_t refused = 0;
	char ui[SUMMARY_LINES][SUMMARY_CAP] = {};
};

// ---------------------------------------------------------------------------------------------------------------
// Log transport and bounds
// ---------------------------------------------------------------------------------------------------------------
// Single-producer (main) / single-consumer (log writer) ring of fixed line slots.
template <int N, size_t L> class LineRing {
  public:
	bool push(const char *line, size_t len) { // producer
		uint32_t h = head.load(std::memory_order_relaxed);
		if (h - tail.load(std::memory_order_acquire) >= (uint32_t)N) {
			dropped.add(1);
			return false;
		}
		if (len > L) {
			len = L;
		}
		for (size_t i = 0; i < len; i++) {
			slot[h % N][i] = line[i];
		}
		slot_len[h % N] = len;
		head.store(h + 1, std::memory_order_release);
		return true;
	}
	bool pop(char *out, size_t cap, size_t *len) { // consumer
		uint32_t t = tail.load(std::memory_order_relaxed);
		if (head.load(std::memory_order_acquire) == t) {
			return false;
		}
		size_t n = slot_len[t % N] < cap ? slot_len[t % N] : cap;
		for (size_t i = 0; i < n; i++) {
			out[i] = slot[t % N][i];
		}
		*len = n;
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
	Counter dropped; // producer only

  private:
	char slot[N][L];
	size_t slot_len[N] = {};
	std::atomic<uint32_t> head{0}, tail{0};
};

struct LogPolicy {
	uint32_t file_cap = 256 * 1024;   // one file never grows past this (checked at the start of a run)
	uint32_t run_budget = 96 * 1024;  // bytes written per app run, header and marker included
	uint32_t marker_reserve = 160;
	int max_failures = 3;             // consecutive failed writes, then stop touching the SD card
};
// Log writer thread only.
class LogBudget {
  public:
	enum class Verdict { WRITE, MARKER_THEN_STOP, DROP };
	explicit LogBudget(const LogPolicy &policy) : pol(policy) {}
	// first write of this run: rotate fps_diag.log -> fps_diag.prev.log when this run could overflow the file cap
	bool rotate_needed(uint64_t existing_size) const { return existing_size + pol.run_budget > pol.file_cap; }
	Verdict admit(size_t len);
	void on_write_result(bool ok);
	bool stopped() const { return exhausted || failures >= pol.max_failures; }
	bool exhausted_budget() const { return exhausted; }
	uint32_t used() const { return bytes; }
	uint32_t dropped() const { return drops; }
	static size_t format_marker(char *buf, size_t cap, uint32_t lines, uint32_t bytes);

  private:
	LogPolicy pol;
	uint32_t bytes = 0;
	uint32_t lines = 0;
	uint32_t drops = 0;
	int failures = 0;
	bool exhausted = false;
};

} // namespace fps_diag
