// Host stand-in: the test records every call (tests/host/test_pocket_ui.cpp). Signatures match source/ui/draw/draw.hpp.
#pragma once
#include <string>
#include "types.hpp"
void Draw(std::string text, float x, float y, float text_size_x, float text_size_y, int abgr8888);
float Draw_get_width(const std::string &text, float text_size_x);
float Draw_get_height(const std::string &text, float text_size_y);
void Draw_x_centered(std::string text, float x0, float x1, float y, float text_size_x, float text_size_y, int abgr8888);
void Draw_right(std::string text, float x1, float y, float text_size_x, float text_size_y, int abgr8888);
void Draw_texture(C2D_Image image, float x, float y, float x_size, float y_size);
void Draw_texture(C2D_Image image, int abgr8888, float x, float y, float x_size, float y_size);
void Draw_line(float x_0, float y_0, int abgr8888_0, float x_1, float y_1, int abgr8888_1, float width);
