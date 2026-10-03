#include "playback_diag.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace playback_diag {

const char *cause_name(Cause c) {
	switch (c) {
	case Cause::NONE: return "NONE";
	case Cause::HTTP_STATUS: return "HTTP_STATUS";
	case Cause::TRANSPORT: return "TRANSPORT";
	case Cause::SIZE_MISMATCH: return "SIZE_MISMATCH";
	case Cause::LENGTH_UNKNOWN: return "LENGTH_UNKNOWN";
	case Cause::READ_BEYOND_END: return "READ_BEYOND_END";
	case Cause::SEEK_REJECTED: return "SEEK_REJECTED";
	case Cause::STREAM_DEAD: return "STREAM_DEAD";
	case Cause::DEMUX_ERROR: return "DEMUX_ERROR";
	case Cause::REINIT_FAILED: return "REINIT_FAILED";
	case Cause::EOF_SHORT: return "EOF_SHORT";
	case Cause::LENGTH_MISMATCH: return "LENGTH_MISMATCH";
	case Cause::INIT_FAILED: return "INIT_FAILED";
	case Cause::DECODE_ERROR: return "DECODE_ERROR";
	case Cause::LIVE_UNSUPPORTED: return "LIVE_UNSUPPORTED";
	}
	return "?";
}
const char *stream_name(StreamId s) {
	switch (s) {
	case StreamId::UNKNOWN: return "unknown";
	case StreamId::VIDEO: return "video";
	case StreamId::AUDIO: return "audio";
	case StreamId::BOTH: return "both";
	}
	return "?";
}
const char *decision_name(Decision d) {
	switch (d) {
	case Decision::VIDEO: return "VIDEO";
	case Decision::AUDIO: return "AUDIO";
	case Decision::END_OF_STREAM: return "END_OF_STREAM";
	case Decision::ERROR: return "ERROR";
	case Decision::INTERRUPTED: return "INTERRUPTED";
	case Decision::NONE: return "NONE";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// redaction
// ---------------------------------------------------------------------------
static bool token_has_prefix_ci(const std::string &tok, const char *prefix) {
	size_t n = strlen(prefix);
	if (tok.size() < n) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		if (tolower((unsigned char)tok[i]) != tolower((unsigned char)prefix[i])) {
			return false;
		}
	}
	return true;
}
static bool contains_ci(const std::string &hay, const char *needle) {
	size_t n = strlen(needle);
	if (n == 0 || hay.size() < n) {
		return false;
	}
	for (size_t i = 0; i + n <= hay.size(); i++) {
		size_t k = 0;
		while (k < n && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k])) {
			k++;
		}
		if (k == n) {
			return true;
		}
	}
	return false;
}
static bool is_sensitive_key(const std::string &key) {
	// bare query-parameter names that identify a session / signed url even without a keyword above
	static const char *const keys[] = {"sig", "lsig", "sparams", "expire", "ip", "ipbits", "n", "pot", "id", "cpn", NULL};
	for (int i = 0; keys[i]; i++) {
		if (token_has_prefix_ci(key, keys[i]) && key.size() == strlen(keys[i])) {
			return true;
		}
	}
	return false;
}
std::string redact(const std::string &in) {
	// 1. credential-like keyword anywhere: the whole text is dropped (headers, JSON, "key: value", ...)
	static const char *const keywords[] = {"authorization", "bearer",   "cookie",   "token",  "password", "passwd",
	                                       "secret",        "signature", "sig=",    "sapisid", "x-goog",   "authuser",
	                                       "basic ",        "key=",      "apikey",  "oauth",  "credential", NULL};
	for (int i = 0; keywords[i]; i++) {
		if (contains_ci(in, keywords[i])) {
			return "[redacted]";
		}
	}
	// 2. token level: whole URLs / userinfo / e-mail-like tokens are removed entirely, query fragments dropped
	std::string out;
	out.reserve(in.size());
	size_t i = 0;
	while (i < in.size()) {
		if (isspace((unsigned char)in[i])) {
			out.push_back(' ');
			i++;
			continue;
		}
		size_t j = i;
		while (j < in.size() && !isspace((unsigned char)in[j])) {
			j++;
		}
		std::string tok = in.substr(i, j - i);
		i = j;

		if (tok.find("://") != std::string::npos || token_has_prefix_ci(tok, "www.") ||
		    token_has_prefix_ci(tok, "mailto:") || tok.find('@') != std::string::npos) {
			out += "[url]";
			continue;
		}
		if (tok.find('?') != std::string::npos || tok.find('&') != std::string::npos) {
			out += "[redacted]";
			continue;
		}
		size_t eq = tok.find('=');
		if (eq != std::string::npos && is_sensitive_key(tok.substr(0, eq))) {
			out += "[redacted]";
			continue;
		}
		for (char c : tok) {
			out.push_back((unsigned char)c < 0x20 ? '?' : c); // control characters
		}
	}
	return out;
}

