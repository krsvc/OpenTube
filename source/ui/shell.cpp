#include "ui/shell.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include "ui/modern.hpp"
#include "ui/colors.hpp"
#include "ui/draw/draw.hpp"
#include "data_io/string_resource.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include "variables.hpp"

namespace shell {

using namespace modern;

int home_tab = 0;
int home_tab_request = -1;
int last_subs_tab = 2;
const void *cue_owner = nullptr;
u32 cur_ground = 0;
u32 ground() { return cur_ground ? cur_ground : DEFAULT_BACK_COLOR; }

std::string tr(const char *key, const char *fallback) {
	std::string s = get_string_resource(key);
	if (s.empty() || s[0] == '[') { // "[SR Not Found]" / "[SR Null Err]": key missing in this language file
		return fallback;
	}
	return s;
}

// ---- destinations -------------------------------------------------------------------------------------------------
Dest active_dest() {
	switch (global_current_scene) {
	case SceneType::HOME:
		return home_tab == 0 ? Dest::HOME : Dest::SUBS;
	case SceneType::SEARCH:
		return Dest::SEARCH;
	case SceneType::DOWNLOADS:
		return Dest::DOWNLOADS;
	case SceneType::HISTORY:
	case SceneType::LIKED:
	case SceneType::SETTINGS:
	case SceneType::ABOUT:
		return Dest::YOU;
	default:
		return Dest::NONE;
	}
}
std::string dest_label(Dest d) {
	switch (d) {
	case Dest::HOME:
		return tr("HOME", "Home");
	case Dest::SUBS:
		return tr("NAV_SUBS", "Subs");
	case Dest::SEARCH:
		return tr("GOTO_SEARCH", "Search");
	case Dest::DOWNLOADS:
		return tr("NAV_DOWNLOADS", "Downloads");
	case Dest::YOU:
		return tr("NAV_YOU", "You");
	default:
		return "";
	}
}
std::string screen_title() {
	switch (global_current_scene) {
	case SceneType::SEARCH:
		return tr("GOTO_SEARCH", "Search");
	case SceneType::DOWNLOADS:
		return tr("NAV_DOWNLOADS", "Downloads");
	case SceneType::HISTORY:
		return tr("WATCH_HISTORY", "Watch History");
	case SceneType::LIKED:
		return tr("LIKED_VIDEOS", "Liked videos");
	case SceneType::SETTINGS:
		return tr("SETTINGS", "Settings");
	case SceneType::ABOUT:
		return tr("ABOUT", "About");
	default:
		return "OpenTube";
	}
}

float nav_col_w() { return 320.0f / DEST_COUNT; }
float nav_col_x(int i) { return i * nav_col_w(); }

static int nav_holding = -1;
static const Icon NAV_ICONS[DEST_COUNT][2] = {{Icon::HOME, Icon::HOME_FILLED},
                                               {Icon::SUBS, Icon::SUBS_FILLED},
                                               {Icon::SEARCH, Icon::SEARCH},
                                               {Icon::DOWNLOAD, Icon::DOWNLOAD_FILLED},
                                               {Icon::YOU, Icon::YOU_FILLED}};

void nav_draw(bool downloads_busy) {
	const ThemeTokens &t = theme_cur();
	rect(0, NAV_Y, 320, NAV_H, t.surf);
	rect(0, NAV_Y, 320, 1, t.line);
	Dest act = active_dest();
	for (int i = 0; i < DEST_COUNT; i++) {
		float cx = nav_col_x(i) + nav_col_w() / 2;
		bool on = (int)act == i;
		if (on) {
			pill(cx - 19, NAV_Y + 4, 38, 20, t.acc_soft);
		} else if (nav_holding == i) {
			pill(cx - 19, NAV_Y + 4, 38, 20, t.surf2);
		}
		icon(NAV_ICONS[i][on], cx, NAV_Y + 14, 17, on ? t.acc_tx : t.tx2, on ? t.acc_soft_on_bg : t.surf);
		std::string label = ellipsize(dest_label((Dest)i), nav_col_w() - 4, T_KEY);
		text_center(label, nav_col_x(i), nav_col_x(i + 1), NAV_Y + 25, T_KEY, on ? t.tx : t.tx2);
		if ((i == (int)Dest::DOWNLOADS && downloads_busy) || (i == (int)Dest::YOU && you_badge)) {
			circle(cx + 12, NAV_Y + 6, 5, t.surf); // DOWNLOADS: a transfer is queued or running; YOU: an update
			circle(cx + 12, NAV_Y + 6, 3.5f, t.acc);
		}
	}
}

static void go(Dest d) {
	switch (d) {
	case Dest::HOME:
	case Dest::SUBS:
		home_tab_request = d == Dest::HOME ? 0 : last_subs_tab;
		if (global_current_scene != SceneType::HOME) {
			global_intent.next_scene = SceneType::HOME;
			global_intent.arg = "";
		}
		break;
	case Dest::SEARCH:
		if (global_current_scene != SceneType::SEARCH) {
			global_intent.next_scene = SceneType::SEARCH;
			global_intent.arg = "";
		}
		break;
	case Dest::DOWNLOADS:
		if (global_current_scene != SceneType::DOWNLOADS) {
			global_intent.next_scene = SceneType::DOWNLOADS;
			global_intent.arg = "";
		}
		break;
	case Dest::YOU:
		open_you_sheet();
		break;
	default:
		break;
	}
	var_need_refresh = true;
}

// ---- sheets -------------------------------------------------------------------------------------------------------
enum class SheetState { NONE, YOU, CONFIRM_EXIT };
static SheetState sheet_state = SheetState::NONE;
static int you_focus = 0;
static int you_holding = -1;   // item index, -2 = close pill, -3 = scrim
static int confirm_holding = -1;
static bool exit_confirmed = false;

// r16b: the You sheet drags down from its header band like the player's sheets (r16 Deck): >= DRAG_CLOSE_PX on release
// closes with a SETTLE_FRAMES ease-out, less settles back; while grabbed / settling the sheet owns every touch
static constexpr int DRAG_CLOSE_PX = 36, SETTLE_FRAMES = 6;
static struct YouDrag {
	bool grabbed = false, closing = false;
	int start_y = 0, settle = 0;
	float dy = 0, from = 0;
} you_drag;
static bool you_swallow = false; // a finger still down when the settle closed the sheet: consumed until lifted
static float you_drag_offset() {
	if (you_drag.grabbed) {
		return you_drag.dy;
	}
	if (you_drag.settle > 0) {
		float t = 1 - (float)you_drag.settle / SETTLE_FRAMES, e = 1 - (1 - t) * (1 - t) * (1 - t);
		float travel = you_sheet_rect().h; // closing: the sheet leaves the screen
		return you_drag.closing ? you_drag.from + (travel - you_drag.from) * e : you_drag.from * (1 - e);
	}
	return 0;
}
bool sheet_open() { return sheet_state != SheetState::NONE; }
bool you_sheet_is_open() { return sheet_state == SheetState::YOU; }
bool exit_confirm_is_open() { return sheet_state == SheetState::CONFIRM_EXIT; }
void open_you_sheet() {
	sheet_state = SheetState::YOU;
	you_focus = 0; // the D-pad starts at the first item
	you_holding = -1;
	you_drag = YouDrag();
	nav_holding = -1; // a nav touch held while START opens the sheet never navigates on its release
	var_need_refresh = true;
}
void on_scene_resume() {
	sheet_state = SheetState::NONE;
	nav_holding = you_holding = confirm_holding = -1;
	you_drag = YouDrag();
	preview_clear();
	var_need_refresh = true;
}

static constexpr float YOU_ROW_H = 28; // r15: 6 rows, sheet top y 28 (the backdrop band above stays tappable)
Rect you_sheet_rect() {
	float h = 36 + YOU_ITEM_COUNT * YOU_ROW_H + 8;
	return {0, 240 - h, 320, h};
}
Rect you_item_rect(int i) {
	Rect s = you_sheet_rect();
	return {6, s.y + 36 + i * YOU_ROW_H, 308, YOU_ROW_H - 2};
}
// r16b: as wide as its "B Close" group needs (German "Schließen" overflowed the fixed 62 px), 62 px at least; the same
// rect is drawn and hit-tested
Rect sheet_close_rect(const Rect &sheet) {
	float w = std::max(62.0f, 8 + 15 + 4 + text_w(tr("CLOSE", "Close"), T_META) + 8);
	return {sheet.x + sheet.w - 10 - w, sheet.y + 9, w, 22};
}
static constexpr float CONFIRM_H = 128;
Rect confirm_button_rect(int i) { return {12.0f + i * 152, 240 - 12 - 34, 144, 34}; }

void draw_sheet_backdrop(float top) {
	const ThemeTokens &t = theme_cur();
	rect(0, 0, 320, top, POCKET_SCRIM);
	rect(0, top - 3, 320, 3, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 3)); // sheet lift
}
float draw_sheet_frame(float top, const std::string &title, const std::string &count, bool close_pill) {
	draw_sheet_backdrop(top);
	round_rect_top(0, top, 320, 240 - top, 16, theme_cur().surf);
	draw_sheet_header(top, title, count, close_pill);
	return top + 36;
}
void draw_sheet_header(float top, const std::string &title, const std::string &count, bool close_pill) {
	const ThemeTokens &t = theme_cur();
	pill(144, top + 6, 32, 4, t.line);
	text(title, 10, text_y(top + 9, 22, T_TITLE), T_TITLE, t.tx);
	if (!count.empty()) {
		text(count, 10 + text_w(title, T_TITLE) + 6, text_y(top + 9, 22, T_META), T_META, t.tx2);
	}
	if (close_pill) {
		Rect c = sheet_close_rect({0, top, 320, 240 - top});
		pill(c.x, c.y, c.w, c.h, t.surf2);
		// r16b: the chip + label group centered on the pill (it sat 4 px left of center)
		std::string label = tr("CLOSE", "Close");
		float x = std::floor(c.x + (c.w - (15 + 4 + text_w(label, T_META))) / 2 + 0.5f);
		x += key_chip("B", x, c.y + 3.5f, t.kc, t.tx) + 4;
		text(label, x, text_y(c.y, c.h, T_META), T_META, t.tx);
	}
	cur_ground = t.surf;
}
bool sheet_should_close(const Hid_info &key, float top, bool close_pill) {
	if (key.p_b) {
		return true;
	}
	if (key.p_touch && key.touch_y >= 0 && key.touch_y < top) {
		return true; // backdrop
	}
	return close_pill && key.p_touch && sheet_close_rect({0, top, 320, 240 - top}).contains(key.touch_x, key.touch_y);
}

