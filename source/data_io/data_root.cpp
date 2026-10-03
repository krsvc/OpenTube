#include "data_io/data_root.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __3DS__
#include <3ds.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

namespace data_root {

const char *const MARKER_NAME = "opentube_data_root.txt";
const char *const SKIPPED_TOP_DIR = "update";

namespace {
std::string no_slash(const std::string &path) {
	std::string p = path;
	while (p.size() > 1 && p.back() == '/') {
		p.pop_back();
	}
	return p;
}
std::string with_slash(const std::string &path) { return path.empty() || path.back() == '/' ? path : path + "/"; }
std::string parent_of(const std::string &path) {
	std::string p = no_slash(path);
	size_t slash = p.rfind('/');
	return slash == std::string::npos || slash == 0 ? "/" : p.substr(0, slash);
}
std::string errno_text(int err) { return "errno " + std::to_string(err); }

// ------------------------------------------------------------------ POSIX implementation
struct PosixIn : InFile {
	int fd;
	int err = 0;
	explicit PosixIn(int fd) : fd(fd) {}
	~PosixIn() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	long read(void *buf, size_t len) override {
		while (true) {
			errno = 0;
			ssize_t n = ::read(fd, buf, len);
			if (n >= 0) {
				return (long)n;
			}
			if (errno != EINTR) {
				err = errno ? errno : EIO;
				return -1;
			}
		}
	}
	int close() override {
		errno = 0;
		int r = ::close(fd);
		fd = -1;
		return r == 0 ? 0 : (err = errno ? errno : EIO);
	}
	int error() override { return err; }
};
struct PosixWrite : OutFile {
	int fd;
	int err = 0;
	explicit PosixWrite(int fd) : fd(fd) {}
	~PosixWrite() {
		if (fd >= 0) {
			::close(fd);
		}
	}
	long write(const void *buf, size_t len) override {
		size_t done = 0;
		while (done < len) {
			errno = 0;
			ssize_t n = ::write(fd, (const char *)buf + done, len - done);
			if (n < 0 && errno == EINTR) {
				continue;
			}
			if (n <= 0) { // a zero-byte write is an error, never retried in a loop
				err = errno ? errno : EIO;
				return (long)done;
			}
			done += (size_t)n;
		}
		return (long)done;
	}
	int sync() override {
		errno = 0;
		return fsync(fd) == 0 ? 0 : (err = errno ? errno : EIO); // sdmc: FSFILE_Flush
	}
	int close() override {
		errno = 0;
		int r = ::close(fd);
		fd = -1;
		return r == 0 ? 0 : (err = errno ? errno : EIO);
	}
	int error() override { return err; }
};
struct PosixRootFs : RootFs {
	Probe probe(const std::string &path) override {
		Probe res;
		struct stat st;
		errno = 0;
#ifdef __3DS__
		int r = stat(no_slash(path).c_str(), &st); // the sdmc devoptab's lstat is its stat: FAT has no links
#else
		int r = lstat(no_slash(path).c_str(), &st);
#endif
		if (r != 0) {
			res.err = errno ? errno : EIO;
			res.kind = res.err == ENOENT ? Probe::ABSENT : Probe::ERROR;
			return res;
		}
		res.kind = S_ISDIR(st.st_mode) ? Probe::DIR : S_ISREG(st.st_mode) ? Probe::FILE : Probe::OTHER;
		res.size = st.st_size < 0 ? UINT64_MAX : (uint64_t)st.st_size; // a negative 32-bit size is refused later
		return res;
	}
	int list_dir(const std::string &path, std::vector<std::string> *names) override {
		errno = 0;
		DIR *dir = opendir(no_slash(path).c_str());
		if (!dir) {
			return errno ? errno : EIO;
		}
		names->clear();
		int err = 0;
		while (true) {
			errno = 0;
			struct dirent *item = readdir(dir);
			if (!item) {
				err = errno;
				break;
			}
			std::string name = item->d_name;
			if (name != "." && name != "..") {
				names->push_back(name);
			}
		}
		closedir(dir);
		return err;
	}
	int make_dir(const std::string &path) override {
		errno = 0;
		return mkdir(no_slash(path).c_str(), 0777) == 0 ? 0 : (errno ? errno : EIO);
	}
	InFile *open_read(const std::string &path, int *err) override {
		int flags = O_RDONLY;
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif
		errno = 0;
		int fd = open(path.c_str(), flags);
		if (fd < 0) {
			*err = errno ? errno : EIO;
			return NULL;
		}
		return new PosixIn(fd);
	}
	OutFile *create_new(const std::string &path, int *err) override {
		int flags = O_WRONLY | O_CREAT | O_EXCL; // sdmc: FSUSER_CreateFile first, which fails if anything exists
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif
		errno = 0;
		int fd = open(path.c_str(), flags, 0666);
		if (fd < 0) {
			*err = errno ? errno : EIO;
			return NULL;
		}
		return new PosixWrite(fd);
	}
	int rename_dir_noreplace(const std::string &from, const std::string &to) override {
		std::string a = no_slash(from), b = no_slash(to);
#ifdef __3DS__
		// not newlib's rename(): libctru's archive_rename deletes an existing destination and retries
		FS_Archive sdmc;
		Result rc = FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
		if (R_FAILED(rc)) {
			return EIO;
		}
		rc = FSUSER_RenameDirectory(sdmc, fsMakePath(PATH_ASCII, a.c_str()), sdmc, fsMakePath(PATH_ASCII, b.c_str()));
		FSUSER_CloseArchive(sdmc);
		if (R_FAILED(rc)) {
			return (rc & 0x3FF) == 0x3FC ? EEXIST : EIO; // description 1020: already exists
		}
		return 0;
#elif defined(__APPLE__)
		errno = 0;
		return renamex_np(a.c_str(), b.c_str(), RENAME_EXCL) == 0 ? 0 : (errno ? errno : EIO);
#elif defined(__linux__) && defined(SYS_renameat2)
		errno = 0;
		return syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), 1 /* RENAME_NOREPLACE */) == 0
		           ? 0
		           : (errno ? errno : EIO);
#else
#error "data_root: no non-replacing directory rename on this platform"
#endif
	}
	int space(const std::string &path, uint64_t *free_bytes, uint64_t *cluster_bytes) override {
		struct statvfs st;
		errno = 0;
		if (statvfs(no_slash(path).c_str(), &st) != 0) {
			return errno ? errno : EIO;
		}
		uint64_t unit = st.f_frsize ? st.f_frsize : st.f_bsize; // sdmc: both are the cluster size
		*free_bytes = (uint64_t)st.f_bavail * unit;
		*cluster_bytes = unit;
		return 0;
	}
};

