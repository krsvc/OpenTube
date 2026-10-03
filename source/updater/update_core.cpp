#include "updater/update_core.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"

namespace updater {

// ---- versions ------------------------------------------------------------------------------------------------------
static bool parse_part(const std::string &s, size_t *i, int max, int *out) {
	size_t start = *i;
	long v = 0;
	while (*i < s.size() && s[*i] >= '0' && s[*i] <= '9') {
		v = v * 10 + (s[*i] - '0');
		if (v > max) {
			return false;
		}
		(*i)++;
	}
	size_t len = *i - start;
	if (len == 0 || (len > 1 && s[start] == '0')) { // no empty part, no leading zero
		return false;
	}
	*out = (int)v;
	return true;
}
bool parse_tag(const std::string &tag, Version *out) {
	if (tag.size() < 6 || tag.size() > 12 || tag[0] != 'v') {
		return false;
	}
	size_t i = 1;
	Version v;
	if (!parse_part(tag, &i, 63, &v.major) || i >= tag.size() || tag[i++] != '.' ||
	    !parse_part(tag, &i, 63, &v.minor) || i >= tag.size() || tag[i++] != '.' ||
	    !parse_part(tag, &i, 15, &v.micro) || i != tag.size()) {
		return false;
	}
	if (out) {
		*out = v;
	}
	return true;
}
int compare(const Version &a, const Version &b) {
	if (a.major != b.major) {
		return a.major < b.major ? -1 : 1;
	}
	if (a.minor != b.minor) {
		return a.minor < b.minor ? -1 : 1;
	}
	if (a.micro != b.micro) {
		return a.micro < b.micro ? -1 : 1;
	}
	return 0;
}
std::string version_string(const Version &v) {
	return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.micro);
}
bool contract_installable(const Version &v) {
	return v.major == 0 && v.minor >= 0 && v.minor <= 63 && v.micro >= 0 && v.micro <= 15;
}
uint16_t cia_title_version(const Version &v) { return (uint16_t)(v.minor << 10 | v.micro); }

// ---- repository / assets ------------------------------------------------------------------------------------------
const char *const OWNER_REPO = "krsvc/OpenTube";
const char *const LATEST_URL = "https://api.github.com/repos/krsvc/OpenTube/releases/latest";
const char *const ASSET_CIA = "OpenTube.cia";
const char *const ASSET_3DSX = "OpenTube.3dsx";
const char *const ASSET_SUMS = "SHA256SUMS";
std::string expected_asset_url(const std::string &tag, const std::string &name) {
	return std::string("https://github.com/") + OWNER_REPO + "/releases/download/" + tag + "/" + name;
}

std::string sanitize_text(const std::string &text, size_t max_bytes) {
	std::string res;
	for (char c : text) {
		unsigned char u = (unsigned char)c;
		if (c == '\r') {
			continue;
		}
		res.push_back((u < 0x20 && c != '\n') || u == 0x7F ? ' ' : c);
	}
	if (res.size() > max_bytes) {
		size_t cut = max_bytes;
		while (cut > 0 && ((unsigned char)res[cut] & 0xC0) == 0x80) {
			cut--;
		}
		res.resize(cut);
	}
	return res;
}

