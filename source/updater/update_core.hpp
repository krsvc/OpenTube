#pragma once
// OpenTube r15 updater core (OPENTUBE_R15_PLAN.md B). Pure C++ (no 3DS headers): parsing, trust decisions, the CIA
// check and the check / download / install procedures over injected transport, staging file system, installer and
// environment, so every decision is host-tested with fakes. The 3DS glue (TLS transport, AM installer, worker thread,
// Settings UI) is update_app.cpp.
//
// Release contract (docs: OPENTUBE_R15_DELIVERY.md "Release publishing contract"):
//   - source: https://api.github.com/repos/krsvc/OpenTube/releases/latest only (GitHub never returns drafts or
//     prereleases there; draft / prerelease true is rejected anyway)
//   - tag: exactly v0.MINOR.MICRO (no leading zeros, MINOR 0..63, MICRO 0..15); the CIA's TMD and ticket title version
//     must be MINOR << 10 | MICRO (the packaging tool's major field carries MINOR; its minor field is not usable)
//   - assets named exactly OpenTube.cia, OpenTube.3dsx (optional), SHA256SUMS; browser_download_url must be
//     https://github.com/krsvc/OpenTube/releases/download/<tag>/<name>; state "uploaded"; bounded sizes
//   - SHA256SUMS: "<64 lowercase hex>  <name>" (or " *<name>") lines, OpenTube.cia mandatory
//   - the CIA: title id 000400000BF74E00 in ticket, TMD and NCCH, sizes consistent, == the API size, SHA-256 == SUMS
//     (and == the API "digest" when GitHub gives one)
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace updater {

// ---- versions ------------------------------------------------------------------------------------------------------
struct Version {
	int major = 0, minor = 0, micro = 0;
	bool operator==(const Version &o) const { return major == o.major && minor == o.minor && micro == o.micro; }
};
bool parse_tag(const std::string &tag, Version *out); // strict "vX.Y.Z", X <= 63, Y <= 63, Z <= 15
int compare(const Version &a, const Version &b);       // <0, 0, >0
std::string version_string(const Version &v);           // "0.15.0"
bool contract_installable(const Version &v);            // major 0 (the CIA title version contract below)
uint16_t cia_title_version(const Version &v);           // minor << 10 | micro

// ---- repository / assets ------------------------------------------------------------------------------------------
extern const char *const OWNER_REPO;   // "krsvc/OpenTube"
extern const char *const LATEST_URL;   // https://api.github.com/repos/krsvc/OpenTube/releases/latest
extern const char *const ASSET_CIA;    // "OpenTube.cia"
extern const char *const ASSET_3DSX;   // "OpenTube.3dsx"
extern const char *const ASSET_SUMS;   // "SHA256SUMS"
constexpr uint64_t TITLE_ID = 0x000400000BF74E00ULL;
constexpr uint64_t MAX_METADATA_BYTES = 256 * 1024;
constexpr uint64_t MAX_SUMS_BYTES = 4096;
constexpr uint64_t MAX_PACKAGE_BYTES = 32ULL * 1024 * 1024;
constexpr size_t MAX_NOTES_BYTES = 2000;
std::string expected_asset_url(const std::string &tag, const std::string &name);

struct Asset {
	bool present = false;
	std::string name, url;
	uint64_t size = 0;
	std::string sha256; // from the API "digest" ("" when GitHub gave none)
	bool operator==(const Asset &o) const {
		return present == o.present && name == o.name && url == o.url && size == o.size && sha256 == o.sha256;
	}
};
struct Release {
	int64_t id = 0;
	std::string tag;
	Version version;
	std::string title; // bounded, sanitized
	std::string notes; // bounded, sanitized
	Asset cia, tdsx, sums;
	// the same release, byte for byte, as far as the install depends on it (no retargeting after a confirmation)
	bool same_target(const Release &o) const {
		return id == o.id && tag == o.tag && cia == o.cia && tdsx == o.tdsx && sums == o.sums;
	}
};
std::string sanitize_text(const std::string &text, size_t max_bytes); // control chars (except \n) -> space, UTF-8 cut
bool parse_release(const std::string &json, Release *out, std::string *why);
// SHA256SUMS -> the lower-case hex digest listed for `name`
bool parse_sums(const std::string &text, const std::string &name, std::string *hex, std::string *why);

// ---- URL policy ---------------------------------------------------------------------------------------------------
bool split_https_url(const std::string &url, std::string *host, std::string *path); // strict, no userinfo / port
bool metadata_host(const std::string &host);        // api.github.com
bool asset_host(const std::string &host);           // github.com + GitHub's release asset CDN hosts
constexpr int MAX_REDIRECTS = 3;

// ---- SHA-256 ------------------------------------------------------------------------------------------------------
class Sha256 {
	uint32_t h[8];
	uint8_t buf[64];
	uint64_t total = 0;
	size_t used = 0;
	void block(const uint8_t *p);

  public:
	Sha256();
	void update(const void *data, size_t len);
	std::string hex(); // finishes; the object is spent afterwards
};
std::string sha256_hex(const std::string &data);

// ---- CIA check ----------------------------------------------------------------------------------------------------
constexpr size_t CIA_HEAD_BYTES = 0x8000; // header + certs + ticket + TMD + the NCCH header, for the sizes accepted
// head: the first bytes of the file (at least min(file_size, CIA_HEAD_BYTES)); checks structure, bounds, title ids
bool check_cia(const uint8_t *head, size_t head_len, uint64_t file_size, uint16_t want_title_version, std::string *why);

// ---- injected services --------------------------------------------------------------------------------------------
struct HttpResult {
	bool transport_error = false; // DNS / connect / TLS (incl. certificate or host name) / timeout / abort
	bool aborted = false;         // the abort callback said stop (cancel / app exit)
	bool too_large = false;       // the body passed max_bytes (transfer stopped)
	int status = 0;
	std::string location;         // Location header of a 3xx
	std::string rate_remaining;   // x-ratelimit-remaining ("" if absent)
	std::string error;            // human readable, never contains a URL query
};
struct Transport {
	virtual ~Transport() {}
	// one request, redirects NOT followed; the body of a 2xx goes to `sink` (false = stop, counts as an error)
	virtual HttpResult get(const std::string &url, uint64_t max_bytes,
	                       const std::function<bool(const uint8_t *, size_t)> &sink,
	                       const std::function<bool()> &abort) = 0;
};
// The staging file (fixed paths chosen by the glue); new_posix_stage_fs() in update_core.cpp. Ownership: the object
// only ever writes or removes a file it created itself, exclusively, during its own lifetime. Anything already at the
// path (a leftover of an earlier session, a user's file, a folder, a link) is refused and kept byte-for-byte.
struct StageFs {
	virtual ~StageFs() {}
	virtual bool ensure_dir(std::string *err) = 0; // the staging folder exists and is a real folder
	// creates the .part file exclusively (it must not exist, unless this object created it earlier: that one is
	// removed first); refuses anything else at the path without touching it
	virtual bool open_write(std::string *err) = 0;
	virtual bool write(const uint8_t *p, size_t n, std::string *err) = 0;
	virtual bool finish_write(std::string *err) = 0; // flush + fsync + close
	virtual bool open_read(uint64_t *size, std::string *err) = 0;
	virtual size_t read(uint8_t *p, size_t n, std::string *err) = 0; // 0 = end or error (err set)
	virtual void close_read() = 0;
	virtual void discard() = 0; // removes the file only if this object created it (and it was not replaced since)
};
StageFs *new_posix_stage_fs(const std::string &dir, const std::string &file); // dir must be app-owned
struct Installer {
	virtual ~Installer() {}
	virtual bool begin(std::string *err) = 0;
	// must write all n bytes; a short or zero write is an error (never retried in a loop)
	virtual bool write(uint64_t offset, const uint8_t *p, size_t n, std::string *err) = 0;
	virtual bool finish(std::string *err) = 0;
	virtual bool abort(std::string *err) = 0; // after begin, when not finishing
};
struct Environment {
	virtual ~Environment() {}
	virtual bool online() = 0;
	// this installation holds the activity gate's install reservation (util/activity_gate.hpp): no playback and no
	// download-library work can be admitted until it is released
	virtual bool install_reserved() = 0;
	virtual bool exit_requested() = 0; // app exit: stop before the system installer is started
	// runs start() (the system installer's begin) under the updater lock unless the app exit was requested first
	// (then *exit_refused and start() is not run). An exit request after this point waits for the installation.
	virtual bool start_guarded(const std::function<bool()> &start, bool *exit_refused) = 0;
};

// ---- procedures (worker thread, no lock held) ----------------------------------------------------------------------
enum class CheckOutcome { NO_RELEASE, UP_TO_DATE, AVAILABLE, NOT_INSTALLABLE, FAILED };
struct CheckResult {
	CheckOutcome outcome = CheckOutcome::FAILED;
	Release release;
	std::string message; // user facing
};
CheckResult check_latest(Transport &t, const Version &current, const std::function<bool()> &abort);

struct Progress {
	std::function<void(uint64_t done, uint64_t total)> report;
};
struct Staged {
	bool ok = false;
	Release release;       // the confirmed release, re-checked
	std::string sha256;    // verified digest of the staged file
	uint64_t size = 0;
	std::string message;
	bool cancelled = false;
};
// re-fetches the metadata (must be the same release as confirmed), SHA256SUMS, then the CIA into staging; the staged
// file is hashed while written, then read back and hashed again, and its structure checked
Staged download_and_verify(Transport &t, StageFs &fs, const Release &confirmed, const Version &current,
                           const std::function<bool()> &abort, const Progress &progress);

struct InstallResult {
	bool ok = false;
	bool refused = false;   // nothing was started (busy / staged file changed)
	bool aborted_ok = false; // the AM install was aborted cleanly (the installed app is unchanged)
	std::string message;
};
// streams the staged file into the installer, hashing again; finish() only when the digest still matches
InstallResult install_staged(StageFs &fs, Installer &inst, Environment &env, const Staged &staged,
                             const Progress &progress);

// ---- UI model (main thread under the glue's lock) -------------------------------------------------------------------
enum class State { IDLE, CHECKING, NO_RELEASE, UP_TO_DATE, AVAILABLE, NOT_INSTALLABLE, CHECK_FAILED, DOWNLOADING,
                   DOWNLOAD_FAILED, READY, INSTALLING, INSTALL_FAILED, INSTALLED };
const char *state_name(State s);
enum class Job { NONE, CHECK, DOWNLOAD, INSTALL };
class Model {
  public:
	State state = State::IDLE;
	Release release;   // what AVAILABLE / READY refer to
	Staged staged;     // READY
	std::string message;
	uint64_t done = 0, total = 0;
	Job job = Job::NONE;        // requested, not yet taken by the worker
	bool running = false;       // the worker is executing a job
	bool cancel_requested = false;
	bool auto_checked = false;  // the once-per-session check was already made (or a manual one)
	bool stopping = false;      // app exit requested: no request and no job claim any more
	bool install_started = false; // the system installer was started (runs to its end; the exit waits for it)

	// user actions; false + message when not allowed in this state
	bool request_check(bool automatic, std::string *msg);
	bool request_download(const Release &shown, std::string *msg); // confirm 1: binds exactly the shown release
	bool request_install(const std::string &shown_sha256, std::string *msg); // confirm 2
	bool request_cancel(std::string *msg); // download only
	bool can_cancel() const { return state == State::DOWNLOADING; }
	bool install_running() const { return state == State::INSTALLING; }
	// app exit (under the glue's lock, atomic with take_job): no job is claimed afterwards; a queued, unclaimed job is
	// dropped (a queued INSTALL goes back to READY, nothing started). Returns the dropped job.
	Job request_stop();
	// worker side
	Job take_job(); // marks running; NONE once stopping
	// the irrevocable start, under the glue's lock: false (start not run) once stopping; else runs start()
	bool start_install(const std::function<bool()> &start, bool *exit_refused);
	void check_done(const CheckResult &r);
	void download_done(const Staged &s);
	void install_done(const InstallResult &r);
};

// ---- worker loop (the glue's worker thread) ------------------------------------------------------------------------
// The exit request and the job claim are decided in ONE critical section of the glue's lock (Model::request_stop /
// take_job), so no job is claimed after Updater_request_stop(). Bounded exit after request_stop(): idle -> the next
// claim returns (one idle() nap); check / download -> their abort callbacks see the exit (transport abort poll);
// install before the system installer started -> exit_requested() / start_guarded() refuse; install after it
// started -> runs to its end: at most size / 64 KiB installer writes, each complete or an error (never retried), then
// finish or abort. Only the system calls themselves are outside this proof.
struct WorkerHost {
	virtual ~WorkerHost() {}
	virtual void lock() = 0;
	virtual void unlock() = 0;
	virtual Model &model() = 0;
	virtual void idle() = 0; // nothing to do: a short bounded nap
	virtual CheckResult check() = 0;
	virtual Staged download(const Release &release) = 0;
	virtual InstallResult install(const Staged &staged) = 0;
	virtual void job_finished(Job job) = 0; // after the model was updated (e.g. release the install reservation)
};
void worker_loop(WorkerHost &h); // returns once the exit was requested and no job is running

} // namespace updater
