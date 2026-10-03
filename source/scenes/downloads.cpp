#include "headers.hpp"
#include "scenes/downloads.hpp"
#include "scenes/video_player.hpp"
#include "ui/ui.hpp"
#include "ui/overlay.hpp"
#include "downloads/downloads_app.hpp"
#include "network_decoder/thumbnail_loader.hpp"
#include <map>
#include <set>

// Downloads library (DOWNLOADS_OFFLINE_BRIEF.md), OpenTube r13 layout (design/opentube-modern-r13 05 / 06):
// 34 px header, 41 px rows with the progress on the tile, state as dot + word + number, round actions; the selected
// item is shown on the top screen; delete / remove asks in a bottom sheet that names its target.
// Touch: row = select, round buttons = actions. Buttons: D-pad up/down select, A play/retry, X cancel/delete,
// Y stop playback, B back. All actions only change the download manager's in-memory state; the worker thread does
// the SD / network work. No network request is made for offline items: a saved thumbnail (r14, thumb_cache.hpp) is
// read from the SD card through the thumbnail loader, otherwise the tile stays neutral.

#define TOAST_MS 6000

namespace DownloadsScene {
bool already_init = false;
ScrollView *list_view = NULL;
bool shown_valid = false;
unsigned shown_gen = 0;
std::vector<std::string> row_ids; // display order of the rows currently built
int selected = 0;
std::string toast;
u64 toast_time = 0;
std::string delete_candidate; // the item the open delete / remove sheet names (fixed when the sheet opens)
int sheet_holding = -1;
int header_holding = -1;
int sheet_last_x = -1, sheet_last_y = -1;
enum class Action { NONE, PLAY, RETRY, CANCEL, DELETE };
std::string pending_id; // set by a button callback, executed after the views' update
Action pending_action = Action::NONE;
u64 last_progress_refresh = 0;
// library summary from the manager's in-memory snapshot (no SD query)
int library_items = 0;
uint64_t library_ready_bytes = 0;
int content_bottom = 200;
std::map<std::string, int> row_thumbs; // r14: item id -> loader handle of its saved thumbnail (rows near the view)
}; // namespace DownloadsScene
using namespace DownloadsScene;

static void show_toast(const std::string &message) {
	toast = message;
	toast_time = osGetTime();
	var_need_refresh = true;
}
static bool find_item(const std::string &id, downloads::ItemView *item) {
	return downloads_running() && downloads_manager().find(id, item);
}

