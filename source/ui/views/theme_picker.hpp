#pragma once
// OpenTube r13 Theme setting: four named swatch cards (Glacier, Slate, Coral, Mint Ink) in one row; the current one is
// ringed in the accent. A tap applies the theme at once (every screen reads the current tokens at draw time) and the
// owner persists it. Card rectangles are drawn and hit-tested from the same function.
#include <functional>
#include "view.hpp"
#include "../ui_common.hpp"
#include "ui/theme.hpp"

struct ThemePickerView : public FixedSizeView {
	using CallBackFuncType = std::function<void(ThemeId)>;
	std::function<std::string()> title;
	CallBackFuncType on_change;
	int holding = -1;
	static constexpr float TITLE_H = DEFAULT_FONT_INTERVAL + 2, GAP = 6, SIDE = 6;

	ThemePickerView(double x0, double y0, double width, double height)
	    : View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	virtual ~ThemePickerView() {}

	ThemePickerView *set_title(std::function<std::string()> title) {
		this->title = title;
		return this;
	}
	ThemePickerView *set_on_change(CallBackFuncType on_change) {
		this->on_change = on_change;
		return this;
	}
	void card_rect(int i, float *x, float *y, float *w, float *h) const {
		*w = (x1 - x0 - SIDE * 2 - GAP * (THEME_COUNT - 1)) / THEME_COUNT;
		*h = y1 - y0 - TITLE_H - 4;
		*x = x0 + SIDE + i * (*w + GAP);
		*y = y0 + TITLE_H;
	}
	int card_at(int tx, int ty) const {
		for (int i = 0; i < THEME_COUNT; i++) {
			float x, y, w, h;
			card_rect(i, &x, &y, &w, &h);
			if (tx >= x && tx < x + w && ty >= y && ty < y + h) {
				return i;
			}
		}
		return -1;
	}
	void reset_holding_status_() override { holding = -1; }
	void on_scroll() override { holding = -1; }

	void draw_() const override {
		const ThemeTokens &t = theme_cur();
		if (title) {
			Draw(title(), x0 + SMALL_MARGIN, y0, 0.5, 0.5, t.tx);
		}
		for (int i = 0; i < THEME_COUNT; i++) {
			const ThemeTokens &s = theme_tokens((ThemeId)i);
			float x, y, w, h;
			card_rect(i, &x, &y, &w, &h);
			bool on = var_theme == (ThemeId)i;
			if (on) {
				modern::round_rect(x, y, w, h, 10, t.acc);
			}
			modern::round_rect(x + 1.5f, y + 1.5f, w - 3, h - 3, 8.5f, holding == i ? t.pressed : t.surf2);
			// swatch: the theme's ground with its accent, as in the reference switcher
			float cx = x + w / 2, cy = y + 13, r = 8;
			modern::circle(cx, cy, r + 1, t.line);
			modern::circle(cx, cy, r, s.bg);
			modern::pie(cx, cy, r, -45, 135, s.acc); // lower-right half in the accent
			std::string name = modern::ellipsize(s.name, w - 6, modern::T_KEY);
			modern::text_center(name, x, x + w, y + h - 16, modern::T_KEY, on ? t.tx : t.tx2);
		}
	}
	void update_(Hid_info key) override {
		if (key.p_touch) {
			holding = card_at(key.touch_x, key.touch_y);
		} else if (key.touch_x != -1 && holding != -1 && card_at(key.touch_x, key.touch_y) != holding) {
			holding = -1;
		}
		if (key.touch_x == -1 && holding != -1) {
			ThemeId id = (ThemeId)holding;
			holding = -1;
			if (id != var_theme) {
				theme_apply(id); // repaints every screen on the next frame
				if (on_change) {
					on_change(id);
				}
			}
			var_need_refresh = true;
		}
	}
};