// ---------------------------------------------------------------------------
// decision seam
// ---------------------------------------------------------------------------
// Per-stream classification used by decide_next_packet().
enum class StreamStatus { NOT_NEEDED, HAS_PACKETS, AT_END, FAILED, INTERRUPTED, PENDING };

static StreamStatus classify(const DecisionInput &in, int type, bool needed, bool has_packets, Cause *cause,
                             uint64_t *seek_pos) {
	*cause = Cause::NONE;
	*seek_pos = 0;
	if (!needed) {
		return StreamStatus::NOT_NEEDED;
	}
	if (has_packets) {
		return StreamStatus::HAS_PACKETS;
	}
	const StreamState &s = in.stream[type];
	// An empty packet buffer after read_packet() means the last read failed.
	// Intentional cancellation (seek / video change / exit) always wins.
	if (in.interrupt) {
		return StreamStatus::INTERRUPTED;
	}
	if (s.present && (s.error || s.quit)) {
		*cause = s.has_first_failure ? s.first_failure.cause : Cause::STREAM_DEAD;
		return StreamStatus::FAILED;
	}
	if (in.reinit_failed[type]) {
		*cause = Cause::REINIT_FAILED;
		return StreamStatus::FAILED;
	}
	int rr = in.read_result[type];
	if (rr < 0 && rr != AVERROR_EOF_VALUE) {
		*cause = Cause::DEMUX_ERROR;
		return StreamStatus::FAILED;
	}
	// Hard evidence that the DECLARED length is wrong wins over "all declared bytes consumed":
	// a seek beyond `len` was rejected, or the server reported a different total. Such a stream
	// must end in ERROR (recovery / manual action), never in EOF autoplay. Review blocker #3.
	if (s.seek_rejected) {
		*cause = Cause::SEEK_REJECTED;
		*seek_pos = s.seek_rejected_pos;
		return StreamStatus::FAILED;
	}
	if (s.length_mismatch_total) {
		*cause = Cause::LENGTH_MISMATCH;
		return StreamStatus::FAILED;
	}
	if (s.present && s.ready && s.read_head >= s.len) {
		return StreamStatus::AT_END; // every byte consumed: genuine end of media
	}
	if (rr == AVERROR_EOF_VALUE) {
		// Demuxer EOF with bytes remaining and no evidence: benign trailing data. No duration
		// heuristics, so short / live content is safe. A transient downloader failure that was
		// retried successfully is deliberately not evidence.
		return StreamStatus::AT_END;
	}
	return StreamStatus::PENDING; // read succeeded but nothing buffered (should not happen)
}

static void fill_record(const DecisionInput &in, int type, Cause cause, uint64_t seek_pos, FailureRecord *out) {
	const StreamState &s = in.stream[type];
	if (s.has_first_failure) {
		*out = s.first_failure; // downloader evidence captured before the FFmpeg flood
	} else {
		*out = FailureRecord();
	}
	if (out->cause != Cause::NONE && out->cause != cause) {
		// keep the originating downloader failure visible next to the classification
		out->detail = std::string("first=") + cause_name(out->cause) + (out->detail.empty() ? "" : " " + out->detail);
	}
	out->cause = cause;
	if (s.length_mismatch_total && !out->actual_len) {
		out->expected_len = s.len;
		out->actual_len = s.length_mismatch_total;
	}
	out->stream = !in.av_separate ? StreamId::BOTH : type == 0 ? StreamId::VIDEO : StreamId::AUDIO;
	out->read_head = s.read_head;
	out->len = s.len;
	out->ffmpeg_code = in.read_result[type];
	out->av_separate = in.av_separate;
	if (seek_pos) {
		out->seek_pos = seek_pos;
	}
}

