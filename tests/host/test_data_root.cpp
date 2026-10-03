// Host tests for the v0.16.2 data root admission / preservation migration (source/data_io/data_root.{hpp,cpp}).
// REAL data_root.cpp on real files in a fresh directory (argv[1]), with a fault-injecting wrapper around the REAL
// POSIX seam. The v0.16.1-style fixture tree is written with the REAL download store, thumbnail cache and liked videos
// store; after a migration the same REAL stores load the new root (and, for rollback, the untouched old root).
// Nothing is ever deleted: a "user removes the partial folder" step is a rename out of the way.
#include "data_io/data_root.hpp"
#include "data_io/liked_videos.hpp"
#include "downloads/download_store.hpp"
#include "downloads/thumb_cache.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace data_root;

namespace {
int passed = 0, failed = 0;
#define CHECK(cond, ...)                                                                                               \
	do {                                                                                                               \
		if (cond) {                                                                                                    \
			passed++;                                                                                                  \
		} else {                                                                                                       \
			failed++;                                                                                                  \
			printf("    FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
			printf(__VA_ARGS__);                                                                                       \
			printf("\n");                                                                                              \
		}                                                                                                              \
	} while (0)

std::string BASE;

// ------------------------------------------------------------------ file helpers (test side, no deletion)
bool write_file(const std::string &path, const std::string &data) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) {
		return false;
	}
	bool ok = fwrite(data.data(), 1, data.size(), fp) == data.size();
	return fclose(fp) == 0 && ok;
}
std::string read_file(const std::string &path) {
	std::string res;
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return "<missing>";
	}
	char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		res.append(buf, n);
	}
	fclose(fp);
	return res;
}
bool exists(const std::string &path) {
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}
void mkdirs(const std::string &path) {
	for (size_t i = 1; i <= path.size(); i++) {
		if (i == path.size() || path[i] == '/') {
			mkdir(path.substr(0, i).c_str(), 0777);
		}
	}
}
// deterministic pseudo-random payload, streamed (never held in memory as a whole)
bool write_pattern(const std::string &path, uint64_t size, uint32_t seed) {
	FILE *fp = fopen(path.c_str(), "wb");
	if (!fp) {
		return false;
	}
	std::vector<char> buf(1 << 20);
	uint32_t x = seed * 2654435761u + 1;
	uint64_t left = size;
	bool ok = true;
	while (left && ok) {
		size_t n = (size_t)std::min<uint64_t>(left, buf.size());
		for (size_t i = 0; i < n; i++) {
			x ^= x << 13, x ^= x >> 17, x ^= x << 5;
			buf[i] = (char)(x >> 24);
		}
		ok = fwrite(buf.data(), 1, n, fp) == n;
		left -= n;
	}
	return fclose(fp) == 0 && ok;
}
uint64_t fnv64_file(const std::string &path) {
	uint64_t h = 1469598103934665603ULL;
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		return 0;
	}
	unsigned char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		for (size_t i = 0; i < n; i++) {
			h = (h ^ buf[i]) * 1099511628211ULL;
		}
	}
	fclose(fp);
	return h;
}
// type, size, content hash and mtime of every entry below `root` (links are not followed)
typedef std::map<std::string, std::string> Snapshot;
void snap_walk(const std::string &root, const std::string &rel, Snapshot *out) {
	struct stat st;
	std::string path = root + rel;
	if (lstat(path.c_str(), &st) != 0) {
		(*out)[rel] = "<absent>";
		return;
	}
	char buf[160];
#ifdef __APPLE__
	long long mt = (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
	long long mt = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
	if (S_ISLNK(st.st_mode)) {
		char target[512] = {0};
		readlink(path.c_str(), target, sizeof(target) - 1);
		(*out)[rel] = std::string("L ") + target;
	} else if (S_ISREG(st.st_mode)) {
		snprintf(buf, sizeof(buf), "F %lld %016llx %lld %o", (long long)st.st_size,
		         (unsigned long long)fnv64_file(path), mt, (unsigned)(st.st_mode & 07777));
		(*out)[rel] = buf;
	} else if (S_ISDIR(st.st_mode)) {
		snprintf(buf, sizeof(buf), "D %lld %o", mt, (unsigned)(st.st_mode & 07777));
		(*out)[rel] = buf;
		DIR *d = opendir(path.c_str());
		if (!d) {
			(*out)[rel] += " <unlistable>";
			return;
		}
		std::vector<std::string> names;
		while (struct dirent *e = readdir(d)) {
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
				names.push_back(e->d_name);
			}
		}
		closedir(d);
		for (auto &n : names) {
			snap_walk(root, rel + "/" + n, out);
		}
	} else {
		(*out)[rel] = "S";
	}
}
Snapshot snap(const std::string &root) {
	Snapshot s;
	std::string r = root;
	while (r.size() > 1 && r.back() == '/') {
		r.pop_back();
	}
	snap_walk(r, "", &s);
	return s;
}
// content-only view (type + size + hash) for comparing the old tree with its copy
std::map<std::string, std::string> content(const Snapshot &s) {
	std::map<std::string, std::string> res;
	for (auto &kv : s) {
		const std::string &v = kv.second;
		if (v[0] == 'F') {
			size_t a = v.find(' '), b = v.find(' ', a + 1), c = v.find(' ', b + 1);
			res[kv.first] = v.substr(0, c);
		} else if (v[0] == 'D') {
			res[kv.first] = "D";
		} else {
			res[kv.first] = v;
		}
	}
	return res;
}

// ------------------------------------------------------------------ fault-injecting seam over the real one
struct FaultFs : RootFs {
	RootFs &real = posix_root_fs();
	std::vector<std::string> mutations, opened;
	size_t max_read = 0, max_write = 0;
	uint64_t read_calls = 0, write_calls = 0;
	// faults (path matched by substring; "" = off)
	std::string open_read_fail;
	int open_read_err = EACCES;
	std::string read_fail;
	uint64_t read_fail_after = 0;
	std::string write_fail;
	uint64_t write_fail_after = 0;
	int write_err = ENOSPC;
	std::string sync_fail, close_fail, src_close_fail, create_fail;
	std::string probe_fail; // exact path (no trailing '/')
	int probe_err = EIO;
	std::string size_lie; // reported size + 1 from the 2nd probe of a matching path on
	int size_lie_probes = 0;
	std::string list_fail;
	bool space_set = false;
	uint64_t space_free = 0, space_cluster = 32768;
	int space_err = 0;
	int rename_err = 0;
	std::function<void()> before_rename;