// ---- r13 row model begin (extracted verbatim by tests/host/run.sh: state words, actions, rows, header, delete sheet;
// it reads items only through find_item() and changes only this scene's selection / pending action / sheet state)
static int row_thumb_handle(const std::string &id) { // r14: the row's saved-thumbnail handle (update_row_thumbnails)
	auto it = row_thumbs.find(id);
	return it == row_thumbs.end() ? -1 : it->second;
}
constexpr int TOP_HEIGHT = 34;
constexpr int ROW_HEIGHT = 41;
static Action primary_action(const downloads::ItemView &item) {
	using S = downloads::ItemState;
	if (item.state == S::READY) {
		return Action::PLAY;
	}
	if (downloads::item_state_active(item.state)) {
		return Action::CANCEL;
	}
	if ((item.state == S::FAILED || item.state == S::CANCELLED || item.state == S::INTERRUPTED ||
	     item.state == S::DAMAGED) &&
	    item.meta_ok && !(item.on_card && !item.owned)) {
		return Action::RETRY;
	}
	return Action::NONE;
}
static Action secondary_action(const downloads::ItemView &item) {
	if (item.state == downloads::ItemState::DELETING || downloads::item_state_active(item.state) ||
	    (item.on_card && !item.owned)) { // not this app's folder: nothing is ever deleted there
		return Action::NONE;
	}
	return Action::DELETE;
}
static const char *action_label(Action action, const downloads::ItemView &item) {
	switch (action) {
	case Action::PLAY:
		return "Play";
	case Action::RETRY:
		return "Retry";
	case Action::CANCEL:
		return "Cancel";
	case Action::DELETE:
		return item.owned ? "Delete" : "Remove"; // a request that never claimed storage leaves only the list
	case Action::NONE:
		break;
	}
	return "";
}
static uint64_t item_bytes(const downloads::ItemView &item) {
	return item.meta.video.size + (item.meta.layout == downloads::Layout::SEPARATE ? item.meta.audio.size : 0);
}
static int item_percent(const downloads::ItemView &item) { // -1 = unknown
	return item.total ? (int)(item.done * 100 / item.total) : -1;
}
static bool item_foreign(const downloads::ItemView &item) { return item.on_card && !item.owned; }
// state as dot + word + number (+ detail) in the state color; never color alone
struct RowStatus {
	std::string word, detail;
	u32 color;
	float progress; // on the tile, -1 = none
	bool dim;       // failed / damaged: dimmed tile with an alert mark
};
static RowStatus row_status(const downloads::ItemView &item) {
	using S = downloads::ItemState;
	const ThemeTokens &t = theme_cur();
	RowStatus r{"", "", t.tx2, -1, false};
	int pct = item_percent(item);
	std::string q = std::to_string(item.meta.quality) + "p";
	switch (item.state) {
	case S::READY:
		r.word = "Ready", r.color = t.ok;
		r.detail = downloads::format_bytes(item_bytes(item)) + " \xC2\xB7 " + q + (item.in_use ? " \xC2\xB7 playing" : "");
		break;
	case S::DOWNLOADING:
		r.word = pct >= 0 ? std::to_string(pct) + "%" : "Downloading", r.color = t.acc_tx;
		r.detail = pct >= 0 ? downloads::format_bytes(item.done) + " of " + downloads::format_bytes(item.total)
		                    : downloads::format_bytes(item.done) + " (size unknown)";
		r.progress = pct >= 0 ? pct / 100.0f : 0;
		break;
	case S::PAUSED_FOR_PLAYBACK: // automatic: resumes by itself when playback stops
		r.word = pct >= 0 ? "Paused " + std::to_string(pct) + "%" : "Paused", r.color = t.warn;
		r.detail = "while a video plays";
		r.progress = pct >= 0 ? pct / 100.0f : 0;
		break;
	case S::QUEUED:
	case S::RESOLVING:
		r.word = downloads::item_state_text(item.state), r.color = t.acc_tx;
		r.detail = q;
		r.progress = 0;
		break;
	case S::FAILED:
		r.word = pct >= 0 ? "Failed " + std::to_string(pct) + "%" : "Failed", r.color = t.err;
		r.detail = item.message, r.dim = true;
		r.progress = pct >= 0 ? pct / 100.0f : -1;
		break;
	case S::CANCELLED:
		r.word = "Cancelled", r.color = t.tx2, r.detail = "not playable, Retry restarts";
		break;
	case S::INTERRUPTED:
		r.word = "Interrupted", r.color = t.warn, r.detail = "not playable, Retry restarts";
		break;
	case S::DAMAGED:
		r.word = "Damaged", r.color = t.err, r.detail = item.message, r.dim = true;
		break;
	case S::DELETING:
		r.word = "Deleting", r.color = t.tx2, r.detail = q;
		break;
	}
	if (item_foreign(item)) {
		r.detail = "folder not managed by OpenTube";
	}
	return r;
}
// two round buttons at the right edge; a single action sits in the right slot
struct RowButtons {
	Action left, right;
};
static RowButtons row_buttons(const downloads::ItemView &item) {
	Action p = primary_action(item), s = secondary_action(item);
	RowButtons b{Action::NONE, Action::NONE};
	if (p == Action::CANCEL) {
		b.right = Action::CANCEL;
	} else {
		b.left = p, b.right = s;
	}
	if (b.left != Action::NONE && b.right == Action::NONE) {
		b.right = b.left, b.left = Action::NONE;
	}
	return b;
}
static constexpr float ROW_BUTTON = 28;
static float row_button_cx(int slot) { return 316 - 5 - ROW_BUTTON / 2 - (slot == 0 ? ROW_BUTTON + 4 : 0); }
static shell::Preview item_preview(const downloads::ItemView &item) {
	using S = downloads::ItemState;
	RowStatus st = row_status(item);
	shell::Preview p;
	p.kind = shell::Preview::Kind::DOWNLOAD;
	p.title = item.meta.title.size() ? item.meta.title : item.id;
	p.state_color = st.color;
	p.progress = st.progress;
	int pct = item_percent(item);
	std::string q = std::to_string(item.meta.quality) + "p";
	switch (item.state) {
	case S::READY:
		p.big = "Ready", p.big_sub = item.in_use ? "playing" : "offline";
		p.meta_lines = {downloads::format_bytes(item_bytes(item)) + " \xC2\xB7 " + q + " \xC2\xB7 " +
		                    Util_convert_seconds_to_time((double)item.meta.duration_ms / 1000),
		                "saved on the SD card"};
		break;
	case S::DOWNLOADING:
		p.big = pct >= 0 ? std::to_string(pct) + "%" : downloads::format_bytes(item.done), p.big_sub = "downloading";
		p.meta_lines = {st.detail + " \xC2\xB7 " + q, "one download at a time"};
		break;
	case S::PAUSED_FOR_PLAYBACK:
		p.big = pct >= 0 ? std::to_string(pct) + "%" : "Paused", p.big_sub = "paused";
		p.meta_lines = {q + " \xC2\xB7 waits while you watch", "continues when playback stops"};
		break;
	case S::FAILED:
		p.big = "Failed", p.big_sub = pct >= 0 ? "at " + std::to_string(pct) + "%" : "";
		p.meta_lines = {item.message, primary_action(item) == Action::RETRY ? "Retry starts it again" : ""};
		break;
	default:
		p.big = st.word, p.big_sub = "";
		p.meta_lines = {st.detail, q};
		break;
	}
	if (item_foreign(item)) {
		p.meta_lines = {"This folder is not managed by OpenTube:", "it is never written or deleted."};
	}
	if (item.thumb) { // r14: the saved image, read from the SD card (never the network)
		p.thumbnail_url = Downloads_thumbnail_key(item.id, true);
	}
	return p;
}
static void request_action(const std::string &id, Action action) {
	pending_id = id;
	pending_action = action;
}

