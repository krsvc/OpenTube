#pragma once
// One media stream -> one SD file, in bounded HTTP chunks streamed straight into the file (DOWNLOADS_OFFLINE_BRIEF.md).
// Memory per transfer: libcurl's receive buffer plus the file's 64 KiB stdio buffer; the body never lands in
// NetworkResult::data. Every chunk is validated (status, content type kind, Content-Range / Content-Length, exact
// byte count) before the next one starts; a failed chunk is rewritten from its first byte, never appended to.
#include <cstdint>
#include <functional>
#include <string>
#include "network_decoder/network_io.hpp"
#include "downloads/download_store.hpp"

namespace downloads {

constexpr uint64_t CHUNK_BYTES = 512 * 1024;
constexpr int MAX_CHUNK_ATTEMPTS = 3;               // explicit bound: 1 try + 2 retries per chunk
constexpr uint64_t SPACE_MARGIN_BYTES = 1024 * 1024; // kept free on the SD card

enum class StopKind { NONE, PAUSE, CANCEL, EXIT };
struct TransferControl {
	virtual ~TransferControl() {}
	virtual StopKind stop_requested() = 0; // polled from the libcurl progress callback and between chunks
	virtual StopKind wait_while_paused() = 0; // blocks (sleeping) while paused; NONE = run again, else CANCEL/EXIT
	virtual void progress(uint64_t stream_done, uint64_t stream_total) = 0;
};

enum class TransferError {
	NONE,
	CANCELLED,
	EXITING,
	OPEN_FAILED,
	NETWORK,        // transport failure after the bounded retries
	HTTP_STATUS,    // non-2xx final response (error bodies are never written)
	BAD_RESPONSE,   // wrong content type / Content-Range / Content-Length / more bytes than requested
	LENGTH_UNKNOWN, // no usable total length
	TOO_LARGE,      // beyond MAX_FILE_BYTES
	DISK_FULL,
	WRITE_FAILED,
	CLOSE_FAILED,   // flush / sync / close failed: data may not be on the card
	SIZE_MISMATCH,  // short body after the bounded retries, or final file size wrong
};
const char *transfer_error_text(TransferError error);

struct StreamPlan {
	std::string url;       // signed: memory only, never logged or persisted
	uint64_t expected_len; // 0 = unknown (then learned from Content-Range)
	const char *kind;      // "video" or "audio": expected content type prefix
	std::string path;      // destination (app-owned file name inside the item folder)
};

struct TransferResult {
	TransferError error = TransferError::NONE;
	int http_status = 0;
	uint64_t bytes = 0; // bytes of this stream confirmed on the card
	uint64_t total = 0;
	std::string detail;
	// instrumentation for the host tests (bounded-memory evidence)
	size_t max_piece = 0;        // largest single write handed to the file
	size_t max_result_data = 0;  // largest NetworkResult::data seen (must stay 0)
	int requests = 0;
};

TransferResult transfer_stream(NetworkSessionList &session, DlFs &fs, const StreamPlan &plan,
                               TransferControl &control, uint64_t chunk_bytes = CHUNK_BYTES);

// OpenTube r14: one small body (a thumbnail) into memory, never onto the card. One GET, no retry (the caller bounds the
// attempts): 200 only, content type `type` (prefix), at most max_bytes (Content-Length and received bytes), and the
// announced length must match. should_abort is polled from the libcurl progress callback.
enum class FetchError { NONE, ABORTED, NETWORK, HTTP_STATUS, BAD_RESPONSE, TOO_LARGE };
struct FetchResult {
	FetchError error = FetchError::NONE;
	int http_status = 0;
	std::string detail;
};
FetchResult fetch_small(NetworkSessionList &session, const std::string &url, const char *type, size_t max_bytes,
                        const std::function<bool()> &should_abort, std::string *out);

// helpers (exposed for tests)
bool parse_content_range(const std::string &value, uint64_t *first, uint64_t *last, uint64_t *total, bool *total_known);
std::string strip_url_parameter(const std::string &url, const std::string &param);

} // namespace downloads
