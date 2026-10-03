// clang-format off
#include "headers.hpp"
#include <sstream> // std::istringstream / std::ostringstream (not transitively included by gcc 16 libstdc++)
#include <functional>
#include <regex>

#include "data_io/settings.hpp"
#include "data_io/history.hpp"
#include "util/misc_tasks.hpp"
#include "scenes/setting_menu.hpp"
#include "scenes/home.hpp"
#include "scenes/video_player.hpp"
#include "ui/overlay.hpp"
#include "ui/ui.hpp"
#include "ui/views/specialized/succinct_channel.hpp"
#include "youtube_parser/parser.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "network_decoder/network_io.hpp"
#include "rapidjson_wrapper.hpp"
#include "oauth/oauth.hpp"
#include "updater/update_app.hpp"
#include "ui/modern.hpp"

struct SectionTitleWithInfoView : public FixedSizeView {
	UI::FlexibleString<SectionTitleWithInfoView> title_text;
	UI::FlexibleString<SectionTitleWithInfoView> info_text;
	int popup_height = 30;
	
	SectionTitleWithInfoView(double x0, double y0, double width, double height) 
		: View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	
	SectionTitleWithInfoView *set_title(std::function<std::string(const SectionTitleWithInfoView &)> title_func) {
		title_text = UI::FlexibleString<SectionTitleWithInfoView>(title_func, *this);
		return this;
	}
	
	SectionTitleWithInfoView *set_info(std::function<std::string(const SectionTitleWithInfoView &)> info_func) {
		info_text = UI::FlexibleString<SectionTitleWithInfoView>(info_func, *this);
		return this;
	}
	
	SectionTitleWithInfoView *set_popup_height(int height) {
		popup_height = height;
		return this;
	}
	
	void draw_() const override {
		double y_pos = (y0 + y1 - DEFAULT_FONT_INTERVAL) / 2 - 2;
		Draw(title_text, x0 + SMALL_MARGIN, y_pos, 0.6, 0.6, DEFAULT_TEXT_COLOR);
		
		double info_x = x0 + SMALL_MARGIN + Draw_get_width(title_text, 0.6) + 5;
		Draw("ⓘ", info_x, y_pos + (0.6 - 0.45) * DEFAULT_FONT_INTERVAL / 2, 0.45, 0.45, POCKET_ACCENT);
	}
	
	void update_(Hid_info key) override {
		if (key.p_touch && key.touch_y >= y0 && key.touch_y < y1) {
			double info_x = x0 + SMALL_MARGIN + Draw_get_width(title_text, 0.6) + 5;
			if (key.touch_x >= info_x - 3 && key.touch_x < info_x + Draw_get_width("ⓘ", 0.45) + 3) {
				extern void (*show_info_popup_callback)(const std::string &, int);
				if (show_info_popup_callback) show_info_popup_callback(info_text, popup_height);
			}
		}
	}
};

namespace Settings {
	bool thread_suspend = false;
	bool already_init = false;
	bool exiting = false;
	
	TextView *toast_view;
	int toast_frames_left = 0;
	
	OverlayDialogView *popup_view;
	VerticalListView *main_view;
	TabView *main_tab_view;
	
	
	// OAuth state
	bool oauth_check_timer_active = false;
	int oauth_check_frames = 0;
	int oauth_timeout_counter = 0;
	const int OAUTH_CHECK_INTERVAL_FRAMES = 300;
	const int OAUTH_TIMEOUT_SECONDS = 900;
	
	SuccinctChannelView *oauth_user_view = nullptr;
	
	Thread oauth_worker_thread;
	
	int CONTENT_Y_HIGH = 240;
	constexpr int TOP_HEIGHT = 34; // OpenTube r13: screen header, segmented tabs below it
	
	constexpr int DIALOG_WIDTH = 240;
	
	// OpenTube r15: the updater lives in updater/update_app.cpp (own-repo releases, verified before install)
	static Mutex resource_lock;
	
	Thread settings_misc_thread;
};
using namespace Settings;

void (*show_info_popup_callback)(const std::string &, int) = nullptr;

