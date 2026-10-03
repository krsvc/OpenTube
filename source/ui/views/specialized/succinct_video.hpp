#pragma once
#include <vector>
#include <string>
#include "network_decoder/thumbnail_loader.hpp"
#include "ui/ui_common.hpp"
#include "ui/modern.hpp"
#include "ui/shell.hpp"
#include "../view.hpp"

#define VIDEO_LIST_THUMBNAIL_HEIGHT 54 // row height (OpenTube r13: 54 px rows, 82x46 thumbnails)
#define VIDEO_LIST_THUMBNAIL_WIDTH 96  // legacy: text width budget used by callers that pre-wrap titles

// OpenTube r13 video row: 82x46 rounded thumbnail with a duration badge, 2-line title, one meta line. Browse scenes:
// the first tap (or the D-pad, ui/views/cue.hpp) cues the row and lights the top screen; a tap on the cued row or A
// opens it. The player keeps single-tap open.
struct SuccinctVideoView : public FixedSizeView {
  private:
	std::vector<std::string> title_lines;
	std::vector<std::string> auxiliary_lines;
	std::string bottom_right_overlay;
	std::string channel_name;
	std::function<void(View &view)> on_open;
	mutable std::vector<std::string> wrapped_title; // cache: wrapping measures glyphs, done once per width
	mutable float wrapped_width = -1;
	mutable std::string fitted_meta;

  public:
	static constexpr float PAD_X = 9, THUMB_W = 82, THUMB_H = 46, GAP = 8, RADIUS = 7;
	int thumbnail_handle = -1;
	std::string thumbnail_url;

	bool is_playlist = false;

	SuccinctVideoView(double x0, double y0, double width, double height)
	    : View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	virtual ~SuccinctVideoView() {}

	SuccinctVideoView *set_title_lines(const std::vector<std::string> &title_lines) { // mandatory
		this->title_lines = title_lines;
		wrapped_width = -1;
		return this;
	}
	SuccinctVideoView *set_auxiliary_lines(const std::vector<std::string> &auxiliary_lines) { // mandatory
		this->auxiliary_lines = auxiliary_lines;
		wrapped_width = -1;
		return this;
	}
	SuccinctVideoView *set_bottom_right_overlay(const std::string &bottom_right_overlay) {
		this->bottom_right_overlay = bottom_right_overlay;
		return this;
	}
	SuccinctVideoView *set_thumbnail_url(const std::string &thumbnail_url) {
		this->thumbnail_url = thumbnail_url;
		return this;
	}
	SuccinctVideoView *set_is_playlist(bool is_playlist) {
		this->is_playlist = is_playlist;
		return this;
	}
	SuccinctVideoView *set_channel_name(const std::string &channel_name) {
		this->channel_name = channel_name;
		wrapped_width = -1;
		return this;
	}
	View *set_on_view_released(std::function<void(View &view)> on_open) override {
		this->on_open = on_open;
		View::on_view_released = [this](View &view) {
			if (shell::cue_enabled() && !shell::cue_tap(static_cast<const View *>(this))) {
				return; // first tap: cued (top screen re-lights), nothing starts
			}
			if (this->on_open) {
				this->on_open(view);
			}
		};
		return this;
	}
	void open() {
		if (on_open) {
			on_open(*this);
		}
	}
	bool can_open() const { return (bool)on_open; }
	std::string full_title() const {
		std::string res;
		for (auto &line : title_lines) {
			res += (res.empty() ? "" : " ") + line;
		}
		return res;
	}
	std::string meta_text() const {
		std::string res = channel_name;
		for (auto &line : auxiliary_lines) {
			if (!line.empty()) {
				res += (res.empty() ? "" : " \xC2\xB7 ") + line;
			}
		}
		return res;
	}
	shell::Preview preview() const {
		shell::Preview p;
		p.kind = is_playlist ? shell::Preview::Kind::PLAYLIST : shell::Preview::Kind::VIDEO;
		p.title = full_title();
		p.channel = channel_name;
		std::string meta;
		for (auto &line : auxiliary_lines) {
			if (!line.empty()) {
				meta += (meta.empty() ? "" : " \xC2\xB7 ") + line;
			}
		}
		p.meta = meta;
		p.duration = bottom_right_overlay;
		p.thumbnail_url = thumbnail_url;
		return p;
	}
	float get_title_width() const { return x1 - x0 - (PAD_X + THUMB_W + GAP) - PAD_X - 18; }
	bool is_cued() const { return shell::cue_enabled() && shell::cue_owner == static_cast<const View *>(this); }