// one library row: tile with the progress on it, title, state line, round actions
struct DownloadRowView : public FixedSizeView {
	std::string id;
	int index;
	int holding = -1; // -1 none, 0 / 1 button slot, 2 row body
	DownloadRowView(const std::string &id, int index) : View(0, 0), FixedSizeView(0, 0, 320, ROW_HEIGHT), id(id), index(index) {}
	void draw_() const override {
		const ThemeTokens &t = theme_cur();
		downloads::ItemView item;
		if (!find_item(id, &item)) {
			return;
		}
		bool cued = index == selected;
		if (cued) {
			modern::round_rect(x0 + 4, y0, 312, ROW_HEIGHT, 10, t.acc);
			modern::round_rect(x0 + 5.5f, y0 + 1.5f, 309, ROW_HEIGHT - 3, 8.5f, t.surf2);
		} else if (holding == 2) {
			modern::round_rect(x0 + 4, y0, 312, ROW_HEIGHT, 10, t.pressed);
		}
		u32 ground = cued ? t.surf2 : holding == 2 ? t.pressed : t.bg;
		RowStatus st = row_status(item);
		// tile: the saved thumbnail (r14, read from the SD card) with a quality badge, else a neutral tile
		const float tx = x0 + 9, ty = y0 + (ROW_HEIGHT - 36) / 2, tw = 64, th = 36;
		std::string q = std::to_string(item.meta.quality) + "p";
		int handle = item.thumb ? row_thumb_handle(id) : -1;
		if (handle != -1 && thumbnail_draw(handle, tx, ty, tw, th)) {
			float qw = modern::text_w(q, modern::T_KEY) + 6;
			modern::round_rect(tx + tw - 3 - qw, ty + th - 3 - 12, qw, 12, 3, 0xC7000000u);
			modern::text_center(q, tx + tw - 3 - qw, tx + tw - 3, ty + th - 3 - 12, modern::T_KEY, 0xFFFFFFFFu);
		} else {
			modern::rect(tx, ty, tw, th, cued ? t.bg : t.surf2);
			modern::icon(modern::Icon::PLAY, tx + tw / 2, ty + th / 2 - 3, 12, t.tx3);
			modern::text_center(q, tx, tx + tw, ty + th - 13, modern::T_KEY, t.tx3);
		}
		if (st.dim) {
			modern::rect(tx, ty, tw, th, 0x73000000);
			modern::icon(modern::Icon::ALERT, tx + tw / 2, ty + th / 2, 16, 0xFFFFFFFF);
		}
		if (st.progress >= 0) {
			modern::rect(tx, ty + th - 3, tw * std::min(1.0f, st.progress), 3, st.color);
		}
		modern::corner_mask(tx, ty, tw, th, 6, ground);
		// text
		RowButtons b = row_buttons(item);
		float text_x = tx + tw + 8;
		float right = (b.left != Action::NONE ? row_button_cx(0) : b.right != Action::NONE ? row_button_cx(1) : 316 + 9) -
		              ROW_BUTTON / 2 - 6;
		std::string title = item.meta.title.size() ? item.meta.title : item.id;
		modern::text(modern::ellipsize(title, right - text_x, modern::T_ROW), text_x, y0 + 5, modern::T_ROW,
		             st.dim ? t.tx2 : t.tx);
		float sy = y0 + 22;
		modern::circle(text_x + 3, sy + 6, 3, st.color);
		float wx = text_x + 10;
		modern::text(st.word, wx, sy, modern::T_META, st.color);
		wx += modern::text_w(st.word, modern::T_META);
		if (!st.detail.empty() && wx + 12 < right) {
			modern::text(modern::ellipsize("\xC2\xB7 " + st.detail, right - wx - 4, modern::T_META), wx + 4, sy,
			             modern::T_META, t.tx2);
		}
		// buttons
		Action slots[2] = {b.left, b.right};
		for (int s = 0; s < 2; s++) {
			if (slots[s] == Action::NONE) {
				continue;
			}
			float cx = row_button_cx(s), cy = y0 + ROW_HEIGHT / 2;
			bool primary = slots[s] == Action::PLAY && cued;
			u32 fill = primary ? t.acc : cued ? t.surf : t.surf2;
			if (holding == s) {
				fill = theme_blend(fill, t.tx, 0.15f);
			}
			modern::circle(cx, cy, ROW_BUTTON / 2, fill);
			modern::Icon ic = slots[s] == Action::PLAY    ? modern::Icon::PLAY
			                  : slots[s] == Action::RETRY  ? modern::Icon::RETRY
			                  : slots[s] == Action::CANCEL ? modern::Icon::CLOSE
			                                               : modern::Icon::TRASH;
			modern::icon(ic, cx, cy, 15, primary ? t.acc_ink : t.tx);
		}
	}
	int target(int x, int y) const {
		if (y < y0 || y >= y1 || x < x0 || x >= x1) {
			return -1;
		}
		downloads::ItemView item;
		if (find_item(id, &item)) {
			RowButtons b = row_buttons(item);
			Action slots[2] = {b.left, b.right};
			for (int s = 0; s < 2; s++) {
				float dx = x - row_button_cx(s), dy = y - (y0 + ROW_HEIGHT / 2);
				if (slots[s] != Action::NONE && dx * dx + dy * dy <= (ROW_BUTTON / 2 + 2) * (ROW_BUTTON / 2 + 2)) {
					return s;
				}
			}
		}
		return 2;
	}
	void on_scroll() override { holding = -1; }
	void reset_holding_status_() override { holding = -1; }
	void update_(Hid_info key) override {
		if (key.p_touch) {
			holding = target(key.touch_x, key.touch_y);
		} else if (key.touch_x != -1 && holding != -1 && target(key.touch_x, key.touch_y) != holding) {
			holding = -1;
		}
		if (key.touch_x == -1 && holding != -1) {
			downloads::ItemView item;
			if (holding == 2) {
				selected = index;
			} else if (find_item(id, &item)) {
				RowButtons b = row_buttons(item);
				request_action(id, holding == 0 ? b.left : b.right);
				selected = index;
			}
			holding = -1;
			var_need_refresh = true;
		}
	}
};

