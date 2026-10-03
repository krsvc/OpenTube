#include "headers.hpp"
#include <map>
#include <string>
#include <vector>

#include "scenes/liked_videos.hpp"
#include "scenes/video_player.hpp"
#include "youtube_parser/parser.hpp"
#include "ui/ui.hpp"
#include "ui/overlay.hpp"
#include "ui/modern.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "data_io/liked_videos_app.hpp"
#include "downloads/downloads_app.hpp"

// OpenTube r15 (OPENTUBE_R15_PLAN.md A): You > Liked videos. Local favorites only, newest first. Built like Watch
// history (same rows, cue, sheet); rows are rebuilt when the store changes (a like in the player, a remove here, a
// failed save reverted) or the Wi-Fi state changes (thumbnail source / offline labels).

#define MAX_THUMBNAIL_LOAD_REQUEST 12

namespace LikedVideos {
bool already_init = false;
int CONTENT_Y_HIGH = 240;
constexpr int HEADER_H = 34;

std::vector<liked::LikedVideo> items;
std::map<const View *, std::string> row_ids; // cued row -> video id
std::map<std::string, std::string> ready_copies; // video id -> READY download item id
unsigned shown_generation = (unsigned)-1;
bool shown_online = false;
std::string open_request, remove_candidate, remove_request;

OverlayView *remove_sheet;
ScrollView *main_view = NULL;
VerticalListView *list_view = NULL;
}; // namespace LikedVideos
using namespace LikedVideos;

static bool online() { return var_wifi_state == 2; }

static void load_ready_copies() {
	ready_copies.clear();
	if (!downloads_running()) {
		return;
	}
	std::map<std::string, int> best;
	for (auto &item : downloads_manager().snapshot()) {
		if (item.state == downloads::ItemState::READY && item.meta_ok && item.owned) {
			auto it = best.find(item.meta.video_id);
			if (it == best.end() || item.meta.quality > it->second) {
				best[item.meta.video_id] = item.meta.quality;
				ready_copies[item.meta.video_id] = item.id;
			}
		}
	}
}

static void open_remove_sheet(const std::string &id) {
	remove_candidate = id;
	remove_sheet->recursive_delete_subviews();
	remove_sheet
	    ->set_subview((new TextView(0, 0, 300, 34))
	                      ->set_text((std::function<std::string()>)[]() {
		                      return shell::tr("REMOVE_LIKED_ITEM", "Remove from Liked videos");
	                      })
	                      ->set_x_alignment(TextView::XAlign::CENTER)
	                      ->set_y_alignment(TextView::YAlign::CENTER)
	                      ->set_text_offset(0, -1)
	                      ->set_on_view_released([](View &) {
		                      remove_request = remove_candidate;
		                      remove_candidate = "";
		                      main_view->reset_holding_status();
		                      remove_sheet->set_is_visible(false);
		                      var_need_refresh = true;
	                      })
	                      ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
	                      ->set_corner_radius(-1))
	    ->set_on_cancel([](OverlayView &view) {
		    remove_candidate = "";
		    main_view->reset_holding_status();
		    view.set_is_visible(false);
		    var_need_refresh = true;
	    })
	    ->set_is_visible(true);
	var_need_refresh = true;
}

static void rebuild() {
	items = liked_snapshot(&shown_generation);
	shown_online = online();
	load_ready_copies();
	row_ids.clear();
	shell::cue_set(nullptr);
	if (main_view) {
		main_view->recursive_delete_subviews();
	}
	delete main_view;

	list_view = (new VerticalListView(0, 0, 320))
	                ->set_margin(SMALL_MARGIN)
	                ->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::LIKED);
	for (auto &v : items) {
		auto copy = ready_copies.find(v.id);
		bool has_copy = copy != ready_copies.end();
		std::string aux = has_copy ? shell::tr("LIKED_ON_SD", "On SD card")
		                  : shown_online ? ""
		                                 : shell::tr("LIKED_NEEDS_INTERNET", "Needs internet");
		// thumbnail: the download's saved one, else YouTube's by id when connected, else the neutral tile (no request)
		std::string thumb;
		if (has_copy) {
			downloads::ItemView item;
			if (downloads_manager().find(copy->second, &item) && item.thumb) {
				thumb = Downloads_thumbnail_key(copy->second, false);
			}
		}
		if (thumb.empty() && shown_online) {
			thumb = youtube_get_video_thumbnail_url_by_id(v.id);
		}
		std::string id = v.id;
		SuccinctVideoView *row = (new SuccinctVideoView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT))
		                             ->set_title_lines(truncate_str(v.title.empty() ? v.id : v.title,
		                                                            320 - VIDEO_LIST_THUMBNAIL_WIDTH - 6, 2, 0.5, 0.5))
		                             ->set_auxiliary_lines({aux})
		                             ->set_channel_name(v.channel)
		                             ->set_bottom_right_overlay(liked::duration_text(v.duration_ms))
		                             ->set_thumbnail_url(thumb);
		row->set_get_background_color(View::STANDARD_BACKGROUND)
		    ->set_on_view_released([id](View &) { open_request = id; })
		    ->add_on_long_hold(40, [id](View &) { open_remove_sheet(id); });
		row_ids[row] = id;
		list_view->views.push_back(row);
	}
	main_view = (new ScrollView(0, 0, 320, 240))
	                ->set_views({(new TextView(0, 0, 320, HEADER_H))
	                                 ->set_text_offset(4, -1)
	                                 ->set_text((std::function<std::string()>)[]() {
		                                 return shell::tr("LIKED_VIDEOS", "Liked videos") +
		                                        (items.empty() ? "" : "  " + std::to_string(items.size()));
	                                 })
	                                 ->set_font_size(MIDDLE_FONT_SIZE, MIDDLE_FONT_INTERVAL),
	                             (new EmptyView(0, 0, 320, 2)), list_view});
	var_need_refresh = true;
}