// ------------------------------------------------------------------ marker
uint32_t fnv1a(const std::string &s) {
	uint32_t h = 2166136261u;
	for (unsigned char c : s) {
		h = (h ^ c) * 16777619u;
	}
	return h;
}
bool parse_decimal(const std::string &s, uint64_t *out) {
	if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0')) {
		return false;
	}
	uint64_t v = 0;
	for (char c : s) {
		if (c < '0' || c > '9') {
			return false;
		}
		v = v * 10 + (uint64_t)(c - '0');
	}
	*out = v;
	return true;
}

// ------------------------------------------------------------------ admission helpers
struct Entry {
	std::string rel; // relative to the root; directories end with '/'
	bool dir = false;
	uint64_t size = 0;
};

Outcome blocked(Block block, const std::string &detail) {
	Outcome o;
	o.status = Status::BLOCKED;
	o.block = block;
	o.detail = detail;
	return o;
}

// printable-ASCII rendering of a (relative) name for the log and the failure screen
std::string printable(const std::string &s, size_t max_len) {
	std::string res;
	for (unsigned char c : s) {
		if (c >= 0x20 && c < 0x7F) {
			res += (char)c;
		} else {
			char buf[8];
			snprintf(buf, sizeof(buf), "\\x%02X", c);
			res += buf;
		}
	}
	if (res.size() > max_len) {
		res = "..." + res.substr(res.size() - (max_len - 3));
	}
	return res;
}

bool valid_component(const std::string &name) {
	if (name.empty() || name.size() > 255 || name == "." || name == "..") {
		return false;
	}
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c)) {
			return false;
		}
	}
	return name.back() != ' ' && name.back() != '.'; // FAT drops them: two names could become one
}

