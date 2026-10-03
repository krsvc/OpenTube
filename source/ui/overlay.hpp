#pragma once
#include "types.hpp"
#include "scene_switcher.hpp"

#define OVERLAY_MENU_ICON_SIZE 29

void draw_overlay_menu(int y);
// returns true when the shell took this frame's whole input (shell::input_taken()): cancel the scene's gestures
bool update_overlay_menu(Hid_info *key);
void close_overlay_menu();
void overlay_menu_on_resume();
