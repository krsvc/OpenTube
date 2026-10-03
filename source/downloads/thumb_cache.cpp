#include "downloads/thumb_cache.hpp"
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>

namespace downloads {

const char *const THUMB_OWNER_FILE = "owner.txt";
const char *const THUMB_OWNER_TEXT = "OTTC1\nOpenTube download thumbnail cache (created by the app)\n";

bool jpeg_dimensions(const std::string &d, int *width, int *height) {
	if (d.size() < 4 || (uint8_t)d[0] != 0xFF || (uint8_t)d[1] != 0xD8) {
		return false;
	}
	size_t p = 2;
	while (p + 4 <= d.size()) {
		if ((uint8_t)d[p] != 0xFF) {
			return false;
		}
		uint8_t m = d[p + 1];
		if (m == 0xFF) { // fill byte
			p++;
			continue;
		}
		if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { // markers without a length
			p += 2;
			continue;
		}
		if (m == 0xD9 || m == 0xDA) { // end / scan data before any frame header
			return false;
		}
		size_t len = (uint8_t)d[p + 2] << 8 | (uint8_t)d[p + 3];
		if (len < 2 || p + 2 + len > d.size()) {
			return false;
		}
		if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { // SOFn
			if (len < 8) {
				return false;
			}
			*height = (uint8_t)d[p + 5] << 8 | (uint8_t)d[p + 6];
			*width = (uint8_t)d[p + 7] << 8 | (uint8_t)d[p + 8];
			return *width > 0 && *height > 0;
		}
		p += 2 + len;
	}
	return false;
}
bool valid_thumbnail(const std::string &data, std::string *why) {
	int w = 0, h = 0;
	if (data.empty() || data.size() > MAX_THUMB_BYTES) {
		*why = "size " + std::to_string(data.size()) + " bytes";
		return false;
	}
	if (!jpeg_dimensions(data, &w, &h)) {
		*why = "not a JPEG image";
		return false;
	}
	if (w > MAX_THUMB_W || h > MAX_THUMB_H) {
		*why = "image " + std::to_string(w) + "x" + std::to_string(h) + " is too large";
		return false;
	}
	if ((uint8_t)data[data.size() - 2] != 0xFF || (uint8_t)data[data.size() - 1] != 0xD9) {
		*why = "incomplete image";
		return false;
	}
	return true;
}
std::string thumb_file_name(const std::string &item_id, bool tmp) {
	std::string video_id;
	if (!parse_item_id(item_id, &video_id, NULL)) {
		return "";
	}
	unsigned mask = 0;
	for (size_t i = 0; i < video_id.size(); i++) {
		mask |= (video_id[i] >= 'A' && video_id[i] <= 'Z') ? 1u << i : 0;
	}
	char buf[8];
	snprintf(buf, sizeof(buf), ".%03x", mask);
	return item_id + buf + (tmp ? ".tmp" : ".jpg");
}
bool read_thumbnail_file(const std::string &path, std::string *out, std::string *why) {
	struct stat st;
#ifdef __3DS__
	int r = stat(path.c_str(), &st); // no symlinks on the SD card file system (same as posix_fs)
#else
	int r = lstat(path.c_str(), &st);
#endif
	if (r != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > MAX_THUMB_BYTES) {
		*why = "no thumbnail file";
		return false;
	}
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		*why = "cannot open the thumbnail";
		return false;
	}
	std::string data(MAX_THUMB_BYTES + 1, '\0');
	size_t n = fread(&data[0], 1, data.size(), fp);
	bool ok = !ferror(fp);
	fclose(fp);
	data.resize(n);
	if (!ok || !valid_thumbnail(data, why)) {
		*why = ok ? *why : "read error";
		return false;
	}
	*out = data;
	return true;
}

ThumbCache::ThumbCache(DlFs &fs, const std::string &dir_) : fs(fs), dir(dir_) {
	if (dir.size() && dir.back() != '/') {
		dir += '/';
	}
}

bool ThumbCache::write_durable(const std::string &path, const std::string &data, std::string *err) {
	DlFile *f = fs.open_truncate(path);
	if (!f) {
		*err = "open failed (errno " + std::to_string(fs.last_errno()) + ")";
		return false;
	}
	bool ok = f->write(data.data(), data.size()) == data.size();
	ok = ok && f->sync();
	int e = f->error();
	bool closed = f->close();
	if (!closed && ok) {
		e = f->error();
	}
	delete f;
	if (!ok || !closed) {
		*err = "write/sync/close failed (errno " + std::to_string(e) + ")";
		return false;
	}
	return true;
}