// marker of `root`: 1 = valid (out set), 0 = absent or invalid, -errno = could not be checked
int read_marker(RootFs &fs, const std::string &root, Marker *out) {
	std::string path = root + MARKER_NAME;
	Probe p = fs.probe(path);
	if (p.kind == Probe::ERROR) {
		return -p.err;
	}
	if (p.kind != Probe::FILE || p.size > MAX_MARKER_BYTES) {
		return 0;
	}
	int err = 0;
	InFile *in = fs.open_read(path, &err);
	if (!in) {
		return -(err ? err : EIO);
	}
	char buf[MAX_MARKER_BYTES + 1];
	size_t len = 0;
	bool ok = true;
	while (len < sizeof(buf)) {
		long n = in->read(buf + len, sizeof(buf) - len);
		if (n < 0) {
			ok = false;
			err = in->error();
			break;
		}
		if (n == 0) {
			break;
		}
		len += (size_t)n;
	}
	int close_err = in->close();
	delete in;
	if (!ok || close_err) {
		return -(err ? err : close_err ? close_err : EIO);
	}
	return len <= MAX_MARKER_BYTES && parse_marker(std::string(buf, len), out) ? 1 : 0;
}

// the marker in `dir`, exclusively created, synced, closed and read back
bool write_marker(RootFs &fs, const std::string &dir, const Marker &marker, std::string *why) {
	std::string text = format_marker(marker);
	int err = 0;
	OutFile *out = fs.create_new(dir + MARKER_NAME, &err);
	if (!out) {
		*why = std::string("create marker: ") + errno_text(err);
		return false;
	}
	bool ok = out->write(text.data(), text.size()) == (long)text.size();
	if (!ok) {
		*why = "write marker: " + errno_text(out->error());
	}
	if (ok && (err = out->sync()) != 0) {
		ok = false;
		*why = "sync marker: " + errno_text(err);
	}
	err = out->close();
	if (ok && err) {
		ok = false;
		*why = "close marker: " + errno_text(err);
	}
	delete out;
	if (!ok) {
		return false;
	}
	Marker back;
	int r = read_marker(fs, dir, &back);
	if (r != 1 || back.migrated != marker.migrated || back.files != marker.files || back.bytes != marker.bytes) {
		*why = r < 0 ? "read back marker: " + errno_text(-r) : "marker read back differs";
		return false;
	}
	return true;
}

struct Scan {
	RootFs &fs;
	const Layout &layout;
	const ProgressFn &progress;
	std::vector<Entry> entries;
	uint64_t files = 0, bytes = 0, dirs = 0;
	Outcome fail;
	bool cancelled() {
		Progress p;
		p.phase = Progress::SCANNING;
		p.done_files = files;
		if (progress && !progress(p)) {
			fail = blocked(Block::CANCELLED, "stopped while checking the old data");
			return true;
		}
		return false;
	}
	bool walk(const std::string &rel_dir, int depth) {
		if (cancelled()) {
			return false;
		}
		std::vector<std::string> names;
		int err = fs.list_dir(layout.old_root + rel_dir, &names);
		if (err) {
			fail = blocked(Block::SOURCE_UNREADABLE, "list /" + printable(rel_dir, 120) + ": " + errno_text(err));
			return false;
		}
		std::sort(names.begin(), names.end());
		for (const std::string &name : names) {
			std::string rel = rel_dir + name;
			if (!valid_component(name)) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "invalid name: " + printable(rel, 120));
				return false;
			}
			Probe p = fs.probe(layout.old_root + rel);
			if (rel_dir.empty() && name == SKIPPED_TOP_DIR && p.kind == Probe::DIR) {
				continue; // updater session staging: a stale .part would block every later update
			}
			if (rel_dir.empty() && name == MARKER_NAME) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "unexpected marker in the old root");
				return false;
			}
			if (p.kind == Probe::ABSENT || p.kind == Probe::ERROR) {
				fail = blocked(Block::SOURCE_UNREADABLE, "probe " + printable(rel, 120) + ": " + errno_text(p.err));
				return false;
			}
			if (p.kind == Probe::OTHER) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "link or special file: " + printable(rel, 120));
				return false;
			}
			if (layout.stage_root.size() + rel.size() + 1 > MAX_PATH_BYTES) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "path too long: " + printable(rel, 120));
				return false;
			}
			if (entries.size() >= MAX_ENTRIES) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "more than " + std::to_string(MAX_ENTRIES) + " entries");
				return false;
			}
			if (p.kind == Probe::FILE) {
				if (p.size > MAX_FILE_BYTES) {
					fail = blocked(Block::SOURCE_UNSUPPORTED, "file too large: " + printable(rel, 120));
					return false;
				}
				Entry e;
				e.rel = rel;
				e.size = p.size;
				entries.push_back(e);
				files++;
				bytes += p.size;
				continue;
			}
			if (depth + 1 > MAX_DEPTH) {
				fail = blocked(Block::SOURCE_UNSUPPORTED, "folders nested too deep: " + printable(rel, 120));
				return false;
			}
			Entry e;
			e.rel = rel + "/";
			e.dir = true;
			entries.push_back(e);
			dirs++;
			if (!walk(rel + "/", depth + 1)) {
				return false;
			}
		}
		return true;
	}
};

