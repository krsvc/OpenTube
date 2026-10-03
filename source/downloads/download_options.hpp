#pragma once
// Which qualities of a loaded video page can be downloaded, and the mapping of a (re-)resolved page onto the
// selected representation. Pure functions over YouTubeVideoDetail (host-tested): only formats the player itself can
// play are offered (H.264 video + AAC audio as separate files, or the muxed itag 18 file), never placeholders.
#include <string>
#include <vector>
#include "youtube_parser/parser.hpp"
#include "downloads/download_manager.hpp"

namespace downloads {

struct DownloadOption {
	int quality = 0;
	Layout layout = Layout::SEPARATE;
	FormatIdentity video;
	FormatIdentity audio;
	uint64_t estimated_bytes = 0; // 0 = unknown (never guessed)
	std::string label;            // "360p · 12.3 MB" / "360p · size unknown"
};

// empty result: *reason says why nothing can be downloaded
std::vector<DownloadOption> download_options_for(const YouTubeVideoDetail &video, bool is_new3ds, std::string *reason);
DownloadRequest download_request_for(const YouTubeVideoDetail &video, const DownloadOption &option);
ResolveResult resolve_from_detail(const YouTubeVideoDetail &video, int quality, Layout layout);

// offline player url: "ftlocal:<item id>" (never a file path)
std::string local_url_for(const std::string &item_id);
bool parse_local_url(const std::string &url, std::string *item_id);

} // namespace downloads