// header: title, and a Stop playback pill while a video holds the downloads
static shell::Rect header_pill_rect() {
	float w = modern::text_w("Stop playback", modern::T_META) + 30;
	return {320 - 10 - w, 6, w, 22};
}
static void draw_header() {
	const ThemeTokens &t = theme_cur();
	modern::rect(0, 0, 320, TOP_HEIGHT, t.bg);
	modern::text(shell::tr("NAV_DOWNLOADS", "Downloads"), 10, modern::text_y(0, TOP_HEIGHT, MIDDLE_FONT_SIZE),
	             MIDDLE_FONT_SIZE, t.tx);
	shell::Rect r = header_pill_rect();
	if (video_playback_active()) {
		modern::pill(r.x, r.y, r.w, r.h, header_holding == 0 ? t.pressed : t.surf2);
		modern::icon(modern::Icon::STOP, r.x + 12, r.y + r.h / 2, 12, t.tx2);
		modern::text("Stop playback", r.x + 22, modern::text_y(r.y, r.h, modern::T_META), modern::T_META, t.tx2);
	} else if (library_items > 0) {
		std::string label = std::to_string(library_items) + (library_items == 1 ? " item" : " items");
		float w = modern::text_w(label, modern::T_META) + 30;
		modern::pill(320 - 10 - w, r.y, w, r.h, t.surf2);
		modern::icon(modern::Icon::SD, 320 - 10 - w + 12, r.y + r.h / 2, 13, t.tx2);
		modern::text(label, 320 - 10 - w + 22, modern::text_y(r.y, r.h, modern::T_META), modern::T_META, t.tx2);
	}
}
// delete / remove confirmation sheet: names the item it was opened for
static constexpr float SHEET_H = 166; // header, target (tile + 2 title lines), 2 detail lines, buttons
static shell::Rect sheet_button_rect(int i) { return {12.0f + i * 152, 240 - 12 - 34, 144, 34}; }
static void draw_delete_sheet() {
	const ThemeTokens &t = theme_cur();
	downloads::ItemView item;
	if (!find_item(delete_candidate, &item)) {
		return;
	}
	float top = 240 - SHEET_H;
	shell::draw_sheet_frame(top, item.owned ? "Delete this download?" : "Remove from the list?", "", false);
	const float tx = 12, ty = top + 38, tw = 72, th = 40;
	modern::rect(tx, ty, tw, th, t.surf2);
	modern::text_center(std::to_string(item.meta.quality) + "p", tx, tx + tw, modern::text_y(ty, th, modern::T_META),
	                    modern::T_META, t.tx3);
	modern::corner_mask(tx, ty, tw, th, 6, t.surf);
	std::vector<std::string> title =
	    modern::wrap(item.meta.title.size() ? item.meta.title : item.id, 320 - 12 - (tx + tw + 10), modern::T_ROW, 2);
	float y = ty - 1;
	for (auto &line : title) {
		modern::text(line, tx + tw + 10, y, modern::T_ROW, t.tx);
		y += 15;
	}
	std::string detail = item.owned ? std::to_string(item.meta.quality) + "p \xC2\xB7 " +
	                                      downloads::format_bytes(item.done ? item.done : item_bytes(item)) +
	                                      " leaves the SD card. You can download it again later."
	                                : "It never claimed storage: the SD card stays unchanged.";
	std::vector<std::string> detail_lines = modern::wrap(detail, 320 - 24, modern::T_META, 2);
	for (size_t i = 0; i < detail_lines.size(); i++) {
		modern::text(detail_lines[i], 12, ty + th + 6 + i * 13, modern::T_META, t.tx2);
	}
	for (int i = 0; i < 2; i++) {
		shell::Rect b = sheet_button_rect(i);
		u32 fill = i == 1 ? t.err : t.surf2, ink = i == 1 ? t.err_ink : t.tx;
		if (sheet_holding == i) {
			fill = theme_blend(fill, ink, 0.15f);
		}
		modern::pill(b.x, b.y, b.w, b.h, fill);
		std::string label = i == 0 ? "Cancel" : item.owned ? "Delete" : "Remove";
		float w = 15 + 5 + modern::text_w(label, modern::T_ROW), x = b.x + (b.w - w) / 2;
		modern::key_chip(i == 0 ? "B" : "A", x, b.y + (b.h - 15) / 2, i == 1 ? THEME_ALPHA(t.err_ink, 0x33) : t.kc, ink);
		modern::text(label, x + 20, modern::text_y(b.y, b.h, modern::T_ROW), modern::T_ROW, ink);
	}
	shell::cur_ground = 0;
}

