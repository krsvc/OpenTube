#pragma once
// OpenTube r15: the app's one liked videos store (data_io/liked_videos.hpp) behind a lock. Any thread may call these;
// only liked_init() (startup) and liked_save_pending() (misc-tasks thread) touch the SD card.
#include <string>
#include <vector>
#include "data_io/liked_videos.hpp"

void liked_init(); // Menu_init: bounded load of /3ds/.../liked_videos.json
bool liked_available(std::string *reason); // false: store locked (reason shown to the user)
bool liked_contains(const std::string &video_id);
// in memory + a save request; message: refusal reason (false) or what happened (true)
bool liked_set(const liked::LikedVideo &video, bool like, std::string *message);
std::vector<liked::LikedVideo> liked_snapshot(unsigned *generation);
unsigned liked_generation();
void liked_save_pending();       // misc-tasks thread: writes the newest state if it changed
std::string liked_take_error();  // a failed save (already reverted in memory), "" if none