	static bool match(const std::string &path, const std::string &pat) {
		return !pat.empty() && path.find(pat) != std::string::npos;
	}
	static std::string strip(const std::string &p) {
		std::string s = p;
		while (s.size() > 1 && s.back() == '/') {
			s.pop_back();
		}
		return s;
	}
	struct In : InFile {
		FaultFs &fs;
		InFile *r;
		std::string path;
		uint64_t done = 0;
		int err = 0;
		In(FaultFs &fs, InFile *r, const std::string &p) : fs(fs), r(r), path(p) {}
		~In() { delete r; }
		long read(void *buf, size_t len) override {
			fs.read_calls++;
			fs.max_read = std::max(fs.max_read, len);
			if (match(path, fs.read_fail) && done >= fs.read_fail_after) {
				err = EIO;
				return -1;
			}
			long n = r->read(buf, len);
			if (n > 0) {
				done += (uint64_t)n;
			}
			return n;
		}
		int close() override {
			int e = r->close();
			return match(path, fs.src_close_fail) ? EIO : e;
		}
		int error() override { return err ? err : r->error(); }
	};
	struct Out : OutFile {
		FaultFs &fs;
		OutFile *r;
		std::string path;
		uint64_t done = 0;
		int err = 0;
		Out(FaultFs &fs, OutFile *r, const std::string &p) : fs(fs), r(r), path(p) {}
		~Out() { delete r; }
		long write(const void *buf, size_t len) override {
			fs.write_calls++;
			fs.max_write = std::max(fs.max_write, len);
			if (match(path, fs.write_fail) && done + len > fs.write_fail_after) {
				size_t part = (size_t)(fs.write_fail_after > done ? fs.write_fail_after - done : 0);
				long n = part ? r->write(buf, part) : 0;
				done += n > 0 ? (uint64_t)n : 0;
				err = fs.write_err;
				return n; // short write: the card is full
			}
			long n = r->write(buf, len);
			done += n > 0 ? (uint64_t)n : 0;
			return n;
		}
		int sync() override {
			int e = r->sync();
			return match(path, fs.sync_fail) ? EIO : e;
		}
		int close() override {
			int e = r->close();
			return match(path, fs.close_fail) ? EIO : e;
		}
		int error() override { return err ? err : r->error(); }
	};
	Probe probe(const std::string &path) override {
		if (!probe_fail.empty() && strip(path) == probe_fail) {
			Probe p;
			p.kind = Probe::ERROR;
			p.err = probe_err;
			return p;
		}
		Probe p = real.probe(path);
		if (match(path, size_lie) && ++size_lie_probes >= 2) {
			p.size++;
		}
		return p;
	}
	int list_dir(const std::string &path, std::vector<std::string> *names) override {
		if (match(path, list_fail)) {
			return EIO;
		}
		return real.list_dir(path, names);
	}
	int make_dir(const std::string &path) override {
		mutations.push_back("mkdir " + path);
		return real.make_dir(path);
	}
	InFile *open_read(const std::string &path, int *err) override {
		opened.push_back(path);
		if (match(path, open_read_fail)) {
			*err = open_read_err;
			return NULL;
		}
		InFile *f = real.open_read(path, err);
		return f ? new In(*this, f, path) : NULL;
	}
	OutFile *create_new(const std::string &path, int *err) override {
		mutations.push_back("create " + path);
		if (match(path, create_fail)) {
			*err = EIO;
			return NULL;
		}
		OutFile *f = real.create_new(path, err);
		return f ? new Out(*this, f, path) : NULL;
	}
	int rename_dir_noreplace(const std::string &from, const std::string &to) override {
		mutations.push_back("rename " + from + " -> " + to);
		if (before_rename) {
			before_rename();
		}
		if (rename_err) {
			return rename_err;
		}
		return real.rename_dir_noreplace(from, to);
	}
	int space(const std::string &path, uint64_t *free_bytes, uint64_t *cluster_bytes) override {
		if (space_err) {
			return space_err;
		}
		if (space_set) {
			*free_bytes = space_free, *cluster_bytes = space_cluster;
			return 0;
		}
		return real.space(path, free_bytes, cluster_bytes);
	}
	bool touched(const std::string &prefix) const {
		for (auto &m : mutations) {
			size_t sp = m.find(' ');
			if (m.compare(sp + 1, prefix.size(), prefix) == 0 || m.find("-> " + prefix) != std::string::npos) {
				return true;
			}
		}
		return false;
	}
};

// ------------------------------------------------------------------ fixture: a v0.16.1-style old root
struct Case {
	std::string dir, sd, old_root, stage, root;
	Layout layout;
	explicit Case(const std::string &name) {
		dir = BASE + "/" + name;
		sd = dir + "/sd";
		old_root = sd + "/3ds/FourthTubeTest/";
		stage = sd + "/3ds/opentube.partial/";
		root = sd + "/3ds/opentube/";
		layout = {old_root, stage, root};
		mkdirs(sd + "/3ds");
	}
};

const char *const VIDEO_ID = "dQw4w9WgXcQ";
const int QUALITY = 144;
const std::string JPEG = std::string("\xFF\xD8\xFF\xC0\x00\x0B\x08\x00\xB4\x01\x40\x01\x01\x11\x00\xFF\xD9", 17);

std::string item_id() { return downloads::make_item_id(VIDEO_ID, QUALITY); }

