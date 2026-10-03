#pragma once
// OpenTube r13 themes (design/opentube-modern-r13/concepts/modern/index.html, the four [data-theme] token sets).
// One layout, four color worlds: every UI color is read from the CURRENT theme at draw time (ui/colors.hpp), so a
// theme switch repaints every open screen on the next frame and nothing keeps an old color.
// Colors are ABGR8888 (citro2d), written from the reference #rrggbb through THEME_RGB().
#include <cstdint>
#include <string>

#define THEME_RGB(hex) ((uint32_t)(0xFF000000u | ((hex) & 0xFF) << 16 | ((hex) & 0xFF00) | ((hex) >> 16 & 0xFF)))
#define THEME_ALPHA(abgr, a) ((uint32_t)(((abgr) & 0x00FFFFFFu) | ((uint32_t)(a) & 0xFF) << 24))

enum class ThemeId { GLACIER = 0, SLATE = 1, CORAL = 2, MINT = 3 };
constexpr int THEME_COUNT = 4;
constexpr ThemeId THEME_DEFAULT = ThemeId::SLATE;

struct ThemeTokens {
	const char *key;  // stable settings value
	const char *name; // displayed name
	bool light;
	// reference tokens (--bg, --surf, --surf2, --line, --tx, --tx2, --tx3, --acc, --acc-ink, --acc-soft, --acc-tx,
	// --ok, --warn, --err, --err-ink, --top, --amb-tint, --sel, --sel-ink, --kc, --shadow)
	uint32_t bg, surf, surf2, line;
	uint32_t tx, tx2, tx3;
	uint32_t acc, acc_ink, acc_soft, acc_tx;
	uint32_t ok, warn, err, err_ink;
	uint32_t top, amb_tint, sel, sel_ink, kc, shadow;
	float amb, tint_op; // ambient glow strength and tint opacity
	// derived (theme.cpp): opaque accent-soft over the ground, pressed tone of raised controls
	uint32_t acc_soft_on_bg, pressed;
};

extern ThemeId var_theme;
extern const ThemeTokens *theme_current_tokens;
inline const ThemeTokens &theme_cur() { return *theme_current_tokens; }
const ThemeTokens &theme_tokens(ThemeId id);

// switches the current theme, keeps the legacy dark flag in sync and requests a redraw
void theme_apply(ThemeId id);
const char *theme_key(ThemeId id);
bool theme_parse(const std::string &value, ThemeId *out); // exact stable key only
// settings.txt resolution: a valid `theme` wins; otherwise legacy `dark_theme` 0 -> Glacier; 1 / missing / invalid
// -> Slate (the default)
ThemeId theme_resolve(bool has_theme, const std::string &theme_value, bool has_dark, const std::string &dark_value);
int theme_legacy_dark(ThemeId id); // value written to `dark_theme` for older builds / light-dark assets

uint32_t theme_blend(uint32_t from, uint32_t to, float ratio); // per channel, alpha included