static bool is_lower_hex64(const std::string &s) {
	if (s.size() != 64) {
		return false;
	}
	for (char c : s) {
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

bool parse_release(const std::string &json, Release *out, std::string *why) {
	auto fail = [&](const std::string &r) {
		if (why) {
			*why = r;
		}
		return false;
	};
	if (json.size() > MAX_METADATA_BYTES) {
		return fail("release metadata too large");
	}
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.c_str(), json.size());
	if (doc.HasParseError() || !doc.IsObject()) {
		return fail("release metadata is not valid JSON");
	}
	auto str = [&](const rapidjson::Value &o, const char *k, std::string *v) {
		if (!o.HasMember(k) || !o[k].IsString()) {
			return false;
		}
		v->assign(o[k].GetString(), o[k].GetStringLength());
		return true;
	};
	Release r;
	if (!doc.HasMember("draft") || !doc["draft"].IsBool() || !doc.HasMember("prerelease") ||
	    !doc["prerelease"].IsBool()) {
		return fail("release metadata lacks draft / prerelease");
	}
	if (doc["draft"].GetBool() || doc["prerelease"].GetBool()) {
		return fail("not a stable release (draft or prerelease)");
	}
	if (!doc.HasMember("id") || !doc["id"].IsInt64() || doc["id"].GetInt64() <= 0) {
		return fail("release id missing");
	}
	r.id = doc["id"].GetInt64();
	if (!str(doc, "tag_name", &r.tag) || !parse_tag(r.tag, &r.version)) {
		return fail("tag is not an OpenTube version (vX.Y.Z)");
	}
	std::string text;
	if (doc.HasMember("name") && doc["name"].IsString()) {
		str(doc, "name", &text);
		r.title = sanitize_text(text, 100);
	}
	if (doc.HasMember("body") && doc["body"].IsString()) {
		str(doc, "body", &text);
		r.notes = sanitize_text(text, MAX_NOTES_BYTES);
	}
	if (!doc.HasMember("assets") || !doc["assets"].IsArray() || doc["assets"].Size() > 50) {
		return fail("release assets missing");
	}
	for (auto &a : doc["assets"].GetArray()) {
		std::string name;
		if (!a.IsObject() || !str(a, "name", &name)) {
			return fail("malformed asset");
		}
		Asset *slot = name == ASSET_CIA ? &r.cia : name == ASSET_3DSX ? &r.tdsx : name == ASSET_SUMS ? &r.sums : NULL;
		if (!slot) {
			continue; // other files (e.g. source archives) are not used
		}
		if (slot->present) {
			return fail("asset " + name + " listed twice");
		}
		std::string state, url;
		if (!str(a, "state", &state) || state != "uploaded") {
			return fail("asset " + name + " is not fully uploaded");
		}
		if (!str(a, "browser_download_url", &url) || url != expected_asset_url(r.tag, name)) {
			return fail("asset " + name + " has an unexpected download address");
		}
		if (!a.HasMember("size") || !a["size"].IsUint64()) {
			return fail("asset " + name + " has no size");
		}
		uint64_t size = a["size"].GetUint64();
		uint64_t max = slot == &r.sums ? MAX_SUMS_BYTES : MAX_PACKAGE_BYTES;
		if (size == 0 || size > max) {
			return fail("asset " + name + " size " + std::to_string(size) + " is out of bounds");
		}
		std::string digest;
		if (a.HasMember("digest") && !a["digest"].IsNull()) {
			if (!str(a, "digest", &digest) || digest.compare(0, 7, "sha256:") != 0 || !is_lower_hex64(digest.substr(7))) {
				return fail("asset " + name + " has an unreadable digest");
			}
			digest = digest.substr(7);
		}
		slot->present = true;
		slot->name = name;
		slot->url = url;
		slot->size = size;
		slot->sha256 = digest;
	}
	if (!r.cia.present) {
		return fail("the release has no OpenTube.cia");
	}
	if (!r.sums.present) {
		return fail("the release has no SHA256SUMS");
	}
	if (out) {
		*out = r;
	}
	return true;
}

bool parse_sums(const std::string &text, const std::string &name, std::string *hex, std::string *why) {
	auto fail = [&](const std::string &r) {
		if (why) {
			*why = r;
		}
		return false;
	};
	if (text.size() > MAX_SUMS_BYTES) {
		return fail("SHA256SUMS too large");
	}
	std::string found;
	std::set<std::string> names;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t nl = text.find('\n', pos);
		std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
		pos = nl == std::string::npos ? text.size() : nl + 1;
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		if (line.size() < 67 || !is_lower_hex64(line.substr(0, 64)) || line[64] != ' ' ||
		    (line[65] != ' ' && line[65] != '*')) {
			return fail("SHA256SUMS has a malformed line");
		}
		std::string file = line.substr(66);
		for (char c : file) {
			if ((unsigned char)c < 0x20 || c == '/' || c == '\\') {
				return fail("SHA256SUMS names an unexpected file");
			}
		}
		if (!names.insert(file).second) {
			return fail("SHA256SUMS lists " + file + " twice");
		}
		if (file == name) {
			found = line.substr(0, 64);
		}
	}
	if (found.empty()) {
		return fail("SHA256SUMS has no line for " + name);
	}
	*hex = found;
	return true;
}

// ---- URL policy ---------------------------------------------------------------------------------------------------
bool split_https_url(const std::string &url, std::string *host, std::string *path) {
	const std::string scheme = "https://";
	if (url.size() > 2048 || url.compare(0, scheme.size(), scheme) != 0) {
		return false;
	}
	size_t slash = url.find('/', scheme.size());
	if (slash == std::string::npos) {
		return false;
	}
	std::string h = url.substr(scheme.size(), slash - scheme.size());
	if (h.empty()) {
		return false;
	}
	for (char c : h) { // no userinfo (@), no port (:), lower-case DNS name only
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
			return false;
		}
	}
	for (size_t i = slash; i < url.size(); i++) {
		unsigned char c = (unsigned char)url[i];
		if (c <= 0x20 || c >= 0x7F || c == '\\') {
			return false;
		}
	}
	if (host) {
		*host = h;
	}
	if (path) {
		*path = url.substr(slash);
	}
	return true;
}
bool metadata_host(const std::string &host) { return host == "api.github.com"; }
bool asset_host(const std::string &host) {
	return host == "github.com" || host == "objects.githubusercontent.com" ||
	       host == "release-assets.githubusercontent.com";
}

// ---- SHA-256 ------------------------------------------------------------------------------------------------------
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
Sha256::Sha256() {
	static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(h, H0, sizeof(h));
}
void Sha256::block(const uint8_t *p) {
	uint32_t w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
	for (int i = 0; i < 64; i++) {
		uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
		uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
	}
	h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}
