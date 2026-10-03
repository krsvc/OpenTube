#include <functional>
#include "ui/views/view.hpp"
#include "ui/colors.hpp"
#include "variables.hpp"
#include "ui/shell.hpp"

u32 View::blend_color(u32 from, u32 to, double ratio) {
	ratio = std::max(0.0, std::min(1.0, ratio));
	u32 res = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		int a = (from >> shift) & 0xFF;
		int b = (to >> shift) & 0xFF;
		res |= (u32)(a + (b - a) * ratio + 0.5) << shift;
	}
	return res;
}

static u32 pressed_background(const View &view, u32 idle_color) {
	if (view.touch_darkness > 0 && view.touch_darkness < 1) {
		var_need_refresh = true;
	}
	return View::blend_color(idle_color, POCKET_PRESSED, view.touch_darkness);
}

// list rows: the ground they sit on (page or sheet), pressed tone while held
const std::function<u32(const View &)> View::STANDARD_BACKGROUND = [](const View &view) {
	return pressed_background(view, shell::ground());
};
// buttons and chips: raised surface, pressed tone while held
const std::function<u32(const View &)> View::STANDARD_BUTTON_BACKGROUND = [](const View &view) {
	return pressed_background(view, POCKET_SURFACE);
};
