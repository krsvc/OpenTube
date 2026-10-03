#pragma once
#include "view.hpp"
#include "../ui_common.hpp"

// simple horizontal line
struct ProgressBarView : public FixedSizeView {
	std::function<u32()> get_color = []() { return POCKET_ACCENT; };
	double progress = 0;
	double progress_displayed = 0;
	double progress_displayed_change_speed = 0.05;

  public:
	ProgressBarView(double x0, double y0, double width, double height)
	    : View(x0, y0), FixedSizeView(x0, y0, width, height) {}
	virtual ~ProgressBarView() {}

	ProgressBarView *set_progress(double progress) {
		this->progress = progress;
		return this;
	}
	ProgressBarView *set_get_color(std::function<u32()> get_color) {
		this->get_color = get_color;
		return this;
	}

	void draw_() const override {
		// OpenTube: rounded track in the raised surface, filled part in the bar color
		double bar_width = get_width() - SMALL_MARGIN * 2;
		float h = get_height();
		modern::round_rect(x0 + SMALL_MARGIN, y0, bar_width, h, h / 2, POCKET_SURFACE);
		float width = std::min(bar_width, bar_width * progress_displayed);
		if (width > 0.5f) {
			modern::round_rect(x0 + SMALL_MARGIN, y0, std::max<float>(width, h), h, h / 2, get_color());
		}
	}
	void update_(Hid_info key) override {
		if (progress != progress_displayed) {
			var_need_refresh = true;
		}
		progress_displayed =
		    std::min(progress, progress_displayed + (progress - progress_displayed) * progress_displayed_change_speed);
	}
};