bool you_badge = false;
static Icon you_icon(YouItem item) {
	switch (item) {
	case YouItem::HISTORY:
		return Icon::HISTORY;
	case YouItem::LIKED:
		return Icon::LIKE;
	case YouItem::SETTINGS:
		return Icon::SETTINGS;
	case YouItem::ACCOUNT:
		return Icon::ACCOUNT;
	case YouItem::ABOUT:
		return Icon::INFO;
	default:
		return Icon::EXIT;
	}
}
static std::string you_label(YouItem item) {
	switch (item) {
	case YouItem::HISTORY:
		return tr("WATCH_HISTORY", "Watch History");
	case YouItem::LIKED:
		return tr("LIKED_VIDEOS", "Liked videos");
	case YouItem::SETTINGS:
		return tr("SETTINGS", "Settings");
	case YouItem::ACCOUNT:
		return tr("ACCOUNT", "Account");
	case YouItem::ABOUT:
		return tr("ABOUT", "About");
	default:
		return tr("EXIT_APP", "Exit the app");
	}
}
static void draw_you_sheet() {
	const ThemeTokens &t = theme_cur();
	const float shift = std::floor(you_drag_offset()); // r16b: the whole sheet follows a drag / settle
	Rect s = you_sheet_rect();
	draw_sheet_frame(s.y + shift, tr("NAV_YOU", "You"), "");
	for (int i = 0; i < YOU_ITEM_COUNT; i++) {
		Rect r = you_item_rect(i);
		r.y += shift;
		bool focus = i == you_focus, hold = i == you_holding;
		if (focus || hold) {
			round_rect(r.x, r.y, r.w, r.h, 10, hold ? t.pressed : t.surf2);
		}
		YouItem item = (YouItem)i;
		icon(you_icon(item), r.x + 18, r.y + r.h / 2, 17, item == YouItem::EXIT ? t.err : t.tx2);
		text(you_label(item), r.x + 36, text_y(r.y, r.h, T_ROW), T_ROW, t.tx);
		if (item == YouItem::SETTINGS && you_badge) { // an update was found (Settings > Update)
			circle(r.x + 36 + text_w(you_label(item), T_ROW) + 7, r.y + r.h / 2 - 5, 3.5f, t.acc);
		}
		if (focus) {
			key_chip("A", r.x + r.w - 21, r.y + (r.h - 15) / 2, t.acc, t.acc_ink);
		}
	}
	cur_ground = 0;
}
static void draw_confirm_sheet() {
	const ThemeTokens &t = theme_cur();
	float top = 240 - CONFIRM_H;
	draw_sheet_frame(top, tr("EXIT_APP", "Exit the app"), "", false);
	std::vector<std::string> lines = wrap(tr("EXIT_CONFIRM", "Are you sure you want to exit?"), 296, T_BODY, 2);
	for (size_t i = 0; i < lines.size(); i++) {
		text(lines[i], 12, top + 40 + i * 15, T_BODY, t.tx2);
	}
	for (int i = 0; i < 2; i++) {
		Rect b = confirm_button_rect(i);
		u32 fill = i == 1 ? t.err : t.surf2, ink = i == 1 ? t.err_ink : t.tx;
		if (confirm_holding == i) {
			fill = theme_blend(fill, ink, 0.15f);
		}
		pill(b.x, b.y, b.w, b.h, fill);
		std::string label = i == 0 ? tr("CANCEL", "Cancel") : tr("EXIT_APP", "Exit the app");
		std::string key = i == 0 ? "B" : "A";
		label = ellipsize(label, b.w - 40, T_ROW);
		float w = 15 + 5 + text_w(label, T_ROW), x = b.x + (b.w - w) / 2;
		key_chip(key, x, b.y + (b.h - 15) / 2, i == 1 ? THEME_ALPHA(t.err_ink, 0x33) : t.kc, ink);
		text(label, x + 20, text_y(b.y, b.h, T_ROW), T_ROW, ink);
	}
	cur_ground = 0;
}
static u64 no_clock() { return 0; }
u64 (*clock_ms)() = no_clock;
static std::string toast_text;
static u64 toast_start = 0;
static bool toast_was_visible = false;
static constexpr u64 TOAST_MS = 3000;
void toast(const std::string &text) {
	toast_text = text;
	toast_start = clock_ms();
	var_need_refresh = true;
}
bool toast_visible() { return !toast_text.empty() && clock_ms() - toast_start < TOAST_MS; }
bool toast_tick() {
	bool visible = toast_visible(), res = visible || toast_was_visible;
	toast_was_visible = visible;
	if (!visible) {
		toast_text.clear();
	}
	return res;
}
static void draw_toast() {
	if (!toast_visible()) {
		return;
	}
	const ThemeTokens &t = theme_cur();
	// above the nav in browse scenes (r16: above the mini-player card when it is shown, so this text-only pill is never
	// read as the card); at the bottom edge in the player (it has no nav)
	float y = global_current_scene == SceneType::VIDEO_PLAYER ? 240 - 26 : video_on_top() ? MINI_Y - 26 : NAV_Y - 26;
	std::string line = ellipsize(toast_text, 290, T_META);
	float w = text_w(line, T_META) + 20;
	pill((320 - w) / 2, y, w, 22, t.tx);
	text_center(line, 0, 320, text_y(y, 22, T_META), T_META, t.bg);
}
void sheets_draw() {
	if (sheet_state == SheetState::YOU) {
		draw_you_sheet();
	} else if (sheet_state == SheetState::CONFIRM_EXIT) {
		draw_confirm_sheet();
	}
	draw_toast();
}