void Sha256::update(const void *data, size_t len) {
	const uint8_t *p = (const uint8_t *)data;
	total += len;
	while (len) {
		size_t n = 64 - used < len ? 64 - used : len;
		memcpy(buf + used, p, n);
		used += n, p += n, len -= n;
		if (used == 64) {
			block(buf);
			used = 0;
		}
	}
}
std::string Sha256::hex() {
	uint64_t bits = total * 8;
	uint8_t pad = 0x80;
	update(&pad, 1);
	uint8_t zero = 0;
	while (used != 56) {
		update(&zero, 1);
	}
	uint8_t len[8];
	for (int i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56 - i * 8));
	}
	update(len, 8);
	char out[65];
	for (int i = 0; i < 8; i++) {
		snprintf(out + i * 8, 9, "%08x", (unsigned)h[i]);
	}
	return std::string(out, 64);
}
std::string sha256_hex(const std::string &data) {
	Sha256 s;
	s.update(data.data(), data.size());
	return s.hex();
}

// ---- CIA check ----------------------------------------------------------------------------------------------------
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint64_t align64(uint64_t x) { return (x + 63) & ~(uint64_t)63; }
static size_t sig_body_offset(uint32_t sig_type) { // signature + padding before the signed body
	switch (sig_type) {
	case 0x10003: // RSA-4096 SHA-256
		return 0x240;
	case 0x10004: // RSA-2048 SHA-256
		return 0x140;
	case 0x10005: // ECDSA SHA-256
		return 0x80;
	default:
		return 0;
	}
}
bool check_cia(const uint8_t *head, size_t head_len, uint64_t file_size, uint16_t want_title_version, std::string *why) {
	auto fail = [&](const std::string &r) {
		if (why) {
			*why = r;
		}
		return false;
	};
	if (file_size < 0x2040 || file_size > MAX_PACKAGE_BYTES) {
		return fail("package size out of bounds");
	}
	size_t need = file_size < CIA_HEAD_BYTES ? (size_t)file_size : CIA_HEAD_BYTES;
	if (head_len < need) {
		return fail("package header not read");
	}
	uint32_t hs = le32(head), cs = le32(head + 8), ts = le32(head + 0xC), tms = le32(head + 0x10), ms = le32(head + 0x14);
	uint16_t type = (uint16_t)(head[4] | head[5] << 8), ver = (uint16_t)(head[6] | head[7] << 8);
	uint64_t content = le64(head + 0x18);
	if (hs != 0x2020 || type != 0 || ver != 0 || cs != 0xA00) {
		return fail("not a CIA file");
	}
	if (ts < 0x2A4 || ts > 0x1000 || tms < 0xB34 || tms > 0x4000 || (ms != 0 && ms != 0x3AC0) || content == 0 ||
	    content > MAX_PACKAGE_BYTES) {
		return fail("CIA section sizes out of bounds");
	}
	uint64_t tik = align64(hs) + align64(cs), tmd = tik + align64(ts), off = tmd + align64(tms);
	uint64_t end = off + align64(content) + ms;
	if (!(file_size == end || (ms == 0 && file_size == off + content))) {
		return fail("CIA size does not match its sections (truncated or padded)");
	}
	if (off + 0x200 > need) {
		return fail("CIA content header outside the checked range");
	}
	// ticket
	size_t tb = sig_body_offset(be32(head + tik));
	if (!tb || tb + 0xA8 > ts) {
		return fail("unsupported ticket signature");
	}
	uint64_t tik_title = be64(head + tik + tb + 0x9C);
	uint16_t tik_version = be16(head + tik + tb + 0xA6);
	// TMD
	size_t mb = sig_body_offset(be32(head + tmd));
	if (!mb || mb + 0xA0 > tms) {
		return fail("unsupported TMD signature");
	}
	const uint8_t *tb_ = head + tmd + mb;
	uint64_t tmd_title = be64(tb_ + 0x4C);
	uint16_t tmd_version = be16(tb_ + 0x9C), count = be16(tb_ + 0x9E);
	if (count < 1 || count > 64 || mb + 0x9C4 + 0x30ULL * count > tms) {
		return fail("TMD content list out of bounds");
	}
	uint64_t sum = 0;
	for (int i = 0; i < count; i++) {
		const uint8_t *c = tb_ + 0x9C4 + 0x30 * i;
		uint16_t index = be16(c + 4), ctype = be16(c + 6);
		uint64_t size = be64(c + 8);
		if (i == 0 && index != 0) {
			return fail("TMD first content is not the main program");
		}
		if (ctype & 1) {
			return fail("encrypted content is not accepted");
		}
		if (size > MAX_PACKAGE_BYTES) {
			return fail("TMD content size out of bounds");
		}
		sum += size;
	}
	if (sum != content) {
		return fail("TMD content sizes do not add up");
	}
	if (tik_title != TITLE_ID || tmd_title != TITLE_ID) {
		char buf[80];
		snprintf(buf, sizeof(buf), "wrong title (%016llX / %016llX)", (unsigned long long)tik_title,
		         (unsigned long long)tmd_title);
		return fail(buf);
	}
	if (tik_version != want_title_version || tmd_version != want_title_version) {
		return fail("title version " + std::to_string(tmd_version) + " does not match the release (" +
		            std::to_string(want_title_version) + ")");
	}
	const uint8_t *ncch = head + off;
	if (memcmp(ncch + 0x100, "NCCH", 4) != 0) {
		return fail("CIA content is not an NCCH program");
	}
	if (le64(ncch + 0x108) != TITLE_ID || le64(ncch + 0x118) != TITLE_ID) {
		return fail("program id is not OpenTube's");
	}
	return true;
}

