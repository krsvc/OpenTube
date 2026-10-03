#pragma once
// OpenTube r15 updater transport: its own libcurl easy handle, HTTPS only, peer AND host name verification against the
// scoped trust bundle shipped in romfs (cert/update_roots.pem), redirects never followed here (update_core decides
// every hop), bounded bodies, timeouts, abort polled at least once a second. The YouTube transport
// (network_decoder/network_io.cpp) is not used and not changed.
#include <string>
#include "updater/update_core.hpp"

#ifndef UPDATE_NET_CONNECT_TIMEOUT_S
#define UPDATE_NET_CONNECT_TIMEOUT_S 20L
#endif
#ifndef UPDATE_NET_STALL_TIMEOUT_S
#define UPDATE_NET_STALL_TIMEOUT_S 30L // less than 1 byte/s for this long ends the request
#endif
#ifndef UPDATE_NET_METADATA_TIMEOUT_S
#define UPDATE_NET_METADATA_TIMEOUT_S 60L // whole request, small bodies (metadata, SHA256SUMS)
#endif

namespace updater {

constexpr size_t MAX_CA_BUNDLE_BYTES = 64 * 1024;
// reads a PEM bundle (bounded; must contain at least one certificate)
bool load_ca_bundle(const std::string &path, std::string *pem, std::string *err);

class CurlTransport : public Transport {
	std::string ca_pem;
	std::string user_agent;

  public:
	CurlTransport(const std::string &ca_pem, const std::string &user_agent) : ca_pem(ca_pem), user_agent(user_agent) {}
	HttpResult get(const std::string &url, uint64_t max_bytes, const std::function<bool(const uint8_t *, size_t)> &sink,
	               const std::function<bool()> &abort) override;
};

} // namespace updater
