// UPnP IGD port mapping (upnp.h).
#include "net/upnp.h"

#include <arpa/inet.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <mutex>
#include <thread>

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include "net/carrier.h"

namespace dcp::net::upnp {
namespace {

struct Igd {
    UPNPUrls urls{};
    IGDdatas data{};
    char lan[64] = {};
    char wan[64] = {};
    bool ok = false;
    int kind = 0;  // UPNP_GetValidIGD result
    ~Igd() {
        if (ok) FreeUPNPUrls(&urls);
    }
};

bool find_igd(Igd* g, std::string* why) {
    int err = 0;
    UPNPDev* devs = upnpDiscover(kDiscoverMs, nullptr, nullptr, 0 /*UPNP_LOCAL_PORT_ANY*/, 0, 2, &err);
    if (!devs) {
        *why = "no UPnP device answered (router has UPnP off or none on this network)";
        return false;
    }
    g->kind = UPNP_GetValidIGD(devs, &g->urls, &g->data, g->lan, sizeof g->lan, g->wan, sizeof g->wan);
    freeUPNPDevlist(devs);
    if (g->kind == 0) {
        *why = "UPnP device found but no Internet Gateway Device";
        return false;
    }
    g->ok = true;
    if (g->kind == 3) {
        *why = "the gateway reports it is not connected";
        return false;
    }
    if (g->kind == 4) {
        *why = "UPnP device is not a recognised IGD";
        return false;
    }
    char ext[40] = {};
    if (UPNP_GetExternalIPAddress(g->urls.controlURL, g->data.first.servicetype, ext) == 0 && ext[0])
        std::snprintf(g->wan, sizeof g->wan, "%s", ext);
    return true;
}

// Adds local `port` (UDP); tries the same external port, then a few others on a conflict; falls back to a permanent
// lease if the router only supports those (UPnP error 725). Returns the external port or 0.
uint16_t add_mapping(Igd& g, uint16_t port, int lease_s, std::string* why) {
    char in[8];
    std::snprintf(in, sizeof in, "%u", port);
    char lease[16];
    for (int k = 0; k < 5; ++k) {
        uint16_t ext = uint16_t(port + k);
        char ex[8];
        std::snprintf(ex, sizeof ex, "%u", ext);
        // Another PC behind this router may already own the port (two players in one home). Some routers replace
        // that mapping instead of answering 718, which leaves both PCs advertising one ip:port; skip it instead.
        char cl[16] = {}, cp[6] = {}, desc[80] = {}, en[4] = {}, dur[16] = {};
        if (UPNP_GetSpecificPortMappingEntry(g.urls.controlURL, g.data.first.servicetype, ex, "UDP", nullptr, cl, cp, desc,
                                             en, dur) == 0 &&
            (std::strcmp(cl, g.lan) != 0 || std::strcmp(cp, in) != 0)) {
            *why = "UDP " + std::string(ex) + " already mapped to " + cl + ":" + cp;
            continue;
        }
        int r = -1;
        for (int attempt = 0; attempt < 2; ++attempt) {
            std::snprintf(lease, sizeof lease, "%d", attempt ? 0 : lease_s);
            r = UPNP_AddPortMapping(g.urls.controlURL, g.data.first.servicetype, ex, in, g.lan, "OpenCrown", "UDP",
                                    nullptr, lease);
            if (r != 725) break;  // OnlyPermanentLeasesSupported -> retry with lease 0
        }
        if (r == 0) return ext;
        const char* s = strupnperror(r);
        *why = "AddPortMapping failed: " + std::to_string(r) + (s ? std::string(" ") + s : std::string());
        if (r != 718 && r != 727) break;  // ConflictInMappingEntry / ExternalPortOnlySupportsWildcard: try next port
    }
    return 0;
}

void del_mapping(Igd& g, uint16_t ext) {
    char ex[8];
    std::snprintf(ex, sizeof ex, "%u", ext);
    UPNP_DeletePortMapping(g.urls.controlURL, g.data.first.servicetype, ex, "UDP", nullptr);
}

// Game-side mapping state (one P2P socket in practice; the hook keeps the latest).
std::mutex g_mu;
std::condition_variable g_cv;
Status g_status;
Igd* g_igd = nullptr;
std::thread g_worker;
bool g_stop = false;
bool g_atexit = false;

void remove_locked_mapping() {  // g_mu held
    if (g_igd && g_status.state == State::Mapped) {
        del_mapping(*g_igd, g_status.ext_port);
        std::fprintf(stderr, "UPNP removed UDP %u -> %u\n", g_status.ext_port, g_status.local_port);
    }
    delete g_igd;
    g_igd = nullptr;
}

void stop_worker() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    std::lock_guard<std::mutex> lk(g_mu);
    remove_locked_mapping();
    g_stop = false;
}

void worker(uint16_t port) {
    auto* g = new Igd;
    std::string why;
    uint16_t ext = 0;
    if (find_igd(g, &why)) ext = add_mapping(*g, port, kLeaseSeconds, &why);
    std::unique_lock<std::mutex> lk(g_mu);
    if (ext) {
        g_igd = g;
        g_status.state = State::Mapped, g_status.ext_port = ext, g_status.ext_ip = g->wan;
        g_status.message = "mapped " + std::string(g->wan) + ":" + std::to_string(ext) + " -> " + g->lan + ":" +
                           std::to_string(port) + (g->kind == 2 ? " (router WAN address is private: double NAT/CGNAT)" : "");
    } else {
        delete g;
        g_status.state = State::Failed, g_status.message = why;
    }
    std::fprintf(stderr, "UPNP UDP %u: %s\n", port, g_status.message.c_str());
    if (!ext) return;
    // Renew at half-life until stopped.
    while (!g_cv.wait_for(lk, std::chrono::seconds(kLeaseSeconds / 2), [] { return g_stop; })) {
        std::string w;
        if (!add_mapping(*g_igd, port, kLeaseSeconds, &w)) std::fprintf(stderr, "UPNP renew failed: %s\n", w.c_str());
    }
}

void hook(uint16_t port, bool open) {
    stop_worker();  // at most one mapping; removes the previous one
    if (!open) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_status = Status{};
        g_status.state = State::Pending, g_status.local_port = port, g_status.message = "discovering";
        if (!g_atexit) g_atexit = true, std::atexit([] { stop_worker(); });
    }
    g_worker = std::thread(worker, port);
}

}  // namespace

bool enabled() {
    const char* e = std::getenv("DCP_UPNP");
    if (!e || !*e) return true;
    return !(std::strcmp(e, "0") == 0 || strcasecmp(e, "off") == 0 || strcasecmp(e, "false") == 0);
}

void install_hook() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!enabled()) {
        std::fprintf(stderr, "UPNP disabled ($DCP_UPNP)\n");
        return;
    }
    set_port_map_hook(hook);
}

Status status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

Status probe(uint16_t port, int lease_s, bool remove_after) {
    Status st;
    st.local_port = port;
    Igd g;
    std::string why;
    uint16_t ext = 0;
    if (find_igd(&g, &why)) ext = add_mapping(g, port, lease_s, &why);
    if (!ext) {
        st.state = State::Failed, st.message = why;
        return st;
    }
    st.state = State::Mapped, st.ext_ip = g.wan, st.ext_port = ext;
    st.message = "mapped " + std::string(g.wan) + ":" + std::to_string(ext) + " -> " + g.lan + ":" + std::to_string(port) +
                 (g.kind == 2 ? " (router WAN address is private: double NAT/CGNAT)" : "");
    if (remove_after) del_mapping(g, ext);
    return st;
}

}  // namespace dcp::net::upnp
