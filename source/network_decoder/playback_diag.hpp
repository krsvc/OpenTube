#pragma once
// Playback diagnostics + bounded recovery policy for FourthTube.
//
// This header is deliberately free of 3DS / FFmpeg dependencies so that the
// decision logic used by the decode thread can be compiled and tested on a host
// machine with a plain C++14 compiler. The production code (network_decoder.cpp,
// video_player.cpp, network_downloader.cpp) feeds real state into these
// functions; the host tests feed fakes into the very same functions.
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>

namespace playback_diag {

// FFmpeg's AVERROR_EOF value (FFERRTAG('E','O','F',' ')). Mirrored here so the
// host build does not need libavutil. network_decoder.cpp static_asserts equality.
static const int AVERROR_EOF_VALUE = -541478725;

enum class Cause {
	NONE = 0,
	HTTP_STATUS,        // downloader got a non-2xx status
	TRANSPORT,          // libcurl-level failure (timeout, reset, TLS ...)
	SIZE_MISMATCH,      // block size returned != expected
	LENGTH_UNKNOWN,     // Content-Range could not be parsed
	READ_BEYOND_END,    // downloader asked to read past the end of the stream
	SEEK_REJECTED,      // FFmpeg asked to seek beyond stream.len
	STREAM_DEAD,        // stream.error / quit without a more specific record
	DEMUX_ERROR,        // av_read_frame returned an error that is not EOF
	REINIT_FAILED,      // periodic avformat reinit failed
	EOF_SHORT,          // demuxer hit EOF before all bytes were consumed AND there is failure evidence
	LENGTH_MISMATCH,    // server Content-Range total disagrees with the clen the stream was created with
	INIT_FAILED,        // network_decoder.init() failed (startup)
	DECODE_ERROR,       // avcodec returned an error (informational)
	LIVE_UNSUPPORTED,   // error on a livestream: recovery not attempted
};
const char *cause_name(Cause c);

enum class StreamId { UNKNOWN = 0, VIDEO, AUDIO, BOTH };
const char *stream_name(StreamId s);

// One diagnostic record. Never contains URLs, query strings, cookies or tokens:
// `detail` is passed through redact() when the record is stored.
struct FailureRecord {
	Cause cause = Cause::NONE;
	StreamId stream = StreamId::UNKNOWN;
	int http_status = 0;      // 0 = n/a
	int ffmpeg_code = 0;      // last av_read_frame / avcodec return value, 0 = n/a
	uint64_t read_head = 0;   // byte position the decoder was reading
	uint64_t len = 0;         // stream length as known by the downloader (clen or Content-Range)
	uint64_t req_start = 0;   // byte range of the failing HTTP request (downloader)
	uint64_t req_end = 0;
	uint64_t expected_len = 0;
	uint64_t actual_len = 0;
	uint64_t seek_pos = 0;    // rejected seek target
	bool av_separate = false;
	double playback_pos = -1; // seconds, -1 = unknown
	int quality_p = 0;        // 0 = audio only
	int attempt = 0;          // recovery attempt number this record belongs to (0 = none yet)
	std::string video_id;     // YouTube video id (not a URL)
	std::string detail;       // short free text, redacted on store
};

// Strict redaction for anything persisted to the SD card: if the text contains any credential-like
// keyword (authorization, bearer, cookie, token, signature, password, ...) the WHOLE text becomes
// "[redacted]"; otherwise every URL / userinfo / e-mail-like token is replaced entirely ("[url]") and
// query-string fragments are dropped. Plain error text ("curl error 28 timeout") is preserved.
std::string redact(const std::string &in);

// ---------------------------------------------------------------------------
// Decision seam: what should the decode thread do next?
// ---------------------------------------------------------------------------
struct StreamState {
	bool present = false;        // NetworkStream pointer is non-null
	bool ready = false;
	bool error = false;
	bool quit = false;
	uint64_t read_head = 0;
	uint64_t len = 0;
	bool has_first_failure = false;
	FailureRecord first_failure; // as captured by the downloader / io callbacks
	bool seek_rejected = false;
	uint64_t seek_rejected_pos = 0;
	uint64_t length_mismatch_total = 0; // Content-Range total when != len (0 = none seen)
};

struct DecisionInput {
	bool av_separate = false;
	bool audio_only = false;
	bool interrupt = false;         // decoder->interrupt (intentional cancel)
	bool packets_video = false;     // packet_buffer[VIDEO] non-empty
	bool packets_audio = false;     // packet_buffer[AUDIO] non-empty
	bool decoded_video_pending = false;
	int read_result[2] = {0, 0};    // last av_read_frame result per type ([0] for BOTH)
	bool reinit_failed[2] = {false, false};
	StreamState stream[2];          // [0] video (or both), [1] audio
	double video_dts = 0;
	double audio_dts = 0;
};

enum class Decision { VIDEO, AUDIO, END_OF_STREAM, ERROR, INTERRUPTED, NONE };
const char *decision_name(Decision d);

// Pure function used by NetworkDecoder::next_decode_type(). When it returns
// ERROR, *out is filled with the evidence captured BEFORE any teardown.
Decision decide_next_packet(const DecisionInput &in, FailureRecord *out);

struct Lock {
	virtual ~Lock() {}
	virtual void lock() = 0;
	virtual void unlock() = 0;
};
struct NullLock : Lock {
	void lock() override {}
	void unlock() override {}
};

// ---------------------------------------------------------------------------
// Per-stream failure evidence (write-once first failure + seek / length metadata).
// One instance lives in every NetworkStream. Writers: downloader thread and the
// decoder io callbacks. Reader: the decode thread (snapshot). All state changes and
// the snapshot happen under the injected lock; the lock is a leaf (no IO inside).
// ---------------------------------------------------------------------------
class StreamEvidence {
  public:
	explicit StreamEvidence(Lock &lock) : lock(lock) {}
	// Write-once. Later calls are ignored. `detail` is redacted before it is stored.
	// Returns true when this call recorded the first failure.
	bool note_failure(Cause cause, int http_status, uint64_t req_start, uint64_t req_end, uint64_t expected_len,
	                  uint64_t actual_len, uint64_t read_head, uint64_t len, const std::string &detail);
	void note_seek_rejected(uint64_t pos);       // FFmpeg asked to seek beyond the declared length
	void note_length_mismatch(uint64_t total);   // server total != declared length (sticky)
	void clear_seek_evidence();                  // stream (re)initialised or explicit user seek
	void snapshot(StreamState *out) const;       // consistent copy of everything above
	bool has_failure() const;
	bool has_length_mismatch() const;

