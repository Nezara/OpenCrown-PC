// Process-wide socket setup (sock.h).
#include "net/sock.h"

#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dcp::net::sock {

bool startup() {
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
        signal(SIGPIPE, SIG_IGN);
        ok = true;
#endif
    });
    return ok;
}

uint32_t process_id() {
#ifdef _WIN32
    return uint32_t(GetCurrentProcessId());
#else
    return uint32_t(getpid());
#endif
}

}  // namespace dcp::net::sock
