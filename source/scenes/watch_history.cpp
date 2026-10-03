#include "headers.hpp"
#include <vector>
#include <string>
#include <set>
#include <map>
#include <numeric>

#include "scenes/watch_history.hpp"
#include "scenes/video_player.hpp"
#include "youtube_parser/parser.hpp"
#include "ui/ui.hpp"
#include "ui/overlay.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "data_io/history.hpp"
#include "util/misc_tasks.hpp"

#define MAX_THUMBNAIL_LOAD_REQUEST 12

namespace WatchHistory {
bool thread_suspend = false;
bool already_init = false;
bool exiting = false;

std::vector<HistoryVideo> watch_history;
std::string clicked_url;
std::string erase_request;

int cur_sort_type = 0;
int sort_request = -1;

int CONTENT_Y_HIGHT = 240; // changes according to whether the video playing bar is drawn or not

OverlayView *on_long_tap_dialog;
ScrollView *main_view = NULL;
VerticalListView *video_list_view = NULL;
}; // namespace WatchHistory
using namespace WatchHistory;

static void update_watch_history(const std::vector<HistoryVideo> &new_watch_history);

void History_init(void) {
	logger.info("history/init", "Initializing...");

	on_long_tap_dialog = new OverlayView(0, 0, 320, 240);
	on_long_tap_dialog->set_as_sheet(true); // OpenTube r13: a bottom sheet over the list
	on_long_tap_dialog->set_is_visible(false);

	load_watch_history();

	History_resume("");
	already_init = true;
}
void History_exit(void) {
	already_init = false;
	thread_suspend = false;
	exiting = true;

	logger.info("history/exit", "Exited.");
}
void History_suspend(void) { thread_suspend = true; }
void History_resume(std::string arg) {
	(void)arg;

	if (main_view) {
		main_view->on_resume();
	}
	overlay_menu_on_resume();
	thread_suspend = false;
	var_need_refresh = true;

	update_watch_history(get_valid_watch_history());
}

static void update_watch_history(const std::vector<HistoryVideo> &new_watch_history) {
	watch_history = new_watch_history;

	if (main_view) {
		main_view->recursive_delete_subviews();
	}
	delete main_view;

	// prepare new views
	video_list_view = (new VerticalListView(0, 0, 320))
	                      ->set_margin(SMALL_MARGIN)
	                      ->enable_thumbnail_request_update(MAX_THUMBNAIL_LOAD_REQUEST, SceneType::HISTORY);
	for (auto i : watch_history) {
		std::string view_count_str;
		{
			std::string view_count_str_tmp = LOCALIZED(MY_VIEW_COUNT_WITH_NUMBER);
			for (size_t j = 0; j < view_count_str_tmp.size();) {
				if (j + 1 < view_count_str_tmp.size() && view_count_str_tmp[j] == '%' &&
				    view_count_str_tmp[j + 1] == '0') {
					view_count_str += std::to_string(i.my_view_count), j += 2;
				} else {
					view_count_str.push_back(view_count_str_tmp[j]), j++;
				}
			}
		}
		std::string last_watch_time_str;
		{
			char tmp[100];
			strftime(tmp, 100, "%Y/%m/%d %H:%M", gmtime(&i.last_watch_time));
			last_watch_time_str = tmp;
		}

		SuccinctVideoView *cur_view =
		    (new SuccinctVideoView(0, 0, 320, VIDEO_LIST_THUMBNAIL_HEIGHT))
		        ->set_title_lines(i.title_lines)
		        ->set_auxiliary_lines({view_count_str + " " + last_watch_time_str})
		        ->set_channel_name(i.author_name)
		        ->set_bottom_right_overlay(i.length_text)
		        ->set_thumbnail_url(youtube_get_video_thumbnail_url_by_id(i.id));

		cur_view
		    ->set_get_background_color(View::STANDARD_BACKGROUND)
		    ->set_on_view_released([i](View &view) { clicked_url = youtube_get_video_url_by_id(i.id); })
		    ->add_on_long_hold(40, [i](View &view) {
			    on_long_tap_dialog->recursive_delete_subviews();
			    on_long_tap_dialog
			        ->set_subview(
			            (new TextView(0, 0, 300, 34))
			                ->set_text((std::function<std::string()>)[]() { return LOCALIZED(REMOVE_HISTORY_ITEM); })
			                ->set_x_alignment(TextView::XAlign::CENTER)
			                ->set_y_alignment(TextView::YAlign::CENTER)
			                ->set_text_offset(0, -1)
			                ->set_on_view_released([i](View &view) {
				                erase_request = i.id;
				                main_view->reset_holding_status();
				                on_long_tap_dialog->set_is_visible(false);
				                var_need_refresh = true;
			                })
			                ->set_get_background_color(View::STANDARD_BUTTON_BACKGROUND)
			                ->set_corner_radius(-1))
			        ->set_on_cancel([](OverlayView &view) {
				        main_view->reset_holding_status();
				        view.set_is_visible(false);
				        var_need_refresh = true;
			        })
			        ->set_is_visible(true);
			    var_need_refresh = true;
		    });

		video_list_view->views.push_back(cur_view);
	}
	constexpr int selector_width = 176;
	constexpr int header_height = 34; // OpenTube r13: screen header with the sort pills on the right
	main_view = (new ScrollView(0, 0, 320, 240))
		->set_views({
			(new HorizontalListView(0, 0, header_height))
				->set_views({
					(new TextView(0, 0, 320 - selector_width, header_height))
						->set_text_offset(4, -1)
						->set_text((std::function<std::string()>) [] () {
		return LOCALIZED(WATCH_HISTORY); })
						->set_font_size(MIDDLE_FONT_SIZE, MIDDLE_FONT_INTERVAL),
					(new SelectorView(0, 0, selector_width, header_height, false))
						->set_texts({
							(std::function<std::string ()>) [] () { return LOCALIZED(BY_LAST_WATCH_TIME); },
							(std::function<std::string ()>) [] () {
		return LOCALIZED(BY_MY_VIEW_COUNT); }
						}, cur_sort_type)
						->set_on_change([](const SelectorView &view) {
		sort_request = cur_sort_type = view.selected_button; })
}),
			(new EmptyView(0, 0, 320, 2)),
			video_list_view
});
}