Decision decide_next_packet(const DecisionInput &in, FailureRecord *out) {
	const int VIDEO = 0, AUDIO = 1;
	bool video_needed = !in.audio_only;
	Cause cause[2];
	uint64_t seek_pos[2];
	StreamStatus st[2];
	st[VIDEO] = classify(in, VIDEO, video_needed, in.packets_video, &cause[VIDEO], &seek_pos[VIDEO]);
	// combined stream: audio state lives in stream[0] as well
	int audio_state_index = in.av_separate ? AUDIO : VIDEO;
	{
		DecisionInput tmp = in;
		if (!in.av_separate) {
			tmp.stream[AUDIO] = in.stream[VIDEO];
			tmp.read_result[AUDIO] = in.read_result[VIDEO];
			tmp.reinit_failed[AUDIO] = in.reinit_failed[VIDEO];
		}
		st[AUDIO] = classify(tmp, AUDIO, true, in.packets_audio, &cause[AUDIO], &seek_pos[AUDIO]);
	}

	// 1. intentional cancel
	if (st[VIDEO] == StreamStatus::INTERRUPTED || st[AUDIO] == StreamStatus::INTERRUPTED) {
		return Decision::INTERRUPTED;
	}
	// 2. any needed stream failed: stop A/V coherently, even if the other one still has packets
	if (st[VIDEO] == StreamStatus::FAILED || st[AUDIO] == StreamStatus::FAILED) {
		int t;
		if (st[VIDEO] == StreamStatus::FAILED && st[AUDIO] == StreamStatus::FAILED) {
			// prefer the stream that carries downloader evidence
			t = in.stream[AUDIO].has_first_failure && !in.stream[VIDEO].has_first_failure ? AUDIO : VIDEO;
		} else {
			t = st[VIDEO] == StreamStatus::FAILED ? VIDEO : AUDIO;
		}
		if (out) {
			fill_record(in, in.av_separate ? t : audio_state_index, cause[t], seek_pos[t], out);
			if (!in.av_separate) {
				out->stream = StreamId::BOTH;
			}
		}
		return Decision::ERROR;
	}
	// 3. packets available: keep draining (also when the other stream already ended)
	if (st[VIDEO] == StreamStatus::HAS_PACKETS && st[AUDIO] == StreamStatus::HAS_PACKETS) {
		return in.video_dts <= in.audio_dts ? Decision::VIDEO : Decision::AUDIO;
	}
	if (st[VIDEO] == StreamStatus::HAS_PACKETS) {
		return Decision::VIDEO;
	}
	if (st[AUDIO] == StreamStatus::HAS_PACKETS) {
		return Decision::AUDIO;
	}
	// 4. nothing buffered: genuine end only when every needed stream is at its end
	bool video_done = st[VIDEO] == StreamStatus::NOT_NEEDED || st[VIDEO] == StreamStatus::AT_END;
	bool audio_done = st[AUDIO] == StreamStatus::AT_END;
	if (video_done && audio_done) {
		if (in.decoded_video_pending) {
			return Decision::VIDEO; // legacy: let the converter drain the decoded frames first
		}
		return Decision::END_OF_STREAM;
	}
	return Decision::NONE;
}

// ---------------------------------------------------------------------------
// DiagLog
// ---------------------------------------------------------------------------
DiagLog::DiagLog(Lock &lock, const Config &cfg, Writer writer, void *writer_ctx, Clock clock, void *clock_ctx)
    : lock(lock), cfg(cfg), writer(writer), writer_ctx(writer_ctx), clock(clock), clock_ctx(clock_ctx) {
	if (this->cfg.max_records == 0) {
		this->cfg.max_records = 1;
	}
	if (this->cfg.max_line < 64) {
		this->cfg.max_line = 64;
	}
	ring.resize(this->cfg.max_records);
}

