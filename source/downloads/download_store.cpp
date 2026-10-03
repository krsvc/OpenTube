#include "downloads/download_store.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

namespace downloads {

const char *const FILE_VIDEO = "video.mp4";
const char *const FILE_AUDIO = "audio.m4a";
const char *const FILE_COMBINED = "media.mp4";
const char *const FILE_META = "meta.txt";
const char *const FILE_META_TMP = "meta.tmp";

static const char META_MAGIC[] = "FTDL1";
static const int SUPPORTED_QUALITIES[] = {144, 240, 360, 480};

// ------------------------------------------------------------------ ids / values
bool is_valid_video_id(const std::string &video_id) {
	if (video_id.size() != 11) {
		return false;
	}
	for (char c : video_id) {
		if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) {
			return false;
		}
	}
	return true;
}
bool is_supported_quality(int quality) {
	for (int q : SUPPORTED_QUALITIES) {
		if (q == quality) {
			return true;
		}
	}
	return false;
}
std::string make_item_id(const std::string &video_id, int quality) {
	if (!is_valid_video_id(video_id) || !is_supported_quality(quality)) {
		return "";
	}
	return video_id + "-" + std::to_string(quality) + "p";
}
bool parse_item_id(const std::string &id, std::string *video_id, int *quality) {
	// "<11 chars>-<digits>p", digits one of the supported qualities
	if (id.size() < 11 + 1 + 3 + 1 || id.size() > 11 + 1 + 3 + 1 || id[11] != '-' || id.back() != 'p') {
		return false;
	}
	std::string vid = id.substr(0, 11);
	std::string digits = id.substr(12, id.size() - 13);
	for (char c : digits) {
		if (c < '0' || c > '9') {
			return false;
		}
	}
	int q = atoi(digits.c_str());
	if (make_item_id(vid, q) != id) {
		return false;
	}
	if (video_id) {
		*video_id = vid;
	}
	if (quality) {
		*quality = q;
	}
	return true;
}
bool is_valid_mime(const std::string &mime) {
	if (mime.empty() || mime.size() > 40) {
		return false;
	}
	size_t slash = mime.find('/');
	if (slash == std::string::npos || slash == 0 || slash + 1 == mime.size() || mime.find('/', slash + 1) != std::string::npos) {
		return false;
	}
	for (char c : mime) {
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '+' || c == '-')) {
			return false;
		}
	}
	return true;
}
std::string base_mime(const std::string &mime_type) {
	std::string res = mime_type.substr(0, mime_type.find(';'));
	while (res.size() && res.back() == ' ') {
		res.pop_back();
	}
	for (auto &c : res) {
		c = tolower((unsigned char)c);
	}
	return is_valid_mime(res) ? res : "";
}
std::string sanitize_title(const std::string &title) {
	std::string res;
	for (char c : title) {
		unsigned char u = (unsigned char)c;
		res.push_back(u < 0x20 || u == 0x7F ? ' ' : c);
	}
	if (res.size() > MAX_TITLE_BYTES) {
		size_t cut = MAX_TITLE_BYTES;
		while (cut > 0 && ((unsigned char)res[cut] & 0xC0) == 0x80) { // never split a UTF-8 sequence
			cut--;
		}
		res.resize(cut);
	}
	return res;
}
const char *state_name(PersistedState state) {
	switch (state) {
	case PersistedState::DOWNLOADING:
		return "downloading";
	case PersistedState::READY:
		return "ready";
	case PersistedState::FAILED:
		return "failed";
	case PersistedState::CANCELLED:
		return "cancelled";
	}
	return "?";
}
std::vector<const char *> media_files(Layout layout) {
	if (layout == Layout::COMBINED) {
		return {FILE_COMBINED};
	}
	return {FILE_VIDEO, FILE_AUDIO};
}

