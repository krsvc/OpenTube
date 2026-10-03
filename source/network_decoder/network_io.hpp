#pragma once
#include <vector>
#include <map>
#include <string>
#include <functional>
#include <3ds.h>
#include <curl/curl.h>

struct NetworkResult {
	std::string redirected_url;
	bool fail =
	    false; // whether some network error occurred; receiving http error code like 404 is still counted as a 'success'
	std::string error;
	int status_code = -1;
	std::string status_message;
	std::vector<u8> data;
	std::map<std::string, std::string> response_headers;

	bool status_code_is_success() { return status_code / 100 == 2; }
	std::string get_header(std::string key);
};
// Streams a response body to its consumer in bounded pieces instead of NetworkResult::data (SD downloads).
// Only the final response of a request (after followed redirects) reaches begin(); its headers are those of that
// response alone. A false return from either call aborts the transfer (NetworkResult::fail, CURLE_WRITE_ERROR).
struct HttpBodySink {
	virtual ~HttpBodySink() {}
	// before the first body byte: status and lower-case headers of the final response
	virtual bool begin(int status_code, const std::map<std::string, std::string> &headers) = 0;
	virtual bool write(const u8 *data, size_t len) = 0;
};
struct HttpRequest { // including https
	std::string method;
	std::string url;
	std::map<std::string, std::string> headers;
	std::string body;
	bool follow_redirect; // ignored when method == POST (never follow redirects when posting)

	using progress_callback_t = std::function<void(u64, u64)>;
	progress_callback_t progress_func{};
	using on_finish_callback_t = std::function<void(NetworkResult &, int)>;
	on_finish_callback_t on_finish{};
	// optional: body goes to this sink (NetworkResult::data stays empty) and the url is never logged
	HttpBodySink *body_sink = NULL;
	// optional: polled from the libcurl progress callback (at least about once per second); true aborts the request
	std::function<bool()> should_abort{};

	static std::map<std::string, std::string> default_headers_added(std::map<std::string, std::string> headers) {
		// Set up default Android/YouTube client headers
		static const std::string DEFAULT_USER_AGENT = "Mozilla/5.0 (Linux; Android 11; Pixel 3a) AppleWebKit/537.36 "
		                                              "(KHTML, like Gecko) Chrome/83.0.4103.101 Mobile Safari/537.36";
		static const std::map<std::string, std::string> default_headers = {
		    {"Accept", "*/*"}, {"Connection", "Keep-Alive"}, {"User-Agent", DEFAULT_USER_AGENT}};
		for (auto default_header : default_headers) {
			if (!headers.count(default_header.first)) {
				headers[default_header.first] = default_header.second;
			}
		}

		return headers;
	}

	static HttpRequest GET(const std::string &url, const std::map<std::string, std::string> &headers,
	                       bool follow_redirect = true) {
		auto updated_headers = default_headers_added(headers);
		return HttpRequest{"GET", url, updated_headers, "", follow_redirect};
	}

	static HttpRequest POST(const std::string &url, const std::map<std::string, std::string> &headers,
	                        const std::string &body) {
		auto updated_headers = default_headers_added(headers);
		return HttpRequest{"POST", url, updated_headers, body, false};
	}

	HttpRequest with_progress_func(progress_callback_t progress_func) const {
		HttpRequest res = *this;
		res.progress_func = progress_func;
		return res;
	}

	HttpRequest with_on_finish_callback(on_finish_callback_t on_finish) const {
		HttpRequest res = *this;
		res.on_finish = on_finish;
		return res;
	}
};

// Bounds for every libcurl request (there was no timeout at all): a dead connection used to block the calling
// thread (downloader / page loader / thumbnail loader) forever. Overridable only for the host tests.
#ifndef NETWORK_IO_CONNECT_TIMEOUT_S
#define NETWORK_IO_CONNECT_TIMEOUT_S 20L // TCP/TLS connect
#endif
#ifndef NETWORK_IO_STALL_TIMEOUT_S
#define NETWORK_IO_STALL_TIMEOUT_S 30L // abort if less than 1 byte/s arrives for this long
#endif

struct NetworkSessionList { // one instance per thread
  private:
	void deinit(); // will be called for each instance when the app exits

	void curl_add_request(const HttpRequest &request, NetworkResult *res);
	CURLMcode curl_perform_requests();
	void curl_clear_requests();

  public:
	// used for libcurl
	CURLM *curl_multi = NULL; // curl manages sessions within a single CURL *
	struct SinkContext; // body-sink state of one request (network_io.cpp)
	struct RequestInternal {
		CURL *curl;
		NetworkResult *res;
		char *errbuf;
		std::string orig_url;
		HttpRequest::on_finish_callback_t on_finish;
		SinkContext *sink_ctx;
	};
	std::vector<RequestInternal> curl_requests; // {curl context, corresponding result, error buffer}

	volatile bool inited = false;

	// this function does NOT perform any network/socket related operations
	void init();
	// For a list used by one thread for its whole life (SD download worker): not registered for at_exit(), so no
	// other thread can free its handles while that thread may still be inside perform() (e.g. a blocking DNS
	// lookup); the owning thread calls cleanup_owned() after its last perform().
	void init_owned();
	void cleanup_owned();
	void close_sessions();

	// network operations
	NetworkResult perform(const HttpRequest &request);
	std::vector<NetworkResult> perform(const std::vector<HttpRequest> &requests);

	static void at_exit();
	static void exit_request();
};

void lock_network_state();
void unlock_network_state();

#define HTTP_STATUS_CODE_OK 200
#define HTTP_STATUS_CODE_NO_CONTENT 204
#define HTTP_STATUS_CODE_PARTIAL_CONTENT 206
#define HTTP_STATUS_CODE_FORBIDDEN 403
#define HTTP_STATUS_CODE_NOT_FOUND 404
