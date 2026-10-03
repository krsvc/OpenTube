#include "ui/modern.hpp"
#include <algorithm>
#include <cmath>
#include "ui/draw/draw.hpp"

namespace modern {

static constexpr float PI = 3.14159265f;
static float rad(float deg) { return deg * PI / 180; }
static int segments(float r) { return r <= 3 ? 2 : r <= 8 ? 4 : r <= 16 ? 6 : 10; }

void rect(float x, float y, float w, float h, u32 color) {
	if (w > 0 && h > 0) {
		C2D_DrawRectSolid(x, y, 0, w, h, color);
	}
}
void triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color) {
	C2D_DrawTriangle(x0, y0, color, x1, y1, color, x2, y2, color, 0);
}
// pie slice from the center, a0 -> a1 in degrees (y grows downwards: 270 is up)
void pie(float cx, float cy, float r, float a0, float a1, u32 color) {
	int n = std::max(1, (int)(segments(r) * (a1 - a0) / 90 + 0.5f));
	for (int i = 0; i < n; i++) {
		float b0 = rad(a0 + (a1 - a0) * i / n), b1 = rad(a0 + (a1 - a0) * (i + 1) / n);
		triangle(cx, cy, cx + r * std::cos(b0), cy + r * std::sin(b0), cx + r * std::cos(b1), cy + r * std::sin(b1),
		         color);
	}
}
void round_rect(float x, float y, float w, float h, float r, u32 color) {
	r = std::min(r, std::min(w, h) / 2);
	if (r < 0.5f) {
		rect(x, y, w, h, color);
		return;
	}
	rect(x + r, y, w - 2 * r, h, color);
	rect(x, y + r, r, h - 2 * r, color);
	rect(x + w - r, y + r, r, h - 2 * r, color);
	pie(x + r, y + r, r, 180, 270, color);
	pie(x + w - r, y + r, r, 270, 360, color);
	pie(x + w - r, y + h - r, r, 0, 90, color);
	pie(x + r, y + h - r, r, 90, 180, color);
}
void round_rect_top(float x, float y, float w, float h, float r, u32 color) {
	r = std::min(r, std::min(w / 2, h));
	rect(x + r, y, w - 2 * r, r, color);
	rect(x, y + r, w, h - r, color);
	pie(x + r, y + r, r, 180, 270, color);
	pie(x + w - r, y + r, r, 270, 360, color);
}
void circle(float cx, float cy, float r, u32 color) { pie(cx, cy, r, 0, 360, color); }
void ring(float cx, float cy, float r, float t, u32 color, float from_deg, float to_deg) {
	float ri = std::max(0.0f, r - t);
	int n = std::max(2, (int)(std::max(6.0f, r * 1.2f) * (to_deg - from_deg) / 360 + 0.5f));
	for (int i = 0; i < n; i++) {
		float b0 = rad(from_deg + (to_deg - from_deg) * i / n), b1 = rad(from_deg + (to_deg - from_deg) * (i + 1) / n);
		float ox0 = cx + r * std::cos(b0), oy0 = cy + r * std::sin(b0), ox1 = cx + r * std::cos(b1),
		      oy1 = cy + r * std::sin(b1);
		float ix0 = cx + ri * std::cos(b0), iy0 = cy + ri * std::sin(b0), ix1 = cx + ri * std::cos(b1),
		      iy1 = cy + ri * std::sin(b1);
		triangle(ox0, oy0, ox1, oy1, ix0, iy0, color);
		triangle(ix0, iy0, ox1, oy1, ix1, iy1, color);
	}
}
void round_rect_outline(float x, float y, float w, float h, float r, float t, u32 color) {
	r = std::min(r, std::min(w, h) / 2);
	if (r < t) {
		rect(x, y, w, t, color), rect(x, y + h - t, w, t, color);
		rect(x, y + t, t, h - 2 * t, color), rect(x + w - t, y + t, t, h - 2 * t, color);
		return;
	}
	rect(x + r, y, w - 2 * r, t, color);
	rect(x + r, y + h - t, w - 2 * r, t, color);
	rect(x, y + r, t, h - 2 * r, color);
	rect(x + w - t, y + r, t, h - 2 * r, color);
	ring(x + r, y + r, r, t, color, 180, 270);
	ring(x + w - r, y + r, r, t, color, 270, 360);
	ring(x + w - r, y + h - r, r, t, color, 0, 90);
	ring(x + r, y + h - r, r, t, color, 90, 180);
}
void stroke(float x0, float y0, float x1, float y1, float width, u32 color, bool round) {
	float dx = x1 - x0, dy = y1 - y0, len = std::sqrt(dx * dx + dy * dy);
	if (len < 0.01f) {
		return;
	}
	float nx = -dy / len * width / 2, ny = dx / len * width / 2;
	triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x0 - nx, y0 - ny, color);
	triangle(x0 - nx, y0 - ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color);
	if (round && width >= 2.5f) { // caps are invisible on thinner strokes: skip their triangles (frame vertex budget)
		circle(x0, y0, width / 2, color);
		circle(x1, y1, width / 2, color);
	}
}
// the part of the r x r corner square outside the arc, as a fan from the square's corner point
static void corner_fan(float px, float py, float cx, float cy, float r, float a0, float a1, u32 ground) {
	int n = segments(r);
	for (int i = 0; i < n; i++) {
		float b0 = rad(a0 + (a1 - a0) * i / n), b1 = rad(a0 + (a1 - a0) * (i + 1) / n);
		triangle(px, py, cx + r * std::cos(b0), cy + r * std::sin(b0), cx + r * std::cos(b1), cy + r * std::sin(b1),
		         ground);
	}
}
void corner_mask(float x, float y, float w, float h, float r, u32 ground) {
	r = std::min(r, std::min(w, h) / 2);
	if (r < 0.5f) {
		return;
	}
	corner_fan(x, y, x + r, y + r, r, 180, 270, ground);
	corner_fan(x + w, y, x + w - r, y + r, r, 270, 360, ground);
	corner_fan(x + w, y + h, x + w - r, y + h - r, r, 0, 90, ground);
	corner_fan(x, y + h, x + r, y + h - r, r, 90, 180, ground);
}
void corner_mask_one(int which, float px, float py, float r, u32 ground) {
	static const float dx[4] = {1, -1, -1, 1}, dy[4] = {1, 1, -1, -1}, a0[4] = {180, 270, 0, 90};
	corner_fan(px, py, px + dx[which] * r, py + dy[which] * r, r, a0[which], a0[which] + 90, ground);
}
void vgradient(float x, float y, float w, float h, u32 top, u32 bottom) {
	if (w > 0 && h > 0) {
		C2D_DrawRectangle(x, y, 0, w, h, top, top, bottom, bottom);
	}
}
static constexpr float GLOW_CORE = 0.3f; // fraction of the radius at full strength
void glow(float cx, float cy, float rx, float ry, u32 color) {
	const int n = 24;
	u32 clear = color & 0x00FFFFFFu;
	for (int i = 0; i < n; i++) {
		float b0 = 2 * PI * i / n, b1 = 2 * PI * (i + 1) / n;
		float c0 = std::cos(b0), s0 = std::sin(b0), c1 = std::cos(b1), s1 = std::sin(b1);
		float ix0 = cx + rx * GLOW_CORE * c0, iy0 = cy + ry * GLOW_CORE * s0;
		float ix1 = cx + rx * GLOW_CORE * c1, iy1 = cy + ry * GLOW_CORE * s1;
		float ox0 = cx + rx * c0, oy0 = cy + ry * s0, ox1 = cx + rx * c1, oy1 = cy + ry * s1;
		C2D_DrawTriangle(cx, cy, color, ix0, iy0, color, ix1, iy1, color, 0);
		C2D_DrawTriangle(ix0, iy0, color, ox0, oy0, clear, ix1, iy1, color, 0);
		C2D_DrawTriangle(ix1, iy1, color, ox0, oy0, clear, ox1, oy1, clear, 0);
	}
}
u32 over(u32 ground, u32 color) {
	float a = (color >> 24) / 255.0f;
	u32 res = 0xFF000000u;
	for (int shift = 0; shift < 24; shift += 8) {
		float g = (ground >> shift) & 0xFF, c = (color >> shift) & 0xFF;
		res |= (u32)(g + (c - g) * a + 0.5f) << shift;
	}
	return res;
}
u32 glow_at(float cx, float cy, float rx, float ry, u32 color, u32 ground, float x, float y) {
	float dx = (x - cx) / rx, dy = (y - cy) / ry, d = std::sqrt(dx * dx + dy * dy);
	float k = d <= GLOW_CORE ? 1 : d >= 1 ? 0 : 1 - (d - GLOW_CORE) / (1 - GLOW_CORE);
	u32 a = (u32)((color >> 24) * k + 0.5f);
	return over(ground, (color & 0x00FFFFFFu) | a << 24);
}

