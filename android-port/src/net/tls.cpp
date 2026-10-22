// TLS client over mbedTLS (tls.h). Pinned to TLS 1.2 like the Pro port's Windows (SChannel) build: Cloudflare serves
// it, and every build then speaks the same protocol to the lobby. The server certificate chain is verified against the
// system's trust anchors and the host name: Windows = the ROOT system certificate store, Android = the CA folder
// (one PEM per file; the updatable Conscrypt APEX copy on Android 14+, else /system/etc/security/cacerts).
// $SSL_CERT_FILE (a PEM bundle) replaces both.
#include "net/tls.h"

#include "net/sock.h"

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <dirent.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace dcp::net::tls {

struct Session {
    sock::Socket s = sock::kInvalid;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    bool eof = false;
    Session() {
        mbedtls_ssl_config_init(&conf);
        mbedtls_ssl_init(&ssl);
    }
    ~Session() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
};

namespace {

std::string err_str(int e) {
    char b[160];
    mbedtls_strerror(e, b, sizeof b);
    char h[32];
    std::snprintf(h, sizeof h, " (-0x%04x)", unsigned(-e));
    return std::string(b) + h;
}

// BIO callbacks on the caller's blocking socket (its SO_RCVTIMEO bounds the handshake).
int bio_send(void* ctx, const unsigned char* p, size_t n) {
    int k = sock::send(*static_cast<sock::Socket*>(ctx), p, n);
    if (k >= 0) return k;
    return sock::interrupted(sock::last_error()) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}
int bio_recv(void* ctx, unsigned char* p, size_t n) {
    int k = sock::recv(*static_cast<sock::Socket*>(ctx), p, n);
    if (k >= 0) return k;  // 0 = peer closed (mbedTLS reports CONN_EOF)
    return sock::interrupted(sock::last_error()) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

// mbedTLS 3.6 still wants an RNG callback on the SSL config (nonces, ECDHE keys); PSA's own DRBG serves it.
int psa_rng(void*, unsigned char* out, size_t n) {
    return psa_generate_random(out, n) == PSA_SUCCESS ? 0 : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

bool insecure_allowed(const std::string& host) {
    const char* e = std::getenv("DCP_LOBBY_TLS_INSECURE");
    return e && *e == '1' && (host == "127.0.0.1" || host == "localhost");
}

#ifdef _WIN32
constexpr const char* kStoreName = "the Windows ROOT certificate store";
// Every certificate of the ROOT system store (DER); ones mbedTLS cannot parse (odd legacy roots) are skipped.
void load_system_store(mbedtls_x509_crt* ca) {
    HCERTSTORE st = CertOpenSystemStoreW(0, L"ROOT");
    if (!st) return;
    for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(st, c)) != nullptr;)
        if (c->dwCertEncodingType & X509_ASN_ENCODING)
            mbedtls_x509_crt_parse_der(ca, c->pbCertEncoded, c->cbCertEncoded);
    CertCloseStore(st, 0);
}
#else
constexpr const char* kStoreName = "/apex/com.android.conscrypt/cacerts or /system/etc/security/cacerts";
// Every file of a CA folder (Android: one PEM certificate per file, named <subject hash>.<n>). Returns the count added.
int load_ca_dir(mbedtls_x509_crt* ca, const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (mbedtls_x509_crt_parse_file(ca, (dir + "/" + e->d_name).c_str()) == 0) ++n;
    }
    closedir(d);
    return n;
}
void load_system_store(mbedtls_x509_crt* ca) {
    // Android 14+ keeps the updatable set in the Conscrypt APEX; the /system copy is the older, frozen one.
    if (load_ca_dir(ca, "/apex/com.android.conscrypt/cacerts") == 0) load_ca_dir(ca, "/system/etc/security/cacerts");
}
#endif

// Trust anchors, loaded once (shared read-only by every session): $SSL_CERT_FILE, else the system store. nullptr when
// none could be loaded.
mbedtls_x509_crt* ca_chain() {
    static mbedtls_x509_crt ca;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        mbedtls_x509_crt_init(&ca);
        if (const char* env = std::getenv("SSL_CERT_FILE"); env && *env)
            mbedtls_x509_crt_parse_file(&ca, env);  // >= 0: parsed (a positive count = some certificates skipped)
        else
            load_system_store(&ca);
        ok = ca.version != 0;  // at least one certificate in the chain
    });
    return ok ? &ca : nullptr;
}

}  // namespace