// ---- procedures ---------------------------------------------------------------------------------------------------
static bool is_redirect(int s) { return s == 301 || s == 302 || s == 303 || s == 307 || s == 308; }

// GET with GitHub's release asset redirect: the first URL is the exact asset URL (github.com), every hop must stay on
// https and the asset hosts; bodies only arrive for the final 2xx
static HttpResult fetch_asset(Transport &t, const std::string &url, uint64_t max,
                              const std::function<bool(const uint8_t *, size_t)> &sink,
                              const std::function<bool()> &abort) {
	std::string cur = url;
	for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
		std::string host;
		if (!split_https_url(cur, &host, NULL) || !(hop == 0 ? host == "github.com" : asset_host(host))) {
			HttpResult r;
			r.transport_error = true;
			r.error = "download address not allowed (" + (host.empty() ? std::string("invalid") : host) + ")";
			return r;
		}
		HttpResult r = t.get(cur, max, sink, abort);
		if (r.transport_error || !is_redirect(r.status)) {
			return r;
		}
		if (r.location.empty()) {
			r.transport_error = true;
			r.error = "redirect without a location";
			return r;
		}
		cur = r.location;
	}
	HttpResult r;
	r.transport_error = true;
	r.error = "too many redirects";
	return r;
}

CheckResult check_latest(Transport &t, const Version &current, const std::function<bool()> &abort) {
	CheckResult res;
	std::string body;
	HttpResult r = t.get(
	    LATEST_URL, MAX_METADATA_BYTES,
	    [&](const uint8_t *p, size_t n) {
		    body.append((const char *)p, n);
		    return true;
	    },
	    abort);
	if (r.aborted) {
		res.message = "Update check stopped.";
		return res;
	}
	if (r.transport_error) {
		res.message = "Could not reach GitHub securely: " + r.error;
		return res;
	}
	if (r.too_large) {
		res.message = "The release information is too large.";
		return res;
	}
	if (r.status == 404) {
		res.outcome = CheckOutcome::NO_RELEASE;
		res.message = "No OpenTube release has been published yet.";
		return res;
	}
	if (r.status == 403 || r.status == 429) {
		res.message = r.rate_remaining == "0" || r.status == 429
		                  ? "GitHub's request limit was reached. Try again later."
		                  : "GitHub refused the request (HTTP " + std::to_string(r.status) + "). Try again later.";
		return res;
	}
	if (r.status != 200) {
		res.message = "GitHub answered HTTP " + std::to_string(r.status) + ".";
		return res;
	}
	std::string why;
	if (!parse_release(body, &res.release, &why)) {
		res.message = "The latest release is not a valid OpenTube release: " + why + ".";
		return res;
	}
	if (compare(res.release.version, current) <= 0) {
		res.outcome = CheckOutcome::UP_TO_DATE;
		res.message = "OpenTube " + version_string(current) + " is up to date.";
		return res;
	}
	if (!contract_installable(res.release.version)) {
		res.outcome = CheckOutcome::NOT_INSTALLABLE;
		res.message = "OpenTube " + version_string(res.release.version) + " is available. It must be installed "
		              "manually (github.com/krsvc/OpenTube/releases).";
		return res;
	}
	res.outcome = CheckOutcome::AVAILABLE;
	res.message = "OpenTube " + version_string(res.release.version) + " is available.";
	return res;
}