// ---- text ---------------------------------------------------------------------------------------------------------
void text(const std::string &s, float x, float y, float size, u32 color) { Draw(s, x, y, size, size, color); }
void text_right(const std::string &s, float x1, float y, float size, u32 color) {
	Draw(s, x1 - Draw_get_width(s, size), y, size, size, color);
}
void text_center(const std::string &s, float x0, float x1, float y, float size, u32 color) {
	Draw(s, (x0 + x1 - Draw_get_width(s, size)) / 2, y, size, size, color);
}
float text_w(const std::string &s, float size) { return Draw_get_width(s, size); }

static size_t cp_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }
static std::vector<std::string> code_points(const std::string &s) {
	std::vector<std::string> res;
	for (size_t i = 0; i < s.size();) {
		size_t n = std::min(cp_len((unsigned char)s[i]), s.size() - i);
		res.push_back(s.substr(i, n));
		i += n;
	}
	return res;
}
std::string ellipsize(const std::string &s, float max_w, float size) {
	if (Draw_get_width(s, size) <= max_w) {
		return s;
	}
	std::vector<std::string> cps = code_points(s);
	std::string res;
	float dots = Draw_get_width("...", size);
	for (auto &cp : cps) {
		if (Draw_get_width(res + cp, size) + dots > max_w) {
			break;
		}
		res += cp;
	}
	while (!res.empty() && res.back() == ' ') {
		res.pop_back();
	}
	return res + "...";
}
static bool is_cjk(const std::string &cp) {
	if (cp.size() < 3) {
		return false;
	}
	unsigned c = ((unsigned char)cp[0] & 0x0F) << 12 | ((unsigned char)cp[1] & 0x3F) << 6 | ((unsigned char)cp[2] & 0x3F);
	return cp.size() == 4 || c >= 0x2E80;
}
std::vector<std::string> wrap(const std::string &s, float max_w, float size, int max_lines) {
	// tokens: a word with its trailing spaces, or one CJK code point
	std::vector<std::string> tokens;
	bool last_cjk = false;
	for (auto cp : code_points(s)) {
		if (cp == "\n") {
			cp = " ";
		}
		bool cjk = is_cjk(cp);
		bool start_new = tokens.empty() || cjk || last_cjk || (tokens.back().back() == ' ' && cp != " ");
		if (start_new) {
			tokens.push_back(cp);
		} else {
			tokens.back() += cp;
		}
		last_cjk = cjk;
	}
	std::vector<std::string> lines;
	std::string line;
	auto trimmed = [](std::string l) {
		while (!l.empty() && l.back() == ' ') {
			l.pop_back();
		}
		return l;
	};
	for (size_t i = 0; i < tokens.size(); i++) {
		std::string candidate = line + tokens[i];
		if (Draw_get_width(trimmed(candidate), size) <= max_w) {
			line = candidate;
			continue;
		}
		if ((int)lines.size() + 1 >= max_lines) { // last allowed line: the rest, ellipsized
			std::string rest = line;
			for (size_t j = i; j < tokens.size(); j++) {
				rest += tokens[j];
			}
			lines.push_back(ellipsize(trimmed(rest), max_w, size));
			return lines;
		}
		if (trimmed(line).empty()) { // a single token wider than the line: break it by code points
			std::string part;
			for (auto &cp : code_points(tokens[i])) {
				if (Draw_get_width(part + cp, size) > max_w && !part.empty()) {
					lines.push_back(part);
					part.clear();
					if ((int)lines.size() >= max_lines) {
						return lines;
					}
				}
				part += cp;
			}
			line = part;
		} else {
			lines.push_back(trimmed(line));
			line = tokens[i][0] == ' ' ? std::string() : tokens[i];
			if (Draw_get_width(trimmed(line), size) > max_w) { // re-run this token as an over-long one
				line.clear();
				i--;
			}
		}
	}
	if (!trimmed(line).empty() && (int)lines.size() < max_lines) {
		lines.push_back(trimmed(line));
	}
	return lines;
}