Session* handshake(uintptr_t sock, const std::string& host, std::string* err) {
    static std::once_flag psa_once;
    static psa_status_t psa = PSA_SUCCESS;
    std::call_once(psa_once, [] { psa = psa_crypto_init(); });  // RNG + crypto backend of mbedTLS 3.x
    if (psa != PSA_SUCCESS) {
        *err = "TLS: psa_crypto_init failed (" + std::to_string(int(psa)) + ")";
        return nullptr;
    }
    Session* t = new Session;
    t->s = sock::Socket(sock);
    bool insecure = insecure_allowed(host);
    mbedtls_x509_crt* ca = insecure ? nullptr : ca_chain();
    if (!insecure && !ca) {
        *err = std::string("TLS: no CA certificates loaded from ") + kStoreName + " ($SSL_CERT_FILE overrides)";
        delete t;
        return nullptr;
    }
    int r = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
    if (r == 0) {
        mbedtls_ssl_conf_min_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
        mbedtls_ssl_conf_authmode(&t->conf, insecure ? MBEDTLS_SSL_VERIFY_NONE : MBEDTLS_SSL_VERIFY_REQUIRED);
        if (ca) mbedtls_ssl_conf_ca_chain(&t->conf, ca, nullptr);
        mbedtls_ssl_conf_rng(&t->conf, psa_rng, nullptr);
        r = mbedtls_ssl_setup(&t->ssl, &t->conf);
    }
    if (r == 0) r = mbedtls_ssl_set_hostname(&t->ssl, host.c_str());  // SNI + certificate name check
    if (r != 0) {
        *err = "TLS: setup " + err_str(r);
        delete t;
        return nullptr;
    }
    mbedtls_ssl_set_bio(&t->ssl, &t->s, bio_send, bio_recv, nullptr);
    while ((r = mbedtls_ssl_handshake(&t->ssl)) != 0) {
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        *err = "TLS: handshake with " + host + " failed: " + err_str(r);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char why[256];
            mbedtls_x509_crt_verify_info(why, sizeof why, "", mbedtls_ssl_get_verify_result(&t->ssl));
            std::string w = why;
            while (!w.empty() && w.back() == '\n') w.pop_back();
            *err += " [" + w + "]";
        }
        delete t;
        return nullptr;
    }
    return t;
}

bool send_all(Session* t, const char* p, size_t n) {
    while (n) {
        int k = mbedtls_ssl_write(&t->ssl, reinterpret_cast<const unsigned char*>(p), n);
        if (k == MBEDTLS_ERR_SSL_WANT_READ || k == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (k <= 0) return false;
        p += k, n -= size_t(k);
    }
    return true;
}

int recv(Session* t, char* out, int n) {
    if (t->eof) return 0;
    for (;;) {
        int k = mbedtls_ssl_read(&t->ssl, reinterpret_cast<unsigned char*>(out), size_t(n));
        if (k > 0) return k;
        if (k == MBEDTLS_ERR_SSL_WANT_READ || k == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (k == 0 || k == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || k == MBEDTLS_ERR_SSL_CONN_EOF ||
            k == MBEDTLS_ERR_NET_RECV_FAILED) {
            t->eof = k != MBEDTLS_ERR_NET_RECV_FAILED;
            return 0;  // closed, or the socket failed / timed out
        }
        std::fprintf(stderr, "TLS decrypt failed: %s\n", err_str(k).c_str());
        return -1;
    }
}

bool pending(Session* t) {
    // mbedTLS reads one record at a time from the socket, so undecrypted bytes stay visible to select(); only
    // plaintext left over from a record (or a seen close) is invisible to it.
    return mbedtls_ssl_get_bytes_avail(&t->ssl) > 0 || t->eof;
}

void close(Session* t) {
    if (!t) return;
    if (!t->eof) mbedtls_ssl_close_notify(&t->ssl);  // best effort
    delete t;
}

}  // namespace dcp::net::tls
