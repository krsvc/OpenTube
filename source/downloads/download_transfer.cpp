#include "downloads/download_transfer.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace downloads {

const char *transfer_error_text(TransferError error) {
	switch (error) {
	case TransferError::NONE:
		return "ok";
	case TransferError::CANCELLED:
		return "cancelled";
	case TransferError::EXITING:
		return "interrupted by app exit";
	case TransferError::OPEN_FAILED:
		return "cannot create the file on the SD card";
	case TransferError::NETWORK:
		return "network error";
	case TransferError::HTTP_STATUS:
		return "server refused the request";
	case TransferError::BAD_RESPONSE:
		return "unexpected server response";
	case TransferError::LENGTH_UNKNOWN:
		return "stream length unknown";
	case TransferError::TOO_LARGE:
		return "file larger than 2 GiB (SD file limit)";
	case TransferError::DISK_FULL:
		return "SD card full";
	case TransferError::WRITE_FAILED:
		return "SD write failed";
	case TransferError::CLOSE_FAILED:
		return "SD flush/close failed";
	case TransferError::SIZE_MISMATCH:
		return "incomplete data";
	}
	return "?";
}

bool parse_content_range(const std::string &value, uint64_t *first, uint64_t *last, uint64_t *total, bool *total_known) {
	// "bytes <first>-<last>/<total|*>"
	if (value.compare(0, 6, "bytes ") != 0) {
		return false;
	}
	size_t dash = value.find('-', 6), slash = value.find('/', 6);
	if (dash == std::string::npos || slash == std::string::npos || dash > slash) {
		return false;
	}
	auto num = [](const std::string &s, uint64_t *out) {
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
		*out = v;
		return true;
	};
	if (!num(value.substr(6, dash - 6), first) || !num(value.substr(dash + 1, slash - dash - 1), last) ||
	    *last < *first) {
		return false;
	}
	std::string t = value.substr(slash + 1);
	if (t == "*") {
		*total_known = false;
		*total = 0;
		return true;
	}
	*total_known = true;
	return num(t, total) && *total > *last;
}

std::string strip_url_parameter(const std::string &url, const std::string &param) {
	std::string res;
	for (size_t i = 0; i < url.size();) {
		if ((url[i] == '&' || url[i] == '?') && url.compare(i + 1, param.size() + 1, param + "=") == 0) {
			size_t next = url.find('&', i + 1);
			if (url[i] == '?') {
				res.push_back('?');
				i = next == std::string::npos ? url.size() : next + 1;
			} else {
				i = next == std::string::npos ? url.size() : next;
			}
		} else {
			res.push_back(url[i++]);
		}
	}
	if (res.size() && (res.back() == '?' || res.back() == '&')) {
		res.pop_back();
	}
	return res;
}

namespace {
enum class Reject { NONE, STATUS, CONTENT_TYPE, RANGE, LENGTH, NO_LENGTH, TOO_MUCH, WRITE };

// validates one chunk response and streams its body into the file at the chunk offset
struct ChunkSink : HttpBodySink {
	DlFile *file;
	const char *kind;
	uint64_t pos;         // chunk start in the stream
	uint64_t want;        // bytes expected for this chunk (unknown length: the requested maximum)
	uint64_t known_total; // 0 = not yet known
	bool learn_length;    // first chunk of a stream with unknown length: take `want` / total from Content-Range

	int status = 0;
	bool begun = false;
	Reject reject = Reject::NONE;
	std::string reject_detail;
	uint64_t received = 0;
	uint64_t learned_total = 0;
	int write_errno = 0;
	size_t max_piece = 0;

	bool fail(Reject r, const std::string &detail) {
		reject = r;
		reject_detail = detail;
		return false;
	}
	bool begin(int status_code, const std::map<std::string, std::string> &headers) override {
		begun = true;
		status = status_code;
		if (status_code != 200 && status_code != 206) {
			return fail(Reject::STATUS, "HTTP " + std::to_string(status_code));
		}
		auto header = [&](const char *key) {
			auto it = headers.find(key);
			return it == headers.end() ? std::string() : it->second;
		};
		std::string type = header("content-type");
		for (auto &c : type) {
			c = tolower((unsigned char)c);
		}
		if (type.size() && type.compare(0, strlen(kind) + 1, std::string(kind) + "/") != 0 &&
		    type.compare(0, 24, "application/octet-stream") != 0) {
			return fail(Reject::CONTENT_TYPE, "content type " + type.substr(0, 40));
		}
		std::string range = header("content-range");
		if (range.size()) {
			uint64_t first = 0, last = 0, total = 0;
			bool total_known = false;
			if (!parse_content_range(range, &first, &last, &total, &total_known) || first != pos) {
				return fail(Reject::RANGE, "content range " + range.substr(0, 60));
			}
			if (learn_length) {
				if (!total_known) {
					return fail(Reject::NO_LENGTH, "no total length");
				}
				if (last - first + 1 > want) {
					return fail(Reject::TOO_MUCH, "range larger than requested");
				}
				want = last - first + 1;
				learned_total = total;
			} else if (last != pos + want - 1 || (total_known && total != known_total)) {
				return fail(Reject::RANGE, "content range " + range.substr(0, 60));
			}
		} else if (learn_length) {
			return fail(Reject::NO_LENGTH, "no Content-Range");
		}
		std::string length = header("content-length");
		if (length.size() && length != std::to_string(want)) {
			return fail(Reject::LENGTH, "content length " + length.substr(0, 20) + " != " + std::to_string(want));
		}
		return true;
	}
	bool write(const u8 *data, size_t len) override {
		if (received + len > want) {
			return fail(Reject::TOO_MUCH, "more bytes than requested (range not honoured)");
		}
		max_piece = std::max(max_piece, len);
		size_t n = file->write(data, len);
		if (n != len) {
			write_errno = file->error();
			return fail(Reject::WRITE, "short write");
		}
		received += len;
		return true;
	}
};

struct FileGuard {
	DlFile *f;
	~FileGuard() {
		if (f) {
			f->close(); // error path: the caller already reports the failure
			delete f;
		}
	}
};
} // namespace