static bool frame_taken = false;
bool input_taken() { return frame_taken; }
static void consume(Hid_info *key) {
	Hid_info empty;
	empty.touch_x = empty.touch_y = -1;
	*key = empty;
	frame_taken = true;
}
void reserve_global_chords(Hid_info *key) {
	if (key->h_select) {
		key->p_a = key->p_x = key->p_y = false;
	}
}
static void you_activate(int i) {
	sheet_state = SheetState::NONE;
	switch ((YouItem)i) {
	case YouItem::HISTORY:
		if (global_current_scene != SceneType::HISTORY) {
			global_intent.next_scene = SceneType::HISTORY, global_intent.arg = "";
		}
		break;
	case YouItem::LIKED:
		if (global_current_scene != SceneType::LIKED) {
			global_intent.next_scene = SceneType::LIKED, global_intent.arg = "";
		}
		break;
	case YouItem::SETTINGS:
		if (global_current_scene != SceneType::SETTINGS) {
			global_intent.next_scene = SceneType::SETTINGS, global_intent.arg = "";
		}
		break;
	case YouItem::ACCOUNT: // account: sign in / out lives in Settings > Advanced
		global_intent.next_scene = SceneType::SETTINGS, global_intent.arg = "account";
		break;
	case YouItem::ABOUT:
		if (global_current_scene != SceneType::ABOUT) {
			global_intent.next_scene = SceneType::ABOUT, global_intent.arg = "";
		}
		break;
	case YouItem::EXIT:
		sheet_state = SheetState::CONFIRM_EXIT;
		confirm_holding = -1;
		break;
	}
	var_need_refresh = true;
}