struct Copier {
	RootFs &fs;
	const Layout &layout;
	const ProgressFn &progress;
	char *buf;
	size_t chunk;
	Progress state;
	Outcome fail;
	bool report() {
		if (progress && !progress(state)) {
			fail = blocked(Block::CANCELLED, "stopped during the copy");
			return false;
		}
		return true;
	}
	bool copy_file(const Entry &e) {
		std::string src = layout.old_root + e.rel, dst = layout.stage_root + e.rel, rel = printable(e.rel, 120);
		Probe p = fs.probe(src);
		if (p.kind != Probe::FILE || p.size != e.size) {
			fail = blocked(Block::COPY_FAILED, "source changed: " + rel);
			return false;
		}
		int err = 0;
		InFile *in = fs.open_read(src, &err);
		if (!in) {
			fail = blocked(Block::COPY_FAILED, "open " + rel + ": " + errno_text(err));
			return false;
		}
		OutFile *out = fs.create_new(dst, &err);
		if (!out) {
			in->close();
			delete in;
			fail = blocked(Block::COPY_FAILED, "create " + rel + ": " + errno_text(err));
			return false;
		}
		uint64_t done = 0;
		bool ok = true;
		while (true) {
			long n = in->read(buf, chunk);
			if (n < 0) {
				ok = false;
				fail = blocked(Block::COPY_FAILED, "read " + rel + ": " + errno_text(in->error()));
				break;
			}
			if (n == 0) {
				break;
			}
			if (done + (uint64_t)n > e.size) {
				ok = false;
				fail = blocked(Block::COPY_FAILED, "source changed: " + rel);
				break;
			}
			if (out->write(buf, (size_t)n) != n) {
				ok = false;
				fail = blocked(Block::COPY_FAILED, "write " + rel + ": " + errno_text(out->error()));
				break;
			}
			done += (uint64_t)n;
			state.done_bytes += (uint64_t)n;
			if (!report()) {
				ok = false;
				break;
			}
		}
		if (ok && done != e.size) {
			ok = false;
			fail = blocked(Block::COPY_FAILED, "source changed: " + rel);
		}
		if (ok && (err = out->sync()) != 0) {
			ok = false;
			fail = blocked(Block::COPY_FAILED, "sync " + rel + ": " + errno_text(err));
		}
		err = out->close(); // always, exactly once
		if (ok && err) {
			ok = false;
			fail = blocked(Block::COPY_FAILED, "close " + rel + ": " + errno_text(err));
		}
		int in_err = in->close();
		if (ok && in_err) {
			ok = false;
			fail = blocked(Block::COPY_FAILED, "close source " + rel + ": " + errno_text(in_err));
		}
		delete out;
		delete in;
		if (!ok) {
			return false;
		}
		Probe q = fs.probe(dst);
		if (q.kind != Probe::FILE || q.size != e.size) {
			fail = blocked(Block::COPY_FAILED, "verify " + rel + ": size on the card differs");
			return false;
		}
		state.done_files++;
		return report();
	}
};

// publish `stage` (holding `marker`) as the new root
Outcome publish(RootFs &fs, const Layout &layout, const Marker &marker, Status status) {
	int err = fs.rename_dir_noreplace(layout.stage_root, layout.new_root);
	if (err) {
		return blocked(Block::PUBLISH_FAILED, "rename staging to the new root: " + errno_text(err));
	}
	Marker back;
	int r = read_marker(fs, layout.new_root, &back);
	if (r != 1 || back.files != marker.files || back.bytes != marker.bytes || back.migrated != marker.migrated) {
		return blocked(Block::PUBLISH_FAILED,
		               r < 0 ? "check the new root: " + errno_text(-r) : "new root marker differs");
	}
	Outcome o;
	o.status = status;
	o.total_files = o.copied_files = marker.files;
	o.total_bytes = o.copied_bytes = marker.bytes;
	return o;
}
} // namespace

