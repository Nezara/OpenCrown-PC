// TLS client over SChannel (tls.h). Pinned to TLS 1.2 through SCHANNEL_CRED: Cloudflare serves it, and it avoids
// TLS 1.3 post-handshake messages (SEC_I_RENEGOTIATE for session tickets), which this minimal client does not handle.
#define SECURITY_WIN32
#include "net/tls.h"

#include <winsock2.h>
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dcp::net::tls {

struct Session {
    SOCKET s = INVALID_SOCKET;
    CredHandle cred{};
    CtxtHandle ctx{};
    bool have_cred = false, have_ctx = false;
    SecPkgContext_StreamSizes sizes{};
    std::string raw;    // received ciphertext not yet decrypted
    std::string plain;  // decrypted bytes not yet returned
    bool eof = false;
};

namespace {

std::string hex32(long v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(v));
    return b;
}

bool sock_send(SOCKET s, const char* p, size_t n) {
    while (n) {
        int k = ::send(s, p, int(n), 0);
        if (k <= 0) return false;
        p += k, n -= size_t(k);
    }
    return true;
}

// Appends whatever the socket has (blocking). False on close/error.
bool sock_fill(Session* t) {
    char buf[16384];
    int k = ::recv(t->s, buf, sizeof buf, 0);
    if (k <= 0) return false;
    t->raw.append(buf, size_t(k));
    return true;
}

void free_session(Session* t) {
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    delete t;
}

bool insecure_allowed(const std::string& host) {
    const char* e = std::getenv("DCP_LOBBY_TLS_INSECURE");
    return e && *e == '1' && (host == "127.0.0.1" || host == "localhost");
}

// Decrypts one record from t->raw if complete. 1 = progress (data or a skipped record), 0 = need more bytes,
// -1 = error / closed.
int decrypt_one(Session* t) {
    if (t->raw.empty()) return 0;
    SecBuffer b[4];
    b[0].BufferType = SECBUFFER_DATA, b[0].pvBuffer = &t->raw[0], b[0].cbBuffer = ULONG(t->raw.size());
    for (int i = 1; i < 4; ++i) b[i].BufferType = SECBUFFER_EMPTY, b[i].pvBuffer = nullptr, b[i].cbBuffer = 0;
    SecBufferDesc d{SECBUFFER_VERSION, 4, b};
    SECURITY_STATUS st = DecryptMessage(&t->ctx, &d, 0, nullptr);
    if (st == SEC_E_INCOMPLETE_MESSAGE) return 0;
    if (st == SEC_I_CONTEXT_EXPIRED) {
        t->eof = true;
        return -1;
    }
    if (st != SEC_E_OK) {
        std::fprintf(stderr, "TLS decrypt failed: %s\n", hex32(st).c_str());
        return -1;
    }
    std::string extra;
    for (auto& x : b) {
        if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) t->plain.append(static_cast<char*>(x.pvBuffer), x.cbBuffer);
        if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer) extra.assign(t->raw.data() + t->raw.size() - x.cbBuffer, x.cbBuffer);
    }
    t->raw.swap(extra);
    return 1;
}

}  // namespace