// the files v0.16.1 keeps under its root, written with the real stores where the app has one
bool make_old_root(const Case &c, uint64_t video_bytes = 300 * 1024, uint64_t audio_bytes = 70 * 1024) {
	std::string o = c.old_root;
	mkdirs(o);
	bool ok = true;
	ok &= write_file(o + "settings.txt", "<lang_ui>en</lang_ui>\n<theme>coral</theme>\n<dark_theme>0</dark_theme>\n");
	ok &=
	    write_file(o + "watch_history.json", "{\"version\":0,\"history\":[{\"id\":\"dQw4w9WgXcQ\",\"title\":\"A\"}]}");
	ok &= write_file(o + "watch_history_tmp.json", "{\"version\":0,\"history\":[]}"); // AtomicFileIO recovery state
	ok &= write_file(o + "subscription.json", "{\"version\":0,\"channels\":[{\"id\":\"UC123\",\"name\":\"C\"}]}");
	std::string token(96, '\0');
	for (size_t i = 0; i < token.size(); i++) {
		token[i] = (char)(i * 37 + 11); // stand-in for the encrypted token blob (never a real account file)
	}
	ok &= write_file(o + "oauth_tokens", token);
	ok &= write_file(o + "playback_diag.log", "# playback diag\n");
	ok &= write_file(o + "freeze_diag.log", "# freeze\n");
	mkdirs(o + "error");
	ok &= write_file(o + "error/2026103455.txt", "N/A\nN/A\nN/A\nN/A");
	mkdirs(o + "update");
	ok &= write_file(o + "update/OpenTube.cia.part", std::string(4096, 'C')); // updater session staging

	// liked videos (real LikedStore save protocol)
	liked::LikedStore likes(downloads::posix_fs(), o);
	likes.load();
	liked::LikedVideo v;
	ok &= liked::make_entry(VIDEO_ID, "Fixture video", "Fixture channel", 212000, 1759500000, &v);
	std::string msg, text;
	ok &= likes.add(v, &msg);
	unsigned g = 0;
	ok &= likes.save_begin(&text, &g);
	std::string err;
	bool saved = likes.save_write(text, &err);
	likes.save_end(g, saved, err);
	ok &= saved;

	// one READY download (real DownloadStore) and its thumbnail (real ThumbCache)
	downloads::DownloadStore store(downloads::posix_fs(), o + "downloads/");
	std::string id = item_id();
	ok &= store.prepare_item_dir(id, true, &err);
	ok &= write_pattern(store.file_path(id, downloads::FILE_VIDEO), video_bytes, 1);
	ok &= write_pattern(store.file_path(id, downloads::FILE_AUDIO), audio_bytes, 2);
	downloads::ItemMeta meta;
	meta.id = id;
	meta.video_id = VIDEO_ID;
	meta.title = "Fixture video";
	meta.quality = QUALITY;
	meta.layout = downloads::Layout::SEPARATE;
	meta.video = {160, "video/mp4", video_bytes};
	meta.audio = {140, "audio/mp4", audio_bytes};
	meta.duration_ms = 212000;
	meta.state = downloads::PersistedState::READY;
	ok &= store.write_meta(meta, &err);
	downloads::ThumbCache thumbs(downloads::posix_fs(), o + "download_thumbs/");
	std::string why;
	ok &= thumbs.owned(true, &why);
	ok &= thumbs.write(id, JPEG, &err);
	if (!ok) {
		printf("    fixture problem: %s %s %s\n", err.c_str(), why.c_str(), msg.c_str());
	}
	return ok;
}

// what the real stores see in a root: the liked list, the READY download with its playback paths, the thumbnail
struct Loaded {
	bool liked_ok = false, download_ok = false, thumb_ok = false;
	std::string video_path;
};
Loaded load_root(const std::string &root) {
	Loaded l;
	liked::LikedStore likes(downloads::posix_fs(), root);
	likes.load();
	l.liked_ok = likes.state() == liked::LoadState::LOADED && likes.items().size() == 1 &&
	             likes.items()[0].id == VIDEO_ID && likes.items()[0].title == "Fixture video";
	downloads::DownloadStore store(downloads::posix_fs(), root + "downloads/");
	for (auto &item : store.scan()) {
		if (item.id == item_id() && item.meta_ok && item.files_ok &&
		    item.meta.state == downloads::PersistedState::READY) {
			// DownloadManager::acquire_playback() hands the player exactly these paths
			l.video_path = store.file_path(item.id, downloads::media_files(item.meta.layout)[0]);
			l.download_ok = read_file(l.video_path) != "<missing>";
		}
	}
	downloads::ThumbCache thumbs(downloads::posix_fs(), root + "download_thumbs/");
	std::string why, data;
	l.thumb_ok = thumbs.owned(false, &why) && thumbs.read(item_id(), &data, &why) && data == JPEG;
	return l;
}

bool marker_of(const std::string &root, Marker *m) { return parse_marker(read_file(root + MARKER_NAME), m); }

// ------------------------------------------------------------------ cases
void test_marker_format() {
	printf("marker format\n");
	Marker m;
	m.migrated = true, m.files = 12, m.bytes = 345678;
	std::string t = format_marker(m);
	Marker back;
	CHECK(parse_marker(t, &back) && back.migrated && back.files == 12 && back.bytes == 345678, "roundtrip");
	Marker f;
	CHECK(parse_marker(format_marker(f), &back) && !back.migrated && back.files == 0, "fresh roundtrip");
	std::vector<std::string> bad = {
	    "",
	    t.substr(0, t.size() - 1), // no final newline
	    t.substr(0, t.size() / 2), // truncated
	    t + "extra=1\n",           // extra line
	    "X" + t,                   // header
	};
	std::string crlf;
	for (char ch : t) {
		crlf += ch == '\n' ? std::string("\r\n") : std::string(1, ch);
	}
	bad.push_back(crlf);
	std::string edited = t;
	edited[edited.find("files=12") + 6] = '9'; // content edited, checksum kept
	bad.push_back(edited);
	std::string upper = t;
	for (size_t i = upper.find("check=") + 6; i < upper.size() - 1; i++) {
		upper[i] = (char)toupper(upper[i]);
	}
	if (upper != t) {
		bad.push_back(upper);
	}
	Marker lead;
	lead.files = 5;
	std::string zero = format_marker(lead);
	zero.replace(zero.find("files=5"), 7, "files=05");
	bad.push_back(zero);
	bad.push_back(t + std::string(600, 'x'));
	int rejected = 0;
	for (auto &b : bad) {
		rejected += !parse_marker(b, &back);
	}
	CHECK(rejected == (int)bad.size(), "invalid markers rejected: %d of %zu", rejected, bad.size());
}

