// Launcher "Report a Bug" (lobby_protocol.md 4.4): one short blocking session - connect, verified hello ("presence":
// false), bug_report with the user's note and the redacted tail of the last game log, close. Run it on a worker
// thread. `log` must already be redacted (redact_log).
// The lobby limits reports per account, per IP and in total (rolling 24 h) and says how many are left.
#pragma once
#include <string>

#include "net/identity.h"

namespace dcp::net {

constexpr size_t kBugReportTextMax = 256;      // characters (the lobby refuses longer notes)
constexpr size_t kBugReportLogMax = 128 * 1024; // bytes of the log's tail that are sent

struct BugReportResult {
    bool ok = false;
    std::string msg;  // shown under the Send button
    int left = -1;    // reports left today (-1 = unknown)
};

// Last max_bytes of a text file, made JSON/UTF-8 safe (non-ASCII bytes -> '?'); "" if unreadable.
std::string log_tail_text(const std::string& path, size_t max_bytes);

// What the report carries of a log: personal data replaced with placeholders - the identity secret and long hex runs
// (<secret>, <hex>), IPv4 addresses except loopback (<ip>), other players' account ids (<id>), the profile / home
// folder, user name and computer name (Windows: %USERPROFILE%, <user>, <pc>; Android: the app's uid name as <user>).
// The launcher shows this text before sending.
std::string redact_log(std::string log, const Identity& id);

// True when the identity secret may be sent to this lobby URL: wss://, or plain ws:// to this PC / a LAN address.
bool lobby_url_safe_for_secret(const std::string& url);

BugReportResult send_bug_report(const std::string& lobby_url, const std::string& name, const Identity& id,
                                const std::string& text, const std::string& log, const std::string& version);

}  // namespace dcp::net
