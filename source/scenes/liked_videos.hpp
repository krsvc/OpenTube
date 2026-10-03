#pragma once
#include <string>

// OpenTube r15: You > Liked videos (local favorites, data_io/liked_videos_app.hpp)
void Liked_init(void);
void Liked_exit(void);
void Liked_suspend(void);
void Liked_resume(std::string arg);
void Liked_draw(void);