Staged download_and_verify(Transport &t, StageFs &fs, const Release &confirmed, const Version &current,
                           const std::function<bool()> &abort, const Progress &progress) {
	Staged st;
	st.release = confirmed;
	auto fail = [&](const std::string &m, bool discard) {
		st.ok = false;
		st.message = m;
		st.cancelled = abort && abort();
		if (discard) {
			fs.discard();
		}
		return st;
	};
	// 1. the release must still be exactly the one the user confirmed
	CheckResult c = check_latest(t, current, abort);
	if (abort && abort()) {
		return fail("Download cancelled.", false);
	}
	if (c.outcome != CheckOutcome::AVAILABLE || !c.release.same_target(confirmed)) {
		return fail(c.outcome == CheckOutcome::FAILED ? c.message
		                                              : "The release changed since you confirmed it. Check again and "
		                                                "review the new version before downloading.",
		            false);
	}
	// 2. checksums
	std::string sums;
	HttpResult r = fetch_asset(
	    t, confirmed.sums.url, MAX_SUMS_BYTES,
	    [&](const uint8_t *p, size_t n) {
		    sums.append((const char *)p, n);
		    return true;
	    },
	    abort);
	if (r.aborted) {
		return fail("Download cancelled.", false);
	}
	if (r.transport_error || r.status != 200 || r.too_large || sums.size() != confirmed.sums.size) {
		return fail("Could not download SHA256SUMS" + (r.error.empty() ? std::string() : ": " + r.error) +
		                (r.status && r.status != 200 ? " (HTTP " + std::to_string(r.status) + ")" : ""),
		            false);
	}
	std::string expected, why;
	if (!parse_sums(sums, ASSET_CIA, &expected, &why)) {
		return fail("SHA256SUMS is not usable: " + why + ".", false);
	}
	if (!confirmed.cia.sha256.empty() && confirmed.cia.sha256 != expected) {
		return fail("GitHub's checksum and SHA256SUMS disagree. Nothing was installed.", false);
	}
	// 3. the package into staging, hashed while written
	std::string err;
	if (!fs.ensure_dir(&err) || !fs.open_write(&err)) {
		return fail("Cannot prepare the SD card: " + err, false); // nothing was created: nothing to discard
	}
	Sha256 h1;
	uint64_t got = 0;
	std::string wr_err;
	r = fetch_asset(
	    t, confirmed.cia.url, confirmed.cia.size,
	    [&](const uint8_t *p, size_t n) {
		    if (got + n > confirmed.cia.size) {
			    wr_err = "more data than announced";
			    return false;
		    }
		    if (!fs.write(p, n, &wr_err)) {
			    return false;
		    }
		    h1.update(p, n);
		    got += n;
		    if (progress.report) {
			    progress.report(got, confirmed.cia.size);
		    }
		    return true;
	    },
	    abort);
	bool closed = fs.finish_write(&err);
	if (r.aborted || (abort && abort())) {
		return fail("Download cancelled.", true);
	}
	if (!wr_err.empty()) {
		return fail("Could not save the update: " + wr_err, true);
	}
	if (r.transport_error || r.status != 200 || r.too_large) {
		return fail("The download failed" + (r.error.empty() ? std::string() : ": " + r.error) +
		                (r.status && r.status != 200 ? " (HTTP " + std::to_string(r.status) + ")" : "") + ".",
		            true);
	}
	if (!closed) {
		return fail("Could not save the update: " + err, true);
	}
	if (got != confirmed.cia.size) {
		return fail("The download is incomplete (" + std::to_string(got) + " of " + std::to_string(confirmed.cia.size) +
		                " bytes).",
		            true);
	}
	if (h1.hex() != expected) {
		return fail("The download does not match its checksum. Nothing was installed.", true);
	}
	// 4. read back from the card: size, digest, CIA structure
	uint64_t size = 0;
	if (!fs.open_read(&size, &err)) {
		return fail("Cannot read the saved update: " + err, true);
	}
	std::vector<uint8_t> head;
	std::vector<uint8_t> buf(64 * 1024);
	Sha256 h2;
	uint64_t read_total = 0;
	for (;;) {
		size_t n = fs.read(buf.data(), buf.size(), &err);
		if (n == 0) {
			break;
		}
		if (head.size() < CIA_HEAD_BYTES) {
			head.insert(head.end(), buf.begin(), buf.begin() + std::min(n, CIA_HEAD_BYTES - head.size()));
		}
		h2.update(buf.data(), n);
		read_total += n;
	}
	fs.close_read();
	if (!err.empty() || size != confirmed.cia.size || read_total != size || h2.hex() != expected) {
		return fail("The saved update does not read back correctly" + (err.empty() ? std::string() : ": " + err) + ".",
		            true);
	}
	if (!check_cia(head.data(), head.size(), size, cia_title_version(confirmed.version), &why)) {
		return fail("The package is not a valid OpenTube update: " + why + ".", true);
	}
	st.ok = true;
	st.sha256 = expected;
	st.size = size;
	st.message = "OpenTube " + version_string(confirmed.version) + " is downloaded and verified.";
	return st;
}