// delete / remove sheet: opening fixes its target; a playing item or a folder that is not the app's is refused
static bool delete_sheet_open(const downloads::ItemView &item, std::string *toast) {
	if (item.in_use) {
		*toast = "This item is playing. Stop playback first.";
		return false;
	}
	if (secondary_action(item) != Action::DELETE) {
		return false;
	}
	delete_candidate = item.id;
	sheet_holding = -1;
	sheet_last_x = sheet_last_y = -1;
	return true;
}
// one input frame of the open sheet: A / Delete button = confirm, B / Cancel / backdrop = cancel, nothing else reacts
enum class SheetInput { NONE, CONFIRM, CANCEL };
static SheetInput delete_sheet_input(const Hid_info &key) {
	SheetInput res = SheetInput::NONE;
	bool released = key.touch_x == -1 && sheet_last_x != -1;
	if (key.p_a) {
		res = SheetInput::CONFIRM;
	} else if (key.p_b) {
		res = SheetInput::CANCEL;
	} else {
		if (key.p_touch) {
			sheet_holding = sheet_button_rect(0).contains(key.touch_x, key.touch_y)   ? 0
			                : sheet_button_rect(1).contains(key.touch_x, key.touch_y) ? 1
			                : key.touch_y < 240 - SHEET_H                               ? 2
			                                                                            : -1;
		}
		if (released && sheet_holding != -1) {
			if (sheet_holding == 1 && sheet_button_rect(1).contains(sheet_last_x, sheet_last_y)) {
				res = SheetInput::CONFIRM;
			} else if ((sheet_holding == 0 && sheet_button_rect(0).contains(sheet_last_x, sheet_last_y)) ||
			           (sheet_holding == 2 && sheet_last_y < 240 - SHEET_H)) {
				res = SheetInput::CANCEL;
			}
			sheet_holding = -1;
		}
	}
	sheet_last_x = key.touch_x, sheet_last_y = key.touch_y;
	return res;
}
// ---- r13 row model end ---------------------------------------------------------------------------------------------

