#pragma once
#include "types.hpp"

void VideoPlayer_init(void);
void VideoPlayer_exit(void);
void VideoPlayer_suspend(void);
void VideoPlayer_resume(std::string arg);

// OpenTube r13: browse scenes show the mini-player card above the nav (ui/shell.hpp content_bottom()); this is the
// height it takes from the content
#define VIDEO_PLAYING_BAR_HEIGHT 46
bool video_is_playing(void);
void video_draw_playing_bar();
void video_update_playing_bar(Hid_info key);
// r13b: the shell took the input (a sheet opened): drop a pending mini-player / scrubber touch without its action
void video_cancel_playing_bar_gesture();

void video_draw_top_screen();
#ifdef DEF_FPS_DIAG
void video_fps_diag_tick(); // diagnostic only: frame-pacing state sample, once per main loop
#endif

void video_set_should_suspend_decoding(bool should_suspend_decoding);
// SD downloads pause while this is true (a video is loaded in the player, playing or paused)
bool video_playback_active();
// stops playback like X+B (the decode thread closes the streams); lets a paused download continue
void video_stop_playback();

void video_set_linear_filter_enabled(bool enabled);
void video_set_show_debug_info(bool show);
void video_set_skip_drawing(bool skip);

bool VideoPlayer_query_running_flag(void);

void VideoPlayer_draw(void);