  private:
	Lock &lock;
	bool has_first = false;
	FailureRecord first;
	bool seek_rej = false;
	uint64_t seek_pos = 0;
	uint64_t length_total = 0;
};

// ---------------------------------------------------------------------------
// Bounded, thread-safe, non-recursive diagnostic log
// ---------------------------------------------------------------------------

class DiagLog {
  public:
	// Returns true on success. Must not block for long; runs on the decode thread.
	typedef bool (*Writer)(const std::string &path, const std::string &data, void *ctx);
	typedef uint64_t (*Clock)(void *ctx); // milliseconds, monotonic-ish

	struct Config {
		std::string path;
		std::string build_id;
		std::string app_version;
		size_t max_records = 48;                  // ring capacity
		size_t max_line = 400;                    // hard cap per rendered record (bytes)
		int max_consecutive_write_failures = 3;   // then stop touching the SD card
	};

	DiagLog(Lock &lock, const Config &cfg, Writer writer, void *writer_ctx, Clock clock, void *clock_ctx);

	// Append a record to the in-memory ring. No file IO. Safe from any thread.
	void record(const FailureRecord &r);
	// Serialize the ring and write it with the injected writer. Re-entrant calls
	// (a writer calling flush) are rejected. Never throws; failures are counted.
	bool flush();

	size_t size() const;
	size_t capacity() const { return cfg.max_records; }
	std::string render() const; // what flush() would write
	bool write_disabled() const { return consecutive_failures >= cfg.max_consecutive_write_failures; }
	int consecutive_write_failures() const { return consecutive_failures; }
	uint64_t dropped() const { return dropped_records; }
	const std::string &path() const { return cfg.path; }

