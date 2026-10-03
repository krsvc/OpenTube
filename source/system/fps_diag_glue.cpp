#ifdef DEF_FPS_DIAG
#include "headers.hpp"
#include "system/fps_diag_glue.hpp"
#include "util/fps_diag.hpp"
#include "network_decoder/network_decoder_multiple.hpp"

namespace {
fps_diag::Probes probes;
fps_diag::LineRing<8, fps_diag::Analyzer::LINE_CAP> ring; // main thread -> log writer
bool ring_sink(const char *line, size_t len, void *) { return ring.push(line, len); }
fps_diag::Analyzer analyzer(probes, ring_sink, nullptr);

// svcGetSystemTick() runs at SYSCLOCK_ARM11 = 268111856 Hz, i.e. 33513982 ticks per 125000 us. Quotient and remainder
// are scaled separately so the product never overflows at any uptime (r9 used 125 and produced milliseconds). u32 wraps
// after ~71 min; every consumer only takes differences of spans far shorter than that.
uint32_t now_us() {
	const uint64_t t = svcGetSystemTick(), per = 33513982;
	return (uint32_t)((t / per) * 125000 + (t % per) * 125000 / per);
}
uint32_t ms_to_us(double ms) {
	if (!(ms > 0)) {
		return 0; // also NaN
	}
	return ms >= 4000000.0 ? 4000000000u : (uint32_t)(ms * 1000.0);
}

// convert thread only
uint32_t conv_t0 = 0;
bool conv_retried = false;
bool conv_paused_seen = false;
uint32_t conv_sync_t0 = 0;
uint32_t conv_copy_t0 = 0;
// main thread only
uint32_t begin_t0 = 0;
uint32_t submit_t0 = 0;
bool debug_drawn = false;
} // namespace