void test_fresh() {
	printf("fresh start: no old, no new root (and no /3ds folder)\n");
	Case c("fresh");
	std::string sd2 = c.dir + "/sd_no3ds";
	mkdirs(sd2);
	Layout l2 = {sd2 + "/3ds/FourthTubeTest/", sd2 + "/3ds/opentube.partial/", sd2 + "/3ds/opentube/"};
	for (const Layout *l : {&c.layout, &l2}) {
		FaultFs fs;
		Outcome o = prepare(fs, *l, nullptr);
		CHECK(o.status == Status::FRESH, "fresh status %s (%s)", block_name(o.block), o.detail.c_str());
		Marker m;
		CHECK(marker_of(l->new_root, &m) && !m.migrated && m.files == 0, "fresh marker");
		CHECK(!exists(l->stage_root) && !exists(l->old_root), "no staging left, old root not created");
		CHECK(!fs.touched(l->old_root), "old root never written");
		Snapshot before = snap(l->new_root);
		FaultFs fs2;
		Outcome again = prepare(fs2, *l, nullptr);
		CHECK(again.status == Status::READY && fs2.mutations.empty(), "second start: READY, nothing written");
		CHECK(snap(l->new_root) == before, "new root unchanged by the second start");
	}
	// an old root appearing later (e.g. v0.16.1 reinstalled and used) is never imported into an established root
	make_old_root(c);
	FaultFs fs3;
	Outcome later = prepare(fs3, c.layout, nullptr);
	CHECK(later.status == Status::READY && fs3.mutations.empty() && fs3.opened.size() == 1,
	      "established root stays authoritative over a later old root (opened %zu)", fs3.opened.size());
}

void test_migrate_full() {
	printf("migration of a complete v0.16.1 root; new-root loads; no remigration; rollback\n");
	Case c("migrate");
	CHECK(make_old_root(c), "fixture");
	Snapshot old_before = snap(c.old_root);
	FaultFs fs;
	std::vector<Progress> seen;
	Outcome o = prepare(fs, c.layout, [&](const Progress &p) {
		seen.push_back(p);
		return true;
	});
	CHECK(o.status == Status::MIGRATED, "status %s (%s)", block_name(o.block), o.detail.c_str());
	CHECK(snap(c.old_root) == old_before, "old root byte-, mtime- and mode-identical");
	CHECK(!fs.touched(c.old_root), "no mutation call under the old root");
	CHECK(!exists(c.stage), "staging folder published (gone)");
	auto old_content = content(old_before), new_content = content(snap(c.root));
	std::map<std::string, std::string> expected;
	for (auto &kv : old_content) {
		if (kv.first.compare(0, 7, "/update") != 0) {
			expected[kv.first] = kv.second;
		}
	}
	Marker m;
	CHECK(marker_of(c.root, &m) && m.migrated, "migrated marker");
	new_content.erase("/" + std::string(MARKER_NAME));
	CHECK(new_content == expected, "new root == old root minus update/ (%zu vs %zu entries)", new_content.size(),
	      expected.size());
	CHECK(!exists(c.root + "update"), "updater staging not copied");
	uint64_t files = 0, bytes = 0;
	for (auto &kv : expected) {
		if (kv.second[0] == 'F') {
			files++;
			bytes += strtoull(kv.second.c_str() + 2, NULL, 10);
		}
	}
	CHECK(m.files == files && m.bytes == bytes && o.copied_bytes == bytes, "marker counts %llu/%llu",
	      (unsigned long long)m.files, (unsigned long long)m.bytes);
	CHECK(read_file(c.root + "settings.txt").find("<theme>coral</theme>") != std::string::npos, "theme carried");
	CHECK(read_file(c.root + "oauth_tokens") == read_file(c.old_root + "oauth_tokens"), "login state bytes");
	CHECK(read_file(c.root + "watch_history_tmp.json") == read_file(c.old_root + "watch_history_tmp.json"),
	      "recovery tmp files carried");
	bool phases =
	    !seen.empty() && seen.front().phase == Progress::SCANNING && seen.back().phase == Progress::PUBLISHING;
	bool monotonic = true;
	for (size_t i = 1; i < seen.size(); i++) {
		monotonic &= seen[i].done_bytes >= seen[i - 1].done_bytes;
	}
	CHECK(phases && monotonic, "progress scanning -> copying -> publishing, monotonic (%zu reports)", seen.size());

	Loaded n = load_root(c.root);
	CHECK(n.liked_ok && n.download_ok && n.thumb_ok, "real stores load the new root (liked %d dl %d thumb %d)",
	      n.liked_ok, n.download_ok, n.thumb_ok);
	CHECK(n.video_path.compare(0, c.root.size(), c.root) == 0, "playback path resolves under the new root: %s",
	      n.video_path.c_str());

	// v0.16.2 uses its root; later starts never remigrate, even when the old root changes (rollback use)
	write_file(c.root + "settings.txt", "<theme>mint</theme>\n");
	write_file(c.old_root + "settings.txt", "<theme>slate</theme>\n"); // v0.16.1 used again after a rollback
	Snapshot new_before = snap(c.root);
	FaultFs fs2;
	Outcome again = prepare(fs2, c.layout, nullptr);
	CHECK(again.status == Status::READY && fs2.mutations.empty(), "relaunch: READY, nothing written");
	CHECK(snap(c.root) == new_before, "relaunch leaves the new root as it is (no remigration)");
	CHECK(read_file(c.root + "settings.txt") == "<theme>mint</theme>\n",
	      "v0.16.2 change kept, old change not imported");
	CHECK(read_file(c.old_root + "settings.txt") == "<theme>slate</theme>\n",
	      "v0.16.2 changes are not synced back to the old root");
}

void test_rollback_reads() {
	printf("rollback: the untouched old root still loads with the same (v0.16.1) store code\n");
	Case c("rollback");
	make_old_root(c);
	Snapshot before = snap(c.old_root);
	FaultFs fs;
	CHECK(prepare(fs, c.layout, nullptr).status == Status::MIGRATED, "migrated");
	// v0.16.2 activity in the new root
	write_file(c.root + "settings.txt", "<theme>glacier</theme>\n");
	liked::LikedStore likes(downloads::posix_fs(), c.root);
	likes.load();
	std::string msg, text, err;
	likes.remove(VIDEO_ID, &msg);
	unsigned g = 0;
	if (likes.save_begin(&text, &g)) {
		bool ok = likes.save_write(text, &err);
		likes.save_end(g, ok, err);
	}
	CHECK(snap(c.old_root) == before, "old root identical after migration and v0.16.2 use");
	Loaded old = load_root(c.old_root);
	CHECK(old.liked_ok && old.download_ok && old.thumb_ok, "old root loads (liked %d dl %d thumb %d)", old.liked_ok,
	      old.download_ok, old.thumb_ok);
	CHECK(old.video_path.compare(0, c.old_root.size(), c.old_root) == 0, "old playback path under the old root");
	CHECK(read_file(c.old_root + "settings.txt").find("coral") != std::string::npos, "old settings as before");
}

