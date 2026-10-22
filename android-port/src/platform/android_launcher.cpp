// JNI side of the Android launcher (android/java/org/opencrown/mobile/Net.java): Test connection (net/online_check.h),
// the friends session (net/friends.h) and Report a Bug (net/bug_report.h), the same calls the PC launcher makes.
// Blocking; Net.java runs them on a worker thread. Results go back as text lines of tab-separated fields (no JNI
// object building); tabs and line breaks inside values are replaced by spaces.
#include <jni.h>

#include <string>

#include "net/bug_report.h"
#include "net/friends.h"
#include "net/identity.h"
#include "net/online_check.h"

namespace {

std::string jstr(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

std::string clean(std::string s) {
  for (char& ch : s)
    if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
  return s;
}

void line(std::string& out, std::initializer_list<std::string> fields) {
  bool first = true;
  for (const std::string& f : fields) {
    if (!first) out += '\t';
    out += clean(f);
    first = false;
  }
  out += '\n';
}

// identity.ini lives in the launcher's OpenCrown folder, as the game's --root.
dcp::net::Identity identity(JNIEnv* env, jstring root) {
  dcp::net::set_user_dir(jstr(env, root));
  return dcp::net::load_identity(dcp::net::identity_dir());
}

}  // namespace

// Lines: verdict, lobby <ok> <msg> <ip> <ms>, stun <ok> <msg> <ip> <port> <nat_same>, upnp <tried> <ok> <msg>.
extern "C" JNIEXPORT jstring JNICALL Java_org_opencrown_mobile_Net_onlineCheck(JNIEnv* env, jclass, jstring lobby,
                                                                              jstring name, jboolean upnp) {
  const dcp::net::OnlineCheck c = dcp::net::run_online_check(jstr(env, lobby), jstr(env, name), upnp, 3658);
  std::string out;
  line(out, {"verdict", c.verdict});
  line(out, {"lobby", c.lobby_ok ? "1" : "0", c.lobby_msg, c.your_ip, std::to_string(c.lobby_ms)});
  line(out, {"stun", c.stun_ok ? "1" : "0", c.stun_msg, c.public_ip, std::to_string(c.public_port),
             std::to_string(c.nat_same)});
  line(out, {"upnp", c.upnp_tried ? "1" : "0", c.upnp_ok ? "1" : "0", c.upnp_msg});
  return env->NewStringUTF(out.c_str());
}

// action: 0 none, 1 request (arg = friend code), 2 accept, 3 decline / cancel, 4 remove (arg = account id).
// Lines: identity <ok> <error>, state <ok> <error>, code <XXXX-XXXX>, action <ok> <msg>,
// friend <id> <name> <online> <join code>, in <id> <name> <ms>, out <id> <name> <ms>.
extern "C" JNIEXPORT jstring JNICALL Java_org_opencrown_mobile_Net_friends(JNIEnv* env, jclass, jstring root,
                                                                          jstring lobby, jstring name, jint action,
                                                                          jstring arg) {
  const dcp::net::Identity id = identity(env, root);
  std::string out;
  line(out, {"identity", id.ok ? "1" : "0", id.error});
  if (!id.ok) return env->NewStringUTF(out.c_str());
  dcp::net::FriendsAction a;
  if (action >= dcp::net::FriendsAction::Request && action <= dcp::net::FriendsAction::Remove)
    a.kind = dcp::net::FriendsAction::Kind(action), a.arg = jstr(env, arg);
  const dcp::net::FriendsState st = dcp::net::run_friends_session(jstr(env, lobby), jstr(env, name), id, a);
  line(out, {"state", st.ok ? "1" : "0", st.error});
  if (!st.code.empty()) line(out, {"code", dcp::net::friend_code_display(st.code)});
  if (a.kind != dcp::net::FriendsAction::None) line(out, {"action", st.action_ok ? "1" : "0", st.action_msg});
  for (const auto& f : st.friends) line(out, {"friend", f.account_id, f.name, f.online ? "1" : "0", f.join_code});
  for (const auto& r : st.incoming) line(out, {"in", r.account_id, r.name, std::to_string(r.ms)});
  for (const auto& r : st.outgoing) line(out, {"out", r.account_id, r.name, std::to_string(r.ms)});
  return env->NewStringUTF(out.c_str());
}

// What a bug report would carry of the last game log (log.txt, redacted, last kBugReportLogMax bytes); "" if none.
extern "C" JNIEXPORT jstring JNICALL Java_org_opencrown_mobile_Net_bugLog(JNIEnv* env, jclass, jstring root,
                                                                         jstring log_path) {
  const dcp::net::Identity id = identity(env, root);
  const std::string tail = dcp::net::log_tail_text(jstr(env, log_path), dcp::net::kBugReportLogMax);
  return env->NewStringUTF(tail.empty() ? "" : dcp::net::redact_log(tail, id).c_str());
}

// Lines: result <ok> <msg> <left>.
extern "C" JNIEXPORT jstring JNICALL Java_org_opencrown_mobile_Net_bugReport(JNIEnv* env, jclass, jstring root,
                                                                            jstring lobby, jstring name, jstring note,
                                                                            jstring log_path, jstring version) {
  const dcp::net::Identity id = identity(env, root);
  std::string out;
  if (!id.ok) {
    line(out, {"result", "0", "No online identity: " + id.error, "-1"});
    return env->NewStringUTF(out.c_str());
  }
  const std::string tail = dcp::net::log_tail_text(jstr(env, log_path), dcp::net::kBugReportLogMax);
  const dcp::net::BugReportResult r = dcp::net::send_bug_report(
      jstr(env, lobby), jstr(env, name), id, jstr(env, note), tail.empty() ? "" : dcp::net::redact_log(tail, id),
      jstr(env, version));
  line(out, {"result", r.ok ? "1" : "0", r.msg, std::to_string(r.left)});
  return env->NewStringUTF(out.c_str());
}
