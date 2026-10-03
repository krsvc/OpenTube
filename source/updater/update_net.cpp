#include "updater/update_net.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>

namespace updater {

bool load_ca_bundle(const std::string &path, std::string *pem, std::string *err) {
	FILE *fp = fopen(path.c_str(), "rb");
	if (!fp) {
		*err = "trust bundle missing";
		return false;
	}
	std::string data(MAX_CA_BUNDLE_BYTES + 1, '\0');
	size_t n = fread(&data[0], 1, data.size(), fp);
	bool ok = !ferror(fp);
	fclose(fp);
	if (!ok || n == 0 || n > MAX_CA_BUNDLE_BYTES) {
		*err = "trust bundle unreadable";
		return false;
	}
	data.resize(n);
	if (data.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
		*err = "trust bundle has no certificate";
		return false;
	}
	*pem = data;
	return true;
}

namespace {
struct Ctx {
	CURL *curl = NULL;
	uint64_t max_bytes = 0, got = 0;
	const std::function<bool(const uint8_t *, size_t)> *sink = NULL;
	const std::function<bool()> *abort = NULL;
	std::string location, rate;
	bool too_large = false, sink_refused = false, aborted = false;
};
std::string lower(std::string s) {
	for (auto &c : s) {
		c = (char)tolower((unsigned char)c);
	}
	return s;
}
std::string trim(const std::string &s) {
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t')) {
		a++;
	}
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) {
		b--;
	}
	return s.substr(a, b - a);
}
size_t header_cb(char *p, size_t, size_t n, void *ud) {
	Ctx *c = (Ctx *)ud;
	std::string line(p, n);
	if (line.compare(0, 5, "HTTP/") == 0) { // a new response: headers of an earlier one do not carry over
		c->location.clear();
		c->rate.clear();
		return n;
	}
	size_t colon = line.find(':');
	if (colon != std::string::npos) {
		std::string name = lower(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
		if (name == "location") {
			c->location = value;
		} else if (name == "x-ratelimit-remaining") {
			c->rate = value;
		}
	}
	return n;
}
size_t write_cb(char *p, size_t, size_t n, void *ud) {
	Ctx *c = (Ctx *)ud;
	long status = 0;
	curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &status);
	if (status / 100 != 2) {
		return n; // error / redirect bodies are dropped (bounded by the same limit)
	}
	if (c->got + n > c->max_bytes) {
		c->too_large = true;
		return 0; // CURLE_WRITE_ERROR
	}
	if (!(*c->sink)((const uint8_t *)p, n)) {
		c->sink_refused = true;
		return 0;
	}
	c->got += n;
	return n;
}
int progress_cb(void *ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	Ctx *c = (Ctx *)ud;
	if (*c->abort && (*c->abort)()) {
		c->aborted = true;
		return 1; // CURLE_ABORTED_BY_CALLBACK
	}
	return 0;
}
} // namespace

HttpResult CurlTransport::get(const std::string &url, uint64_t max_bytes,
                              const std::function<bool(const uint8_t *, size_t)> &sink,
                              const std::function<bool()> &abort) {
	HttpResult r;
	if (abort && abort()) {
		r.aborted = r.transport_error = true;
		r.error = "stopped";
		return r;
	}
	CURL *curl = curl_easy_init();
	if (!curl) {
		r.transport_error = true;
		r.error = "network library unavailable";
		return r;
	}
	char *errbuf = (char *)calloc(1, CURL_ERROR_SIZE);
	Ctx ctx;
	ctx.curl = curl;
	ctx.max_bytes = max_bytes;
	ctx.sink = &sink;
	ctx.abort = &abort;
	struct curl_blob ca;
	ca.data = (void *)ca_pem.data();
	ca.len = ca_pem.size();
	ca.flags = CURL_BLOB_COPY;
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Accept: application/vnd.github+json, application/octet-stream");
	headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
	bool small = max_bytes <= MAX_METADATA_BYTES;
	bool ok = curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)CURLPROTO_HTTPS) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str()) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, UPDATE_NET_CONNECT_TIMEOUT_S) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, UPDATE_NET_STALL_TIMEOUT_S) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_TIMEOUT, small ? UPDATE_NET_METADATA_TIMEOUT_S : 0L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)max_bytes) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf) == CURLE_OK &&
	          curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK;
	CURLcode res = ok ? curl_easy_perform(curl) : CURLE_FAILED_INIT;
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	std::string detail = errbuf && errbuf[0] ? errbuf : curl_easy_strerror(res);
	// libcurl may still write the error buffer during cleanup: free it only afterwards (network_io.cpp r6)
	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);
	free(errbuf);
	r.status = (int)status;
	r.location = ctx.location;
	r.rate_remaining = ctx.rate;
	if (!ok) {
		r.transport_error = true;
		r.error = "a required TLS option is not available";
		return r;
	}
	if (res == CURLE_OK) {
		return r;
	}
	r.transport_error = true;
	if (ctx.aborted || res == CURLE_ABORTED_BY_CALLBACK) {
		r.aborted = true;
		r.error = "stopped";
	} else if (ctx.too_large || res == CURLE_FILESIZE_EXCEEDED) {
		r.too_large = true;
		r.transport_error = false;
		r.error = "more data than allowed";
	} else if (ctx.sink_refused) {
		r.error = "could not store the data";
	} else if (res == CURLE_PEER_FAILED_VERIFICATION || res == CURLE_SSL_CACERT_BADFILE ||
	           res == CURLE_SSL_CONNECT_ERROR) {
		r.error = "the server's certificate could not be verified (" + detail + ")";
	} else if (res == CURLE_OPERATION_TIMEDOUT) {
		r.error = "the connection timed out";
	} else if (res == CURLE_UNSUPPORTED_PROTOCOL) {
		r.error = "only HTTPS is allowed";
	} else {
		r.error = detail;
	}
	return r;
}

} // namespace updater