void test_foreign_new_root() {
	printf("foreign / unestablished new root: never adopted, overwritten or merged\n");
	struct Variant {
		const char *name;
		std::function<void(const Case &)> make;
	};
	Marker valid;
	valid.migrated = true, valid.files = 1, valid.bytes = 2;
	std::string good = format_marker(valid);
	std::vector<Variant> vs = {
	    {"empty_dir", [](const Case &c) { mkdirs(c.root); }},
	    {"user_files",
	     [](const Case &c) {
		     mkdirs(c.root + "music");
		     write_file(c.root + "settings.txt", "user");
		     write_file(c.root + "music/a.mp3", "x");
	     }},
	    {"file", [](const Case &c) { write_file(c.sd + "/3ds/opentube", "a file"); }},
	    {"link_to_dir",
	     [](const Case &c) {
		     mkdirs(c.dir + "/elsewhere");
		     symlink((c.dir + "/elsewhere").c_str(), (c.sd + "/3ds/opentube").c_str());
	     }},
	    {"bad_checksum",
	     [good](const Case &c) {
		     mkdirs(c.root);
		     std::string t = good;
		     t[t.find("bytes=2") + 6] = '3';
		     write_file(c.root + MARKER_NAME, t);
	     }},
	    {"truncated_marker",
	     [good](const Case &c) {
		     mkdirs(c.root);
		     write_file(c.root + MARKER_NAME, good.substr(0, 30));
	     }},
	    {"marker_is_dir", [](const Case &c) { mkdirs(c.root + MARKER_NAME); }},
	    {"marker_is_link",
	     [good](const Case &c) {
		     mkdirs(c.root);
		     write_file(c.dir + "/m.txt", good);
		     symlink((c.dir + "/m.txt").c_str(), (c.root + MARKER_NAME).c_str());
	     }},
	    {"oversize_marker",
	     [good](const Case &c) {
		     mkdirs(c.root);
		     write_file(c.root + MARKER_NAME, good + std::string(1000, ' '));
	     }},
	};
	for (auto &v : vs) {
		Case c(std::string("foreign_") + v.name);
		make_old_root(c);
		v.make(c);
		Snapshot old_before = snap(c.old_root), new_before = snap(c.root);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.status == Status::BLOCKED && o.block == Block::NEW_ROOT_FOREIGN, "%s: %s (%s)", v.name,
		      block_name(o.block), o.detail.c_str());
		CHECK(fs.mutations.empty() && !exists(c.stage), "%s: nothing written", v.name);
		CHECK(snap(c.root) == new_before && snap(c.old_root) == old_before, "%s: foreign and old bytes kept", v.name);
		CHECK(!o.stage_written && user_message(o, c.layout).find("won't use, change or delete") != std::string::npos,
		      "%s: actionable message", v.name);
	}
	// a valid marker in a hand-made folder IS an established root (the marker is the only claim): documented
}

void test_partial_and_interruption() {
	printf("interrupted copy and relaunch; leftover staging is refused, never reused or removed\n");
	{
		Case c("stage_leftover");
		make_old_root(c);
		mkdirs(c.stage + "downloads");
		Marker m;
		m.migrated = true;
		write_file(c.stage + MARKER_NAME, format_marker(m)); // even a "complete-looking" staging folder
		Snapshot sb = snap(c.stage), ob = snap(c.old_root);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.block == Block::PARTIAL_LEFTOVER && fs.mutations.empty(), "leftover refused: %s", block_name(o.block));
		CHECK(snap(c.stage) == sb && snap(c.old_root) == ob && !exists(c.root), "leftover, old kept; no new root");
	}
	{
		Case c("stage_foreign"); // a user's own folder that happens to have the staging name
		make_old_root(c);
		mkdirs(c.stage + "photos");
		write_file(c.stage + "photos/holiday.jpg", "user bytes");
		Snapshot sb = snap(c.stage), ob = snap(c.old_root);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		std::string msg = user_message(o, c.layout);
		CHECK(o.block == Block::PARTIAL_LEFTOVER && fs.mutations.empty() && fs.opened.empty(), "foreign staging: %s",
		      block_name(o.block));
		CHECK(snap(c.stage) == sb && snap(c.old_root) == ob && !exists(c.root), "foreign staging bytes kept");
		CHECK(msg.find("rename sd:" + c.stage) != std::string::npos &&
		          msg.find("keeping its contents") != std::string::npos && msg.find("Delete") == std::string::npos,
		      "foreign staging: non-destructive advice only: %s", msg.c_str());
	}
	{
		Case c("stage_is_file");
		make_old_root(c);
		write_file(c.sd + "/3ds/opentube.partial", "x");
		FaultFs fs;
		CHECK(prepare(fs, c.layout, nullptr).block == Block::PARTIAL_LEFTOVER && fs.mutations.empty(),
		      "file at staging");
	}
	struct Stop {
		const char *name;
		Progress::Phase phase;
		int after; // reports of that phase before stopping
		bool stage_expected;
	};
	for (Stop s : {Stop{"scan", Progress::SCANNING, 1, false}, Stop{"mid_copy", Progress::COPYING, 2, true},
	               Stop{"before_publish", Progress::PUBLISHING, 0, true}}) {
		Case c(std::string("interrupt_") + s.name);
		make_old_root(c);
		Snapshot ob = snap(c.old_root);
		int n = 0;
		FaultFs fs;
		Outcome o = prepare(
		    fs, c.layout,
		    [&](const Progress &p) {
			    if (p.phase == s.phase && n++ >= s.after) {
				    return false; // the system asked the app to close
			    }
			    return true;
		    },
		    16 * 1024);
		CHECK(o.block == Block::CANCELLED && o.stage_written == s.stage_expected, "%s: cancelled (%s), staging %d",
		      s.name, block_name(o.block), (int)o.stage_written);
		CHECK(exists(c.stage) == s.stage_expected && !exists(c.root), "%s: no usable new root", s.name);
		CHECK(snap(c.old_root) == ob, "%s: old root unchanged", s.name);
		std::string msg = user_message(o, c.layout);
		CHECK((msg.find("rename sd:" + c.stage) != std::string::npos) == s.stage_expected, "%s: message: %s", s.name,
		      msg.c_str());
		// relaunch
		Snapshot stage_before = snap(c.stage);
		FaultFs fs2;
		Outcome again = prepare(fs2, c.layout, nullptr);
		if (s.stage_expected) {
			CHECK(again.block == Block::PARTIAL_LEFTOVER && fs2.mutations.empty(), "%s: relaunch refused", s.name);
			CHECK(snap(c.stage) == stage_before, "%s: partial output kept as it was", s.name);
			// the user removes the partial folder (simulated by moving it out of /3ds, nothing is deleted)
			CHECK(rename(c.stage.substr(0, c.stage.size() - 1).c_str(), (c.dir + "/removed_by_user").c_str()) == 0,
			      "move away");
			FaultFs fs3;
			Outcome retry = prepare(fs3, c.layout, nullptr);
			CHECK(retry.status == Status::MIGRATED, "%s: copy starts over after removal: %s", s.name,
			      block_name(retry.block));
		} else {
			CHECK(again.status == Status::MIGRATED, "%s: nothing was written, relaunch migrates", s.name);
		}
		CHECK(snap(c.old_root) == ob, "%s: old root unchanged after relaunch", s.name);
		Loaded l = load_root(c.root);
		CHECK(l.liked_ok && l.download_ok && l.thumb_ok, "%s: complete new root after the retry", s.name);
	}
}

