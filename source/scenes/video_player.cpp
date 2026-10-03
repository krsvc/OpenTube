#include "headers.hpp"
#include <vector>
#include <numeric>
#include <math.h>
#include <regex>

#include "scenes/video_player.hpp"
#include "scenes/home.hpp"
#include "ui/overlay.hpp"
#include "ui/ui.hpp"
#include "network_decoder/network_io.hpp"
#include "network_decoder/network_decoder_multiple.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "util/async_task.hpp"
#include "util/misc_tasks.hpp"
#include "util/util.hpp"
#include "util/freeze_diag.hpp"
#include "util/activity_gate.hpp"
#include "system/fps_diag_glue.hpp"
#ifdef DEF_FPS_DIAG
#include "util/fps_diag.hpp"
#endif
#include "data_io/subscription_util.hpp"
#include "downloads/downloads_app.hpp"
#include "data_io/liked_videos_app.hpp"

#define ICON_SIZE 55
#define TAB_SELECTOR_HEIGHT 20
#define TAB_SELECTOR_SELECTED_LINE_HEIGHT 3
#define SUGGESTIONS_VERTICAL_INTERVAL (VIDEO_LIST_THUMBNAIL_HEIGHT + SMALL_MARGIN)
#define SUGGESTION_LOAD_MORE_MARGIN 30
#define COMMENT_LOAD_MORE_MARGIN 30
#define CONTROL_BUTTON_HEIGHT 20
#define EQUALIZER_POPUP_WIDTH 280
#define EQUALIZER_POPUP_HEIGHT 200

#define MAX_THUMBNAIL_LOAD_REQUEST 12
#define MAX_COMMENT_ICON_LOAD_REQUEST 18
#define MAX_RETRY_CNT 2 // 3 trials as a total

#define TAB_GENERAL 0
#define TAB_COMMENTS 1
#define TAB_SUGGESTIONS 2
#define TAB_CAPTIONS 3
#define TAB_PLAYBACK 4
#define TAB_PLAYLIST 5

#define TAB_MAX_NUM 6

namespace VideoPlayer {
bool vid_main_run = false;
bool vid_thread_run = false;
bool vid_already_init = false;
bool vid_thread_suspend = true;
volatile bool should_suspend_decoding = false;
volatile bool vid_play_request = false;
volatile bool vid_seek_request = false;
volatile bool vid_change_video_request = false;
volatile bool vid_pausing = false;
volatile bool vid_pausing_seek = false;
volatile bool eof_reached = false;
volatile bool audio_only_mode = false;
volatile bool video_skip_drawing = false; // for performance reason, enabled when opening keyboard
volatile int video_p_value = 360;
volatile double seek_at_init_request = -1;
double vid_time[2][320];
double vid_copy_time[2] = {
    0,
    0,
};
double vid_audio_time = 0;
double vid_video_time = 0;
double vid_convert_time = 0;
double vid_frametime = 0;
double vid_framerate = 0;
int vid_sample_rate = 0;
double vid_duration = 0;
double vid_zoom = 1;
double vid_x = 0;
double vid_y = 15;
double vid_current_pos = 0;
volatile double vid_seek_pos = 0;
double vid_min_time = 0;
double vid_max_time = 0;
double vid_total_time = 0;
double vid_recent_time[90];
double vid_recent_total_time = 0;
int vid_total_frames = 0;
int vid_width = 0;
int vid_width_org = 0;
int vid_height = 0;
int vid_height_org = 0;
int vid_tex_width[2] = {0, 0};
int vid_tex_height[2] = {0, 0};
int texture_index_head = 0; // write the texture alternately to avoid scattering
std::string cur_displaying_url = "";
std::string cur_playing_url = "";
std::string vid_video_format = "n/a";
std::string vid_audio_format = "n/a";
Image_data vid_image[2];
int icon_thumbnail_handle = -1;
C2D_Image vid_banner[2];
C2D_Image vid_control[2];
C2D_Image play_button_texture[2];
Thread vid_decode_thread, vid_convert_thread;

// OpenTube r13: every tab body opens in a bottom sheet at y 30 (36 px header): this is the sheet's content height
constexpr int CONTENT_Y_HIGH = 240 - 30 - 36;

Thread stream_downloader_thread;
NetworkStreamDownloader stream_downloader;

Thread livestream_initer_thread;
NetworkMultipleDecoder network_decoder;
// same Mutex, plus diagnostic-only breadcrumbs (util/freeze_diag.hpp): who holds it, who waits for it since when
class TracedMutex {
	Mutex m;
	const freeze_diag::LockId id;
	const char *const name;

  public:
	TracedMutex(freeze_diag::LockId id, const char *name) : id(id), name(name) {}
	void lock() {
		freeze_diag::wait_begin(name);
		m.lock();
		freeze_diag::wait_end();
		freeze_diag::lock_acquired(id);
	}
	void unlock() {
		freeze_diag::lock_released(id);
		m.unlock();
	}
};
TracedMutex network_decoder_critical_lock(freeze_diag::DECODER_CRITICAL, "crit"); // locked when seeking or deiniting

TracedMutex small_resource_lock(freeze_diag::SMALL_RESOURCE, "small"); // locking basically all std::vector, std::string, etc
YouTubeVideoDetail cur_video_info;
YouTubeVideoDetail playing_video_info;
std::map<std::string, YouTubeVideoDetail> video_info_cache;
int video_retry_left = 0;

// --- playback diagnostics / bounded recovery (network_decoder/playback_diag.hpp) -----------------
// Log file: DEF_PLAYBACK_DIAG_LOG_PATH (/3ds/FourthTubeTest/playback_diag.log in the diag build),
// ring of 48 records, <= 400 bytes each, rewritten only from the decode thread at failure points.
struct DiagMutexLock : playback_diag::Lock {
	Mutex m;
	void lock() override { m.lock(); }
	void unlock() override { m.unlock(); }
};
DiagMutexLock playback_diag_lock;
static bool playback_diag_writer(const std::string &path, const std::string &data, void *) {
	return Path(path).write_file((const u8 *)data.data(), data.size()).code == 0;
}
static uint64_t playback_diag_clock(void *) { return osGetTime(); }
static playback_diag::DiagLog::Config playback_diag_config() {
	playback_diag::DiagLog::Config cfg;
	cfg.path = DEF_PLAYBACK_DIAG_LOG_PATH;
	cfg.build_id = DEF_DIAG_BUILD_ID;
	cfg.app_version = DEF_CURRENT_APP_VER;
	return cfg;
}
playback_diag::DiagLog playback_diag_log(playback_diag_lock, playback_diag_config(), playback_diag_writer, NULL,
                                         playback_diag_clock, NULL);
// runtime recovery budget (2 attempts per video) + generation fence; mutated under small_resource_lock
playback_diag::RecoveryController playback_recovery;
static const char *const RECOVERY_STATUS_STR = "Recovering playback";
// ownership of async video-page loads (review blocker #1); guarded by small_resource_lock
playback_diag::RequestRegistry load_requests;
// adds the player-side context (position, quality, video id) and stores the record; no file IO
static void playback_diag_record(playback_diag::FailureRecord rec, int attempt, double pos,
                                 const std::string &video_id) {
	rec.playback_pos = pos;
	rec.quality_p = audio_only_mode ? 0 : (int)video_p_value;
	rec.attempt = attempt;
	rec.video_id = video_id;
	playback_diag_log.record(rec);
}

// --- offline playback of SD downloads (DOWNLOADS_OFFLINE_BRIEF.md); all guarded by small_resource_lock -----------
// playing_local describes playing_video_info when it is an SD download. Lease ownership (download manager):
//   local_published_lease : taken when the item was published to the player (load_video_page)
//   local_session_lease   : the item the decode thread has open; released by the decode thread after deinit
// Delete is refused while either lease exists, so no file is removed under the decoder.
struct LocalPlayback {
	bool active = false;
	downloads::Playable playable;
};
LocalPlayback playing_local;
unsigned local_published_lease = 0;
unsigned local_session_lease = 0;
// detail screen: outcome of the last Download press (main thread only)
std::string download_action_video_id;
std::string download_action_status;

std::set<PostView *> comment_thumbnail_loaded_list;

std::string channel_id_pressed;
std::string suggestion_clicked_url; // also used for playlist

TabView *main_view = NULL;

// main tab
ScrollView *main_tab_view = (new ScrollView(0, 0, 320, CONTENT_Y_HIGH));
ImageView *main_icon_view = (new ImageView(0, 0, ICON_SIZE, ICON_SIZE));

// suggestion tab
VerticalListView *suggestion_main_view =
    (new VerticalListView(0, 0, 320))
        ->set_margin(SMALL_MARGIN)
        ->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::VIDEO_PLAYER);
View *suggestion_bottom_view = new EmptyView(0, 0, 320, 0);
ScrollView *suggestion_tab_view;

// comment tab
View *comments_top_view = new EmptyView(0, 0, 320, 4);
VerticalListView *comments_main_view = new VerticalListView(0, 0, 320);
View *comments_bottom_view = new EmptyView(0, 0, 320, 0);
ScrollView *comment_tab_view = NULL;

// caption tab
ScrollView *caption_main_view;
VerticalListView *caption_language_select_view;
ViewReferenceView *captions_tab_view = new ViewReferenceView();
CaptionOverlayView *caption_overlay_view;

// list tab
constexpr int PLAYLIST_TOP_HEIGHT = 30;
VerticalListView *playlist_tab_view;
ScrollView *playlist_list_view;
VerticalListView *playlist_list_videos_view;
TextView *playlist_title_view;
TextView *playlist_author_view;

// playback tab
SuccinctVideoView *cur_playing_video_view;
CustomView *download_progress_view = NULL;
SelectorView *video_quality_selector_view;
SelectorView *video_loop_view;
VerticalListView *debug_info_view = NULL;
ScrollView *playback_tab_view = NULL;

OverlayView *equalizer_popup_view = (new OverlayView(0, 0, 320, 240));
}; // namespace VideoPlayer
using namespace VideoPlayer;

static const char *volatile network_waiting_status = NULL;
const char *get_network_waiting_status() {
	if (network_waiting_status) {
		return network_waiting_status;
	}
	return network_decoder.get_network_waiting_status();
}

void video_set_should_suspend_decoding(bool should_suspend) { should_suspend_decoding = should_suspend; }

bool video_playback_active() { return vid_play_request || vid_change_video_request; }
void video_stop_playback() { vid_play_request = false; } // same as X+B: the decode thread stops and closes the streams

static bool is_local_video_url(const std::string &url) { return downloads::parse_local_url(url, NULL); }
static bool video_info_playable(const YouTubeVideoDetail &info) {
	return is_local_video_url(info.url) ? info.playability_status == "OK" : info.is_playable();
}
// Offline copy: built from the download library in memory only. No page, thumbnail, caption, comment, suggestion,
// token or signed-url request is made for it, and it never touches the SD card here.
static YouTubeVideoDetail local_video_detail(const std::string &url, downloads::ItemView *item_out) {
	YouTubeVideoDetail res;
	res.url = url;
	res.duration_ms = 0;
	res.is_livestream = false;
	res.livestream_type = YouTubeVideoDetail::LivestreamType::PREMIERE;
	res.is_upcoming = false;
	res.stream_fragment_len = -1;
	res.comment_continue_type = -1;
	res.comments_disabled = false;
	res.playlist.total_videos = 0;
	res.playlist.selected_index = -1;
	std::string id;
	downloads::ItemView item;
	if (downloads::parse_local_url(url, &id) && downloads_running() && downloads_manager().find(id, &item) &&
	    item.state == downloads::ItemState::READY) {
		res.id = item.id; // distinct from the online video id: an online copy playing never masks the local one
		res.title = item.meta.title.size() ? item.meta.title : "(untitled)";
		res.duration_ms = (int)std::min<int64_t>(item.meta.duration_ms, std::numeric_limits<int>::max());
		res.playability_status = "OK";
		res.error = "Offline copy: not available without a connection";
		*item_out = item;
	} else {
		res.playability_status = "OFFLINE_UNAVAILABLE";
		res.playability_reason = "This download is not available.\nIt was deleted, is unfinished or damaged.\n"
		                         "Open Downloads to retry or delete it.";
	}
	return res;
}

static void send_seek_request_wo_lock(double pos);
static void send_change_video_request(std::string url, bool update_player, bool update_view, bool force_load);
// recovery_generation != 0 marks a recovery-originated player reload: its publication is additionally
// fenced by the recovery generation (any later user action makes it stale)
static void send_change_video_request_wo_lock(std::string url, bool update_player, bool update_view, bool force_load,
                                              unsigned recovery_generation = 0);

static void load_video_page(void *);
static void load_more_comments(void *);
static void load_more_suggestions(void *);
static void load_more_replies(void *);
static void load_caption(void *);

static void decode_thread(void *arg);
static void convert_thread(void *arg);

