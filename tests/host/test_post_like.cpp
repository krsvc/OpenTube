// Host regression for the comment like icon (COMMENT_LIKE_FIX_BRIEF.md). REAL ui/views/specialized/post.cpp (the whole
// file: PostView::draw_ / update_ of comments, replies and community posts), REAL ui/modern.cpp, ui/theme.cpp,
// ui/shell.cpp and ui/views/view.cpp, against a draw recorder that keeps texture identity (the legacy
// var_texture_thumb_up images are told apart from every other texture). Stand-ins: drawing / thumbnails (hardware),
// localized strings, and Util_find_timestamp_in_text (a text parser behind the app umbrella header; the fixture
// comments hold no timestamp). Nothing touches the network, media or an account.
// Asserted in all four themes: no legacy thumb texture and no opaque backing in the 16x16 like slot, the vector like
// icon centered in that slot in the count's color, the count text and every layout coordinate unchanged, nested replies
// and community posts covered, no like row in description mode, the slot stays display-only. A LAYOUT line per case is
// printed so the runner can compare the layout of the old and the corrected code.
// argv[1] (optional): folder for the [fixture] JSON captures rendered by render_draw_capture.py.
#include "ui/views/specialized/post.hpp"
#include "ui/modern.hpp"
#include "ui/shell.hpp"
#include "ui/theme.hpp"
#include "scene_switcher.hpp"
#include "network_decoder/thumbnail_loader.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

// ---- globals the real code reads ----------------------------------------------------------------------------------
bool var_night_mode = true;
bool var_need_refresh = false;
bool var_eco_mode = true;
bool var_show_fps = false;
u8 var_wifi_state = 2, var_wifi_signal = 3, var_battery_charge = 0;
int var_battery_level_raw = 80;
bool var_battery_level_exact = true;
int var_hours = 21, var_minutes = 47;
char var_status[128] = "";
int var_community_image_size = 200;
SceneType global_current_scene = SceneType::HOME;
Intent global_intent;
// distinct fake textures: the like slot must never use the legacy (opaque) thumb up images
static C3D_Tex *const TEX_SQUARE = (C3D_Tex *)0x10;
static C3D_Tex *const TEX_THUMB_UP_DAY = (C3D_Tex *)0x51;
static C3D_Tex *const TEX_THUMB_UP_NIGHT = (C3D_Tex *)0x52;
C2D_Image var_square_image[1] = {{TEX_SQUARE, nullptr}};
C2D_Image var_texture_thumb_up[2] = {{TEX_THUMB_UP_DAY, nullptr}, {TEX_THUMB_UP_NIGHT, nullptr}};

std::string get_string_resource(std::string id) {
	static const std::map<std::string, std::string> en = {{"SHOW_MORE", "Show more"},
	                                                      {"SHOW_REPLIES", "Show replies"},
	                                                      {"HIDE_REPLIES", "Hide replies"},
	                                                      {"SHOW_MORE_REPLIES", "Show more replies"},
	                                                      {"LOADING", "Loading..."}};
	auto it = en.find(id);
	return it == en.end() ? id : it->second;
}
int Util_find_timestamp_in_text(const std::string &, int, int *, int *, double *) { return -1; } // none found

// ---- thumbnails (no loader): avatars are labelled placeholders ------------------------------------------------------
int thumbnail_request(const std::string &, SceneType, int, ThumbnailType) { return -1; }
void thumbnail_cancel_request(int) {}
bool thumbnail_is_available(int) { return true; }
int thumbnail_get_status_code(int) { return 200; }
void thumbnail_get_dimensions(int, int *w, int *h) { *w = 320, *h = 180; }