static void sync_with_manager() {
	if (!downloads_running()) {
		return;
	}
	unsigned gen = 0;
	std::vector<downloads::ItemView> items = downloads_manager().snapshot(&gen);
	if (shown_valid && gen == shown_gen) {
		return;
	}
	shown_valid = true;
	shown_gen = gen;
	list_view->recursive_delete_subviews();
	row_ids.clear();
	library_items = items.size();
	library_ready_bytes = 0;
	std::vector<View *> rows;
	for (size_t i = 0; i < items.size(); i++) {
		rows.push_back(new DownloadRowView(items[i].id, i));
		row_ids.push_back(items[i].id);
		if (items[i].state == downloads::ItemState::READY) {
			library_ready_bytes += item_bytes(items[i]);
		}
	}
	if (rows.empty()) {
		rows.push_back((new TextView(0, 0, 320, DEFAULT_FONT_INTERVAL * 3))
		                   ->set_text_lines<std::function<std::string()>>(
		                       {[]() {
			                        return downloads_manager().library_loaded() ? std::string("No downloads yet.")
			                                                                    : std::string("Loading...");
		                        },
		                        []() { return std::string("Open a video, then Download in its Info page."); }})
		                   ->set_x_alignment(TextView::XAlign::CENTER)
		                   ->set_get_text_color([]() { return LIGHT0_TEXT_COLOR; }));
	}
	list_view->set_views(rows);
	selected = std::max(0, std::min<int>(selected, (int)row_ids.size() - 1));
	var_need_refresh = true;
}

