#include "data_io/liked_videos.hpp"
#include <cerrno>
#include <cstdio>
#include <set>
#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

namespace liked {

const char *const FILE_MAIN = "liked_videos.json";
const char *const FILE_TMP = "liked_videos.tmp";
const char *const FORMAT_NAME = "opentube.liked_videos";

static std::string bounded_text(const std::string &text, size_t max_bytes) {
	std::string res;
	for (char c : text) {
		unsigned char u = (unsigned char)c;
		res.push_back(u < 0x20 || u == 0x7F ? ' ' : c);
	}
	if (res.size() > max_bytes) {
		size_t cut = max_bytes;
		while (cut > 0 && ((unsigned char)res[cut] & 0xC0) == 0x80) { // never split a UTF-8 sequence
			cut--;
		}
		res.resize(cut);
	}
	return res;
}

bool make_entry(const std::string &id, const std::string &title, const std::string &channel, int64_t duration_ms,
                int64_t liked_at, LikedVideo *out) {
	if (!downloads::is_valid_video_id(id)) {
		return false;
	}
	LikedVideo v;
	v.id = id;
	v.title = bounded_text(title, MAX_TITLE_BYTES);
	v.channel = bounded_text(channel, MAX_CHANNEL_BYTES);
	v.duration_ms = duration_ms > 0 && duration_ms <= MAX_DURATION_MS ? duration_ms : 0;
	v.liked_at = liked_at > 0 ? liked_at : 0;
	if (out) {
		*out = v;
	}
	return true;
}

static bool entry_valid(const LikedVideo &v) {
	return downloads::is_valid_video_id(v.id) && v.title.size() <= MAX_TITLE_BYTES &&
	       v.channel.size() <= MAX_CHANNEL_BYTES && bounded_text(v.title, MAX_TITLE_BYTES) == v.title &&
	       bounded_text(v.channel, MAX_CHANNEL_BYTES) == v.channel && v.duration_ms >= 0 &&
	       v.duration_ms <= MAX_DURATION_MS && v.liked_at >= 0;
}

std::string serialize(const std::vector<LikedVideo> &items) {
	rapidjson::StringBuffer buf;
	rapidjson::Writer<rapidjson::StringBuffer> w(buf);
	w.StartObject();
	w.Key("format");
	w.String(FORMAT_NAME);
	w.Key("version");
	w.Int(FORMAT_VERSION);
	w.Key("items");
	w.StartArray();
	for (auto &v : items) {
		w.StartObject();
		w.Key("id");
		w.String(v.id);
		w.Key("title");
		w.String(v.title);
		w.Key("channel");
		w.String(v.channel);
		w.Key("duration_ms");
		w.Int64(v.duration_ms);
		w.Key("liked_at");
		w.Int64(v.liked_at);
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	return std::string(buf.GetString(), buf.GetSize()) + "\n";
}

// exactly these members, each once (a duplicate name leaves another one missing)
static bool has_exact_members(const rapidjson::Value &obj, const std::vector<const char *> &names) {
	if (!obj.IsObject() || obj.MemberCount() != names.size()) {
		return false;
	}
	for (const char *n : names) {
		if (!obj.HasMember(n)) {
			return false;
		}
	}
	return true;
}

bool parse(const std::string &text, std::vector<LikedVideo> *out, std::string *why) {
	auto fail = [&](const std::string &reason) {
		if (why) {
			*why = reason;
		}
		return false;
	};
	if (text.size() > MAX_FILE_BYTES) {
		return fail("file too large");
	}
	if (text.find('\0') != std::string::npos) {
		return fail("not a text file");
	}
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseValidateEncodingFlag>(text.c_str(), text.size());
	if (doc.HasParseError()) {
		return fail("not valid JSON (error " + std::to_string((int)doc.GetParseError()) + " at " +
		            std::to_string(doc.GetErrorOffset()) + ")");
	}
	if (!has_exact_members(doc, {"format", "version", "items"})) {
		return fail("not an OpenTube liked videos file");
	}
	if (!doc["format"].IsString() || std::string(doc["format"].GetString()) != FORMAT_NAME) {
		return fail("not an OpenTube liked videos file");
	}
	if (!doc["version"].IsInt() || doc["version"].GetInt() != FORMAT_VERSION) {
		return fail("unsupported version");
	}
	const rapidjson::Value &arr = doc["items"];
	if (!arr.IsArray()) {
		return fail("items is not a list");
	}
	if (arr.Size() > MAX_ITEMS) {
		return fail("more than " + std::to_string(MAX_ITEMS) + " items");
	}
	std::vector<LikedVideo> res;
	std::set<std::string> ids;
	for (rapidjson::SizeType i = 0; i < arr.Size(); i++) {
		const rapidjson::Value &e = arr[i];
		std::string pos = "item " + std::to_string(i + 1);
		if (!has_exact_members(e, {"id", "title", "channel", "duration_ms", "liked_at"}) || !e["id"].IsString() ||
		    !e["title"].IsString() || !e["channel"].IsString() || !e["duration_ms"].IsInt64() ||
		    !e["liked_at"].IsInt64()) {
			return fail(pos + " is malformed");
		}
		LikedVideo v;
		v.id.assign(e["id"].GetString(), e["id"].GetStringLength());
		v.title.assign(e["title"].GetString(), e["title"].GetStringLength());
		v.channel.assign(e["channel"].GetString(), e["channel"].GetStringLength());
		v.duration_ms = e["duration_ms"].GetInt64();
		v.liked_at = e["liked_at"].GetInt64();
		if (!entry_valid(v)) {
			return fail(pos + " has an invalid value");
		}
		if (!ids.insert(v.id).second) {
			return fail(pos + " repeats a video");
		}
		res.push_back(v);
	}
	if (out) {
		*out = res;
	}
	return true;
}

std::string duration_text(int64_t duration_ms) {
	if (duration_ms <= 0 || duration_ms > MAX_DURATION_MS) {
		return "";
	}
	int64_t t = duration_ms / 1000;
	char buf[32];
	if (t >= 3600) {
		snprintf(buf, sizeof(buf), "%d:%02d:%02d", (int)(t / 3600), (int)(t / 60 % 60), (int)(t % 60));
	} else {
		snprintf(buf, sizeof(buf), "%d:%02d", (int)(t / 60), (int)(t % 60));
	}
	return buf;
}
OpenRoute open_route(bool online, bool has_ready_copy) {
	if (online) {
		return OpenRoute::ONLINE;
	}
	return has_ready_copy ? OpenRoute::LOCAL : OpenRoute::UNAVAILABLE;
}

LikedStore::LikedStore(downloads::DlFs &fs, const std::string &dir_) : fs(fs), dir(dir_) {
	if (dir.empty() || dir.back() != '/') {
		dir += '/';
	}
}

void LikedStore::load() {
	list.clear();
	saved.clear();
	problem.clear();
	gen = saved_gen = 0;
	auto lock_with = [&](const std::string &reason) {
		load_state = LoadState::LOCKED;
		problem = reason;
	};
	downloads::DlStat st = fs.stat_path(path(FILE_MAIN));
	if (st.exists) {
		std::string text, why;
		if (!st.is_file || st.is_link) {
			return lock_with(std::string(FILE_MAIN) + " is not a regular file");
		}
		if (st.size > MAX_FILE_BYTES) {
			return lock_with(std::string(FILE_MAIN) + " is too large");
		}
		if (!fs.read_small(path(FILE_MAIN), MAX_FILE_BYTES, &text)) {
			return lock_with(std::string(FILE_MAIN) + " cannot be read (errno " + std::to_string(fs.last_errno()) +
			                 ")");
		}
		if (!parse(text, &list, &why)) {
			list.clear();
			return lock_with(std::string(FILE_MAIN) + ": " + why);
		}
		saved = list;
		load_state = LoadState::LOADED;
		return;
	}
	// no main file: a valid tmp is a finished save whose rename was interrupted
	downloads::DlStat tst = fs.stat_path(path(FILE_TMP));
	std::string text;
	if (tst.exists && tst.is_file && !tst.is_link && tst.size <= MAX_FILE_BYTES &&
	    fs.read_small(path(FILE_TMP), MAX_FILE_BYTES, &text) && parse(text, &list, NULL)) {
		saved = list;
		load_state = LoadState::RECOVERED;
		return;
	}
	list.clear();
	load_state = LoadState::EMPTY;
}

bool LikedStore::contains(const std::string &id) const {
	for (auto &v : list) {
		if (v.id == id) {
			return true;
		}
	}
	return false;
}

bool LikedStore::add(const LikedVideo &video, std::string *message) {
	if (locked()) {
		*message = "Liked videos are unavailable: " + (problem.empty() ? std::string("not loaded") : problem);
		return false;
	}
	if (!entry_valid(video)) {
		*message = "This video cannot be saved (invalid video id).";
		return false;
	}
	if (contains(video.id)) {
		return true;
	}
	if (list.size() >= MAX_ITEMS) {
		*message = "Liked videos is full (" + std::to_string(MAX_ITEMS) + "). Remove some first.";
		return false;
	}
	list.insert(list.begin(), video);
	gen++;
	return true;
}

bool LikedStore::remove(const std::string &id, std::string *message) {
	if (locked()) {
		*message = "Liked videos are unavailable: " + (problem.empty() ? std::string("not loaded") : problem);
		return false;
	}
	for (size_t i = 0; i < list.size(); i++) {
		if (list[i].id == id) {
			list.erase(list.begin() + i);
			gen++;
			return true;
		}
	}
	return true;
}

bool LikedStore::save_begin(std::string *text, unsigned *snapshot_gen) {
	if (locked() || save_running || gen == saved_gen) {
		return false;
	}
	saving = list;
	saving_gen = gen;
	save_running = true;
	*text = serialize(saving);
	*snapshot_gen = saving_gen;
	return true;
}

bool LikedStore::save_write(const std::string &text, std::string *error) const {
	std::string why;
	if (!parse(text, NULL, &why)) { // never persist something the loader would lock on
		*error = "refusing to write an invalid list: " + why;
		return false;
	}
	std::string root = dir.substr(0, dir.size() - 1);
	downloads::DlStat rst = fs.stat_path(root);
	if (!rst.exists) {
		int r = fs.make_dir(root);
		if (r != 0 && r != EEXIST) {
			*error = "cannot create the data folder (errno " + std::to_string(r) + ")";
			return false;
		}
	} else if (!rst.is_dir || rst.is_link) {
		*error = "the data folder is not a folder";
		return false;
	}
	downloads::DlStat mst = fs.stat_path(path(FILE_MAIN));
	if (mst.exists && (!mst.is_file || mst.is_link)) {
		*error = std::string(FILE_MAIN) + " is not a regular file";
		return false;
	}
	if (mst.exists) { // replaced only while it still is this app's list (it may have been swapped on the card)
		std::string current;
		if (mst.size > MAX_FILE_BYTES || !fs.read_small(path(FILE_MAIN), MAX_FILE_BYTES, &current) ||
		    !parse(current, NULL, NULL)) {
			*error = std::string(FILE_MAIN) + " on the SD card is no longer a valid list; it was left untouched";
			return false;
		}
	}
	downloads::DlStat tst = fs.stat_path(path(FILE_TMP));
	if (tst.exists && (!tst.is_file || tst.is_link)) {
		*error = std::string(FILE_TMP) + " is not a regular file";
		return false;
	}
	downloads::DlFile *f = fs.open_truncate(path(FILE_TMP));
	if (!f) {
		*error = "cannot open the file (errno " + std::to_string(fs.last_errno()) + ")";
		return false;
	}
	bool ok = f->write(text.data(), text.size()) == text.size();
	ok = ok && f->sync();
	int e = f->error();
	bool closed = f->close();
	if (!closed && ok) {
		e = f->error();
	}
	delete f;
	if (!ok || !closed) {
		*error = "write failed (errno " + std::to_string(e) + (e == ENOSPC ? ", SD card full" : "") + ")";
		return false;
	}
	int r = fs.remove_file(path(FILE_MAIN));
	if (r != 0 && r != ENOENT) {
		*error = "cannot replace the old list (errno " + std::to_string(r) + ")";
		return false;
	}
	r = fs.rename_file(path(FILE_TMP), path(FILE_MAIN));
	if (r != 0) {
		*error = "cannot publish the new list (errno " + std::to_string(r) + ")";
		return false;
	}
	return true;
}

void LikedStore::save_end(unsigned snapshot_gen, bool ok, const std::string &error) {
	if (!save_running || snapshot_gen != saving_gen) {
		return;
	}
	save_running = false;
	if (ok) {
		saved = saving;
		saved_gen = saving_gen;
		return;
	}
	// not on the card: show what is (later edits were made on top of the failed state and go with it)
	list = saved;
	gen++;
	saved_gen = gen;
	last_error = error;
}

std::string LikedStore::take_error() {
	std::string e = last_error;
	last_error.clear();
	return e;
}

} // namespace liked