void VideoPlayer_init(void) {
	logger.info(DEF_SAPP0_INIT_STR, "Initializing...");
	bool new_3ds = false;
	Result_with_string result;

	vid_thread_run = true;

	suggestion_tab_view =
	    (new ScrollView(0, 0, 320, CONTENT_Y_HIGH))->set_views({suggestion_main_view, suggestion_bottom_view});
	comment_tab_view = (new ScrollView(0, 0, 320, CONTENT_Y_HIGH))
	                       ->set_views({comments_top_view, comments_main_view, comments_bottom_view});

	caption_language_select_view = new VerticalListView(0, 0, 320);
	captions_tab_view->set_subview(caption_language_select_view);
	caption_main_view = new ScrollView(0, 0, 320, CONTENT_Y_HIGH);
	caption_overlay_view = new CaptionOverlayView(0, 0, 400, 240);
	cur_playing_video_view =
	    (new SuccinctVideoView(SMALL_MARGIN, 0, 320 - SMALL_MARGIN * 2, VIDEO_LIST_THUMBNAIL_HEIGHT));
	download_progress_view = (new CustomView(0, 0, 320, SMALL_MARGIN * 2))->set_draw([](const CustomView &view) {
		int y = view.y0;
		int sample = 50;
		auto progress_bars = network_decoder.get_buffering_progress_bars(sample);
		for (size_t i = 0; i < 2; i++) {
			if (i < progress_bars.size() && progress_bars[i].second.size()) {
				auto &cur_bar = progress_bars[i].second;
				auto cur_pos = progress_bars[i].first;

				int sample = 50;
				int bar_len = 310;
				for (int i = 0; i < sample; i++) {
					int xl = 5 + bar_len * i / sample;
					int xr = 5 + bar_len * (i + 1) / sample;
					Draw_texture(var_square_image[0], View::blend_color(POCKET_SEPARATOR, POCKET_ACCENT, cur_bar[i] / 100),
					             xl, y, xr - xl, SMALL_MARGIN);
				}
				float head_x = 5 + bar_len * cur_pos;
				Draw_texture(var_square_image[0], DEFAULT_TEXT_COLOR, head_x - 1, y, SMALL_MARGIN, SMALL_MARGIN);
			}
			y += SMALL_MARGIN;
		}
	});
	video_quality_selector_view = (new SelectorView(0, 0, 320, 35, false))
	                                  ->set_texts({(std::function<std::string()>)[]() { return LOCALIZED(OFF);
}
}, 0)
	                                  ->set_title([](const SelectorView &view) {
	return LOCALIZED(VIDEO); });
debug_info_view =
    (new VerticalListView(0, 0, 320))
        ->set_views({(new TextView(SMALL_MARGIN, 0, 320, DEFAULT_FONT_INTERVAL * 8))
                         ->set_text_lines<std::function<std::string()>>(
                             {[]() { return vid_video_format; }, []() { return vid_audio_format; },
                              []() {
	                              return std::to_string(vid_width_org) + "x" + std::to_string(vid_height_org) + "@" +
	                                     std::to_string(vid_framerate).substr(0, 5) + "FPS";
                              },
                              []() {
	                              auto decoder_type = network_decoder.get_decoder_type();
	                              std::string decoder_type_str =
	                                  decoder_type == NetworkMultipleDecoder::DecoderType::NA   ? "N/A"
	                                  : decoder_type == NetworkMultipleDecoder::DecoderType::HW ? LOCALIZED(HW_DECODER)
	                                  : decoder_type == NetworkMultipleDecoder::DecoderType::MT_SLICE
	                                      ? LOCALIZED(MULTITHREAD_SLICE)
	                                  : decoder_type == NetworkMultipleDecoder::DecoderType::MT_FRAME
	                                      ? LOCALIZED(MULTITHREAD_FRAME)
	                                      : LOCALIZED(SINGLE_THREAD);
	                              return LOCALIZED(DECODER_TYPE) + " : " + decoder_type_str;
                              },
                              []() {
	                              const char *message = get_network_waiting_status();
	                              return LOCALIZED(WAITING_STATUS) + " : " + std::string(message ? message : "");
                              },
                              []() {
	                              u32 cpu_limit;
	                              APT_GetAppCpuTimeLimit(&cpu_limit);
	                              return LOCALIZED(CPU_LIMIT) + " : " + std::to_string(cpu_limit) + "%";
                              },
                              []() {
	                              return LOCALIZED(FORWARD_BUFFER) + " : " +
	                                     (cur_video_info.is_livestream && network_decoder.ready
	                                          ? std::to_string(network_decoder.get_forward_buffer())
	                                          : "N/A");
                              },
                              []() {
	                              return LOCALIZED(RAW_FRAME_BUFFER) + " : " +
	                                     std::to_string(network_decoder.get_raw_buffer_num()) + "/" +
	                                     std::to_string(network_decoder.get_raw_buffer_num_max());
                              }}),
                     (new RuleView(0, 0, 320, SMALL_MARGIN * 2)),
                     (new CustomView(0, 0, 320, 160))->set_draw([](const CustomView &view) {
	                     int y = view.y0;
	                     fps_hooks::debug_view_drawn(); // tags the frame-pacing window: debug view cost included

	                     // decoding time graph
	                     for (int i = 0; i < 319; i++) {
		                     Draw_line(i, y + 90 - vid_time[1][i], DEF_DRAW_BLUE, i + 1, y + 90 - vid_time[1][i + 1],
		                               DEF_DRAW_BLUE, 1); // Thread 1
		                     Draw_line(i, y + 90 - vid_time[0][i], DEF_DRAW_RED, i + 1, y + 90 - vid_time[0][i + 1],
		                               DEF_DRAW_RED, 1); // Thread 0
	                     }

	                     Draw_line(0, y + 90, DEFAULT_TEXT_COLOR, 320, y + 90, DEFAULT_TEXT_COLOR, 2);
	                     Draw_line(0, y + 90 - vid_frametime, 0xFFFFFF00, 320, y + 90 - vid_frametime, 0xFFFFFF00, 2);
	                     if (vid_total_frames != 0 && vid_min_time != 0 && vid_recent_total_time != 0) {
		                     Draw("Avg: " + std::to_string(1000 / (vid_total_time / vid_total_frames)).substr(0, 5) +
		                              " Min: " + std::to_string(1000 / vid_max_time).substr(0, 5) +
		                              " Max: " + std::to_string(1000 / vid_min_time).substr(0, 5) + " Recent Avg: " +
		                              std::to_string(1000 / (vid_recent_total_time / 90)).substr(0, 5) + " FPS",
		                          0, y + 90, 0.4, 0.4, DEFAULT_TEXT_COLOR);
	                     }

	                     Draw("Deadline : " + std::to_string(vid_frametime).substr(0, 5) + "ms", 0, y + 100, 0.4, 0.4,
	                          0xFFFFFF00);
	                     Draw("Video decode : " + std::to_string(vid_video_time).substr(0, 5) + "ms", 0, y + 110, 0.4,
	                          0.4, DEF_DRAW_RED);
	                     Draw("Audio decode : " + std::to_string(vid_audio_time).substr(0, 5) + "ms", 0, y + 120, 0.4,
	                          0.4, DEF_DRAW_RED);
	                     // Draw("Data copy 0 : " + std::to_string(vid_copy_time[0]).substr(0, 5) + "ms", 160, 120, 0.4,
	                     // 0.4, DEF_DRAW_BLUE);
	                     Draw("Color convert : " + std::to_string(vid_convert_time).substr(0, 5) + "ms", 160, y + 110,
	                          0.4, 0.4, DEF_DRAW_BLUE);
	                     Draw("Data copy 1 : " + std::to_string(vid_copy_time[1]).substr(0, 5) + "ms", 160, y + 120,
	                          0.4, 0.4, DEF_DRAW_BLUE);
	                     Draw("Thread 0 : " + std::to_string(vid_time[0][319]).substr(0, 6) + "ms", 0, y + 130, 0.5,
	                          0.5, DEF_DRAW_RED);
	                     Draw("Thread 1 : " + std::to_string(vid_time[1][319]).substr(0, 6) + "ms", 160, y + 130, 0.5,
	                          0.5, DEF_DRAW_BLUE);
	                     Draw("Zoom : x" + std::to_string(vid_zoom).substr(0, 5) +
	                              " X : " + std::to_string((int)vid_x) + " Y : " + std::to_string((int)vid_y),
	                          0, y + 140, 0.5, 0.5, DEFAULT_TEXT_COLOR);
                     })
#ifdef DEF_FPS_DIAG
                         , // frame-pacing summary (util/fps_diag.hpp), formatted once per second by the analyzer
                     (new TextView(0, 0, 320, 11 * 4))
                         ->set_font_size(0.4, 11)
                         ->set_text_lines<std::function<std::string()>>(
                             {[]() { return std::string(fps_hooks::summary_line(0)); },
                              []() { return std::string(fps_hooks::summary_line(1)); },
                              []() { return std::string(fps_hooks::summary_line(2)); },
                              []() { return std::string(fps_hooks::summary_line(3)); }})
#endif
                 });
    playback_tab_view =
	    (new ScrollView(0, 0, 320, CONTENT_Y_HIGH))
	        ->set_margin(SMALL_MARGIN)
	        ->set_views(
	            {(new EmptyView(0, 0, 320, 0)), // margin at the top(using ScrollView's auto margin)
	             (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))->set_text((std::function<std::string()>)[]() {
	return LOCALIZED(CUR_PLAYING_VIDEO);
	             }),
	             cur_playing_video_view,
	             (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))->set_text((std::function<std::string()>)[]() {
	return LOCALIZED(BUFFERING_PROGRESS);
	             }),
	             download_progress_view,
	             (new TextView(SMALL_MARGIN * 2, 0, 100, CONTROL_BUTTON_HEIGHT))
	                 ->set_text((std::function<std::string()>)[]() {
	return LOCALIZED(RELOAD); })
	                 ->set_x_alignment(TextView::XAlign::CENTER)
	                 ->set_get_background_color([](const View &view) {
	return is_async_task_running(load_video_page) ? POCKET_PRESSED : View::STANDARD_BUTTON_BACKGROUND(view);
	                 })
	                 ->set_on_view_released([](View &view) {
	if (!is_async_task_running(load_video_page)) {
		seek_at_init_request = vid_current_pos;
		playback_recovery.user_reset(); // manual reload: fresh runtime recovery budget
		send_change_video_request_wo_lock(cur_playing_url, true, false, true);
		video_retry_left = MAX_RETRY_CNT;
	}
	                 }),
	             video_quality_selector_view,
	             video_loop_view = (new SelectorView(0, 0, 320, 35, true))
	                                   ->set_texts({(std::function<std::string()>)[]() { return LOCALIZED(OFF); },
	                                                (std::function<std::string()>)[]() {
	return LOCALIZED(ON); }
    },
	                                               var_loop_mode)
	                                   ->set_title([](const SelectorView &) {
	return LOCALIZED(LOOP); })
	                                   ->set_on_change([](const SelectorView &view) {
	if (var_loop_mode != view.selected_button) {
		var_loop_mode = view.selected_button;
		misc_tasks_request(TASK_SAVE_SETTINGS);
	}
	                                   }),
	             (new BarView(0, 0, 260, 30))                     // preamp
	                 ->set_values(std::log(0.25), std::log(4), 0) // exponential scale
	                 ->set_title([](const BarView &view) {
	return LOCALIZED(PREAMP) + " : " + std::to_string((int)std::round(std::exp(view.get_value()) * 100)) + "%";
	                 })
	                 ->set_while_holding([](BarView &view) {
	double volume = std::exp(view.get_value());
	if (volume >= 0.95 && volume <= 1.05) {
		volume = 1.0, view.set_value(0);
	}
	// volume change needs a reconstruction of the filter graph, so don't update volume too often
	static int cnt = 0;
	if (++cnt >= 15) {
		cnt = 0, network_decoder.set_preamp(volume);
	}
	                 })
	                 ->set_on_release([](BarView &view) {
	double volume = std::exp(view.get_value());
	if (volume >= 0.95 && volume <= 1.05) {
		volume = 1.0, view.set_value(0);
	}
	network_decoder.set_preamp(volume);
	                 }),
	             (new BarView(0, 0, 260, 30)) // speed
	                 ->set_values(0.25, 1.5, 1.0)
	                 ->set_title([](const BarView &view) {
	return LOCALIZED(SPEED) + " : " + std::to_string((int)std::round(view.get_value() * 100)) + "%";
	                 })
	                 ->set_while_holding([](BarView &view) {
	view.fix_close_values(0.97, 1.0, 1.03);
	static int cnt = 0;
	if (++cnt >= 15) {
		cnt = 0, network_decoder.set_tempo(view.get_value());
	}
	                 })
	                 ->set_on_release([](BarView &view) {
	view.fix_close_values(0.97, 1.0, 1.03);
	network_decoder.set_tempo(view.get_value());
	                 }),
	             (new BarView(0, 0, 260, 30)) // pitch
	                 ->set_values(0.5, 2.0, 1.0)
	                 ->set_title([](const BarView &view) {
	return LOCALIZED(PITCH) + " : " + std::to_string((int)std::round(view.get_value() * 100)) + "%";
	                 })
	                 ->set_while_holding([](BarView &view) {
	view.fix_close_values(0.97, 1.0, 1.03);
	static int cnt = 0;
	if (++cnt >= 15) {
		cnt = 0, network_decoder.set_pitch(view.get_value());
	}
	                 })
	                 ->set_on_release([](BarView &view) {
	view.fix_close_values(0.97, 1.0, 1.03);
	network_decoder.set_pitch(view.get_value());
	                 }),
	             (new TextView(SMALL_MARGIN * 2, 0, 100, 20))
	                 ->set_text([]() {
	return LOCALIZED(OPEN_EQUALIZER); })
	                 ->set_x_alignment(TextView::XAlign::CENTER)
	                 ->set_text_offset(0, -1)
	                 ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	                 ->set_on_view_released([](View &view) {
	equalizer_popup_view->set_is_visible(true); }),
	             (new RuleView(0, 0, 320, SMALL_MARGIN)), debug_info_view
    });
    std::vector<View *> equalizer_views;
    constexpr int equalizer_hz_list[18] = {65,   92,   131,  185,  262,  370,  523,   740,   1047,
                                           1480, 2093, 2960, 4186, 5920, 8372, 11840, 16744, 20000};
    static double equalizer_values[18];
    static double equalizer_applied[18]; // what network_decoder was last given per band (its requests start at 1.0 too)
    std::fill(equalizer_values, equalizer_values + 18, 1.0);
    std::fill(equalizer_applied, equalizer_applied + 18, 1.0);
    // a cancelled band drag (B, backdrop, Close, a shell-taken frame) shows the value last applied again (r13d)
    struct EqualizerBarView : public BarView {
	    int band;
	    EqualizerBarView(int band) : View(0, 0), BarView(0, 0, 180, DEFAULT_FONT_INTERVAL), band(band) {}
	    void reset_holding_status_() override {
		    if (holding) {
			    set_value(equalizer_applied[band]);
			    var_need_refresh = true;
		    }
		    BarView::reset_holding_status_();
	    }
    };
    for (int i = 0; i < 18; i++) {
	    BarView *bar_view = (new EqualizerBarView(i))
	                            ->set_values_sync(0.0, 2.0, &equalizer_values[i])
	                            ->set_while_holding([i](BarView &view) {
		                            static int cnt = 0;
		                            if (++cnt >= 30) {
			                            cnt = 0;
			                            network_decoder.set_equalizer_value(i, view.get_value());
			                            equalizer_applied[i] = view.get_value();
		                            }
	                            })
	                            ->set_on_release([i](BarView &view) {
		                            logger.info("debug", std::to_string(i) + " : " + std::to_string(view.get_value()));
		                            network_decoder.set_equalizer_value(i, view.get_value());
		                            equalizer_applied[i] = view.get_value();
	                            });
	    equalizer_views.push_back((new HorizontalListView(0, 0, DEFAULT_FONT_INTERVAL))
	                                  ->set_views({(new TextView(0, 0, 60, DEFAULT_FONT_INTERVAL))
	                                                   ->set_text(std::to_string(equalizer_hz_list[i]) + " Hz"),
	                                               bar_view}));
    }
    equalizer_popup_view->set_subview(
        (new VerticalListView(0, 0, EQUALIZER_POPUP_WIDTH))
            ->set_views({(new TextView(0, 0, EQUALIZER_POPUP_WIDTH, 19))
                             ->set_text([]() { return LOCALIZED(RESET); })
                             ->set_x_alignment(TextView::XAlign::CENTER)
                             ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
                             ->set_on_view_released([](View &) {
	                             std::fill(equalizer_values, equalizer_values + 18, 1.0);
	                             for (int i = 0; i < 18; i++) {
		                             network_decoder.set_equalizer_value(i, 1);
		                             equalizer_applied[i] = 1;
	                             }
                             }),
                         (new RuleView(0, 0, EQUALIZER_POPUP_WIDTH, 1))->set_margin(0),
                         (new ScrollView(0, 0, EQUALIZER_POPUP_WIDTH, EQUALIZER_POPUP_HEIGHT - 40))
                             ->set_views(equalizer_views)
                             ->set_get_background_color([](const View &) { return POCKET_SURFACE; }),
                         (new TextView(0, 0, EQUALIZER_POPUP_WIDTH, 20))
                             ->set_text([]() { return LOCALIZED(CLOSE); })
                             ->set_x_alignment(TextView::XAlign::CENTER)
                             ->set_on_view_released([](View &) {
	                             equalizer_popup_view->set_is_visible(false);
	                             equalizer_popup_view->reset_holding_status();
	                             var_need_refresh = true;
                             })
                             ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)})
            ->set_draw_order({2, 0, 1, 3}));
    equalizer_popup_view->set_on_cancel([](OverlayView &view) { // backdrop tap, B (VideoPlayer_draw)
	    view.set_is_visible(false);
	    view.reset_holding_status(); // a held RESET / band must not fire when the stylus lifts later (r13c)
	    var_need_refresh = true;
    });
    equalizer_popup_view->set_is_visible(false);
    playlist_title_view =
        (new TextView(0, 0, 320, MIDDLE_FONT_INTERVAL))->set_font_size(MIDDLE_FONT_SIZE, MIDDLE_FONT_INTERVAL);
    playlist_author_view =
        (new TextView(0, 0, 320, PLAYLIST_TOP_HEIGHT - MIDDLE_FONT_INTERVAL))->set_get_text_color([]() {
	        return LIGHT0_TEXT_COLOR;
        });
    playlist_list_videos_view =
        (new VerticalListView(0, 0, 320))
            ->set_margin(SMALL_MARGIN)
            ->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::VIDEO_PLAYER);
    playlist_list_view =
        (new ScrollView(0, 0, 320, CONTENT_Y_HIGH - PLAYLIST_TOP_HEIGHT))->set_views({playlist_list_videos_view});
    playlist_tab_view = (new VerticalListView(0, 0, 320))
                            ->set_views({playlist_title_view, playlist_author_view,
                                         (new RuleView(0, 0, 320, SMALL_MARGIN)), playlist_list_view})
                            ->set_draw_order({3, 2, 1, 0});
    for (int i = 0; i < 3; i++) {
	    playlist_tab_view->views[i]->set_get_background_color([](const View &) { return DEFAULT_BACK_COLOR; });
    }

    // default : 5 tabs
    main_view =
        (new TabView(0, 240 - CONTENT_Y_HIGH, 320, CONTENT_Y_HIGH))
            ->set_style(TabView::Style::HIDDEN) // sections open as sheets (Deck)
            ->set_lr_tab_switch_enabled(false)
            ->set_stretch_subview(true)
            ->set_tab_font_size(0.4)
            ->set_views({main_tab_view, suggestion_tab_view, comment_tab_view, captions_tab_view, playback_tab_view},
                        TAB_GENERAL)
            ->set_tab_texts<std::function<std::string()>>(
                {[]() { return LOCALIZED(GENERAL); }, []() { return LOCALIZED(SUGGESTIONS); },
                 []() { return LOCALIZED(COMMENTS); }, []() { return LOCALIZED(CAPTIONS); },
                 []() { return LOCALIZED(PLAYBACK); }});

    add_cpu_limit(CPU_LIMIT);
    if (var_is_new3ds) {
	    bool frame_cores[4] = {false, true, var_core2_available, var_core3_available};
	    bool slice_cores[4] = {false, true, false, var_core3_available};
	    network_decoder.set_frame_cores_enabled(frame_cores);
	    network_decoder.set_slice_cores_enabled(slice_cores);

	    vid_decode_thread =
	        threadCreate(decode_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_HIGH, 2, false);
	    vid_convert_thread =
	        threadCreate(convert_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	    livestream_initer_thread = threadCreate(livestream_initer_thread_func, &network_decoder, DEF_STACKSIZE,
	                                            DEF_THREAD_PRIORITY_NORMAL, 2, false);
    } else {
	    bool frame_cores[4] = {true, true, false, false};
	    bool slice_cores[4] = {false, true, false, false};
	    network_decoder.set_frame_cores_enabled(frame_cores);
	    network_decoder.set_slice_cores_enabled(slice_cores);

	    vid_decode_thread =
	        threadCreate(decode_thread, (void *)("1"), DEF_STACKSIZE, DEF_THREAD_PRIORITY_HIGH, 1, false);
	    vid_convert_thread =
	        threadCreate(convert_thread, (void *)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_NORMAL, 0, false);
	    livestream_initer_thread = threadCreate(livestream_initer_thread_func, &network_decoder, DEF_STACKSIZE,
	                                            DEF_THREAD_PRIORITY_NORMAL, 1, false);
    }
    stream_downloader_thread = threadCreate(network_downloader_thread, &stream_downloader, DEF_STACKSIZE,
                                            DEF_THREAD_PRIORITY_NORMAL, 0, false);

    vid_total_time = 0;
    vid_total_frames = 0;
    vid_min_time = 99999999;
    vid_max_time = 0;
    vid_recent_total_time = 0;
    for (int i = 0; i < 90; i++) {
	    vid_recent_time[i] = 0;
    }

    for (int i = 0; i < 2; i++) {
	    vid_tex_width[i] = 0;
	    vid_tex_height[i] = 0;
    }

    for (int i = 0; i < 320; i++) {
	    vid_time[0][i] = 0;
	    vid_time[1][i] = 0;
    }

    vid_audio_time = 0;
    vid_video_time = 0;
    vid_copy_time[0] = 0;
    vid_copy_time[1] = 0;
    vid_convert_time = 0;

    for (int i = 0; i < 2; i++) {
	    result = Draw_c2d_image_init(&vid_image[i], 1024, 1024, GPU_RGB565);
	    if (result.code != 0) {
		    Util_err_set_error_message(DEF_ERR_OUT_OF_LINEAR_MEMORY_STR, "", DEF_SAPP0_INIT_STR,
		                               DEF_ERR_OUT_OF_LINEAR_MEMORY);
		    Util_err_set_error_show_flag(true);
		    vid_thread_run = false;
	    }
	    Draw_c2d_image_set_filter(&vid_image[i], var_video_linear_filter);
    }

    result = Draw_load_texture("romfs:/gfx/draw/video_player/banner.t3x", 61, vid_banner, 0, 2);
    if (result.code != 0) {
	    logger.error(DEF_SAPP0_INIT_STR, "Draw_load_texture()..." + result.string + result.error_description,
	                 result.code);
    }

    result = Draw_load_texture("romfs:/gfx/draw/video_player/control.t3x", 62, vid_control, 0, 2);
    if (result.code != 0) {
	    logger.error(DEF_SAPP0_INIT_STR, "Draw_load_texture()..." + result.string + result.error_description,
	                 result.code);
    }

    result = Draw_load_texture("romfs:/gfx/draw/thumb_up.t3x", 63, var_texture_thumb_up, 0, 2);
    if (result.code != 0) {
	    logger.error(DEF_SAPP0_INIT_STR, "Draw_load_texture()..." + result.string + result.error_description,
	                 result.code);
    }

    result = Draw_load_texture("romfs:/gfx/draw/thumb_down.t3x", 64, var_texture_thumb_down, 0, 2);
    if (result.code != 0) {
	    logger.error(DEF_SAPP0_INIT_STR, "Draw_load_texture()..." + result.string + result.error_description,
	                 result.code);
    }

    vid_x = 0;
    vid_y = 15;
    vid_frametime = 0;
    vid_framerate = 0;
    vid_current_pos = 0;
    vid_duration = 0;
    vid_zoom = 1;
    vid_width = 0;
    vid_height = 0;
    vid_video_format = "n/a";
    vid_audio_format = "n/a";
    video_p_value = var_is_new3ds ? 360 : 144;

    VideoPlayer_resume("");
    vid_already_init = true;

    video_set_show_debug_info(var_video_show_debug_info);
    video_set_linear_filter_enabled(var_video_linear_filter);
    logger.info(DEF_SAPP0_INIT_STR, "Initialized.");
    }
    void VideoPlayer_exit(void) {
	    logger.info(DEF_SAPP0_EXIT_STR, "Exiting...");
	    u64 time_out = 10000000000;
	    Result_with_string result;

	    vid_already_init = false;
	    vid_thread_suspend = false;
	    vid_thread_run = false;
	    vid_play_request = false;
	    playback_recovery.cancel(); // exit: no stale recovery may restart anything

	    stream_downloader.request_thread_exit();
	    network_decoder.interrupt = true;
	    network_decoder.request_thread_exit();
	    logger.info(DEF_SAPP0_EXIT_STR, "threadJoin()...", threadJoin(vid_decode_thread, time_out));
	    logger.info(DEF_SAPP0_EXIT_STR, "threadJoin()...", threadJoin(vid_convert_thread, time_out));
	    logger.info(DEF_SAPP0_EXIT_STR, "threadJoin()...", threadJoin(stream_downloader_thread, time_out));
	    logger.info(DEF_SAPP0_EXIT_STR, "threadJoin()...", threadJoin(livestream_initer_thread, time_out));
	    threadFree(vid_decode_thread);
	    threadFree(vid_convert_thread);
	    threadFree(stream_downloader_thread);
	    threadFree(livestream_initer_thread);
	    stream_downloader.delete_all();
	    if (playback_diag_log.size()) { // decode thread is joined: single final write
		    playback_diag_log.flush();
	    }

	    small_resource_lock.lock();
	    load_requests.cancel_all(); // any in-flight page load may no longer publish

	    // clean up views
	    suggestion_tab_view->recursive_delete_subviews();
	    delete suggestion_tab_view;
	    suggestion_tab_view = NULL;
	    suggestion_main_view = NULL;
	    suggestion_bottom_view = NULL;

	    comment_tab_view->recursive_delete_subviews();
	    delete comment_tab_view;
	    comments_top_view = NULL;
	    comments_main_view = NULL;
	    comments_bottom_view = NULL;
	    comment_tab_view = NULL;

	    delete caption_language_select_view;
	    delete caption_main_view;
	    delete caption_overlay_view;
	    caption_language_select_view = NULL;
	    caption_main_view = NULL;
	    caption_overlay_view = NULL;
	    captions_tab_view = NULL;

	    playback_tab_view->recursive_delete_subviews();
	    delete playback_tab_view;
	    playback_tab_view = NULL;
	    debug_info_view = NULL;
	    download_progress_view = NULL;

	    playlist_tab_view->recursive_delete_subviews();
	    delete playlist_tab_view;
	    playlist_tab_view = NULL;

	    small_resource_lock.unlock();

	    remove_cpu_limit(CPU_LIMIT);

	    Draw_free_texture(61);
	    Draw_free_texture(62);
	    Draw_free_texture(63);
	    Draw_free_texture(64);

	    for (int i = 0; i < 2; i++) {
		    Draw_c2d_image_free(vid_image[i]);
	    }

	    logger.info(DEF_SAPP0_EXIT_STR, "Exited.");
    }
    namespace Bar {
    extern bool fullscreen_override;
    }
    void VideoPlayer_suspend(void) {
	    vid_thread_suspend = true;
	    vid_main_run = false;
	    Bar::fullscreen_override = false; // r16: only the visit Y opened is full screen
    }
    namespace Deck {
    void close_section();
    }
    void VideoPlayer_resume(std::string arg) {
	    if (arg != "") {
		    if (arg != cur_displaying_url) {
			    send_change_video_request(arg, !vid_play_request || (vid_pausing && eof_reached), true, false);
			    video_retry_left = MAX_RETRY_CNT;
		    }
	    }
	    main_view->reset_holding_status();
	    // reset scroll
	    main_tab_view->reset();
	    suggestion_tab_view->reset();
	    comment_tab_view->reset();
	    caption_main_view->reset();
	    playlist_list_view->reset();

	    overlay_menu_on_resume();
	    Deck::close_section();
	    vid_thread_suspend = false;
	    vid_main_run = true;
	    var_need_refresh = true;
    }

    // START : functions called from async_task.cpp
    /* -------------------------------------------------------------------------------------------------------------- */
    /* -------------------------------------------------------------------------------------------------------------- */
    /* -------------------------------------------------------------------------------------------------------------- */

#define TITLE_MAX_WIDTH (320 - SMALL_MARGIN * 2)
#define DESC_MAX_WIDTH (320 - SMALL_MARGIN * 2)
#define SUGGESTION_TITLE_MAX_WIDTH (320 - (VIDEO_LIST_THUMBNAIL_WIDTH + SMALL_MARGIN))
#define COMMENT_MAX_WIDTH (320 - (POST_ICON_SIZE + 2 * SMALL_MARGIN))
#define REPLY_INDENT 25
#define REPLY_MAX_WIDTH (320 - REPLY_INDENT - (REPLY_ICON_SIZE + 2 * SMALL_MARGIN))
#define CAPTION_TIMESTAMP_WIDTH 60
#define CAPTION_CONTENT_MAX_WIDTH (320 - CAPTION_TIMESTAMP_WIDTH)

    static void update_suggestion_bottom_view() {
	    delete suggestion_bottom_view;
	    if (cur_video_info.error != "" || cur_video_info.has_more_suggestions() || !cur_video_info.suggestions.size()) {
		    TextView *bottom_view = (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 2))
		                                ->set_text(cur_video_info.error != ""              ? cur_video_info.error
		                                           : cur_video_info.has_more_suggestions() ? LOCALIZED(LOADING)
		                                           : !cur_video_info.suggestions.size()    ? LOCALIZED(EMPTY)
		                                                                                   : "IE")
		                                ->set_font_size(0.5, DEFAULT_FONT_INTERVAL)
		                                ->set_x_alignment(TextView::XAlign::CENTER)
		                                ->set_y_alignment(TextView::YAlign::UP);

		    suggestion_tab_view->set_on_child_drawn(1, [](const ScrollView &, int) {
			    if (cur_video_info.has_more_suggestions() && cur_video_info.error == "") {
				    if (!is_async_task_running(load_video_page) && !is_async_task_running(load_more_suggestions)) {
					    queue_async_task(load_more_suggestions, &cur_video_info);
				    }
			    }
		    });
		    suggestion_bottom_view = bottom_view;
	    } else {
		    suggestion_bottom_view = new EmptyView(0, 0, 320, 0);
	    }
	    suggestion_tab_view->views[1] = suggestion_bottom_view;
    }
    // also used for playlist items
    static SuccinctVideoView *suggestion_to_view(const YouTubeSuccinctItem &item) {
	    SuccinctVideoView *cur_view = (new SuccinctVideoView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT));
	    cur_view->set_title_lines(truncate_str(item.get_name(), SUGGESTION_TITLE_MAX_WIDTH, 2, 0.5, 0.5));
	    cur_view->set_thumbnail_url(item.get_thumbnail_url());
	    if (item.type == YouTubeSuccinctItem::VIDEO) {
		    cur_view->set_auxiliary_lines({item.video.author});
		    cur_view->set_bottom_right_overlay(item.video.duration_text);
	    } else if (item.type == YouTubeSuccinctItem::PLAYLIST) {
		    cur_view->set_auxiliary_lines({item.playlist.video_count_str});
	    }
	    cur_view->set_get_background_color(View::STANDARD_BACKGROUND);
	    cur_view->set_on_view_released([item](View &view) { suggestion_clicked_url = item.get_url(); });
	    cur_view->set_is_playlist(item.type == YouTubeSuccinctItem::PLAYLIST);

	    return cur_view;
    }

    static void update_comment_bottom_view() {
	    delete comments_bottom_view;
	    if (cur_video_info.comments_disabled || cur_video_info.error != "" || cur_video_info.has_more_comments() ||
	        !cur_video_info.comments.size()) {
		    TextView *bottom_view = (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 2))
		                                ->set_text(cur_video_info.comments_disabled     ? LOCALIZED(COMMENTS_DISABLED)
		                                           : cur_video_info.error != ""         ? cur_video_info.error
		                                           : cur_video_info.has_more_comments() ? LOCALIZED(LOADING)
		                                           : !cur_video_info.comments.size()    ? LOCALIZED(NO_COMMENTS)
		                                                                                : "IE")
		                                ->set_font_size(0.5, DEFAULT_FONT_INTERVAL)
		                                ->set_x_alignment(TextView::XAlign::CENTER)
		                                ->set_y_alignment(TextView::YAlign::UP);

		    comment_tab_view->set_on_child_drawn(2, [](const ScrollView &, int) {
			    if (cur_video_info.has_more_comments() && cur_video_info.error == "") {
				    if (!is_async_task_running(load_video_page) && !is_async_task_running(load_more_comments)) {
					    queue_async_task(load_more_comments, &cur_video_info);
				    }
			    }
		    });
		    comments_bottom_view = bottom_view;
	    } else {
		    comments_bottom_view = new EmptyView(0, 0, 320, 0);
	    }
	    comment_tab_view->views[2] = comments_bottom_view;
    }