// ---- icons --------------------------------------------------------------------------------------------------------
void icon(Icon which, float cx, float cy, float s, u32 c, u32 ground) {
	const float k = s / 24; // reference icons use a 24 unit view box
	auto X = [&](float v) { return cx + (v - 12) * k; };
	auto Y = [&](float v) { return cy + (v - 12) * k; };
	auto L = [&](float x0, float y0, float x1, float y1, float w) { stroke(X(x0), Y(y0), X(x1), Y(y1), w * k, c, true); };
	auto arc = [&](float x, float y, float r, float t, float a0, float a1) { ring(X(x), Y(y), (r + t / 2) * k, t * k, c, a0, a1); };
	switch (which) {
	case Icon::HOME:
		L(4, 10.5, 12, 4, 2), L(12, 4, 20, 10.5, 2), L(20, 10.5, 20, 20, 2), L(20, 20, 14.5, 20, 2);
		L(14.5, 20, 14.5, 14.5, 2), L(14.5, 14.5, 9.5, 14.5, 2), L(9.5, 14.5, 9.5, 20, 2), L(9.5, 20, 4, 20, 2);
		L(4, 20, 4, 10.5, 2);
		break;
	case Icon::HOME_FILLED:
		triangle(X(3), Y(10.5), X(12), Y(3), X(21), Y(10.5), c);
		rect(X(3.5), Y(10.5), 6 * k, 10.5 * k, c), rect(X(14.5), Y(10.5), 6 * k, 10.5 * k, c);
		rect(X(9.5), Y(10.5), 5 * k, 4 * k, c);
		break;
	case Icon::SUBS:
	case Icon::SUBS_FILLED:
		if (which == Icon::SUBS_FILLED) {
			round_rect(X(2.5), Y(6.5), 19 * k, 14 * k, 3.5 * k, c);
			triangle(X(10.5), Y(10.8), X(10.5), Y(16.2), X(15), Y(13.5), ground);
		} else {
			round_rect_outline(X(2.5), Y(6.5), 19 * k, 14 * k, 3.5 * k, 2 * k, c);
			triangle(X(10.5), Y(10.8), X(10.5), Y(16.2), X(15), Y(13.5), c);
		}
		L(6.5, 4, 17.5, 4, 2);
		break;
	case Icon::SEARCH:
		arc(10.5, 10.5, 6, 2.2, 0, 360), L(15, 15, 20, 20, 2.4);
		break;
	case Icon::DOWNLOAD:
		L(12, 4, 12, 14, 2.2), L(12, 14, 8, 10, 2.2), L(12, 14, 16, 10, 2.2), L(5, 19, 19, 19, 2.2);
		break;
	case Icon::DOWNLOAD_FILLED:
		circle(X(12), Y(11), 8.5 * k, c);
		stroke(X(12), Y(6.5), X(12), Y(13.5), 2 * k, ground, true);
		stroke(X(12), Y(13.5), X(9), Y(10.5), 2 * k, ground, true), stroke(X(12), Y(13.5), X(15), Y(10.5), 2 * k, ground, true);
		L(5, 21.5, 19, 21.5, 2);
		break;
	case Icon::YOU:
	case Icon::ACCOUNT:
		arc(12, 8.5, 3.8, 2, 0, 360), arc(12, 21, 7, 2, 195, 345);
		break;
	case Icon::YOU_FILLED:
		circle(X(12), Y(8.5), 4.8 * k, c), pie(X(12), Y(21.5), 8.2 * k, 180, 360, c);
		break;
	case Icon::PLAY:
		triangle(X(7.5), Y(4.5), X(7.5), Y(19.5), X(19.8), Y(12), c);
		break;
	case Icon::PAUSE:
		round_rect(X(6), Y(4.5), 4.2 * k, 15 * k, 1.3 * k, c), round_rect(X(13.8), Y(4.5), 4.2 * k, 15 * k, 1.3 * k, c);
		break;
	case Icon::STOP:
		round_rect(X(6), Y(6), 12 * k, 12 * k, 2 * k, c);
		break;
	case Icon::BACK10: // r16: open ring with a filled arrowhead at 12 o'clock, "10" in the key size inside (s >= 30)
	case Icon::FWD10: {
		bool back = which == Icon::BACK10;
		float m = back ? 1 : -1; // FWD10 is the mirror image
		if (back) {
			arc(12, 12, 8, 2, 270, 575);
		} else {
			arc(12, 12, 8, 2, -35, 270);
		}
		triangle(X(12 - m * 4.8f), Y(4), X(12 + m * 0.2f), Y(0.9f), X(12 + m * 0.2f), Y(7.1f), c);
		float size = T_KEY * std::max(0.72f, std::min(1.0f, s / 30));
		text_center("10", X(4), X(20), std::floor(text_y(Y(12), 0, size) + 0.5f), size, c);
		break;
	}
	case Icon::RETRY:
	case Icon::RELOAD:
		arc(12, 12, 7, 2.2, -180, 135), L(19.5, 3.8, 19.5, 7.8, 2.2), L(19.5, 7.8, 15.5, 7.8, 2.2);
		break;
	case Icon::CC:
		round_rect_outline(X(3), Y(5.5), 18 * k, 13 * k, 3 * k, 2 * k, c);
		arc(9.3, 12, 1.9, 1.8, 45, 315), arc(15.3, 12, 1.9, 1.8, 45, 315);
		break;
	case Icon::TUNE:
	case Icon::SETTINGS:
		L(4, 7, 12.5, 7, 2), L(17.5, 7, 20, 7, 2), L(4, 17, 6.5, 17, 2), L(11.5, 17, 20, 17, 2);
		arc(15, 7, 2.3, 2, 0, 360), arc(9, 17, 2.3, 2, 0, 360);
		break;
	case Icon::CLOSE:
		L(6.5, 6.5, 17.5, 17.5, 2.3), L(17.5, 6.5, 6.5, 17.5, 2.3);
		break;
	case Icon::TRASH:
		L(4.5, 7, 19.5, 7, 2), L(9.5, 7, 9.5, 4.8, 2), L(9.5, 4.8, 14.5, 4.8, 2), L(14.5, 4.8, 14.5, 7, 2);
		L(6.5, 7, 7.5, 19.5, 2), L(7.5, 19.5, 16.5, 19.5, 2), L(16.5, 19.5, 17.5, 7, 2);
		break;
	case Icon::ALERT:
		arc(12, 12, 9, 2, 0, 360), L(12, 7.5, 12, 13, 2.3), circle(X(12), Y(16.5), 1.3 * k, c);
		break;
	case Icon::INFO:
		arc(12, 12, 9, 2, 0, 360), L(12, 11, 12, 16.5, 2.3), circle(X(12), Y(7.8), 1.3 * k, c);
		break;
	case Icon::SD:
		L(7, 3.5, 15.5, 3.5, 2), L(15.5, 3.5, 19, 7, 2), L(19, 7, 19, 20.5, 2), L(19, 20.5, 6, 20.5, 2);
		L(6, 20.5, 6, 3.5, 2), L(6, 3.5, 7, 3.5, 2), L(10, 7, 10, 10, 1.8), L(13, 7, 13, 10, 1.8);
		break;
	case Icon::HISTORY:
		arc(12, 12, 8, 2, 0, 360), L(12, 12, 12, 7.5, 2), L(12, 12, 15.5, 14, 2);
		break;
	case Icon::EXIT:
		arc(12, 13, 7, 2.2, -60, 240), L(12, 3.5, 12, 11, 2.2);
		break;
	case Icon::LIST:
		L(4, 7, 15, 7, 2), L(4, 12, 12, 12, 2), L(4, 17, 10, 17, 2);
		triangle(X(15), Y(12), X(15), Y(20), X(21.5), Y(16), c);
		break;
	case Icon::COMMENTS:
		L(4, 5, 20, 5, 2), L(20, 5, 20, 16, 2), L(20, 16, 9, 16, 2), L(9, 16, 4, 20, 2), L(4, 20, 4, 5, 2);
		break;
	case Icon::EXPAND:
		L(4, 9, 4, 4, 2), L(4, 4, 9, 4, 2), L(15, 4, 20, 4, 2), L(20, 4, 20, 9, 2);
		L(20, 15, 20, 20, 2), L(20, 20, 15, 20, 2), L(9, 20, 4, 20, 2), L(4, 20, 4, 15, 2);
		break;
	case Icon::LOADING:
		circle(X(6), Y(12), 2 * k, c), circle(X(12), Y(12), 2 * k, c), circle(X(18), Y(12), 2 * k, c);
		break;
	case Icon::LIKE: // thumbs up: filled cuff, outlined hand with the thumb raised
		round_rect(X(2.5), Y(10), 4 * k, 10.5 * k, 1 * k, c);
		L(9, 10.5, 12.3, 3.8, 2), L(12.3, 3.8, 14.3, 4.8, 2), L(14.3, 4.8, 13.6, 9.8, 2), L(13.6, 9.8, 19.4, 9.8, 2);
		L(19.4, 9.8, 20.8, 11.6, 2), L(20.8, 11.6, 18.9, 19.5, 2), L(18.9, 19.5, 9, 19.5, 2), L(9, 19.5, 9, 10.5, 2);
		break;
	case Icon::DISLIKE: // r16: the thumbs-up outline turned upside down (Info sheet count, display only)
		round_rect(X(2.5), Y(3.5), 4 * k, 10.5 * k, 1 * k, c);
		L(9, 13.5, 12.3, 20.2, 2), L(12.3, 20.2, 14.3, 19.2, 2), L(14.3, 19.2, 13.6, 14.2, 2), L(13.6, 14.2, 19.4, 14.2, 2);
		L(19.4, 14.2, 20.8, 12.4, 2), L(20.8, 12.4, 18.9, 4.5, 2), L(18.9, 4.5, 9, 4.5, 2), L(9, 4.5, 9, 13.5, 2);
		break;
	case Icon::HELP: // r16: ring with a drawn question mark (the player's Controls entry)
		arc(12, 12, 9, 2, 0, 360), arc(12, 9.6f, 3, 2, 180, 405);
		L(14.1f, 11.7f, 12, 13.3f, 2), L(12, 13.3f, 12, 14.2f, 2), circle(X(12), Y(17.4f), 1.35f * k, c);
		break;
	case Icon::LIKE_FILLED: { // r15: the same hand filled (liked on this console)
		static const float P[8][2] = {{9, 10.5}, {12.3, 3.8}, {14.3, 4.8}, {13.6, 9.8}, {19.4, 9.8}, {20.8, 11.6}, {18.9, 19.5}, {9, 19.5}};
		round_rect(X(2.5), Y(10), 4 * k, 10.5 * k, 1 * k, c);
		for (int i = 0; i < 8; i++) { // fan from a point every edge of the outline can see
			triangle(X(14.5), Y(14), X(P[i][0]), Y(P[i][1]), X(P[(i + 1) % 8][0]), Y(P[(i + 1) % 8][1]), c);
		}
		for (int i = 0; i < 8; i++) {
			L(P[i][0], P[i][1], P[(i + 1) % 8][0], P[(i + 1) % 8][1], 2);
		}
		break;
	}
	}
}
void wifi(float cx, float cy, int level, u32 c) {
	u32 faint = (c & 0x00FFFFFFu) | 0x55000000u;
	float by = cy + 4.5f;
	for (int i = 0; i < 3; i++) {
		float r = 3.5f + i * 3;
		ring(cx, by, r, 1.6f, level > i ? c : faint, 225, 315);
	}
	circle(cx, by - 0.5f, 1.2f, level >= 0 ? c : faint);
}
void battery(float x, float y, int percent, bool charging, u32 c) {
	u32 faint = (c & 0x00FFFFFFu) | 0x8C000000u;
	round_rect_outline(x, y, 17, 9, 2.5f, 1.2f, faint);
	rect(x + 17.5f, y + 3, 1.5f, 3, faint);
	float w = 13 * std::max(0, std::min(100, percent)) / 100.0f;
	round_rect(x + 2, y + 2, std::max(1.5f, w), 5, 1.2f, charging ? theme_cur().ok : c);
}
void logo_tile(float x, float y, float h) {
	float w = h * 16 / 11;
	round_rect(x, y, w, h, h * 3.5f / 11, theme_cur().acc);
	float nx = x + w * 6.2f / 16, ny = y + h * 2.5f / 11, nh = h * 6 / 11;
	triangle(nx, ny, nx, ny + nh, nx + w * 5 / 16, ny + nh / 2, theme_cur().acc_ink);
}
void dpad_glyph(float cx, float cy, float s, u32 c, bool up_down, bool left_right) {
	float a = s / 3;
	u32 faint = (c & 0x00FFFFFFu) | 0x5A000000u;
	rect(cx - a / 2, cy - a / 2, a, a, c);
	rect(cx - a / 2, cy - s / 2, a, a, up_down ? c : faint), rect(cx - a / 2, cy + a / 2, a, a, up_down ? c : faint);
	rect(cx - s / 2, cy - a / 2, a, a, left_right ? c : faint), rect(cx + a / 2, cy - a / 2, a, a, left_right ? c : faint);
}

