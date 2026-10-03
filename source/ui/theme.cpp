#include "ui/theme.hpp"
#include <cstdlib>

extern bool var_night_mode;
extern bool var_need_refresh;

uint32_t theme_blend(uint32_t from, uint32_t to, float ratio) {
	ratio = ratio < 0 ? 0 : ratio > 1 ? 1 : ratio;
	uint32_t res = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		int a = (from >> shift) & 0xFF;
		int b = (to >> shift) & 0xFF;
		res |= (uint32_t)(a + (b - a) * ratio + 0.5f) << shift;
	}
	return res;
}

namespace {
// alpha of an rgba() reference token, 0..255
constexpr uint32_t A(float a) { return (uint32_t)(a * 255 + 0.5f); }

ThemeTokens make(ThemeTokens t, float acc_soft_alpha) {
	// --acc-soft is translucent in the dark themes: the opaque value over the ground is used where a flat fill is
	// needed (selected rows); the translucent one is kept for pills drawn over surfaces
	if (acc_soft_alpha < 1) {
		t.acc_soft_on_bg = theme_blend(t.bg, t.acc_soft | 0xFF000000u, acc_soft_alpha);
		t.acc_soft = THEME_ALPHA(t.acc_soft, A(acc_soft_alpha));
	} else {
		t.acc_soft_on_bg = t.acc_soft;
	}
	t.pressed = theme_blend(t.surf2, t.tx, t.light ? 0.08f : 0.10f);
	return t;
}

const ThemeTokens THEMES[THEME_COUNT] = {
    make({"glacier", "Glacier", true,
          THEME_RGB(0xf3f6fb), THEME_RGB(0xffffff), THEME_RGB(0xe6edf7), THEME_RGB(0xd9e2ee),
          THEME_RGB(0x0b1526), THEME_RGB(0x3f4d69), THEME_RGB(0x5a6983),
          THEME_RGB(0x1764f5), THEME_RGB(0xffffff), THEME_RGB(0xdce7ff), THEME_RGB(0x0b45ba),
          THEME_RGB(0x0b6b45), THEME_RGB(0x8f5200), THEME_RGB(0xb8291f), THEME_RGB(0xffffff),
          THEME_RGB(0xe9f0fb), THEME_RGB(0x2f6df2), THEME_RGB(0x0b1526), THEME_RGB(0xffffff), THEME_RGB(0xdde5f0),
          THEME_ALPHA(THEME_RGB(0x000000), A(.16f)),
          .34f, .92f, 0, 0},
         1.0f),
    make({"slate", "Slate", false,
          THEME_RGB(0x19212f), THEME_RGB(0x212b3c), THEME_RGB(0x2b3649), THEME_RGB(0x2f3b50),
          THEME_RGB(0xeef3fb), THEME_RGB(0xa9b5c9), THEME_RGB(0x8b98ae),
          THEME_RGB(0x5c9eff), THEME_RGB(0x061431), THEME_RGB(0x5c9eff), THEME_RGB(0x8cbaff),
          THEME_RGB(0x3fcf8e), THEME_RGB(0xf5b43d), THEME_RGB(0xff6b5e), THEME_RGB(0x2b0703),
          THEME_RGB(0x131b28), THEME_RGB(0x3f73c4), THEME_RGB(0xeef3fb), THEME_RGB(0x19212f), THEME_RGB(0x323e54),
          THEME_ALPHA(THEME_RGB(0x000000), A(.45f)),
          .55f, .88f, 0, 0},
         .18f),
    make({"coral", "Coral", false,
          THEME_RGB(0x1c1917), THEME_RGB(0x26221f), THEME_RGB(0x322d2a), THEME_RGB(0x37312d),
          THEME_RGB(0xf8f3ee), THEME_RGB(0xbbb2aa), THEME_RGB(0x9c938b),
          THEME_RGB(0xff7a66), THEME_RGB(0x2b0d08), THEME_RGB(0xff7a66), THEME_RGB(0xff9b8b),
          THEME_RGB(0x62d493), THEME_RGB(0xf7b545), THEME_RGB(0xff5a4a), THEME_RGB(0x2b0703),
          THEME_RGB(0x17130f), THEME_RGB(0xd27a4c), THEME_RGB(0xf8f3ee), THEME_RGB(0x1c1917), THEME_RGB(0x3b3532),
          THEME_ALPHA(THEME_RGB(0x000000), A(.5f)),
          .6f, .7f, 0, 0},
         .17f),
    make({"mint", "Mint Ink", false,
          THEME_RGB(0x07111f), THEME_RGB(0x0d1a2d), THEME_RGB(0x152540), THEME_RGB(0x1a2a45),
          THEME_RGB(0xe9f2ff), THEME_RGB(0xa2b4d1), THEME_RGB(0x8194b5),
          THEME_RGB(0x37e6b0), THEME_RGB(0x03261b), THEME_RGB(0x37e6b0), THEME_RGB(0x5ff0c3),
          THEME_RGB(0x37e6b0), THEME_RGB(0xffc24b), THEME_RGB(0xff6b7d), THEME_RGB(0x2d0710),
          THEME_RGB(0x050d19), THEME_RGB(0x11917c), THEME_RGB(0x37e6b0), THEME_RGB(0x03261b), THEME_RGB(0x1d2f4d),
          THEME_ALPHA(THEME_RGB(0x000000), A(.55f)),
          .62f, .82f, 0, 0},
         .15f),
};
} // namespace

ThemeId var_theme = THEME_DEFAULT;
const ThemeTokens *theme_current_tokens = &THEMES[(int)THEME_DEFAULT];

const ThemeTokens &theme_tokens(ThemeId id) {
	int i = (int)id;
	return THEMES[i >= 0 && i < THEME_COUNT ? i : (int)THEME_DEFAULT];
}
void theme_apply(ThemeId id) {
	if ((int)id < 0 || (int)id >= THEME_COUNT) {
		id = THEME_DEFAULT;
	}
	var_theme = id;
	theme_current_tokens = &THEMES[(int)id];
	var_night_mode = !THEMES[(int)id].light; // legacy flag: light/dark assets, older code paths
	var_need_refresh = true;
}
const char *theme_key(ThemeId id) { return theme_tokens(id).key; }
bool theme_parse(const std::string &value, ThemeId *out) {
	for (int i = 0; i < THEME_COUNT; i++) {
		if (value == THEMES[i].key) {
			if (out) {
				*out = (ThemeId)i;
			}
			return true;
		}
	}
	return false;
}
ThemeId theme_resolve(bool has_theme, const std::string &theme_value, bool has_dark, const std::string &dark_value) {
	ThemeId id;
	if (has_theme && theme_parse(theme_value, &id)) {
		return id;
	}
	if (has_dark && !dark_value.empty()) {
		char *end;
		long v = strtol(dark_value.c_str(), &end, 10);
		if (!*end && v == 0) {
			return ThemeId::GLACIER; // the r12d light theme
		}
	}
	return THEME_DEFAULT;
}
int theme_legacy_dark(ThemeId id) { return theme_tokens(id).light ? 0 : 1; }