#define COMMENT_MAX_LINE_NUM 1000 // this limit exists due to performance reason (TODO : more efficient truncating)
    static PostView *comment_to_view(const YouTubeVideoDetail::Comment &comment, int comment_index) {
	    auto &cur_content = comment.content;
	    std::vector<std::string> cur_lines;
	    auto itr = cur_content.begin();
	    while (itr != cur_content.end()) {
		    if (cur_lines.size() >= COMMENT_MAX_LINE_NUM) {
			    break;
		    }
		    auto next_itr = std::find(itr, cur_content.end(), '\n');
		    auto tmp = truncate_str(std::string(itr, next_itr), COMMENT_MAX_WIDTH,
		                            COMMENT_MAX_LINE_NUM - cur_lines.size(), 0.5, 0.5);
		    cur_lines.insert(cur_lines.end(), tmp.begin(), tmp.end());

		    if (next_itr != cur_content.end()) {
			    itr = std::next(next_itr);
		    } else {
			    break;
		    }
	    }
	    std::string author_id = comment.author.id;
	    PostView *result =
	        (new PostView(0, 0, 320))
	            ->set_author_name(comment.author.name)
	            ->set_author_icon_url(comment.author.icon_url)
	            ->set_time_str(comment.publish_date)
	            ->set_upvote_str(comment.upvotes_str)
	            ->set_content_lines(cur_lines)
	            ->set_has_more_replies(
	                [comment_index]() { return cur_video_info.comments[comment_index].has_more_replies(); })
	            ->set_on_author_icon_pressed([author_id](const PostView &view) { channel_id_pressed = author_id; })
	            ->set_on_load_more_replies_pressed([comment_index](PostView &view) {
		            queue_async_task(load_more_replies, (void *)comment_index);
		            view.is_loading_replies = true;
	            })
	            ->set_on_timestamp_pressed([](double seconds) {
		            send_seek_request_wo_lock(seconds);
		            var_need_refresh = true;
	            });

	    for (size_t i = 0; i < comment.replies.size(); i++) {
		    auto &cur_reply = comment.replies[i];
		    auto &cur_content = cur_reply.content;
		    std::vector<std::string> cur_reply_lines;
		    auto itr = cur_content.begin();
		    while (itr != cur_content.end()) {
			    if (cur_reply_lines.size() >= COMMENT_MAX_LINE_NUM) {
				    break;
			    }
			    auto next_itr = std::find(itr, cur_content.end(), '\n');
			    auto tmp = truncate_str(std::string(itr, next_itr), REPLY_MAX_WIDTH,
			                            COMMENT_MAX_LINE_NUM - cur_reply_lines.size(), 0.5, 0.5);
			    cur_reply_lines.insert(cur_reply_lines.end(), tmp.begin(), tmp.end());

			    if (next_itr != cur_content.end()) {
				    itr = std::next(next_itr);
			    } else {
				    break;
			    }
		    }
		    std::string reply_author_id = cur_reply.author.id;
		    result->replies.push_back((new PostView(REPLY_INDENT, 0, 320 - REPLY_INDENT))
		                                  ->set_author_name(cur_reply.author.name)
		                                  ->set_author_icon_url(cur_reply.author.icon_url)
		                                  ->set_time_str(cur_reply.publish_date)
		                                  ->set_upvote_str(cur_reply.upvotes_str)
		                                  ->set_content_lines(cur_reply_lines)
		                                  ->set_has_more_replies([]() { return false; })
		                                  ->set_on_author_icon_pressed([reply_author_id](const PostView &view) {
			                                  channel_id_pressed = reply_author_id;
		                                  })
		                                  ->set_is_reply(true)
		                                  ->set_on_timestamp_pressed([](double seconds) {
			                                  send_seek_request_wo_lock(seconds);
			                                  var_need_refresh = true;
		                                  }));
	    }

	    return result;
    }

    // r16: the Info / offline page buttons are theme pills (were square legacy fills): the accent for the primary
    // action, the raised surface for the rest, the pressed tone while held (same touch_darkness ramp as
    // View::STANDARD_BUTTON_BACKGROUND)
    static u32 accent_button_background(const View &view) {
	    if (view.touch_darkness > 0 && view.touch_darkness < 1) {
		    var_need_refresh = true;
	    }
	    return View::blend_color(POCKET_ACCENT, POCKET_ON_ACCENT, view.touch_darkness * 0.2);
    }
    static TextView *pill_button(TextView *view) {
	    view->set_corner_radius(-1);
	    return view;
    }
    // the Info page's Subscribe pill is drawn and hit-tested this far left of its slot, so it ends 10 px from the edge
    // like the deck's controls (the legacy row put it 3 px from the edge)
    constexpr int SUBSCRIBE_INSET = 7;
    // one line that ends in an ellipsis instead of running under the next control
    static std::string one_line(const std::string &s, int max_width, double size) {
	    std::vector<std::string> lines = truncate_str(s, max_width, 1, size, size);
	    return lines.empty() ? std::string() : lines[0];
    }
    // "Download to SD card" block of an online video page: only qualities the player can really play are offered,
    // the size only when the page states it. Built on the async thread; everything the views need is copied in.
    static std::vector<View *> download_action_views(const YouTubeVideoDetail &info) {
	    std::vector<View *> res;
	    std::string reason;
	    std::vector<downloads::DownloadOption> options = downloads::download_options_for(info, var_is_new3ds, &reason);
	    res.push_back((new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
	                      ->set_text("Download to SD card")
	                      ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; }));
	    if (options.empty()) {
		    res.push_back((new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 2))
		                      ->set_text_lines(truncate_str(reason, 320 - SMALL_MARGIN * 2, 2, 0.45, 0.45))
		                      ->set_font_size(0.45, DEFAULT_FONT_INTERVAL)
		                      ->set_get_text_color([]() { return LIGHT1_TEXT_COLOR; }));
		    res.push_back(new RuleView(0, 0, 320, SMALL_MARGIN * 2));
		    return res;
	    }
	    std::vector<downloads::DownloadRequest> requests;
	    std::vector<std::string> ids, size_texts;
	    std::vector<UI::FlexibleString<SelectorView>> chips;
	    int initial = 0;
	    for (size_t i = 0; i < options.size(); i++) {
		    requests.push_back(downloads::download_request_for(info, options[i]));
		    ids.push_back(downloads::make_item_id(info.id, options[i].quality));
		    size_texts.push_back(std::to_string(options[i].quality) + "p, " +
		                         (options[i].estimated_bytes ? "about " + downloads::format_bytes(options[i].estimated_bytes)
		                                                     : std::string("size unknown")) +
		                         (options[i].layout == downloads::Layout::COMBINED ? ", one muxed file"
		                                                                           : ", video + audio files"));
		    chips.push_back(std::to_string(options[i].quality) + "p");
		    if (options[i].quality == var_video_quality) {
			    initial = i;
		    }
	    }
	    SelectorView *selector = (new SelectorView(0, 0, 320, 25, false))->set_texts(chips, initial);
	    res.push_back(selector);
	    std::string video_id = info.id;
	    const int button_width = (320 - SMALL_MARGIN * 4) / 3;
	    TextView *download_button = pill_button(new TextView(0, 0, button_width, 20));
	    download_button->set_text("Download")
	        ->set_x_alignment(TextView::XAlign::CENTER)
	        ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	        ->set_on_view_released([selector, requests, video_id](View &) {
		        std::string message = "Downloads are not available.";
		        int index = selector->selected_button;
		        if (downloads_running() && index >= 0 && index < (int)requests.size()) {
			        bool accepted = downloads_manager().request_download(requests[index], &message);
			        if (accepted && video_playback_active()) {
				        message = "Queued, paused while a video plays: tap Stop playback.";
			        }
		        }
		        download_action_video_id = video_id;
		        download_action_status = message;
		        var_need_refresh = true;
	        });
	    TextView *stop_button = pill_button(new TextView(0, 0, button_width, 20));
	    stop_button->set_text("Stop playback")
	        ->set_x_alignment(TextView::XAlign::CENTER)
	        ->set_get_text_color([]() { return video_playback_active() ? DEFAULT_TEXT_COLOR : LIGHT1_TEXT_COLOR; })
	        ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	        ->set_on_view_released([](View &) {
		        if (video_playback_active()) {
			        video_stop_playback();
			        var_need_refresh = true;
		        }
	        });
	    TextView *library_button = pill_button(new TextView(0, 0, button_width, 20));
	    library_button->set_text("Downloads")
	        ->set_x_alignment(TextView::XAlign::CENTER)
	        ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	        ->set_on_view_released([](View &) {
		        global_intent.next_scene = SceneType::DOWNLOADS;
		        global_intent.arg = "";
	        });
	    res.push_back((new HorizontalListView(0, 0, 20))
	                      ->set_views({(new EmptyView(0, 0, SMALL_MARGIN, 20)), download_button,
	                                   (new EmptyView(0, 0, SMALL_MARGIN, 20)), stop_button,
	                                   (new EmptyView(0, 0, SMALL_MARGIN, 20)), library_button}));
	    res.push_back(
	        (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 3))
	            ->set_text_lines<std::function<std::string()>>(
	                {[selector, size_texts]() {
		                 int index = selector->selected_button;
		                 return index >= 0 && index < (int)size_texts.size() ? size_texts[index] : std::string();
	                 },
	                 [video_id]() { return download_action_video_id == video_id ? download_action_status : std::string(); },
	                 [ids]() {
		                 std::string line;
		                 downloads::ItemView item;
		                 for (auto &id : ids) {
			                 if (!downloads_running() || !downloads_manager().find(id, &item)) {
				                 continue;
			                 }
			                 std::string part = std::to_string(item.meta.quality) + "p " +
			                                    downloads::item_state_text(item.state);
			                 if (item.state == downloads::ItemState::DOWNLOADING && item.total) {
				                 part += " " + std::to_string((int)(item.done * 100 / item.total)) + "%";
			                 }
			                 line += (line.empty() ? "" : ", ") + part;
		                 }
		                 return line;
	                 }})
	            ->set_font_size(0.45, DEFAULT_FONT_INTERVAL)
	            ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; }));
	    res.push_back(new RuleView(0, 0, 320, SMALL_MARGIN * 2));
	    return res;
    }
    // main tab of an offline copy: local metadata only (no channel icon, counts or description, which need a connection)
    static std::vector<View *> local_main_tab_views(const YouTubeVideoDetail &info, const downloads::ItemView &item,
                                                    const std::string &url) {
	    std::vector<View *> res;
	    float title_font_size = MIDDLE_FONT_SIZE;
	    std::vector<std::string> title_lines =
	        truncate_str(info.title, TITLE_MAX_WIDTH, 3, MIDDLE_FONT_SIZE, MIDDLE_FONT_SIZE);
	    if (title_lines.size() == 3) {
		    title_lines = truncate_str(info.title, TITLE_MAX_WIDTH, 2, 0.5, 0.5);
		    title_font_size = 0.5;
	    }
	    res.push_back(
	        (new TextView(0, 0, 320, 15 * title_lines.size()))->set_text_lines(title_lines)->set_font_size(title_font_size, 15));
	    res.push_back(new EmptyView(0, 0, 320, SMALL_MARGIN));
	    uint64_t bytes = item.meta.video.size + (item.meta.layout == downloads::Layout::SEPARATE ? item.meta.audio.size : 0);
	    res.push_back((new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
	                      ->set_text("Offline copy  " + std::to_string(item.meta.quality) + "p  " +
	                                 downloads::format_bytes(bytes) + "  " +
	                                 Util_convert_seconds_to_time((double)item.meta.duration_ms / 1000))
	                      ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; }));
	    res.push_back(new RuleView(0, 0, 320, SMALL_MARGIN * 2));
	    TextView *play_button = pill_button(new TextView(0, 0, 160 - SMALL_MARGIN, 20));
	    play_button
	        ->set_text((std::function<std::string()>)[]() {
		        return cur_video_info.id == playing_video_info.id && vid_play_request ? LOCALIZED(PLAYING) : LOCALIZED(PLAY);
	        })
	        ->set_x_alignment(TextView::XAlign::CENTER)
	        ->set_get_text_color([]() {
		        return cur_video_info.id == playing_video_info.id && vid_play_request ? THEME_TOKEN(acc_tx) : POCKET_ON_ACCENT;
	        })
	        ->set_get_background_color([](const View &view) -> u32 {
		        if (cur_video_info.id == playing_video_info.id && vid_play_request) {
			        return POCKET_ACCENT_TINT;
		        }
		        return accent_button_background(view);
	        })
	        ->set_on_view_released([url](View &) {
		        if (cur_video_info.id != playing_video_info.id || !vid_play_request) {
			        send_change_video_request_wo_lock(url, true, false, false);
		        }
	        });
	    TextView *library_button = pill_button(new TextView(0, 0, 160 - SMALL_MARGIN * 2, 20));
	    library_button->set_text("Downloads")
	        ->set_x_alignment(TextView::XAlign::CENTER)
	        ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	        ->set_on_view_released([](View &) {
		        global_intent.next_scene = SceneType::DOWNLOADS;
		        global_intent.arg = "";
	        });
	    res.push_back((new HorizontalListView(0, 0, 20))
	                      ->set_views({(new EmptyView(0, 0, SMALL_MARGIN, 20)), play_button,
	                                   (new EmptyView(0, 0, SMALL_MARGIN, 20)), library_button}));
	    res.push_back(new RuleView(0, 0, 320, SMALL_MARGIN * 2));
	    res.push_back((new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 3))
	                      ->set_text_lines(std::vector<std::string>{"Played from the SD card, no connection needed.",
	                                                                "Captions, comments, suggestions and channel",
	                                                                "details are not shown for offline copies."})
	                      ->set_font_size(0.45, DEFAULT_FONT_INTERVAL)
	                      ->set_get_text_color([]() { return LIGHT1_TEXT_COLOR; }));
	    return res;
    }

    // arg :
    //   cur_playing_url : only update the data for the player
    //   cur_displaying_url : only update the displayed data
    //   NULL : both
    // arg : request id from load_requests.admit() (see send_change_video_request_wo_lock)
    static void load_video_page(void *arg) {
	    const unsigned request_id = (unsigned)(uintptr_t)arg;
	    struct FinishGuard { // drops the request record on every return path
		    unsigned id;
		    ~FinishGuard() {
			    small_resource_lock.lock();
			    load_requests.finish(id);
			    small_resource_lock.unlock();
		    }
	    } finish_guard = {request_id};

	    small_resource_lock.lock();
	    playback_diag::RequestRegistry::Request request;
	    if (!load_requests.lookup(request_id, &request)) { // superseded before it even started
		    small_resource_lock.unlock();
		    return;
	    }
	    const bool is_to_play = request.to_play;
	    const bool is_to_display = request.to_display;
	    const std::string url = request.url; // immutable: captured at admission, not read from a global now
	    const bool is_local = is_local_video_url(url); // SD download: never loaded from the network
	    downloads::ItemView local_item;
	    YouTubeVideoDetail tmp_video_info;
	    bool need_loading = false;
	    if (!is_local && video_info_cache.count(url)) {
		    tmp_video_info = video_info_cache[url];
	    } else {
		    need_loading = true;
	    }
	    if (need_loading && is_to_display) {
		    static TextView *loading_view = (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
		                                        ->set_text([]() { return LOCALIZED(LOADING); })
		                                        ->set_x_alignment(TextView::XAlign::CENTER)
		                                        ->set_y_alignment(TextView::YAlign::UP);
		    for (int i = 0; i < 4; i++) {
			    main_view->views[i] = loading_view;
		    }
		    // tab #4(Playback) and #5(Playlist) remain displayed
		    var_need_refresh = true;
	    }
	    small_resource_lock.unlock();

	    if (need_loading && is_local) {
		    tmp_video_info = local_video_detail(url, &local_item);
	    } else if (need_loading) {
		    logger.info("player/load-v", "request : " + url);
		    freeze_diag::phase("async:page_load");
		    add_cpu_limit(ADDITIONAL_CPU_LIMIT);
		    tmp_video_info = youtube_load_video_page(url);
		    remove_cpu_limit(ADDITIONAL_CPU_LIMIT);
		    freeze_diag::phase("async:page_publish");
	    }

	    if (is_to_display) {
		    logger.info("player/load-v", "truncate/view creation start");
		    // prepare views in the main tab
		    std::vector<View *> main_tab_views;
		    ImageView *new_main_icon_view = (new ImageView(0, 0, ICON_SIZE, ICON_SIZE));
		    if (tmp_video_info.title == "" && tmp_video_info.playability_reason != "") {
			    // Check if it's an age-restricted video
			    std::string display_message = tmp_video_info.playability_reason;
			    if (tmp_video_info.playability_status == "UNPLAYABLE" ||
			        tmp_video_info.playability_status == "LOGIN_REQUIRED" ||
			        tmp_video_info.playability_reason.find("age") != std::string::npos ||
			        tmp_video_info.playability_reason.find("restricted") != std::string::npos ||
			        tmp_video_info.playability_reason.find("sign in") != std::string::npos) {
				    display_message = LOCALIZED(AGE_RESTRICTED_VIDEO);
			    }
			    main_tab_views = {(new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 6))
			                          ->set_text_lines(split_string(display_message, '\n'))
			                          ->set_x_alignment(TextView::XAlign::CENTER)
			                          ->set_font_size(0.45, DEFAULT_FONT_INTERVAL)};
		    } else if (is_local) {
			    main_tab_views = local_main_tab_views(tmp_video_info, local_item, url);
		    } else {
			    // wrap main title
			    float title_font_size;
			    std::vector<std::string> title_lines =
			        truncate_str(tmp_video_info.title, TITLE_MAX_WIDTH, 3, MIDDLE_FONT_SIZE, MIDDLE_FONT_SIZE);
			    if (title_lines.size() == 3) {
				    title_lines = truncate_str(tmp_video_info.title, TITLE_MAX_WIDTH, 2, 0.5, 0.5);
				    title_font_size = 0.5;
			    } else {
				    title_font_size = MIDDLE_FONT_SIZE;
			    }

			    std::string author_id = tmp_video_info.author.id;
			    new_main_icon_view
			        ->set_handle(thumbnail_request(tmp_video_info.author.icon_url, SceneType::VIDEO_PLAYER, 1000,
			                                       ThumbnailType::ICON))
			        ->set_on_view_released([author_id](View &) { channel_id_pressed = author_id; });
			    main_tab_views = {
			        (new TextView(0, 0, 320, 15 * title_lines.size())) // title
			            ->set_text_lines(title_lines)
			            ->set_font_size(title_font_size, 15),
			        (new EmptyView(0, 0, 320, SMALL_MARGIN)),
			        (new HorizontalListView(0, 0, DEFAULT_FONT_INTERVAL)) // view count
			            ->set_views({
			                (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
			                    ->set_text(tmp_video_info.views_str)
			                    ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; }),
			            }),
			        (new HorizontalListView(0, 0, DEFAULT_FONT_INTERVAL)) // publish date
			            ->set_views({(new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
			                             ->set_text(tmp_video_info.publish_date)
			                             ->set_x_alignment(TextView::XAlign::LEFT)
			                             ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; })}),
			        (new CustomView(0, 0, 320, DEFAULT_FONT_INTERVAL + 4)) // like/dislike
			            ->set_draw([](const CustomView &view) {
				            // r16: vector outlines in the theme's secondary ink (the opaque thumb textures drew as squares);
				            // YouTube's public counts only, never the local like toggle
				            const ThemeTokens &t = theme_cur();
				            float y = view.y0, h = DEFAULT_FONT_INTERVAL + 4, x = std::floor(view.x0 + SMALL_MARGIN);
				            modern::icon(modern::Icon::LIKE, x + 8, y + h / 2, 15, t.tx2);
				            x += 20;
				            modern::text(cur_video_info.like_count_str, x, modern::text_y(y, h, modern::T_META), modern::T_META, t.tx2);
				            x += modern::text_w(cur_video_info.like_count_str, modern::T_META) + 12;
				            if (!cur_video_info.dislike_count_str.empty()) {
					            modern::icon(modern::Icon::DISLIKE, x + 8, y + h / 2, 15, t.tx2);
					            modern::text(cur_video_info.dislike_count_str, x + 20, modern::text_y(y, h, modern::T_META),
					                         modern::T_META, t.tx2);
				            }
			            }),
			        (new RuleView(0, 0, 320, SMALL_MARGIN * 2)),
			        (new HorizontalListView(0, 0, ICON_SIZE)) // author
			            ->set_views(
			                {(new EmptyView(0, 0, SMALL_MARGIN, ICON_SIZE)), new_main_icon_view,
			                 (new EmptyView(0, 0, SMALL_MARGIN, ICON_SIZE)),
			                 (new VerticalListView(0, 0,
			                                       tmp_video_info.author.is_collaboration
			                                           ? 320 - SMALL_MARGIN * 2 - ICON_SIZE
			                                           : 320 - SMALL_MARGIN * 3 - ICON_SIZE - SUBSCRIBE_BUTTON_WIDTH))
			                     ->set_views(
			                         {(new EmptyView(0, 0,
			                                         tmp_video_info.author.is_collaboration
			                                             ? 320 - SMALL_MARGIN * 2 - ICON_SIZE
			                                             : 320 - SMALL_MARGIN * 3 - ICON_SIZE - SUBSCRIBE_BUTTON_WIDTH,
			                                         ICON_SIZE * 0.1)),
			                          (new TextView(0, 0,
			                                        tmp_video_info.author.is_collaboration
			                                            ? 320 - SMALL_MARGIN * 2 - ICON_SIZE
			                                            : 320 - SMALL_MARGIN * 3 - ICON_SIZE - SUBSCRIBE_BUTTON_WIDTH,
			                                        DEFAULT_FONT_INTERVAL + SMALL_MARGIN))
			                              ->set_text(one_line(tmp_video_info.author.name,
			                                                  (tmp_video_info.author.is_collaboration ? 320 - SMALL_MARGIN * 2 - ICON_SIZE
			                                                                                          : 320 - SMALL_MARGIN * 3 - ICON_SIZE -
			                                                                                                SUBSCRIBE_BUTTON_WIDTH) -
			                                                      SMALL_MARGIN * 2 - SUBSCRIBE_INSET,
			                                                  0.55))
			                              ->set_font_size(0.55, DEFAULT_FONT_INTERVAL + SMALL_MARGIN),
			                          (new TextView(0, 0,
			                                        tmp_video_info.author.is_collaboration
			                                            ? 320 - SMALL_MARGIN * 2 - ICON_SIZE
			                                            : 320 - SMALL_MARGIN * 3 - ICON_SIZE - SUBSCRIBE_BUTTON_WIDTH,
			                                        DEFAULT_FONT_INTERVAL))
			                              ->set_text(tmp_video_info.author.subscribers.empty()
			                                             ? ""
			                                             : (tmp_video_info.metadata_from_android_vr
			                                                    ? tmp_video_info.author.subscribers
			                                                    : std::regex_replace(
			                                                          LOCALIZED(SUBSCRIBER_COUNT), std::regex("%0"),
			                                                          tmp_video_info.author.subscribers)))
			                              ->set_font_size(0.45, DEFAULT_FONT_INTERVAL + SMALL_MARGIN)
			                              ->set_get_text_color([]() { return LIGHT1_TEXT_COLOR; })}),
			                 tmp_video_info.author.is_collaboration
			                     ? (View *)(new EmptyView(0, 0, 0, ICON_SIZE))
			                     : (View *)(new CustomView(0, 0, SUBSCRIBE_BUTTON_WIDTH, ICON_SIZE))
			                           ->set_draw([author_id](const CustomView &view) {
				                           // r16: the deck's Subscribe pill (was a square), label centered on the drawn rect; same hit rect
				                           const ThemeTokens &t = theme_cur();
				                           bool is_subscribed = subscription_is_subscribed(author_id);
				                           std::string button_text = modern::ellipsize(is_subscribed ? LOCALIZED(SUBSCRIBED) : LOCALIZED(SUBSCRIBE),
				                                                                       SUBSCRIBE_BUTTON_WIDTH - 12, modern::T_META);
				                           float button_y = std::floor(view.y0 + (ICON_SIZE - SUBSCRIBE_BUTTON_HEIGHT) / 2);
				                           modern::pill(view.x0 - SUBSCRIBE_INSET, button_y, SUBSCRIBE_BUTTON_WIDTH, SUBSCRIBE_BUTTON_HEIGHT, is_subscribed ? t.surf2 : t.sel);
				                           modern::text_center(button_text, view.x0 - SUBSCRIBE_INSET, view.x0 - SUBSCRIBE_INSET + SUBSCRIBE_BUTTON_WIDTH,
				                                               modern::text_y(button_y, SUBSCRIBE_BUTTON_HEIGHT, modern::T_META), modern::T_META,
				                                               is_subscribed ? t.tx : t.sel_ink);
			                           })
			                           ->set_update([author_id](CustomView &view, Hid_info key) {
				                           float button_y = view.y0 + (ICON_SIZE - SUBSCRIBE_BUTTON_HEIGHT) / 2;
				                           bool in_button = key.touch_x >= view.x0 - SUBSCRIBE_INSET &&
				                                            key.touch_x < view.x0 - SUBSCRIBE_INSET + SUBSCRIBE_BUTTON_WIDTH &&
				                                            key.touch_y >= button_y &&
				                                            key.touch_y < button_y + SUBSCRIBE_BUTTON_HEIGHT;
				                           if (key.p_touch && in_button) {
					                           bool cur_subscribed = subscription_is_subscribed(author_id);
					                           if (cur_subscribed) {
						                           subscription_unsubscribe(author_id);
					                           } else {
						                           SubscriptionChannel new_channel;
						                           new_channel.id = author_id;
						                           new_channel.url = "https://m.youtube.com/channel/" + author_id;
						                           new_channel.name = cur_video_info.author.name;
						                           new_channel.icon_url = cur_video_info.author.icon_url;
						                           new_channel.subscriber_count_str = cur_video_info.author.subscribers;
						                           subscription_subscribe(new_channel);
					                           }
					                           misc_tasks_request(TASK_SAVE_SUBSCRIPTION);
					                           Home_update_local_channels();
					                           var_need_refresh = true;
				                           }
			                           })}),
			        (new RuleView(0, 0, 320, SMALL_MARGIN * 2))};
			    if (tmp_video_info.is_upcoming) {
				    std::vector<View *> add_views = {
				        (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))->set_text(tmp_video_info.playability_reason),
				        (new RuleView(0, 0, 320, SMALL_MARGIN * 2))};
				    main_tab_views.insert(main_tab_views.end(), add_views.begin(), add_views.end());
			    }
			    {
				    TextView *play_button = pill_button(new TextView(0, 0, 160 - SMALL_MARGIN, 20));
				    bool playable = tmp_video_info.is_playable();
				    play_button
				        ->set_text((std::function<std::string()>)[]() {
					        return cur_video_info.id == playing_video_info.id ? LOCALIZED(PLAYING) : LOCALIZED(PLAY);
				        })
				        ->set_x_alignment(TextView::XAlign::CENTER)
				        ->set_get_text_color([playable]() {
					        return cur_video_info.id == playing_video_info.id ? THEME_TOKEN(acc_tx)
					               : playable                                  ? POCKET_ON_ACCENT
					                                                           : LIGHT1_TEXT_COLOR;
				        });
				    if (playable) {
					    play_button
					        ->set_on_view_released([url](View &) {
						        if (cur_video_info.id != playing_video_info.id) {
							        send_change_video_request_wo_lock(url, true, false, false);
						        }
					        })
					        ->set_get_background_color([](const View &view) -> u32 {
						        if (cur_video_info.id == playing_video_info.id) {
							        return POCKET_ACCENT_TINT;
						        }
						        return accent_button_background(view);
					        });
				    } else {
					    play_button->set_get_background_color([](const View &) { return POCKET_SURFACE; });
					    logger.info("video_player", "Video is not playable: " + tmp_video_info.playability_reason);
				    }
				    TextView *reload_button = pill_button(new TextView(0, 0, 160 - SMALL_MARGIN * 2, 20));
				    reload_button->set_text((std::function<std::string()>)[]() { return LOCALIZED(RELOAD); })
				        ->set_x_alignment(TextView::XAlign::CENTER)
				        ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
				        ->set_on_view_released([url](View &) {
					        if (!is_async_task_running(load_video_page)) {
						        send_change_video_request_wo_lock(cur_displaying_url, false, true, true);
						        video_retry_left = MAX_RETRY_CNT;
					        }
				        });

				    main_tab_views.push_back(
				        (new HorizontalListView(0, 0, 20))
				            ->set_views({(new EmptyView(0, 0, SMALL_MARGIN, 20)), play_button,
				                         (new EmptyView(0, 0, SMALL_MARGIN, 20)), reload_button}));
				    main_tab_views.push_back(
				        (new RuleView(0, 0, 320, SMALL_MARGIN * 2)));
			    }
			    for (auto view : download_action_views(tmp_video_info)) { // SD download of this video
				    main_tab_views.push_back(view);
			    }
			    {
				    std::vector<std::string> description_lines;
				    auto &description = tmp_video_info.description;
				    auto itr = description.begin();
				    while (itr != description.end()) {
					    auto next_itr = std::find(itr, description.end(), '\n');
					    auto cur_lines = truncate_str(std::string(itr, next_itr), DESC_MAX_WIDTH, 100, 0.5, 0.5);
					    description_lines.insert(description_lines.end(), cur_lines.begin(), cur_lines.end());
					    if (next_itr != description.end()) {
						    itr = std::next(next_itr);
					    } else {
						    break;
					    }
				    }

				    // Create PostView for description to enable timestamp clicking
				    PostView *description_view = (new PostView(0, 0, 320))
				                                     ->set_is_description_mode(true)
				                                     ->set_author_name("")
				                                     ->set_author_icon_url("")
				                                     ->set_time_str("")
				                                     ->set_upvote_str("")
				                                     ->set_content_lines(description_lines)
				                                     ->set_has_more_replies([]() { return false; })
				                                     ->set_on_timestamp_pressed([](double seconds) {
					                                     send_seek_request_wo_lock(seconds);
					                                     var_need_refresh = true;
				                                     });
				    description_view->lines_shown = description_lines.size();

				    std::vector<View *> add_views = {
				        description_view,
				        (new RuleView(0, 0, 320, SMALL_MARGIN * 2))};
				    main_tab_views.insert(main_tab_views.end(), add_views.begin(), add_views.end());
			    }
		    }
		    // prepare suggestion view
		    std::vector<View *> new_suggestion_views;
		    for (size_t i = 0; i < tmp_video_info.suggestions.size(); i++) {
			    new_suggestion_views.push_back(suggestion_to_view(tmp_video_info.suggestions[i]));
		    }

		    // prepare comment views (comments exist from the first if it's loaded from cache)
		    std::vector<View *> new_comment_views;
		    for (size_t i = 0; i < tmp_video_info.comments.size(); i++) {
			    new_comment_views.push_back(comment_to_view(tmp_video_info.comments[i], i));
		    }

		    // prepare captions view
		    static std::string selected_base_lang = "";
		    static std::string selected_translation_lang = "";
		    static std::pair<std::string *, std::string *> load_caption_arg = {&selected_base_lang,
		                                                                       &selected_translation_lang};

		    selected_base_lang =
		        tmp_video_info.caption_base_languages.size() ? tmp_video_info.caption_base_languages[0].id : "";
		    selected_translation_lang = "";

		    int exit_button_height = DEFAULT_FONT_INTERVAL * 1.5;
		    View *exit_button_view =
		        (new HorizontalListView(0, 0, exit_button_height))
		            ->set_views({(new EmptyView(0, 0, 320 - 100, exit_button_height))
		                             ->set_get_background_color([](const View &) { return DEFAULT_BACK_COLOR; }),
		                         (new TextView(0, 0, 100, exit_button_height))
		                             ->set_text((std::function<std::string()>)[]() { return LOCALIZED(OK); })
		                             ->set_x_alignment(TextView::XAlign::CENTER)
		                             ->set_text_offset(0, -2.0)
		                             ->set_get_text_color([]() { return POCKET_ON_ACCENT; })
		                             ->set_get_background_color([](const View &view) {
			                             return View::blend_color(POCKET_ACCENT, POCKET_PRESSED, view.touch_darkness * 0.5);
		                             })
		                             ->set_on_view_released(
		                                 [](View &view) { queue_async_task(load_caption, &load_caption_arg); })})
		            ->set_draw_order({1, 0});

		    int language_selector_height = CONTENT_Y_HIGH - exit_button_view->get_height();
		    // base languages
		    VerticalListView *caption_left_view = new VerticalListView(0, 0, 160);
		    caption_left_view->views.push_back(
		        (new TextView(0, 0, 160, DEFAULT_FONT_INTERVAL * 1.2))
		            ->set_text((std::function<std::string()>)[]() { return LOCALIZED(CAPTION_BASE_LANGUAGES); })
		            ->set_get_background_color([](const View &) { return DEFAULT_BACK_COLOR; }));
		    caption_left_view->views.push_back((new RuleView(0, 0, 160, 3))->set_get_background_color([](const View &) {
			    return DEFAULT_BACK_COLOR;
		    }));
		    ScrollView *base_languages_selector_view =
		        new ScrollView(0, 0, 160, language_selector_height - caption_left_view->get_height());
		    for (size_t i = 0; i < tmp_video_info.caption_base_languages.size(); i++) {
			    auto &cur_lang = tmp_video_info.caption_base_languages[i];
			    base_languages_selector_view->views.push_back(
			        (new TextView(0, 0, 160, DEFAULT_FONT_INTERVAL))
			            ->set_text(cur_lang.name)
			            ->set_text_offset(0.0, -1.0)
			            ->set_get_text_color(
			                [cur_lang]() { return cur_lang.id == selected_base_lang ? POCKET_ACCENT : DEFAULT_TEXT_COLOR; })
			            ->set_get_background_color([cur_lang](const View &view) -> u32 {
				            if (cur_lang.id == selected_base_lang) {
					            return POCKET_ACCENT_TINT;
				            }
				            return View::blend_color(DEFAULT_BACK_COLOR, POCKET_PRESSED,
				                                     std::min(1.0, 0.15 * view.view_holding_time));
			            })
			            ->set_on_view_released([cur_lang](View &view) { selected_base_lang = cur_lang.id; }));
		    }
		    caption_left_view->views.push_back(base_languages_selector_view);
		    caption_left_view->set_draw_order({2, 0, 1});

		    // translation languages
		    VerticalListView *caption_right_view = new VerticalListView(0, 0, 160);
		    ScrollView *translation_languages_selector_view = new ScrollView(0, 0, 160, 0); // dummy height
		caption_right_view->views.push_back(
		    (new SelectorView(0, 0, 160, DEFAULT_FONT_INTERVAL * 2.5, false))
		        ->set_texts({(std::function<std::string()>)[]() { return LOCALIZED(OFF);
	    }
	    , (std::function<std::string()>)[]() { return LOCALIZED(ON); }
    },
		                    0)
		        ->set_title([](const SelectorView &) {
	return LOCALIZED(CAPTION_TRANSLATION); })
		        ->set_on_change([translation_languages_selector_view](const SelectorView &view) {
	translation_languages_selector_view->set_is_visible(view.selected_button)->set_is_touchable(view.selected_button);
		        })
		        ->set_get_background_color([](const View &) {
	return DEFAULT_BACK_COLOR; }));
    translation_languages_selector_view->set_height(language_selector_height - caption_right_view->get_height());
    for (size_t i = 0; i < tmp_video_info.caption_translation_languages.size(); i++) {
	    auto &cur_lang = tmp_video_info.caption_translation_languages[i];
	    TextView *cur_option_view =
	        (new TextView(0, 0, 160, DEFAULT_FONT_INTERVAL))->set_text(cur_lang.name)->set_text_offset(0.0, -1.0);
	    cur_option_view->set_get_text_color(
	        [cur_lang]() { return cur_lang.id == selected_translation_lang ? POCKET_ACCENT : DEFAULT_TEXT_COLOR; });
	    cur_option_view->set_get_background_color([cur_lang](const View &view) -> u32 {
		    if (cur_lang.id == selected_translation_lang) {
			    return POCKET_ACCENT_TINT;
		    }
		    return View::blend_color(DEFAULT_BACK_COLOR, POCKET_PRESSED, std::min(1.0, 0.15 * view.view_holding_time));
	    });
	    cur_option_view->set_on_view_released([cur_lang](View &view) { selected_translation_lang = cur_lang.id; });
	    translation_languages_selector_view->views.push_back(cur_option_view);
    }
    translation_languages_selector_view->set_is_visible(false)->set_is_touchable(false);
    caption_right_view->views.push_back(translation_languages_selector_view);
    caption_right_view->set_draw_order({1, 0});

    // prepare playlist view
    double playlist_view_scroll = playlist_list_view->get_offset(); // this call should not need mutex locking
    std::vector<View *> new_playlist_views;
    for (auto video : tmp_video_info.playlist.videos) {
	    new_playlist_views.push_back(suggestion_to_view(YouTubeSuccinctItem(video)));
    }
    if (tmp_video_info.playlist.id != "" && tmp_video_info.playlist.id == playing_video_info.playlist.id) {
	    for (size_t i = 0; i < tmp_video_info.playlist.videos.size(); i++) {
		    if (youtube_get_video_id_by_url(tmp_video_info.playlist.videos[i].url) == playing_video_info.id) {
			    new_playlist_views[i]->set_get_background_color([](const View &) { return POCKET_PRESSED; });
		    }
	    }
    }
    if (tmp_video_info.playlist.selected_index >= 0 &&
        tmp_video_info.playlist.selected_index < (int)new_playlist_views.size()) {
	    new_playlist_views[tmp_video_info.playlist.selected_index]->set_get_background_color(
	        [](const View &) { return POCKET_ACCENT_TINT; });
	    double selected_y = tmp_video_info.playlist.selected_index * SUGGESTIONS_VERTICAL_INTERVAL;
	    if (playlist_view_scroll < selected_y - (CONTENT_Y_HIGH - PLAYLIST_TOP_HEIGHT)) {
		    playlist_view_scroll = selected_y - (CONTENT_Y_HIGH - PLAYLIST_TOP_HEIGHT) + SUGGESTIONS_VERTICAL_INTERVAL;
	    }
	    if (playlist_view_scroll > selected_y + SUGGESTIONS_VERTICAL_INTERVAL) {
		    playlist_view_scroll = selected_y;
	    }
    } else {
	    playlist_view_scroll = 0;
    }
    playlist_view_scroll =
        std::min<double>(playlist_view_scroll, tmp_video_info.playlist.videos.size() * SUGGESTIONS_VERTICAL_INTERVAL -
                                                   (CONTENT_Y_HIGH - PLAYLIST_TOP_HEIGHT));
    playlist_view_scroll = std::max<double>(playlist_view_scroll, 0);

    logger.info("player/load-v", "truncate/view creation end");

    // acquire lock and perform actual replacements
    small_resource_lock.lock();
    if (!vid_already_init) { // app shut down while loading
	    small_resource_lock.unlock();
	    return;
    }
    cur_video_info = tmp_video_info;
    video_info_cache[url] = tmp_video_info;
    if (cur_video_info.playlist.videos.size()) {
	    main_view
	        ->set_views({main_tab_view, suggestion_tab_view, comment_tab_view, captions_tab_view, playback_tab_view,
	                     playlist_tab_view},
	                    TAB_PLAYLIST)
	        ->set_tab_texts<std::function<std::string()>>(
	            {[]() { return LOCALIZED(GENERAL); }, []() { return LOCALIZED(SUGGESTIONS); },
	             []() { return LOCALIZED(COMMENTS); }, []() { return LOCALIZED(CAPTIONS); },
	             []() { return LOCALIZED(PLAYBACK); }, []() { return LOCALIZED(PLAYLIST); }});
    } else {
	    main_view
	        ->set_views({main_tab_view, suggestion_tab_view, comment_tab_view, captions_tab_view, playback_tab_view},
	                    TAB_GENERAL)
	        ->set_tab_texts<std::function<std::string()>>(
	            {[]() { return LOCALIZED(GENERAL); }, []() { return LOCALIZED(SUGGESTIONS); },
	             []() { return LOCALIZED(COMMENTS); }, []() { return LOCALIZED(CAPTIONS); },
	             []() { return LOCALIZED(PLAYBACK); }});
    }

    thumbnail_cancel_request(main_icon_view->handle);
    main_tab_view->recursive_delete_subviews();
    main_icon_view = new_main_icon_view;
    main_tab_view->set_views({main_tab_views});

    suggestion_main_view->recursive_delete_subviews();
    suggestion_main_view->views = new_suggestion_views;
    suggestion_tab_view->reset();
    update_suggestion_bottom_view();

    // cancel thumbnail requests
    for (auto view : comment_thumbnail_loaded_list) {
	    thumbnail_cancel_request(view->author_icon_handle);
	    view->author_icon_handle = -1;
    }
    comment_thumbnail_loaded_list.clear();
    comments_main_view->recursive_delete_subviews();
    comments_main_view->views = new_comment_views;
    comment_tab_view->reset();
    update_comment_bottom_view();

    caption_main_view->recursive_delete_subviews();
    caption_overlay_view->set_caption_data({});
    caption_language_select_view->recursive_delete_subviews();
    if (is_local) { // offline copy: no caption list, and no caption request can be made from here
	    for (View *unused : {exit_button_view, (View *)caption_left_view, (View *)caption_right_view}) {
		    unused->recursive_delete_subviews();
		    delete unused;
	    }
	    caption_language_select_view->set_views(
	        {(new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 2))
	             ->set_text("Captions are not available for offline copies.")
	             ->set_x_alignment(TextView::XAlign::CENTER)});
	    caption_language_select_view->set_draw_order({0});
    } else {
	    caption_language_select_view->set_views(
	        {exit_button_view, (new HorizontalListView(0, 0, CONTENT_Y_HIGH - exit_button_view->get_height()))
	                               ->set_views({caption_left_view, caption_right_view})});
	    caption_language_select_view->set_draw_order({1, 0});
    }
    captions_tab_view->set_subview(caption_language_select_view);

    playlist_title_view->set_text(tmp_video_info.playlist.title);
    playlist_author_view->set_text(tmp_video_info.playlist.author_name);
    playlist_list_videos_view->recursive_delete_subviews();
    playlist_list_videos_view->views = new_playlist_views;
    playlist_list_view->set_offset(playlist_view_scroll);

    var_need_refresh = true;
    small_resource_lock.unlock();
    }

    if (is_to_play && video_info_playable(tmp_video_info)) {
	    small_resource_lock.lock();
	    if (!vid_already_init) { // app shut down while loading
		    small_resource_lock.unlock();
		    return;
	    }
	    // Revalidate ownership under the same lock, AFTER the network round trip and immediately before
	    // any player mutation: a newer player request (other video / play / manual reload / startup retry)
	    // or, for a recovery refresh, any user action that bumped the recovery generation (seek, play,
	    // quality change, exit) makes this result stale and it must not touch the player.
	    if (!load_requests.may_publish_player(request_id, playback_recovery)) {
		    logger.info("player/load-v", "stale player load dropped");
		    small_resource_lock.unlock();
		    return;
	    }
	    // r15b: admitted against an update installation before anything of the player changes; kept until the decode
	    // thread is idle again (util/activity_gate.hpp)
	    if (!activity_gate().begin_playback_start()) {
		    small_resource_lock.unlock();
		    Util_err_set_error_message("Playback unavailable",
		                               "An OpenTube update is being installed. Playback is available again when it "
		                               "has finished.",
		                               DEF_SAPP0_MAIN_STR);
		    Util_err_set_error_show_flag(true);
		    var_need_refresh = true;
		    return;
	    }
	    // offline copy: reserve the item before the decode thread may open its files (delete is refused meanwhile)
	    downloads::Playable new_playable;
	    if (is_local) {
		    std::string lease_error = "The download library is not running.";
		    if (!downloads_running() || !downloads_manager().acquire_playback(local_item.id, &new_playable, &lease_error)) {
			    activity_gate().end_playback_start(); // abandoned: no request flag was set
			    small_resource_lock.unlock();
			    Util_err_set_error_message("Offline playback unavailable", lease_error, DEF_SAPP0_MAIN_STR);
			    Util_err_set_error_show_flag(true);
			    var_need_refresh = true;
			    return;
		    }
	    }
	    if (local_published_lease && local_published_lease != local_session_lease) {
		    downloads_manager().release_playback(local_published_lease); // superseded before the decoder opened it
	    }
	    local_published_lease = is_local ? new_playable.lease : 0;
	    playing_local.active = is_local;
	    playing_local.playable = new_playable;
	    if (tmp_video_info.duration_ms > 60 * 60 * 1000) {
		    tmp_video_info.both_stream_url =
		        ""; // for some reason, itag 18 causes problems when the video is long enough
	    }
	    playing_video_info = tmp_video_info;
	    video_info_cache[url] = tmp_video_info;

	    vid_change_video_request = true;
	    activity_gate().end_playback_start(); // the request flag now keeps the admission
	    if (network_decoder.ready) {
		    network_decoder.interrupt = true;
	    }

	    thumbnail_cancel_request(cur_playing_video_view->thumbnail_handle);
	    cur_playing_video_view->thumbnail_handle = -1;
	    cur_playing_video_view
	        ->set_title_lines(
	            truncate_str(tmp_video_info.title, 320 - VIDEO_LIST_THUMBNAIL_WIDTH - SMALL_MARGIN * 3, 2, 0.5, 0.5))
	        ->set_thumbnail_url(tmp_video_info.succinct_thumbnail_url)
	        ->set_auxiliary_lines({tmp_video_info.author.name});
	    cur_playing_video_view->thumbnail_handle = thumbnail_request(
	        tmp_video_info.succinct_thumbnail_url, SceneType::VIDEO_PLAYER, 999, ThumbnailType::VIDEO_THUMBNAIL);

	    // prepare the quality selector
	    std::vector<int> available_qualities;
	    for (auto &i : tmp_video_info.video_stream_urls) {
		    available_qualities.push_back(i.first);
	    }
	    if (is_local) {
		    available_qualities = {new_playable.quality}; // the downloaded quality only
	    }

	    video_quality_selector_view->button_texts = {(std::function<std::string()>)[](){return LOCALIZED(OFF);
    }
    }
    ;
    for (auto i : available_qualities) {
	    if (var_is_new3ds || i <= 240) {
		    video_quality_selector_view->button_texts.push_back(std::to_string(i) + "p");
	    }
    }
    video_quality_selector_view->button_num = video_quality_selector_view->button_texts.size();

    auto is_available = [&](int p_value) {
	    return tmp_video_info.video_stream_urls.count(p_value) ||
	           ((p_value == 360 || p_value == 480) && tmp_video_info.both_stream_url != "");
    };
    if (is_local) {
	    audio_only_mode = var_video_quality == 0;
	    video_p_value = new_playable.quality;
    } else if (var_video_quality == 0) {
	    audio_only_mode = true;
    } else {
	    if (!audio_only_mode && !is_available(var_video_quality)) {
		    video_p_value = var_is_new3ds ? 360 : 144;
		    if (var_is_new3ds && !is_available(video_p_value)) {
			    auto it =
			        std::find_if(available_qualities.rbegin(), available_qualities.rend(),
			                     [&is_available](int quality) { return quality != 480 && is_available(quality); });
			    if (it != available_qualities.rend()) {
				    video_p_value = *it;
			    } else {
				    audio_only_mode = true;
			    }
		    } else {
			    video_p_value = 144;
			    if (!is_available(video_p_value)) {
				    audio_only_mode = true;
			    }
		    }
	    } else {
		    video_p_value = var_video_quality;
	    }
    }

    if (audio_only_mode) {
	    video_quality_selector_view->selected_button = 0;
    } else {
	    auto it = std::find(available_qualities.begin(), available_qualities.end(), (int)video_p_value);
	    if (it != available_qualities.end()) {
		    video_quality_selector_view->selected_button = 1 + (it - available_qualities.begin());
	    } else {
		    auto closest_it = std::min_element(available_qualities.begin(), available_qualities.end(),
		                                       [video_p_value = video_p_value](int a, int b) {
			                                       return std::abs(a - video_p_value) < std::abs(b - video_p_value);
		                                       });
		    if (closest_it != available_qualities.end()) {
			    video_p_value = *closest_it;
			    video_quality_selector_view->selected_button = 1 + (closest_it - available_qualities.begin());
		    } else {
			    audio_only_mode = true;
			    video_quality_selector_view->selected_button = 0;
		    }
	    }
    }
    video_quality_selector_view->set_on_change([available_qualities](const SelectorView &view) {
	    bool changed = false;
	    if (view.selected_button == 0) {
		    if (!audio_only_mode) {
			    changed = true;
		    }
		    audio_only_mode = true;
	    } else {
		    int new_p_value = available_qualities[view.selected_button - 1];
		    if (audio_only_mode || video_p_value != new_p_value) {
			    changed = true;
		    }
		    audio_only_mode = false;
		    video_p_value = new_p_value;
	    }
	    if (changed) {
		    seek_at_init_request = vid_current_pos;
		    playback_recovery.user_reset(); // user changed quality: fresh budget, stale recovery fenced
		    // r15b: admitted atomically with the request flag (util/activity_gate.hpp)
		    if (!activity_gate().admit_playback([]() { vid_change_video_request = true; })) {
			    Util_err_set_error_message("Playback unavailable",
			                               "An OpenTube update is being installed. Playback is available again when "
			                               "it has finished.",
			                               DEF_SAPP0_MAIN_STR);
			    Util_err_set_error_show_flag(true);
			    var_need_refresh = true;
		    }
		    if (network_decoder.ready) {
			    network_decoder.interrupt = true;
		    }
	    }
	    var_video_quality = audio_only_mode ? 0 : video_p_value;
	    misc_tasks_request(TASK_SAVE_SETTINGS);
    });
    // update playlist tab
    if (cur_video_info.playlist.id != "" && cur_video_info.playlist.id == tmp_video_info.playlist.id) {
	    for (size_t i = 0; i < cur_video_info.playlist.videos.size(); i++) {
		    if ((int)i == cur_video_info.playlist.selected_index) {
			    playlist_list_videos_view->views[i]->set_get_background_color(
			        [](const View &) { return POCKET_PRESSED; });
		    } else if (youtube_get_video_id_by_url(tmp_video_info.playlist.videos[i].url) == tmp_video_info.id) {
			    playlist_list_videos_view->views[i]->set_get_background_color(
			        [](const View &) { return POCKET_ACCENT_TINT; });
		    } else {
			    playlist_list_videos_view->views[i]->set_get_background_color(View::STANDARD_BACKGROUND);
		    }
	    }
    }
    small_resource_lock.unlock();
    }
    }
    static void load_more_suggestions(void *arg_) {
	    YouTubeVideoDetail *arg = (YouTubeVideoDetail *)arg_;

	    add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	    auto new_result = *arg;
	    new_result.load_more_suggestions();
	    remove_cpu_limit(ADDITIONAL_CPU_LIMIT);

	    // wrap suggestion titles
	    logger.info("player/load-s", "truncate/view creation start");
	    std::vector<View *> new_suggestion_views;
	    for (size_t i = arg->suggestions.size(); i < new_result.suggestions.size(); i++) {
		    new_suggestion_views.push_back(suggestion_to_view(new_result.suggestions[i]));
	    }
	    logger.info("player/load-s", "truncate/view creation end");

	    small_resource_lock.lock();
	    if (!vid_already_init) { // app shut down while loading
		    small_resource_lock.unlock();
		    return;
	    }
	    if (new_result.error != "") {
		    cur_video_info.error = new_result.error;
	    } else {
		    cur_video_info = new_result;
		    suggestion_main_view->views.insert(suggestion_main_view->views.end(), new_suggestion_views.begin(),
		                                       new_suggestion_views.end());
		    update_suggestion_bottom_view();
		    video_info_cache[cur_video_info.url] = new_result;
	    }
	    var_need_refresh = true;
	    small_resource_lock.unlock();
    }

    static void load_more_comments(void *arg_) {
	    YouTubeVideoDetail *arg = (YouTubeVideoDetail *)arg_;

	    add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	    auto new_result = *arg;
	    new_result.load_more_comments();
	    remove_cpu_limit(ADDITIONAL_CPU_LIMIT);

	    std::vector<View *> new_comment_views;
	    // wrap comments
	    logger.info("player/load-c", "truncate/views creation start");
	    for (size_t i = arg->comments.size(); i < new_result.comments.size(); i++) {
		    new_comment_views.push_back(comment_to_view(new_result.comments[i], i));
	    }
	    logger.info("player/load-c", "truncate/views creation end");

	    small_resource_lock.lock();
	    if (!vid_already_init) { // app shut down while loading
		    small_resource_lock.unlock();
		    return;
	    }
	    cur_video_info = new_result;
	    video_info_cache[cur_video_info.url] = new_result;
	    comments_main_view->views.insert(comments_main_view->views.end(), new_comment_views.begin(),
	                                     new_comment_views.end());
	    update_comment_bottom_view();
	    var_need_refresh = true;
	    small_resource_lock.unlock();
    }

    static void load_more_replies(void *arg_) {
	    int comment_index = (int)arg_;
	    PostView *comment_view = dynamic_cast<PostView *>(comments_main_view->views[comment_index]);
	    YouTubeVideoDetail::Comment &comment = cur_video_info.comments[comment_index];

	    add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	    auto new_comment = comment;
	    new_comment.load_more_replies();
	    remove_cpu_limit(ADDITIONAL_CPU_LIMIT);

	    std::vector<PostView *> new_reply_views;
	    // wrap comments
	    logger.info("player/load-r", "truncate start");
	    for (size_t i = comment.replies.size(); i < new_comment.replies.size(); i++) {
		    auto &cur_reply = new_comment.replies[i];
		    auto &cur_content = cur_reply.content;
		    std::vector<std::string> cur_lines;
		    auto itr = cur_content.begin();
		    while (itr != cur_content.end()) {
			    if (cur_lines.size() >= COMMENT_MAX_LINE_NUM) {
				    break;
			    }
			    auto next_itr = std::find(itr, cur_content.end(), '\n');
			    auto tmp = truncate_str(std::string(itr, next_itr), REPLY_MAX_WIDTH,
			                            COMMENT_MAX_LINE_NUM - cur_lines.size(), 0.5, 0.5);
			    cur_lines.insert(cur_lines.end(), tmp.begin(), tmp.end());

			    if (next_itr != cur_content.end()) {
				    itr = std::next(next_itr);
			    } else {
				    break;
			    }
		    }
		    std::string author_id = cur_reply.author.id;
		    new_reply_views.push_back(
		        (new PostView(REPLY_INDENT, 0, 320 - REPLY_INDENT))
		            ->set_author_name(cur_reply.author.name)
		            ->set_author_icon_url(cur_reply.author.icon_url)
		            ->set_time_str(cur_reply.publish_date)
		            ->set_upvote_str(cur_reply.upvotes_str)
		            ->set_content_lines(cur_lines)
		            ->set_has_more_replies([]() { return false; })
		            ->set_on_author_icon_pressed([author_id](const PostView &view) { channel_id_pressed = author_id; })
		            ->set_is_reply(true)
		            ->set_on_timestamp_pressed([](double seconds) {
			            send_seek_request_wo_lock(seconds);
			            var_need_refresh = true;
		            }));
	    }
	    logger.info("player/load-r", "truncate end");

	    small_resource_lock.lock();
	    if (!vid_already_init) { // app shut down while loading
		    small_resource_lock.unlock();
		    return;
	    }
	    comment = new_comment; // do not apply to the cache because it's a mess to also save the folding status
	    comment_view->replies.insert(comment_view->replies.end(), new_reply_views.begin(), new_reply_views.end());
	    comment_view->replies_shown = comment_view->replies.size();
	    comment_view->is_loading_replies = false;
	    small_resource_lock.unlock();
	    var_need_refresh = true;
    }
    static void load_caption(void *arg_) {
	    auto *arg = (std::pair<std::string *, std::string *> *)arg_;
	    auto &base_lang_id = *arg->first;
	    auto &translation_lang_id = *arg->second;

	    add_cpu_limit(ADDITIONAL_CPU_LIMIT);
	    auto new_result = cur_video_info;
	    new_result.load_caption(base_lang_id, translation_lang_id);
	    remove_cpu_limit(ADDITIONAL_CPU_LIMIT);

	    std::vector<View *> caption_main_views;

	    int top_button_height = DEFAULT_FONT_INTERVAL * 1.5;
	    int top_button_width = 140;
	caption_main_views.push_back(
	    (new HorizontalListView(0, 0, top_button_height))
	        ->set_views({(new SelectorView(0, 0, 320 - top_button_width, top_button_height, false))
	                         ->set_texts({(std::function<std::string()>)[]() { return LOCALIZED(OFF); },
	                                      (std::function<std::string()>)[]() {
		return LOCALIZED(ON); }
    },
	                                     1)
	                         ->set_on_change([](const SelectorView &view) {
	caption_overlay_view->set_is_visible(view.selected_button);
	                         }),
	                     (new TextView(0, 0, top_button_width, top_button_height))
	                         ->set_text((std::function<std::string()>)[]() {
	return LOCALIZED(SELECT_LANGUAGE); })
	                         ->set_text_offset(0.0, -2.0)
	                         ->set_x_alignment(TextView::XAlign::CENTER)
	                         ->set_on_view_released(
	                             [](View &view) {
	captions_tab_view->set_subview(caption_language_select_view); })
	                         ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
    }));
    if (!new_result.caption_data[{base_lang_id, translation_lang_id}].size()) {
	    caption_main_views.push_back(
	        (new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL))
	            ->set_text((std::function<std::string()>)[]() { return LOCALIZED(NO_CAPTION); })
	            ->set_x_alignment(TextView::XAlign::CENTER));
    }
    for (const auto &caption_piece : new_result.caption_data[{base_lang_id, translation_lang_id}]) {
	    const auto &cur_content = caption_piece.content;

	    if (!cur_content.size() || cur_content == "\n") {
		    continue;
	    }

	    std::vector<std::string> cur_lines;
	    auto itr = cur_content.begin();
	    while (itr != cur_content.end()) {
		    auto next_itr = std::find(itr, cur_content.end(), '\n');
		    auto tmp = truncate_str(std::string(itr, next_itr), CAPTION_CONTENT_MAX_WIDTH, 20, 0.5, 0.5);
		    cur_lines.insert(cur_lines.end(), tmp.begin(), tmp.end());

		    if (next_itr != cur_content.end()) {
			    itr = std::next(next_itr);
		    } else {
			    break;
		    }
	    }

	    float start_time = caption_piece.start_time;
	    float end_time = caption_piece.end_time;
	    caption_main_views.push_back(
	        (new HorizontalListView(0, 0, DEFAULT_FONT_INTERVAL * cur_lines.size() + SMALL_MARGIN))
	            ->set_views({(new TextView(0, 0, CAPTION_TIMESTAMP_WIDTH, DEFAULT_FONT_INTERVAL))
	                             ->set_text(Util_convert_seconds_to_time(start_time))
	                             ->set_text_offset(0.0, -1.0)
	                             ->set_get_text_color([]() { return COLOR_LINK; })
	                             ->set_on_view_released([start_time](View &view) {
		                             if (network_decoder.ready) {
			                             send_seek_request_wo_lock(start_time);
		                             }
	                             }),
	                         (new TextView(0, 0, CAPTION_CONTENT_MAX_WIDTH, DEFAULT_FONT_INTERVAL * cur_lines.size()))
	                             ->set_text_lines(cur_lines)
	                             ->set_text_offset(0.0, -1.0)
	                             ->set_get_background_color([start_time, end_time](const View &) {
		                             return vid_current_pos >= start_time && vid_current_pos < end_time
		                                        ? LIGHT1_BACK_COLOR
		                                        : DEFAULT_BACK_COLOR;
	                             })}));
    }
    caption_main_views.push_back(new EmptyView(0, 0, 320, SMALL_MARGIN));

    // caption overlay
    CaptionOverlayView *new_caption_overlay_view =
        (new CaptionOverlayView(0, 0, 400, 240))
            ->set_caption_data(new_result.caption_data[{base_lang_id, translation_lang_id}]);

    small_resource_lock.lock();
    if (!vid_already_init) { // app shut down while loading
	    small_resource_lock.unlock();
	    return;
    }
    caption_main_view->recursive_delete_subviews();
    caption_main_view->reset();
    caption_main_view->set_views(caption_main_views);
    captions_tab_view->set_subview(caption_main_view);

    delete caption_overlay_view;
    caption_overlay_view = new_caption_overlay_view;
    caption_overlay_view->set_is_visible(true);

    small_resource_lock.unlock();
    }

    /* -------------------------------------------------------------------------------------------------------------- */
    /* -------------------------------------------------------------------------------------------------------------- */
    /* -------------------------------------------------------------------------------------------------------------- */
    // END : functions called from async_task.cpp

    static bool send_load_more_suggestions_request() {
	    if (is_async_task_running(load_video_page) || is_async_task_running(load_more_suggestions)) {
		    return false;
	    }
	    queue_async_task(load_more_suggestions, &cur_video_info);
	    return true;
    }

    // should be called while `small_resource_lock` is locked
    static void send_change_video_request_wo_lock(std::string url, bool update_player, bool update_view,
                                                  bool force_load, unsigned recovery_generation) {
	    remove_all_async_tasks_with_type(load_video_page);
	    remove_all_async_tasks_with_type(load_more_suggestions);
	    remove_all_async_tasks_with_type(load_more_comments);

	    unsigned generation_token = 0;
	    if (update_player) {
		    // any player-affecting request fences a pending recovery; a recovery-originated reload captures
		    // the new generation so that any LATER user action fences its own publication too
		    unsigned g = playback_recovery.cancel();
		    if (recovery_generation != 0) {
			    generation_token = g;
		    }
	    }
	    if (force_load) {
		    video_info_cache.erase(url);
	    }

	    if (update_player) {
		    if (cur_playing_url != url) {
			    vid_play_request = false;
			    cur_playing_url = url;
		    }
	    }
	    if (update_view) {
		    if (cur_displaying_url != url) {
			    cur_displaying_url = url;
			    if (main_view->selected_tab != TAB_PLAYLIST) {
				    main_view->selected_tab = TAB_GENERAL;
			    }
		    }
	    }
	    if (update_player || update_view) {
		    // the async task carries an id to an immutable request record, never a pointer to a mutable global
		    unsigned id = load_requests.admit(update_player ? cur_playing_url : cur_displaying_url, update_player,
		                                      update_view, generation_token);
		    queue_async_task(load_video_page, (void *)(uintptr_t)id);
	    }
	    var_need_refresh = true;
    }
    static void send_change_video_request(std::string url, bool update_player, bool update_view, bool force_load) {
	    small_resource_lock.lock();
	    send_change_video_request_wo_lock(url, update_player, update_view, force_load);
	    small_resource_lock.unlock();
    }

    static void send_seek_request_wo_lock(double pos) {
	    playback_recovery.cancel(); // a user seek cancels any pending recovery
	    vid_seek_pos = pos;
	    vid_current_pos = pos;
	    vid_seek_request = true;
	    if (network_decoder.ready) { // avoid locking while initing
		    network_decoder.interrupt = true;
	    }
    }
    static void send_seek_request(double pos) {
	    small_resource_lock.lock();
	    send_seek_request_wo_lock(pos);
	    small_resource_lock.unlock();
    }

    bool video_is_playing() {
	    if (!vid_play_request && vid_pausing_seek) {
		    small_resource_lock.lock();
		    vid_pausing_seek = false;
		    if (!vid_pausing) {
			    Util_speaker_resume(0);
		    }
		    small_resource_lock.unlock();
	    }
	    return vid_play_request;
    }