static void open_video(const std::string &id) {
	auto copy = ready_copies.find(id);
	switch (liked::open_route(online(), copy != ready_copies.end())) {
	case liked::OpenRoute::ONLINE:
		global_intent.next_scene = SceneType::VIDEO_PLAYER;
		global_intent.arg = youtube_get_video_url_by_id(id);
		break;
	case liked::OpenRoute::LOCAL:
		global_intent.next_scene = SceneType::VIDEO_PLAYER;
		global_intent.arg = downloads::local_url_for(copy->second);
		break;
	default:
		shell::toast(shell::tr("LIKED_OFFLINE_UNAVAILABLE", "Offline: this video is not downloaded"));
		break;
	}
}

void Liked_init(void) {
	logger.info("liked/init", "Initializing...");
	liked_init(); // bounded load of the list (startup, like watch history)
	remove_sheet = new OverlayView(0, 0, 320, 240);
	remove_sheet->set_as_sheet(true);
	remove_sheet->set_is_visible(false);
	rebuild();
	already_init = true;
}
void Liked_exit(void) {
	already_init = false;
	logger.info("liked/exit", "Exited.");
}
void Liked_suspend(void) {}
void Liked_resume(std::string arg) {
	(void)arg;
	overlay_menu_on_resume();
	remove_sheet->set_is_visible(false);
	remove_candidate = remove_request = open_request = "";
	rebuild();
	if (main_view) {
		main_view->on_resume();
	}
}

void Liked_draw(void) {
	Hid_info key;
	Util_hid_query_key_state(&key);

	thumbnail_set_active_scene(SceneType::LIKED);
	if (liked_generation() != shown_generation || online() != shown_online) {
		rebuild();
	}

	bool video_playing_bar_show = video_is_playing();
	CONTENT_Y_HIGH = shell::content_bottom(video_playing_bar_show);
	shell::set_top_hints({{"A", shell::tr("HINT_OPEN", "Open")},
	                      {"dud", shell::tr("HINT_BROWSE", "Browse")},
	                      {"X", shell::tr("HINT_REMOVE", "Remove")},
	                      {"B", shell::tr("HINT_BACK", "Back")},
	                      {"START", shell::tr("HINT_MENU", "Menu")}});
	main_view->update_y_range(0, CONTENT_Y_HIGH);

	if (var_need_refresh || !var_eco_mode) {
		var_need_refresh = false;
		Draw_frame_ready();
		video_draw_top_screen();

		Draw_screen_ready(1, DEFAULT_BACK_COLOR);
		main_view->draw();
		std::string reason;
		if (!liked_available(&reason)) {
			std::vector<std::string> lines = modern::wrap(
			    shell::tr("LIKED_LOCKED", "Liked videos could not be read. The file was left untouched:") + " " + reason,
			    296, modern::T_BODY, 5);
			for (size_t i = 0; i < lines.size(); i++) {
				modern::text(lines[i], 12, HEADER_H + 14 + i * 15, modern::T_BODY, theme_cur().err);
			}
		} else if (items.empty()) {
			std::vector<std::string> lines = modern::wrap(
			    shell::tr("LIKED_EMPTY", "No liked videos yet. Tap Like under a video to keep it here. Likes stay on "
			                             "this console and are not sent to YouTube."),
			    296, modern::T_BODY, 4);
			for (size_t i = 0; i < lines.size(); i++) {
				modern::text(lines[i], 12, HEADER_H + 14 + i * 15, modern::T_BODY, theme_cur().tx2);
			}
		}

		if (video_playing_bar_show) {
			video_draw_playing_bar();
		}
		draw_overlay_menu(CONTENT_Y_HIGH);
		remove_sheet->draw(); // the remove sheet covers the nav

		if (Util_expl_query_show_flag()) {
			Util_expl_draw();
		}
		if (Util_err_query_error_show_flag()) {
			Util_err_draw();
		}
		Draw_touch_pos();
		Draw_apply_draw();
	} else {
		gspWaitForVBlank();
	}

	if (Util_err_query_error_show_flag()) {
		Util_err_main(key);
	} else if (Util_expl_query_show_flag()) {
		Util_expl_main(key);
	} else {
		if (remove_sheet->is_visible) {
			if (key.p_a && !remove_candidate.empty()) { // A confirms, like a tap on the button
				remove_request = remove_candidate;
				remove_candidate = "";
				main_view->reset_holding_status();
				remove_sheet->set_is_visible(false);
				var_need_refresh = true;
			} else {
				remove_sheet->update(key);
			}
			key.p_b = false; // B closed the sheet
		} else {
			bool taken = update_overlay_menu(&key);
			if (taken) { // the shell owns this frame: pending touches end without their action
				main_view->reset_holding_status();
				video_cancel_playing_bar_gesture();
			} else {
				main_view->update(key);
			}
			View *cued = cue::update(list_view, main_view, cue::list_top_in(main_view, list_view), key);
			if (!taken && key.p_x && cued && row_ids.count(cued)) {
				open_remove_sheet(row_ids[cued]);
			}
			if (open_request != "") {
				open_video(open_request);
				open_request = "";
			}
			if (video_playing_bar_show) {
				video_update_playing_bar(key);
			}
		}
		if (remove_request != "") {
			liked::LikedVideo v;
			v.id = remove_request;
			std::string message;
			liked_set(v, false, &message);
			shell::toast(message);
			remove_request = "";
		}

		if (key.p_b) {
			global_intent.next_scene = SceneType::BACK;
		}
	}
}