void test_space() {
	printf("free space estimate and real ENOSPC\n");
	Case c("space");
	make_old_root(c);
	Snapshot ob = snap(c.old_root);
	FaultFs probe_fs;
	probe_fs.space_set = true, probe_fs.space_free = 0;
	Outcome need = prepare(probe_fs, c.layout, nullptr);
	CHECK(need.block == Block::NO_SPACE && need.need_bytes > need.total_bytes && probe_fs.mutations.empty(),
	      "no space: need %llu for %llu bytes, nothing written", (unsigned long long)need.need_bytes,
	      (unsigned long long)need.total_bytes);
	FaultFs fs;
	fs.space_set = true, fs.space_free = need.need_bytes - 1;
	Outcome o = prepare(fs, c.layout, nullptr);
	CHECK(o.block == Block::NO_SPACE && fs.mutations.empty() && !exists(c.stage), "one byte short: refused");
	std::string msg = user_message(o, c.layout);
	CHECK(msg.find("Needed: ") != std::string::npos && msg.find("free: ") != std::string::npos, "MB in the message");
	FaultFs fs2;
	fs2.space_err = EIO;
	Outcome u = prepare(fs2, c.layout, nullptr);
	CHECK(u.block == Block::SPACE_UNKNOWN && fs2.mutations.empty(), "space unknown: refused, nothing written");
	CHECK(snap(c.old_root) == ob, "old root unchanged");

	Case c2("space_enospc");
	make_old_root(c2);
	Snapshot ob2 = snap(c2.old_root);
	FaultFs fs3;
	fs3.space_set = true, fs3.space_free = need.need_bytes; // estimate passes, the card fills up anyway
	fs3.write_fail = "video.mp4", fs3.write_fail_after = 100 * 1024, fs3.write_err = ENOSPC;
	Outcome e = prepare(fs3, c2.layout, nullptr, 32 * 1024);
	CHECK(e.block == Block::COPY_FAILED && e.detail.find("errno 28") != std::string::npos && e.stage_written,
	      "ENOSPC: %s", e.detail.c_str());
	CHECK(!exists(c2.root) && exists(c2.stage) && snap(c2.old_root) == ob2, "partial kept apart, old unchanged");
	FaultFs fs4;
	CHECK(prepare(fs4, c2.layout, nullptr).block == Block::PARTIAL_LEFTOVER, "relaunch refused (no half library)");
}

void test_io_faults() {
	printf("read / write / sync / close / open faults and a changing source\n");
	struct Fault {
		const char *name;
		std::function<void(FaultFs &)> arm;
		const char *detail;
	};
	std::vector<Fault> faults = {
	    {"read", [](FaultFs &f) { f.read_fail = "video.mp4", f.read_fail_after = 64 * 1024; }, "read "},
	    {"open_source", [](FaultFs &f) { f.open_read_fail = "audio.m4a"; }, "open "},
	    {"short_write", [](FaultFs &f) { f.write_fail = "audio.m4a", f.write_fail_after = 5000, f.write_err = EIO; },
	     "write "},
	    {"sync", [](FaultFs &f) { f.sync_fail = "oauth_tokens"; }, "sync "},
	    {"close", [](FaultFs &f) { f.close_fail = "watch_history.json"; }, "close "},
	    {"source_close", [](FaultFs &f) { f.src_close_fail = "settings.txt"; }, "close source "},
	    {"create", [](FaultFs &f) { f.create_fail = "subscription.json"; }, "create "},
	    {"source_changed", [](FaultFs &f) { f.size_lie = "video.mp4"; }, "source changed"},
	    {"marker_sync", [](FaultFs &f) { f.sync_fail = MARKER_NAME; }, "sync marker"},
	    {"publish_rename", [](FaultFs &f) { f.rename_err = EIO; }, "rename staging"},
	};
	for (auto &fa : faults) {
		Case c(std::string("fault_") + fa.name);
		make_old_root(c);
		Snapshot ob = snap(c.old_root);
		FaultFs fs;
		fa.arm(fs);
		Outcome o = prepare(fs, c.layout, nullptr, 32 * 1024);
		bool kind = o.block == Block::COPY_FAILED || o.block == Block::PUBLISH_FAILED;
		CHECK(kind && o.detail.find(fa.detail) != std::string::npos && o.stage_written, "%s: %s (%s)", fa.name,
		      block_name(o.block), o.detail.c_str());
		CHECK(!exists(c.root) && exists(c.stage), "%s: no new root, partial output apart", fa.name);
		CHECK(snap(c.old_root) == ob && !fs.touched(c.old_root), "%s: old root unchanged", fa.name);
		std::string msg = user_message(o, c.layout);
		CHECK(msg.find("rename sd:" + c.stage) != std::string::npos, "%s: actionable message", fa.name);
		FaultFs again;
		CHECK(prepare(again, c.layout, nullptr).block == Block::PARTIAL_LEFTOVER && again.mutations.empty(),
		      "%s: relaunch refused, no half-copied library is opened", fa.name);
	}
}