bool ThumbCache::owned(bool create, std::string *why) {
	if (dir.empty()) {
		*why = "thumbnails are off";
		return false;
	}
	if (refused) {
		*why = "the thumbnail folder is not this app's";
		return false;
	}
	auto refuse = [&](const std::string &reason) {
		refused = true;
		verified = false;
		*why = reason + "; thumbnails are off for this run, nothing in it is changed";
		return false;
	};
	std::string folder = dir.substr(0, dir.size() - 1), marker = dir + THUMB_OWNER_FILE;
	DlStat st = fs.stat_path(folder);
	if (!st.exists && !st.is_link) {
		if (!create) {
			*why = "no thumbnail folder yet";
			return false;
		}
		DlStat parent = fs.stat_path(folder.substr(0, folder.rfind('/')));
		if (!parent.is_dir || parent.is_link) {
			*why = "the app data folder is missing";
			return false;
		}
		int r = fs.make_dir(folder);
		if (r == EEXIST) { // it appeared since the stat, so this app did not create it
			return refuse("the thumbnail folder already exists");
		}
		if (r != 0) {
			*why = "cannot create the thumbnail folder (errno " + std::to_string(r) + ")";
			return false;
		}
		st = fs.stat_path(folder);
		if (!st.is_dir || st.is_link) {
			return refuse("the thumbnail path is not a real folder");
		}
		std::string err;
		if (!write_durable(marker, THUMB_OWNER_TEXT, &err)) {
			// undo this claim: only the marker this call wrote and the (then empty) folder it created
			fs.remove_file(marker);
			fs.remove_dir(folder);
			*why = "cannot mark the thumbnail folder: " + err;
			return false;
		}
		verified = true;
		return true;
	}
	if (!st.is_dir || st.is_link) {
		return refuse("the thumbnail path is not a real folder");
	}
	DlStat m = fs.stat_path(marker);
	std::string text;
	std::string want = THUMB_OWNER_TEXT;
	if (!m.is_file || m.is_link || m.size != want.size() || !fs.read_small(marker, want.size(), &text) || text != want) {
		return refuse("the thumbnail folder was not created by this app");
	}
	verified = true;
	return true;
}

bool ThumbCache::present(const std::string &item_id) {
	std::string why, name = thumb_file_name(item_id);
	if (name.empty() || !enabled() || (!verified && !owned(false, &why))) {
		return false;
	}
	DlStat st = fs.stat_path(dir + name);
	return st.exists && st.is_file && !st.is_link && st.size > 0 && st.size <= MAX_THUMB_BYTES;
}
bool ThumbCache::read(const std::string &item_id, std::string *out, std::string *why) {
	if (!present(item_id)) {
		*why = "no thumbnail file";
		return false;
	}
	std::string data;
	if (!fs.read_small(path(item_id), MAX_THUMB_BYTES, &data)) {
		*why = "cannot read the thumbnail";
		return false;
	}
	if (!valid_thumbnail(data, why)) {
		return false;
	}
	*out = data;
	return true;
}
bool ThumbCache::write(const std::string &item_id, const std::string &jpeg, std::string *err) {
	std::string why, name = thumb_file_name(item_id);
	if (name.empty()) {
		*err = "invalid item id";
		return false;
	}
	if (!valid_thumbnail(jpeg, &why)) {
		*err = "invalid thumbnail: " + why;
		return false;
	}
	if (!owned(true, &why)) { // re-proven on the card right before every write
		*err = why;
		return false;
	}
	std::string tmp = dir + thumb_file_name(item_id, true), main = dir + name;
	for (const std::string &p : {tmp, main}) {
		DlStat st = fs.stat_path(p);
		if ((st.exists || st.is_link) && (!st.is_file || st.is_link)) {
			*err = "unexpected non-file entry " + p.substr(dir.size()) + " left in place";
			return false;
		}
	}
	if (!fs.stat_path(main).exists) {
		std::vector<std::string> names;
		if (!fs.list_dir(dir, &names)) {
			*err = "cannot list the thumbnail folder";
			return false;
		}
		if (names.size() >= MAX_THUMB_ENTRIES) {
			*err = "the thumbnail folder is full";
			return false;
		}
	}
	if (!write_durable(tmp, jpeg, err)) {
		fs.remove_file(tmp); // this app's own name in its own folder, a regular file or absent (checked above)
		*err = "thumbnail " + *err;
		return false;
	}
	int r = fs.remove_file(main);
	if (r != 0 && r != ENOENT) {
		fs.remove_file(tmp);
		*err = "cannot replace the thumbnail (errno " + std::to_string(r) + ")";
		return false;
	}
	r = fs.rename_file(tmp, main);
	if (r != 0) {
		fs.remove_file(tmp);
		*err = "cannot publish the thumbnail (errno " + std::to_string(r) + ")";
		return false;
	}
	return true;
}
bool ThumbCache::remove(const std::string &item_id, std::string *err) {
	std::string why, name = thumb_file_name(item_id);
	if (name.empty()) {
		*err = "invalid item id";
		return false;
	}
	if (!owned(false, &why)) { // re-proven on the card; nothing is removed from a folder that is not this app's
		*err = why;
		return false;
	}
	bool ok = true;
	for (const std::string &p : {dir + thumb_file_name(item_id, true), dir + name}) {
		DlStat st = fs.stat_path(p);
		if (!st.exists && !st.is_link) {
			continue;
		}
		if (!st.is_file || st.is_link) {
			*err = "unexpected non-file entry " + p.substr(dir.size()) + " left in place";
			ok = false;
			continue;
		}
		int r = fs.remove_file(p);
		if (r != 0 && r != ENOENT) {
			*err = "cannot remove " + p.substr(dir.size()) + " (errno " + std::to_string(r) + ")";
			ok = false;
		}
	}
	return ok;
}

} // namespace downloads