static std::string fmt_u64(uint64_t v) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
	return buf;
}
std::string DiagLog::format_record(const FailureRecord &r, uint64_t t_ms, size_t max_line) {
	char head[256];
	snprintf(head, sizeof(head), "t=%llu cause=%s stream=%s http=%d ff=%d pos=%.2f q=%d attempt=%d avsep=%d",
	         (unsigned long long)t_ms, cause_name(r.cause), stream_name(r.stream), r.http_status, r.ffmpeg_code,
	         r.playback_pos, r.quality_p, r.attempt, r.av_separate ? 1 : 0);
	std::string line = head;
	line += " read_head=" + fmt_u64(r.read_head) + " len=" + fmt_u64(r.len);
	if (r.req_end || r.req_start) {
		line += " req=" + fmt_u64(r.req_start) + "-" + fmt_u64(r.req_end);
	}
	if (r.expected_len || r.actual_len) {
		line += " expected=" + fmt_u64(r.expected_len) + " actual=" + fmt_u64(r.actual_len);
	}
	if (r.seek_pos) {
		line += " seek=" + fmt_u64(r.seek_pos);
	}
	if (!r.video_id.empty()) {
		// video ids are 11 chars of [A-Za-z0-9_-]; anything else is not an id and is dropped
		bool ok = r.video_id.size() <= 16;
		for (char c : r.video_id) {
			if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
				ok = false;
			}
		}
		line += " vid=" + std::string(ok ? r.video_id : "[redacted]");
	}
	if (!r.detail.empty()) {
		std::string d = redact(r.detail);
		std::string clean;
		for (char c : d) {
			clean.push_back(c == '"' ? '\'' : c);
		}
		line += " detail=\"" + clean + "\"";
	}
	if (line.size() > max_line) {
		line = line.substr(0, max_line - 3) + "...";
	}
	return line;
}

void DiagLog::record(const FailureRecord &r) {
	uint64_t t = clock ? clock(clock_ctx) : 0;
	std::string line = format_record(r, t, cfg.max_line);
	lock.lock();
	ring[head] = line;
	head = (head + 1) % cfg.max_records;
	if (count < cfg.max_records) {
		count++;
	} else {
		dropped_records++;
	}
	lock.unlock();
}

std::string DiagLog::render_locked() const {
	std::string out = "# FourthTube playback diagnostics build=" + cfg.build_id + " ver=" + cfg.app_version +
	                  " cap=" + fmt_u64(cfg.max_records) + " dropped=" + fmt_u64(dropped_records) + "\n";
	size_t start = count < cfg.max_records ? 0 : head;
	for (size_t i = 0; i < count; i++) {
		out += ring[(start + i) % cfg.max_records];
		out += '\n';
	}
	return out;
}
std::string DiagLog::render() const {
	lock.lock();
	std::string s = render_locked();
	lock.unlock();
	return s;
}
size_t DiagLog::size() const {
	lock.lock();
	size_t n = count;
	lock.unlock();
	return n;
}
bool DiagLog::flush() {
	if (flushing) {
		return false; // re-entrant call from a writer: refuse, never recurse
	}
	if (write_disabled() || !writer) {
		return false;
	}
	flushing = true;
	lock.lock();
	std::string data = render_locked();
	lock.unlock();
	bool ok = writer(cfg.path, data, writer_ctx); // outside the lock: the writer may be slow (SD card)
	if (ok) {
		consecutive_failures = 0;
	} else {
		consecutive_failures++;
	}
	flushing = false;
	return ok;
}

// ---------------------------------------------------------------------------
// StreamEvidence
// ---------------------------------------------------------------------------
bool StreamEvidence::note_failure(Cause cause, int http_status, uint64_t req_start, uint64_t req_end,
                                  uint64_t expected_len, uint64_t actual_len, uint64_t read_head, uint64_t len,
                                  const std::string &detail) {
	std::string clean = redact(detail); // string work outside the lock
	bool won = false;
	lock.lock();
	if (!has_first) {
		first = FailureRecord();
		first.cause = cause;
		first.http_status = http_status;
		first.req_start = req_start;
		first.req_end = req_end;
		first.expected_len = expected_len;
		first.actual_len = actual_len;
		first.read_head = read_head;
		first.len = len;
		first.detail = clean;
		has_first = true;
		won = true;
	}
	lock.unlock();
	return won;
}
void StreamEvidence::note_seek_rejected(uint64_t pos) {
	lock.lock();
	if (!seek_rej) {
		seek_rej = true;
		seek_pos = pos;
	}
	lock.unlock();
}
void StreamEvidence::note_length_mismatch(uint64_t total) {
	lock.lock();
	if (!length_total) {
		length_total = total;
	}
	lock.unlock();
}
void StreamEvidence::clear_seek_evidence() {
	lock.lock();
	seek_rej = false;
	seek_pos = 0;
	lock.unlock();
}
void StreamEvidence::snapshot(StreamState *out) const {
	lock.lock();
	out->has_first_failure = has_first;
	if (has_first) {
		out->first_failure = first;
	}
	out->seek_rejected = seek_rej;
	out->seek_rejected_pos = seek_pos;
	out->length_mismatch_total = length_total;
	lock.unlock();
}
bool StreamEvidence::has_failure() const {
	lock.lock();
	bool r = has_first;
	lock.unlock();
	return r;
}
bool StreamEvidence::has_length_mismatch() const {
	lock.lock();
	bool r = length_total != 0;
	lock.unlock();
	return r;
}

