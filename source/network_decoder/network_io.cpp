#include "headers.hpp"
#include "network_io.hpp"
#include "util/freeze_diag.hpp"
#include <deque>

#include <fcntl.h>

#include <sys/types.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static volatile bool exiting = false;

static std::vector<NetworkSessionList *> deinit_list;

void NetworkSessionList::init() {
	inited = true;
	deinit_list.push_back(this);
}
void NetworkSessionList::deinit() {
	inited = false;

	// curl cleanup
	if (curl_multi) {
		curl_multi_cleanup(curl_multi);
		curl_multi = NULL;
	}
}
void NetworkSessionList::init_owned() { inited = true; }
void NetworkSessionList::cleanup_owned() { deinit(); }
void NetworkSessionList::exit_request() { exiting = true; }
void NetworkSessionList::at_exit() {
	for (auto session_list : deinit_list) {
		session_list->deinit();
	}
	deinit_list.clear();
}

static std::string remove_leading_whitespaces(std::string str) {
	size_t i = 0;
	while (i < str.size() && str[i] == ' ') {
		i++;
	}
	return str.substr(i, str.size() - i);
}

// libcurl callback functions
static size_t curl_receive_data_callback_func(char *in_ptr, size_t, size_t len, void *user_data) {
	std::vector<u8> *out = (std::vector<u8> *)user_data;
	out->insert(out->end(), in_ptr, in_ptr + len);

	// Util_log_save("curl", "received : " + std::to_string(len));
	return len;
}
static size_t curl_receive_headers_callback_func(char *in_ptr, size_t, size_t len, void *user_data) {
	std::map<std::string, std::string> *out = (std::map<std::string, std::string> *)user_data;

	std::string cur_line = std::string(in_ptr, in_ptr + len);
	if (cur_line.size() && cur_line.back() == '\n') {
		cur_line.pop_back();
	}
	if (cur_line.size() && cur_line.back() == '\r') {
		cur_line.pop_back();
	}
	auto colon = std::find(cur_line.begin(), cur_line.end(), ':');
	if (colon == cur_line.end()) {
		// Util_log_save("curl", "unknown header line : " + cur_line);
	} else {
		std::string header_name = remove_leading_whitespaces(std::string(cur_line.begin(), colon));
		std::string header_content = remove_leading_whitespaces(std::string(colon + 1, cur_line.end()));
		// Util_log_save("curl", "header line : " + header_name + " : " + header_content);
		for (auto &c : header_name) {
			c = tolower(c);
		}
		(*out)[header_name] = header_content;
	}
	return len;
}
// body-sink requests (HttpRequest::body_sink): headers are collected per response (a followed redirect's headers
// are dropped at the next status line) and the body never touches NetworkResult::data
struct NetworkSessionList::SinkContext {
	CURL *curl = NULL;
	HttpBodySink *sink = NULL;
	std::map<std::string, std::string> headers;
	bool begun = false;
	bool accepted = false;
};
static size_t curl_receive_headers_sink_callback_func(char *in_ptr, size_t, size_t len, void *user_data) {
	NetworkSessionList::SinkContext *ctx = (NetworkSessionList::SinkContext *)user_data;
	if (len >= 5 && memcmp(in_ptr, "HTTP/", 5) == 0) {
		ctx->headers.clear(); // a new response (after a redirect / 1xx) starts
		return len;
	}
	return curl_receive_headers_callback_func(in_ptr, 1, len, &ctx->headers);
}
static size_t curl_receive_data_sink_callback_func(char *in_ptr, size_t, size_t len, void *user_data) {
	NetworkSessionList::SinkContext *ctx = (NetworkSessionList::SinkContext *)user_data;
	if (!ctx->begun) {
		long status_code = 0;
		curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &status_code);
		ctx->begun = true;
		ctx->accepted = ctx->sink->begin((int)status_code, ctx->headers);
	}
	if (!ctx->accepted || !ctx->sink->write((const u8 *)in_ptr, len)) {
		return len ? 0 : 1; // != len: libcurl aborts with CURLE_WRITE_ERROR
	}
	return len;
}
static int curl_set_socket_options(void *, curl_socket_t sockfd, curlsocktype purpose) {
	static const int SOCKET_BUFFER_MAX_SIZE = 0x8000;

	// expand socket buffer size
	setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &SOCKET_BUFFER_MAX_SIZE, sizeof(int));

	return CURL_SOCKOPT_OK;
}
static int curl_progress_callback_func(void *data, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                                       curl_off_t ulnow) {
	const HttpRequest *request = (const HttpRequest *)data;
	if (exiting) {
		return 1; // app exit: abort now (CURLE_ABORTED_BY_CALLBACK) instead of after the transfer
	}
	if (request->should_abort && request->should_abort()) {
		return 1; // owner cancelled / paused this request
	}
	if (request->progress_func) {
		request->progress_func(dlnow, dltotal);
	}
	return CURL_PROGRESSFUNC_CONTINUE;
}
static int curl_debug_callback_func(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr) {
	std::string prefix;
	if (type == CURLINFO_HEADER_OUT) {
		prefix = "h>";
	}
	if (type == CURLINFO_HEADER_IN) {
		prefix = "h<";
	}
	if (type == CURLINFO_DATA_OUT) {
		prefix = "d>";
	}
	if (type == CURLINFO_SSL_DATA_OUT) {
		prefix = "D>";
	}
	if (type == CURLINFO_DATA_IN) {
		prefix = "d<";
	}
	if (type == CURLINFO_SSL_DATA_IN) {
		prefix = "D<";
	}
	logger.info("curl", prefix + std::string(data, data + size));
	return 0;
}

