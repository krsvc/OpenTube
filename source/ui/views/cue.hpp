#pragma once
// OpenTube r13 cue for browse lists: D-pad up / down moves the cued row (the list scrolls to keep it visible), A opens
// it, and the top screen follows the cue. Touch goes through the rows themselves (first tap cues, second tap opens).
// Main thread only; call with the scene's list lock held, after the list got this frame's input.
#include <vector>
#include "ui/views/vertical_list.hpp"
#include "ui/views/scroll.hpp"
#include "ui/views/specialized/succinct_video.hpp"
#include "ui/views/specialized/succinct_channel.hpp"
#include "ui/shell.hpp"

namespace cue {

inline bool is_row(View *v) { return dynamic_cast<SuccinctVideoView *>(v) || dynamic_cast<SuccinctChannelView *>(v); }
inline shell::Preview preview_of(View *v) {
	if (auto *video = dynamic_cast<SuccinctVideoView *>(v)) {
		return video->preview();
	}
	if (auto *channel = dynamic_cast<SuccinctChannelView *>(v)) {
		return channel->preview();
	}
	return shell::Preview();
}
inline void open(View *v) {
	if (auto *video = dynamic_cast<SuccinctVideoView *>(v)) {
		video->open();
	} else if (auto *channel = dynamic_cast<SuccinctChannelView *>(v)) {
		channel->open();
	}
}

// y of `list` inside the scroll view's content (views stacked above it, plus the scroll margins)
inline double list_top_in(ScrollView *scroll, View *list) {
	double y = 0;
	for (auto v : scroll->views) {
		if (v == list) {
			break;
		}
		y += v->get_height() + scroll->margin;
	}
	return y;
}
// list_top: y of the list's first row inside the scroll view's content. Returns the cued row (or NULL).
inline View *update(VerticalListView *list, ScrollView *scroll, double list_top, const Hid_info &key,
                    bool allow_a = true) {
	if (!list || !scroll) {
		return NULL;
	}
	std::vector<View *> rows;
	std::vector<double> tops;
	double y = list_top;
	for (auto v : list->views) {
		if (is_row(v)) {
			rows.push_back(v);
			tops.push_back(y);
		}
		y += v->get_height() + list->margin;
	}
	if (rows.empty()) {
		shell::preview_clear();
		return NULL;
	}
	int idx = -1;
	for (size_t i = 0; i < rows.size(); i++) {
		if (rows[i] == shell::cue_owner) {
			idx = i;
		}
	}
	if (idx == -1) { // nothing of this list is cued: the first row that is (partly) visible
		idx = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			if (tops[i] + rows[i]->get_height() > scroll->get_offset()) {
				idx = i;
				break;
			}
		}
		shell::cue_set(rows[idx]);
	}
	if (key.p_d_down || key.p_d_up) {
		idx = std::max(0, std::min<int>(rows.size() - 1, idx + (key.p_d_down ? 1 : -1)));
		shell::cue_set(rows[idx]);
		double top = tops[idx], h = rows[idx]->get_height(), off = scroll->get_offset(), vis = scroll->get_height();
		if (top < off) {
			scroll->set_offset(top);
		} else if (top + h > off + vis) {
			scroll->set_offset(top + h - vis);
		}
	}
	shell::preview_set(preview_of(rows[idx]));
	if (allow_a && key.p_a) {
		open(rows[idx]);
	}
	return rows[idx];
}

} // namespace cue