TransferResult transfer_stream(NetworkSessionList &session, DlFs &fs, const StreamPlan &plan, TransferControl &control,
                               uint64_t chunk_bytes) {
	TransferResult res;
	res.total = plan.expected_len;
	if (plan.expected_len > MAX_FILE_BYTES) {
		res.error = TransferError::TOO_LARGE;
		return res;
	}
	FileGuard file = {fs.open_truncate(plan.path)};
	if (!file.f) {
		res.error = TransferError::OPEN_FAILED;
		res.detail = "errno " + std::to_string(fs.last_errno());
		return res;
	}
	std::string url = plan.url;
	uint64_t total = plan.expected_len;
	uint64_t pos = 0;
	const std::string dir = plan.path.substr(0, plan.path.rfind('/') + 1);

	auto stopped = [&](StopKind k) {
		res.error = k == StopKind::EXIT ? TransferError::EXITING : TransferError::CANCELLED;
		return res;
	};
	while (total == 0 || pos < total) {
		int attempts = 0;
		bool chunk_done = false;
		while (!chunk_done) {
			StopKind k = control.stop_requested();
			if (k == StopKind::PAUSE) {
				k = control.wait_while_paused();
			}
			if (k == StopKind::CANCEL || k == StopKind::EXIT) {
				return stopped(k);
			}
			uint64_t want = total ? std::min(chunk_bytes, total - pos) : chunk_bytes;
			if (pos + want > MAX_FILE_BYTES) {
				res.error = TransferError::TOO_LARGE;
				return res;
			}
			uint64_t free_now = fs.free_bytes(dir);
			if (free_now != std::numeric_limits<uint64_t>::max() && free_now < want + SPACE_MARGIN_BYTES) {
				res.error = TransferError::DISK_FULL;
				res.detail = std::to_string(free_now) + " bytes free";
				return res;
			}
			if (!file.f->seek(pos)) {
				res.error = TransferError::WRITE_FAILED;
				res.detail = "seek errno " + std::to_string(file.f->error());
				return res;
			}
			ChunkSink sink;
			sink.file = file.f;
			sink.kind = plan.kind;
			sink.pos = pos;
			sink.want = want;
			sink.known_total = total;
			sink.learn_length = total == 0;

			HttpRequest request =
			    total ? HttpRequest::GET(url + (url.find('?') == std::string::npos ? "?" : "&") + "range=" +
			                                 std::to_string(pos) + "-" + std::to_string(pos + want - 1),
			                             {})
			          : HttpRequest::GET(url, {{"Range", "bytes=" + std::to_string(pos) + "-" +
			                                                 std::to_string(pos + want - 1)}});
			request.body_sink = &sink;
			request.should_abort = [&control]() { return control.stop_requested() != StopKind::NONE; };
			NetworkResult result = session.perform(request);
			res.requests++;
			res.max_result_data = std::max(res.max_result_data, result.data.size());
			res.max_piece = std::max(res.max_piece, sink.max_piece);
			if (result.redirected_url.size() && !result.fail) {
				url = strip_url_parameter(result.redirected_url, "range"); // memory only
			}

			if (sink.reject == Reject::WRITE) {
				int e = sink.write_errno;
				res.error = e == ENOSPC ? TransferError::DISK_FULL
				            : e == EFBIG ? TransferError::TOO_LARGE
				                         : TransferError::WRITE_FAILED;
				res.detail = "errno " + std::to_string(e);
				return res;
			}
			k = control.stop_requested();
			if (k != StopKind::NONE) {
				continue; // pause: this chunk is fetched again from its first byte; cancel/exit: loop top returns
			}
			int status = sink.begun ? sink.status : result.status_code;
			res.http_status = status;
			if (sink.reject == Reject::STATUS || (!result.fail && !sink.begun && status / 100 != 2)) {
				if (status >= 500 && ++attempts < MAX_CHUNK_ATTEMPTS) {
					continue; // explicit bounded retry of a server-side failure
				}
				res.error = TransferError::HTTP_STATUS;
				res.detail = "HTTP " + std::to_string(status);
				return res;
			}
			if (sink.reject == Reject::NO_LENGTH) {
				res.error = TransferError::LENGTH_UNKNOWN;
				res.detail = sink.reject_detail;
				return res;
			}
			if (sink.reject != Reject::NONE) {
				res.error = TransferError::BAD_RESPONSE;
				res.detail = sink.reject_detail;
				return res;
			}
			if (result.fail || sink.received != sink.want || !sink.begun) {
				if (++attempts < MAX_CHUNK_ATTEMPTS) {
					continue; // transport failure / short body: rewrite this chunk from its first byte
				}
				res.error = result.fail ? TransferError::NETWORK : TransferError::SIZE_MISMATCH;
				res.detail = result.fail ? result.error.substr(0, 80)
				                         : std::to_string(sink.received) + " of " + std::to_string(sink.want) + " bytes";
				return res;
			}
			if (total == 0) {
				if (sink.learned_total > MAX_FILE_BYTES) {
					res.error = TransferError::TOO_LARGE;
					return res;
				}
				total = sink.learned_total;
				res.total = total;
			}
			pos += sink.want;
			res.bytes = pos;
			chunk_done = true;
			control.progress(pos, total);
		}
	}
	if (!file.f->sync()) {
		res.error = TransferError::CLOSE_FAILED;
		res.detail = "sync errno " + std::to_string(file.f->error());
		return res;
	}
	DlFile *f = file.f;
	file.f = NULL;
	bool closed = f->close();
	int close_errno = f->error();
	delete f;
	if (!closed) {
		res.error = close_errno == ENOSPC ? TransferError::DISK_FULL : TransferError::CLOSE_FAILED;
		res.detail = "close errno " + std::to_string(close_errno);
		return res;
	}
	DlStat st = fs.stat_path(plan.path);
	if (!st.exists || st.size != total) {
		res.error = TransferError::SIZE_MISMATCH;
		res.detail = "file has " + std::to_string(st.size) + " of " + std::to_string(total) + " bytes";
		return res;
	}
	res.bytes = total;
	return res;
}