bool shell_update(Hid_info *key) {
	static int last_touch_x = -1, last_touch_y = -1;
	bool released = key->touch_x == -1 && last_touch_x != -1;
	int rx = last_touch_x, ry = last_touch_y;
	last_touch_x = key->touch_x, last_touch_y = key->touch_y;
	frame_taken = false;
	reserve_global_chords(key);

	if (exit_confirmed) {
		exit_confirmed = false;
		global_intent.next_scene = SceneType::EXIT;
		global_intent.arg = "";
		consume(key);
		return true;
	}
	if (sheet_state == SheetState::CONFIRM_EXIT) {
		var_need_refresh = true;
		if (key->p_a) {
			exit_confirmed = true; // committed next frame, after this frame's scene update saw no input
		} else if (key->p_b) {
			sheet_state = SheetState::YOU; // same as r12d: cancel returns to the menu
		} else {
			if (key->p_touch) {
				confirm_holding = -1;
				for (int i = 0; i < 2; i++) {
					if (confirm_button_rect(i).contains(key->touch_x, key->touch_y)) {
						confirm_holding = i;
					}
				}
				if (confirm_holding == -1 && key->touch_y < 240 - CONFIRM_H) {
					confirm_holding = -3;
				}
			}
			if (released && confirm_holding >= 0 && confirm_button_rect(confirm_holding).contains(rx, ry)) {
				if (confirm_holding == 1) {
					exit_confirmed = true;
				} else {
					sheet_state = SheetState::YOU;
				}
			} else if (released && confirm_holding == -3 && ry < 240 - CONFIRM_H) {
				sheet_state = SheetState::YOU;
			}
			if (released) {
				confirm_holding = -1;
			}
		}
		consume(key);
		return true;
	}
	if (sheet_state == SheetState::YOU) {
		var_need_refresh = true;
		Rect s = you_sheet_rect();
		if (key->p_b || (key->p_start && !key->h_select)) {
			sheet_state = SheetState::NONE; // also ends a drag / settle
			you_drag = YouDrag();
		} else if (you_drag.settle > 0) { // r16b: settling after a drag: nothing else reacts
			if (--you_drag.settle == 0) {
				if (you_drag.closing) {
					sheet_state = SheetState::NONE;
					you_swallow = key->touch_x != -1;
				}
				you_drag = YouDrag();
			}
		} else if (you_drag.grabbed) {
			if (key->touch_x != -1) {
				you_drag.dy = std::max(0, key->touch_y - you_drag.start_y);
			} else { // released: close past the threshold, else settle back
				you_drag.grabbed = false;
				you_drag.closing = you_drag.dy >= DRAG_CLOSE_PX;
				you_drag.from = you_drag.dy;
				you_drag.settle = SETTLE_FRAMES;
			}
		} else if (key->p_touch && key->touch_y >= s.y && key->touch_y < s.y + 36 &&
		           !sheet_close_rect(s).contains(key->touch_x, key->touch_y)) {
			you_drag.grabbed = true; // the header band (title, grab handle), not the Close pill or a row
			you_drag.start_y = key->touch_y;
			you_holding = -1;
		} else if (key->p_d_down || key->p_d_up) {
			you_focus = (you_focus + (key->p_d_down ? 1 : YOU_ITEM_COUNT - 1)) % YOU_ITEM_COUNT;
		} else if (key->p_a) {
			you_activate(you_focus);
		} else {
			if (key->p_touch) {
				you_holding = -1;
				for (int i = 0; i < YOU_ITEM_COUNT; i++) {
					if (you_item_rect(i).contains(key->touch_x, key->touch_y)) {
						you_holding = i;
					}
				}
				if (sheet_close_rect(s).contains(key->touch_x, key->touch_y)) {
					you_holding = -2;
				} else if (key->touch_y < s.y) {
					you_holding = -3;
				}
			}
			if (released) {
				if (you_holding >= 0 && you_item_rect(you_holding).contains(rx, ry)) {
					you_focus = you_holding;
					you_activate(you_holding);
				} else if ((you_holding == -2 && sheet_close_rect(s).contains(rx, ry)) || (you_holding == -3 && ry < s.y)) {
					sheet_state = SheetState::NONE;
				}
				you_holding = -1;
			}
		}
		consume(key);
		return true;
	}

	if (you_swallow) { // r16b: the finger that was on the sheet when it closed: nothing below reacts to it
		if (key->touch_x == -1) {
			you_swallow = false;
		} else {
			consume(key);
			return true;
		}
	}
	// closed: START opens the You sheet, the nav takes touches that start on it
	if (key->p_start && !key->h_select) {
		open_you_sheet();
		consume(key);
		return true;
	}
	if (global_current_scene == SceneType::VIDEO_PLAYER) {
		return false; // the player has no nav
	}
	bool consumed = false;
	if (key->p_touch && key->touch_y >= NAV_Y) {
		nav_holding = std::min(DEST_COUNT - 1, std::max(0, (int)(key->touch_x / nav_col_w())));
		var_need_refresh = true;
	}
	if (nav_holding != -1) {
		if (key->touch_x != -1 && (key->touch_y < NAV_Y || (int)(key->touch_x / nav_col_w()) != nav_holding)) {
			nav_holding = -1; // slid off
			var_need_refresh = true;
		} else if (released) {
			int d = nav_holding;
			nav_holding = -1;
			go((Dest)d);
		}
		consumed = true;
		// only the touch belongs to the nav; buttons still reach the scene
		key->touch_x = key->touch_y = -1;
		key->p_touch = key->h_touch = false;
	}
	return consumed;
}

