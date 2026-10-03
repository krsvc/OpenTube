#include "ui/overlay.hpp"
#include "variables.hpp"
#include "headers.hpp"
#include "ui/ui.hpp"
#include "ui/shell.hpp"
#include "downloads/downloads_app.hpp"
#include "data_io/liked_videos_app.hpp"
#include "scenes/video_player.hpp"

// OpenTube r13: the r12d hamburger menu (Home / Search / History / Downloads / Exit / Settings / About) is replaced by
// the bottom nav (Home, Subs, Search, Downloads, You) and the You sheet (Watch history, Settings, Account, About,
// Exit + confirmation). Every scene already drew and updated this overlay, so they all get the shell here.
// The `y` argument (old menu icon position) is no longer used: the nav has a fixed place at the bottom edge.

void draw_overlay_menu(int y) {
	(void)y;
	if (global_current_scene != SceneType::VIDEO_PLAYER) {
		shell::nav_draw(downloads_running() && downloads_manager().busy());
	}
	shell::sheets_draw();
}
bool update_overlay_menu(Hid_info *key) {
	static SceneType prev_scene = SceneType::SEARCH;
	if (prev_scene != global_current_scene) {
		var_need_refresh = true;
		prev_scene = global_current_scene;
	}
	// r15: toasts (liked videos) in every scene; a failed save was already reverted in memory: say so
	shell::clock_ms = osGetTime;
	shell::video_on_top = video_playback_active; // r16: one-tap open + toast above the mini-player while one is loaded
	std::string liked_error = liked_take_error();
	if (!liked_error.empty()) {
		shell::toast("Not saved: " + liked_error);
	}
	if (shell::toast_tick()) {
		var_need_refresh = true;
	}
	shell::shell_update(key);
	return shell::input_taken();
}
void close_overlay_menu() { shell::on_scene_resume(); }
void overlay_menu_on_resume() { shell::on_scene_resume(); }
