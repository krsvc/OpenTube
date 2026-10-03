#include "downloads/download_options.hpp"
#include <cstring>

namespace downloads {

static const char LOCAL_URL_PREFIX[] = "ftlocal:";
static constexpr int64_t COMBINED_MAX_DURATION_MS = 60LL * 60 * 1000; // the player drops itag 18 above one hour

static FormatIdentity identity_of(const YouTubeVideoDetail::StreamFormat &format) {
	FormatIdentity res;
	res.itag = format.itag;
	res.mime = base_mime(format.mime_type);
	res.size = format.content_length > 0 && (uint64_t)format.content_length <= MAX_FILE_BYTES
	               ? (uint64_t)format.content_length
	               : 0;
	return res;
}
static bool usable(const FormatIdentity &f, const char *kind) {
	return f.itag > 0 && is_valid_mime(f.mime) && f.mime.compare(0, strlen(kind), kind) == 0;
}

std::vector<DownloadOption> download_options_for(const YouTubeVideoDetail &video, bool is_new3ds, std::string *reason) {
	std::vector<DownloadOption> res;
	if (!is_valid_video_id(video.id)) {
		*reason = "Video details are not loaded.";
		return res;
	}
	if (video.is_livestream) {
		*reason = "Livestreams cannot be downloaded.";
		return res;
	}
	if (video.is_upcoming) {
		*reason = "Upcoming videos cannot be downloaded yet.";
		return res;
	}
	if (!video.is_playable()) {
		*reason = "Not playable" + (video.playability_reason.size() ? ": " + video.playability_reason : ".");
		return res;
	}
	FormatIdentity audio = identity_of(video.audio_stream_format);
	bool audio_ok = video.audio_stream_url.size() && usable(audio, "audio/");
	for (auto &entry : video.video_stream_urls) {
		int quality = entry.first;
		auto format_it = video.video_stream_formats.find(quality);
		if (!is_supported_quality(quality) || entry.second.empty() || format_it == video.video_stream_formats.end() ||
		    !audio_ok) {
			continue;
		}
		if (!is_new3ds && quality > 240) {
			continue; // same limit as the player's quality selector on Old 3DS
		}
		DownloadOption o;
		o.quality = quality;
		o.layout = Layout::SEPARATE;
		o.video = identity_of(format_it->second);
		o.audio = audio;
		if (!usable(o.video, "video/")) {
			continue;
		}
		o.estimated_bytes = o.video.size && o.audio.size ? o.video.size + o.audio.size : 0;
		res.push_back(o);
	}
	bool has_separate_360 = false;
	for (auto &o : res) {
		has_separate_360 |= o.quality == 360;
	}
	// muxed itag 18 (360p): what the player uses when no separate 360p stream exists
	if (is_new3ds && !has_separate_360 && video.both_stream_url.size() &&
	    video.duration_ms <= COMBINED_MAX_DURATION_MS) {
		DownloadOption o;
		o.quality = 360;
		o.layout = Layout::COMBINED;
		o.video = identity_of(video.both_stream_format);
		if (usable(o.video, "video/")) {
			o.estimated_bytes = o.video.size;
			res.push_back(o);
		}
	}
	for (auto &o : res) {
		o.label = std::to_string(o.quality) + "p " +
		          (o.estimated_bytes ? "~" + format_bytes(o.estimated_bytes) : std::string("size ?"));
	}
	if (res.empty()) {
		*reason = "No downloadable H.264 + AAC format for this device.";
	}
	return res;
}

DownloadRequest download_request_for(const YouTubeVideoDetail &video, const DownloadOption &option) {
	DownloadRequest r;
	r.video_id = video.id;
	r.title = video.title;
	r.quality = option.quality;
	r.layout = option.layout;
	r.video = option.video;
	r.audio = option.audio;
	r.duration_ms = video.duration_ms > 0 ? video.duration_ms : 0;
	return r;
}

ResolveResult resolve_from_detail(const YouTubeVideoDetail &video, int quality, Layout layout) {
	ResolveResult r;
	r.is_livestream = video.is_livestream;
	r.is_upcoming = video.is_upcoming;
	r.title = video.title;
	r.duration_ms = video.duration_ms > 0 ? video.duration_ms : 0;
	if (video.error.size() && video.title.empty()) {
		r.error = "Could not load the video page. Check the connection and retry.";
		return r;
	}
	if (!video.is_playable()) {
		r.error = "Not playable now" + (video.playability_reason.size() ? ": " + video.playability_reason : ".");
		return r;
	}
	if (layout == Layout::COMBINED) {
		if (video.both_stream_url.empty() || quality != 360) {
			r.error = "The selected format is no longer offered.";
			return r;
		}
		r.video.url = video.both_stream_url;
		r.video.format = identity_of(video.both_stream_format);
	} else {
		auto url_it = video.video_stream_urls.find(quality);
		auto format_it = video.video_stream_formats.find(quality);
		if (url_it == video.video_stream_urls.end() || url_it->second.empty() ||
		    format_it == video.video_stream_formats.end() || video.audio_stream_url.empty()) {
			r.error = "The selected format is no longer offered.";
			return r;
		}
		r.video.url = url_it->second;
		r.video.format = identity_of(format_it->second);
		r.audio.url = video.audio_stream_url;
		r.audio.format = identity_of(video.audio_stream_format);
	}
	r.ok = true;
	return r;
}

std::string local_url_for(const std::string &item_id) { return LOCAL_URL_PREFIX + item_id; }
bool parse_local_url(const std::string &url, std::string *item_id) {
	if (url.compare(0, sizeof(LOCAL_URL_PREFIX) - 1, LOCAL_URL_PREFIX) != 0) {
		return false;
	}
	std::string id = url.substr(sizeof(LOCAL_URL_PREFIX) - 1);
	if (!parse_item_id(id, NULL, NULL)) {
		return false;
	}
	if (item_id) {
		*item_id = id;
	}
	return true;
}

} // namespace downloads
