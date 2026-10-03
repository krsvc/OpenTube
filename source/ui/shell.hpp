#pragma once
// OpenTube r13 browse shell (OPENTUBE_R13_PLAN.md): five-item bottom nav, the "You" sheet (history, settings,
// account, about, exit + confirmation), the cue model (select a row without playing it) and the top-screen preview.
// Main thread only. Scenes keep their own content; they reserve the nav (and the mini-player slot while a video is
// loaded) through content_bottom().
#include <string>
#include <vector>
#include "types.hpp"
#include "scene_switcher.hpp"

namespace shell {

constexpr int NAV_H = 40;
constexpr int NAV_Y = 240 - NAV_H;
constexpr int MINI_H = 42;
constexpr int MINI_X = 6;
constexpr int MINI_W = 320 - 12;
constexpr int MINI_Y = NAV_Y - 4 - MINI_H; // 154: the mini-player card sits 4 px above the nav
constexpr int CHIP_ROW_H = 32;
// last y a browse scene may use for content: above the nav, and above the mini-player card while one is shown
inline int content_bottom(bool mini_player) { return mini_player ? MINI_Y - 2 : NAV_Y; }

enum class Dest { HOME, SUBS, SEARCH, DOWNLOADS, YOU, NONE };
constexpr int DEST_COUNT = 5;
Dest active_dest();
std::string dest_label(Dest d);

// Home scene tabs (0 Home, 1 Subscribed channels, 2 New videos): Home publishes its tab, the nav asks for one
extern int home_tab;
extern int home_tab_request; // -1 = none; consumed by Home
extern int last_subs_tab;    // 1 or 2, the Subs destination reopens the last one

// ---- nav + sheets (drawn / updated by ui/overlay.cpp for every browse scene) -----------------------------------------
void nav_draw(bool downloads_busy);
// consumes the touches / keys it uses (and everything while a sheet is open). Returns true when it consumed input.
bool shell_update(Hid_info *key);
// r13b: true when the last shell_update() took the WHOLE frame (a sheet is open, opened or closed): the scene must
// cancel its pending gestures (reset_holding_status) instead of updating, the key it got is empty
bool input_taken();
// r13b: SELECT + X / Y / (R +) A are Menu_main's global chords (log, debug, FPS): the scene never sees those buttons
void reserve_global_chords(Hid_info *key);
void sheets_draw();
bool sheet_open();
void on_scene_resume();
// geometry used by the tests
float nav_col_x(int i);
float nav_col_w();
struct Rect {
	float x, y, w, h;
	bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
// r15: Liked videos after Watch history; code uses these names, never the numeric position
enum class YouItem { HISTORY, LIKED, SETTINGS, ACCOUNT, ABOUT, EXIT };
constexpr int YOU_ITEM_COUNT = (int)YouItem::EXIT + 1;
Rect you_sheet_rect();
Rect you_item_rect(int i);
Rect sheet_close_rect(const Rect &sheet);
Rect confirm_button_rect(int i); // 0 cancel, 1 exit
void open_you_sheet();
bool you_sheet_is_open();
bool exit_confirm_is_open();

// r15: a short message over the bottom edge for ~3 s (drawn by sheets_draw(), takes no input)
void toast(const std::string &text);
bool toast_visible();
bool toast_tick();          // per frame: true while one shows or just ended (the scene must redraw)
extern u64 (*clock_ms)();   // the device sets osGetTime (ui/overlay.cpp); tests set a fake clock
// r15: a small dot on the You nav item and the Settings row (an update was found)
extern bool you_badge;

// ---- generic bottom sheet frame for scenes (player sections, delete confirmation, history remove) -----------------
// draws scrim + surface + grab handle + header (title, optional count, "B Close" pill); returns the content top y
float draw_sheet_frame(float top, const std::string &title, const std::string &count, bool close_pill = true);
// r14: its pieces, for a sheet whose content can scroll above its body and must be painted over again
void draw_sheet_backdrop(float top); // scrim above `top` + the lift shadow
void draw_sheet_header(float top, const std::string &title, const std::string &count, bool close_pill = true);
// what a sheet does with an input frame: scrim tap / B / close pill -> close
bool sheet_should_close(const Hid_info &key, float top, bool close_pill = true);

// ---- cue: the row selected by a first tap or the D-pad; a tap on it (or A) opens it ----------------------------------
extern const void *cue_owner; // identity of the row's View subobject (View *), never dereferenced
bool cue_enabled();           // browse scenes; the player keeps single-tap open
// true = already cued: open it; false = it is cued now. r16: while a playing video owns the top screen (the cued
// preview cannot be seen there) a tap opens the row's page at once; the video keeps playing (nothing auto-replaces it)
bool cue_tap(const void *row);
// r16: a video is loaded while browsing (it owns the top screen, the mini-player card is shown). The device sets it
// (ui/overlay.cpp); tests set a fake
extern bool (*video_on_top)();
void cue_set(const void *row);

// ---- top screen ---------------------------------------------------------------------------------------------------
struct Preview {
	enum class Kind { NONE, VIDEO, CHANNEL, PLAYLIST, DOWNLOAD } kind = Kind::NONE;
	std::string title, channel, meta, duration, thumbnail_url;
	bool round_thumbnail = false; // channel icons
	// downloads: big state line + meta lines + progress on the hero
	std::string big, big_sub;
	u32 state_color = 0;
	float progress = -1;
	std::vector<std::string> meta_lines;
	std::string footer_title, footer_left, footer_right; // storage card
	bool operator==(const Preview &o) const;
};
void preview_set(const Preview &p);
void preview_clear();
const Preview &preview_current();
bool preview_visible(); // set for the current scene
void set_top_hints(const std::vector<std::pair<std::string, std::string>> &hints);
// browse top screen: status bar, glow, hero (or the idle wordmark), hints
void top_draw_browse();
void status_bar(bool video_mode, const std::string &label);
void full_player_pill(float top); // "Y Full player" over a playing video while browsing
std::string screen_title();       // OpenTube / Downloads / Search / ...

// the row / sheet ground a rounded thumbnail is masked against (sheets change it while they draw)
extern u32 cur_ground;
u32 ground();

std::string tr(const char *key, const char *fallback); // localized string with an English fallback

} // namespace shell