#define SMALL_FONT_SIZE 0.4

    // OpenTube r13: the r12d 20 px playing bar became two pieces with the same actions.
    //  - the player's large scrubber (draw_scrubber / update_scrubber): grab, audio paused while seeking, one seek on
    //    release, "Recovering playback" / waiting status next to the time;
    //  - the browse mini-player (video_draw_playing_bar / video_update_playing_bar): pause / resume, stop (same as
    //    X+B), tap = full player (the old maximize), progress on its top edge, Y = full player outside Downloads.
    namespace Bar {
    bool bar_grabbed = false;
    double last_grab_timestamp = 0;
    int last_touch_x = -1;
    int last_touch_y = -1;
    float bar_x_l = 10, bar_x_r = 310, bar_y = 92; // track [bar_x_l, bar_x_r], center line bar_y
    constexpr int GRAB_TOLERANCE = 8;
    constexpr int MINI_BUTTON = 28;
    int mini_holding = -1; // 0 card, 1 pause / resume, 2 stop
    bool fullscreen_override = false; // r16: Y from browsing opened this player visit in full screen (never saved)
    int mini_thumb_handle = -1;
    std::string mini_thumb_id;
    u64 mini_thumb_checked = 0;

    // the r12d play-zone action (touch): resume / pause, or start the displayed video when nothing plays.
    // small_resource_lock held.
    void toggle_play_wo_lock() {
	    if (get_network_waiting_status() || vid_pausing_seek) {
		    return;
	    }
	    if (vid_play_request) {
		    if (vid_pausing) {
			    vid_pausing = false;
			    if (eof_reached) {
				    send_seek_request_wo_lock(0);
			    }
			    Util_speaker_resume(0);
		    } else {
			    vid_pausing = true;
			    Util_speaker_pause(0);
		    }
	    } else if (video_info_playable(cur_video_info)) {
		    network_waiting_status = NULL;
		    playback_recovery.user_reset(); // explicit play: fresh runtime recovery budget
		    send_change_video_request_wo_lock(cur_displaying_url, true, false, false);
	    }
    }

    void draw_scrubber(float x, float y, float w) {
	    const ThemeTokens &t = theme_cur();
	    bar_x_l = x, bar_x_r = x + w, bar_y = y + 22;
	    double bar_timestamp = vid_current_pos;
	    if (bar_grabbed) {
		    bar_timestamp = last_grab_timestamp;
	    }
	    double vid_progress = network_decoder.get_bar_pos_from_timestamp(bar_timestamp);
	    std::string time_cur = Util_convert_seconds_to_time(bar_timestamp);
	    std::string time_total = Util_convert_seconds_to_time(vid_duration);
	    modern::text(time_cur, x, y, modern::T_META, t.tx);
	    float total_w = modern::text_w(time_total, modern::T_META);
	    modern::text_right(time_total, x + w, y, modern::T_META, t.tx2);
	    const char *status = get_network_waiting_status();
	    if (status) { // loading / recovering stays visible next to the time
		    float sx = x + modern::text_w(time_cur, modern::T_META) + 4;
		    modern::text(modern::ellipsize(std::string("\xC2\xB7 ") + status, x + w - total_w - 6 - sx, modern::T_META), sx,
		                 y, modern::T_META, status == RECOVERY_STATUS_STR ? t.acc_tx : t.tx2);
	    }
	    modern::pill(bar_x_l, bar_y - 2, w, 4, t.surf2);
	    if (vid_duration != 0) {
		    float px = bar_x_l + w * vid_progress;
		    if (px - bar_x_l >= 1) {
			    modern::pill(bar_x_l, bar_y - 2, std::max(4.0f, px - bar_x_l), 4, t.acc);
		    }
		    modern::circle(px, bar_y, bar_grabbed ? 9 : 8, t.bg);
		    modern::circle(px, bar_y, bar_grabbed ? 7 : 6, t.acc);
	    }
    }
    void update_scrubber(Hid_info key) {
	    if (!bar_grabbed && network_decoder.ready && key.p_touch && bar_x_l - 5 <= key.touch_x &&
	        key.touch_x <= bar_x_r + 5 && bar_y - GRAB_TOLERANCE <= key.touch_y && key.touch_y <= bar_y + GRAB_TOLERANCE) {
		    bar_grabbed = true;
		    var_need_refresh = true;
	    }
	    if (bar_grabbed) {
		    last_grab_timestamp = network_decoder.get_timestamp_from_bar_pos(
		        ((key.touch_x != -1 ? key.touch_x : last_touch_x) - bar_x_l) / (bar_x_r - bar_x_l));
		    var_need_refresh = true;
	    }
	    if (bar_grabbed && key.touch_x == -1) {
		    if (network_decoder.ready) {
			    send_seek_request(last_grab_timestamp);
		    }
		    bar_grabbed = false;
		    var_need_refresh = true;
	    }
	    small_resource_lock.lock();
	    if (!vid_pausing_seek && bar_grabbed && !vid_pausing) {
		    Util_speaker_pause(0);
	    }
	    if (vid_pausing_seek && !bar_grabbed && !vid_pausing) {
		    Util_speaker_resume(0);
	    }
	    vid_pausing_seek = bar_grabbed;
	    small_resource_lock.unlock();

	    last_touch_x = key.touch_x;
	    last_touch_y = key.touch_y;
    }

    // mini-player card above the nav
    static float mini_button_cx(int i) { // 1 pause / resume, 2 stop
	    return shell::MINI_X + shell::MINI_W - 6 - MINI_BUTTON / 2.0f - (i == 1 ? MINI_BUTTON + 5 : 0);
    }
    static int mini_target(int x, int y) {
	    float cy = shell::MINI_Y + shell::MINI_H / 2.0f;
	    for (int i = 1; i <= 2; i++) {
		    float dx = x - mini_button_cx(i), dy = y - cy;
		    if (dx * dx + dy * dy <= (MINI_BUTTON / 2.0f + 3) * (MINI_BUTTON / 2.0f + 3)) {
			    return i;
		    }
	    }
	    if (x >= shell::MINI_X && x < shell::MINI_X + shell::MINI_W && y >= shell::MINI_Y && y < shell::MINI_Y + shell::MINI_H) {
		    return 0;
	    }
	    return -1;
    }
    void video_draw_playing_bar() {
	    const ThemeTokens &t = theme_cur();
	    const float x = shell::MINI_X, y = shell::MINI_Y, w = shell::MINI_W, h = shell::MINI_H;
	    small_resource_lock.lock();
	    std::string title = playing_video_info.title, author = playing_video_info.author.name, id = playing_video_info.id;
	    small_resource_lock.unlock();
	    // the shell never borrows the player's textures: its own request. r16: an offline copy uses its saved image
	    // through the loader's SD-only key (an owned item the downloads worker marked; never a network URL); a copy
	    // whose image is not known yet is looked up again at most every 2 s, not every frame
	    bool local = is_local_video_url(cur_playing_url);
	    u64 now = osGetTime();
	    if (id != mini_thumb_id || (local && mini_thumb_handle == -1 && !id.empty() && now - mini_thumb_checked >= 2000)) {
		    thumbnail_cancel_request(mini_thumb_handle);
		    mini_thumb_handle = -1;
		    mini_thumb_id = id;
		    mini_thumb_checked = now;
		    downloads::ItemView item;
		    if (!id.empty() && !local) {
			    mini_thumb_handle = thumbnail_request(youtube_get_video_thumbnail_url_by_id(id), SceneType::VIDEO_PLAYER,
			                                          PRIORITY_FOREGROUND, ThumbnailType::VIDEO_THUMBNAIL);
		    } else if (!id.empty() && downloads_running() && downloads_manager().find(id, &item) && item.thumb) {
			    mini_thumb_handle = thumbnail_request(Downloads_thumbnail_key(id, false), SceneType::VIDEO_PLAYER,
			                                          PRIORITY_FOREGROUND, ThumbnailType::VIDEO_THUMBNAIL);
		    }
	    }
	    modern::round_rect(x + 1, y + 4, w - 2, h, 11, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 2));
	    modern::round_rect(x, y, w, h, 11, t.line);
	    modern::round_rect(x + 1, y + 1, w - 2, h - 2, 10, t.surf);
	    double progress = vid_duration > 0 ? std::max(0.0, std::min(1.0, vid_current_pos / vid_duration)) : 0;
	    if (progress > 0) {
		    modern::rect(x + 8, y + 1, (w - 16) * progress, 2, t.acc);
	    }
	    const float tx = x + 5, ty = y + 5, tw = 58, th = 32;
	    if (!thumbnail_draw(mini_thumb_handle, tx, ty, tw, th)) {
		    modern::rect(tx, ty, tw, th, t.surf2);
		    modern::icon(modern::Icon::PLAY, tx + tw / 2, ty + th / 2, 12, t.tx3);
	    }
	    modern::corner_mask(tx, ty, tw, th, 6, t.surf);
	    float text_x = tx + tw + 7, text_w = mini_button_cx(1) - MINI_BUTTON / 2.0f - 6 - text_x;
	    modern::text(modern::ellipsize(title.size() ? title : LOCALIZED(LOADING), text_w, modern::T_ROW), text_x, y + 6,
	                 modern::T_ROW, t.tx);
	    const char *status = get_network_waiting_status();
	    std::string meta = status ? std::string(status)
	                              : Util_convert_seconds_to_time(vid_current_pos) + " / " +
	                                    Util_convert_seconds_to_time(vid_duration) + (author.size() ? " \xC2\xB7 " + author : "");
	    modern::text(modern::ellipsize(meta, text_w, modern::T_META), text_x, y + 23, modern::T_META,
	                 status == RECOVERY_STATUS_STR ? t.acc_tx : t.tx2);
	    float cy = y + h / 2;
	    modern::circle(mini_button_cx(1), cy, MINI_BUTTON / 2.0f, mini_holding == 1 ? theme_blend(t.acc, t.acc_ink, 0.2f) : t.acc);
	    modern::icon(status ? modern::Icon::LOADING
	                        : (vid_pausing || vid_pausing_seek) ? modern::Icon::PLAY : modern::Icon::PAUSE,
	                 mini_button_cx(1), cy, 15, t.acc_ink);
	    modern::circle(mini_button_cx(2), cy, MINI_BUTTON / 2.0f, mini_holding == 2 ? t.pressed : t.surf2);
	    modern::icon(modern::Icon::CLOSE, mini_button_cx(2), cy, 15, t.tx);
    }
    void video_update_playing_bar(Hid_info key) {
	    if (bar_grabbed && key.touch_x == -1) { // left the player while seeking: release without a seek
		    bar_grabbed = false;
		    small_resource_lock.lock();
		    if (vid_pausing_seek && !vid_pausing) {
			    Util_speaker_resume(0);
		    }
		    vid_pausing_seek = false;
		    small_resource_lock.unlock();
	    }
	    bool released = key.touch_x == -1 && last_touch_x != -1;
	    if (key.p_touch) {
		    mini_holding = mini_target(key.touch_x, key.touch_y);
		    if (mini_holding != -1) {
			    var_need_refresh = true;
		    }
	    } else if (key.touch_x != -1 && mini_holding != -1 && mini_target(key.touch_x, key.touch_y) != mini_holding) {
		    mini_holding = -1; // slid off
		    var_need_refresh = true;
	    }
	    if (released && mini_holding != -1) {
		    if (mini_holding == 0) {
			    global_intent.next_scene = SceneType::VIDEO_PLAYER;
			    global_intent.arg = cur_playing_url;
		    } else if (mini_holding == 1) {
			    small_resource_lock.lock();
			    toggle_play_wo_lock();
			    small_resource_lock.unlock();
		    } else {
			    video_stop_playback();
		    }
		    mini_holding = -1;
		    var_need_refresh = true;
	    }
	    if (key.p_y && global_current_scene != SceneType::DOWNLOADS) { // Downloads keeps Y = stop playback
		    global_intent.next_scene = SceneType::VIDEO_PLAYER;
		    global_intent.arg = cur_playing_url;
		    fullscreen_override = true; // r16: land on the video full screen; the setting is not touched
	    }
	    last_touch_x = key.touch_x;
	    last_touch_y = key.touch_y;
    }
    // r13b: the shell took the input (START opened the You sheet): drop a pending scrubber grab / mini-player tap
    // without its action (no seek, no full player / pause / stop); audio paused for the seek resumes.
    // small_resource_lock NOT held.
    void cancel_gestures() {
	    if (bar_grabbed) {
		    bar_grabbed = false;
		    small_resource_lock.lock();
		    if (vid_pausing_seek && !vid_pausing) {
			    Util_speaker_resume(0);
		    }
		    vid_pausing_seek = false;
		    small_resource_lock.unlock();
	    }
	    if (mini_holding != -1) {
		    mini_holding = -1;
		    var_need_refresh = true;
	    }
	    last_touch_x = last_touch_y = -1;
    }
    } // namespace Bar
    void video_update_playing_bar(Hid_info key) { Bar::video_update_playing_bar(key); }
    void video_draw_playing_bar() { Bar::video_draw_playing_bar(); }
    void video_cancel_playing_bar_gesture() { Bar::cancel_gestures(); }

    // OpenTube r13 player lower screen ("deck"): title, channel + Subscribe, large scrubber, round transport, CC and
    // quality pills, action pills, Up next / Comments / Info. Every r12d tab body stays unchanged and opens in a sheet
    // (main_view with a hidden selector). Layout + input only: the decoder, threads and recovery are not touched.
    namespace Deck {
    constexpr int SHEET_TOP = 240 - CONTENT_Y_HIGH - 36;
    constexpr int SEC_INFO = 0, SEC_UPNEXT = 1, SEC_COMMENTS = 2, SEC_CAPTIONS = 3, SEC_PLAYBACK = 4, SEC_PLAYLIST = 5;
    int section = -1; // open sheet (main_view tab index), -1 = none
    int holding = -1;
    int last_touch_x = -1, last_touch_y = -1;
    std::string title_cache_src;
    std::vector<std::string> title_cache;
    int title_cache_lines = 2;
    enum Control { C_SUBSCRIBE, C_CC, C_BACK10, C_PLAY, C_FWD10, C_QUALITY, C_DOWNLOAD, C_RELOAD, C_TAB0, C_TAB1, C_TAB2,
                   C_SHEET_ALT, C_LIKE, C_HELP, C_PLAY_NOW, C_NOW_TOGGLE, C_COUNT };
    // r16: the Controls sheet ("?" at the end of the tab bar) is the deck's own sheet, not a main_view section
    bool help_open = false;
    // r16 sheet drag: a press in the header band grabs the open sheet; release past DRAG_CLOSE_PX closes it with a
    // SETTLE_FRAMES ease-out, otherwise it settles back. While grabbed / settling the sheet owns every touch.
    constexpr int DRAG_CLOSE_PX = 36, SETTLE_FRAMES = 6;
    struct SheetDrag {
	    bool grabbed = false, closing = false;
	    int start_y = 0, settle = 0; // settle: frames left
	    float dy = 0, from = 0;
    } drag;
    bool swallow_touch = false; // a finger still down when a settle ended: ignored until it is lifted

    static bool has_playlist() { return main_view->views.size() > SEC_PLAYLIST; }
    static bool is_local_page() { return is_local_video_url(cur_displaying_url); }
    bool sheet_shown() { return section >= 0 || help_open; }
    // r16: a video plays while another page is displayed (a row opened from browsing or Up next): the deck keeps the
    // playing video's transport as a "Now playing" strip and offers Play now for the displayed one
    bool selected_mode() { return vid_play_request && cur_displaying_url != cur_playing_url; }
    // the displayed page's info is loaded (not the previous page's while the new one loads) and playable
    static bool play_now_ready() {
	    bool same_page = is_local_page() ? cur_video_info.url == cur_displaying_url
	                                     : !cur_video_info.id.empty() &&
	                                           youtube_get_video_id_by_url(cur_displaying_url) == cur_video_info.id;
	    return same_page && video_info_playable(cur_video_info);
    }
    static bool can_subscribe() {
	    return !is_local_page() && !cur_video_info.author.id.empty() && !cur_video_info.author.is_collaboration;
    }
    static std::string subscribe_label() {
	    return subscription_is_subscribed(cur_video_info.author.id) ? LOCALIZED(SUBSCRIBED) : LOCALIZED(SUBSCRIBE);
    }
    static std::string quality_label() {
	    if (audio_only_mode) {
		    return "Audio";
	    }
	    return std::to_string((int)video_p_value) + "p";
    }
    static std::string download_label() { return is_local_page() ? "Library" : "Download"; }
    // r15: the like pill belongs to the page it is drawn on (cur_video_info); while another page is loading (its url
    // is already cur_displaying_url, the old info still shown) or on an SD copy page there is none
    static bool like_visible() {
	    return !is_local_page() && liked::make_entry(cur_video_info.id, "", "", 0, 0, NULL) &&
	           youtube_get_video_id_by_url(cur_displaying_url) == cur_video_info.id;
    }
    // YouTube's public count stays as served (external data); "Like" when it is hidden
    static std::string like_label() {
	    return cur_video_info.like_count_str.empty() ? shell::tr("LIKE", "Like") : cur_video_info.like_count_str;
    }
    static std::string tab_label(int i) {
	    return i == 0 ? shell::tr("UP_NEXT", "Up next") : i == 1 ? LOCALIZED(COMMENTS) : LOCALIZED(INFO);
    }
    static std::string section_title(int s) {
	    switch (s) {
	    case SEC_INFO:
		    return LOCALIZED(INFO);
	    case SEC_UPNEXT:
		    return shell::tr("UP_NEXT", "Up next");
	    case SEC_COMMENTS:
		    return LOCALIZED(COMMENTS);
	    case SEC_CAPTIONS:
		    return LOCALIZED(CAPTIONS);
	    case SEC_PLAYBACK:
		    return LOCALIZED(PLAYBACK);
	    default:
		    return LOCALIZED(PLAYLIST);
	    }
    }
    // the playlist and the suggestions share the Up next button; the sheet header switches between them
    static std::string sheet_alt_label() {
	    if (section == SEC_PLAYLIST) {
		    return LOCALIZED(SUGGESTIONS);
	    }
	    if (section == SEC_UPNEXT && has_playlist()) {
		    return LOCALIZED(PLAYLIST);
	    }
	    return "";
    }
    // r16 geometry: the r15c hint row is gone, its 20 px went to the transport (Play 48, seek 40 px) and the rows
    // below; every hit box is at least its r15c size. Row centers: transport 128, actions 175, tabs 213.
    constexpr float ACTION_Y = 162, ACTION_H = 26, TAB_Y = 198, TAB_H = 30, TAB_W = 88, NOW_Y = 68, NOW_H = 34;
    // r16b selected mode: Play now 168 x 48 centered on x 160; CC / quality 60 px wide on its center line (y 132);
    // every neighbour (CC, quality, Now playing strip) >= 6 px away. rect_of() is both the drawing and the hit rect.
    constexpr float PLAY_NOW_W = 168, PLAY_NOW_H = 48, PLAY_NOW_Y = NOW_Y + NOW_H + 6, SEL_SIDE_W = 60;
    shell::Rect rect_of(int c) {
	    switch (c) {
	    case C_SUBSCRIBE: {
		    float w = modern::text_w(subscribe_label(), modern::T_META) + 22;
		    return {310 - w, 44, w, 22};
	    }
	    case C_CC:
		    if (selected_mode()) {
			    return {10, PLAY_NOW_Y + (PLAY_NOW_H - 26) / 2, SEL_SIDE_W, 26};
		    }
		    return {10, 115, 68, 26};
	    case C_BACK10:
		    return {84, 108, 40, 40};
	    case C_PLAY:
		    return {136, 104, 48, 48};
	    case C_FWD10:
		    return {196, 108, 40, 40};
	    case C_QUALITY:
		    if (selected_mode()) {
			    return {310 - SEL_SIDE_W, PLAY_NOW_Y + (PLAY_NOW_H - 26) / 2, SEL_SIDE_W, 26};
		    }
		    return {242, 115, 68, 26};
	    case C_PLAY_NOW:
		    return {160 - PLAY_NOW_W / 2, PLAY_NOW_Y, PLAY_NOW_W, PLAY_NOW_H};
	    case C_NOW_TOGGLE:
		    return {10, NOW_Y, 40, NOW_H};
	    case C_LIKE:
		    return {10, ACTION_Y, modern::text_w(like_label(), modern::T_META) + 34, ACTION_H};
	    case C_DOWNLOAD:
	    case C_RELOAD: {
		    float x = 10;
		    if (like_visible()) {
			    x += rect_of(C_LIKE).w + 5;
		    }
		    float dw = modern::text_w(download_label(), modern::T_META) + 34;
		    if (c == C_DOWNLOAD) {
			    return {x, ACTION_Y, dw, ACTION_H};
		    }
		    return {x + dw + 5, ACTION_Y, modern::text_w(LOCALIZED(RELOAD), modern::T_META) + 34, ACTION_H};
	    }
	    case C_TAB0:
	    case C_TAB1:
	    case C_TAB2:
		    return {10 + (c - C_TAB0) * TAB_W, TAB_Y, TAB_W, TAB_H};
	    case C_HELP:
		    return {280, TAB_Y, 30, TAB_H};
	    case C_SHEET_ALT: {
		    std::string label = sheet_alt_label();
		    if (label.empty()) {
			    return {0, 0, 0, 0};
		    }
		    float w = modern::text_w(label, modern::T_META) + 20;
		    return {shell::sheet_close_rect({0, SHEET_TOP, 320, 240 - SHEET_TOP}).x - 6 - w, SHEET_TOP + 9, w, 22};
	    }
	    }
	    return {0, 0, 0, 0};
    }
    static bool control_enabled(int c) {
	    if (c == C_SHEET_ALT) {
		    return section >= 0 && !help_open && !sheet_alt_label().empty();
	    }
	    if (sheet_shown()) {
		    return false;
	    }
	    if (c == C_SUBSCRIBE) {
		    return can_subscribe();
	    }
	    if (c == C_LIKE) {
		    return like_visible();
	    }
	    if (c == C_PLAY_NOW || c == C_NOW_TOGGLE) {
		    return selected_mode();
	    }
	    if (c == C_BACK10 || c == C_PLAY || c == C_FWD10) {
		    return !selected_mode();
	    }
	    return true;
    }
    static int control_at(int x, int y) {
	    for (int c = 0; c < C_COUNT; c++) {
		    if (control_enabled(c) && rect_of(c).contains(x, y)) {
			    return c;
		    }
	    }
	    return -1;
    }
    void open_section(int s) {
	    if (s < 0 || s >= (int)main_view->views.size()) {
		    return;
	    }
	    section = s;
	    help_open = false;
	    drag = SheetDrag();
	    main_view->selected_tab = s;
	    main_view->reset_holding_status();
	    holding = -1;
	    var_need_refresh = true;
    }
    void close_section() {
	    section = -1;
	    help_open = false;
	    drag = SheetDrag();
	    main_view->reset_holding_status();
	    holding = -1;
	    var_need_refresh = true;
    }
    void open_help() {
	    close_section();
	    help_open = true;
    }
    // r13b: the shell took the input: a held control / sheet row is dropped without its action. r16: a grabbed sheet
    // is released where it was opened (no close); a settle in progress ends at once
    void cancel_gesture() {
	    main_view->reset_holding_status();
	    holding = -1;
	    last_touch_x = last_touch_y = -1;
	    if (drag.settle > 0 && drag.closing) {
		    close_section();
	    }
	    drag = SheetDrag();
    }
    // the sheet's top edge this frame (pixel aligned): open position + the drag / settle offset
    static float sheet_offset() {
	    if (drag.grabbed) {
		    return drag.dy;
	    }
	    if (drag.settle > 0) {
		    float t = 1 - (float)drag.settle / SETTLE_FRAMES, e = 1 - (1 - t) * (1 - t) * (1 - t); // ease-out
		    return drag.closing ? drag.from + (240 - SHEET_TOP - drag.from) * e : drag.from * (1 - e);
	    }
	    return 0;
    }
    static bool in_grab_band(int x, int y) {
	    if (y < SHEET_TOP || y >= SHEET_TOP + 36) {
		    return false;
	    }
	    shell::Rect alt = rect_of(C_SHEET_ALT);
	    return !shell::sheet_close_rect({0, SHEET_TOP, 320, 240 - SHEET_TOP}).contains(x, y) &&
	           !(control_enabled(C_SHEET_ALT) && alt.contains(x, y));
    }
    // per frame while a sheet is shown, before its other input: true when the drag owns this frame's touch
    bool update_drag(const Hid_info &key) {
	    if (drag.settle > 0) {
		    var_need_refresh = true;
		    if (--drag.settle == 0) {
			    if (drag.closing) {
				    close_section();
			    }
			    drag = SheetDrag();
			    swallow_touch = key.touch_x != -1;
		    }
		    return true;
	    }
	    if (!drag.grabbed) {
		    if (key.p_touch && holding == -1 && in_grab_band(key.touch_x, key.touch_y)) {
			    drag.grabbed = true;
			    drag.start_y = key.touch_y;
			    drag.dy = 0;
			    main_view->reset_holding_status();
			    var_need_refresh = true;
			    return true;
		    }
		    return false;
	    }
	    var_need_refresh = true;
	    if (key.touch_x != -1) {
		    drag.dy = std::max(0, key.touch_y - drag.start_y);
		    return true;
	    }
	    drag.grabbed = false; // released: close past the threshold, else settle back
	    drag.closing = drag.dy >= DRAG_CLOSE_PX;
	    drag.from = drag.dy;
	    drag.settle = SETTLE_FRAMES;
	    return true;
    }

    static void round_button(const shell::Rect &r, bool primary, bool pressed, modern::Icon ic) {
	    const ThemeTokens &t = theme_cur();
	    float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
	    if (primary) {
		    modern::circle(cx, cy + 2, r.w / 2, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 2));
	    }
	    u32 fill = primary ? t.acc : t.surf2;
	    modern::circle(cx, cy, r.w / 2, pressed ? theme_blend(fill, primary ? t.acc_ink : t.tx, 0.15f) : fill);
	    // r16: seek symbols at 30 px (arc + arrowhead, "10" in the key size), the play / pause glyph at 24 px
	    modern::icon(ic, cx, cy, primary ? 24 : 30, primary ? t.acc_ink : t.tx);
    }
    // icon + label as one group centered on the pill's real geometry (r16: action text was left-aligned)
    static void pill_label(const shell::Rect &r, modern::Icon ic, const std::string &label, u32 ink) {
	    float w = 14 + 5 + modern::text_w(label, modern::T_META), x = std::floor(r.x + (r.w - w) / 2);
	    modern::icon(ic, x + 7, r.y + r.h / 2, 14, ink);
	    modern::text(label, x + 19, modern::text_y(r.y, r.h, modern::T_META), modern::T_META, ink);
    }
    static void side_pill(const shell::Rect &r, bool pressed, modern::Icon ic, const std::string &label, bool on) {
	    const ThemeTokens &t = theme_cur();
	    modern::pill(r.x, r.y, r.w, r.h, pressed ? t.pressed : on ? t.acc_soft_on_bg : t.surf2);
	    pill_label(r, ic, label, on ? t.acc_tx : t.tx);
    }
    static void action_pill(const shell::Rect &r, bool pressed, modern::Icon ic, const std::string &label) {
	    const ThemeTokens &t = theme_cur();
	    modern::pill(r.x, r.y, r.w, r.h, pressed ? t.pressed : t.surf2);
	    pill_label(r, ic, label, t.tx);
    }

    // two lines on the deck; one ellipsized line while a sheet is open (r16: its second line was cut by the sheet)
    static void draw_title() {
	    std::string title = cur_video_info.title.size() ? cur_video_info.title : LOCALIZED(LOADING);
	    int lines = sheet_shown() ? 1 : 2;
	    if (title != title_cache_src || title_cache_lines != lines) {
		    title_cache_src = title;
		    title_cache_lines = lines;
		    title_cache = modern::wrap(title, 300, modern::T_TITLE, lines);
	    }
	    for (size_t i = 0; i < title_cache.size(); i++) {
		    modern::text(title_cache[i], 10, 7 + i * 16, modern::T_TITLE, theme_cur().tx);
	    }
    }
    // r16 selected mode: the playing video's strip (its own pause / resume, title, time, progress) where the scrubber
    // is while it is the displayed video
    static void draw_now_playing() {
	    const ThemeTokens &t = theme_cur();
	    const float x = 10, y = NOW_Y, w = 300, h = NOW_H;
	    modern::round_rect(x, y, w, h, 10, t.line);
	    modern::round_rect(x + 1, y + 1, w - 2, h - 2, 9, t.surf);
	    const char *status = get_network_waiting_status();
	    float cx = x + 19, cy = y + h / 2;
	    modern::circle(cx, cy, 13, holding == C_NOW_TOGGLE ? theme_blend(t.acc, t.acc_ink, 0.15f) : t.acc);
	    modern::icon(status ? modern::Icon::LOADING : (vid_pausing || vid_pausing_seek) ? modern::Icon::PLAY : modern::Icon::PAUSE,
	                 cx, cy, 14, t.acc_ink);
	    float tx = x + 40, tw = x + w - 8 - tx;
	    std::string title = playing_video_info.title.size() ? playing_video_info.title : LOCALIZED(LOADING);
	    modern::text(modern::ellipsize(title, tw, modern::T_META), tx, y + 2, modern::T_META, t.tx);
	    std::string meta = status ? std::string(status)
	                              : shell::tr("NOW_PLAYING", "Now playing") + " \xC2\xB7 " +
	                                    Util_convert_seconds_to_time(vid_current_pos) + " / " +
	                                    Util_convert_seconds_to_time(vid_duration);
	    modern::text(modern::ellipsize(meta, tw, modern::T_KEY), tx, y + 15, modern::T_KEY,
	                 status == RECOVERY_STATUS_STR ? t.acc_tx : t.tx2);
	    double progress = vid_duration > 0 ? std::max(0.0, std::min(1.0, vid_current_pos / vid_duration)) : 0;
	    modern::rect(tx, y + h - 4, tw, 2, t.surf2);
	    if (progress > 0) {
		    modern::rect(tx, y + h - 4, std::max(2.0f, (float)(tw * progress)), 2, t.acc);
	    }
    }
    static void draw_play_now() {
	    const ThemeTokens &t = theme_cur();
	    shell::Rect r = rect_of(C_PLAY_NOW);
	    bool ready = play_now_ready();
	    u32 fill = ready ? t.acc : t.surf2, ink = ready ? t.acc_ink : t.tx3;
	    if (ready) {
		    modern::pill(r.x, r.y + 2, r.w, r.h, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 2));
	    }
	    modern::pill(r.x, r.y, r.w, r.h, holding == C_PLAY_NOW && ready ? theme_blend(fill, ink, 0.15f) : fill);
	    // r16b: centered on the INK of the group: the play triangle spans 7.5..19.8 of its 24-unit box, so centering the
	    // box left the label ~3 px right of center
	    const float icon_s = 22, k = icon_s / 24, tri_w = (19.8f - 7.5f) * k, gap = 7;
	    std::string label = shell::tr("PLAY_NOW", "Play now");
	    float w = tri_w + gap + modern::text_w(label, modern::T_TITLE), x = std::floor(r.x + (r.w - w) / 2 + 0.5f);
	    modern::icon(modern::Icon::PLAY, x + (12 - 7.5f) * k, r.y + r.h / 2, icon_s, ink);
	    modern::text(label, x + tri_w + gap, modern::text_y(r.y, r.h, modern::T_TITLE), modern::T_TITLE, ink);
    }
    // small_resource_lock held
    void draw() {
	    const ThemeTokens &t = theme_cur();
	    draw_title();
	    // channel row
	    float cx = 10;
	    if (is_local_page()) {
		    modern::icon(modern::Icon::SD, cx + 10, 55, 16, t.tx2);
		    modern::text("Offline copy \xC2\xB7 SD card", cx + 26, modern::text_y(44, 22, modern::T_META), modern::T_META,
		                 t.tx2);
	    } else if (!cur_video_info.author.name.empty()) {
		    if (!thumbnail_draw(main_icon_view->handle, cx, 45, 20, 20)) {
			    modern::circle(cx + 10, 55, 10, t.surf2);
		    }
		    float right = can_subscribe() ? rect_of(C_SUBSCRIBE).x - 8 : 310;
		    std::string name = modern::ellipsize(cur_video_info.author.name, right - (cx + 26), modern::T_META);
		    modern::text(name, cx + 26, modern::text_y(44, 22, modern::T_META), modern::T_META, t.tx);
		    float nx = cx + 26 + modern::text_w(name, modern::T_META) + 6;
		    if (!cur_video_info.author.subscribers.empty() && nx < right - 20) {
			    modern::text(modern::ellipsize(cur_video_info.author.subscribers, right - nx, modern::T_META), nx,
			                 modern::text_y(44, 22, modern::T_META), modern::T_META, t.tx2);
		    }
		    if (can_subscribe()) {
			    shell::Rect r = rect_of(C_SUBSCRIBE);
			    bool subscribed = subscription_is_subscribed(cur_video_info.author.id);
			    u32 fill = subscribed ? t.surf2 : t.sel;
			    modern::pill(r.x, r.y, r.w, r.h, holding == C_SUBSCRIBE ? theme_blend(fill, t.tx, 0.15f) : fill);
			    modern::text_center(subscribe_label(), r.x, r.x + r.w, modern::text_y(r.y, r.h, modern::T_META),
			                        modern::T_META, subscribed ? t.tx : t.sel_ink);
		    }
	    }
	    const char *status = get_network_waiting_status();
	    side_pill(rect_of(C_CC), holding == C_CC, modern::Icon::CC, "CC", section == SEC_CAPTIONS);
	    if (selected_mode()) { // r16: the displayed page is not the playing video
		    draw_now_playing();
		    draw_play_now();
	    } else {
		    Bar::draw_scrubber(10, 70, 300);
		    round_button(rect_of(C_BACK10), false, holding == C_BACK10, modern::Icon::BACK10);
		    round_button(rect_of(C_PLAY), true, holding == C_PLAY,
		                 status                                                  ? modern::Icon::LOADING
		                 : (!vid_play_request || vid_pausing || vid_pausing_seek) ? modern::Icon::PLAY
		                                                                          : modern::Icon::PAUSE);
		    round_button(rect_of(C_FWD10), false, holding == C_FWD10, modern::Icon::FWD10);
	    }
	    side_pill(rect_of(C_QUALITY), holding == C_QUALITY, modern::Icon::TUNE, quality_label(), section == SEC_PLAYBACK);
	    // actions: like (r15: local Liked videos toggle; the count is YouTube's, unchanged), download / library, reload
	    if (like_visible()) {
		    shell::Rect r = rect_of(C_LIKE);
		    bool liked = liked_contains(cur_video_info.id);
		    u32 fill = liked ? t.acc_soft_on_bg : t.surf2, ink = liked ? t.acc_tx : t.tx;
		    modern::pill(r.x, r.y, r.w, r.h, holding == C_LIKE ? t.pressed : fill);
		    // r14: vector icon (the opaque thumb_up.t3x tinted to a square)
		    pill_label(r, liked ? modern::Icon::LIKE_FILLED : modern::Icon::LIKE, like_label(), ink);
	    }
	    action_pill(rect_of(C_DOWNLOAD), holding == C_DOWNLOAD, modern::Icon::DOWNLOAD, download_label());
	    action_pill(rect_of(C_RELOAD), holding == C_RELOAD, modern::Icon::RELOAD, LOCALIZED(RELOAD));
	    // Up next / Comments / Info, then the Controls entry (r16: replaces the always-on hint row)
	    const float bar_w = 3 * TAB_W;
	    modern::pill(10, TAB_Y, bar_w, TAB_H, t.line);
	    modern::pill(11, TAB_Y + 1, bar_w - 2, TAB_H - 2, t.surf);
	    for (int i = 0; i < 3; i++) {
		    shell::Rect r = rect_of(C_TAB0 + i);
		    int sec = i == 0 ? (section == SEC_PLAYLIST ? SEC_PLAYLIST : SEC_UPNEXT) : i == 1 ? SEC_COMMENTS : SEC_INFO;
		    bool on = section == sec || holding == C_TAB0 + i;
		    if (on) {
			    modern::pill(r.x + 2, r.y + 2, r.w - 4, r.h - 4, t.surf2);
		    }
		    modern::text_center(modern::ellipsize(tab_label(i), r.w - 10, modern::T_META), r.x, r.x + r.w,
		                        modern::text_y(r.y, r.h, modern::T_META), modern::T_META, on ? t.tx : t.tx2);
	    }
	    shell::Rect h = rect_of(C_HELP);
	    modern::circle(h.x + h.w / 2, h.y + h.h / 2, h.w / 2, t.line);
	    modern::circle(h.x + h.w / 2, h.y + h.h / 2, h.w / 2 - 1, help_open || holding == C_HELP ? t.surf2 : t.surf);
	    modern::icon(modern::Icon::HELP, h.x + h.w / 2, h.y + h.h / 2, 18, help_open ? t.tx : t.tx2);
    }
    // r16 Controls sheet: every physical shortcut of the player in the browse screens' keycap language
    static void draw_help(float top) {
	    const ThemeTokens &t = theme_cur();
	    shell::draw_sheet_frame(top, shell::tr("CONTROLS", "Controls"), "");
	    const std::vector<std::pair<std::string, std::string>> rows = {
	        {"A", shell::tr("HINT_PLAY", "Play") + " / " + shell::tr("HINT_PAUSE", "Pause")},
	        {"dlr", shell::tr("HELP_SEEK10", "Back / forward 10 s")},
	        {"ZL ZR", shell::tr("HELP_SEEK5", "Back / forward 5 s")},
	        {"X + B", shell::tr("HELP_STOP", "Stop playback")},
	        {"B", shell::tr("HELP_BACK", "Back, or close a sheet")},
	        {"Y", shell::tr("HELP_FULL", "Full player (while browsing)")},
	        {"START", shell::tr("HINT_MENU", "Menu")},
	    };
	    float y = top + 40;
	    for (auto &row : rows) {
		    modern::hints({{row.first, ""}}, 14, y + 2, t.tx, t.kc, t.tx, 0);
		    modern::text(modern::ellipsize(row.second, 310 - 84, modern::T_META), 84, modern::text_y(y, 19, modern::T_META),
		                 modern::T_META, t.tx);
		    y += 20;
	    }
	    modern::pill(16, y + 8, 28, 4, t.line); // the grab handle, as drawn on every sheet
	    modern::text(modern::ellipsize(shell::tr("HELP_DRAG", "Drag the top edge down to close"), 310 - 84, modern::T_META), 84,
	                 modern::text_y(y, 19, modern::T_META), modern::T_META, t.tx2);
	    shell::cur_ground = 0;
    }
    // small_resource_lock held
    void draw_sheet() {
	    if (!sheet_shown()) {
		    return;
	    }
	    const ThemeTokens &t = theme_cur();
	    const float top = std::floor(SHEET_TOP + sheet_offset()), shift = top - SHEET_TOP;
	    if (help_open) {
		    draw_help(top);
		    return;
	    }
	    section = main_view->selected_tab; // a page load may switch it (playlist / general), the header follows
	    std::string count;
	    if (section == SEC_COMMENTS && cur_video_info.comments.size()) {
		    count = std::to_string(cur_video_info.comments.size()) + (cur_video_info.has_more_comments() ? "+" : "");
	    }
	    // r14: the draw layer has no clip: the sheet surface first, the content, then everything above the sheet body
	    // again over rows scrolled up there (the deck strip under the backdrop: ground + title, the only deck element
	    // there; the backdrop; the header band), so the header and its Close pill always stay on top.
	    // r16: all of it follows the drag / settle offset
	    shell::draw_sheet_backdrop(top);
	    modern::round_rect_top(0, top, 320, 240 - top, 16, t.surf);
	    shell::cur_ground = t.surf;
	    main_view->draw(0, shift);
	    modern::rect(0, 0, 320, top + 16, DEFAULT_BACK_COLOR);
	    draw_title();
	    shell::draw_sheet_backdrop(top);
	    modern::round_rect_top(0, top, 320, 240 - CONTENT_Y_HIGH - SHEET_TOP, 16, t.surf);
	    shell::draw_sheet_header(top, section_title(section), count);
	    shell::Rect alt = rect_of(C_SHEET_ALT);
	    if (alt.w > 0) {
		    modern::pill(alt.x, alt.y + shift, alt.w, alt.h, holding == C_SHEET_ALT ? t.pressed : t.surf2);
		    modern::text_center(sheet_alt_label(), alt.x, alt.x + alt.w, modern::text_y(alt.y + shift, alt.h, modern::T_META),
		                        modern::T_META, t.tx);
	    }
	    shell::cur_ground = 0;
    }

    // touch on the deck (no sheet) or on the sheet header; returns the control released this frame (or -1).
    // No lock needed: it only reads geometry and the displayed info (main thread; the async page load replaces
    // cur_video_info under small_resource_lock, so the caller holds it).
    int update_touch(const Hid_info &key) {
	    bool released = key.touch_x == -1 && last_touch_x != -1;
	    int rx = last_touch_x, ry = last_touch_y;
	    last_touch_x = key.touch_x, last_touch_y = key.touch_y;
	    int done = -1;
	    if (key.p_touch) {
		    holding = control_at(key.touch_x, key.touch_y);
		    if (holding != -1) {
			    var_need_refresh = true;
		    }
	    } else if (key.touch_x != -1 && holding != -1 && control_at(key.touch_x, key.touch_y) != holding) {
		    holding = -1;
		    var_need_refresh = true;
	    }
	    if (released) {
		    if (holding != -1 && control_at(rx, ry) == holding) {
			    done = holding;
		    }
		    holding = -1;
		    var_need_refresh = true;
	    }
	    return done;
    }
    // actions that change the displayed page / subscriptions / sections: small_resource_lock held
    void apply_locked(int c) {
	    switch (c) {
	    case C_SUBSCRIBE: {
		    std::string author_id = cur_video_info.author.id;
		    if (subscription_is_subscribed(author_id)) {
			    subscription_unsubscribe(author_id);
		    } else {
			    SubscriptionChannel new_channel;
			    new_channel.id = author_id;
			    new_channel.url = "https://m.youtube.com/channel/" + author_id;
			    new_channel.name = cur_video_info.author.name;
			    new_channel.icon_url = cur_video_info.author.icon_url;
			    new_channel.subscriber_count_str = cur_video_info.author.subscribers;
			    subscription_subscribe(new_channel);
		    }
		    misc_tasks_request(TASK_SAVE_SUBSCRIPTION);
		    Home_update_local_channels();
		    break;
	    }
	    case C_CC:
		    open_section(SEC_CAPTIONS);
		    break;
	    case C_QUALITY:
		    open_section(SEC_PLAYBACK);
		    break;
	    case C_TAB0:
		    open_section(has_playlist() ? SEC_PLAYLIST : SEC_UPNEXT);
		    break;
	    case C_TAB1:
		    open_section(SEC_COMMENTS);
		    break;
	    case C_TAB2:
		    open_section(SEC_INFO);
		    break;
	    case C_SHEET_ALT:
		    open_section(section == SEC_PLAYLIST ? SEC_UPNEXT : SEC_PLAYLIST);
		    break;
	    case C_DOWNLOAD:
		    if (is_local_page()) {
			    global_intent.next_scene = SceneType::DOWNLOADS;
			    global_intent.arg = "";
		    } else {
			    open_section(SEC_INFO); // the download options live in the video's Info page: scroll to them
			    double y = 0;
			    for (auto view : main_tab_view->views) {
				    TextView *label = dynamic_cast<TextView *>(view);
				    if (label && label->text.size() == 1 && (std::string)label->text[0] == "Download to SD card") {
					    main_tab_view->set_offset(y);
					    break;
				    }
				    y += view->get_height() + main_tab_view->margin;
			    }
		    }
		    break;
	    case C_RELOAD: // same as the Playback tab's Reload
		    if (!is_async_task_running(load_video_page)) {
			    seek_at_init_request = vid_current_pos;
			    playback_recovery.user_reset(); // manual reload: fresh runtime recovery budget
			    send_change_video_request_wo_lock(cur_playing_url, true, false, true);
			    video_retry_left = MAX_RETRY_CNT;
		    }
		    break;
	    case C_PLAY:
	    case C_NOW_TOGGLE: // r16: the Now playing strip's pause / resume (same as Play while it is the displayed video)
		    Bar::toggle_play_wo_lock();
		    break;
	    case C_PLAY_NOW: // r16: the displayed video replaces the playing one through the same admission as the idle Play
		    if (selected_mode() && play_now_ready()) {
			    network_waiting_status = NULL;
			    playback_recovery.user_reset(); // explicit play: fresh runtime recovery budget
			    send_change_video_request_wo_lock(cur_displaying_url, true, false, false);
		    }
		    break;
	    case C_HELP:
		    open_help();
		    break;
	    case C_LIKE: { // r15: the displayed page's video (never the one playing, if that is another)
		    liked::LikedVideo entry;
		    std::string message;
		    if (like_visible() && liked::make_entry(cur_video_info.id, cur_video_info.title, cur_video_info.author.name,
		                                            cur_video_info.duration_ms, time(NULL), &entry)) {
			    liked_set(entry, !liked_contains(entry.id), &message);
			    shell::toast(message);
		    }
		    break;
	    }
	    default:
		    break;
	    }
	    var_need_refresh = true;
    }
    } // namespace Deck

    void video_set_linear_filter_enabled(bool enabled) {
	    if (vid_already_init) {
		    for (int i = 0; i < 2; i++) {
			    Draw_c2d_image_set_filter(&vid_image[i], enabled);
		    }
	    }
    }
    void video_set_skip_drawing(bool skip) { video_skip_drawing = skip; }

    // r16: full screen only in the player itself (the setting, or Y from browsing for this visit); browse scenes always
    // keep the status bar over a playing video. The persisted full_screen_mode is never written here.
    static bool video_full_screen_now() {
	    return global_current_scene == SceneType::VIDEO_PLAYER && (var_full_screen_mode || Bar::fullscreen_override);
    }
    bool video_should_draw_top_bar() { return !video_full_screen_now() || !network_decoder.ready; }
    u32 video_get_top_screen_background_color() {
	    return vid_play_request && network_decoder.ready && !audio_only_mode ? DEF_DRAW_BLACK : DEFAULT_BACK_COLOR;
    }
    void video_draw_video_frame() {
	    int texture_index = !texture_index_head;
	    if (vid_play_request && network_decoder.ready && !audio_only_mode) {
		    fps_hooks::render_video_drawn();
		    Draw_texture(vid_image[texture_index].c2d, vid_x, vid_y, vid_tex_width[texture_index] * vid_zoom,
		                 vid_tex_height[texture_index] * vid_zoom);
		    // load_caption() (async thread) deletes and replaces the overlay under this lock on every language change
		    small_resource_lock.lock();
		    caption_overlay_view->cur_timestamp = vid_current_pos;
		    caption_overlay_view->draw();
		    small_resource_lock.unlock();
	    } else { // r14: no frame (the decoder stopped since the caller's check): the OpenTube tile, not the legacy art
		    modern::rect(0, 15, 400, 225, theme_cur().top);
		    modern::logo_tile((400 - 44 * 16 / 11.0f) / 2, 15 + (225 - 44) / 2.0f, 44);
	    }
    }
    void video_draw_top_screen() {
	    // fit to screen size
	    const bool full_screen = video_full_screen_now();
	    vid_zoom = std::min(401.0 / vid_width_org, (full_screen ? 241.0 : 226.0) / vid_height_org);
	    vid_zoom = std::min(10.0, std::max(0.05, vid_zoom));
	    vid_x = (400 - (vid_width_org * vid_zoom)) / 2;
	    vid_y = ((full_screen ? 240 : 225) - (vid_height_org * vid_zoom)) / 2;
	    if (!full_screen) {
		    vid_y += 15;
	    }

	    bool video_shown = vid_play_request && network_decoder.ready && !audio_only_mode;
	    if (!video_shown) { // OpenTube r13: the browse top screen (cued item in ambient light, or the idle wordmark)
		    Draw_screen_ready(0, theme_cur().top);
		    shell::top_draw_browse();
		    logger.draw();
		    if (var_debug_mode) {
			    Draw_debug_info();
		    }
		    return;
	    }
	    Draw_screen_ready(0, video_get_top_screen_background_color());
	    video_draw_video_frame();
	    logger.draw();
	    if (video_should_draw_top_bar()) {
		    shell::status_bar(true, global_current_scene != SceneType::VIDEO_PLAYER
		                                ? shell::screen_title()
		                                : shell::tr("NOW_PLAYING", "Now playing") + " \xC2\xB7 " +
		                                      std::to_string((int)video_p_value) + "p");
	    }
	    if (global_current_scene != SceneType::VIDEO_PLAYER) { // browsing below: the video keeps the top screen
		    shell::full_player_pill(25);
	    }
	    if (var_debug_mode) {
		    Draw_debug_info();
	    }
    }

