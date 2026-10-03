#pragma once
// OpenTube r15 updater, 3DS side (OPENTUBE_R15_PLAN.md B): one worker thread for the whole session runs the
// update_core procedures; the UI (Settings > Update) only reads snapshots and posts confirmed requests.
// Lifecycle: Updater_start() in Menu_init; Menu_exit_request() -> Updater_request_stop() (atomic with the worker's job
// claim: nothing is claimed afterwards and an unclaimed INSTALL is dropped; a check / download stops at its next abort
// poll; an installation whose system installer already started runs to its end); Menu_exit_pending() waits for
// Updater_finished() (the worker thread's kernel handle signalled); Menu_exit() -> Updater_stop() before services end.
#include <string>
#include "updater/update_core.hpp"

void Updater_start();
void Updater_request_stop();
bool Updater_finished(); // true once the worker thread has really ended (or never started)
bool Updater_stop();     // joins the worker; false if it is still live after the bound (resources are then kept)
void Updater_idle_tick(); // main loop: the once-per-session automatic check, and the You badge

struct UpdaterView {
	updater::State state = updater::State::IDLE;
	std::string message;
	updater::Release release;
	updater::Staged staged;
	uint64_t done = 0, total = 0;
	bool is_3dsx = false;
};
UpdaterView Updater_snapshot();
std::string Updater_current_version(); // "0.15.0"
bool Updater_check(std::string *msg);
bool Updater_download(const updater::Release &shown, std::string *msg);
bool Updater_install(const std::string &shown_sha256, std::string *msg);
bool Updater_cancel(std::string *msg);
bool Updater_install_running();
// advisory, for the confirmation dialog: why an installation would be refused now ("" = it would not). The admission
// itself is Updater_install()'s activity-gate reservation (util/activity_gate.hpp).
std::string Updater_install_blocker();