void History_draw(void) {
	Hid_info key;
	Util_hid_query_key_state(&key);

	thumbnail_set_active_scene(SceneType::HISTORY);

	bool video_playing_bar_show = video_is_playing();
	CONTENT_Y_HIGHT = shell::content_bottom(video_playing_bar_show);
	shell::set_top_hints({{"A", shell::tr("HINT_OPEN", "Open")},
	                      {"dud", shell::tr("HINT_BROWSE", "Browse")},
	                      {"B", shell::tr("HINT_BACK", "Back")},
	                      {"START", shell::tr("HINT_MENU", "Menu")}});
	main_view->update_y_range(0, CONTENT_Y_HIGHT);

	if (var_need_refresh || !var_eco_mode) {
		var_need_refresh = false;
		Draw_frame_ready();
		video_draw_top_screen();

		Draw_screen_ready(1, DEFAULT_BACK_COLOR);

		main_view->draw();

		if (video_playing_bar_show) {
			video_draw_playing_bar();
		}
		draw_overlay_menu(CONTENT_Y_HIGHT);
		on_long_tap_dialog->draw(); // the remove sheet covers the nav

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
		if (on_long_tap_dialog->is_visible) {
			on_long_tap_dialog->update(key);
			key.p_b = false; // B closed the sheet
		} else {
			if (update_overlay_menu(&key)) { // the shell owns this frame: pending touches end without their action
				main_view->reset_holding_status();
				video_cancel_playing_bar_gesture();
			} else {
				main_view->update(key);
			}
			cue::update(video_list_view, main_view, cue::list_top_in(main_view, video_list_view), key);
			if (clicked_url != "") {
				global_intent.next_scene = SceneType::VIDEO_PLAYER;
				global_intent.arg = clicked_url;
				clicked_url = "";
			}
			if (sort_request != -1) {
				auto tmp_watch_history = watch_history;
				std::sort(tmp_watch_history.begin(), tmp_watch_history.end(),
				          [](const HistoryVideo &i, const HistoryVideo &j) {
					          if (sort_request == 0) {
						          return i.last_watch_time > j.last_watch_time;
					          }
					          if (sort_request == 1) {
						          return i.my_view_count > j.my_view_count;
					          }
					          // should not reach here
					          return false;
				          });
				update_watch_history(tmp_watch_history);

				sort_request = -1;
			}
			if (erase_request != "") {
				// erase
				std::vector<HistoryVideo> tmp_watch_history;
				for (auto video : watch_history) {
					if (video.id != erase_request) {
						tmp_watch_history.push_back(video);
					}
				}
				update_watch_history(tmp_watch_history);
				history_erase_by_id(erase_request);
				misc_tasks_request(TASK_SAVE_HISTORY);

				erase_request = "";
			}
			if (video_playing_bar_show) {
				video_update_playing_bar(key);
			}
		}

		if (key.p_b) {
			global_intent.next_scene = SceneType::BACK;
		}
	}
}