// ---- OpenTube r15 Update tab ------------------------------------------------------------------------------------------
static std::string mb(uint64_t bytes) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
	return buf;
}
static std::string update_action_label(const UpdaterView &v) {
	using updater::State;
	std::string next = "OpenTube " + updater::version_string(v.release.version);
	switch (v.state) {
	case State::AVAILABLE:
		return v.is_3dsx ? shell::tr("UPDATE_CHECK_AGAIN", "Check again") : shell::tr("UPDATE_DOWNLOAD", "Download") + " " + next;
	case State::DOWNLOAD_FAILED:
		return shell::tr("UPDATE_DOWNLOAD_AGAIN", "Try the download again");
	case State::DOWNLOADING:
		return shell::tr("UPDATE_CANCEL_DOWNLOAD", "Cancel download");
	case State::READY:
		return shell::tr("UPDATE_INSTALL", "Install") + " " + next;
	case State::CHECKING:
	case State::INSTALLING:
	case State::INSTALLED:
		return "";
	default:
		return shell::tr("UPDATE_CHECK", "Check for updates");
	}
}
static void confirm(const std::string &text, const std::string &ok_label, std::function<void()> on_ok) {
	std::vector<std::string> lines = modern::wrap(text, DIALOG_WIDTH, modern::T_BODY, 6);
	popup_view->get_message_view()->set_text_lines(lines)->update_y_range(0, 15 * lines.size() + 10);
	popup_view->set_buttons<std::function<std::string ()> >({
		[] () { return LOCALIZED(CANCEL); },
		[ok_label] () { return ok_label; }
	},
	[on_ok] (OverlayDialogView &, int button_pressed) {
		if (button_pressed == 1) on_ok();
		return true; // close the dialog
	});
	popup_view->set_is_visible(true);
	var_need_refresh = true;
}
static void update_action() {
	using updater::State;
	UpdaterView v = Updater_snapshot(); // the confirmation binds exactly what is shown now
	std::string msg, next = "OpenTube " + updater::version_string(v.release.version);
	switch (v.state) {
	case State::AVAILABLE:
	case State::DOWNLOAD_FAILED:
		if (v.is_3dsx) {
			if (!Updater_check(&msg)) shell::toast(msg);
			break;
		}
		confirm("Download " + next + " (" + mb(v.release.cia.size) + ") from github.com/krsvc/OpenTube? It is checked "
		        "against SHA256SUMS. Nothing is installed until you confirm again.",
		        shell::tr("UPDATE_DOWNLOAD", "Download"), [v] () {
			std::string m;
			if (!Updater_download(v.release, &m)) shell::toast(m);
		});
		break;
	case State::DOWNLOADING:
		if (!Updater_cancel(&msg)) shell::toast(msg);
		break;
	case State::READY: {
		std::string blocker = Updater_install_blocker();
		if (!blocker.empty()) {
			shell::toast(blocker); // never stopped for the user
			break;
		}
		confirm("Install " + next + " now? Keep the console on and the SD card in until it finishes; the installation "
		        "cannot be cancelled. Settings, history, liked videos and downloads are kept.",
		        shell::tr("UPDATE_INSTALL", "Install"), [v] () {
			std::string m;
			if (!Updater_install(v.staged.sha256, &m)) shell::toast(m);
		});
		break;
	}
	case State::CHECKING:
	case State::INSTALLING:
	case State::INSTALLED:
		break;
	default:
		if (!Updater_check(&msg)) shell::toast(msg);
		break;
	}
	var_need_refresh = true;
}
// status, version details, notes and progress of the updater (read-only; the button below acts)
struct UpdatePanelView : public FixedSizeView {
	static constexpr int HEIGHT = 200;
	UpdatePanelView(double x0, double y0, double w, double h) : View(x0, y0), FixedSizeView(x0, y0, w, h) {}
	void draw_() const override {
		using updater::State;
		const ThemeTokens &t = theme_cur();
		UpdaterView v = Updater_snapshot();
		float x = x0 + 10, y = y0 + 6, w = 300;
		modern::text("OpenTube " + Updater_current_version(), x, y, modern::T_TITLE, t.tx);
		modern::text(shell::tr("UPDATE_SOURCE", "Stable releases from github.com/krsvc/OpenTube"), x, y + 18,
		             modern::T_META, t.tx2);
		y += 36;
		for (auto &line : modern::wrap(v.message.empty() ? shell::tr("UPDATE_IDLE", "Not checked yet in this session.") : v.message,
		                               w, modern::T_BODY, 3)) {
			modern::text(line, x, y, modern::T_BODY, v.state == State::CHECK_FAILED || v.state == State::DOWNLOAD_FAILED ||
			                                                 v.state == State::INSTALL_FAILED ? t.err : t.tx);
			y += 15;
		}
		if ((v.state == State::DOWNLOADING || v.state == State::INSTALLING) && v.total) {
			float f = (float)std::min<uint64_t>(v.done, v.total) / v.total;
			modern::pill(x, y + 4, w, 6, t.surf2);
			modern::pill(x, y + 4, std::max(6.0f, w * f), 6, t.acc);
			modern::text(mb(v.done) + " / " + mb(v.total), x, y + 12, modern::T_META, t.tx2);
			y += 30;
		}
		bool has_release = !v.release.tag.empty() && v.state != State::UP_TO_DATE && v.state != State::NO_RELEASE &&
		                   v.state != State::CHECK_FAILED && v.state != State::CHECKING;
		if (has_release) {
			modern::text(modern::ellipsize("OpenTube " + updater::version_string(v.release.version) +
			                                   (v.release.title.empty() ? "" : " \xC2\xB7 " + v.release.title),
			                               w, modern::T_ROW),
			             x, y + 2, modern::T_ROW, t.tx);
			y += 20;
			int budget = v.is_3dsx ? 3 : 4; // release notes: their own line breaks kept, bounded
			for (size_t pos = 0; pos <= v.release.notes.size() && budget > 0;) {
				size_t nl = v.release.notes.find('\n', pos);
				std::string para = v.release.notes.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
				pos = nl == std::string::npos ? v.release.notes.size() + 1 : nl + 1;
				for (auto &line : modern::wrap(para, w, modern::T_META, budget)) {
					modern::text(line, x, y, modern::T_META, t.tx2);
					y += 13;
					budget--;
				}
			}
			if (v.is_3dsx) {
				for (auto &line : modern::wrap("This copy runs as .3dsx: download OpenTube.3dsx from the release page and "
				                               "replace the file yourself. The app does not overwrite itself.",
				                               w, modern::T_META, 3)) {
					modern::text(line, x, y, modern::T_META, t.acc_tx);
					y += 13;
				}
			}
		}
	}
	void update_(Hid_info) override {}
};