InstallResult install_staged(StageFs &fs, Installer &inst, Environment &env, const Staged &staged,
                             const Progress &progress) {
	InstallResult res;
	if (!staged.ok) {
		res.refused = true;
		res.message = "Nothing verified to install.";
		return res;
	}
	// admission is the activity gate's reservation (taken before the job was queued, held until this returns), not a
	// "nothing is playing / downloading" read here, which could be outdated by the time the installer starts
	if (!env.install_reserved()) {
		res.refused = true;
		res.message = "The installation was not reserved. Try again.";
		return res;
	}
	auto closing = [&]() {
		res.refused = true;
		res.message = "The app is closing: the installation was not started. Nothing was changed.";
		return res;
	};
	if (env.exit_requested()) {
		return closing();
	}
	std::string err, why;
	// pass 1 (nothing started yet): the staged file must still be exactly the verified one
	uint64_t size = 0;
	std::vector<uint8_t> buf(64 * 1024), head;
	if (!fs.open_read(&size, &err) || size != staged.size) {
		fs.close_read();
		res.refused = true;
		res.message = "The saved update changed or is missing. Download it again.";
		return res;
	}
	Sha256 h1;
	for (;;) {
		size_t n = fs.read(buf.data(), buf.size(), &err);
		if (n == 0) {
			break;
		}
		if (head.size() < CIA_HEAD_BYTES) {
			head.insert(head.end(), buf.begin(), buf.begin() + std::min(n, CIA_HEAD_BYTES - head.size()));
		}
		h1.update(buf.data(), n);
		if (env.exit_requested()) { // pass 1 is bounded by the exit: nothing was started yet
			fs.close_read();
			return closing();
		}
	}
	fs.close_read();
	if (!err.empty() || h1.hex() != staged.sha256 ||
	    !check_cia(head.data(), head.size(), size, cia_title_version(staged.release.version), &why)) {
		res.refused = true;
		res.message = "The saved update changed or is damaged. Download it again.";
		return res;
	}
	// pass 2: into the system installer, hashed again; finish only on a match
	if (!fs.open_read(&size, &err) || size != staged.size) {
		fs.close_read();
		res.refused = true;
		res.message = "The saved update changed or is missing. Download it again.";
		return res;
	}
	bool exit_refused = false;
	if (!env.start_guarded([&]() { return inst.begin(&err); }, &exit_refused)) {
		fs.close_read();
		if (exit_refused) {
			return closing();
		}
		res.refused = true;
		res.message = "The system installer could not start: " + err + ". Nothing was changed.";
		return res;
	}
	Sha256 h2;
	uint64_t offset = 0;
	bool write_ok = true;
	for (;;) {
		size_t n = fs.read(buf.data(), buf.size(), &err);
		if (n == 0) {
			break;
		}
		if (!inst.write(offset, buf.data(), n, &err)) {
			write_ok = false;
			break;
		}
		h2.update(buf.data(), n);
		offset += n;
		if (progress.report) {
			progress.report(offset, size);
		}
	}
	fs.close_read();
	if (!write_ok || !err.empty() || offset != size || h2.hex() != staged.sha256) {
		std::string cause = !write_ok ? "writing to the system installer failed: " + err
		                    : !err.empty() ? "reading the saved update failed: " + err
		                                   : "the saved update changed during the installation";
		std::string abort_err;
		res.aborted_ok = inst.abort(&abort_err);
		res.message = "Installation stopped: " + cause + ". " +
		              (res.aborted_ok ? "The installed OpenTube was left as it was."
		                              : "The system installer did not confirm the abort (" + abort_err +
		                                    "); if OpenTube misbehaves, reinstall it from a CIA file.");
		return res;
	}
	if (!inst.finish(&err)) {
		res.message = "The system did not accept the package: " + err +
		              ". If OpenTube misbehaves after a restart, reinstall it from a CIA file.";
		return res;
	}
	fs.discard();
	res.ok = true;
	res.message = "OpenTube " + version_string(staged.release.version) + " is installed. Close OpenTube and start it "
	              "again from the HOME Menu to use it.";
	return res;
}