// r14: saved thumbnails of the rows within 3 of the visible ones; every other handle is cancelled
static void update_row_thumbnails() {
	int first = list_view->get_offset() / ROW_HEIGHT - 3;
	int last = (list_view->get_offset() + list_view->get_height()) / ROW_HEIGHT + 3;
	std::set<std::string> want;
	for (int i = std::max(0, first); i <= last && i < (int)row_ids.size(); i++) {
		downloads::ItemView item;
		if (find_item(row_ids[i], &item) && item.thumb) {
			want.insert(item.id);
		}
	}
	for (auto it = row_thumbs.begin(); it != row_thumbs.end();) {
		if (want.count(it->first)) {
			++it;
		} else {
			thumbnail_cancel_request(it->second);
			it = row_thumbs.erase(it);
		}
	}
	for (auto &id : want) {
		if (!row_thumbs.count(id)) {
			row_thumbs[id] = thumbnail_request(Downloads_thumbnail_key(id, false), SceneType::DOWNLOADS, 0,
			                                   ThumbnailType::VIDEO_THUMBNAIL);
		}
	}
}
static void clear_row_thumbnails() {
	for (auto &entry : row_thumbs) {
		thumbnail_cancel_request(entry.second);
	}
	row_thumbs.clear();
}

static void open_delete_confirmation(const std::string &id) {
	downloads::ItemView item;
	std::string message;
	if (!find_item(id, &item)) {
		return;
	}
	if (!delete_sheet_open(item, &message) && message.size()) {
		show_toast(message);
	}
	var_need_refresh = true;
}
static void confirm_delete() {
	std::string message;
	downloads_manager().request_delete(delete_candidate, &message);
	show_toast(message);
	delete_candidate = "";
	var_need_refresh = true;
}
static void run_pending_action() {
	if (pending_action == Action::NONE) {
		return;
	}
	std::string id = pending_id;
	Action action = pending_action;
	pending_action = Action::NONE;
	pending_id = "";
	std::string message;
	switch (action) {
	case Action::PLAY:
		global_intent.next_scene = SceneType::VIDEO_PLAYER;
		global_intent.arg = downloads::local_url_for(id);
		break;
	case Action::RETRY:
		downloads_manager().request_retry(id, &message);
		if (video_playback_active()) {
			message += " Paused while a video plays (Y: stop playback).";
		}
		show_toast(message);
		break;
	case Action::CANCEL:
		downloads_manager().request_cancel(id, &message);
		show_toast(message);
		break;
	case Action::DELETE:
		open_delete_confirmation(id);
		break;
	case Action::NONE:
		break;
	}
}
static void select_row(int index) {
	if (row_ids.empty()) {
		return;
	}
	selected = std::max(0, std::min<int>(index, (int)row_ids.size() - 1));
	double top = selected * ROW_HEIGHT;
	double visible = list_view->get_height();
	if (top < list_view->get_offset()) {
		list_view->set_offset(top);
	} else if (top + ROW_HEIGHT > list_view->get_offset() + visible) {
		list_view->set_offset(top + ROW_HEIGHT - visible);
	}
	var_need_refresh = true;
}

void Downloads_init(void) {
	logger.info("downloads/init", "Initializing...");
	list_view = new ScrollView(0, TOP_HEIGHT, 320, 200 - TOP_HEIGHT);
	Downloads_resume("");
	already_init = true;
	logger.info("downloads/init", "Initialized.");
}
void Downloads_exit(void) {
	already_init = false;
	clear_row_thumbnails();
	if (list_view) {
		list_view->recursive_delete_subviews();
		delete list_view;
		list_view = NULL;
	}
	logger.info("downloads/exit", "Exited.");
}
void Downloads_suspend(void) { clear_row_thumbnails(); }
void Downloads_resume(std::string arg) {
	(void)arg;
	if (list_view) {
		list_view->on_resume();
	}
	overlay_menu_on_resume();
	shown_valid = false; // rebuild from the current library
	delete_candidate = "";
	var_need_refresh = true;
}