namespace {
struct SmallSink : HttpBodySink {
	const char *type;
	size_t max_bytes;
	std::string *out;
	int status = 0;
	bool begun = false;
	FetchError reject = FetchError::NONE;
	std::string detail;
	std::string length; // announced Content-Length ("" = none)

	bool fail(FetchError e, const std::string &why) {
		reject = e;
		detail = why;
		return false;
	}
	bool begin(int status_code, const std::map<std::string, std::string> &headers) override {
		begun = true;
		status = status_code;
		if (status_code != 200) {
			return fail(FetchError::HTTP_STATUS, "HTTP " + std::to_string(status_code));
		}
		auto it = headers.find("content-type");
		std::string ct = it == headers.end() ? "" : it->second;
		for (auto &c : ct) {
			c = tolower((unsigned char)c);
		}
		if (ct.compare(0, strlen(type), type) != 0) {
			return fail(FetchError::BAD_RESPONSE, "content type " + ct.substr(0, 40));
		}
		it = headers.find("content-length");
		length = it == headers.end() ? "" : it->second;
		if (length.size() && (length.size() > 9 || strtoull(length.c_str(), NULL, 10) > max_bytes)) {
			return fail(FetchError::TOO_LARGE, "content length " + length.substr(0, 20));
		}
		return true;
	}
	bool write(const u8 *data, size_t len) override {
		if (out->size() + len > max_bytes) {
			return fail(FetchError::TOO_LARGE, "more than " + std::to_string(max_bytes) + " bytes");
		}
		out->append((const char *)data, len);
		return true;
	}
};
} // namespace

FetchResult fetch_small(NetworkSessionList &session, const std::string &url, const char *type, size_t max_bytes,
                        const std::function<bool()> &should_abort, std::string *out) {
	FetchResult res;
	out->clear();
	SmallSink sink;
	sink.type = type;
	sink.max_bytes = max_bytes;
	sink.out = out;
	HttpRequest request = HttpRequest::GET(url, {});
	request.body_sink = &sink;
	request.should_abort = should_abort;
	NetworkResult result = session.perform(request);
	res.http_status = sink.begun ? sink.status : result.status_code;
	if (should_abort && should_abort()) {
		res.error = FetchError::ABORTED;
	} else if (sink.reject != FetchError::NONE) {
		res.error = sink.reject;
		res.detail = sink.detail;
	} else if (result.fail || !sink.begun) {
		res.error = result.fail ? FetchError::NETWORK : FetchError::HTTP_STATUS;
		res.detail = result.fail ? result.error.substr(0, 80) : "HTTP " + std::to_string(res.http_status);
	} else if (sink.length.size() && sink.length != std::to_string(out->size())) {
		res.error = FetchError::BAD_RESPONSE;
		res.detail = std::to_string(out->size()) + " of " + sink.length + " bytes";
	}
	if (res.error != FetchError::NONE) {
		out->clear();
	}
	return res;
}

} // namespace downloads