// ---- cue ----------------------------------------------------------------------------------------------------------
bool cue_enabled() { return global_current_scene != SceneType::VIDEO_PLAYER; }
static bool no_video() { return false; }
bool (*video_on_top)() = no_video;
bool cue_tap(const void *row) {
	if (cue_owner == row) {
		return true;
	}
	if (video_on_top()) {
		cue_owner = row;
		return true;
	}
	cue_owner = row;
	var_need_refresh = true;
	return false;
}
void cue_set(const void *row) {
	if (cue_owner != row) {
		var_need_refresh = true;
	}
	cue_owner = row;
}

// ---- top screen ---------------------------------------------------------------------------------------------------
bool Preview::operator==(const Preview &o) const {
	return kind == o.kind && title == o.title && channel == o.channel && meta == o.meta && duration == o.duration &&
	       thumbnail_url == o.thumbnail_url && round_thumbnail == o.round_thumbnail && big == o.big &&
	       big_sub == o.big_sub && state_color == o.state_color && std::fabs(progress - o.progress) < 0.001f &&
	       meta_lines == o.meta_lines && footer_title == o.footer_title && footer_left == o.footer_left &&
	       footer_right == o.footer_right;
}
static Preview cur_preview;
static bool has_preview = false;
static SceneType preview_scene = SceneType::HOME;
static int hero_handle = -1; // the shell's OWN request for the cued URL (never a row's handle)
// r14: in browse scenes a video's hero asks for its own larger image (mqdefault, 320x180) instead of enlarging the
// row's 120x68 default.jpg; the row URL stays the fallback until (or if never) the larger one is loaded
static int hero_large_handle = -1;
static std::string hero_large_url(const Preview &p) {
	static const std::string prefix = "https://i.ytimg.com/vi/", suffix = "/default.jpg";
	const std::string &u = p.thumbnail_url;
	if (!cue_enabled() || p.kind != Preview::Kind::VIDEO || p.round_thumbnail ||
	    u.size() != prefix.size() + 11 + suffix.size() || u.compare(0, prefix.size(), prefix) != 0 ||
	    u.compare(u.size() - suffix.size(), suffix.size(), suffix) != 0) {
		return "";
	}
	std::string id = u.substr(prefix.size(), 11);
	for (char c : id) {
		if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) {
			return "";
		}
	}
	return prefix + id + "/mqdefault.jpg";
}
static void hero_cancel() {
	for (int *h : {&hero_handle, &hero_large_handle}) {
		if (*h != -1) {
			thumbnail_cancel_request(*h);
		}
		*h = -1;
	}
}
static std::vector<std::string> hero_title;
static std::vector<std::pair<std::string, std::string>> top_hints;