// ---- draw recorder
// ---------------------------------------------------------------------------------------------------
struct Op {
	enum Kind { RECT, TEXT, TRI, LINE, THUMB, GRAD, TEX } kind;
	float x, y, w, h; // RECT/TEXT/THUMB/GRAD/TEX box; LINE/TRI: pts
	u32 color;
	std::string text;
	float size;
	float pts[6];
	const C3D_Tex *tex;
};
static std::vector<Op> ops;
static float text_width(const std::string &t, float s) {
	int cps = 0;
	for (unsigned char c : t) {
		cps += (c & 0xC0) != 0x80;
	}
	return cps * 13.0f * s;
}
float Draw_get_width(const std::string &t, float s) { return text_width(t, s); }
float Draw_get_height(const std::string &t, float s) { return (1 + std::count(t.begin(), t.end(), '\n')) * 20.0 * s; }
void Draw(std::string t, float x, float y, float sx, float sy, int c) {
	ops.push_back({Op::TEXT, x, y, text_width(t, sx), 20 * sy, (u32)c, t, sx, {}, nullptr});
}
void Draw_x_centered(std::string t, float x0, float x1, float y, float sx, float sy, int c) {
	Draw(t, (x0 + x1 - text_width(t, sx)) / 2, y, sx, sy, c);
}
void Draw_right(std::string t, float x1, float y, float sx, float sy, int c) {
	Draw(t, x1 - text_width(t, sx), y, sx, sy, c);
}
void Draw_texture(C2D_Image image, int c, float x, float y, float w, float h) {
	ops.push_back({Op::TEX, x, y, w, h, (u32)c, "", 0, {}, image.tex});
}
void Draw_texture(C2D_Image image, float x, float y, float w, float h) { Draw_texture(image, 0, x, y, w, h); }
void Draw_line(float x0, float y0, int c0, float x1, float y1, int, float width) {
	ops.push_back({Op::LINE, 0, 0, width, 0, (u32)c0, "", 0, {x0, y0, x1, y1, 0, 0}, nullptr});
}
bool C2D_DrawCircleSolid(float x, float y, float, float r, u32 c) {
	ops.push_back({Op::RECT, x - r, y - r, 2 * r, 2 * r, c, "", 0, {}, nullptr});
	return true;
}
bool C2D_DrawTriangle(float x0, float y0, u32 c0, float x1, float y1, u32, float x2, float y2, u32, float) {
	ops.push_back({Op::TRI, 0, 0, 0, 0, c0, "", 0, {x0, y0, x1, y1, x2, y2}, nullptr});
	return true;
}
bool C2D_DrawRectSolid(float x, float y, float, float w, float h, u32 c) {
	ops.push_back({Op::RECT, x, y, w, h, c, "", 0, {}, nullptr});
	return true;
}
bool C2D_DrawRectangle(float x, float y, float, float w, float h, u32 c0, u32, u32, u32) {
	ops.push_back({Op::GRAD, x, y, w, h, c0, "", 0, {}, nullptr});
	return true;
}
bool thumbnail_draw(int, int x, int y, int w, int h) {
	ops.push_back({Op::THUMB, (float)x, (float)y, (float)w, (float)h, 0xFF808080, "", 0, {}, nullptr});
	return true;
}