void test_source_refusals() {
	printf("links, special files, invalid names, unreadable source vs absent source\n");
	struct Src {
		const char *name;
		std::function<void(const Case &)> make;
		Block want;
	};
	std::vector<Src> srcs = {
	    {"file_link",
	     [](const Case &c) {
		     write_file(c.dir + "/secret.txt", "outside");
		     symlink((c.dir + "/secret.txt").c_str(), (c.old_root + "downloads/link.mp4").c_str());
	     },
	     Block::SOURCE_UNSUPPORTED},
	    {"dir_link",
	     [](const Case &c) {
		     mkdirs(c.dir + "/outside");
		     symlink((c.dir + "/outside").c_str(), (c.old_root + "error/linked").c_str());
	     },
	     Block::SOURCE_UNSUPPORTED},
	    {"root_link",
	     [](const Case &c) {
		     std::string real = c.dir + "/real_old";
		     rename(c.old_root.substr(0, c.old_root.size() - 1).c_str(), real.c_str());
		     symlink(real.c_str(), c.old_root.substr(0, c.old_root.size() - 1).c_str());
	     },
	     Block::SOURCE_UNSUPPORTED},
	    {"fifo", [](const Case &c) { mkfifo((c.old_root + "pipe").c_str(), 0644); }, Block::SOURCE_UNSUPPORTED},
	    {"control_char_name",
	     [](const Case &c) {
		     write_file(c.old_root + "a\x01"
		                             "b",
		                "x");
	     },
	     Block::SOURCE_UNSUPPORTED},
	    {"trailing_dot_name", [](const Case &c) { write_file(c.old_root + "error/x.", "x"); },
	     Block::SOURCE_UNSUPPORTED},
	    {"too_deep", [](const Case &c) { mkdirs(c.old_root + "a/b/c/d/e/f/g/h/i"); }, Block::SOURCE_UNSUPPORTED},
	    {"root_is_file",
	     [](const Case &c) {
		     rename(c.old_root.substr(0, c.old_root.size() - 1).c_str(), (c.dir + "/moved_old").c_str());
		     write_file(c.old_root.substr(0, c.old_root.size() - 1), "file");
	     },
	     Block::SOURCE_UNSUPPORTED},
	    {"root_unlistable", [](const Case &c) { chmod(c.old_root.c_str(), 0000); }, Block::SOURCE_UNREADABLE},
	    {"subdir_unlistable", [](const Case &c) { chmod((c.old_root + "error").c_str(), 0000); },
	     Block::SOURCE_UNREADABLE},
	};
	for (auto &s : srcs) {
		Case c(std::string("source_") + s.name);
		make_old_root(c);
		s.make(c);
		Snapshot ob = snap(c.old_root);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.block == s.want, "%s: %s (%s)", s.name, block_name(o.block), o.detail.c_str());
		CHECK(fs.mutations.empty() && fs.opened.empty() && !exists(c.stage) && !exists(c.root),
		      "%s: nothing opened or written (opened %zu, mutations %zu)", s.name, fs.opened.size(),
		      fs.mutations.size());
		chmod(c.old_root.c_str(), 0755);
		chmod((c.old_root + "error").c_str(), 0755);
		CHECK(snap(c.old_root) == ob || std::string(s.name).find("unlistable") != std::string::npos,
		      "%s: old root unchanged", s.name);
		std::string msg = user_message(o, c.layout);
		CHECK(msg.find("Nothing was") != std::string::npos, "%s: message says nothing changed", s.name);
	}
	{
		Case c("source_probe_error"); // e.g. an SD read error on the old root (3DS: not ENOENT)
		make_old_root(c);
		FaultFs fs;
		fs.probe_fail = c.old_root.substr(0, c.old_root.size() - 1);
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.block == Block::SOURCE_UNREADABLE && fs.mutations.empty(), "unreadable old root is not absent: %s",
		      block_name(o.block));
	}
	{
		Case c("source_list_error");
		make_old_root(c);
		FaultFs fs;
		fs.list_fail = "downloads";
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.block == Block::SOURCE_UNREADABLE && fs.mutations.empty(), "list error: %s", block_name(o.block));
	}
	{
		Case c("new_root_probe_error");
		make_old_root(c);
		FaultFs fs;
		fs.probe_fail = c.root.substr(0, c.root.size() - 1);
		CHECK(prepare(fs, c.layout, nullptr).block == Block::NEW_ROOT_UNREADABLE && fs.mutations.empty(),
		      "new root unreadable");
		FaultFs fs2;
		fs2.probe_fail = c.stage.substr(0, c.stage.size() - 1);
		CHECK(prepare(fs2, c.layout, nullptr).block == Block::STAGE_UNREADABLE && fs2.mutations.empty(),
		      "staging unreadable");
	}
	{
		Case c("source_empty_dir"); // an old root without files still is an installation: copied (nothing to copy)
		mkdirs(c.old_root);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		Marker m;
		CHECK(o.status == Status::MIGRATED && marker_of(c.root, &m) && m.migrated && m.files == 0, "empty old root");
	}
}

void test_publish_collision() {
	printf("destination collision at publication (the new root appears before the rename)\n");
	for (int variant = 0; variant < 2; variant++) {
		Case c(variant ? "collision_file" : "collision_empty_dir");
		make_old_root(c);
		Snapshot ob = snap(c.old_root);
		FaultFs fs;
		fs.before_rename = [&]() {
			if (variant) {
				write_file(c.sd + "/3ds/opentube", "foreign");
			} else {
				mkdirs(c.root); // libctru's newlib rename() would delete this and replace it
			}
		};
		Outcome o = prepare(fs, c.layout, nullptr);
		CHECK(o.block == Block::PUBLISH_FAILED && o.detail.find("errno " + std::to_string(EEXIST)) != std::string::npos,
		      "variant %d: %s (%s)", variant, block_name(o.block), o.detail.c_str());
		Snapshot ns = snap(c.root.substr(0, c.root.size() - 1));
		CHECK(variant ? ns[""].compare(0, 2, "F ") == 0 && read_file(c.sd + "/3ds/opentube") == "foreign"
		              : ns.size() == 1 && ns[""][0] == 'D',
		      "variant %d: foreign destination kept as it was", variant);
		Marker m;
		CHECK(exists(c.stage) && marker_of(c.stage, &m), "variant %d: complete staging kept apart", variant);
		CHECK(snap(c.old_root) == ob, "variant %d: old unchanged", variant);
		FaultFs fs2;
		CHECK(prepare(fs2, c.layout, nullptr).block == Block::NEW_ROOT_FOREIGN && fs2.mutations.empty(),
		      "variant %d: relaunch refuses the foreign root", variant);
	}
}