#ifdef DEF_FPS_DIAG
    // main thread, once per main loop: lock-free state sample for the frame-pacing analyzer (util/fps_diag.hpp)
    void video_fps_diag_tick() {
	    using namespace fps_diag;
	    uint32_t f = 0;
	    if (!(vid_play_request && network_decoder.ready)) {
		    f |= F_NOT_PLAYING;
	    }
	    if (vid_pausing || vid_pausing_seek) {
		    f |= F_PAUSED;
	    }
	    if (vid_seek_request || vid_change_video_request) {
		    f |= F_SEEKING;
	    }
	    if (network_waiting_status) { // player-owned status: reading stream / seeking / recovering
		    f |= F_WAITING;
	    }
	    if (eof_reached) {
		    f |= F_EOF;
	    }
	    if (video_skip_drawing) {
		    f |= F_SKIP_DRAW;
	    }
	    if (should_suspend_decoding || var_app_suspended) {
		    f |= F_SUSPENDED;
	    }
	    if (audio_only_mode) {
		    f |= F_AUDIO_ONLY;
	    }
	    f |= (var_debug_mode ? (uint32_t)M_TOP_DEBUG : 0u) | (var_eco_mode ? (uint32_t)M_ECO : 0u) |
	         (var_full_screen_mode ? (uint32_t)M_FULLSCREEN : 0u) | (var_is_new3ds ? (uint32_t)M_NEW3DS : 0u);
	    const char *scene = global_current_scene == SceneType::VIDEO_PLAYER ? "player"
	                        : global_current_scene == SceneType::HOME       ? "home"
	                        : global_current_scene == SceneType::SEARCH     ? "search"
	                        : global_current_scene == SceneType::CHANNEL    ? "channel"
	                        : global_current_scene == SceneType::HISTORY    ? "history"
	                        : global_current_scene == SceneType::SETTINGS   ? "settings"
	                        : global_current_scene == SceneType::ABOUT      ? "about"
	                                                                        : "other";
	    // same accessor the playing bar calls every drawn frame (player status first, decoder status only if none)
	    fps_hooks::main_tick(f, scene, get_network_waiting_status);
    }