// ---- checks
// ----------------------------------------------------------------------------------------------------------
static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		checks++;                                                                                                      \
		if (!(cond)) {                                                                                                 \
			failures++;                                                                                                \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

struct Box {
	float x0, y0, x1, y1;
};
static Box bounds(const Op &o) {
	if (o.kind == Op::TRI || o.kind == Op::LINE) {
		int n = o.kind == Op::TRI ? 6 : 4;
		Box b{o.pts[0], o.pts[1], o.pts[0], o.pts[1]};
		for (int i = 0; i < n; i += 2) {
			b.x0 = std::min(b.x0, o.pts[i]), b.x1 = std::max(b.x1, o.pts[i]);
			b.y0 = std::min(b.y0, o.pts[i + 1]), b.y1 = std::max(b.y1, o.pts[i + 1]);
		}
		return b;
	}
	return {o.x, o.y, o.x + o.w, o.y + o.h};
}
static bool overlaps(const Box &a, const Box &b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
static const Op *find_text(const std::string &t) {
	for (auto &o : ops) {
		if (o.kind == Op::TEXT && o.text == t) {
			return &o;
		}
	}
	return nullptr;
}
static int legacy_thumb_draws() {
	int n = 0;
	for (auto &o : ops) {
		n += o.kind == Op::TEX && (o.tex == TEX_THUMB_UP_DAY || o.tex == TEX_THUMB_UP_NIGHT);
	}
	return n;
}

// the like row of one PostView whose 16x16 slot starts at (slot_x, slot_y); `count` is its upvote text
static void check_like_row(const char *label, float slot_x, float slot_y, const std::string &count) {
	const ThemeTokens &t = theme_cur();
	const Op *c = find_text(count);
	CHECK(c != nullptr, "%s: count text '%s' drawn", label, count.c_str());
	if (c) { // count text, its font and color as before
		CHECK(c->x == slot_x + 16 + SMALL_MARGIN && c->y == slot_y + 1 && c->size == 0.44f && c->color == t.tx3,
		      "%s: count at (%g,%g) size %g color %08x (want (%g,%g) 0.44 %08x)", label, c->x, c->y, c->size, c->color,
		      slot_x + 16 + SMALL_MARGIN, slot_y + 1, t.tx3);
	}
	Box slot{slot_x, slot_y, slot_x + 16, slot_y + 16};
	int icon_ops = 0, tiles = 0, other_color = 0;
	Box ib{1e9, 1e9, -1e9, -1e9};
	for (auto &o : ops) {
		if (o.kind == Op::TEXT || !overlaps(bounds(o), slot)) {
			continue;
		}
		Box b = bounds(o);
		bool tile = o.kind == Op::TEX || o.kind == Op::THUMB || o.kind == Op::GRAD ||
		            (b.x1 - b.x0 >= 12 && b.y1 - b.y0 >= 12); // a backing that would cover the slot
		tiles += tile;
		if (tile) {
			continue;
		}
		const float e = 0.51f;
		bool inside = b.x0 >= slot.x0 - e && b.x1 <= slot.x1 + e && b.y0 >= slot.y0 - e && b.y1 <= slot.y1 + e;
		if (inside && o.color == t.tx3) {
			icon_ops++;
			ib.x0 = std::min(ib.x0, b.x0), ib.y0 = std::min(ib.y0, b.y0);
			ib.x1 = std::max(ib.x1, b.x1), ib.y1 = std::max(ib.y1, b.y1);
		} else {
			other_color++;
		}
	}
	CHECK(tiles == 0, "%s: no texture / opaque square backing in the 16x16 like slot (%d)", label, tiles);
	CHECK(other_color == 0, "%s: nothing but the like icon (count color) in the slot (%d other)", label, other_color);
	CHECK(icon_ops >= 10, "%s: vector like icon drawn in the slot (%d primitives)", label, icon_ops);
	if (icon_ops) {
		float cx = (ib.x0 + ib.x1) / 2, cy = (ib.y0 + ib.y1) / 2;
		CHECK(std::fabs(cx - (slot_x + 8)) <= 1.5f && std::fabs(cy - (slot_y + 8)) <= 1.5f && ib.x1 - ib.x0 >= 10 &&
		          ib.y1 - ib.y0 >= 9,
		      "%s: icon centered in the slot (bbox %.1f,%.1f-%.1f,%.1f)", label, ib.x0, ib.y0, ib.x1, ib.y1);
	}
}

// ---- fixtures
// ----------------------------------------------------------------------------------------------------------
static PostView *make_comment(bool with_reply) { // placed by View::draw(x, y) like the comment list does
	PostView *p = (new PostView(0, 0, 320))
	                  ->set_author_name("@fixture_author")
	                  ->set_time_str("6 days ago")
	                  ->set_upvote_str("186")
	                  ->set_author_icon_url("fixture://avatar")
	                  ->set_content_lines({"The librarian continues the", "story in the next part", "of the series",
	                                       "and a fourth line"})
	                  ->set_has_more_replies([]() { return true; });
	if (with_reply) {
		PostView *r = (new PostView(0, 0, 320))
		                  ->set_author_name("@fixture_reply")
		                  ->set_time_str("2 days ago")
		                  ->set_upvote_str("12")
		                  ->set_author_icon_url("fixture://avatar2")
		                  ->set_content_lines({"A nested reply"})
		                  ->set_has_more_replies([]() { return false; })
		                  ->set_is_reply(true);
		p->replies.push_back(r);
		p->replies_shown = 1;
	}
	return p;
}
static float like_slot_y(float y0, int lines_shown, bool show_more, float icon) { // the draw_ layout, restated
	float y = y0 + DEFAULT_FONT_INTERVAL * (1 + lines_shown);
	if (show_more) {
		y += SMALL_MARGIN + DEFAULT_FONT_INTERVAL;
	}
	return std::max(y, y0 + icon + SMALL_MARGIN) + SMALL_MARGIN;
}

static std::string capture_dir;
static void write_capture(const std::string &name, u32 background, const std::string &note) {
	if (capture_dir.empty()) {
		return;
	}
	std::ofstream out(capture_dir + "/" + name + ".json");
	out << "{\"name\":\"" << name << "\",\"width\":320,\"height\":240,\"background\":" << background << ",\"note\":\""
	    << note << "\",\"ops\":[\n";
	static const char *kinds[] = {"rect", "text", "tri", "line", "thumb", "grad", "rect"};
	for (size_t i = 0; i < ops.size(); i++) {
		const Op &o = ops[i];
		u32 c = o.kind == Op::TEX && !o.color ? 0xFFFFFFFF : o.color; // a texture: its tint (white when untinted)
		out << "{\"k\":\"" << kinds[o.kind] << "\",\"x\":" << o.x << ",\"y\":" << o.y << ",\"w\":" << o.w
		    << ",\"h\":" << o.h << ",\"c\":" << c << ",\"s\":" << o.size << ",\"t\":\"" << o.text << "\",\"p\":[";
		for (int j = 0; j < 6; j++) {
			out << o.pts[j] << (j < 5 ? "," : "");
		}
		out << "],\"cs\":[" << c << "," << c << "," << c << "," << c << "]}" << (i + 1 < ops.size() ? ",\n" : "\n");
	}
	out << "]}\n";
}

int main(int argc, char **argv) {
	if (argc > 1) {
		capture_dir = argv[1];
	}
	const ThemeId themes[] = {ThemeId::GLACIER, ThemeId::SLATE, ThemeId::CORAL, ThemeId::MINT};
	for (ThemeId id : themes) {
		theme_apply(id);
		const char *tn = theme_key(id);
		const ThemeTokens &t = theme_cur();
		char label[96];

		// 1. a comment with "Show more", one nested reply shown and "Show more replies" (the reference photo's row)
		ops.clear();
		const float y0 = 4;
		PostView *p = make_comment(true);
		static_cast<View *>(p)->draw(0, y0);
		float sx = SMALL_MARGIN * 2 + POST_ICON_SIZE, sy = like_slot_y(y0, 3, true, POST_ICON_SIZE);
		snprintf(label, sizeof(label), "%s comment", tn);
		check_like_row(label, sx, sy, "186");
		// the reply starts below the parent's like row and its "Hide replies" line
		float ry = sy + 16 + SMALL_MARGIN + SMALL_MARGIN + DEFAULT_FONT_INTERVAL + SMALL_MARGIN;
		snprintf(label, sizeof(label), "%s nested reply", tn);
		check_like_row(label, SMALL_MARGIN * 2 + REPLY_ICON_SIZE, like_slot_y(ry, 1, false, REPLY_ICON_SIZE), "12");
		CHECK(legacy_thumb_draws() == 0, "%s: legacy thumb_up texture drawn %d time(s) (opaque tile)", tn,
		      legacy_thumb_draws());
		const Op *sm = find_text("Show more replies");
		printf("LAYOUT %s comment height=%g count=(%g,%g) reply_count=(%g,%g) show_more_replies=(%g,%g)\n", tn,
		       p->get_height(), find_text("186") ? find_text("186")->x : -1,
		       find_text("186") ? find_text("186")->y : -1, find_text("12") ? find_text("12")->x : -1,
		       find_text("12") ? find_text("12")->y : -1, sm ? sm->x : -1, sm ? sm->y : -1);
		write_capture(std::string("fixture_comment_like_") + tn, t.surf, // fixture background: the sheet surface
		              "[fixture] PostView comment + nested reply, NOT a console screenshot");

		// 2. interactions unchanged: the like slot is display-only, "Show more replies" still works
		int load_more = 0, author = 0;
		p->set_on_load_more_replies_pressed([&](PostView &) { load_more++; });
		p->set_on_author_icon_pressed([&](const PostView &) { author++; });
		float h_before = p->get_height();
		size_t lines_before = p->lines_shown, replies_before = p->replies_shown;
		Hid_info tap;
		tap.p_touch = tap.h_touch = true, tap.touch_x = (int)sx + 8, tap.touch_y = (int)sy + 8;
		Hid_info up;
		up.touch_x = up.touch_y = -1;
		static_cast<View *>(p)->update(tap, 0, y0), static_cast<View *>(p)->update(up, 0, y0);
		CHECK(load_more == 0 && author == 0 && p->get_height() == h_before && p->lines_shown == lines_before &&
		          p->replies_shown == replies_before,
		      "%s: tapping the like icon changes nothing (display only)", tn);
		if (sm) {
			tap.touch_x = (int)sm->x + 4, tap.touch_y = (int)sm->y + 6;
			static_cast<View *>(p)->update(tap, 0, y0), static_cast<View *>(p)->update(up, 0, y0);
			CHECK(load_more == 1, "%s: 'Show more replies' still loads more (%d)", tn, load_more);
		}
		p->recursive_delete_subviews();
		delete p;

		// 3. a community post (same PostView, timestamps off)
		ops.clear();
		PostView *c = (new PostView(0, 0, 320))
		                  ->set_author_name("@fixture_channel")
		                  ->set_time_str("1 week ago")
		                  ->set_upvote_str("1.2K")
		                  ->set_author_icon_url("fixture://channel")
		                  ->set_content_lines({"A community post"})
		                  ->set_has_more_replies([]() { return false; })
		                  ->set_disable_timestamps(true);
		static_cast<View *>(c)->draw(0, 0);
		snprintf(label, sizeof(label), "%s community post", tn);
		check_like_row(label, SMALL_MARGIN * 2 + POST_ICON_SIZE, like_slot_y(0, 1, false, POST_ICON_SIZE), "1.2K");
		printf("LAYOUT %s community height=%g count=(%g,%g)\n", tn, c->get_height(),
		       find_text("1.2K") ? find_text("1.2K")->x : -1, find_text("1.2K") ? find_text("1.2K")->y : -1);
		delete c;

		// 4. description mode: no like row at all
		ops.clear();
		PostView *d = (new PostView(0, 0, 320))
		                  ->set_author_name("@fixture_desc")
		                  ->set_upvote_str("999")
		                  ->set_content_lines({"A video description"})
		                  ->set_has_more_replies([]() { return false; })
		                  ->set_is_description_mode(true);
		static_cast<View *>(d)->draw(0, 0);
		int slot_like = 0;
		for (auto &o : ops) {
			slot_like += o.kind != Op::TEXT;
		}
		CHECK(find_text("999") == nullptr && legacy_thumb_draws() == 0 && slot_like == 0,
		      "%s: description mode draws no like row (%d shapes)", tn, slot_like);
		printf("LAYOUT %s description height=%g\n", tn, d->get_height());
		delete d;
	}
	printf("post like: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