void NetworkSessionList::curl_add_request(const HttpRequest &request, NetworkResult *res) {
	if (!curl_multi) {
		curl_multi = curl_multi_init();
		curl_multi_setopt(curl_multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
	}
	CURL *curl;
	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "br");
	curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive_data_callback_func);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_receive_headers_callback_func);
	curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, curl_set_socket_options);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &request); // the request outlives perform()
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback_func);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (long)0);
	// curl_easy_setopt(curl, CURLOPT_VERBOSE, (long) 1);
	// curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, curl_debug_callback_func);
	char *curl_errbuf = (char *)malloc(CURL_ERROR_SIZE);
	memset(curl_errbuf, 0, CURL_ERROR_SIZE);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
	curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
	// curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
	// no total timeout (big ranges on slow links are fine), but a dead or stalled connection must end
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, NETWORK_IO_CONNECT_TIMEOUT_S);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, NETWORK_IO_STALL_TIMEOUT_S);

	SinkContext *sink_ctx = NULL;
	if (request.body_sink) {
		sink_ctx = new SinkContext();
		sink_ctx->curl = curl;
		sink_ctx->sink = request.body_sink;
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_receive_data_sink_callback_func);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_receive_headers_sink_callback_func);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, sink_ctx);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, sink_ctx);
	} else {
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res->data);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res->response_headers);
	}
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, (long)request.follow_redirect);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
	curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());

	struct curl_slist *request_headers_list = NULL;
	for (auto i : request.headers) {
		request_headers_list = curl_slist_append(request_headers_list, (i.first + ": " + i.second).c_str());
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers_list);

	curl_multi_add_handle(curl_multi, curl);
	curl_requests.push_back({curl, res, curl_errbuf, request.url, request.on_finish, sink_ctx});
}
CURLMcode NetworkSessionList::curl_perform_requests() {
	auto read_multi_info = [this]() {
		CURLMsg *msg;
		int msg_left;
		while ((msg = curl_multi_info_read(curl_multi, &msg_left))) {
			if (msg->msg == CURLMSG_DONE) {
				CURL *curl = msg->easy_handle;

				int request_index = -1;
				for (size_t i = 0; i < curl_requests.size(); i++) {
					if (curl_requests[i].curl == msg->easy_handle) {
						request_index = i;
						break;
					}
				}
				// Util_log_save("bench", "finished #" + std::to_string(request_index));

				if (request_index == -1) {
					logger.error("curl", "unexpected : while processing multi message corresponding request not found");
					continue;
				}
				RequestInternal &req = curl_requests[request_index];
				NetworkResult &res = *req.res;

				CURLcode each_result = msg->data.result;
				if (each_result == CURLE_OK) {
					long status_code;
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
					res.status_code = status_code;

					char *redirected_url;
					curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &redirected_url);
					res.redirected_url = redirected_url;
					if (req.sink_ctx) {
						res.response_headers = req.sink_ctx->headers; // final response only
					} else if (res.redirected_url != req.orig_url) { // (sink requests carry signed urls: not logged)
						logger.info("curl", "redir : " + res.redirected_url);
					}
				} else {
					logger.error("curl",
					             std::string("deep fail : ") + curl_easy_strerror(each_result) + " / " + req.errbuf);
					res.fail = true;
					res.error = req.errbuf;
				}
				if (req.on_finish) {
					req.on_finish(res, request_index);
				}
			}
		}
	};

	int running_request_num;
	freeze_diag::counter_reset(); // diagnostic: loop iterations of this perform (a spinning loop shows up here)
	do {
		freeze_diag::counter_bump();
		CURLMcode res = curl_multi_perform(curl_multi, &running_request_num);
		if (res) {
			std::string err = curl_multi_strerror(res);
			logger.error("curl", "curl multi deep fail : " + err);
			for (auto &i : curl_requests) {
				i.res->fail = true;
				i.res->error = err;
			}
			return res;
		}
		if (running_request_num) {
			curl_multi_poll(curl_multi, NULL, 0, 1000, NULL); // short: an exit request is noticed within ~1 s
		}
		if (exiting) {
			for (auto &i : curl_requests) {
				i.res->fail = true;
				i.res->error = "The app is exiting";
			}
			return CURLM_OK;
		}
		read_multi_info();
	} while (running_request_num > 0);

	return CURLM_OK;
}
void NetworkSessionList::curl_clear_requests() {
	for (auto &i : curl_requests) {
		// libcurl may still write CURLOPT_ERRORBUFFER while an unfinished transfer is removed (e.g. "Callback
		// aborted" on exit): the buffer must outlive curl_easy_cleanup()
		curl_multi_remove_handle(curl_multi, i.curl);
		curl_easy_cleanup(i.curl);
		free(i.errbuf);
		delete i.sink_ctx;
	}
	curl_requests.clear();
}