void preview_set(const Preview &p) {
	if (has_preview && preview_scene == global_current_scene && cur_preview == p) {
		return;
	}
	bool new_image = !has_preview || preview_scene != global_current_scene || p.thumbnail_url != cur_preview.thumbnail_url ||
	                 p.round_thumbnail != cur_preview.round_thumbnail;
	cur_preview = p;
	has_preview = true;
	preview_scene = global_current_scene;
	hero_title = wrap(p.title, 134, T_HEADLINE, p.kind == Preview::Kind::DOWNLOAD ? 3 : 4);
	if (new_image) {
		hero_cancel();
		if (!p.thumbnail_url.empty()) {
			hero_handle = thumbnail_request(p.thumbnail_url, global_current_scene, PRIORITY_FOREGROUND,
			                                p.round_thumbnail ? ThumbnailType::ICON : ThumbnailType::VIDEO_THUMBNAIL);
		}
		std::string large = hero_large_url(p);
		if (!large.empty()) {
			hero_large_handle =
			    thumbnail_request(large, global_current_scene, PRIORITY_FOREGROUND, ThumbnailType::VIDEO_PREVIEW);
		}
	}
	var_need_refresh = true;
}
void preview_clear() {
	hero_cancel();
	has_preview = false;
	cur_preview = Preview();
	hero_title.clear();
	var_need_refresh = true;
}
const Preview &preview_current() { return cur_preview; }
bool preview_visible() {
	return has_preview && preview_scene == global_current_scene && cur_preview.kind != Preview::Kind::NONE;
}
void set_top_hints(const std::vector<std::pair<std::string, std::string>> &hints) { top_hints = hints; }