float key_chip(const std::string &key, float x, float y, u32 fill, u32 ink) {
	const float h = 15;
	if (key == "dud" || key == "dlr" || key == "dpad") {
		pill(x, y, h, h, fill);
		dpad_glyph(x + h / 2, y + h / 2, 10, ink, key != "dlr", key != "dud");
		return h;
	}
	float w = std::max(h, text_w(key, T_KEY) + 6);
	pill(x, y, w, h, fill);
	text_center(key, x, x + w, text_y(y, h, T_KEY), T_KEY, ink);
	return w;
}
float hints(const std::vector<std::pair<std::string, std::string>> &items, float x, float y, u32 ink, u32 chip_fill,
            u32 chip_ink, float gap) {
	for (size_t i = 0; i < items.size(); i++) {
		const std::string &keys = items[i].first;
		size_t start = 0;
		while (start <= keys.size()) {
			size_t sp = keys.find(' ', start);
			std::string key = keys.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
			if (key == "+") {
				text(key, x, text_y(y, 15, T_META), T_META, ink);
				x += text_w(key, T_META) + 2;
			} else if (!key.empty()) {
				x += key_chip(key, x, y, chip_fill, chip_ink) + 2;
			}
			if (sp == std::string::npos) {
				break;
			}
			start = sp + 1;
		}
		x += 2;
		text(items[i].second, x, text_y(y, 15, T_META), T_META, ink);
		x += text_w(items[i].second, T_META) + (i + 1 < items.size() ? gap : 0);
	}
	return x;
}

} // namespace modern
