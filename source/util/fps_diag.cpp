// compiled only when DEF_FPS_DIAG is defined (diag builds unless DIAG_FPS=0; host tests define it)
#ifdef DEF_FPS_DIAG
#include "fps_diag.hpp"
#include <cstdio>
#include <cstring>

namespace fps_diag {

int vblank_bucket(uint32_t interval_us) {
	uint32_t n = (interval_us + VBLANK_US / 2) / VBLANK_US;
	return n >= (uint32_t)(HIST_N - 1) ? HIST_N - 1 : (int)n;
}
int duration_bucket(uint32_t us) {
	static const uint32_t limits[HIST_N - 1] = {1000, 2000, 4000, 8000, 16000, 33000, 66000};
	for (int i = 0; i < HIST_N - 1; i++) {
		if (us < limits[i]) {
			return i;
		}
	}
	return HIST_N - 1;
}
int skew_bucket(int32_t skew_us) {
	if (skew_us < -100000) {
		return 0;
	}
	if (skew_us < -50000) {
		return 1;
	}
	if (skew_us < -17000) {
		return 2;
	}
	if (skew_us < -3000) {
		return 3;
	}
	if (skew_us <= 3000) {
		return 4;
	}
	if (skew_us <= 17000) {
		return 5;
	}
	if (skew_us <= 50000) {
		return 6;
	}
	return 7;
}
uint32_t hitch_threshold_us(uint32_t nominal_mfps, uint32_t tempo_milli) {
	uint64_t rate = (uint64_t)nominal_mfps * tempo_milli; // frames per 1e6 s
	if (rate == 0) {
		return 0;
	}
	uint64_t period_us = 1000000000000ULL / rate;
	uint64_t need = (period_us + VBLANK_US - 1) / VBLANK_US; // VBlanks one source frame normally stays on screen
	if (need < 1) {
		need = 1;
	}
	if (need > 1000) {
		need = 1000;
	}
	return (uint32_t)((need + 1) * VBLANK_US - VBLANK_US / 2);
}
int lead_bucket(uint32_t us) {
	static const uint32_t limits[HIST_N - 1] = {20000, 50000, 100000, 200000, 300000, 500000, 1000000};
	for (int i = 0; i < HIST_N - 1; i++) {
		if (us < limits[i]) {
			return i;
		}
	}
	return HIST_N - 1;
}
uint32_t count_full_scale(const int16_t *pcm, uint32_t n_values) {
	uint32_t n = 0;
	for (uint32_t i = 0; pcm && i < n_values; i++) {
		n += (pcm[i] == 32767 || pcm[i] == -32768);
	}
	return n;
}

void Session::set_meta(uint32_t mfps, uint32_t w, uint32_t h, uint32_t kind, uint32_t q, uint32_t session_flags,
                       const char *client_literal) {
	nominal_mfps.set(mfps);
	width.set(w);
	height.set(h);
	decoder_kind.set(kind);
	quality.set(q);
	flags.set(session_flags);
	client.store(client_literal, std::memory_order_relaxed);
}

void DecoderProbe::on_video_decode(bool frame_queued, uint32_t us) {
	if (!frame_queued) {
		return;
	}
	decoded.add(1);
	decode_us.add(us);
	decode_hist.add(duration_bucket(us));
}

void AudioProbe::set_meta(uint32_t src, uint32_t ch, uint32_t spk) {
	src_rate.set(src);
	channels.set(ch);
	speaker_rate.set(spk);
}
void AudioProbe::on_decoded(bool ok, uint32_t us) {
	if (!ok) {
		errors.add(1);
		return;
	}
	decoded.add(1);
	decode_us.add(us);
	decode_hist.add(duration_bucket(us));
}
void AudioProbe::on_add_begin(uint32_t queued_ahead_samples, bool running, uint32_t generation, const int16_t *pcm,
                              uint32_t n_values) {
	if (!have_gen || generation != seen_gen) {
		have_gen = true; // first buffer of this generation (play start / seek): the queue was empty on purpose
		seen_gen = generation;
	} else if (!running) {
		dry.add(1);
	}
	uint32_t rate = speaker_rate.get();
	if (rate) {
		lead_hist.add(lead_bucket((uint32_t)((uint64_t)queued_ahead_samples * 1000000 / rate)));
	}
	full_scale.add(count_full_scale(pcm, n_values));
}
void AudioProbe::on_add_retry(bool queue_full) {
	if (queue_full) {
		full_retries.add(1);
	} else {
		add_errors.add(1);
	}
}
void AudioProbe::on_added(uint32_t nsamples) {
	queued.add(1);
	samples.add(nsamples);
}

void ConverterProbe::on_frame_got(uint32_t wait_us, bool starved) {
	got.add(1);
	if (starved) {
		starve_events.add(1);
		starve_us.add(wait_us);
	}
}
void ConverterProbe::on_converted(uint32_t us) {
	convert_us.add(us);
	convert_hist.add(duration_bucket(us));
}
void ConverterProbe::on_synced(uint32_t wait_us, bool have_audio_clock, int32_t skew_us) {
	sync_us.add(wait_us);
	sync_hist.add(duration_bucket(wait_us));
	if (have_audio_clock) {
		skew_hist.add(skew_bucket(skew_us));
	} else {
		no_audio.add(1);
	}
}
void ConverterProbe::on_published(uint32_t now_us, uint32_t copy, int64_t pts_us, uint32_t tempo, uint32_t generation) {
	if (!have_last || generation != seen_gen) {
		have_last = true; // first publication of this generation: baseline only
		seen_gen = generation;
	} else {
		pub_interval_hist.add(vblank_bucket(now_us - last_pub_us));
		int64_t d = pts_us - last_pts_us;
		if (d > 0 && d <= 1000000) {
			media_us.add((uint32_t)d);
		} else {
			pts_disc.add(1);
		}
	}
	last_pub_us = now_us;
	last_pts_us = pts_us;
	copy_us.add(copy);
	copy_hist.add(duration_bucket(copy));
	tempo_milli.set(tempo);
	seq.store(seq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

void RenderProbe::on_frame_begin(uint32_t wait_us) {
	begin_pending = true;
	pending_begin_us = wait_us;
}
void RenderProbe::on_video_drawn(uint32_t s) {
	video_pending = true;
	pending_seq = s;
}
void RenderProbe::on_submitted(uint32_t now_us, uint32_t sub_us, uint32_t generation, uint32_t nominal_mfps,
                               uint32_t tempo_milli) {
	submits.add(1);
	if (!video_pending) {
		begin_pending = false;
		return;
	}
	video_submits.add(1);
	submit_us.add(sub_us);
	submit_hist.add(duration_bucket(sub_us));
	if (begin_pending) {
		begin_us.add(pending_begin_us);
		begin_hist.add(duration_bucket(pending_begin_us));
	}
	if (!have_last || generation != seen_gen) {
		// first video submit of this generation: counts as shown, but no interval / gap across the reset
		have_last = true;
		seen_gen = generation;
		last_seq = pending_seq;
		last_unique_us = now_us;
		unique.add(1);
	} else {
		uint32_t d = pending_seq - last_seq;
		if (d == 0) {
			repeats.add(1);
		} else if (d < 0x80000000u) {
			uint32_t iv = now_us - last_unique_us;
			unique.add(1);
			missed.add(d - 1);
			unique_interval_hist.add(vblank_bucket(iv));
			uint32_t limit = hitch_threshold_us(nominal_mfps, tempo_milli);
			if (limit && iv >= limit) {
				hitches.add(1);
			}
			last_seq = pending_seq;
			last_unique_us = now_us;
		} else {
			last_seq = pending_seq; // older than the last one: cannot happen with a monotonic seq, rebase
		}
	}
	video_pending = false;
	begin_pending = false;
}

// ---------------------------------------------------------------------------------------------------------------
static void snap_hist(const Hist &h, uint32_t *out) {
	for (int i = 0; i < HIST_N; i++) {
		out[i] = h.b[i].get();
	}
}
void take_snapshot(const Probes &p, Counts *out) {
	uint32_t *v = out->v;
	v[DEC_FRAMES] = p.dec.decoded.get();
	v[DEC_US] = p.dec.decode_us.get();
	v[DEC_BP] = p.dec.backpressure.get();
	v[CONV_GOT] = p.conv.got.get();
	v[CONV_STARVE_EV] = p.conv.starve_events.get();
	v[CONV_STARVE_US] = p.conv.starve_us.get();
	v[CONV_US] = p.conv.convert_us.get();
	v[CONV_SYNC_US] = p.conv.sync_us.get();
	v[CONV_NO_AUDIO] = p.conv.no_audio.get();
	v[CONV_COPY_US] = p.conv.copy_us.get();
	v[CONV_SKIPPED] = p.conv.skipped_draw.get();
	v[CONV_MEDIA_US] = p.conv.media_us.get();
	v[CONV_PTS_DISC] = p.conv.pts_disc.get();
	v[CONV_PUB] = p.conv.published();
	v[R_SUBMITS] = p.render.submits.get();
	v[R_VIDEO_SUBMITS] = p.render.video_submits.get();
	v[R_UNIQUE] = p.render.unique.get();
	v[R_REPEATS] = p.render.repeats.get();
	v[R_MISSED] = p.render.missed.get();
	v[R_HITCHES] = p.render.hitches.get();
	v[R_SUBMIT_US] = p.render.submit_us.get();
	v[R_BEGIN_US] = p.render.begin_us.get();
	snap_hist(p.dec.decode_hist, v + H_DEC);
	snap_hist(p.conv.convert_hist, v + H_CONV);
	snap_hist(p.conv.sync_hist, v + H_SYNC);
	snap_hist(p.conv.copy_hist, v + H_COPY);
	snap_hist(p.conv.skew_hist, v + H_SKEW);
	snap_hist(p.conv.pub_interval_hist, v + H_PUB_IV);
	snap_hist(p.render.unique_interval_hist, v + H_UNIQ_IV);
	snap_hist(p.render.submit_hist, v + H_SUBMIT);
	snap_hist(p.render.begin_hist, v + H_BEGIN);
	v[A_DEC] = p.audio.decoded.get();
	v[A_ERR] = p.audio.errors.get();
	v[A_DEC_US] = p.audio.decode_us.get();
	v[A_QUEUED] = p.audio.queued.get();
	v[A_SAMPLES] = p.audio.samples.get();
	v[A_FULL] = p.audio.full_retries.get();
	v[A_ADD_ERR] = p.audio.add_errors.get();
	v[A_DRY] = p.audio.dry.get();
	v[A_FS] = p.audio.full_scale.get();
	v[DSP_FRAMES] = p.dsp.frames.get();
	v[DSP_DROPPED] = p.dsp.dropped.get();
	snap_hist(p.audio.decode_hist, v + H_ADEC);
	snap_hist(p.audio.lead_hist, v + H_LEAD);
}
void delta(const Counts &before, const Counts &after, Counts *out) {
	for (int i = 0; i < FIELD_N; i++) {
		out->v[i] = after.v[i] - before.v[i]; // unsigned: correct across a u32 wrap
	}
}
void Accum::clear() {
	for (int i = 0; i < FIELD_N; i++) {
		v[i] = 0;
	}
}
void Accum::add(const Counts &d) {
	for (int i = 0; i < FIELD_N; i++) {
		v[i] += d.v[i];
	}
}

const char *reason_name(int bit) {
	static const char *const names[20] = {"stop", "pause", "seek", "wait",  "eof", "kbd",   "bg",    "audio", "b8",
	                                      "b9",   "b10",   "b11",  "b12",   "b13", "b14",   "b15",   "gen",   "long",
	                                      "nosrc", "settle"};
	return bit >= 0 && bit < 20 ? names[bit] : "?";
}
static const char *decoder_name(uint32_t kind) {
	switch (kind) {
	case DEC_HW:
		return "HW";
	case DEC_MT_FRAME:
		return "MTF";
	case DEC_MT_SLICE:
		return "MTS";
	case DEC_SINGLE:
		return "ST";
	default:
		return "?";
	}
}

// ---------------------------------------------------------------------------------------------------------------
// bounded text output: literals and integers only
namespace {
struct Out {
	char *p;
	char *end; // last usable byte (room for NUL kept)
	Out(char *buf, size_t cap) : p(buf), end(buf + (cap ? cap - 1 : 0)) {
		if (cap) {
			*buf = '\0';
		}
	}
	void fmt(const char *f, unsigned long long a = 0, unsigned long long b = 0) {
		if (p >= end) {
			return;
		}
		int n = snprintf(p, end - p + 1, f, a, b);
		if (n > 0) {
			p += (size_t)n > (size_t)(end - p) ? (size_t)(end - p) : (size_t)n;
		}
	}
	void lit(const char *s) { // literal, bounded, printable, no spaces
		if (!s) {
			s = "?";
		}
		for (int i = 0; s[i] && i < 40 && p < end; i++) {
			char c = s[i];
			*p++ = (c > ' ' && c < 127) ? c : '_';
		}
		*p = '\0';
	}
	void kv(const char *k, unsigned long long v) {
		fmt(" ", 0);
		lit(k);
		fmt("=%llu", v);
	}
	void milli(const char *k, uint64_t v) { // v / 1000 with 3 decimals
		fmt(" ", 0);
		lit(k);
		fmt("=%llu.%03llu", v / 1000, v % 1000);
	}
	void hist(const char *k, const uint64_t *h) {
		fmt(" ", 0);
		lit(k);
		fmt("=%llu", h[0]);
		for (int i = 1; i < HIST_N; i++) {
			fmt(",%llu", h[i]);
		}
	}
};
uint64_t avg(uint64_t sum, uint64_t n) { return n ? sum / n : 0; }
} // namespace

void Analyzer::capture_meta() {
	iv.have_meta = true;
	iv.mfps = p.session.nominal_mfps.get();
	iv.width = p.session.width.get();
	iv.height = p.session.height.get();
	iv.quality = p.session.quality.get();
	iv.decoder_kind = p.session.decoder_kind.get();
	iv.session_flags = p.session.flags.get();
	iv.client = p.session.client.load(std::memory_order_relaxed);
	iv.tempo_milli = p.conv.tempo_milli.get();
	iv.a_src_rate = p.audio.src_rate.get();
	iv.a_channels = p.audio.channels.get();
	iv.a_speaker_rate = p.audio.speaker_rate.get();
}

size_t Analyzer::format_interval(char *buf, size_t cap) const {
	Out o(buf, cap);
	const uint64_t *s = iv.steady.v;
	o.fmt("fps1 i=%llu", line_no);
	o.kv("gen", iv.key.gen);
	o.fmt(" scn=", 0);
	o.lit(iv.scene);
	o.fmt(" cli=", 0);
	o.lit(iv.have_meta && iv.client ? iv.client : "?");
	o.fmt(" dec=", 0);
	o.lit(decoder_name(iv.decoder_kind));
	o.fmt(" res=%llux%llu", iv.width, iv.height);
	o.milli("src", iv.mfps);
	o.milli("spd", iv.tempo_milli);
	o.kv("q", iv.quality);
	o.kv("audio", (iv.session_flags & S_AUDIO_ONLY) ? 1 : 0);
	o.kv("live", (iv.session_flags & S_LIVE) ? 1 : 0);
	o.kv("n3ds", (iv.flags_or & M_NEW3DS) ? 1 : 0);
	o.kv("eco", (iv.flags_or & M_ECO) ? 1 : 0);
	o.kv("fs", (iv.flags_or & M_FULLSCREEN) ? 1 : 0);
	o.kv("dbg", (iv.flags_or & M_DEBUG_VIEW) ? 1 : 0);
	o.kv("tdbg", (iv.flags_or & M_TOP_DEBUG) ? 1 : 0);
	o.kv("mix", iv.mixed ? 1 : 0);
	o.fmt(" | steady", 0);
	o.kv("w", iv.steady_windows);
	o.kv("wall_ms", iv.steady_wall_us / 1000);
	o.kv("stall", iv.stall_windows);
	o.kv("pub", s[CONV_PUB]);
	o.kv("uniq", s[R_UNIQUE]);
	o.kv("rep", s[R_REPEATS]);
	o.kv("miss", s[R_MISSED]);
	o.kv("hitch", s[R_HITCHES]);
	o.kv("vsub", s[R_VIDEO_SUBMITS]);
	o.kv("sub", s[R_SUBMITS]);
	o.kv("skipd", s[CONV_SKIPPED]);
	o.kv("media_ms", s[CONV_MEDIA_US] / 1000);
	o.kv("ptsdisc", s[CONV_PTS_DISC]);
	o.kv("dec", s[DEC_FRAMES]);
	o.kv("bp", s[DEC_BP]);
	o.kv("got", s[CONV_GOT]);
	o.fmt(" starve=%llu/%llums", s[CONV_STARVE_EV], s[CONV_STARVE_US] / 1000);
	o.kv("noaud", s[CONV_NO_AUDIO]);
	o.fmt(" | avg_us", 0);
	o.kv("dec", avg(s[DEC_US], s[DEC_FRAMES]));
	o.kv("conv", avg(s[CONV_US], s[CONV_GOT]));
	o.kv("sync", avg(s[CONV_SYNC_US], s[CONV_GOT]));
	o.kv("copy", avg(s[CONV_COPY_US], s[CONV_PUB]));
	o.kv("sub", avg(s[R_SUBMIT_US], s[R_VIDEO_SUBMITS]));
	o.kv("beg", avg(s[R_BEGIN_US], s[R_VIDEO_SUBMITS]));
	o.fmt(" | h", 0);
	o.hist("uiv", s + H_UNIQ_IV);
	o.hist("piv", s + H_PUB_IV);
	o.hist("skew", s + H_SKEW);
	o.hist("dec", s + H_DEC);
	o.hist("conv", s + H_CONV);
	o.hist("sync", s + H_SYNC);
	o.hist("copy", s + H_COPY);
	o.hist("sub", s + H_SUBMIT);
	o.hist("beg", s + H_BEGIN);
	o.fmt(" | ns", 0);
	o.kv("wall_ms", iv.nonsteady_wall_us / 1000);
	for (int b = 0; b < 20; b++) {
		if (iv.reason_windows[b]) {
			o.kv(reason_name(b), iv.reason_windows[b]);
		}
	}
	o.fmt(" | refused=%llu", refused); // earlier lines the ring could not take (log writer behind)
	// fmt=2: audio health (ECO_AUDIO_DIAGNOSIS.md), appended so the fmt=1 part keeps its layout
	o.fmt(" | aud rate=%llu/%llu", iv.a_src_rate, iv.a_speaker_rate);
	o.kv("ch", iv.a_channels);
	o.kv("frames", s[A_DEC]);
	o.kv("err", s[A_ERR]);
	o.kv("add", s[A_QUEUED]);
	o.kv("smp", s[A_SAMPLES]);
	o.kv("full", s[A_FULL]);
	o.kv("adderr", s[A_ADD_ERR]);
	o.kv("dry", s[A_DRY]);
	o.kv("clip", s[A_FS]);
	o.kv("dspf", s[DSP_FRAMES]);
	o.kv("drop", s[DSP_DROPPED]);
	o.kv("adec_us", avg(s[A_DEC_US], s[A_DEC]));
	o.fmt(" | audh", 0);
	o.hist("lead", s + H_LEAD);
	o.hist("adec", s + H_ADEC);
	o.fmt(" | audns dry=%llu drop=%llu", iv.ns_dry, iv.ns_dsp_dropped);
	return o.p - buf;
}

void Analyzer::emit_interval() {
	if (iv.windows > 0 && iv.playing_windows > 0) {
		char line[LINE_CAP];
		size_t n = format_interval(line, sizeof(line) - 1);
		line[n++] = '\n';
		line_no++;
		if (sink && sink(line, n, sink_ctx)) {
			emitted++;
		} else {
			refused++;
		}
	}
	iv = Interval();
}

void Analyzer::add_to_interval(const Counts &d, uint32_t reasons, uint32_t wall) {
	iv.windows++;
	if (!(win_flags & F_NOT_PLAYING)) {
		iv.playing_windows++;
	}
	iv.flags_or |= win_flags;
	iv.scene = win_scene;
	if (reasons == 0) {
		iv.steady_windows++;
		iv.steady_wall_us += wall;
		iv.steady.add(d);
		if (d.v[CONV_PUB] == 0) {
			iv.stall_windows++;
		}
	} else {
		iv.nonsteady_wall_us += wall;
		iv.ns_dry += d.v[A_DRY];
		iv.ns_dsp_dropped += d.v[DSP_DROPPED];
		for (int b = 0; b < 20; b++) {
			if (reasons & (1u << b)) {
				iv.reason_windows[b]++;
			}
		}
	}
	if (!(reasons & R_GEN)) {
		capture_meta();
	}
}

void Analyzer::close_window(uint32_t now_us) {
	Counts now, d;
	take_snapshot(p, &now);
	delta(snap, now, &d);
	snap = now;
	uint32_t wall = now_us - win_start_us;
	uint32_t gen = p.session.generation.get();
	uint32_t reasons = win_flags & DISQUALIFY_MASK;
	if (wall > LONG_WINDOW_US) {
		reasons |= R_LONG;
	}
	if (gen != win_start_gen) {
		reasons |= R_GEN;
	}
	if (!(reasons & (F_NOT_PLAYING | F_AUDIO_ONLY)) && p.session.nominal_mfps.get() == 0) {
		reasons |= R_NO_SRC;
	}
	bool clean = reasons == 0;
	if (clean && !prev_clean) {
		reasons |= R_SETTLE;
	}
	prev_clean = clean;

	uint32_t ps = p.session.play_starts.get();
	if (ps != sess_play_starts) {
		sess_play_starts = ps;
		sess_steady.clear();
		sess_steady_windows = 0;
		sess_steady_wall_us = 0;
		sess_stall_windows = 0;
	}
	if (reasons == 0) {
		sess_steady.add(d);
		sess_steady_windows++;
		sess_steady_wall_us += wall;
		if (d.v[CONV_PUB] == 0) {
			sess_stall_windows++;
		}
	}
	last_d = d;
	last_r = reasons;
	last_wall = wall;
	windows_closed++;

	Key k;
	k.gen = gen;
	k.meta = win_flags & KEY_META_MASK;
	k.scene = win_scene;
	if (iv.windows > 0 && k != iv.key) {
		bool gen_split = k.gen != iv.key.gen && iv.steady_windows > 0;
		if (gen_split || iv.windows >= INTERVAL_MIN_WINDOWS) {
			emit_interval();
		} else {
			iv.mixed = true;
		}
	}
	iv.key = k;
	add_to_interval(d, reasons, wall);
	if (iv.windows >= INTERVAL_MAX_WINDOWS) {
		emit_interval();
	}
	win_start_us = now_us;
	win_start_gen = gen;
	win_flags = 0;
	update_summary();
}

void Analyzer::tick(const StateSample &s) {
	if (!started) {
		started = true;
		take_snapshot(p, &snap);
		win_start_us = s.now_us;
		win_start_gen = p.session.generation.get();
		sess_play_starts = p.session.play_starts.get();
		win_flags = 0;
		update_summary();
	}
	win_flags |= s.flags;
	win_scene = s.scene ? s.scene : "?";
	if ((uint32_t)(s.now_us - win_start_us) >= WINDOW_US) {
		close_window(s.now_us);
	}
}

void Analyzer::flush() {
	if (iv.windows > 0) {
		emit_interval();
	}
}

// x10 fixed point "12.3", "?" when the denominator is 0
static void put_tenths(Out &o, uint64_t num, uint64_t den) {
	if (!den) {
		o.fmt("?", 0);
		return;
	}
	uint64_t t = (num * 10 + den / 2) / den;
	o.fmt("%llu.%llu", t / 10, t % 10);
}

void Analyzer::update_summary() {
	uint32_t mfps = p.session.nominal_mfps.get();
	uint32_t tempo = p.conv.tempo_milli.get();
	const Counts &d = last_d;
	{
		Out o(ui[0], SUMMARY_CAP);
		o.fmt("FPS diag src ", 0);
		if (mfps) {
			o.fmt("%llu.%02llu", mfps / 1000, (mfps % 1000) / 10);
		} else {
			o.fmt("?", 0);
		}
		if (tempo) {
			o.fmt(" x%llu.%02llu", tempo / 1000, (tempo % 1000) / 10);
		} else {
			o.fmt(" x?", 0);
		}
		if (windows_closed) {
			o.fmt(" | 1s dec %llu pub %llu", d.v[DEC_FRAMES], d.v[CONV_PUB]);
		} else {
			o.fmt(" | no window yet", 0);
		}
	}
	{
		Out o(ui[1], SUMMARY_CAP);
		if (!windows_closed) {
			o.fmt("1s ?", 0);
		} else {
			o.fmt("1s shown %llu rep %llu", d.v[R_UNIQUE], d.v[R_REPEATS]);
			o.fmt(" miss %llu hitch %llu |", d.v[R_MISSED], d.v[R_HITCHES]);
			if (last_r == 0) {
				o.fmt(" STEADY", 0);
			}
			bool first = true;
			for (int b = 0; b < 20; b++) {
				if (last_r & (1u << b)) {
					o.fmt(first ? " " : ",", 0);
					o.lit(reason_name(b));
					first = false;
				}
			}
		}
	}
	{
		Out o(ui[2], SUMMARY_CAP);
		const uint64_t *s = sess_steady.v;
		if (sess_steady_windows == 0) {
			o.fmt("steady: none yet (2 clean 1s windows needed)", 0);
		} else {
			uint64_t wall_ms = sess_steady_wall_us / 1000;
			o.fmt("steady %llus shown ", sess_steady_wall_us / 1000000);
			put_tenths(o, s[R_UNIQUE] * 1000, wall_ms);
			o.fmt("/s pub ", 0);
			put_tenths(o, s[CONV_PUB] * 1000, wall_ms);
			o.fmt("/s miss %llu hitch %llu", s[R_MISSED], s[R_HITCHES]);
			o.fmt(" stall %llu", sess_stall_windows);
		}
	}
	{
		Out o(ui[3], SUMMARY_CAP);
		o.fmt("ms dec ", 0);
		put_tenths(o, d.v[DEC_US], (uint64_t)d.v[DEC_FRAMES] * 1000);
		o.fmt(" conv ", 0);
		put_tenths(o, d.v[CONV_US], (uint64_t)d.v[CONV_GOT] * 1000);
		o.fmt(" sync ", 0);
		put_tenths(o, d.v[CONV_SYNC_US], (uint64_t)d.v[CONV_GOT] * 1000);
		o.fmt(" sub ", 0);
		put_tenths(o, d.v[R_SUBMIT_US], (uint64_t)d.v[R_VIDEO_SUBMITS] * 1000);
		uint32_t st = p.log.state.load(std::memory_order_relaxed);
		o.fmt(" | log ", 0);
		o.lit(st == LOG_OK ? "ok" : st == LOG_FULL ? "full" : st == LOG_FAILED ? "fail" : "-");
		o.fmt(" %llu", p.log.lines.get());
	}
}

// ---------------------------------------------------------------------------------------------------------------
LogBudget::Verdict LogBudget::admit(size_t len) {
	if (stopped()) {
		drops++;
		return Verdict::DROP;
	}
	if ((uint64_t)bytes + len + pol.marker_reserve > pol.run_budget) {
		exhausted = true;
		drops++;
		return Verdict::MARKER_THEN_STOP;
	}
	bytes += (uint32_t)len;
	lines++;
	return Verdict::WRITE;
}
void LogBudget::on_write_result(bool ok) { failures = ok ? 0 : failures + 1; }
size_t LogBudget::format_marker(char *buf, size_t cap, uint32_t lines, uint32_t bytes) {
	Out o(buf, cap);
	o.fmt("# fps diag run budget reached after %llu lines / %llu bytes; later lines of this run are dropped\n", lines,
	      bytes);
	return o.p - buf;
}

} // namespace fps_diag
#endif