namespace fps_hooks {

void dec_play_start() { probes.session.on_play_start(); }
void dec_generation_bump() { probes.session.bump(); }
void dec_session_meta(double framerate, int width, int height, int decoder_type, int quality, bool audio_only,
                      bool live, const char *client) {
	using DT = NetworkMultipleDecoder::DecoderType;
	uint32_t kind = fps_diag::DEC_UNKNOWN;
	switch ((DT)decoder_type) {
	case DT::HW:
		kind = fps_diag::DEC_HW;
		break;
	case DT::MT_FRAME:
		kind = fps_diag::DEC_MT_FRAME;
		break;
	case DT::MT_SLICE:
		kind = fps_diag::DEC_MT_SLICE;
		break;
	case DT::ST:
		kind = fps_diag::DEC_SINGLE;
		break;
	default:
		break;
	}
	uint32_t mfps = (framerate > 0 && framerate < 1000) ? (uint32_t)(framerate * 1000.0 + 0.5) : 0;
	uint32_t flags = (audio_only ? (uint32_t)fps_diag::S_AUDIO_ONLY : 0u) | (live ? (uint32_t)fps_diag::S_LIVE : 0u);
	probes.session.set_meta(mfps, width > 0 ? width : 0, height > 0 ? height : 0, kind, quality > 0 ? quality : 0, flags,
	                        client);
}
void dec_backpressure_wait() { probes.dec.on_backpressure_wait(); }
void dec_video_decoded(bool frame_queued, double decode_ms) { probes.dec.on_video_decode(frame_queued, ms_to_us(decode_ms)); }

// audio (decode thread, play channel 0). `size` is decode_audio()'s size: bytes per channel (2 * samples per channel),
// the same unit Util_speaker_add_buffer() takes.
void aud_session(int src_rate, int channels, int speaker_rate) {
	probes.audio.set_meta(src_rate > 0 ? src_rate : 0, channels > 0 ? channels : 0, speaker_rate > 0 ? speaker_rate : 0);
}
void aud_decoded(uint32_t result_code, double decode_ms) {
	if (result_code != DEF_ERR_NEED_MORE_INPUT) { // waiting for input is not a decode
		probes.audio.on_decoded(result_code == 0, ms_to_us(decode_ms));
	}
}
void aud_add_begin(const unsigned char *pcm, int size, int channels) {
	uint32_t ahead = Util_speaker_queued_samples(0);
	bool running = Util_speaker_is_playing(0) || Util_speaker_is_paused(0);
	uint32_t values = (size > 0 && channels > 0) ? (uint32_t)(size / 2) * (uint32_t)channels : 0;
	probes.audio.on_add_begin(ahead, running, probes.session.generation.get(), (const int16_t *)pcm, values);
}
void aud_add_retry(uint32_t result_code) { probes.audio.on_add_retry(result_code != DEF_ERR_OUT_OF_LINEAR_MEMORY); }
void aud_added(int size) { probes.audio.on_added(size > 0 ? (uint32_t)(size / 2) : 0); }

void conv_get_begin() {
	conv_t0 = now_us();
	conv_retried = false;
	conv_paused_seen = false;
}
void conv_get_retry(bool paused) {
	conv_retried = true;
	conv_paused_seen = conv_paused_seen || paused;
}
void conv_frame_got() { probes.conv.on_frame_got(now_us() - conv_t0, conv_retried && !conv_paused_seen); }
void conv_converted(double convert_ms) { probes.conv.on_converted(ms_to_us(convert_ms)); }
void conv_sync_begin() { conv_sync_t0 = now_us(); }
void conv_sync_end(double pts, double audio_pos) {
	uint32_t now = now_us();
	bool have_clock = audio_pos >= 0;
	double skew_s = have_clock ? pts - audio_pos : 0;
	int32_t skew_us = skew_s > 2000.0 ? 2000000000 : skew_s < -2000.0 ? -2000000000 : (int32_t)(skew_s * 1000000.0);
	probes.conv.on_synced(now - conv_sync_t0, have_clock, skew_us);
	conv_copy_t0 = now;
}
void conv_published(double pts, double tempo) {
	uint32_t now = now_us();
	uint32_t tempo_milli = (tempo > 0 && tempo < 100) ? (uint32_t)(tempo * 1000.0 + 0.5) : 0;
	int64_t pts_us = (pts > -1e9 && pts < 1e9) ? (int64_t)(pts * 1000000.0) : 0;
	probes.conv.on_published(now, now - conv_copy_t0, pts_us, tempo_milli, probes.session.generation.get());
}
void conv_skipped_draw() { probes.conv.on_skipped_draw(); }

void render_video_drawn() { probes.render.on_video_drawn(probes.conv.seq.load(std::memory_order_acquire)); }
void frame_begin_enter() { begin_t0 = now_us(); }
void frame_begin_exit() { probes.render.on_frame_begin(now_us() - begin_t0); }
void submit_enter() { submit_t0 = now_us(); }
void submit_exit() {
	uint32_t now = now_us();
	probes.render.on_submitted(now, now - submit_t0, probes.session.generation.get(), probes.session.nominal_mfps.get(),
	                           probes.conv.tempo_milli.get());
}
void debug_view_drawn() { debug_drawn = true; }
void main_tick(uint32_t flags, const char *scene, WaitingFn decoder_waiting) {
	probes.dsp.frames.set(ndspGetFrameCount()); // plain 32-bit loads of the ndsp thread's cumulative counters
	probes.dsp.dropped.set(ndspGetDroppedFrames());
	fps_diag::StateSample s;
	s.now_us = now_us();
	// the decoder's waiting status takes its critical_op_lock: sampled once per window, not per frame
	if (analyzer.window_due(s.now_us) && decoder_waiting && decoder_waiting()) {
		flags |= fps_diag::F_WAITING;
	}
	if (debug_drawn) {
		flags |= fps_diag::M_DEBUG_VIEW;
		debug_drawn = false;
	}
	s.flags = flags;
	s.scene = scene;
	analyzer.tick(s);
}
const char *summary_line(int i) { return analyzer.summary(i); }
void exit_flush() { analyzer.flush(); }

// ---------------------------------------------------------------------------------------------------------------
// log writer: runs on the freeze watchdog thread (250 ms tick), direct FS IPC, no malloc / stdio / app locks
static char log_path[64];
static char prev_path[64];
static char header[200];
static size_t header_len = 0;
static fps_diag::LogBudget budget{fps_diag::LogPolicy()};
static bool run_started = false;
static char line_buf[fps_diag::Analyzer::LINE_CAP];

void log_init() { // main thread, before the watchdog starts
	snprintf(log_path, sizeof(log_path), "%sfps_diag.log", (DEF_MAIN_DIR).c_str());
	snprintf(prev_path, sizeof(prev_path), "%sfps_diag.prev.log", (DEF_MAIN_DIR).c_str());
	fps_diag::LogPolicy pol;
	int n = snprintf(header, sizeof(header),
	                 "# FourthTube FPS diag fmt=2 build=%s ver=%s window_ms=%u interval_max_windows=%d vblank_us=%u "
	                 "run_budget=%u file_cap=%u\n",
	                 DEF_DIAG_BUILD_ID, (DEF_CURRENT_APP_VER).c_str(), (unsigned)(fps_diag::WINDOW_US / 1000),
	                 fps_diag::INTERVAL_MAX_WINDOWS, (unsigned)fps_diag::VBLANK_US, (unsigned)pol.run_budget,
	                 (unsigned)pol.file_cap);
	header_len = (n > 0 && (size_t)n < sizeof(header)) ? (size_t)n : 0;
}

static bool write_at(Handle file, u64 *offset, const char *data, size_t len) {
	u32 written = 0;
	bool ok = R_SUCCEEDED(FSFILE_Write(file, &written, *offset, data, len, 0)) && written == len;
	*offset += written;
	return ok;
}
static bool writer_disabled = false; // rotation impossible: never grow the file past its cap
static void publish_state() {
	uint32_t st = budget.exhausted_budget()               ? fps_diag::LOG_FULL
	              : (budget.stopped() || writer_disabled) ? fps_diag::LOG_FAILED
	              : probes.log.lines.get()                ? fps_diag::LOG_OK
	                                                      : fps_diag::LOG_IDLE;
	probes.log.state.store(st, std::memory_order_relaxed);
}

void log_drain() {
	size_t len = 0;
	if (!log_path[0] || !ring.pop(line_buf, sizeof(line_buf), &len)) {
		return;
	}
	if (budget.stopped() || writer_disabled) {
		do {
			budget.admit(len); // counted as dropped
			probes.log.dropped.add(1);
		} while (ring.pop(line_buf, sizeof(line_buf), &len));
		publish_state();
		return;
	}
	FS_Archive sdmc;
	if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
		budget.on_write_result(false);
		probes.log.failures.add(1);
		probes.log.dropped.add(1);
		publish_state();
		return;
	}
	Handle file;
	u64 size = 0;
	bool opened = R_SUCCEEDED(
	    FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_ASCII, log_path), FS_OPEN_WRITE | FS_OPEN_CREATE, 0));
	if (opened) {
		FSFILE_GetSize(file, &size);
		if (!run_started && budget.rotate_needed(size)) {
			// keep this run's lines and the previous file; only an older .prev file is replaced
			FSFILE_Close(file);
			FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, prev_path));
			FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, log_path), sdmc, fsMakePath(PATH_ASCII, prev_path));
			size = 0;
			opened = R_SUCCEEDED(
			    FSUSER_OpenFile(&file, sdmc, fsMakePath(PATH_ASCII, log_path), FS_OPEN_WRITE | FS_OPEN_CREATE, 0));
			if (opened) {
				FSFILE_GetSize(file, &size);
				if (budget.rotate_needed(size)) { // rename failed: stop for this run instead of growing the file
					FSFILE_Close(file);
					opened = false;
					writer_disabled = true;
				}
			}
		}
	}
	if (!opened) {
		budget.on_write_result(false);
		probes.log.failures.add(1);
		probes.log.dropped.add(1);
		FSUSER_CloseArchive(sdmc);
		publish_state();
		return;
	}
	if (!run_started) {
		run_started = true;
		if (header_len && budget.admit(header_len) == fps_diag::LogBudget::Verdict::WRITE) {
			budget.on_write_result(write_at(file, &size, header, header_len));
		}
	}
	do {
		fps_diag::LogBudget::Verdict v = budget.admit(len);
		if (v == fps_diag::LogBudget::Verdict::WRITE) {
			bool ok = write_at(file, &size, line_buf, len);
			budget.on_write_result(ok);
			if (ok) {
				probes.log.lines.add(1);
				probes.log.bytes.add(len);
			} else {
				probes.log.failures.add(1);
			}
		} else {
			probes.log.dropped.add(1);
			if (v == fps_diag::LogBudget::Verdict::MARKER_THEN_STOP) {
				char marker[160];
				size_t m = fps_diag::LogBudget::format_marker(marker, sizeof(marker), probes.log.lines.get(),
				                                              budget.used());
				write_at(file, &size, marker, m);
			}
		}
	} while (ring.pop(line_buf, sizeof(line_buf), &len));
	FSFILE_Flush(file);
	FSFILE_Close(file);
	FSUSER_CloseArchive(sdmc);
	publish_state();
}

} // namespace fps_hooks
#endif