// ---- UI model -----------------------------------------------------------------------------------------------------
const char *state_name(State s) {
	static const char *names[] = {"idle",      "checking",  "no-release",  "up-to-date",     "available",
	                              "not-installable", "check-failed", "downloading", "download-failed", "ready",
	                              "installing", "install-failed", "installed"};
	return names[(int)s];
}
bool Model::request_check(bool automatic, std::string *msg) {
	if (stopping) {
		*msg = "The app is closing.";
		return false;
	}
	if (automatic && auto_checked) {
		*msg = "already checked in this session";
		return false;
	}
	if (running || job != Job::NONE || state == State::CHECKING || state == State::DOWNLOADING ||
	    state == State::READY || state == State::INSTALLING || state == State::INSTALLED) {
		*msg = "An update action is already in progress.";
		return false;
	}
	auto_checked = true;
	state = State::CHECKING;
	job = Job::CHECK;
	message = "Checking for updates...";
	return true;
}
bool Model::request_download(const Release &shown, std::string *msg) {
	if (stopping) {
		*msg = "The app is closing.";
		return false;
	}
	if ((state != State::AVAILABLE && state != State::DOWNLOAD_FAILED) || running || job != Job::NONE ||
	    !shown.same_target(release) || release.tag.empty()) {
		*msg = "Check for updates again first.";
		return false;
	}
	state = State::DOWNLOADING;
	job = Job::DOWNLOAD;
	cancel_requested = false;
	done = 0, total = release.cia.size;
	message = "Downloading OpenTube " + version_string(release.version) + "...";
	return true;
}
bool Model::request_install(const std::string &shown_sha256, std::string *msg) {
	if (stopping) {
		*msg = "The app is closing.";
		return false;
	}
	if (state != State::READY || running || job != Job::NONE || !staged.ok || shown_sha256 != staged.sha256) {
		*msg = "There is no verified update to install.";
		return false;
	}
	state = State::INSTALLING;
	job = Job::INSTALL;
	done = 0, total = staged.size;
	message = "Installing OpenTube " + version_string(staged.release.version) + ". Do not turn off the console.";
	return true;
}
bool Model::request_cancel(std::string *msg) {
	if (state != State::DOWNLOADING) {
		*msg = state == State::INSTALLING ? "The installation cannot be cancelled." : "Nothing to cancel.";
		return false;
	}
	cancel_requested = true;
	message = "Cancelling...";
	return true;
}
Job Model::request_stop() {
	stopping = true;
	Job dropped = job;
	job = Job::NONE;
	if (dropped == Job::INSTALL) {
		state = State::READY; // the verified file is still there; nothing was started
		message = "The app is closing: the installation was not started.";
	} else if (dropped == Job::DOWNLOAD) {
		state = State::AVAILABLE;
		message = "The app is closing: the download was not started.";
	} else if (dropped == Job::CHECK) {
		state = State::IDLE;
		message = "";
	}
	return dropped;
}
Job Model::take_job() {
	if (stopping) {
		return Job::NONE;
	}
	Job j = job;
	job = Job::NONE;
	running = j != Job::NONE;
	return j;
}
bool Model::start_install(const std::function<bool()> &start, bool *exit_refused) {
	*exit_refused = stopping;
	if (stopping) {
		return false;
	}
	install_started = start();
	return install_started;
}
void Model::check_done(const CheckResult &r) {
	running = false;
	message = r.message;
	switch (r.outcome) {
	case CheckOutcome::NO_RELEASE:
		state = State::NO_RELEASE;
		break;
	case CheckOutcome::UP_TO_DATE:
		state = State::UP_TO_DATE;
		break;
	case CheckOutcome::AVAILABLE:
		state = State::AVAILABLE;
		release = r.release;
		break;
	case CheckOutcome::NOT_INSTALLABLE:
		state = State::NOT_INSTALLABLE;
		release = r.release;
		break;
	default:
		state = State::CHECK_FAILED;
		break;
	}
}
void Model::download_done(const Staged &s) {
	running = false;
	cancel_requested = false;
	message = s.message;
	if (s.ok) {
		state = State::READY;
		staged = s;
	} else {
		state = s.cancelled ? State::AVAILABLE : State::DOWNLOAD_FAILED;
		staged = Staged();
	}
}
void Model::install_done(const InstallResult &r) {
	running = false;
	install_started = false;
	message = r.message;
	if (r.ok) {
		state = State::INSTALLED;
	} else if (r.refused) {
		state = State::READY; // nothing was started: the verified file is still there
	} else {
		state = State::INSTALL_FAILED;
		staged = Staged();
	}
}

// ---- worker loop --------------------------------------------------------------------------------------------------
void worker_loop(WorkerHost &h) {
	for (;;) {
		h.lock();
		Model &m = h.model();
		Job job = m.take_job(); // NONE once stopping: the exit and the claim are decided here, under one lock
		bool stop = m.stopping;
		Release release = m.release;
		Staged staged = m.staged;
		h.unlock();
		if (job == Job::NONE) {
			if (stop) {
				return;
			}
			h.idle();
			continue;
		}
		if (job == Job::CHECK) {
			CheckResult r = h.check();
			h.lock();
			h.model().check_done(r);
			h.unlock();
		} else if (job == Job::DOWNLOAD) {
			Staged s = h.download(release);
			h.lock();
			h.model().download_done(s);
			h.unlock();
		} else {
			InstallResult r = h.install(staged);
			h.lock();
			h.model().install_done(r);
			h.unlock();
		}
		h.job_finished(job);
	}
}

