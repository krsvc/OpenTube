#pragma once
#include "ui/theme.hpp"

// abgr8888 color
// COLOR_GRAY(0xFF) = WHITE, COLOR_GRAY(0x00) = BLACK
#define COLOR_GRAY(x) (0xFF000000 | (x) << 16 | (x) << 8 | (x))

#define DEF_DRAW_RED 0xFF0000FF
#define DEF_DRAW_GREEN 0xFF00FF00
#define DEF_DRAW_BLUE 0xFFFF0000
#define DEF_DRAW_BLACK 0xFF000000
#define DEF_DRAW_WHITE 0xFFFFFFFF
#define DEF_DRAW_AQUA 0xFFFFFF00
#define DEF_DRAW_YELLOW 0xFF00C5FF
#define DEF_DRAW_LIGHT_GRAY 0xFFAAAAAA
#define DEF_DRAW_GRAY 0xFF777777
#define DEF_DRAW_DARK_GRAY 0xFF333333
#define DEF_DRAW_WEAK_RED 0x500000FF
#define DEF_DRAW_WEAK_ORANGE 0x500078FF
#define DEF_DRAW_WEAK_GREEN 0x5000FF00
#define DEF_DRAW_WEAK_BLUE 0x50FF0000
#define DEF_DRAW_WEAK_BLACK 0x50000000
#define DEF_DRAW_WEAK_WHITE 0x50FFFFFF
#define DEF_DRAW_WEAK_AQUA 0x50FFFF00
#define DEF_DRAW_WEAK_YELLOW 0x5000C5FF
#define DEF_DRAW_NO_COLOR 0x0

#define COLOR_LIGHT_BLUE 0xFFEEAAAA
#define COLOR_LIGHT_GREEN 0xFFAAEEAA

#define COLOR_LINK THEME_TOKEN(acc_tx)

// Pocket palette (dark = Pocket Dark, light = Pocket light). Literal values are ABGR of the #rrggbb in the comment.
#define POCKET_DARK_BACKGROUND 0xFF251E18   // #181e25
#define POCKET_DARK_SURFACE 0xFF3B3128      // #28313b
#define POCKET_DARK_PRESSED 0xFF4A3E33      // #333e4a
#define POCKET_DARK_SEPARATOR 0xFF4B4035    // #35404b
#define POCKET_DARK_TEXT 0xFFF4F1EE         // #eef1f4
#define POCKET_DARK_TEXT_STRONG2 0xFFD9D1C9 // #c9d1d9
#define POCKET_DARK_TEXT2 0xFFB1A498        // #98a4b1
#define POCKET_DARK_ACCENT 0xFF6B79EF       // #ef796b
#define POCKET_DARK_ACCENT_TINT 0xFF373447  // #473437

#define POCKET_LIGHT_BACKGROUND 0xFFF5F8F8   // #f8f8f5
#define POCKET_LIGHT_SURFACE 0xFFEAEEEC      // #eceeea
#define POCKET_LIGHT_PRESSED 0xFFDCE1DD      // #dde1dc
#define POCKET_LIGHT_SEPARATOR 0xFFD5DAD6    // #d6dad5
#define POCKET_LIGHT_TEXT 0xFF3B3225         // #25323b
#define POCKET_LIGHT_TEXT_STRONG2 0xFF5A5145 // #45515a
#define POCKET_LIGHT_TEXT2 0xFF7C746B        // #6b747c
#define POCKET_LIGHT_ACCENT 0xFF4651E3       // #e35146
#define POCKET_LIGHT_ACCENT_TINT 0xFFD5D9F6  // #f6d9d5

// OpenTube r13: every UI color below reads the CURRENT theme (ui/theme.hpp) at draw time. The literal Pocket values
// above stay as named constants (closing screen, r12d references); nothing in the UI selects them by var_night_mode.
#define POCKET_THEMED(name) (var_night_mode ? POCKET_DARK_##name : POCKET_LIGHT_##name) // legacy, unused by the UI
#define THEME_TOKEN(field) (theme_cur().field)
#define POCKET_SURFACE THEME_TOKEN(surf2)          // raised controls: chips, buttons, tracks
#define POCKET_PRESSED THEME_TOKEN(pressed)        // pressed tone of rows and controls
#define POCKET_SEPARATOR THEME_TOKEN(line)         // hairlines
#define POCKET_ACCENT THEME_TOKEN(acc)             // the one accent
#define POCKET_ACCENT_TINT THEME_TOKEN(acc_soft_on_bg) // selected row / toggled fill, opaque
#define POCKET_ON_ACCENT THEME_TOKEN(acc_ink)
#define POCKET_SCRIM 0x80000000                    // sheet backdrop, 0.5 black
#define THEME_SHEET THEME_TOKEN(surf)              // sheets, nav bar, cards

#define DEFAULT_TEXT_COLOR THEME_TOKEN(tx)
#define LIGHT0_TEXT_COLOR THEME_TOKEN(tx2)
#define LIGHT1_TEXT_COLOR THEME_TOKEN(tx3)
#define DEFAULT_BACK_COLOR THEME_TOKEN(bg)
#define LIGHT0_BACK_COLOR THEME_TOKEN(surf)
#define LIGHT1_BACK_COLOR THEME_TOKEN(pressed)
#define LIGHT2_BACK_COLOR THEME_TOKEN(line)
#define LIGHT3_BACK_COLOR THEME_TOKEN(acc)