void show_info_popup(const std::string &message, int height = 30) {
	if (!popup_view) return;
	
	popup_view->get_message_view()
		->set_text_lines(split_string(message, '\n'))
		->update_y_range(0, height);
	popup_view->set_buttons<std::function<std::string()>>({
		[]() { return LOCALIZED(OK); }
	}, [](OverlayDialogView &, int) {
		return true;
	});
	popup_view->set_is_visible(true);
	var_need_refresh = true;
}


static void oauth_worker_thread_func(void *) {
	logger.info("oauth_worker", "Thread started.");
	oauth_timeout_counter = 0;
	
	while (!exiting) {
		if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATING && oauth_check_timer_active) {
			int poll_interval = std::max(OAuth::interval, 8);
			oauth_timeout_counter += poll_interval;
			
			logger.info("oauth_worker", "Checking device flow authentication...");
			OAuth::check_device_flow();
			
			if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATED) {
				oauth_check_timer_active = false;
				oauth_timeout_counter = 0;
				var_oauth_enabled = true;
				misc_tasks_request(TASK_SAVE_SETTINGS);

				resource_lock.lock();
				if (oauth_user_view) {
					std::string user_name = OAuth::get_user_account_name();
					std::string handle = OAuth::get_user_handle();
					std::string subscriber_count = OAuth::get_user_subscriber_count();
					std::string photo_url = OAuth::get_user_photo_url();
					
					std::vector<std::string> aux_lines;
					if (!handle.empty()) aux_lines.push_back(handle);
					if (!subscriber_count.empty()) aux_lines.push_back(subscriber_count);
					
					oauth_user_view->set_name(user_name);
					oauth_user_view->set_auxiliary_lines(aux_lines);
					oauth_user_view->set_thumbnail_url(photo_url);
					oauth_user_view->set_height(CHANNEL_ICON_HEIGHT);
					if (!photo_url.empty()) {
						oauth_user_view->thumbnail_handle = thumbnail_request(photo_url, SceneType::SETTINGS, PRIORITY_FOREGROUND, ThumbnailType::ICON);
					}
				}
				if (popup_view->is_visible) {
					std::string success_msg = std::string(LOCALIZED(OAUTH_AUTHENTICATED)) + "\n\n" + LOCALIZED(OAUTH_TOKEN_WARNING);
					popup_view->get_message_view()->set_text_lines(split_string(success_msg, '\n'))->update_y_range(0, 80);
					popup_view->set_buttons<std::function<std::string ()> >({
						[] () { return LOCALIZED(OK); }
					},
					[] (OverlayDialogView &, int button_pressed) {
						return true;
					});
				}
				resource_lock.unlock();
				var_need_refresh = true;
			} else if (OAuth::oauth_state == OAuth::OAuthState::ERROR || oauth_timeout_counter >= OAUTH_TIMEOUT_SECONDS) {
				oauth_check_timer_active = false;
				oauth_timeout_counter = 0;
				
				char log_msg[512];
				snprintf(log_msg, sizeof(log_msg), "OAuth error detected: %s", OAuth::oauth_error_message.c_str());
				logger.info("oauth_worker", log_msg);
				
				if (oauth_timeout_counter >= OAUTH_TIMEOUT_SECONDS) {
					OAuth::device_code = "";
					OAuth::user_code = "";
					OAuth::verification_url = "";
				}
				
				resource_lock.lock();
				if (popup_view->is_visible) {
					std::string error_msg;
					if (oauth_timeout_counter >= OAUTH_TIMEOUT_SECONDS) {
						error_msg = LOCALIZED(OAUTH_TIMEOUT);
					} else {
						error_msg = LOCALIZED(OAUTH_ERROR);
						if (!OAuth::oauth_error_message.empty()) {
							error_msg += "\n\n";
							error_msg += OAuth::oauth_error_message;
						}
					}
					popup_view->get_message_view()->set_text_lines(split_string(error_msg, '\n'))->update_y_range(0, 80);
					popup_view->set_buttons<std::function<std::string ()> >({
						[] () { return LOCALIZED(OK); }
					},
					[] (OverlayDialogView &, int button_pressed) {
						return true;
					});
				}
				resource_lock.unlock();
				var_need_refresh = true;
				
				usleep(5000000);
				resource_lock.lock();
				popup_view->set_is_visible(false);
				resource_lock.unlock();
				var_need_refresh = true;
			}
		}
		
		int sleep_seconds = (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATING && oauth_check_timer_active) ?
			std::max(OAuth::interval, 8) : 2;
		usleep(sleep_seconds * 1000000);
	}
	
	logger.info("oauth_worker", "Thread exit.");
	threadExit(0);
}

namespace {
	const std::vector<std::string> languages_ui = {"en", "ja", "de", "fr", "it", "es"};
	const std::vector<std::string> languages_content = {"en", "ja", "de", "fr", "it", "es"};

	int get_language_index(const std::vector<std::string>& codes, const std::string& lang) {
		for (size_t i = 0; i < codes.size(); ++i) {
			if (codes[i] == lang) return i;
		}
		return 0;
	}
}

