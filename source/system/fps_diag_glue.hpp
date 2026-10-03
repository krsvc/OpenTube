#pragma once
// DIAGNOSTIC ONLY (FPS_MEASUREMENT_PLAN.md): 3DS glue for util/fps_diag.hpp. Hooks take timestamps and forward local
// values to the single-writer probes; they never block, allocate, format or touch the SD card on the frame path.
// Without DEF_FPS_DIAG (make diag DIAG_FPS=0, or a normal build) every hook is an empty inline function, so the same
// tree builds without instrumentation for an overhead baseline.
#include <cstdint>

namespace fps_hooks {
#ifdef DEF_FPS_DIAG
// decode thread
void dec_play_start();
void dec_generation_bump();
void dec_session_meta(double framerate, int width, int height, int decoder_type, int quality, bool audio_only,
                      bool live, const char *client);
void dec_backpressure_wait();
void dec_video_decoded(bool frame_queued, double decode_ms);
void aud_session(int src_rate, int channels, int speaker_rate);
void aud_decoded(uint32_t result_code, double decode_ms);
void aud_add_begin(const unsigned char *pcm, int size, int channels);
void aud_add_retry(uint32_t result_code);
void aud_added(int size);
// convert thread
void conv_get_begin();
void conv_get_retry(bool paused);
void conv_frame_got();
void conv_converted(double convert_ms);
void conv_sync_begin();
void conv_sync_end(double pts, double audio_pos);
void conv_published(double pts, double tempo);
void conv_skipped_draw();
// main thread
void render_video_drawn();
void frame_begin_enter();
void frame_begin_exit();
void submit_enter();
void submit_exit();
void debug_view_drawn();
typedef const char *(*WaitingFn)();
void main_tick(uint32_t flags, const char *scene, WaitingFn decoder_waiting);
const char *summary_line(int i);
void exit_flush();
void log_init();
// log writer (freeze watchdog thread)
void log_drain();
#else
inline void dec_play_start() {}
inline void dec_generation_bump() {}
inline void dec_backpressure_wait() {}
inline void dec_video_decoded(bool, double) {}
inline void aud_session(int, int, int) {}
inline void aud_decoded(uint32_t, double) {}
inline void aud_add_begin(const unsigned char *, int, int) {}
inline void aud_add_retry(uint32_t) {}
inline void aud_added(int) {}
inline void conv_get_begin() {}
inline void conv_get_retry(bool) {}
inline void conv_frame_got() {}
inline void conv_converted(double) {}
inline void conv_sync_begin() {}
inline void conv_sync_end(double, double) {}
inline void conv_published(double, double) {}
inline void conv_skipped_draw() {}
inline void render_video_drawn() {}
inline void frame_begin_enter() {}
inline void frame_begin_exit() {}
inline void submit_enter() {}
inline void submit_exit() {}
inline void debug_view_drawn() {}
#endif
} // namespace fps_hooks
