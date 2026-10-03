#pragma once

enum class SceneType {
	VIDEO_PLAYER,
	SEARCH,
	SETTINGS,
	CHANNEL,
	ABOUT,
	HISTORY,
	HOME,
	DOWNLOADS,
	LIKED, // OpenTube r15: You > Liked videos
	// used for intent
	NO_CHANGE,
	BACK,
	EXIT
};
struct Intent {
	SceneType next_scene = SceneType::NO_CHANGE;
	std::string arg = "";

	bool operator==(const Intent &rhs) { return next_scene == rhs.next_scene && arg == rhs.arg; }
	bool operator!=(const Intent &rhs) { return !(*this == rhs); }
};

extern SceneType global_current_scene;
extern Intent global_intent;

void Menu_init(void);

// exit request -> wait -> commit (main.cpp): Menu_exit() tears services down and main() returns (the runtime then
// unmaps the heap) only after every download / stream resolution owner has really ended
void Menu_exit_request(void);
bool Menu_exit_ready(void);
void Menu_stopping(void); // one frame of the visible stopping state; keeps APT (HOME / sleep) serviced
void Menu_exit(void);

bool Menu_main(void);

void Menu_get_system_info(void);

int Menu_check_free_ram(void);