void Sem_init(void) {
	logger.info("settings/init", "Initializing...");
	Result_with_string result;
	
	load_settings();
	load_string_resources(var_lang);
	
	popup_view = new OverlayDialogView(0, 0, 320, 240);
	popup_view->set_is_visible(false);
	toast_view = new TextView((320 - 150) / 2, 190, 150, DEFAULT_FONT_INTERVAL + SMALL_MARGIN);
	toast_view->set_is_visible(false);
	
	main_tab_view = (new TabView(0, 0, 320, CONTENT_Y_HIGH - TOP_HEIGHT))
		->set_stretch_subview(true)
		->set_tab_font_size(0.4)
		->set_views({
			// Tab #1 : UI/Display
			(new ScrollView(0, 0, 320, 0))
				->set_views({
					// UI language
					(new GridSelectorView(0, 0, 320, 70, false))
						->set_texts({
							"English",
							"日本語",
							"Deutsch",
							"Français",
							"Italiano",
							"Español"
						}, get_language_index(languages_ui, var_lang))
						->set_row_counts({3, 3})
						->set_title([](const GridSelectorView &) { return LOCALIZED(UI_LANGUAGE); })
						->set_on_change([](const GridSelectorView &view) {
							auto next_lang = languages_ui[view.selected_button];
							if (var_lang != next_lang) {
								var_lang = next_lang;
								misc_tasks_request(TASK_RELOAD_STRING_RESOURCE);
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// Content language
					(new GridSelectorView(0, 0, 320, 70, false))
						->set_texts({
							"English",
							"日本語",
							"Deutsch",
							"Français",
							"Italiano",
							"Español"
						}, get_language_index(languages_content, var_lang_content))
						->set_row_counts({3, 3})
						->set_title([](const GridSelectorView &) { return LOCALIZED(CONTENT_LANGUAGE); })
						->set_on_change([](const GridSelectorView &view) {
							auto next_lang = languages_content[view.selected_button];
							if (var_lang_content != next_lang) {
								var_lang_content = next_lang;
								misc_tasks_request(TASK_SAVE_SETTINGS);
								youtube_change_content_language(var_lang_content);
							}
						}),
					// LCD Brightness
					(new BarView(0, 0, 320, 40))
						->set_values_sync(15, 163, &var_lcd_brightness)
						->set_title([] (const BarView &view) { return LOCALIZED(LCD_BRIGHTNESS); })
						->set_while_holding([] (const BarView &view) { misc_tasks_request(TASK_CHANGE_BRIGHTNESS); })
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					// Time to turn off LCD
					(new BarView(0, 0, 320, 40))
						->set_values(10, 310, var_time_to_turn_off_lcd != 0 && var_time_to_turn_off_lcd <= 309 ? var_time_to_turn_off_lcd : 310)
						->set_title([] (const BarView &view) { return LOCALIZED(TIME_TO_TURN_OFF_LCD) + " : " +
							(view.get_value() <= 309 ? std::to_string((int) view.get_value()) + " " + LOCALIZED(SECONDS) : LOCALIZED(NEVER_TURN_OFF)); })
						->set_on_release([] (const BarView &view) {
							var_time_to_turn_off_lcd = view.get_value() <= 309 ? view.get_value() : 0;
							misc_tasks_request(TASK_SAVE_SETTINGS);
						}),
					// Screen Toggle Mode
					(new SelectorView(0, 0, 320, 35, false))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(SCREEN_TOGGLE_BOTTOM); },
							(std::function<std::string ()>) []() { return LOCALIZED(SCREEN_TOGGLE_TOP); },
							(std::function<std::string ()>) []() { return LOCALIZED(SCREEN_TOGGLE_BOTH); }
						}, var_screen_off_mode)
						->set_title([](const SelectorView &) { return LOCALIZED(SCREEN_TOGGLE_MODE); })
						->set_on_change([](const SelectorView &view) {
							if (var_screen_off_mode != view.selected_button) {
								var_screen_off_mode = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// full screen mode
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_full_screen_mode)
						->set_title([](const SelectorView &) { return LOCALIZED(FULL_SCREEN_MODE); })
						->set_on_change([](const SelectorView &view) {
							if (var_full_screen_mode != view.selected_button) {
								var_full_screen_mode = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// show full dislike/like
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_full_dislike_like_count)
						->set_title([](const SelectorView &) { return LOCALIZED(SHOW_FULL_DISLIKE); })
						->set_on_change([](const SelectorView &view) {
							if (var_full_dislike_like_count != view.selected_button) {
								var_full_dislike_like_count = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// Hide pointer option
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_hide_pointer)
						->set_title([](const SelectorView &) { return LOCALIZED(HIDE_POINTER); })
						->set_on_change([](const SelectorView &view) {
							if (var_hide_pointer != view.selected_button) {
								var_hide_pointer = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// Dark theme
					(new ThemePickerView(0, 0, 320, 64)) // OpenTube r13: replaces the Dark theme toggle (dark_theme kept in sync)
						->set_title([]() { return shell::tr("THEME", "Theme"); })
						->set_on_change([](ThemeId) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					// Disable pull to refresh
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_disable_pull_to_refresh)
						->set_title([](const SelectorView &) { return LOCALIZED(DISABLE_PULL_TO_REFRESH); })
						->set_info([](const SelectorView &) { return LOCALIZED(INFO_DISABLE_PULL_TO_REFRESH); })
						->set_popup_height(60)
						->set_on_change([](const SelectorView &view) {
							if (var_disable_pull_to_refresh != view.selected_button) {
								var_disable_pull_to_refresh = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
								Home_rebuild_feed_tab();
								Home_rebuild_channels_tab();
								Home_update_pull_to_refresh();
							}
						}),
					// Scroll speed 0
					(new BarView(0, 0, 320, 35))
						->set_values_sync(DPAD_SCROLL_SPEED_MIN, DPAD_SCROLL_SPEED_MAX, &var_dpad_scroll_speed0)
						->set_title([] (const BarView &view) { return LOCALIZED(SCROLL_SPEED0) + " : " + double2str(var_dpad_scroll_speed0, 1); })
						->set_while_holding([] (const BarView &view) {
							var_dpad_scroll_speed1 = std::max(var_dpad_scroll_speed1, var_dpad_scroll_speed0);
						})
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					// Scroll speed 1
					(new BarView(0, 0, 320, 35))
						->set_values_sync(DPAD_SCROLL_SPEED_MIN, DPAD_SCROLL_SPEED_MAX, &var_dpad_scroll_speed1)
						->set_title([] (const BarView &view) { return LOCALIZED(SCROLL_SPEED1) + " : " + double2str(var_dpad_scroll_speed1, 1); })
						->set_while_holding([] (const BarView &view) {
							var_dpad_scroll_speed0 = std::min(var_dpad_scroll_speed0, var_dpad_scroll_speed1);
						})
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					// Scroll speed change threshold
					(new BarView(0, 0, 320, 40))
						->set_values_sync(DPAD_SCROLL_THRESHOLD_MIN, DPAD_SCROLL_THRESHOLD_MAX, &var_dpad_scroll_speed1_threshold)
						->set_title([] (const BarView &view) { return LOCALIZED(SCROLL_SPEED_THRESHOLD) + " : " + double2str(var_dpad_scroll_speed1_threshold, 1); })
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					// Size of images in community posts
					(new BarView(0, 0, 320, 40))
						->set_values_sync(COMMUNITY_IMAGE_SIZE_MIN, COMMUNITY_IMAGE_SIZE_MAX, &var_community_image_size)
						->set_title([] (const BarView &view) { return LOCALIZED(COMMUNITY_POST_IMAGE_SIZE) + " : " + std::to_string(var_community_image_size) + " px"; })
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); }),
					(new EmptyView(0, 0, 320, 10))
				}),
			// Tab #2 : Playback
			(new ScrollView(0, 0, 320, 0))
				->set_views({
					// Autoplay
					(new SelectorView(0, 0, 320, 35, false))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ONLY_IN_PLAYLIST); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_autoplay_level)
						->set_title([](const SelectorView &) { return LOCALIZED(AUTOPLAY); })
						->set_on_change([](const SelectorView &view) {
							if (var_autoplay_level != view.selected_button) {
								var_autoplay_level = view.selected_button;
								misc_tasks_request(TASK_CHANGE_BRIGHTNESS);
							}
						}),

					// Forward buffer ratio
					(new BarView(0, 0, 320, 40))
						->set_values_sync(0.1, 1.0, &var_forward_buffer_ratio)
						->set_title([] (const BarView &view) {
							char ratio_str[16];
							snprintf(ratio_str, 16, "%.2f", var_forward_buffer_ratio);
							return LOCALIZED(FORWARD_BUFFER_RATIO) + " : " + ratio_str;
						})
						->set_on_release([] (const BarView &view) { misc_tasks_request(TASK_SAVE_SETTINGS); })
				}),
			// Tab #3 : Data
			(new ScrollView(0, 0, 320, 0))
				->set_views({
					// History recording
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(DISABLED); },
							(std::function<std::string ()>) []() { return LOCALIZED(ENABLED); }
						}, var_history_enabled)
						->set_title([](const SelectorView &) { return LOCALIZED(WATCH_HISTORY); })
						->set_on_change([](const SelectorView &view) {
							if (var_history_enabled != view.selected_button) {
								var_history_enabled = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					(new EmptyView(0, 0, 320, 10)),
					// Erase history
					(new TextView(10, 0, 120, DEFAULT_FONT_INTERVAL + SMALL_MARGIN * 2))
						->set_text((std::function<std::string ()>) [] () { return LOCALIZED(REMOVE_ALL_HISTORY); })
						->set_x_alignment(TextView::XAlign::CENTER)
						->set_text_offset(0, -2)
						->set_get_text_color([] () { return POCKET_ON_ACCENT; })
						->set_get_background_color([] (const View &view) {
							return View::blend_color(POCKET_ACCENT, POCKET_PRESSED, view.touch_darkness * 0.5);
						})
						->set_on_view_released([] (View &view) {
							popup_view->get_message_view()->set_text_lines(split_string(LOCALIZED(REMOVE_ALL_HISTORY_CONFIRM), '\n'))->update_y_range(0, 30);
							popup_view->set_buttons<std::function<std::string ()> >({
								[] () { return LOCALIZED(CANCEL); },
								[] () { return LOCALIZED(OK); }
							},
							[] (OverlayDialogView &, int button_pressed) {
								if (button_pressed == 1) {
									history_erase_all();
									misc_tasks_request(TASK_SAVE_HISTORY);
									
									float tabbed_content_y_high = CONTENT_Y_HIGH - main_tab_view->tab_selector_height;
									toast_view
										->set_text((std::function<std::string ()>) [] () { return LOCALIZED(ALL_HISTORY_REMOVED); })
										->set_x_alignment(TextView::XAlign::CENTER)
										->set_text_offset(0, -1)
										->set_get_text_color([] () { return DEFAULT_TEXT_COLOR; })
										->update_y_range(tabbed_content_y_high - 20, tabbed_content_y_high - 5)
										->set_get_background_color([] (const View &) { return POCKET_PRESSED; })
										->set_is_visible(true);
									toast_frames_left = 120;
								}
								return true; // close the dialog
							});
							popup_view->set_is_visible(true);
							var_need_refresh = true;
						}),
					(new EmptyView(0, 0, 320, 10))
				}),
			// Tab #4 : Update (OpenTube r15: krsvc/OpenTube stable releases, two confirmations, see update_app.hpp)
			(new ScrollView(0, 0, 320, 0))
				->set_views({
					new UpdatePanelView(0, 0, 320, UpdatePanelView::HEIGHT),
					(new TextView(10, 0, 300, DEFAULT_FONT_INTERVAL + SMALL_MARGIN * 2))
						->set_text((std::function<std::string ()>) [] () { return update_action_label(Updater_snapshot()); })
						->set_x_alignment(TextView::XAlign::CENTER)
						->set_text_offset(0, -1)
						->set_on_view_released([] (const View &) { update_action(); })
						->set_get_background_color([] (const View &view) -> u32 {
							return update_action_label(Updater_snapshot()).empty() ? DEFAULT_BACK_COLOR
							                                                       : View::STANDARD_BUTTON_BACKGROUND(view);
						}),
					(new EmptyView(0, 0, 320, DEFAULT_FONT_INTERVAL))
				}),
			// Tab #5 : Advanced
			(new ScrollView(0, 0, 320, 0))
				->set_views({
					// Eco mode
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_eco_mode)
						->set_title([](const SelectorView &) { return LOCALIZED(ECO_MODE); })
						->set_info([](const SelectorView &) { return LOCALIZED(INFO_ECO_MODE); })
						->set_popup_height(60)
						->set_on_change([](const SelectorView &view) {
							if (var_eco_mode != view.selected_button) {
								var_eco_mode = view.selected_button;
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					// Use linear filter
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_video_linear_filter)
						->set_title([](const SelectorView &) { return LOCALIZED(LINEAR_FILTER); })
						->set_on_change([](const SelectorView &view) {
							if (var_video_linear_filter != view.selected_button) {
								var_video_linear_filter = view.selected_button;
								video_set_linear_filter_enabled(var_video_linear_filter);
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					(new EmptyView(0, 0, 320, 10)),
					// Debug info in the control tab
					(new SelectorView(0, 0, 320, 35, true))
						->set_texts({
							(std::function<std::string ()>) []() { return LOCALIZED(OFF); },
							(std::function<std::string ()>) []() { return LOCALIZED(ON); }
						}, var_video_show_debug_info)
						->set_title([](const SelectorView &view) { return LOCALIZED(VIDEO_SHOW_DEBUG_INFO); })
						->set_on_change([](const SelectorView &view) {
							if (var_video_show_debug_info != view.selected_button) {
								var_video_show_debug_info = view.selected_button;
								video_set_show_debug_info(var_video_show_debug_info);
								misc_tasks_request(TASK_SAVE_SETTINGS);
							}
						}),
					(new EmptyView(0, 0, 320, 10)),
                    // Select app data to use
                    (new SelectorView(0, 0, 320, 35, false))
                        ->set_texts({
                            (std::function<std::string ()>) []() { return "Android"; },
                            (std::function<std::string ()>) []() { return "Android VR"; },
                            (std::function<std::string ()>) []() { return "visionOS"; }
                        }, var_player_response)
                        ->set_title([](const SelectorView &) { return LOCALIZED(PLAYER_RESPONSE); })
                        ->set_info([](const SelectorView &) { return LOCALIZED(INFO_PLAYER_RESPONSE); })
                        ->set_popup_height(50)
                        ->set_on_change([](const SelectorView &view) {
                            if (var_player_response != view.selected_button) {
                                var_player_response = view.selected_button;
                                if (view.selected_button == 0) {
                                    var_player_response = 0; // Android
                                } else if (view.selected_button == 1) {
                                    var_player_response = 1; // Android VR
                                } else if (view.selected_button == 2) {
                                    var_player_response = 2; // visionOS
                                }
                                misc_tasks_request(TASK_SAVE_SETTINGS);
                            }
                        }),
                    (new EmptyView(0, 0, 320, 10)),
					// OAuth Settings Section
					(new SectionTitleWithInfoView(0, 0, 320, DEFAULT_FONT_INTERVAL + SMALL_MARGIN * 2))
						->set_title([] (const SectionTitleWithInfoView &) { return LOCALIZED(OAUTH); })
						->set_info([] (const SectionTitleWithInfoView &) { return LOCALIZED(INFO_OAUTH); })
						->set_popup_height(85),
					(new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL + SMALL_MARGIN))
						->set_text([] () -> std::string {
							const std::string prefix = LOCALIZED(OAUTH_STATUS) + ": ";
							switch (OAuth::oauth_state) {
								case OAuth::OAuthState::NOT_AUTHENTICATED: return prefix + LOCALIZED(OAUTH_NOT_AUTHENTICATED);
								case OAuth::OAuthState::AUTHENTICATED: return prefix + LOCALIZED(OAUTH_AUTHENTICATED);
								case OAuth::OAuthState::AUTHENTICATING: return prefix + LOCALIZED(OAUTH_AUTHENTICATING);
								case OAuth::OAuthState::ERROR: return prefix + LOCALIZED(OAUTH_ERROR);
								default: return "";
							}
						}),
					(new EmptyView(0, 0, 320, SMALL_MARGIN)),
					(oauth_user_view = (new SuccinctChannelView(0, 0, 320, 0))
						->set_name("")
						->set_auxiliary_lines({})
						->set_thumbnail_url("")),
					(new EmptyView(0, 0, 320, SMALL_MARGIN)),
					(new TextView(10, 0, 120, DEFAULT_FONT_INTERVAL + SMALL_MARGIN * 2))
						->set_text([] () {
							if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATED) {
								return LOCALIZED(OAUTH_LOGOUT);
							} else if (OAuth::oauth_state == OAuth::OAuthState::ERROR) {
								return LOCALIZED(RETRY);
							} else {
								return LOCALIZED(OAUTH_LOGIN);
							}
						})
						->set_x_alignment(TextView::XAlign::CENTER)
						->set_text_offset(0, -2)
						->set_get_text_color([] () {
							return OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATED ? DEFAULT_TEXT_COLOR : POCKET_ON_ACCENT;
						})
						->set_get_background_color([] (const View &view) {
							if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATED) {
								return View::STANDARD_BUTTON_BACKGROUND(view); // logout: neutral
							}
							return View::blend_color(POCKET_ACCENT, POCKET_PRESSED, view.touch_darkness * 0.5); // login: primary
						})
						->set_on_view_released([] (View &) {
							if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATED) {
								OAuth::revoke_tokens();
								var_oauth_enabled = false;
								misc_tasks_request(TASK_SAVE_SETTINGS);

								if (oauth_user_view) {
									oauth_user_view->set_name("");
									oauth_user_view->set_auxiliary_lines({});
									oauth_user_view->set_thumbnail_url("");
									oauth_user_view->thumbnail_handle = -1;
									oauth_user_view->set_height(0);
								}
							} else if (OAuth::oauth_state == OAuth::OAuthState::ERROR) {
								OAuth::refresh_access_token();
							} else if (OAuth::oauth_state == OAuth::OAuthState::NOT_AUTHENTICATED) {
								OAuth::start_device_flow();
								if (OAuth::oauth_state == OAuth::OAuthState::AUTHENTICATING) {
									auto cancel_auth = [] () {
										oauth_check_timer_active = false;
										oauth_timeout_counter = 0;
										OAuth::revoke_tokens();
									};
									
									std::string message = LOCALIZED(OAUTH_LOGIN_INSTRUCTION) + "\n\n" +
										std::regex_replace(LOCALIZED(OAUTH_USER_CODE), std::regex("%0"), OAuth::user_code) + "\n" +
										LOCALIZED(OAUTH_VERIFICATION_URL) + "\n\n" + LOCALIZED(OAUTH_WAITING);
									
									popup_view->get_message_view()->set_text_lines(split_string(message, '\n'))->update_y_range(0, 120);
									popup_view->set_buttons<std::function<std::string ()> >({
										[] () { return LOCALIZED(CANCEL); }
									}, [cancel_auth] (OverlayDialogView &, int) {
										cancel_auth();
										return true;
									});
									popup_view->set_on_cancel([cancel_auth](OverlayView &view) {
										cancel_auth();
										view.set_is_visible(false);
										var_need_refresh = true;
									});
									popup_view->set_is_visible(true);
									oauth_check_timer_active = true;
									oauth_timeout_counter = oauth_check_frames = 0;
								}
							}
							var_need_refresh = true;
						}),
					(new EmptyView(0, 0, 320, 10)),
				})
		}, 0)
		->set_tab_texts<std::function<std::string ()> >({
			[] () { return LOCALIZED(SETTINGS_DISPLAY_UI); },
			[] () { return LOCALIZED(PLAYBACK); },
			[] () { return LOCALIZED(SETTINGS_DATA); },
			[] () { return LOCALIZED(UPDATE); },
			[] () { return LOCALIZED(SETTINGS_ADVANCED); }
		});
	main_view = (new VerticalListView(0, 0, 320))
		->set_views({
			// 'Settings'
			(new TextView(0, 0, 320, TOP_HEIGHT))
				->set_text((std::function<std::string ()>) [] () { return LOCALIZED(SETTINGS); })
				->set_font_size(MIDDLE_FONT_SIZE, MIDDLE_FONT_INTERVAL)
				->set_text_offset(7, -1)
				->set_get_background_color([] (const View &) { return DEFAULT_BACK_COLOR; }),
			(new EmptyView(0, 0, 320, 0)),
			main_tab_view
		})
		->set_draw_order({2, 1, 0});
		
	
	oauth_worker_thread = threadCreate(oauth_worker_thread_func, (void*)(""), DEF_STACKSIZE, DEF_THREAD_PRIORITY_LOW, 1, false);
	
	// Initialize OAuth
	OAuth::init();
	if (OAuth::is_authenticated()) {
		var_oauth_enabled = true;

		std::string user_name = OAuth::get_user_account_name();
		std::string handle = OAuth::get_user_handle();
		std::string subscriber_count = OAuth::get_user_subscriber_count();
		std::string photo_url = OAuth::get_user_photo_url();
		
		if (oauth_user_view && !user_name.empty()) {
			std::vector<std::string> aux_lines;
			if (!handle.empty()) aux_lines.push_back(handle);
			if (!subscriber_count.empty()) aux_lines.push_back(subscriber_count);
			
			oauth_user_view->set_name(user_name);
			oauth_user_view->set_auxiliary_lines(aux_lines);
			oauth_user_view->set_thumbnail_url(photo_url);
			oauth_user_view->set_height(CHANNEL_ICON_HEIGHT);
			if (!photo_url.empty()) {
				oauth_user_view->thumbnail_handle = thumbnail_request(photo_url, SceneType::SETTINGS, PRIORITY_FOREGROUND, ThumbnailType::ICON);
			}
		}
	}
	
	show_info_popup_callback = show_info_popup;
	
	Sem_resume("");
	already_init = true;
}
void Sem_exit(void) {
	already_init = false;
	thread_suspend = false;
	exiting = true;
	
	save_settings();
	OAuth::exit();
	
	u64 time_out = 10000000000;
	logger.info(DEF_MENU_EXIT_STR, "oauth threadJoin()...", threadJoin(oauth_worker_thread, time_out));
	threadFree(oauth_worker_thread);
	
	main_view->recursive_delete_subviews();
	delete main_view;
	main_view = NULL;
	main_tab_view = NULL;
	
	logger.info("settings/exit", "Exited.");
}
void Sem_suspend(void) {
	thread_suspend = true;
}
void Sem_resume(std::string arg) {
	overlay_menu_on_resume();
	if (arg == "account" && main_tab_view && main_tab_view->views.size() >= 5) {
		main_tab_view->selected_tab = 4; // You > Account: sign in / out lives in Advanced
	}
	thread_suspend = false;
	var_need_refresh = true;
}


void Sem_draw(void)
{
	Hid_info key;
	Util_hid_query_key_state(&key);
	
	thumbnail_set_active_scene(SceneType::SETTINGS);
	
	bool video_playing_bar_show = video_is_playing();
	CONTENT_Y_HIGH = shell::content_bottom(video_playing_bar_show);
	toast_view->y0 = CONTENT_Y_HIGH - toast_view->get_height() - 6, toast_view->y1 = CONTENT_Y_HIGH - 6;
	shell::set_top_hints({{"L R", shell::tr("HINT_TABS", "Tabs")},
	                      {"B", shell::tr("HINT_BACK", "Back")},
	                      {"START", shell::tr("HINT_MENU", "Menu")}});
	main_tab_view->update_y_range(0, CONTENT_Y_HIGH - TOP_HEIGHT);
	
	if(var_need_refresh || !var_eco_mode)
	{
		var_need_refresh = false;
		Draw_frame_ready();
		video_draw_top_screen();
		
		Draw_screen_ready(1, DEFAULT_BACK_COLOR);
		
		resource_lock.lock();
		main_view->draw();
		resource_lock.unlock();
		
		if (video_playing_bar_show) video_draw_playing_bar();
		draw_overlay_menu(CONTENT_Y_HIGH);
		resource_lock.lock();
		popup_view->draw(); // sheets and toasts above the nav
		toast_view->draw();
		resource_lock.unlock();
		
		if(Util_expl_query_show_flag())
			Util_expl_draw();

		if(Util_err_query_error_show_flag())
			Util_err_draw();

		Draw_touch_pos();

		Draw_apply_draw();
	}
	else
		gspWaitForVBlank();
	
	if (--toast_frames_left <= 0) toast_view->set_is_visible(false);

	if (Util_err_query_error_show_flag()) {
		Util_err_main(key);
	} else if(Util_expl_query_show_flag()) {
		Util_expl_main(key);
	} else {
		resource_lock.lock();
		bool popup_open = popup_view->is_visible; // the open sheet owns every input: no nav, You sheet, mini-player or Back
		resource_lock.unlock();
		bool taken = !popup_open && update_overlay_menu(&key);
		if (popup_open) shell::reserve_global_chords(&key);

		// toast_view is never 'updated'
		resource_lock.lock();
		if (popup_open) popup_view->update(key);
		else if (taken) {
			main_view->reset_holding_status();
			video_cancel_playing_bar_gesture();
		} else main_view->update(key);
		resource_lock.unlock();
		if (popup_open) return; // B closed the sheet or did nothing: no Back, no mini-player

		if (video_playing_bar_show) video_update_playing_bar(key);
		
		if (key.p_b) global_intent.next_scene = SceneType::BACK;
	}
}