// the reference glow: ellipse near (34 %, 42 %) of the top screen, faded into the ground from 35 % down
static constexpr float GLOW_CX = 136, GLOW_CY = 101, GLOW_RX = 176, GLOW_RY = 150, FADE_Y = 84;
static u32 glow_color() {
	const ThemeTokens &t = theme_cur();
	return THEME_ALPHA(t.amb_tint, (u32)(t.amb * t.tint_op * 0.62f * 255));
}
static u32 top_ground_at(float x, float y) {
	const ThemeTokens &t = theme_cur();
	u32 g = glow_at(GLOW_CX, GLOW_CY, GLOW_RX, GLOW_RY, glow_color(), t.top, x, y);
	if (y > FADE_Y) {
		g = over(g, THEME_ALPHA(t.top, (u32)(0.85f * 255 * std::min(1.0f, (y - FADE_Y) / (240 - FADE_Y)))));
	}
	return g;
}

void status_bar(bool video_mode, const std::string &label) {
	const ThemeTokens &t = theme_cur();
	float h = video_mode ? 15 : 18;
	u32 ink = video_mode ? 0xFFE9E9E9u : t.tx;
	if (video_mode) {
		rect(0, 0, 400, h, 0xFF000000u);
	}
	logo_tile(video_mode ? 8 : 10, (h - 11) / 2, 11);
	text(ellipsize(label, 230, T_META), video_mode ? 31 : 32, text_y(0, h, T_META), T_META, ink);
	float x = 400 - 10 - 19;
	battery(x, (h - 9) / 2, var_battery_level_raw, var_battery_charge, ink);
	if (var_battery_level_exact) { // r16: MCUHWC's percentage; PTMU's coarse fallback shows the icon only
		std::string pct = std::to_string(std::max(0, std::min(100, var_battery_level_raw))) + "%";
		text_right(pct, x - 3, text_y(0, h, T_KEY), T_KEY, ink);
		x -= text_w(pct, T_KEY) + 3;
	}
	x -= 8;
	wifi(x - 6, h / 2 - 1, var_wifi_state == 2 && var_wifi_signal <= 3 ? var_wifi_signal : -1, ink);
	x -= 16;
	char clock[16];
	snprintf(clock, sizeof clock, "%02d:%02d", var_hours, var_minutes);
	text_right(clock, x, text_y(0, h, T_META), T_META, ink);
	x -= text_w(clock, T_META) + 6;
	if (var_eco_mode) {
		text_right("ECO", x, text_y(0, h, T_KEY), T_KEY, video_mode ? 0xFFA0A0A0u : t.tx3);
		x -= text_w("ECO", T_KEY) + 6;
	}
	if (var_show_fps) { // diagnostic (SELECT+R+A): var_status starts with "NNFPS "
		std::string fps(var_status);
		fps = fps.substr(0, fps.find(' '));
		text_right(fps, x, text_y(0, h, T_KEY), T_KEY, ink);
	}
}
void full_player_pill(float top) {
	std::string label = tr("HINT_FULL_PLAYER", "Full player");
	float w = 6 + 15 + 5 + text_w(label, T_META) + 8, x = 400 - 10 - w;
	pill(x, top, w, 20, 0x9E080808u);
	key_chip("Y", x + 6, top + 2.5f, 0x38FFFFFFu, 0xFFFFFFFFu);
	text(label, x + 26, text_y(top, 20, T_META), T_META, 0xFFFFFFFFu);
}

