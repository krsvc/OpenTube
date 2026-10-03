#pragma once
// OpenTube r13 native drawing primitives (design/opentube-modern-r13): rounded cards and pills, rings, rounded-image
// corner masks, a vertex-colored ambient glow, stroked icons, key chips and text fitting. Everything is flat citro2d
// geometry (triangles / solid rects): no shader, no texture allocation, no read-back. Rounded corners are
// non-overlapping triangle fans so translucent fills (accent-soft) blend once.
#include <string>
#include <vector>
#include "3ds.h"
#include "ui/colors.hpp"

namespace modern {

// type scale of the reference (Urbanist px) mapped onto the 3DS system font (line box = 20 px * size)
constexpr float T_DISPLAY = 1.05f;  // 30 px: the big download number
constexpr float T_HEADLINE = 0.64f; // 17.5 px: cued title on the top screen
constexpr float T_TITLE = 0.56f;    // 13.5-16 px: player title, sheet headers
constexpr float T_ROW = 0.5f;       // 12-12.5 px: row titles
constexpr float T_BODY = 0.46f;     // 11.5 px: descriptions, comments
constexpr float T_META = 0.43f;     // 10.5-11 px: channel, views, hints
constexpr float T_KEY = 0.4f;       // 10 px: key chips, badges, nav labels (the floor)

inline float line_h(float size) { return 20.0f * size; }
// y for text vertically centered in [y, y + h)
inline float text_y(float y, float h, float size) { return y + (h - 20.0f * size) / 2 - 1; }

// ---- shapes -------------------------------------------------------------------------------------------------------
void rect(float x, float y, float w, float h, u32 color);
void round_rect(float x, float y, float w, float h, float r, u32 color);
void round_rect_top(float x, float y, float w, float h, float r, u32 color); // sheets: only the top corners
inline void pill(float x, float y, float w, float h, u32 color) { round_rect(x, y, w, h, h / 2, color); }
void circle(float cx, float cy, float r, u32 color);
void pie(float cx, float cy, float r, float from_deg, float to_deg, u32 color); // y grows down: 270 is up
void ring(float cx, float cy, float r, float thickness, u32 color, float from_deg = 0, float to_deg = 360);
// outline of a rounded rect drawn inside [x, x + w) (1.5 px cued ring, hairlines)
void round_rect_outline(float x, float y, float w, float h, float r, float thickness, u32 color);
// thick segment with square ends (a quad); round = round caps
void stroke(float x0, float y0, float x1, float y1, float width, u32 color, bool round = false);
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color);
// paints the outside of a rounded corner in `ground` over an image drawn in [x, x + w) x [y, y + h)
void corner_mask(float x, float y, float w, float h, float r, u32 ground);
// one corner (0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left) of a rect whose corner point is (px, py)
void corner_mask_one(int which, float px, float py, float r, u32 ground);
// vertical gradient (top color -> bottom color, alpha included)
void vgradient(float x, float y, float w, float h, u32 top, u32 bottom);
// ambient glow: an ellipse whose alpha falls from `color` in the middle to 0 at the rim (vertex colors)
void glow(float cx, float cy, float rx, float ry, u32 color);
// color of glow() at (x, y), used to paint masks that must match a glowing ground
u32 glow_at(float cx, float cy, float rx, float ry, u32 color, u32 ground, float x, float y);
u32 over(u32 ground, u32 color); // `color` (with alpha) composited over an opaque ground

// ---- text ---------------------------------------------------------------------------------------------------------
void text(const std::string &s, float x, float y, float size, u32 color);
void text_right(const std::string &s, float x1, float y, float size, u32 color);
void text_center(const std::string &s, float x0, float x1, float y, float size, u32 color);
float text_w(const std::string &s, float size);
std::string ellipsize(const std::string &s, float max_w, float size);
// word wrap into at most max_lines, the last line ellipsized; CJK text without spaces wraps per code point
std::vector<std::string> wrap(const std::string &s, float max_w, float size, int max_lines);

// ---- icons (drawn centered at cx, cy inside an s x s box) ----------------------------------------------------------
enum class Icon {
	HOME, HOME_FILLED, SUBS, SUBS_FILLED, SEARCH, DOWNLOAD, DOWNLOAD_FILLED, YOU, YOU_FILLED,
	PLAY, PAUSE, STOP, BACK10, FWD10, CC, TUNE, CLOSE, TRASH, RETRY, ALERT, SD, HISTORY, SETTINGS, INFO, EXIT,
	LIST, COMMENTS, RELOAD, ACCOUNT, EXPAND, LOADING, LIKE, LIKE_FILLED, DISLIKE, HELP
};
void icon(Icon which, float cx, float cy, float s, u32 color, u32 ground = 0);
void wifi(float cx, float cy, int level /* 0..3, <0 = off */, u32 color);
void battery(float x, float y, int percent, bool charging, u32 color);
void logo_tile(float x, float y, float h); // 16x11 accent tile with the play notch, scaled to height h
void dpad_glyph(float cx, float cy, float s, u32 color, bool up_down, bool left_right);

// ---- key chips ("A Play") -----------------------------------------------------------------------------------------
float key_chip(const std::string &key, float x, float y, u32 fill, u32 ink); // returns the chip width
// a row of hints; returns the right edge. `on_top` uses the neutral 22% gray fill of the top screen
float hints(const std::vector<std::pair<std::string, std::string>> &items, float x, float y, u32 ink, u32 chip_fill,
            u32 chip_ink, float gap = 10);

} // namespace modern
