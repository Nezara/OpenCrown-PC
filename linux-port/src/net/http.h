// Small blocking HTTP/1.1 GET client for the launcher's self-updater (net/update.h): https:// through net/tls (mbedTLS,
// certificate + host name verified against the system CA bundle, never the insecure mode), plain http:// only to
// 127.0.0.1 / localhost (the updater's local test server). Follows redirects (301/302/303/307/308, at most 5), reads
// Content-Length, chunked and until-close bodies, streams the body to a callback. Every hop (the first URL and each
// redirect target) must pass the caller's host check, so a redirect can never lead off the allow-list.
// Run it on a worker thread: connect / reads are bounded by timeouts, `cancel` is polled between reads.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dcp::net::http {

struct Url {
    bool secure = false;  // https
    std::string host;     // lower case
    uint16_t port = 0;
    std::string path;     // "/..." incl. query
};
// http(s)://host[:port][/path]. False for anything else (user info, IPv6 literals and other schemes are refused).
bool parse_url(const std::string& url, Url* out);
// Resolves a Location header against the URL it came from (absolute, scheme-relative "//host/..." or "/path").
std::string resolve(const Url& base, const std::string& location);

// Host policy for the updater: https to github.com, *.github.com, *.githubusercontent.com; with allow_local also
// http:// or https:// to 127.0.0.1 / localhost. Plain http:// anywhere else is refused.
bool update_host_allowed(const Url& u, bool allow_local);

struct Request {
    std::string url;
    std::vector<std::string> headers;           // "Name: value" lines (User-Agent, Accept, ...)
    std::function<bool(const Url&)> allowed;     // checked for every hop; required
    // Body bytes as they arrive; return false to abort ("aborted" error).
    std::function<bool(const char*, size_t)> sink;
    // Called after each read: bytes so far, total (-1 when the server sent no length).
    std::function<void(uint64_t, int64_t)> progress;
    const std::atomic<bool>* cancel = nullptr;
    int timeout_ms = 15000;                      // connect and each read
    int max_redirects = 5;
};

struct Response {
    bool ok = false;      // final status 200 and the whole body delivered
    int status = 0;       // last HTTP status seen (0 = no response)
    std::string error;    // human-readable reason when !ok
    std::string final_url;
    int64_t length = -1;  // Content-Length of the final response, -1 if none
};

Response get(const Request& rq);

// Convenience: whole body into *body (refused beyond max_bytes).
Response get_string(Request rq, std::string* body, size_t max_bytes = 8u << 20);

}  // namespace dcp::net::http