NetworkResult NetworkSessionList::perform(const HttpRequest &request) {
	NetworkResult result;

	if (!this->inited) {
		result.fail = true;
		result.error = "invalid session list";
		return result;
	}

	const char *prev_phase = freeze_diag::current_phase();
	freeze_diag::phase("net:perform");
	this->curl_add_request(request, &result);
	this->curl_perform_requests();
	this->curl_clear_requests();
	freeze_diag::phase(prev_phase);
	return result;
}
std::vector<NetworkResult> NetworkSessionList::perform(const std::vector<HttpRequest> &requests) {
	std::vector<NetworkResult> results(requests.size());

	if (!this->inited) {
		for (auto result : results) {
			result.fail = true;
			result.error = "invalid session list";
		}
		return results;
	}

	const char *prev_phase = freeze_diag::current_phase();
	freeze_diag::phase("net:perform_multi");
	for (size_t i = 0; i < requests.size(); i++) {
		this->curl_add_request(requests[i], &results[i]);
	}
	this->curl_perform_requests();
	this->curl_clear_requests();
	freeze_diag::phase(prev_phase);
	return results;
}

std::string NetworkResult::get_header(std::string key) {
	for (auto &c : key) {
		c = tolower(c);
	}
	return response_headers.count(key) ? response_headers[key] : "";
}

static bool exclusive_state_entered = false;
void lock_network_state() {
	int res = 0;
	if (!exclusive_state_entered) {
		res = ndmuInit();
		if (res != 0) {
			logger.error("init", "ndmuInit(): " + std::to_string(res));
		}
		res = NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
		if (R_SUCCEEDED(res)) {
			res = NDMU_LockState(); // prevents ndm from switching to StreetPass when the lid is closed
		}
		exclusive_state_entered = R_SUCCEEDED(res);
	}
}
void unlock_network_state() {
	int res = 0;
	if (exclusive_state_entered) {
		res = NDMU_UnlockState();
		if (R_SUCCEEDED(res)) {
			res = NDMU_LeaveExclusiveState();
		}
		ndmuExit();
		exclusive_state_entered = R_FAILED(res);
	}
}