// ---------------------------------------------------------------------------
// RequestRegistry
// ---------------------------------------------------------------------------
unsigned RequestRegistry::admit(const std::string &url, bool to_play, bool to_display, unsigned recovery_generation) {
	Request r;
	r.id = next_id++;
	if (next_id == 0) {
		next_id = 1;
	}
	r.url = url;
	r.to_play = to_play;
	r.to_display = to_display;
	r.from_recovery = recovery_generation != 0;
	r.recovery_generation = recovery_generation;
	if (to_play) {
		latest_player = r.id;
	}
	if (to_display) {
		latest_display = r.id;
	}
	requests[r.id] = r;
	// drop superseded records: anything that is neither the latest player nor the latest display request
	for (std::map<unsigned, Request>::iterator it = requests.begin(); it != requests.end();) {
		if (it->first != latest_player && it->first != latest_display) {
			it = requests.erase(it);
		} else {
			++it;
		}
	}
	return r.id;
}
bool RequestRegistry::lookup(unsigned id, Request *out) const {
	std::map<unsigned, Request>::const_iterator it = requests.find(id);
	if (it == requests.end()) {
		return false;
	}
	if (out) {
		*out = it->second;
	}
	return true;
}
bool RequestRegistry::may_publish_player(unsigned id, const RecoveryController &rc) const {
	std::map<unsigned, Request>::const_iterator it = requests.find(id);
	if (it == requests.end() || !it->second.to_play || id != latest_player) {
		return false; // superseded by a newer player request, or cancelled
	}
	if (it->second.from_recovery && !rc.is_current(it->second.recovery_generation)) {
		return false; // user acted (seek / play / reload / quality / exit) while the recovery refresh was in flight
	}
	return true;
}
void RequestRegistry::finish(unsigned id) {
	requests.erase(id);
	if (latest_player == id) {
		latest_player = 0;
	}
	if (latest_display == id) {
		latest_display = 0;
	}
}
void RequestRegistry::cancel_all() {
	requests.clear();
	latest_player = latest_display = 0;
}

// ---------------------------------------------------------------------------
// RecoveryController
// ---------------------------------------------------------------------------
int RecoveryController::backoff_for_attempt(int attempt) {
	if (attempt < 1) {
		attempt = 1;
	}
	int ms = BASE_BACKOFF_MS;
	for (int i = 1; i < attempt && ms < MAX_BACKOFF_MS; i++) {
		ms *= 2;
	}
	return ms > MAX_BACKOFF_MS ? MAX_BACKOFF_MS : ms;
}
unsigned RecoveryController::cancel() {
	generation_ = generation_ + 1;
	if (generation_ == 0) {
		generation_ = 1;
	}
	return generation_;
}
unsigned RecoveryController::user_reset() {
	attempts_ = 0;
	return cancel();
}
void RecoveryController::on_playback_started(const std::string &video_key) {
	if (key_ != video_key) {
		// a different video started without going through on_runtime_failure: fresh budget
		key_ = video_key;
		attempts_ = 0;
	}
	// same video: keep the budget as is (recovery-initiated restart must not refill it)
}
RecoveryController::Plan RecoveryController::on_runtime_failure(const std::string &video_key, double playback_pos,
                                                                bool is_livestream) {
	Plan plan;
	plan.generation = generation_;
	if (key_ != video_key) {
		key_ = video_key;
		attempts_ = 0;
	}
	plan.resume_pos = (playback_pos == playback_pos && playback_pos > 0) ? playback_pos : 0; // NaN / negative -> 0
	if (is_livestream) {
		plan.action = Action::UNSUPPORTED;
		return plan;
	}
	if (attempts_ >= MAX_ATTEMPTS) {
		plan.action = Action::GIVE_UP;
		plan.attempt = attempts_;
		return plan;
	}
	attempts_++;
	plan.action = Action::RETRY;
	plan.attempt = attempts_;
	plan.backoff_ms = backoff_for_attempt(attempts_);
	return plan;
}

} // namespace playback_diag
