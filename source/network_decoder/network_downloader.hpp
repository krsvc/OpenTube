#pragma once
#include <vector>
#include <map>
#include <string>
#include <3ds.h>
#include <cstdio>
#include "system/libctru_wrapper.hpp"
#include "network_io.hpp"
#include "playback_diag.hpp"

// one instance per one url (once constructed, the url is not changeable)
struct NetworkStream {
	static constexpr u64 BLOCK_SIZE = 0x40000; // 256 KiB
	static constexpr u64 NEW3DS_MAX_CACHE_BLOCKS = 12 * 1000 * 1000 / BLOCK_SIZE;
	static constexpr u64 OLD3DS_MAX_CACHE_BLOCKS = 4 * 1000 * 1000 / BLOCK_SIZE;
	static constexpr int RETRY_CNT_MAX = 1;
	static u64 get_block_num(u64 size) { return (size + BLOCK_SIZE - 1) / BLOCK_SIZE; }

	std::string url;
	Mutex downloaded_data_lock; // std::map needs locking when searching and inserting at the same time
	u64 len = 0;
	u64 block_num = 0;
	std::map<u64, std::vector<u8>> downloaded_data;
	bool whole_download = false;
	NetworkSessionList *session_list = NULL;

	// anything above here is not supposed to be used from outside network_downloader.cpp and network_downloader.hpp
	volatile bool ready = false;
	volatile bool suspend_request = false;
	volatile bool quit_request = false;
	volatile bool error = false;
	volatile int retry_cnt_left = RETRY_CNT_MAX;
	volatile u64 read_head = 0;
	const char *volatile network_waiting_status = NULL;
	bool disable_interrupt = false;
	// used for livestreams
	int seq_head = -1;
	int seq_id = -1;
	bool livestream_eof = false;
	bool livestream_private = false;
	bool read_dead_tried = false;

	// --- playback diagnostics (see playback_diag.hpp) ---
	// Write-once first failure + seek / length metadata. Writers: downloader thread and the decoder
	// io callbacks; reader: decode thread (snapshot). Guarded by its own leaf lock (no IO inside),
	// so the std::string inside the record is never read while another thread writes it.
	struct EvidenceLock : playback_diag::Lock {
		Mutex m;
		void lock() override { m.lock(); }
		void unlock() override { m.unlock(); }
	};
	EvidenceLock evidence_lock;
	playback_diag::StreamEvidence evidence{evidence_lock};
	bool content_length_checked = false; // downloader thread only

	// Local-file backend (offline playback of an SD download, DOWNLOADS_OFFLINE_BRIEF.md): the whole content is
	// available from `local_file`, nothing is ever downloaded for this stream (the downloader thread skips it), and
	// reads go to the file under downloaded_data_lock. The object is owned like a network stream (deleted by its
	// NetworkStreamDownloader after quit_request), but the file itself is closed synchronously by close_local() on the
	// decoding thread when the decoder lets go of the stream, so the player's playback lease can end right after.
	bool local = false;
	FILE *local_file = NULL; // NULL once closed
	// NULL + *error when the file cannot be opened or its size differs from expected_len (truncated / replaced)
	static NetworkStream *open_local(const std::string &path, u64 expected_len, std::string *error);
	void close_local(); // idempotent; no-op for network streams
	~NetworkStream();

	// if `whole_download` is true, it will not use Range request but download the whole content at once (used for
	// livestreams)
	NetworkStream(std::string url, int64_t len, bool whole_download, NetworkSessionList *session_list)
	    : url(url), len(len < 0 ? 0 : len), block_num(get_block_num(this->len)), whole_download(whole_download),
	      session_list(session_list) {}
	NetworkStream(const NetworkStream &) = delete;
	NetworkStream &operator=(const NetworkStream &) = delete;

	double get_download_percentage();
	std::vector<double> get_buffering_progress_bar(int res_len);

	// check if the data of the current stream of range [start, start + size) is already downloaded and available
	bool is_data_available(u64 start, u64 size);

	// this function must only be called when is_data_available(start, size) returns true
	// returns the data of the stream of range [start, start + size)
	std::vector<u8> get_data(u64 start, u64 size);

	// this function is supposed to be called from NetworkStreamDownloader::*
	void set_data(u64 block, std::vector<u8> data);
};

// each instance of this class is paired with one downloader thread
// it owns NetworkStream instances, and the one with the least margin (as in proportion to the length of the entire
// stream) is the target of next downloading
class NetworkStreamDownloader {
  private:
	static constexpr u64 BLOCK_SIZE = NetworkStream::BLOCK_SIZE;
	static constexpr const char *USER_AGENT = "Mozilla/5.0 (Linux; Android 11; Pixel 3a) AppleWebKit/537.36 (KHTML, "
	                                          "like Gecko) Chrome/83.0.4103.101 Mobile Safari/537.36";

	Mutex streams_lock;
	std::vector<NetworkStream *> streams;

	bool thread_exit_requested = false;

  public:
	NetworkStreamDownloader() = default;

	// the pointer must be one that has been new-ed : it will be deleted once quit_request is made
	void add_stream(NetworkStream *stream);

	void request_thread_exit() { thread_exit_requested = true; }
	void delete_all();

	void downloader_thread();
};
// 'arg' should be a pointer to an instance of NetworkStreamDownloader
void network_downloader_thread(void *arg);