RootFs &posix_root_fs() {
	static PosixRootFs fs;
	return fs;
}

std::string format_marker(const Marker &marker) {
	std::string body = std::string("OpenTube data root 1\n") + "origin=" + (marker.migrated ? "migrated" : "fresh") +
	                   "\nfiles=" + std::to_string(marker.files) + "\nbytes=" + std::to_string(marker.bytes) + "\n";
	char check[16];
	snprintf(check, sizeof(check), "%08x", (unsigned)fnv1a(body));
	return body + "check=" + check + "\n";
}

bool parse_marker(const std::string &text, Marker *out) {
	if (text.size() > MAX_MARKER_BYTES || text.empty() || text.back() != '\n') {
		return false;
	}
	std::vector<std::string> lines;
	size_t start = 0;
	while (start < text.size()) {
		size_t nl = text.find('\n', start);
		lines.push_back(text.substr(start, nl - start));
		start = nl + 1;
	}
	if (lines.size() != 5 || lines[0] != "OpenTube data root 1") {
		return false;
	}
	Marker m;
	if (lines[1] == "origin=migrated") {
		m.migrated = true;
	} else if (lines[1] != "origin=fresh") {
		return false;
	}
	if (lines[2].compare(0, 6, "files=") != 0 || !parse_decimal(lines[2].substr(6), &m.files) ||
	    lines[3].compare(0, 6, "bytes=") != 0 || !parse_decimal(lines[3].substr(6), &m.bytes) ||
	    lines[4].size() != 14 || lines[4].compare(0, 6, "check=") != 0) {
		return false;
	}
	for (size_t i = 6; i < 14; i++) {
		char c = lines[4][i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	char check[16];
	snprintf(check, sizeof(check), "%08x", (unsigned)fnv1a(text.substr(0, text.size() - 15)));
	if (lines[4].substr(6) != check) {
		return false;
	}
	*out = m;
	return true;
}

const char *block_name(Block block) {
	switch (block) {
	case Block::NONE:
		return "none";
	case Block::NEW_ROOT_FOREIGN:
		return "new root foreign";
	case Block::NEW_ROOT_UNREADABLE:
		return "new root unreadable";
	case Block::PARTIAL_LEFTOVER:
		return "partial leftover";
	case Block::STAGE_UNREADABLE:
		return "staging unreadable";
	case Block::SOURCE_UNREADABLE:
		return "source unreadable";
	case Block::SOURCE_UNSUPPORTED:
		return "source unsupported";
	case Block::SPACE_UNKNOWN:
		return "space unknown";
	case Block::NO_SPACE:
		return "no space";
	case Block::NO_MEMORY:
		return "no memory";
	case Block::COPY_FAILED:
		return "copy failed";
	case Block::PUBLISH_FAILED:
		return "publish failed";
	case Block::CANCELLED:
		return "cancelled";
	}
	return "?";
}

Outcome prepare(RootFs &fs, const Layout &in_layout, const ProgressFn &progress, size_t chunk_bytes) {
	Layout layout = {with_slash(in_layout.old_root), with_slash(in_layout.stage_root), with_slash(in_layout.new_root)};

	// 1. an established new root is authoritative: nothing else is looked at (no remigration)
	Probe root = fs.probe(layout.new_root);
	if (root.kind == Probe::ERROR) {
		return blocked(Block::NEW_ROOT_UNREADABLE, "probe new root: " + errno_text(root.err));
	}
	if (root.kind != Probe::ABSENT) {
		if (root.kind != Probe::DIR) {
			return blocked(Block::NEW_ROOT_FOREIGN, "new root is not a folder");
		}
		Marker m;
		int r = read_marker(fs, layout.new_root, &m);
		if (r < 0) {
			return blocked(Block::NEW_ROOT_UNREADABLE, "read new root marker: " + errno_text(-r));
		}
		if (r == 0) {
			return blocked(Block::NEW_ROOT_FOREIGN, "new root has no valid marker");
		}
		Outcome o;
		o.status = Status::READY;
		return o;
	}

	// 2. an earlier interrupted attempt is never reused, merged into or removed
	Probe stage = fs.probe(layout.stage_root);
	if (stage.kind == Probe::ERROR) {
		return blocked(Block::STAGE_UNREADABLE, "probe staging: " + errno_text(stage.err));
	}
	if (stage.kind != Probe::ABSENT) {
		return blocked(Block::PARTIAL_LEFTOVER, "staging folder exists");
	}

	// 3. the predecessor root: absent (ENOENT) is a fresh start, anything unreadable blocks
	Probe old = fs.probe(layout.old_root);
	if (old.kind == Probe::ERROR) {
		return blocked(Block::SOURCE_UNREADABLE, "probe old root: " + errno_text(old.err));
	}
	if (old.kind == Probe::FILE || old.kind == Probe::OTHER) {
		return blocked(Block::SOURCE_UNSUPPORTED, "old root is not a folder");
	}
	if (old.kind == Probe::ABSENT) {
		int err = fs.make_dir(parent_of(layout.stage_root));
		if (err && err != EEXIST) {
			return blocked(Block::PUBLISH_FAILED, "create parent folder: " + errno_text(err));
		}
		err = fs.make_dir(layout.stage_root);
		if (err) {
			return blocked(err == EEXIST ? Block::PARTIAL_LEFTOVER : Block::PUBLISH_FAILED,
			               "create staging: " + errno_text(err));
		}
		Marker m;
		std::string why;
		Outcome o = write_marker(fs, layout.stage_root, m, &why) ? publish(fs, layout, m, Status::FRESH)
		                                                         : blocked(Block::PUBLISH_FAILED, why);
		o.stage_written = !o.ready();
		return o;
	}

	// migrate: scan (read only), space, buffer, then the copy into the staging folder
	Scan scan{fs, layout, progress, {}, 0, 0, 0, Outcome()};
	if (!scan.walk("", 0)) {
		return scan.fail;
	}
	uint64_t free_bytes = 0, cluster = 0;
	int err = fs.space(parent_of(layout.stage_root), &free_bytes, &cluster);
	if (err) {
		return blocked(Block::SPACE_UNKNOWN, "free space: " + errno_text(err));
	}
	if (!cluster) {
		cluster = 32 * 1024;
	}
	uint64_t need = RESERVE_BYTES + (scan.dirs + 2) * cluster;
	for (const Entry &e : scan.entries) {
		need += e.dir ? 0 : (e.size + cluster - 1) / cluster * cluster;
	}
	if (free_bytes < need) {
		Outcome o =
		    blocked(Block::NO_SPACE, "need " + std::to_string(need) + " bytes, free " + std::to_string(free_bytes));
		o.need_bytes = need, o.free_bytes = free_bytes;
		o.total_bytes = scan.bytes, o.total_files = scan.files;
		return o;
	}
	if (chunk_bytes == 0) {
		chunk_bytes = DEFAULT_CHUNK_BYTES;
	}
	char *buf = (char *)malloc(chunk_bytes);
	if (!buf) {
		return blocked(Block::NO_MEMORY, "copy buffer");
	}

	Copier copier{fs, layout, progress, buf, chunk_bytes, Progress(), Outcome()};
	copier.state.phase = Progress::COPYING;
	copier.state.total_bytes = scan.bytes;
	copier.state.total_files = scan.files;
	bool stage_written = false;
	auto finish = [&](Outcome o) {
		free(buf);
		o.stage_written = stage_written && !o.ready();
		o.need_bytes = need, o.free_bytes = free_bytes;
		o.total_bytes = scan.bytes, o.total_files = scan.files;
		o.copied_bytes = copier.state.done_bytes, o.copied_files = copier.state.done_files;
		return o;
	};
	if (!copier.report()) {
		return finish(copier.fail); // nothing written yet
	}
	err = fs.make_dir(layout.stage_root);
	if (err) {
		return finish(blocked(err == EEXIST ? Block::PARTIAL_LEFTOVER : Block::COPY_FAILED,
		                      "create staging: " + errno_text(err)));
	}
	stage_written = true;
	for (const Entry &e : scan.entries) {
		if (e.dir) {
			err = fs.make_dir(layout.stage_root + e.rel);
			if (err) {
				return finish(
				    blocked(Block::COPY_FAILED, "create folder " + printable(e.rel, 120) + ": " + errno_text(err)));
			}
		} else if (!copier.copy_file(e)) {
			return finish(copier.fail);
		}
	}
	copier.state.phase = Progress::PUBLISHING;
	if (!copier.report()) {
		return finish(copier.fail);
	}
	Marker m;
	m.migrated = true;
	m.files = scan.files;
	m.bytes = scan.bytes;
	std::string why;
	if (!write_marker(fs, layout.stage_root, m, &why)) {
		return finish(blocked(Block::PUBLISH_FAILED, why));
	}
	return finish(publish(fs, layout, m, Status::MIGRATED));
}

std::string user_message(const Outcome &o, const Layout &layout) {
	if (o.ready()) {
		return "";
	}
	std::string old_dir = "sd:" + with_slash(layout.old_root), stage = "sd:" + with_slash(layout.stage_root),
	            root = "sd:" + with_slash(layout.new_root);
	std::string unchanged = "Your old data in " + old_dir + "\nwas not changed.";
	// never advise deleting: the folder name alone does not prove whose bytes are in it
	std::string move_stage = "On a computer, rename " + stage +
	                         "\n(e.g. to opentube.partial.old), keeping its contents."
	                         "\nThen start OpenTube again.";
	auto mb = [](uint64_t b) { return std::to_string((b + 1024 * 1024 - 1) / (1024 * 1024)); };
	std::string head = "OpenTube can't start: data folder problem.\n\n", body;
	switch (o.block) {
	case Block::NEW_ROOT_FOREIGN:
		body = root + " already exists, but OpenTube\ndid not create it (no valid " + MARKER_NAME +
		       ").\nOpenTube won't use, change or delete it.\n\nRename or move that folder on a computer,\nthen start "
		       "OpenTube again.\n" +
		       unchanged;
		break;
	case Block::NEW_ROOT_UNREADABLE:
	case Block::STAGE_UNREADABLE:
		body = "The SD card could not be read at\n" + (o.block == Block::STAGE_UNREADABLE ? stage : root) +
		       "\nNothing was changed.\n\nCheck the SD card on a computer, then try again.\n" + unchanged;
		break;
	case Block::PARTIAL_LEFTOVER:
		body = "A folder " + stage +
		       " exists\n(an interrupted copy, or something else).\nOpenTube won't use, "
		       "change or delete it.\n\n" +
		       move_stage + "\n" + unchanged;
		break;
	case Block::SOURCE_UNREADABLE:
		body = "Your old OpenTube data in " + old_dir +
		       "\ncould not be read, so it was not copied.\nNothing was "
		       "changed.\n\nCheck the SD card on a computer, then try again.";
		break;
	case Block::SOURCE_UNSUPPORTED:
		body = old_dir +
		       " contains something OpenTube\ncan't copy safely (a link, special file, bad name,\nor a too "
		       "deep / long path):\n" +
		       printable(o.detail, 52) +
		       "\nNothing was changed. Move that item out of the\n"
		       "folder on a computer, then try again.";
		break;
	case Block::SPACE_UNKNOWN:
		body =
		    "OpenTube couldn't check the free space on the\nSD card, so it didn't copy your data.\nNothing was changed."
		    "\n\nCheck the SD card on a computer, then try again.\n" +
		    unchanged;
		break;
	case Block::NO_SPACE:
		body = "Not enough free space to copy your data to\n" + root + "\nNeeded: " + mb(o.need_bytes) +
		       " MB, free: " + mb(o.free_bytes) + " MB.\n\nFree up space on the SD card, then try again.\n" + unchanged;
		break;
	case Block::NO_MEMORY:
		body = "Not enough memory to copy your data.\nNothing was changed.\n\nRestart the console and try again.\n" +
		       unchanged;
		break;
	case Block::COPY_FAILED:
	case Block::PUBLISH_FAILED:
		body = "Setting up " + root + " failed:\n" + printable(o.detail, 52) + "\n" + unchanged +
		       "\n\nCheck the SD card and its free space.\n" +
		       (o.stage_written ? move_stage : std::string("Then start OpenTube again."));
		break;
	case Block::CANCELLED:
		body = "The copy was stopped because OpenTube was closed.\n" + unchanged + "\n\n" +
		       (o.stage_written ? move_stage : std::string("Start OpenTube again to copy your data."));
		break;
	case Block::NONE:
		body = "Unknown problem.\n" + unchanged;
		break;
	}
	return head + body;
}

} // namespace data_root
