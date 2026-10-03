#pragma once
#include "view.hpp"
#include "../ui_common.hpp"

struct TabView : public FixedSizeView {
  private:
	std::vector<UI::FlexibleString<TabView>> tab_texts;

  public:
	using CallBackFuncType = std::function<void(const TabView &value)>;
	TabView(double x0, double y0, double width, double height) : View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	virtual ~TabView() {}

	// OpenTube r13: the selector sits on TOP of the content (the bottom edge belongs to the nav).
	//   CHIPS: 32 px row of 22 px pill chips (home categories), SEGMENTED: 26 px track in a 30 px row,
	//   HIDDEN: no selector (the owner switches tabs, e.g. the player's sections).
	enum class Style { CHIPS, SEGMENTED, HIDDEN };
	Style style = Style::SEGMENTED;
	std::vector<View *> views;
	double tab_selector_height = 30;
	double tab_selector_selected_line_height = 2;
	int selected_tab = 0;
	int tab_holding = -1;
	bool stretch_subview = false;
	bool lr_tab_switch_enabled = true;
	double tab_font_size = modern::T_META;

	static constexpr float CHIP_H = 22, CHIP_GAP = 5, CHIP_PAD = 10, CHIP_SIDE = 8;
	int get_tab_num() const { return std::min(tab_texts.size(), views.size()); }
	float tab_width() const { return (x1 - x0 - 20) / std::max(1, get_tab_num()); }
	float tab_pos_x(int tab) const { // segmented: segment left edges (tab_pos_x(n) = right edge of the last one)
		return x0 + 10 + (x1 - x0 - 20) * tab / std::max(1, get_tab_num());
	}
	std::string tab_text(int i) const { return tab_texts[i]; }
	// chip i of the CHIPS style (the same rectangle is drawn and hit-tested)
	void chip_rect(int i, float *x, float *w) const {
		float cx = x0 + CHIP_SIDE, scroll = 0;
		std::vector<float> xs, ws;
		for (int j = 0; j < get_tab_num(); j++) {
			float cw = modern::text_w(tab_texts[j], tab_font_size) + CHIP_PAD * 2;
			xs.push_back(cx), ws.push_back(cw);
			cx += cw + CHIP_GAP;
		}
		if (selected_tab < (int)xs.size()) { // keep the selected chip inside the row
			scroll = std::max(0.0f, xs[selected_tab] + ws[selected_tab] - (float)(x1 - CHIP_SIDE));
		}
		*x = xs[i] - scroll, *w = ws[i];
	}
	float chip_y() const { return y0 + (tab_selector_height - CHIP_H) / 2; }
	int tab_at(int tx, int ty) const {
		if (style == Style::HIDDEN || ty < y0 || ty >= y0 + tab_selector_height) {
			return -1;
		}
		for (int i = 0; i < get_tab_num(); i++) {
			if (style == Style::CHIPS) {
				float cx, cw;
				chip_rect(i, &cx, &cw);
				if (tx >= cx && tx < cx + cw && ty >= chip_y() && ty < chip_y() + CHIP_H) {
					return i;
				}
			} else if (tx >= tab_pos_x(i) && tx < tab_pos_x(i + 1)) {
				return i;
			}
		}
		return -1;
	}

	template <typename T> TabView *set_tab_texts(const std::vector<T> &tab_texts) {
		this->tab_texts = std::vector<UI::FlexibleString<TabView>>(tab_texts.begin(), tab_texts.end());
		return this;
	}
	TabView *set_views(const std::vector<View *> &views, int selected_tab = 0) {
		this->views = views;
		this->selected_tab = selected_tab;
		return this;
	}
	TabView *set_style(Style style) {
		this->style = style;
		tab_selector_height = style == Style::CHIPS ? 32 : style == Style::SEGMENTED ? 30 : 0;
		return this;
	}
	TabView *set_stretch_subview(bool stretch_subview) {
		this->stretch_subview = stretch_subview;
		return this;
	}
	TabView *set_tab_font_size(double tab_font_size) {
		(void)tab_font_size; // r13: one label size for every selector
		return this;
	}
	TabView *set_lr_tab_switch_enabled(bool lr_tab_switch_enabled) {
		this->lr_tab_switch_enabled = lr_tab_switch_enabled;
		return this;
	}
	void on_scroll() override { tab_holding = -1; }
	void reset_holding_status_() override { // every page: a hidden page may keep a held row (r13b)
		tab_holding = -1;
		for (auto view : views) {
			view->reset_holding_status();
		}
	}
	void recursive_delete_subviews() override {
		for (auto view : views) {
			view->recursive_delete_subviews();
			delete view;
		}
		views.clear();
	}