void Downloads_draw(void) {
	Hid_info key;
	Util_hid_query_key_state(&key);

	bool video_playing_bar_show = video_is_playing();
	content_bottom = shell::content_bottom(video_playing_bar_show);
	list_view->update_y_range(TOP_HEIGHT, content_bottom);
	sync_with_manager();
	update_row_thumbnails();
	if (downloads_running() && downloads_manager().busy() && osGetTime() - last_progress_refresh > 500) {
		last_progress_refresh = osGetTime(); // progress text changes without a library change (eco mode)
		var_need_refresh = true;
	}
	if (toast.size() && osGetTime() - toast_time < TOAST_MS + 100) {
		var_need_refresh = true;
	}
	// top screen: the selected item (state line, progress on the hero, library summary card)
	downloads::ItemView sel_item;
	if (selected < (int)row_ids.size() && find_item(row_ids[selected], &sel_item)) {
		shell::Preview p = item_preview(sel_item);
		p.footer_title = "SD card";
		p.footer_right = downloads::format_bytes(library_ready_bytes) + " saved offline";
		p.footer_left = !downloads_manager().library_loaded() ? std::string("Loading the download library...")
		                : downloads_manager().busy() && video_playback_active()
		                    ? std::string("A video is playing: downloading is paused.")
		                    : std::to_string(library_items) + (library_items == 1 ? " item" : " items") +
		                          " \xC2\xB7 one download at a time \xC2\xB7 free space is not measured here";
		shell::preview_set(p);
	} else {
		shell::preview_clear();
	}
	shell::set_top_hints({{"A", "Play/Retry"}, {"X", "Cancel/Delete"}, {"Y", "Stop playback"}, {"dud", "Select"}});

	if (var_need_refresh || !var_eco_mode) {
		var_need_refresh = false;
		Draw_frame_ready();
		video_draw_top_screen();

		Draw_screen_ready(1, DEFAULT_BACK_COLOR);
		list_view->draw();
		draw_header();
		if (!downloads_running()) {
			modern::text_center("Downloads are not available.", 0, 320, 60, modern::T_META, LIGHT0_TEXT_COLOR);
		}
		if (toast.size() && osGetTime() - toast_time < TOAST_MS) { // floating status pill above the nav
			std::string line = modern::ellipsize(toast, 290, modern::T_META);
			float w = modern::text_w(line, modern::T_META) + 20;
			modern::pill((320 - w) / 2, content_bottom - 28, w, 22, POCKET_PRESSED);
			modern::text_center(line, 0, 320, modern::text_y(content_bottom - 28, 22, modern::T_META), modern::T_META,
			                    DEFAULT_TEXT_COLOR);
		}
		if (video_playing_bar_show) {
			video_draw_playing_bar();
		}
		draw_overlay_menu(content_bottom);
		if (delete_candidate.size()) {
			draw_delete_sheet();
		}

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
	} else if (delete_candidate.size()) { // the sheet traps input: nothing under it reacts
		shell::reserve_global_chords(&key);
		SheetInput in = delete_sheet_input(key);
		if (in == SheetInput::CONFIRM) {
			confirm_delete();
		} else if (in == SheetInput::CANCEL) {
			delete_candidate = "";
		}
		var_need_refresh = true;
	} else {
		bool taken = update_overlay_menu(&key);
		if (taken) { // the shell owns this frame: a held pill / row button / mini-player touch ends without its action
			header_holding = -1;
			list_view->reset_holding_status();
			video_cancel_playing_bar_gesture();
		}
		// header pill: stop playback (same as Y)
		shell::Rect hp = header_pill_rect();
		if (key.p_touch) {
			header_holding = video_playback_active() && hp.contains(key.touch_x, key.touch_y) ? 0 : -1;
		}
		if (key.touch_x == -1 && header_holding == 0) {
			header_holding = -1;
			if (video_playback_active()) {
				video_stop_playback();
				show_toast("Playback stopped. The download continues.");
			}
		}
		if (!taken) {
			list_view->update(key);
		}
		if (key.p_d_down || key.p_d_up) {
			select_row(selected + (key.p_d_down ? 1 : -1));
		}
		downloads::ItemView item;
		bool has_item = selected < (int)row_ids.size() && find_item(row_ids[selected], &item);
		if (key.p_a && has_item && primary_action(item) != Action::CANCEL) {
			request_action(item.id, primary_action(item));
		} else if (key.p_x && has_item) {
			request_action(item.id, downloads::item_state_active(item.state) ? Action::CANCEL : secondary_action(item));
		} else if (key.p_y && video_playback_active()) {
			video_stop_playback();
			show_toast("Playback stopped. The download continues.");
		}
		run_pending_action();
		if (video_playing_bar_show) {
			video_update_playing_bar(key);
		}
		if (key.p_b) {
			global_intent.next_scene = SceneType::BACK;
		}
	}
}