	static std::string format_record(const FailureRecord &r, uint64_t t_ms, size_t max_line);

  private:
	Lock &lock;
	Config cfg;
	Writer writer;
	void *writer_ctx;
	Clock clock;
	void *clock_ctx;
	std::vector<std::string> ring; // already rendered lines
	size_t head = 0;               // next write index
	size_t count = 0;
	uint64_t dropped_records = 0;
	int consecutive_failures = 0;
	volatile bool flushing = false;
	std::string render_locked() const;
};

// ---------------------------------------------------------------------------
// Per-video recovery budget with generation fencing
// ---------------------------------------------------------------------------
class RecoveryController {
  public:
	static const int MAX_ATTEMPTS = 2;
	static const int BASE_BACKOFF_MS = 1500;
	static const int MAX_BACKOFF_MS = 6000;

	enum class Action { NONE, RETRY, GIVE_UP, UNSUPPORTED };
	struct Plan {
		Action action = Action::NONE;
		int attempt = 0;        // 1-based attempt number when RETRY
		int backoff_ms = 0;
		double resume_pos = 0;  // seconds
		unsigned generation = 0; // fence token captured when the plan was made
	};

	// Any player-affecting request (new video, seek, exit, reload, quality change).
	// Bumps the generation so a pending recovery becomes stale. Returns new generation.
	unsigned cancel();
	// Explicit user action on the player (manual reload / play button / new video):
	// resets the runtime budget AND bumps the generation.
	unsigned user_reset();
	// Called by the decode thread when next_decode_type() returned ERROR.
	// `video_key` identifies the video (budget is per key); a new key resets the budget.
	Plan on_runtime_failure(const std::string &video_key, double playback_pos, bool is_livestream);
	// Called after a successful (re)initialisation. Must NOT reset the runtime budget
	// (otherwise startup success + runtime failure would loop forever).
	void on_playback_started(const std::string &video_key);

	bool is_current(unsigned generation) const { return generation == generation_; }
	unsigned generation() const { return generation_; }
	int attempts_used() const { return attempts_; }
	bool exhausted() const { return attempts_ >= MAX_ATTEMPTS; }
	const std::string &budget_key() const { return key_; }

	static int backoff_for_attempt(int attempt);

  private:
	volatile unsigned generation_ = 1;
	std::string key_;
	int attempts_ = 0;
};

// ---------------------------------------------------------------------------
// Ownership of asynchronous video-page loads (review blocker #1).
// A load request is admitted under the player lock and gets an immutable record
// (url, kind, recovery generation) plus an id that travels through the async task
// instead of a pointer to a mutable global. Right before the player is mutated the
// task must re-validate its id under the same lock: a newer player request, a user
// action that bumped the recovery generation (for recovery-originated loads), or
// cancel_all() (exit) makes the publication stale and it is dropped.
// All methods must be called with the player lock held.
// ---------------------------------------------------------------------------
class RequestRegistry {
  public:
	struct Request {
		unsigned id = 0;
		std::string url;
		bool to_play = false;
		bool to_display = false;
		bool from_recovery = false;
		unsigned recovery_generation = 0;
	};
	// recovery_generation == 0 : user / system request (fenced only by newer player requests and exit)
	unsigned admit(const std::string &url, bool to_play, bool to_display, unsigned recovery_generation);
	// start of the async task: fetch the immutable request; false = already superseded
	bool lookup(unsigned id, Request *out) const;
	// publication gate for playing_video_info / vid_change_video_request
	bool may_publish_player(unsigned id, const RecoveryController &rc) const;
	void finish(unsigned id);
	void cancel_all();
	size_t size() const { return requests.size(); }
	unsigned latest_player_request() const { return latest_player; }

  private:
	std::map<unsigned, Request> requests;
	unsigned next_id = 1;
	unsigned latest_player = 0;
	unsigned latest_display = 0;
};

} // namespace playback_diag