#endif

    // offline session: open the SD files (decode thread) and hand them to the existing decoder pipeline
    static Result_with_string init_local_decoder(const downloads::Playable &p) {
	    Result_with_string result;
	    std::string error;
	    bool separate = p.layout == downloads::Layout::SEPARATE;
	    NetworkStream *video = NULL, *audio = NULL;
	    if (separate && audio_only_mode) {
		    video = audio = NetworkStream::open_local(p.audio_path, p.audio_size, &error);
	    } else {
		    video = audio = NetworkStream::open_local(p.video_path, p.video_size, &error);
		    if (video && separate) {
			    audio = NetworkStream::open_local(p.audio_path, p.audio_size, &error);
			    if (!audio) {
				    delete video; // not handed to the downloader yet: still owned here
				    video = NULL;
			    }
		    }
	    }
	    if (!video) {
		    result.code = DEF_ERR_OTHER;
		    result.string = "Offline copy could not be opened";
		    result.error_description = error;
		    return result;
	    }
	    bool hw = var_is_new3ds && (audio_only_mode || !separate || p.quality == 360 || p.quality == 480);
	    return network_decoder.init_local(video, audio, stream_downloader, hw);
    }
    static void decode_thread(void *arg) {
	    logger.info(DEF_SAPP0_DECODE_THREAD_STR, "Thread started.");
	    freeze_diag::register_thread(freeze_diag::DECODE);
	    freeze_diag::phase("dec:idle");

	    Result_with_string result;
	    int ch = 0;
	    int audio_size = 0;
	    int w = 0;
	    int h = 0;
	    bool key = false;
	    std::string format = "";
	    std::string type = (char *)arg;
	    playback_diag::RecoveryController::Plan pending_recovery; // decided when ERROR is seen, acted on after deinit
	    bool decode_error_noted = false;                          // first decode error per video goes to the diag log
	    bool local_session = false;    // this session plays an SD download (no network, no recovery reload)
	    bool local_read_failed = false; // an SD read failed mid-playback: bounded local error after deinit
	    std::string diag_video_id;
	    TickCounter counter0, counter1;
	    osTickCounterStart(&counter0);
	    osTickCounterStart(&counter1);

	    while (vid_thread_run) {
		    if (vid_play_request || vid_change_video_request) {
			    fps_hooks::dec_play_start();
			    vid_x = 0;
			    vid_y = 15;
			    vid_frametime = 0;
			    vid_framerate = 0;
			    vid_current_pos = 0;
			    vid_duration = 0;
			    vid_zoom = 1;
			    vid_width = 0;
			    vid_height = 0;
			    texture_index_head = 0;
			    vid_video_format = "n/a";
			    vid_audio_format = "n/a";
			    vid_change_video_request = false;
			    vid_seek_request = false;
			    eof_reached = false;
			    vid_play_request = true;
			    pending_recovery = playback_diag::RecoveryController::Plan();
			    decode_error_noted = false;
			    diag_video_id = playing_video_info.id;
			    vid_total_time = 0;
			    vid_total_frames = 0;
			    vid_min_time = 99999999;
			    vid_max_time = 0;
			    vid_recent_total_time = 0;
			    for (int i = 0; i < 90; i++) {
				    vid_recent_time[i] = 0;
			    }

			    for (int i = 0; i < 2; i++) {
				    vid_tex_width[i] = 0;
				    vid_tex_height[i] = 0;
			    }

			    for (int i = 0; i < 320; i++) {
				    vid_time[0][i] = 0;
				    vid_time[1][i] = 0;
			    }

			    vid_audio_time = 0;
			    vid_video_time = 0;
			    vid_copy_time[0] = 0;
			    vid_copy_time[1] = 0;
			    vid_convert_time = 0;

			    // video page parsing sometimes randomly fails, so try several times
			    freeze_diag::phase("dec:init");
			    network_waiting_status = "Reading Stream";
			    local_session = false;
			    local_read_failed = false;
			    downloads::Playable local_playable;
			    std::string local_error;
			    small_resource_lock.lock();
			    if (playing_local.active) {
				    local_session = true;
				    local_playable = playing_local.playable;
				    if (!local_published_lease) { // restarted without a new publish (quality change after a stop)
					    downloads::Playable again;
					    if (downloads_manager().acquire_playback(local_playable.id, &again, &local_error)) {
						    local_published_lease = again.lease;
					    }
				    }
				    local_session_lease = local_published_lease; // released by this thread after deinit
			    }
			    small_resource_lock.unlock();
			    if (local_session) {
				    if (local_session_lease) {
					    result = init_local_decoder(local_playable);
				    } else {
					    result.code = -1;
					    result.string = "Offline copy unavailable";
					    result.error_description = local_error;
				    }
			    } else if (audio_only_mode) {
				    result = network_decoder.init(
				        playing_video_info.audio_stream_url, stream_downloader,
				        playing_video_info.is_livestream ? playing_video_info.stream_fragment_len : -1,
				        playing_video_info.needs_timestamp_adjusting(), var_is_new3ds);
			    } else if (playing_video_info.video_stream_urls[(int)video_p_value] != "" &&
			               playing_video_info.audio_stream_url != "") {
				    result = network_decoder.init(
				        playing_video_info.video_stream_urls[(int)video_p_value], playing_video_info.audio_stream_url,
				        stream_downloader,
				        playing_video_info.is_livestream ? playing_video_info.stream_fragment_len : -1,
				        playing_video_info.needs_timestamp_adjusting(),
				        var_is_new3ds && (video_p_value == 360 || video_p_value == 480));
			    } else if ((video_p_value == 360 || video_p_value == 480) && playing_video_info.both_stream_url != "") {
				    // itag 18 (both_stream) of a long video takes too much time and sometimes leads to a crash
				    result = network_decoder.init(
				        playing_video_info.both_stream_url, stream_downloader,
				        playing_video_info.is_livestream ? playing_video_info.stream_fragment_len : -1,
				        playing_video_info.needs_timestamp_adjusting(), var_is_new3ds);
			    } else {
				    result.code = -1;
				    result.string = "YouTube parser error";
				    result.error_description = "No valid stream url extracted";
			    }

			    logger.info(DEF_SAPP0_DECODE_THREAD_STR,
			                "network_decoder.init()..." + result.string + result.error_description, result.code);
			    if (result.code != 0) {
				    {
					    playback_diag::FailureRecord rec;
					    rec.cause = playback_diag::Cause::INIT_FAILED;
					    rec.ffmpeg_code = (int)result.code;
					    // free linear memory at the failure point (mvdstd work buffer still held): tells a failed
					    // linearAlloc (mvd output frame) apart from a heap malloc with the same error text
					    rec.detail = result.string + result.error_description +
					                 " lin_free=" + std::to_string(linearSpaceFree() / 1024) + "K";
					    playback_diag_record(rec, MAX_RETRY_CNT - video_retry_left, seek_at_init_request, diag_video_id);
					    playback_diag_log.flush();
				    }
				    if (local_session) { // an SD file does not get better by re-reading it: bounded local error
					    Util_err_set_error_message(result.string,
					                               result.error_description +
					                                   "\nOpen Downloads: delete it and download it again.",
					                               DEF_SAPP0_DECODE_THREAD_STR, result.code);
					    Util_err_set_error_show_flag(true);
					    var_need_refresh = true;
				    } else if (video_retry_left > 0) {
					    video_retry_left--;
					    logger.caution("dec", "failed, retrying. retry cnt left:" + std::to_string(video_retry_left));
					    send_change_video_request(cur_playing_url, true, false, true);
				    } else {
					    Util_err_set_error_message(result.string, result.error_description, DEF_SAPP0_DECODE_THREAD_STR,
					                               result.code);
					    Util_err_set_error_show_flag(true);
					    var_need_refresh = true;
				    }
				    vid_play_request = false;
			    }
			    network_waiting_status = NULL;

			    if (vid_play_request) {
				    small_resource_lock.lock();
				    playback_recovery.on_playback_started(cur_playing_url); // does NOT refill the runtime budget
				    small_resource_lock.unlock();
				    {
					    auto tmp = network_decoder.get_audio_info();
					    // bitrate = tmp.bitrate;
					    vid_sample_rate = tmp.sample_rate;
					    ch = tmp.ch;
					    vid_audio_format = tmp.format_name;
					    vid_duration = tmp.duration;
				    }
				    Util_speaker_init(0, ch, vid_sample_rate);
				    {
					    auto tmp = network_decoder.get_video_info();
					    vid_width = vid_width_org = tmp.width;
					    vid_height = vid_height_org = tmp.height;
					    vid_framerate = tmp.framerate;
					    vid_video_format = tmp.format_name;
					    vid_duration = tmp.duration;
					    vid_frametime = 1000.0 / vid_framerate;

					    if (vid_width % 16 != 0) {
						    vid_width += 16 - vid_width % 16;
					    }
					    if (vid_height % 16 != 0) {
						    vid_height += 16 - vid_height % 16;
					    }
				    }
#ifdef DEF_FPS_DIAG
				    fps_hooks::dec_session_meta(vid_framerate, vid_width_org, vid_height_org,
				                                (int)network_decoder.get_decoder_type(), video_p_value, audio_only_mode,
				                                playing_video_info.is_livestream, playing_video_info.player_client);
				    fps_hooks::aud_session(vid_sample_rate, ch, vid_sample_rate); // decoder rate, rate given to ndsp
#endif
			    }

			    if (seek_at_init_request >= 0) {
				    vid_seek_request = true;
				    vid_seek_pos = seek_at_init_request;
				    seek_at_init_request = -1;
			    }

			    osTickCounterUpdate(&counter1);
			    freeze_diag::phase("dec:play");
			    while (vid_play_request && !should_suspend_decoding) {
				    if (vid_seek_request && !vid_change_video_request) {
					    network_waiting_status = "Seeking";
					    Util_speaker_clear_buffer(0);
					    network_decoder_critical_lock.lock(); // the converter thread is now suspended
					    fps_hooks::dec_generation_bump();
					    vid_current_pos = vid_seek_pos;
					    while (vid_seek_request && !vid_change_video_request && vid_play_request) {
						    small_resource_lock.lock();
						    double seek_pos_bak = vid_seek_pos;
						    vid_seek_request = false;
						    network_decoder.interrupt = false;
						    small_resource_lock.unlock();

						    result = network_decoder.seek(seek_pos_bak * 1000 * 1000); // nano seconds
						    // if seek failed because of the lock, it's probably another seek request or other requests
						    // (change video etc...), so we can ignore it
						    if (network_decoder.interrupt) {
							    logger.info(DEF_SAPP0_DECODE_THREAD_STR, "seek interrupted");
							    continue;
						    }
						    logger.info(DEF_SAPP0_DECODE_THREAD_STR,
						                "network_decoder.seek()..." + result.string + result.error_description,
						                result.code);
						    if (result.code != 0) {
							    vid_play_request = false;
						    }
						    break;
					    }
					    fps_hooks::dec_generation_bump();
					    network_decoder_critical_lock.unlock();
					    if (eof_reached) {
						    vid_pausing = false; // if it was stuck at the end of the video, resume playing after seek
					    }
					    network_waiting_status = NULL;
					    var_need_refresh = true;
				    }
				    if (vid_change_video_request || !vid_play_request) {
					    break;
				    }
				    vid_duration = network_decoder.get_duration();

				    auto type = network_decoder.next_decode_type();

				    if (network_decoder.live_failure_pending) { // livestream: behaviour unchanged, evidence only
					    network_decoder.live_failure_pending = false;
					    playback_diag::FailureRecord rec = network_decoder.get_last_failure();
					    rec.detail = "livestream fragment failed; automatic recovery not attempted" +
					                 (rec.detail.empty() ? "" : " " + rec.detail);
					    playback_diag_record(rec, 0, vid_current_pos, diag_video_id);
					    network_decoder.clear_last_failure();
					    playback_diag_log.flush();
				    }
				    if (type == NetworkMultipleDecoder::PacketType::ERROR) {
					    // A needed stream failed (transport / http / demux). The evidence was captured before
					    // any teardown. This is NOT an EOF: autoplay and loop must not trigger. Stop audio and
					    // video coherently now; the recovery decision is acted on after deinit (owner thread).
					    playback_diag::FailureRecord rec = network_decoder.get_last_failure();
					    double fail_pos = vid_current_pos;
					    if (local_session) {
						    local_read_failed = true; // no page reload / network recovery for an SD file
					    } else {
						    small_resource_lock.lock();
						    pending_recovery = playback_recovery.on_runtime_failure(
						        cur_playing_url, fail_pos, network_decoder.get_is_livestream());
						    small_resource_lock.unlock();
					    }
					    playback_diag_record(rec,
					                         pending_recovery.attempt ? pending_recovery.attempt
					                                                  : playback_recovery.attempts_used(),
					                         fail_pos, diag_video_id);
					    logger.error(DEF_SAPP0_DECODE_THREAD_STR,
					                 std::string("stream failure : ") + playback_diag::cause_name(rec.cause) + " [" +
					                     playback_diag::stream_name(rec.stream) +
					                     "] http=" + std::to_string(rec.http_status) +
					                     " ff=" + std::to_string(rec.ffmpeg_code) +
					                     " read_head=" + std::to_string(rec.read_head) + "/" +
					                     std::to_string(rec.len) + " pos=" + std::to_string(fail_pos));
					    playback_diag_log.flush();
					    eof_reached = false;
					    vid_play_request = false;
					    break;
				    }

				    if (type == NetworkMultipleDecoder::PacketType::EoF) {
					    // wait for the audio buffer to be empty
					    if (audio_only_mode) {
						    while (!Util_speaker_is_buffer_empty(0) && vid_play_request && !vid_seek_request &&
						           !vid_change_video_request) {
							    usleep(100000);
						    }
					    }
					    vid_pausing = true;
					    bool break_flag = false;
					    if (!eof_reached) { // the first time it reaches EOF
						    small_resource_lock.lock();
						    if (var_loop_mode == false) {
							    if ((var_autoplay_level == 2 && playing_video_info.has_next_video()) ||
							        (var_autoplay_level == 1 && playing_video_info.has_next_video_in_playlist())) {

								    send_change_video_request_wo_lock(playing_video_info.get_next_video().url, true,
								                                      false, false);
								    break_flag = true;
							    }
						    } else if (var_loop_mode == true) {
							    vid_seek_request = true;
							    vid_seek_pos = 0;
						    }
						    small_resource_lock.unlock();
					    }
					    eof_reached = true;
					    if (break_flag) {
						    break;
					    }
					    usleep(10000);
					    continue;
				    } else {
					    eof_reached = false;
				    }

				    if (type == NetworkMultipleDecoder::PacketType::AUDIO) {
					    double pos;
					    u8 *audio = NULL;
					    osTickCounterUpdate(&counter0);
					    result = network_decoder.decode_audio(&audio_size, &audio, &pos);
					    osTickCounterUpdate(&counter0);
					    vid_audio_time = osTickCounterRead(&counter0);
					    fps_hooks::aud_decoded(result.code, vid_audio_time);

					    if (result.code == 0) {
						    fps_hooks::aud_add_begin(audio, audio_size, ch);
						    while (true) {
							    result = Util_speaker_add_buffer(0, ch, audio, audio_size, pos);
							    if (result.code == 0 || !vid_play_request || vid_seek_request ||
							        vid_change_video_request) {
								    break;
							    }
							    // Util_log_save(DEF_SAPP0_DECODE_THREAD_STR, "audio queue full");
							    fps_hooks::aud_add_retry(result.code);

							    usleep(10000);
						    }
						    if (result.code == 0) {
							    fps_hooks::aud_added(audio_size);
						    }
						    free(audio);
						    audio = NULL;
					    } else if (result.code != DEF_ERR_NEED_MORE_INPUT) { // ignore NEED_MORE_INPUT error
						    logger.error(DEF_SAPP0_DECODE_THREAD_STR,
						                 "Util_audio_decoder_decode()..." + result.string + result.error_description,
						                 result.code);
						    if (!decode_error_noted) { // once per video, informational
							    decode_error_noted = true;
							    playback_diag::FailureRecord rec;
							    rec.cause = playback_diag::Cause::DECODE_ERROR;
							    rec.stream = playback_diag::StreamId::AUDIO;
							    rec.ffmpeg_code = (int)result.code;
							    rec.detail = result.error_description;
							    playback_diag_record(rec, playback_recovery.attempts_used(), vid_current_pos,
							                         diag_video_id);
							    playback_diag_log.flush();
						    }
					    }
				    } else if (type == NetworkMultipleDecoder::PacketType::VIDEO) {
					    osTickCounterUpdate(&counter0);
					    result = network_decoder.decode_video(&w, &h, &key);
					    osTickCounterUpdate(&counter0);

					    while (result.code == DEF_ERR_NEED_MORE_OUTPUT && vid_play_request && !vid_seek_request &&
					           !vid_change_video_request) {
						    fps_hooks::dec_backpressure_wait();
						    usleep(10000);
						    osTickCounterUpdate(&counter0);
						    result = network_decoder.decode_video(&w, &h, &key);
						    osTickCounterUpdate(&counter0);
					    }
					    vid_video_time = osTickCounterRead(&counter0);
					    fps_hooks::dec_video_decoded(result.code == 0, vid_video_time);

					    // get the elapsed time from the previous frame
					    osTickCounterUpdate(&counter1);
					    double cur_frame_internval = osTickCounterRead(&counter1);

					    vid_min_time = std::min(vid_min_time, cur_frame_internval);
					    vid_max_time = std::max(vid_max_time, cur_frame_internval);
					    vid_total_time += cur_frame_internval;
					    vid_total_frames++;
					    vid_recent_time[89] = cur_frame_internval;
					    vid_recent_total_time = std::accumulate(vid_recent_time, vid_recent_time + 90, 0.0);
					    for (int i = 1; i < 90; i++) {
						    vid_recent_time[i - 1] = vid_recent_time[i];
					    }

					    vid_time[0][319] = cur_frame_internval;
					    for (int i = 1; i < 320; i++) {
						    vid_time[0][i - 1] = vid_time[0][i];
					    }

					    if (vid_play_request && !vid_seek_request && !vid_change_video_request) {
						    if (result.code != 0 && result.code != DEF_ERR_NEED_MORE_INPUT) {
							    logger.error(DEF_SAPP0_DECODE_THREAD_STR,
							                 "Util_video_decoder_decode()..." + result.string +
							                     result.error_description,
							                 result.code);
							    if (!decode_error_noted) { // once per video, informational
								    decode_error_noted = true;
								    playback_diag::FailureRecord rec;
								    rec.cause = playback_diag::Cause::DECODE_ERROR;
								    rec.stream = playback_diag::StreamId::VIDEO;
								    rec.ffmpeg_code = (int)result.code;
								    rec.detail = result.error_description;
								    playback_diag_record(rec, playback_recovery.attempts_used(), vid_current_pos,
								                         diag_video_id);
								    playback_diag_log.flush();
							    }
						    }
					    }
				    } else if (type == NetworkMultipleDecoder::PacketType::INTERRUPTED) {
					    continue;
				    } else {
					    logger.error(DEF_SAPP0_DECODE_THREAD_STR, "unknown type of packet");
				    }
			    }

			    network_waiting_status = NULL;
			    fps_hooks::dec_generation_bump(); // play loop left (stop / error / change / suspend)

			    freeze_diag::phase("dec:stop_audio");
			    while (Util_speaker_is_playing(0) && vid_play_request) {
				    usleep(10000);
			    }
			    Util_speaker_exit(0);

			    if (!vid_change_video_request) {
				    vid_play_request = false;
			    }

			    // make sure the convert thread stops before closing network_decoder
			    freeze_diag::phase("dec:deinit");
			    network_decoder_critical_lock.lock(); // the converter thread is now suspended
			    network_decoder.deinit();
			    network_decoder_critical_lock.unlock();
			    freeze_diag::phase("dec:after_deinit");

			    var_need_refresh = true;
			    vid_pausing = false;
			    vid_seek_request = false;
			    logger.info(DEF_SAPP0_DECODE_THREAD_STR, "deinit complete");

			    // the SD files are closed now (NetworkDecoderFFmpegIOData::deinit / a failed init close local streams on
			    // this thread; the downloader only frees the objects later): give back the lease unless the same
			    // published item is re-initialised right away (quality change / reload)
			    small_resource_lock.lock();
			    if (local_session_lease) {
				    bool keep = local_session_lease == local_published_lease && (vid_play_request || vid_change_video_request);
				    if (!keep) {
					    downloads_manager().release_playback(local_session_lease);
					    if (local_session_lease == local_published_lease) {
						    local_published_lease = 0;
					    }
				    }
				    local_session_lease = 0;
			    }
			    small_resource_lock.unlock();
			    if (local_read_failed) {
				    local_read_failed = false;
				    Util_err_set_error_message("Offline playback stopped",
				                               "The downloaded file could not be read (SD card removed or file "
				                               "damaged).\nOpen Downloads: delete it and download it again.",
				                               DEF_SAPP0_DECODE_THREAD_STR);
				    Util_err_set_error_show_flag(true);
				    var_need_refresh = true;
			    }

			    if (pending_recovery.action != playback_diag::RecoveryController::Action::NONE) {
				    using Action = playback_diag::RecoveryController::Action;
				    playback_diag::RecoveryController::Plan plan = pending_recovery;
				    pending_recovery = playback_diag::RecoveryController::Plan();
				    if (plan.action == Action::RETRY) {
					    // Controlled restart on the decode thread using the existing Playback > Reload path
					    // (refresh video info + restart at the last position). Backoff instead of a tight loop;
					    // a new video / seek / play / reload / exit bumps the generation and fences this plan.
					    network_waiting_status = RECOVERY_STATUS_STR;
					    var_need_refresh = true;
					    logger.caution(DEF_SAPP0_DECODE_THREAD_STR,
					                   "recovering playback : attempt " + std::to_string(plan.attempt) + "/" +
					                       std::to_string(playback_diag::RecoveryController::MAX_ATTEMPTS) + " in " +
					                       std::to_string(plan.backoff_ms) + " ms at " +
					                       std::to_string(plan.resume_pos) + " s");
					    auto stale = [&]() {
						    return !vid_thread_run || !playback_recovery.is_current(plan.generation) ||
						           vid_change_video_request || vid_play_request;
					    };
					    int waited_ms = 0;
					    freeze_diag::phase("dec:recovery_backoff");
					    while (waited_ms < plan.backoff_ms && !stale()) {
						    usleep(50000);
						    if (!should_suspend_decoding) {
							    waited_ms += 50;
						    }
					    }
					    network_waiting_status = NULL; // cleared before the reload so the UI can never get stuck
					    bool issued = false;
					    if (!stale()) {
						    small_resource_lock.lock();
						    if (!stale()) {
							    seek_at_init_request = plan.resume_pos;
							    // NOTE: video_retry_left (startup budget) is intentionally NOT refilled here
							    send_change_video_request_wo_lock(cur_playing_url, true, false, true, plan.generation);
							    issued = true;
						    }
						    small_resource_lock.unlock();
					    }
					    logger.info(DEF_SAPP0_DECODE_THREAD_STR,
					                issued ? "recovery reload issued" : "recovery cancelled (stale)");
					    var_need_refresh = true;
				    } else if (plan.action == Action::GIVE_UP || plan.action == Action::UNSUPPORTED) {
					    // Recoverable error surfaced to the user; manual action required, never autoplay.
					    std::string description =
					        plan.action == Action::GIVE_UP
					            ? "Stream failed again after " +
					                  std::to_string(playback_diag::RecoveryController::MAX_ATTEMPTS) +
					                  " recovery attempts. Use Playback > Reload to retry."
					            : "Livestream failed; automatic recovery is disabled for live content. Reload manually.";
					    Util_err_set_error_message("Playback stopped (recoverable)", description,
					                               DEF_SAPP0_DECODE_THREAD_STR);
					    Util_err_set_error_show_flag(true);
					    var_need_refresh = true;
					    playback_diag_log.flush();
				    }
			    }
		    } else {
			    usleep(DEF_ACTIVE_THREAD_SLEEP_TIME);
		    }
		    // r15b: idle (every stream of the last session closed): the playback admission ends unless a request is
		    // pending, read under the gate lock (util/activity_gate.hpp)
		    activity_gate().playback_idle([]() { return !vid_play_request && !vid_change_video_request; });
		    freeze_diag::phase("dec:idle");

		    while (vid_thread_run && (should_suspend_decoding ||
		                              (vid_thread_suspend && !vid_play_request && !vid_change_video_request))) {
			    usleep(DEF_INACTIVE_THREAD_SLEEP_TIME);
		    }
	    }

	    logger.info(DEF_SAPP0_DECODE_THREAD_STR, "Thread exit.");
	    threadExit(0);
    }

    static void convert_thread(void *arg) {
	    logger.info(DEF_SAPP0_CONVERT_THREAD_STR, "Thread started.");
	    u8 *yuv_video = NULL;
	    u8 *video = NULL;
	    TickCounter counter0, counter1;
	    Result_with_string result;

	    osTickCounterStart(&counter0);
	    freeze_diag::register_thread(freeze_diag::CONVERT);
	    freeze_diag::phase("conv:idle");

	    y2rInit();

	    while (vid_thread_run) {
		    if (vid_play_request && !vid_seek_request && !vid_change_video_request) {
			    network_decoder_critical_lock.lock();
			    freeze_diag::phase("conv:frames");
			    while (vid_play_request && !vid_seek_request && !vid_change_video_request) {
				    double pts;
				    fps_hooks::conv_get_begin();
				    do {
					    osTickCounterUpdate(&counter1);
					    osTickCounterUpdate(&counter0);
					    result = network_decoder.get_decoded_video_frame(
					        vid_width, vid_height, network_decoder.hw_decoder_enabled ? &video : &yuv_video, &pts);
					    osTickCounterUpdate(&counter0);
					    if (result.code != DEF_ERR_NEED_MORE_INPUT) {
						    break;
					    }
					    fps_hooks::conv_get_retry(vid_pausing || vid_pausing_seek);
					    if (vid_pausing || vid_pausing_seek) {
						    usleep(10000);
					    } else {
						    usleep(3000);
					    }
				    } while (vid_play_request && !vid_seek_request && !vid_change_video_request && !audio_only_mode);

				    if (audio_only_mode) {
					    while (audio_only_mode && vid_play_request && !vid_seek_request && !vid_change_video_request) {
						    double tmp = Util_speaker_get_current_timestamp(0, vid_sample_rate);
						    if (tmp != -1) {
							    vid_current_pos = tmp;
						    }
						    usleep(50000);
					    }
					    break;
				    }
				    if (!vid_play_request || vid_seek_request || vid_change_video_request) {
					    break;
				    }
				    if (result.code != 0) { // this is an unexpected error
					    logger.error(DEF_SAPP0_CONVERT_THREAD_STR,
					                 "failure getting decoded result" + result.string + result.error_description,
					                 result.code);
					    vid_play_request = false;
					    break;
				    }
				    fps_hooks::conv_frame_got();

				    bool video_need_free = false;
				    vid_copy_time[0] = osTickCounterRead(&counter0);

				    osTickCounterUpdate(&counter0);
				    if (!network_decoder.hw_decoder_enabled) {
					    result = Util_converter_y2r_yuv420p_to_bgr565(yuv_video, &video, vid_width, vid_height, false);
					    video_need_free = true;
				    }
				    osTickCounterUpdate(&counter0);
				    vid_convert_time = osTickCounterRead(&counter0);
				    fps_hooks::conv_converted(vid_convert_time);

				    double cur_convert_time = 0;

				    if (result.code == 0) {
					    // we don't want to include the sleep time in the performance profiling
					    osTickCounterUpdate(&counter0);
					    vid_copy_time[1] = osTickCounterRead(&counter0);
					    osTickCounterUpdate(&counter1);
					    cur_convert_time = osTickCounterRead(&counter1);

					    // sync with sound
					    fps_hooks::conv_sync_begin();
					    double cur_sound_pos = Util_speaker_get_current_timestamp(0, vid_sample_rate);
					    if (cur_sound_pos >= 0) { // if sound is not playing, probably because the video is lagging
						                          // behind, draw immediately
						    while (pts - cur_sound_pos > 0.003 && vid_play_request && !vid_seek_request &&
						           !vid_change_video_request) {
							    double sleep_microseconds =
							        std::min(0.1, (pts - cur_sound_pos - 0.0015) / network_decoder.get_tempo()) *
							        1000000;
							    usleep(sleep_microseconds);
							    cur_sound_pos = Util_speaker_get_current_timestamp(0, vid_sample_rate);
							    if (cur_sound_pos < 0) {
								    break;
							    }
						    }
					    }
					    fps_hooks::conv_sync_end(pts, cur_sound_pos);
					    vid_current_pos = pts;

					    osTickCounterUpdate(&counter0);
					    osTickCounterUpdate(&counter1);

					    if (!video_skip_drawing) {
						    vid_tex_width[texture_index_head] = vid_width_org;
						    vid_tex_height[texture_index_head] = vid_height_org;
						    result = Draw_set_texture_data(&vid_image[texture_index_head], video, vid_width,
						                                   vid_height_org, 1024, 1024, GPU_RGB565);
						    if (result.code != 0) {
							    logger.error(DEF_SAPP0_CONVERT_THREAD_STR,
							                 "Draw_set_texture_data()..." + result.string + result.error_description,
							                 result.code);
						    }
						    texture_index_head = !texture_index_head;
						    fps_hooks::conv_published(pts, network_decoder.get_tempo());
					    } else {
						    fps_hooks::conv_skipped_draw();
					    }

					    osTickCounterUpdate(&counter0);
					    vid_copy_time[1] += osTickCounterRead(&counter0);

					    var_need_refresh = true;
				    } else {
					    logger.error(DEF_SAPP0_CONVERT_THREAD_STR,
					                 "Util_converter_yuv420p_to_bgr565()..." + result.string + result.error_description,
					                 result.code);
				    }

				    if (video_need_free) {
					    free(video);
				    }
				    video = NULL;
				    yuv_video = NULL; // this is the result of network_decoder.get_decoded_video_frame(), so it should
				                      // not be freed

				    osTickCounterUpdate(&counter1);
				    cur_convert_time += osTickCounterRead(&counter1);
				    vid_time[1][319] = cur_convert_time;
				    for (int i = 1; i < 320; i++) {
					    vid_time[1][i - 1] = vid_time[1][i];
				    }
			    }
			    network_decoder_critical_lock.unlock();
			    freeze_diag::phase("conv:idle");
		    } else {
			    usleep(DEF_ACTIVE_THREAD_SLEEP_TIME);
		    }

		    while (vid_thread_run && (should_suspend_decoding ||
		                              (vid_thread_suspend && !vid_play_request && !vid_change_video_request))) {
			    usleep(DEF_INACTIVE_THREAD_SLEEP_TIME);
		    }
	    }

	    y2rExit();

	    logger.info(DEF_SAPP0_CONVERT_THREAD_STR, "Thread exit.");
	    threadExit(0);
    }

    void video_set_show_debug_info(bool show) {
	    if (vid_already_init) {
		    small_resource_lock.lock();
		    if (show && playback_tab_view->views.back() != debug_info_view) {
			    playback_tab_view->views.push_back(debug_info_view);
		    }
		    if (!show && playback_tab_view->views.back() == debug_info_view) {
			    playback_tab_view->views.pop_back();
		    }
		    small_resource_lock.unlock();
	    }
    }

    void VideoPlayer_draw(void) {
	    Hid_info key;
	    Util_hid_query_key_state(&key);

	    thumbnail_set_active_scene(SceneType::VIDEO_PLAYER);

	    bool video_playing_bar_show = video_is_playing();
	    (void)video_playing_bar_show; // the deck's scrubber is always shown here

	    // top screen while nothing plays: the displayed video as the hero (online thumbnail only; offline copies
	    // never trigger a network request)
	    small_resource_lock.lock();
	    {
		    shell::Preview p;
		    p.kind = shell::Preview::Kind::VIDEO;
		    p.title = cur_video_info.title;
		    p.channel = cur_video_info.author.name;
		    p.meta = cur_video_info.views_str + (cur_video_info.publish_date.size() && cur_video_info.views_str.size() ? " \xC2\xB7 " : "") +
		             cur_video_info.publish_date;
		    if (!is_local_video_url(cur_displaying_url) && !cur_video_info.id.empty()) {
			    p.thumbnail_url = youtube_get_video_thumbnail_url_by_id(cur_video_info.id);
		    }
		    if (p.title.empty()) {
			    shell::preview_clear();
		    } else {
			    shell::preview_set(p);
		    }
	    }
	    small_resource_lock.unlock();
	    shell::set_top_hints({{"A", vid_play_request ? shell::tr("HINT_PAUSE", "Pause") : shell::tr("HINT_PLAY", "Play")},
	                          {"B", shell::tr("HINT_BACK", "Back")}});

	    if (var_need_refresh || !var_eco_mode) {
		    var_need_refresh = false;
		    freeze_diag::phase("vp:draw_top");
		    Draw_frame_ready();
		    video_draw_top_screen();

		    Draw_screen_ready(1, DEFAULT_BACK_COLOR);

		    freeze_diag::phase("vp:draw_bottom");
		    small_resource_lock.lock();
		    Deck::draw();
		    Deck::draw_sheet();
		    draw_overlay_menu(0);
		    equalizer_popup_view->draw();
		    small_resource_lock.unlock();

		    if (Util_expl_query_show_flag()) {
			    Util_expl_draw();
		    }

		    if (Util_err_query_error_show_flag()) {
			    Util_err_draw();
		    }

		    Draw_touch_pos();

		    freeze_diag::phase("vp:present");
		    Draw_apply_draw();
	    } else {
		    freeze_diag::phase("vp:vblank");
		    gspWaitForVBlank();
	    }
	    freeze_diag::phase("vp:update");

	    if (Util_err_query_error_show_flag()) {
		    Util_err_main(key);
	    } else if (Util_expl_query_show_flag()) {
		    Util_expl_main(key);
	    } else {
		    /* ****************************** LOCK START ******************************  */
		    small_resource_lock.lock();

		    // thumbnail request update (this should be done while `small_resource_lock` is locked)
		    // YES, this section needs refactoring, seriously
		    if (cur_video_info.comments.size()) { // comments
			    std::vector<std::pair<float, PostView *>>
			        comments_list; // list of comment views whose author's thumbnails should be loaded
			    {
				    constexpr int LOW = -800;
				    constexpr int HIGH = 1040;
				    float cur_y = -comment_tab_view->get_offset();
				    for (size_t i = 0; i < comments_main_view->views.size(); i++) {
					    float cur_height = comments_main_view->views[i]->get_height();
					    if (cur_y < HIGH && cur_y + cur_height >= LOW) {
						    auto parent_comment_view = dynamic_cast<PostView *>(comments_main_view->views[i]);
						    if (cur_y + parent_comment_view->get_self_height() >= LOW) {
							    comments_list.push_back({cur_y, parent_comment_view});
						    }
						    auto list = parent_comment_view->get_reply_pos_list(); // {y offset, reply view}
						    for (auto j : list) {
							    float cur_reply_height = j.second->get_height();
							    if (cur_y + j.first < HIGH && cur_y + j.first + cur_reply_height > LOW) {
								    comments_list.push_back({cur_y + j.first, j.second});
							    }
						    }
					    }
					    cur_y += cur_height;
				    }
				    if (comments_list.size() > MAX_COMMENT_ICON_LOAD_REQUEST) {
					    int leftover = comments_list.size() - MAX_COMMENT_ICON_LOAD_REQUEST;
					    comments_list.erase(comments_list.begin(), comments_list.begin() + leftover / 2);
					    comments_list.erase(comments_list.end() - (leftover - leftover / 2), comments_list.end());
				    }
			    }

			    std::set<PostView *> newly_loading_views, cancelling_views;
			    for (auto i : comment_thumbnail_loaded_list) {
				    cancelling_views.insert(i);
			    }
			    for (auto i : comments_list) {
				    newly_loading_views.insert(i.second);
			    }
			    for (auto i : comment_thumbnail_loaded_list) {
				    newly_loading_views.erase(i);
			    }
			    for (auto i : comments_list) {
				    cancelling_views.erase(i.second);
			    }

			    for (auto i : cancelling_views) {
				    thumbnail_cancel_request(i->author_icon_handle);
				    i->author_icon_handle = -1;
				    comment_thumbnail_loaded_list.erase(i);
			    }
			    for (auto i : newly_loading_views) {
				    i->author_icon_handle =
				        thumbnail_request(i->author_icon_url, SceneType::VIDEO_PLAYER, 0, ThumbnailType::ICON);
				    comment_thumbnail_loaded_list.insert(i);
			    }

			    std::vector<std::pair<int, int>> priority_list;
			    auto priority = [&](float y) {
				    if (y < 0) {
					    return 500 + y / 100;
				    }
				    if (y < 240) {
					    return PRIORITY_FOREGROUND + y;
				    }
				    return 500 + (240 - y) / 100;
			    };
			    for (auto i : comments_list) {
				    priority_list.push_back({i.second->author_icon_handle, priority(i.first)});
			    }
			    thumbnail_set_priorities(priority_list);
		    }

		    bool taken = update_overlay_menu(&key);
		    if (Deck::swallow_touch) { // r16: the finger that was on a sheet when it settled: nothing under it reacts
			    if (key.touch_x == -1) {
				    Deck::swallow_touch = false;
			    } else {
				    key.touch_x = key.touch_y = -1, key.p_touch = key.h_touch = false;
			    }
		    }

		    bool stop_chord = (key.h_x && key.p_b) || (key.h_b && key.p_x);
		    // equalizer: B alone closes it when B is released; X joining makes it the X+B stop chord (either order)
		    static bool eq_b_held = false;
		    if (taken || !equalizer_popup_view->is_visible) {
			    eq_b_held = false;
		    }
		    if (taken) { // the shell owns this frame: pending touches end without their action
			    Deck::cancel_gesture();
			    equalizer_popup_view->reset_holding_status();
		    } else if (equalizer_popup_view->is_visible) {
			    Hid_info eq_key = key;
			    eq_key.p_b = false; // B is handled here, not by the overlay
			    equalizer_popup_view->update(eq_key);
			    if (key.p_b && !key.h_x) {
				    eq_b_held = true;
			    }
			    if (eq_b_held && (key.h_x || key.p_x)) {
				    eq_b_held = false; // the stop chord, not a close
			    } else if (eq_b_held && !key.h_b) {
				    eq_b_held = false;
				    equalizer_popup_view->on_cancel_func(*equalizer_popup_view);
			    }
			    if (!stop_chord) {
				    key.p_b = false; // B belongs to the equalizer: no Back
			    }
			    key.touch_x = key.touch_y = -1, key.p_touch = false; // nothing under the equalizer sees the touch
		    } else if (Deck::sheet_shown()) {
			    int alt = Deck::update_touch(key);
			    if (!stop_chord && key.p_b) {
				    Deck::close_section(); // B closes (also mid-drag)
				    key.p_b = false;       // B closed the sheet: no scene change
			    } else if (Deck::update_drag(key)) {
				    // r16: grabbed at the header band or settling: the sheet owns the touch (no row, pill or backdrop)
			    } else if (alt == Deck::C_SHEET_ALT) {
				    Deck::apply_locked(alt);
			    } else if (!stop_chord && shell::sheet_should_close(key, Deck::SHEET_TOP)) {
				    Deck::close_section();
				    key.p_b = false; // B closed the sheet: no scene change
			    } else if (Deck::holding == -1 && Deck::section >= 0) {
				    main_view->update(key);
			    }
			    key.touch_x = key.touch_y = -1, key.p_touch = false; // nothing under the sheet sees the touch
		    } else {
			    int c = Deck::update_touch(key);
			    if (c != -1 && c != Deck::C_BACK10 && c != Deck::C_FWD10) {
				    Deck::apply_locked(c);
			    } else if (c == Deck::C_BACK10 || c == Deck::C_FWD10) {
				    key.p_d_left = c == Deck::C_BACK10, key.p_d_right = c == Deck::C_FWD10; // same as the D-pad
			    }
		    }

		    if (channel_id_pressed != "") {
			    global_intent.next_scene = SceneType::CHANNEL;
			    global_intent.arg = channel_id_pressed;
			    channel_id_pressed = "";
		    }
		    if (suggestion_clicked_url != "") {
			    global_intent.next_scene = SceneType::VIDEO_PLAYER;
			    global_intent.arg = suggestion_clicked_url;
			    suggestion_clicked_url = "";
		    }

		    small_resource_lock.unlock();
		    /* ****************************** LOCK END ******************************  */

		    if (taken || (Bar::bar_grabbed && Deck::selected_mode())) {
			    Bar::cancel_gestures(); // r16: no scrubber in selected mode (the Now playing strip is there)
		    } else if (!Deck::sheet_shown() && !equalizer_popup_view->is_visible && Deck::holding == -1 &&
		               !Deck::selected_mode()) {
			    Bar::update_scrubber(key);
		    }
		    if (key.p_a && !vid_play_request) { // r13: A also starts the displayed video (same as its Play button)
			    small_resource_lock.lock();
			    Bar::toggle_play_wo_lock();
			    small_resource_lock.unlock();
		    } else if (key.p_a) {
			    if (vid_play_request) {
				    if (vid_pausing) {
					    if (!vid_pausing_seek) {
						    Util_speaker_resume(0);
					    }
					    if (eof_reached) {
						    send_seek_request(0);
					    }
					    vid_pausing = false;
				    } else {
					    if (!vid_pausing_seek) {
						    Util_speaker_pause(0);
					    }
					    vid_pausing = true;
				    }
			    }
		    } else if ((key.h_x && key.p_b) || (key.h_b && key.p_x)) {
			    vid_play_request = false;
		    } else if (key.p_b) {
			    global_intent.next_scene = SceneType::BACK;
		    } else if (key.p_d_right || key.p_d_left) {
			    if (network_decoder.ready) {
				    double pos = vid_current_pos;
				    pos += key.p_d_right ? 10 : -10;
				    pos = std::max<double>(0, pos);
				    pos = std::min<double>(vid_duration, pos);
				    send_seek_request(pos);
			    }
		    } else if (key.p_zr || key.p_zl) {
			    if (network_decoder.ready) {
				    double pos = vid_current_pos;
				    pos += key.p_zr ? 5 : -5;
				    pos = std::max<double>(0, pos);
				    pos = std::min<double>(vid_duration, pos);
				    send_seek_request(pos);
			    }
		    }
	    }
    }