	void draw_() const override {
		int tab_num = get_tab_num();
		if (!tab_num) {
			return;
		}
		if (stretch_subview) {
			FixedHeightView *cur_view = dynamic_cast<FixedHeightView *>(views[selected_tab]);
			if (cur_view) {
				cur_view->update_y_range(0, y1 - tab_selector_height - y0);
			}
		}
		views[selected_tab]->draw(x0, y0 + tab_selector_height);
		if (style == Style::HIDDEN) {
			return;
		}
		const ThemeTokens &t = theme_cur();
		// selector over the content that scrolled up behind it
		modern::rect(x0, y0, x1 - x0, tab_selector_height, DEFAULT_BACK_COLOR);
		if (style == Style::CHIPS) {
			for (int i = 0; i < tab_num; i++) {
				float cx, cw;
				chip_rect(i, &cx, &cw);
				if (cx + cw < x0 || cx > x1) {
					continue;
				}
				bool on = i == selected_tab;
				u32 fill = on ? t.sel : i == tab_holding ? t.pressed : t.surf2;
				modern::pill(cx, chip_y(), cw, CHIP_H, fill);
				modern::text_center(tab_texts[i], cx, cx + cw, modern::text_y(chip_y(), CHIP_H, tab_font_size),
				                    tab_font_size, on ? t.sel_ink : t.tx);
			}
			return;
		}
		// segmented: surface track with hairline, active segment raised
		float ty = y0 + (tab_selector_height - 26) / 2;
		modern::pill(x0 + 10, ty, x1 - x0 - 20, 26, t.line);
		modern::pill(x0 + 11, ty + 1, x1 - x0 - 22, 24, t.surf);
		for (int i = 0; i < tab_num; i++) {
			bool on = i == selected_tab;
			if (on || i == tab_holding) {
				modern::pill(tab_pos_x(i) + 2, ty + 2, tab_width() - 4, 22, on ? t.surf2 : t.pressed);
			}
			std::string label = modern::ellipsize(tab_texts[i], tab_width() - 8, tab_font_size);
			modern::text_center(label, tab_pos_x(i), tab_pos_x(i + 1), modern::text_y(ty, 26, tab_font_size),
			                    tab_font_size, on ? t.tx : t.tx2);
		}
	}
	void update_(Hid_info key) override {
		if (lr_tab_switch_enabled) {
			if (key.p_r) {
				selected_tab++;
				if (selected_tab >= (int)views.size()) {
					selected_tab -= views.size();
				}
				var_need_refresh = true;
			}
			if (key.p_l) {
				selected_tab--;
				if (selected_tab < 0) {
					selected_tab += views.size();
				}
				var_need_refresh = true;
			}
		}
		int tab_holded = tab_at(key.touch_x, key.touch_y);
		if (key.p_touch) {
			tab_holding = tab_holded;
			if (tab_holding != -1) {
				var_need_refresh = true;
			}
		}
		if (key.touch_x == -1 && tab_holding != -1) {
			selected_tab = tab_holding, var_need_refresh = true;
		}
		if (tab_holded != tab_holding) {
			tab_holding = -1;
		}

		if (stretch_subview) {
			FixedHeightView *cur_view = dynamic_cast<FixedHeightView *>(views[selected_tab]);
			if (cur_view) {
				cur_view->update_y_range(0, y1 - tab_selector_height - y0);
			}
		}
		if (tab_holding != -1 || (key.touch_y >= y0 && key.touch_y < y0 + tab_selector_height)) {
			key.touch_x = key.touch_y = -1; // a touch on the selector never reaches the content under it
			key.p_touch = false;
		}
		views[selected_tab]->update(key, x0, y0 + tab_selector_height);
	}
};