Session* handshake(uintptr_t sock, const std::string& host, std::string* err) {
    Session* t = new Session;
    t->s = SOCKET(sock);
    bool insecure = insecure_allowed(host);

    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
                 (insecure ? SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_SERVERNAME_CHECK : SCH_CRED_AUTO_CRED_VALIDATION);
    TimeStamp exp;
    SECURITY_STATUS st = AcquireCredentialsHandleA(nullptr, const_cast<char*>(UNISP_NAME_A), SECPKG_CRED_OUTBOUND,
                                                   nullptr, &sc, nullptr, nullptr, &t->cred, &exp);
    if (st != SEC_E_OK) {
        *err = "TLS: AcquireCredentialsHandle " + hex32(st);
        free_session(t);
        return nullptr;
    }
    t->have_cred = true;

    const ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_EXTENDED_ERROR | (insecure ? ISC_REQ_MANUAL_CRED_VALIDATION : 0);
    bool first = true;
    for (;;) {
        SecBuffer ib[2], ob[1];
        ib[0].BufferType = SECBUFFER_TOKEN, ib[0].pvBuffer = t->raw.empty() ? nullptr : &t->raw[0];
        ib[0].cbBuffer = ULONG(t->raw.size());
        ib[1].BufferType = SECBUFFER_EMPTY, ib[1].pvBuffer = nullptr, ib[1].cbBuffer = 0;
        ob[0].BufferType = SECBUFFER_TOKEN, ob[0].pvBuffer = nullptr, ob[0].cbBuffer = 0;
        SecBufferDesc id{SECBUFFER_VERSION, 2, ib}, od{SECBUFFER_VERSION, 1, ob};
        ULONG attrs = 0;
        st = InitializeSecurityContextA(&t->cred, first ? nullptr : &t->ctx, const_cast<char*>(host.c_str()), req, 0, 0,
                                        first ? nullptr : &id, 0, &t->ctx, &od, &attrs, &exp);
        if (first && (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || st == SEC_I_INCOMPLETE_CREDENTIALS))
            t->have_ctx = true;
        first = false;
        if (ob[0].cbBuffer && ob[0].pvBuffer) {
            bool ok = sock_send(t->s, static_cast<char*>(ob[0].pvBuffer), ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!ok) {
                *err = "TLS: handshake send failed";
                free_session(t);
                return nullptr;
            }
        }
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (!sock_fill(t)) break;
            continue;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED && st != SEC_I_INCOMPLETE_CREDENTIALS) break;
        // Keep unprocessed bytes (SECBUFFER_EXTRA), drop consumed ones.
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer)
            t->raw = t->raw.substr(t->raw.size() - ib[1].cbBuffer);
        else
            t->raw.clear();
        if (st == SEC_E_OK) {
            QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes);
            return t;  // t->raw may already hold application data
        }
        if (st == SEC_I_CONTINUE_NEEDED && t->raw.empty() && !sock_fill(t)) break;
    }
    *err = "TLS: handshake with " + host + " failed (" + hex32(st) + ")";
    free_session(t);
    return nullptr;
}

bool send_all(Session* t, const char* p, size_t n) {
    std::vector<char> buf;
    while (n) {
        size_t k = n < t->sizes.cbMaximumMessage ? n : t->sizes.cbMaximumMessage;
        buf.resize(t->sizes.cbHeader + k + t->sizes.cbTrailer);
        std::memcpy(buf.data() + t->sizes.cbHeader, p, k);
        SecBuffer b[4];
        b[0].BufferType = SECBUFFER_STREAM_HEADER, b[0].pvBuffer = buf.data(), b[0].cbBuffer = t->sizes.cbHeader;
        b[1].BufferType = SECBUFFER_DATA, b[1].pvBuffer = buf.data() + t->sizes.cbHeader, b[1].cbBuffer = ULONG(k);
        b[2].BufferType = SECBUFFER_STREAM_TRAILER, b[2].pvBuffer = buf.data() + t->sizes.cbHeader + k;
        b[2].cbBuffer = t->sizes.cbTrailer;
        b[3].BufferType = SECBUFFER_EMPTY, b[3].pvBuffer = nullptr, b[3].cbBuffer = 0;
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&t->ctx, 0, &d, 0) != SEC_E_OK) return false;
        if (!sock_send(t->s, buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
        p += k, n -= k;
    }
    return true;
}

int recv(Session* t, char* out, int n) {
    while (t->plain.empty()) {
        int r = decrypt_one(t);
        if (r < 0) return t->eof ? 0 : -1;
        if (r > 0) continue;
        if (!sock_fill(t)) return 0;
    }
    int k = int(t->plain.size()) < n ? int(t->plain.size()) : n;
    std::memcpy(out, t->plain.data(), size_t(k));
    t->plain.erase(0, size_t(k));
    return k;
}

bool pending(Session* t) {
    while (t->plain.empty() && decrypt_one(t) > 0) {
    }
    return !t->plain.empty() || t->eof;
}

void close(Session* t) {
    if (!t) return;
    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer b{sizeof type, SECBUFFER_TOKEN, &type};
    SecBufferDesc d{SECBUFFER_VERSION, 1, &b};
    if (ApplyControlToken(&t->ctx, &d) == SEC_E_OK) {
        SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc od{SECBUFFER_VERSION, 1, &ob};
        ULONG attrs = 0;
        TimeStamp exp;
        const ULONG req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_STREAM |
                          ISC_REQ_ALLOCATE_MEMORY;
        if (InitializeSecurityContextA(&t->cred, &t->ctx, nullptr, req, 0, 0, nullptr, 0, &t->ctx, &od, &attrs, &exp) ==
                SEC_E_OK &&
            ob.pvBuffer) {
            sock_send(t->s, static_cast<char*>(ob.pvBuffer), ob.cbBuffer);
        }
        if (ob.pvBuffer) FreeContextBuffer(ob.pvBuffer);
    }
    free_session(t);
}

}  // namespace dcp::net::tls
