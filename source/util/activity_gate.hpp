#pragma once
// OpenTube r15b: one admission gate between the update installation and the two activities it must never overlap:
// video playback and the SD download library's work (transfer jobs, deletes, thumbnails, the start-up scan).
//
// Rule: an installation is reserved only while no playback and no download-library work is admitted, and neither is
// admitted while an installation is reserved. Every check-and-commit runs under this gate's own lock, so the two
// sides cannot both pass their check (no separate "is it busy?" read followed by a later start).
//
// Lock order: the gate lock is a LEAF. Callers may already hold their own lock when they enter (player
// small_resource_lock, download manager lock, updater model lock), so the order is always <caller lock> -> gate.
// While the gate lock is held nothing else runs except a caller's commit / idle predicate, which only reads or sets
// plain flags (no lock, no IO, no wait).
//
// Holders:
//  - playback: admitted at every start of a playback request, either atomically with setting the player's request
//    flag (admit_playback(commit): commit runs inside the gate lock) or, when the start needs more steps under the
//    player's own lock, by begin_playback_start() ... end_playback_start() around them (the request flag is set before
//    end_playback_start(), or the start is abandoned). Dropped only by playback_idle(still_idle) on the decode thread
//    once it is idle, no start is in progress and, read under the gate lock, no request flag is set.
//  - download library: hold_downloads() / release_downloads(), counted, held from the moment work is accepted until
//    it has ended (the manager keeps at most one hold).
//  - installation: reserve_install() from the confirmation until the system installer finished or aborted (or the
//    request was dropped without starting); release_install() once.
#include <functional>
#include <string>
#include "system/libctru_wrapper.hpp"

class ActivityGate {
  public:
	bool admit_playback(const std::function<void()> &commit) {
		lock.lock();
		bool ok = !install;
		if (ok) {
			playback = true;
			if (commit) {
				commit();
			}
		}
		lock.unlock();
		return ok;
	}
	bool begin_playback_start() {
		lock.lock();
		bool ok = !install;
		if (ok) {
			playback = true;
			starting++;
		}
		lock.unlock();
		return ok;
	}
	void end_playback_start() {
		lock.lock();
		if (starting > 0) {
			starting--;
		}
		lock.unlock();
	}
	void playback_idle(const std::function<bool()> &still_idle) {
		lock.lock();
		if (playback && starting == 0 && (!still_idle || still_idle())) {
			playback = false;
		}
		lock.unlock();
	}
	bool hold_downloads() {
		lock.lock();
		bool ok = !install;
		if (ok) {
			downloads++;
		}
		lock.unlock();
		return ok;
	}
	void release_downloads() {
		lock.lock();
		if (downloads > 0) {
			downloads--;
		}
		lock.unlock();
	}
	bool reserve_install(std::string *why) {
		lock.lock();
		std::string w = install ? "An update installation is already in progress."
		                : playback ? "Stop the video first (X + B in the player), then install."
		                : downloads ? "The download library is busy (a download, delete or thumbnail). Wait for it or "
		                              "cancel the download, then install."
		                            : "";
		if (w.empty()) {
			install = true;
		}
		lock.unlock();
		if (why) {
			*why = w;
		}
		return w.empty();
	}
	void release_install() {
		lock.lock();
		install = false;
		lock.unlock();
	}
	bool install_reserved() {
		lock.lock();
		bool r = install;
		lock.unlock();
		return r;
	}
	bool playback_admitted() {
		lock.lock();
		bool r = playback;
		lock.unlock();
		return r;
	}
	int downloads_held() {
		lock.lock();
		int r = downloads;
		lock.unlock();
		return r;
	}

  private:
	Mutex lock;
	bool install = false;
	bool playback = false;
	int starting = 0; // playback starts between begin_playback_start() and end_playback_start()
	int downloads = 0;
};

// the app's one gate (function-local static: header only, C++14)
inline ActivityGate &activity_gate() {
	static ActivityGate gate;
	return gate;
}