// ------------------------------------------------------------------ metadata
static uint32_t crc32_of(const std::string &data) {
	uint32_t crc = 0xFFFFFFFFu;
	for (unsigned char c : data) {
		crc ^= c;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}
static std::string hex8(uint32_t v) {
	char buf[9];
	snprintf(buf, sizeof(buf), "%08x", (unsigned)v);
	return buf;
}
std::string serialize_meta(const ItemMeta &meta) {
	std::string body = std::string(META_MAGIC) + "\n";
	auto add = [&](const char *key, const std::string &value) { body += std::string(key) + "=" + value + "\n"; };
	add("id", meta.id);
	add("video_id", meta.video_id);
	add("title", sanitize_title(meta.title));
	add("quality", std::to_string(meta.quality));
	add("layout", meta.layout == Layout::COMBINED ? "combined" : "separate");
	add("v_itag", std::to_string(meta.video.itag));
	add("v_mime", meta.video.mime);
	add("v_size", std::to_string(meta.video.size));
	if (meta.layout == Layout::SEPARATE) {
		add("a_itag", std::to_string(meta.audio.itag));
		add("a_mime", meta.audio.mime);
		add("a_size", std::to_string(meta.audio.size));
	}
	add("duration_ms", std::to_string(meta.duration_ms));
	add("state", state_name(meta.state));
	return body + "end=" + hex8(crc32_of(body)) + "\n";
}
static bool parse_u64(const std::string &s, uint64_t max, uint64_t *out) {
	if (s.empty() || s.size() > 19) {
		return false;
	}
	uint64_t v = 0;
	for (char c : s) {
		if (c < '0' || c > '9') {
			return false;
		}
		v = v * 10 + (c - '0');
	}
	if (v > max) {
		return false;
	}
	*out = v;
	return true;
}
bool parse_meta(const std::string &text, ItemMeta *out, std::string *why) {
	auto fail = [&](const std::string &reason) {
		if (why) {
			*why = reason;
		}
		return false;
	};
	if (text.size() > MAX_META_BYTES) {
		return fail("metadata too large");
	}
	size_t end_pos = text.rfind("end=");
	if (end_pos == std::string::npos || (end_pos != 0 && text[end_pos - 1] != '\n')) {
		return fail("metadata incomplete (no end marker)");
	}
	std::string body = text.substr(0, end_pos);
	if (text.substr(end_pos) != "end=" + hex8(crc32_of(body)) + "\n") {
		return fail("metadata checksum mismatch or trailing data");
	}
	if (body.compare(0, sizeof(META_MAGIC), std::string(META_MAGIC) + "\n") != 0) {
		return fail("unknown metadata format");
	}
	std::vector<std::pair<std::string, std::string>> fields;
	for (size_t pos = sizeof(META_MAGIC); pos < body.size();) {
		size_t nl = body.find('\n', pos);
		if (nl == std::string::npos) {
			return fail("unterminated metadata line");
		}
		std::string line = body.substr(pos, nl - pos);
		size_t eq = line.find('=');
		if (eq == std::string::npos || eq == 0) {
			return fail("malformed metadata line");
		}
		for (unsigned char c : line) {
			if (c < 0x20 || c == 0x7F) {
				return fail("control character in metadata");
			}
		}
		std::string key = line.substr(0, eq);
		for (auto &f : fields) {
			if (f.first == key) {
				return fail("duplicate metadata key " + key);
			}
		}
		fields.push_back({key, line.substr(eq + 1)});
		pos = nl + 1;
	}
	auto get = [&](const char *key, std::string *value) {
		for (auto &f : fields) {
			if (f.first == key) {
				*value = f.second;
				return true;
			}
		}
		return false;
	};
	static const char *const KNOWN[] = {"id",     "video_id", "title",  "quality", "layout",      "v_itag", "v_mime",
	                                    "v_size", "a_itag",   "a_mime", "a_size",  "duration_ms", "state"};
	for (auto &f : fields) {
		bool known = false;
		for (const char *k : KNOWN) {
			known |= f.first == k;
		}
		if (!known) {
			return fail("unknown metadata key " + f.first);
		}
	}
	ItemMeta m;
	std::string s;
	uint64_t n = 0;
	if (!get("id", &m.id) || !get("video_id", &m.video_id) || !get("title", &m.title)) {
		return fail("missing id/video_id/title");
	}
	if (!is_valid_video_id(m.video_id) || m.title.size() > MAX_TITLE_BYTES) {
		return fail("invalid video id or title");
	}
	if (!get("quality", &s) || !parse_u64(s, 10000, &n) || !is_supported_quality((int)n)) {
		return fail("invalid quality");
	}
	m.quality = (int)n;
	if (make_item_id(m.video_id, m.quality) != m.id) {
		return fail("id does not match video id and quality");
	}
	if (!get("layout", &s) || (s != "separate" && s != "combined")) {
		return fail("invalid layout");
	}
	m.layout = s == "combined" ? Layout::COMBINED : Layout::SEPARATE;
	auto get_format = [&](const char *itag_key, const char *mime_key, const char *size_key, FormatIdentity *f) {
		std::string v;
		uint64_t num = 0;
		if (!get(itag_key, &v) || !parse_u64(v, 9999, &num) || num == 0) {
			return false;
		}
		f->itag = (int)num;
		if (!get(mime_key, &f->mime) || !is_valid_mime(f->mime)) {
			return false;
		}
		if (!get(size_key, &v) || !parse_u64(v, MAX_FILE_BYTES, &f->size)) {
			return false;
		}
		return true;
	};
	if (!get_format("v_itag", "v_mime", "v_size", &m.video)) {
		return fail("invalid video format identity");
	}
	if (m.layout == Layout::SEPARATE) {
		if (!get_format("a_itag", "a_mime", "a_size", &m.audio)) {
			return fail("invalid audio format identity");
		}
		if (m.video.mime.compare(0, 6, "video/") != 0 || m.audio.mime.compare(0, 6, "audio/") != 0) {
			return fail("format kinds do not match the layout");
		}
	} else {
		if (get("a_itag", &s) || get("a_mime", &s) || get("a_size", &s)) {
			return fail("audio fields in a combined item");
		}
		if (m.video.mime.compare(0, 6, "video/") != 0) {
			return fail("format kind does not match the layout");
		}
	}
	if (!get("duration_ms", &s) || !parse_u64(s, MAX_DURATION_MS, &n)) {
		return fail("invalid duration");
	}
	m.duration_ms = (int64_t)n;
	if (!get("state", &s)) {
		return fail("missing state");
	}
	if (s == "downloading") {
		m.state = PersistedState::DOWNLOADING;
	} else if (s == "ready") {
		m.state = PersistedState::READY;
	} else if (s == "failed") {
		m.state = PersistedState::FAILED;
	} else if (s == "cancelled") {
		m.state = PersistedState::CANCELLED;
	} else {
		return fail("invalid state");
	}
	if (m.state == PersistedState::READY && (m.video.size == 0 || (m.layout == Layout::SEPARATE && m.audio.size == 0))) {
		return fail("ready item without known sizes");
	}
	if (out) {
		*out = m;
	}
	return true;
}

// ------------------------------------------------------------------ POSIX / newlib file system
namespace {
struct PosixFile : DlFile {
	FILE *fp;
	int err = 0;
	char *buf;
	explicit PosixFile(FILE *fp) : fp(fp), buf((char *)malloc(64 * 1024)) {
		if (buf) {
			setvbuf(fp, buf, _IOFBF, 64 * 1024); // one SD write per 64 KiB instead of per BUFSIZ
		}
	}
	~PosixFile() {
		if (fp) {
			fclose(fp);
		}
		free(buf);
	}
	size_t write(const void *data, size_t len) override {
		errno = 0;
		size_t n = fwrite(data, 1, len, fp);
		if (n != len) {
			err = errno ? errno : EIO;
		}
		return n;
	}
	bool seek(uint64_t pos) override {
		if (pos > MAX_FILE_BYTES) {
			err = EFBIG;
			return false;
		}
		errno = 0;
		if (fseek(fp, (long)pos, SEEK_SET) != 0) {
			err = errno ? errno : EIO;
			return false;
		}
		return true;
	}
	bool sync() override {
		errno = 0;
		if (fflush(fp) != 0) {
			err = errno ? errno : EIO;
			return false;
		}
		errno = 0;
		if (fsync(fileno(fp)) != 0 && errno != EINVAL && errno != ENOSYS) {
			err = errno ? errno : EIO;
			return false;
		}
		return true;
	}
	bool close() override {
		errno = 0;
		int r = fclose(fp);
		fp = NULL;
		if (r != 0) {
			err = errno ? errno : EIO;
			return false;
		}
		return true;
	}
	int error() override { return err; }
};

struct PosixFs : DlFs {
	int err = 0;
	DlFile *open_truncate(const std::string &path) override {
		errno = 0;
		FILE *fp = fopen(path.c_str(), "wb");
		if (!fp) {
			err = errno ? errno : EIO;
			return NULL;
		}
		return new PosixFile(fp);
	}
	bool read_small(const std::string &path, size_t max_len, std::string *out) override {
		errno = 0;
		FILE *fp = fopen(path.c_str(), "rb");
		if (!fp) {
			err = errno ? errno : EIO;
			return false;
		}
		std::string data(max_len + 1, '\0');
		size_t n = fread(&data[0], 1, data.size(), fp);
		bool ok = !ferror(fp);
		fclose(fp);
		if (!ok || n > max_len) {
			err = ok ? EFBIG : EIO;
			return false;
		}
		data.resize(n);
		*out = data;
		return true;
	}
	int remove_file(const std::string &path) override {
		errno = 0;
		return unlink(path.c_str()) == 0 ? 0 : (err = errno ? errno : EIO);
	}
	int rename_file(const std::string &from, const std::string &to) override {
		errno = 0;
		return rename(from.c_str(), to.c_str()) == 0 ? 0 : (err = errno ? errno : EIO);
	}
	int make_dir(const std::string &path) override {
		errno = 0;
		return mkdir(path.c_str(), 0777) == 0 ? 0 : (err = errno ? errno : EIO);
	}
	int remove_dir(const std::string &path) override {
		errno = 0;
		return rmdir(path.c_str()) == 0 ? 0 : (err = errno ? errno : EIO);
	}
	bool list_dir(const std::string &path, std::vector<std::string> *names) override {
		errno = 0;
		DIR *dir = opendir(path.c_str());
		if (!dir) {
			err = errno ? errno : EIO;
			return false;
		}
		names->clear();
		while (struct dirent *item = readdir(dir)) {
			std::string name = item->d_name;
			if (name != "." && name != "..") {
				names->push_back(name);
			}
		}
		closedir(dir);
		return true;
	}
	DlStat stat_path(const std::string &path) override {
		DlStat res;
		struct stat st;
		std::string p = path;
		while (p.size() > 1 && p.back() == '/') {
			p.pop_back();
		}
#ifdef __3DS__
		int r = stat(p.c_str(), &st); // no symlinks on the SD card file system
#else
		int r = lstat(p.c_str(), &st);
#endif
		if (r != 0) {
			err = errno;
			return res;
		}
		res.exists = true;
		res.is_dir = S_ISDIR(st.st_mode);
		res.is_file = S_ISREG(st.st_mode);
#ifdef S_ISLNK
		res.is_link = S_ISLNK(st.st_mode);
#endif
		res.size = (uint64_t)st.st_size;
		return res;
	}
	uint64_t free_bytes(const std::string &path) override {
		struct statvfs st;
		if (statvfs(path.c_str(), &st) != 0) {
			return std::numeric_limits<uint64_t>::max();
		}
		uint64_t unit = st.f_frsize ? st.f_frsize : st.f_bsize;
		return (uint64_t)st.f_bavail * unit;
	}
	int last_errno() override { return err; }
};
} // namespace

DlFs &posix_fs() {
	static PosixFs fs;
	return fs;
}

// ------------------------------------------------------------------ store
DownloadStore::DownloadStore(DlFs &fs, const std::string &root_) : fs(fs), root(root_) {
	if (root.empty() || root.back() != '/') {
		root += '/';
	}
}
std::string DownloadStore::item_dir(const std::string &id) const { return root + id + "/"; }
std::string DownloadStore::file_path(const std::string &id, const char *name) const { return item_dir(id) + name; }

static bool is_owned_name(const std::string &name) {
	return name == FILE_VIDEO || name == FILE_AUDIO || name == FILE_COMBINED || name == FILE_META ||
	       name == FILE_META_TMP;
}

bool DownloadStore::prepare_item_dir(const std::string &id, bool claim, std::string *err) {
	if (!parse_item_id(id, NULL, NULL)) {
		*err = "invalid item id";
		return false;
	}
	// root: create the missing components of the app-fixed root path only; the root itself must be a real directory
	for (size_t slash = root.find('/', 1); slash != std::string::npos; slash = root.find('/', slash + 1)) {
		std::string part = root.substr(0, slash);
		bool is_root = slash + 1 == root.size();
		DlStat st = fs.stat_path(part);
		if (!st.exists) {
			int r = fs.make_dir(part);
			if (r != 0 && r != EEXIST) {
				*err = "cannot create " + part + " (errno " + std::to_string(r) + ")";
				return false;
			}
			st = fs.stat_path(part);
		}
		if (is_root ? (!st.is_dir || st.is_link) : st.is_file) {
			*err = part + " is not a directory";
			return false;
		}
	}
	std::string dir = item_dir(id);
	DlStat st = fs.stat_path(dir);
	if (claim) {
		if (st.exists || st.is_link) {
			*err = "a folder for this item already exists and was not created by this download; nothing was changed";
			return false;
		}
		int r = fs.make_dir(dir.substr(0, dir.size() - 1));
		if (r != 0) {
			*err = r == EEXIST ? "a folder for this item already exists; nothing was changed"
			                   : "cannot create the item folder (errno " + std::to_string(r) + ")";
			return false;
		}
		st = fs.stat_path(dir);
		if (!st.is_dir || st.is_link) {
			*err = "item path is not a real directory";
			return false;
		}
		return true; // created just now: empty and owned
	}
	std::string why;
	if (!owns_item(id, &why)) {
		*err = "this folder is not a download of this app (" + why + "); nothing was changed";
		return false;
	}
	std::vector<std::string> names;
	if (!fs.list_dir(dir, &names)) {
		*err = "cannot list the item folder";
		return false;
	}
	for (auto &name : names) {
		if (!is_owned_name(name)) {
			continue; // never touched
		}
		DlStat f = fs.stat_path(dir + name);
		if (!f.is_file || f.is_link) {
			*err = "unexpected non-file entry " + name + " in the item folder";
			return false;
		}
	}
	return true;
}

bool DownloadStore::owns_item(const std::string &id, std::string *why) {
	if (!parse_item_id(id, NULL, NULL)) {
		*why = "invalid item id";
		return false;
	}
	DlStat st = fs.stat_path(item_dir(id));
	if (!st.exists || !st.is_dir || st.is_link) {
		*why = "not a real folder";
		return false;
	}
	ItemMeta meta;
	return load_meta(id, &meta, why);
}
void DownloadStore::release_claim(const std::string &id) {
	if (!parse_item_id(id, NULL, NULL)) {
		return;
	}
	fs.remove_file(file_path(id, FILE_META_TMP));
	fs.remove_dir(item_dir(id).substr(0, item_dir(id).size() - 1)); // fails (keeps it) unless empty
}

bool DownloadStore::write_durable(const std::string &path, const std::string &data, std::string *err) {
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

bool DownloadStore::write_meta(const ItemMeta &meta, std::string *err) {
	if (!parse_item_id(meta.id, NULL, NULL)) {
		*err = "invalid item id";
		return false;
	}
	std::string text = serialize_meta(meta);
	std::string why;
	if (!parse_meta(text, NULL, &why)) { // never persist something the loader would reject
		*err = "refusing to write invalid metadata: " + why;
		return false;
	}
	std::string tmp = file_path(meta.id, FILE_META_TMP), main = file_path(meta.id, FILE_META);
	if (!write_durable(tmp, text, err)) {
		*err = "metadata " + *err;
		return false;
	}
	int r = fs.remove_file(main);
	if (r != 0 && r != ENOENT) {
		*err = "cannot replace metadata (errno " + std::to_string(r) + ")";
		return false;
	}
	r = fs.rename_file(tmp, main);
	if (r != 0) {
		*err = "cannot publish metadata (errno " + std::to_string(r) + ")";
		return false;
	}
	return true;
}

bool DownloadStore::load_meta(const std::string &id, ItemMeta *out, std::string *why) {
	std::string text, why_tmp, why_main = "no metadata";
	// meta.tmp exists only between a complete durable write and its rename: when it parses, it is the newest
	if (fs.read_small(file_path(id, FILE_META_TMP), MAX_META_BYTES, &text)) {
		ItemMeta m;
		if (parse_meta(text, &m, &why_tmp) && m.id == id) {
			*out = m;
			return true;
		}
	}
	if (fs.read_small(file_path(id, FILE_META), MAX_META_BYTES, &text)) {
		ItemMeta m;
		if (parse_meta(text, &m, &why_main)) {
			if (m.id == id) {
				*out = m;
				return true;
			}
			why_main = "metadata belongs to another item";
		}
	}
	if (why) {
		*why = why_main;
	}
	return false;
}

bool DownloadStore::verify_media(const ItemMeta &meta, std::string *why) {
	std::vector<const FormatIdentity *> formats = {&meta.video};
	if (meta.layout == Layout::SEPARATE) {
		formats.push_back(&meta.audio);
	}
	auto names = media_files(meta.layout);
	for (size_t i = 0; i < names.size(); i++) {
		DlStat st = fs.stat_path(file_path(meta.id, names[i]));
		if (!st.exists || !st.is_file || st.is_link) {
			*why = std::string(names[i]) + " is missing";
			return false;
		}
		if (formats[i]->size == 0 || st.size != formats[i]->size) {
			*why = std::string(names[i]) + " has " + std::to_string(st.size) + " of " +
			       std::to_string(formats[i]->size) + " bytes";
			return false;
		}
	}
	return true;
}

std::vector<ScannedItem> DownloadStore::scan() {
	std::vector<ScannedItem> res;
	std::vector<std::string> names;
	DlStat root_st = fs.stat_path(root);
	if (!root_st.exists || !root_st.is_dir || root_st.is_link || !fs.list_dir(root, &names)) {
		return res;
	}
	std::sort(names.begin(), names.end());
	for (auto &name : names) {
		if (!parse_item_id(name, NULL, NULL)) {
			continue; // not ours: ignored, untouched
		}
		DlStat st = fs.stat_path(item_dir(name));
		if (!st.is_dir || st.is_link) {
			continue;
		}
		DlStat m1 = fs.stat_path(file_path(name, FILE_META)), m2 = fs.stat_path(file_path(name, FILE_META_TMP));
		if (!m1.exists && !m2.exists) {
			continue; // an item exists only once its metadata was written; leftovers stay untouched
		}
		ScannedItem item;
		item.id = name;
		item.meta_ok = load_meta(name, &item.meta, &item.problem);
		if (item.meta_ok && item.meta.state == PersistedState::READY) {
			item.files_ok = verify_media(item.meta, &item.problem);
		}
		res.push_back(item);
	}
	return res;
}

bool DownloadStore::delete_item(const std::string &id, std::string *err) {
	if (!parse_item_id(id, NULL, NULL)) {
		*err = "invalid item id";
		return false;
	}
	std::string dir = item_dir(id);
	DlStat st = fs.stat_path(dir);
	if (!st.exists && !st.is_link) {
		return true;
	}
	std::string why;
	if (!owns_item(id, &why)) { // never infer ownership from file names or unreadable metadata
		*err = "not a download of this app (" + why + "); nothing was deleted";
		return false;
	}
	// media first, metadata last: an interrupted delete leaves a non-ready item, never a "ready" one without media
	const char *order[] = {FILE_VIDEO, FILE_AUDIO, FILE_COMBINED, FILE_META_TMP, FILE_META};
	for (const char *name : order) {
		std::string path = dir + name;
		DlStat f = fs.stat_path(path);
		if (!f.exists) {
			continue;
		}
		if (!f.is_file || f.is_link) {
			*err = std::string("unexpected entry ") + name + " left in place";
			return false;
		}
		int r = fs.remove_file(path);
		if (r != 0 && r != ENOENT) {
			*err = std::string("cannot remove ") + name + " (errno " + std::to_string(r) + ")";
			return false;
		}
	}
	int r = fs.remove_dir(dir.substr(0, dir.size() - 1));
	if (r != 0) {
		*err = "files removed; folder kept because it contains entries this app did not create";
		return false;
	}
	return true;
}

} // namespace downloads