static void draw_hero() {
	const ThemeTokens &t = theme_cur();
	const Preview &p = cur_preview;
	const float hx = 12, hy = 28, hw = 232, hh = 130, r = 10;
	// hero lift: two soft layers under the card
	round_rect(hx + 1, hy + 5, hw - 2, hh, r + 2, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 3));
	round_rect(hx + 2, hy + 8, hw - 4, hh, r + 4, THEME_ALPHA(t.shadow, (t.shadow >> 24) / 5));
	bool drawn = false;
	if (p.round_thumbnail) { // channel: icon centered on a card
		round_rect(hx, hy, hw, hh, r, t.surf2);
		drawn = thumbnail_draw(hero_handle, hx + (hw - 96) / 2, hy + (hh - 96) / 2, 96, 96);
		if (!drawn) {
			icon(Icon::YOU, hx + hw / 2, hy + hh / 2, 48, t.tx3);
		}
	} else {
		drawn = (hero_large_handle != -1 && thumbnail_draw(hero_large_handle, hx, hy, hw, hh)) ||
		        thumbnail_draw(hero_handle, hx, hy, hw, hh);
		if (!drawn) { // honest neutral tile: no image yet / none for offline items
			rect(hx, hy, hw, hh, t.surf2);
			icon(p.kind == Preview::Kind::DOWNLOAD ? Icon::DOWNLOAD : Icon::PLAY, hx + hw / 2, hy + hh / 2, 34, t.tx3);
		}
		if (p.progress >= 0) {
			rect(hx, hy + hh - 3, hw * std::min(1.0f, p.progress), 3, p.state_color ? p.state_color : t.acc);
		}
		if (!p.duration.empty()) {
			float w = text_w(p.duration, T_KEY) + 8;
			round_rect(hx + hw - 6 - w, hy + hh - 6 - 13, w, 13, 4, 0xC7000000u);
			text_center(p.duration, hx + hw - 6 - w, hx + hw - 6, text_y(hy + hh - 19, 13, T_KEY), T_KEY, 0xFFFFFFFFu);
		}
		// rounded corners painted in the ground each corner actually sits on (glow + fade included)
		u32 c[4] = {top_ground_at(hx, hy), top_ground_at(hx + hw, hy), top_ground_at(hx + hw, hy + hh),
		            top_ground_at(hx, hy + hh)};
		for (int i = 0; i < 4; i++) {
			corner_mask_one(i, i == 1 || i == 2 ? hx + hw : hx, i >= 2 ? hy + hh : hy, r, c[i]);
		}
	}

	// title first, then channel, then meta (no eyebrow)
	float x = 256, y = 28, w = 134;
	for (auto &line : hero_title) {
		text(line, x, y, T_HEADLINE, t.tx);
		y += 20;
	}
	y += 4;
	if (p.kind == Preview::Kind::DOWNLOAD) {
		if (!p.big.empty()) {
			u32 sc = p.state_color ? p.state_color : t.acc_tx;
			text(p.big, x, y - 4, T_DISPLAY, sc);
			float bw = text_w(p.big, T_DISPLAY);
			if (bw + 6 < w) {
				text(ellipsize(p.big_sub, w - bw - 6, T_META), x + bw + 6, y + 9, T_META, sc);
			}
			y += 28;
		}
		for (auto &line : p.meta_lines) {
			if (y > 160) {
				break;
			}
			text(ellipsize(line, w, T_META), x, y, T_META, t.tx2);
			y += 14;
		}
		if (!p.footer_title.empty()) { // storage card: the app's own items only, no SD query
			const float cx = 12, cy = 170, cw = 376, ch = 36;
			round_rect(cx, cy, cw, ch, 10, THEME_ALPHA(t.surf, 0xC7));
			round_rect_outline(cx, cy, cw, ch, 10, 1, t.line);
			text(p.footer_title, cx + 10, cy + 5, T_META, t.tx2);
			text_right(p.footer_right, cx + cw - 10, cy + 5, T_META, t.tx);
			text(ellipsize(p.footer_left, cw - 20, T_KEY), cx + 10, cy + 20, T_KEY, t.tx2);
		}
		return;
	}
	if (!p.channel.empty() && y < 150) {
		text(ellipsize(p.channel, w, T_META), x, y, T_META, t.tx2);
		y += 15;
	}
	for (auto &line : wrap(p.meta, w, T_META, 2)) {
		if (y > 150) {
			break;
		}
		text(line, x, y, T_META, t.tx2);
		y += 14;
	}
}
static void draw_idle() {
	const ThemeTokens &t = theme_cur();
	const float h = 30, w = h * 16 / 11;
	std::string word = "OpenTube";
	float tw = text_w(word, 1.0f), total = w + 10 + tw, x = (400 - total) / 2;
	logo_tile(x, 80, h);
	text(word, x + w + 10, text_y(80, h, 1.0f), 1.0f, t.tx);
	std::string sub = global_current_scene == SceneType::HOME ? tr("IDLE_HINT", "Pick a video below") : screen_title();
	text_center(sub, 0, 400, 124, T_META, t.tx2);
}
void top_draw_browse() {
	const ThemeTokens &t = theme_cur();
	rect(0, 0, 400, 240, t.top);
	glow(GLOW_CX, GLOW_CY, GLOW_RX, GLOW_RY, glow_color());
	vgradient(0, FADE_Y, 400, 240 - FADE_Y, THEME_ALPHA(t.top, 0), THEME_ALPHA(t.top, (u32)(0.85f * 255)));
	status_bar(false, screen_title());
	if (preview_visible()) {
		draw_hero();
	} else {
		draw_idle();
	}
	if (!top_hints.empty()) {
		hints(top_hints, 12, 240 - 9 - 15, t.tx2, 0x38808080u, t.tx, 12);
	}
}

} // namespace shell
