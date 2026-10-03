#pragma once
#include "view.hpp"
#include "../ui_common.hpp"

// Tab selector at the top, unfixed height

struct Tab2View : public FixedWidthView {
  private:
	std::vector<UI::FlexibleString<Tab2View>> tab_texts;

  public:
	using CallBackFuncType = std::function<void(const Tab2View &value)>;
	Tab2View(double x0, double y0, double width) : View(x0, y0), FixedWidthView(x0, y0, width) {}
	virtual ~Tab2View() {}

	std::vector<View *> views;
	double tab_selector_height = 30; // OpenTube r13: segmented control (26 px track)
	double tab_selector_selected_line_height = 2;
	double tab_font_size = modern::T_META;
	int selected_tab = 0;
	int tab_holding = -1;
	bool lr_tab_switch_enabled = true;

	int get_tab_num() const { return std::min(tab_texts.size(), views.size()); }
	float tab_width() const { return (x1 - x0 - 20) / get_tab_num(); }
	float tab_pos_x(int tab) const { return x0 + 10 + (x1 - x0 - 20) * tab / get_tab_num(); }

	template <class T> Tab2View *set_tab_texts(const std::vector<T> &tab_texts) {
		this->tab_texts = decltype(this->tab_texts)(tab_texts.begin(), tab_texts.end());
		return this;
	}
	Tab2View *set_views(const std::vector<View *> &views, int selected_tab = 0) {
		this->views = views;
		this->selected_tab = selected_tab;
		return this;
	}
	Tab2View *set_lr_tab_switch_enabled(bool lr_tab_switch_enabled) {
		this->lr_tab_switch_enabled = lr_tab_switch_enabled;
		return this;
	}
	Tab2View *set_tab_font_size(double tab_font_size) {
		(void)tab_font_size; // r13: one label size for every selector
		return this;
	}
	void on_scroll() override {
		tab_holding = -1;
		views[selected_tab]->on_scroll();
	}
	void reset_holding_status_() override {
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
	float get_height() const override { return tab_selector_height + views[selected_tab]->get_height(); }

	void draw_() const override {
		int tab_num = get_tab_num();
		if (!tab_num) {
			return;
		}

		views[selected_tab]->draw(x0, y0 + tab_selector_height);

		// OpenTube segmented control: surface track with hairline, active segment raised
		const ThemeTokens &t = theme_cur();
		modern::rect(x0, y0, x1 - x0, tab_selector_height, DEFAULT_BACK_COLOR);
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
		int tab_holded = -1;
		if (key.touch_y >= y0 && key.touch_y < y0 + tab_selector_height) {
			tab_holded = std::max(0, std::min<int>(get_tab_num() - 1, (key.touch_x - x0 - 10) * get_tab_num() / (x1 - x0 - 20)));
		}

		if (key.p_touch) {
			tab_holding = tab_holded;
		}
		if (key.touch_x == -1 && tab_holding != -1) {
			selected_tab = tab_holding, var_need_refresh = true;
		}
		if (tab_holded != tab_holding) {
			tab_holding = -1;
		}

		views[selected_tab]->update(key, x0, y0 + tab_selector_height);
	}
};
