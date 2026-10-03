#pragma once
#include <vector>
#include <string>
#include "network_decoder/thumbnail_loader.hpp"
#include "ui/ui_common.hpp"
#include "../view.hpp"
#include "succinct_video.hpp"

#define CHANNEL_ICON_HEIGHT 54
#define CHANNEL_ICON_WIDTH 96

// OpenTube r13 channel row: 46 px round icon in the thumbnail column, name + meta; same cue / open taps as the
// video rows (succinct_video.hpp)
struct SuccinctChannelView : public FixedSizeView {
  private:
	std::string name;
	std::vector<std::string> auxiliary_lines;
	std::function<void(View &view)> on_open;
	mutable std::string fitted_name, fitted_meta;
	mutable float fitted_width = -1;

  public:
	int thumbnail_handle = -1;
	std::string thumbnail_url;

	SuccinctChannelView(double x0, double y0, double width, double height)
	    : View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	virtual ~SuccinctChannelView() {}

	SuccinctChannelView *set_name(const std::string &name) { // mandatory
		this->name = name;
		fitted_width = -1;
		return this;
	}
	SuccinctChannelView *set_auxiliary_lines(const std::vector<std::string> &auxiliary_lines) { // mandatory
		this->auxiliary_lines = auxiliary_lines;
		fitted_width = -1;
		return this;
	}
	SuccinctChannelView *set_thumbnail_url(const std::string &thumbnail_url) {
		this->thumbnail_url = thumbnail_url;
		return this;
	}
	View *set_on_view_released(std::function<void(View &view)> on_open) override {
		this->on_open = on_open;
		View::on_view_released = [this](View &view) {
			if (shell::cue_enabled() && !shell::cue_tap(static_cast<const View *>(this))) {
				return;
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
	std::string meta_text() const {
		std::string res;
		for (auto &line : auxiliary_lines) {
			if (!line.empty()) {
				res += (res.empty() ? "" : " \xC2\xB7 ") + line;
			}
		}
		return res;
	}
	shell::Preview preview() const {
		shell::Preview p;
		p.kind = shell::Preview::Kind::CHANNEL;
		p.title = name;
		p.meta = meta_text();
		p.thumbnail_url = thumbnail_url;
		p.round_thumbnail = true;
		return p;
	}
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
	void draw_() const override {
		const ThemeTokens &t = theme_cur();
		float row_h = y1 - y0, icon = std::min<float>(46, row_h - 8);
		float cx = x0 + SuccinctVideoView::PAD_X + SuccinctVideoView::THUMB_W / 2, cy = y0 + row_h / 2;
		if (!thumbnail_draw(thumbnail_handle, cx - icon / 2, cy - icon / 2, icon, icon)) {
			modern::circle(cx, cy, icon / 2, t.surf2);
			modern::icon(modern::Icon::YOU, cx, cy, icon * 0.55f, t.tx3);
		}
		float text_x = x0 + SuccinctVideoView::PAD_X + SuccinctVideoView::THUMB_W + SuccinctVideoView::GAP;
		float text_w = x1 - SuccinctVideoView::PAD_X - 18 - text_x;
		if (fitted_width != text_w) {
			fitted_name = modern::ellipsize(name, text_w, modern::T_ROW);
			fitted_meta = modern::ellipsize(meta_text(), text_w + 18, modern::T_META);
			fitted_width = text_w;
		}
		float y = y0 + (row_h - 30) / 2 - 1;
		modern::text(fitted_name, text_x, y, modern::T_ROW, t.tx);
		modern::text(fitted_meta, text_x, y + 16, modern::T_META, t.tx2);
		if (is_cued()) {
			modern::key_chip("A", x1 - 4 - 6 - 15, y0 + 6, t.acc, t.acc_ink);
		}
	}
	void update_(Hid_info key) override {}
};
