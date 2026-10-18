// TLS client over a connected TCP socket, using mbedTLS (static, fetched at configure time): no exceptions. Only used
// by ws::Conn for wss:// lobby URLs (the Cloudflare Worker). The server certificate is validated against the system
// CA bundle and the host name. For local tests against a self-signed
// endpoint (`wrangler dev --local-protocol https`), $DCP_LOBBY_TLS_INSECURE=1 skips validation, but only when the host
// is 127.0.0.1 / localhost.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dcp::net::tls {

struct Session;

// Runs the client handshake on `sock` (blocking, bounded by the socket's SO_RCVTIMEO). nullptr + *err on failure;
// the socket stays owned by the caller either way.
Session* handshake(uintptr_t sock, const std::string& host, std::string* err);
// Encrypts and sends all bytes. False on failure.
bool send_all(Session* t, const char* p, size_t n);
// Returns decrypted bytes (> 0), 0 when the peer closed, < 0 on error. Blocks on the socket only when nothing is
// buffered.
int recv(Session* t, char* buf, int n);
// True when decrypted data is available without touching the socket (select() cannot see it).
bool pending(Session* t);
// Sends close_notify (best effort) and frees the session. Does not close the socket.
void close(Session* t);

}  // namespace dcp::net::tls