void test_large_file() {
	printf("large file: streamed through one bounded buffer\n");
	Case c("large");
	const uint64_t big = 96ULL * 1024 * 1024 + 12345;
	CHECK(make_old_root(c, big, 1024 * 1024), "fixture with a %llu byte video", (unsigned long long)big);
	Snapshot ob = snap(c.old_root);
	fflush(stdout);
	int pipefd[2];
	pipe(pipefd);
	pid_t pid = fork();
	if (pid == 0) {
		struct rusage before, after;
		getrusage(RUSAGE_SELF, &before);
		FaultFs fs;
		Outcome o = prepare(fs, c.layout, nullptr);
		getrusage(RUSAGE_SELF, &after);
#ifdef __APPLE__
		long long grow = (long long)after.ru_maxrss - (long long)before.ru_maxrss; // bytes
#else
		long long grow = ((long long)after.ru_maxrss - (long long)before.ru_maxrss) * 1024; // KiB
#endif
		char buf[256];
		int n = snprintf(buf, sizeof(buf), "%d %zu %zu %llu %lld\n", (int)o.status, fs.max_read, fs.max_write,
		                 (unsigned long long)fs.read_calls, grow);
		write(pipefd[1], buf, n);
		_exit(0);
	}
	close(pipefd[1]);
	char buf[256] = {0};
	read(pipefd[0], buf, sizeof(buf) - 1);
	close(pipefd[0]);
	waitpid(pid, NULL, 0);
	int status = -1;
	size_t max_read = 0, max_write = 0;
	unsigned long long reads = 0;
	long long grow = -1;
	sscanf(buf, "%d %zu %zu %llu %lld", &status, &max_read, &max_write, &reads, &grow);
	printf("    status %d, max read %zu, max write %zu, read calls %llu, peak RSS growth %lld bytes\n", status,
	       max_read, max_write, reads, grow);
	CHECK(status == (int)Status::MIGRATED, "migrated");
	CHECK(max_read <= DEFAULT_CHUNK_BYTES && max_write <= DEFAULT_CHUNK_BYTES, "every I/O request <= one chunk");
	CHECK(reads >= big / DEFAULT_CHUNK_BYTES, "streamed in chunks");
	CHECK(grow >= 0 && grow < 16LL * 1024 * 1024, "peak memory growth bounded (< 16 MiB for a 96 MiB file)");
	CHECK(fnv64_file(c.root + "downloads/" + item_id() + "/video.mp4") ==
	          fnv64_file(c.old_root + "downloads/" + item_id() + "/video.mp4"),
	      "large file copied intact");
	CHECK(snap(c.old_root) == ob, "old root unchanged");
}

void test_messages() {
	printf("failure screen text: bounded, plain ASCII, for the production layout\n");
	Layout prod = {"/3ds/FourthTubeTest/", "/3ds/opentube.partial/", "/3ds/opentube/"};
	for (int b = (int)Block::NEW_ROOT_FOREIGN; b <= (int)Block::CANCELLED; b++) {
		for (int written = 0; written < 2; written++) {
			Outcome o;
			o.status = Status::BLOCKED;
			o.block = (Block)b;
			o.stage_written = written;
			o.need_bytes = 5ULL << 30, o.free_bytes = 123ULL << 20;
			o.detail = "write downloads/dQw4w9WgXcQ-360p/video.mp4: errno 28 " + std::string(80, 'z') + "\x01\xC3\xA9";
			std::string m = user_message(o, prod);
			int lines = 1;
			size_t longest = 0, cur = 0;
			bool ascii = true;
			for (char ch : m) {
				ascii &= (unsigned char)ch >= 0x20 && (unsigned char)ch < 0x7F ? true : ch == '\n';
				if (ch == '\n') {
					lines++, longest = std::max(longest, cur), cur = 0;
				} else {
					cur++;
				}
			}
			longest = std::max(longest, cur);
			// never advise deleting anything (only "won't use, change or delete it" about OpenTube itself)
			std::string lower = m;
			for (auto &ch : lower) {
				ch = (char)tolower(ch);
			}
			size_t own = 0, any = 0;
			for (size_t at = 0; (at = lower.find("delete", at)) != std::string::npos; at++) {
				any++;
			}
			for (size_t at = 0; (at = lower.find("change or delete it", at)) != std::string::npos; at++) {
				own++;
			}
			CHECK(any == own, "%s/%d: no deletion advice", block_name((Block)b), written);
			CHECK(lines <= 12 && longest <= 56 && ascii && m.find("FourthTubeTest") != std::string::npos,
			      "%s/%d: %d lines, longest %zu, ascii %d", block_name((Block)b), written, lines, longest, ascii);
		}
	}
	Outcome ok;
	ok.status = Status::READY;
	CHECK(user_message(ok, prod).empty(), "no message when ready");
}
} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <fresh fixture dir>\n", argv[0]);
		return 2;
	}
	BASE = argv[1];
	if (exists(BASE)) {
		fprintf(stderr, "refusing to reuse %s (no deletion): pass a fresh directory\n", BASE.c_str());
		return 2;
	}
	mkdirs(BASE);
	test_marker_format();
	test_fresh();
	test_migrate_full();
	test_rollback_reads();
	test_foreign_new_root();
	test_partial_and_interruption();
	test_space();
	test_io_faults();
	test_source_refusals();
	test_publish_collision();
	test_large_file();
	test_messages();
	printf("data_root: %d checks passed, %d failed\n", passed, failed);
	return failed ? 1 : 0;
}