	void draw_background() const override {
		const ThemeTokens &t = theme_cur();
		float bx = x0 + 4, bw = x1 - x0 - 8;
		if (is_cued()) {
			modern::round_rect(bx, y0, bw, y1 - y0, 10, t.acc);
			modern::round_rect(bx + 1.5f, y0 + 1.5f, bw - 3, y1 - y0 - 3, 8.5f, t.surf2);
			return;
		}
		u32 color = get_background_color ? get_background_color(*this) : background_color;
		if ((color >> 24) && color != shell::ground()) {
			modern::round_rect(bx, y0, bw, y1 - y0, 10, color);
		}
	}
	u32 row_ground() const {
		if (is_cued()) {
			return theme_cur().surf2;
		}
		u32 color = get_background_color ? get_background_color(*this) : background_color;
		return (color >> 24) ? color : shell::ground();
	}
	void draw_() const override {
		const ThemeTokens &t = theme_cur();
		float row_h = y1 - y0;
		float th_h = std::min<float>(THUMB_H, row_h - 8), th_w = th_h * THUMB_W / THUMB_H;
		float tx = x0 + PAD_X, ty = y0 + (row_h - th_h) / 2;
		if (!thumbnail_draw(thumbnail_handle, tx, ty, th_w, th_h)) {
			modern::rect(tx, ty, th_w, th_h, t.surf2); // neutral until the image arrives
			modern::icon(modern::Icon::PLAY, tx + th_w / 2, ty + th_h / 2, 14, t.tx3);
		}
		if (is_playlist) {
			double overlay_proportion = 0.34;
			modern::rect(tx + th_w * (1 - overlay_proportion), ty, th_w * overlay_proportion, th_h, 0xA0000000);
			Draw_x_centered(LOCALIZED(PLAYLIST_SHORT), tx + th_w * (1 - overlay_proportion), tx + th_w,
			                ty + th_h / 2 - 6, modern::T_KEY, modern::T_KEY, (u32)-1);
		}
		if (bottom_right_overlay.size()) { // duration badge on the thumbnail
			float w = modern::text_w(bottom_right_overlay, modern::T_KEY) + 6;
			modern::round_rect(tx + th_w - 3 - w, ty + th_h - 3 - 12, w, 12, 3, 0xC7000000);
			modern::text_center(bottom_right_overlay, tx + th_w - 3 - w, tx + th_w - 3,
			                    modern::text_y(ty + th_h - 15, 12, modern::T_KEY), modern::T_KEY, 0xFFFFFFFF);
		}
		modern::corner_mask(tx, ty, th_w, th_h, RADIUS, row_ground());

		float text_x = tx + th_w + GAP, text_w = x1 - PAD_X - 18 - text_x;
		if (wrapped_width != text_w) {
			wrapped_title = modern::wrap(full_title(), text_w, modern::T_ROW, 2);
			fitted_meta = modern::ellipsize(meta_text(), x1 - PAD_X - text_x, modern::T_META);
			wrapped_width = text_w;
		}
		float block = wrapped_title.size() * 15 + 14;
		float y = y0 + (row_h - block) / 2 - 1;
		for (auto &line : wrapped_title) {
			modern::text(line, text_x, y, modern::T_ROW, t.tx);
			y += 15;
		}
		y += 1;
		modern::text(fitted_meta, text_x, y, modern::T_META, t.tx2);
		if (is_cued()) {
			modern::key_chip("A", x1 - 4 - 6 - 15, y0 + 6, t.acc, t.acc_ink);
		}
	}
	void update_(Hid_info key) override {}
};