// ---- POSIX staging file -------------------------------------------------------------------------------------------
namespace {
struct PosixStage : StageFs {
	std::string dir, path;
	FILE *w = NULL, *r = NULL;
	char *wbuf = NULL;
	bool owned = false;        // this object created the file at `path` (O_CREAT | O_EXCL) and has not removed it
	dev_t own_dev = 0;         // identity of that file where the file system reports one (host); the 3DS SD card
	ino_t own_ino = 0;         // reports no inode, so there only `owned` guards the removal
	PosixStage(const std::string &d, const std::string &f) : dir(d), path(d + "/" + f) {}
	~PosixStage() {
		close_read();
		if (w) {
			fclose(w);
		}
		free(wbuf);
	}
	static std::string errstr(const char *what) { return std::string(what) + " (errno " + std::to_string(errno) + ")"; }
	bool ensure_dir(std::string *err) override {
		std::string parent = dir.substr(0, dir.rfind('/'));
		for (const std::string &p : {parent, dir}) {
			struct stat st;
#ifdef __3DS__
			int s = stat(p.c_str(), &st);
#else
			int s = lstat(p.c_str(), &st);
#endif
			if (s != 0) {
				errno = 0;
				if (mkdir(p.c_str(), 0777) != 0 && errno != EEXIST) {
					*err = errstr("cannot create the update folder");
					return false;
				}
			} else if (!S_ISDIR(st.st_mode)) {
				*err = "the update folder is not a folder";
				return false;
			}
		}
		return true;
	}
	// the file at `path` is still the one this object created (as far as the file system can tell)
	bool still_ours() {
		if (!owned) {
			return false;
		}
		struct stat st;
#ifdef __3DS__
		int s = stat(path.c_str(), &st);
#else
		int s = lstat(path.c_str(), &st);
#endif
		if (s != 0 || !S_ISREG(st.st_mode)) {
			return false;
		}
		return own_ino == 0 || (st.st_dev == own_dev && st.st_ino == own_ino);
	}
	bool open_write(std::string *err) override {
		if (w) {
			*err = "the staging file is already open";
			return false;
		}
		close_read();
		if (owned) { // our own earlier file (e.g. an install that failed in this session): replaced by a fresh one
			if (!still_ours() || remove(path.c_str()) != 0) {
				owned = false;
				*err = "the earlier update file changed or cannot be removed; it was left as it is";
				return false;
			}
			owned = false;
		}
		errno = 0;
		int flags = O_WRONLY | O_CREAT | O_EXCL; // never opens, truncates or follows anything that already exists
#ifdef O_NOFOLLOW
		flags |= O_NOFOLLOW;
#endif
		int fd = open(path.c_str(), flags, 0666);
		if (fd < 0) {
			*err = errno == EEXIST ? "a file named " + path.substr(path.rfind('/') + 1) +
			                             " is already in the update folder. OpenTube never overwrites or removes a file it "
			                             "did not create in this session: remove it with a file manager, then try again"
			                       : errstr("cannot create the staging file");
			return false;
		}
		struct stat st;
		if (fstat(fd, &st) == 0) {
			own_dev = st.st_dev, own_ino = st.st_ino;
		} else {
			own_dev = 0, own_ino = 0;
		}
		owned = true;
		w = fdopen(fd, "wb");
		if (!w) {
			*err = errstr("cannot open the staging file");
			close(fd);
			return false; // created by us: discard() may remove it
		}
		wbuf = (char *)malloc(64 * 1024);
		if (wbuf) {
			setvbuf(w, wbuf, _IOFBF, 64 * 1024);
		}
		return true;
	}
	bool write(const uint8_t *p, size_t n, std::string *err) override {
		errno = 0;
		if (!w || fwrite(p, 1, n, w) != n) {
			*err = errno == ENOSPC ? std::string("the SD card is full") : errstr("write failed");
			return false;
		}
		return true;
	}
	bool finish_write(std::string *err) override {
		if (!w) {
			return true;
		}
		bool ok = fflush(w) == 0;
		if (ok && fsync(fileno(w)) != 0 && errno != EINVAL && errno != ENOSYS) {
			ok = false;
		}
		int e = errno;
		ok = fclose(w) == 0 && ok;
		w = NULL;
		free(wbuf);
		wbuf = NULL;
		if (!ok) {
			*err = e == ENOSPC ? std::string("the SD card is full") : "saving failed (errno " + std::to_string(e) + ")";
		}
		return ok;
	}
	bool open_read(uint64_t *size, std::string *err) override {
		close_read();
		errno = 0;
		r = fopen(path.c_str(), "rb");
		if (!r || fseek(r, 0, SEEK_END) != 0) {
			*err = errstr("cannot open the saved update");
			return false;
		}
		long l = ftell(r);
		if (l < 0 || fseek(r, 0, SEEK_SET) != 0) {
			*err = errstr("cannot size the saved update");
			return false;
		}
		*size = (uint64_t)l;
		return true;
	}
	size_t read(uint8_t *p, size_t n, std::string *err) override {
		if (!r) {
			*err = "not open";
			return 0;
		}
		size_t got = fread(p, 1, n, r);
		if (got == 0 && ferror(r)) {
			*err = errstr("read failed");
		}
		return got;
	}
	void close_read() override {
		if (r) {
			fclose(r);
			r = NULL;
		}
	}
	void discard() override {
		close_read();
		if (w) {
			fclose(w);
			w = NULL;
			free(wbuf);
			wbuf = NULL;
		}
		if (still_ours()) {
			remove(path.c_str());
		}
		owned = false;
	}
};
} // namespace
StageFs *new_posix_stage_fs(const std::string &dir, const std::string &file) { return new PosixStage(dir, file); }

} // namespace updater
