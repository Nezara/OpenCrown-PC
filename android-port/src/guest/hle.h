// Our own implementations of the Vita system functions the game imports, looked up by NID.
// A handler reads its arguments from r0-r3 (+ the stack) and leaves the result in r0 (r0:r1 for 64-bit).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest/cpu.h"
#include "guest/module.h"

namespace guest {

using HleFn = void (*)(Cpu&);

struct HleEntry {
  const char* name;
  HleFn fn;
};

// Registry filled by each library's register_*() at start-up.
std::unordered_map<uint32_t, HleEntry>& hle_table();
void hle_register(uint32_t nid, const char* name, HleFn fn);

// Variable imports: `make` builds the variable in guest memory (once) and returns its address.
using HleVarFn = std::function<uint32_t(Memory&)>;
void hle_register_var(uint32_t nid, const char* name, HleVarFn make);
// Address of our variable for `nid` (built on first use), 0 if we don't provide it.
uint32_t hle_var_address(Memory& mem, uint32_t nid);

struct HleOptions {
  bool keep_going = false;  // unimplemented import: log it, return 0 and continue instead of stopping
  bool trace = false;       // log every import call (name, r0-r3, caller)
  std::vector<std::string> trace_only;  // if set, trace only imports whose name contains one of these
};

// Import call statistics for the calling host thread since its last report: the most called / most expensive
// imports as "name calls ms" text, per frame (frame timing log). Empty when the thread made no import calls.
std::string svc_report_for(std::thread::id thread, double frames);

// Spike log segments (hle/frame_stats.cpp), calling thread only: svc_segment_start() marks the start of a frame work
// segment (from inside an import: once that import has returned, so the marking wait is in neither segment);
// svc_segment_end() = the imports since then: total / blocking time (waits, locks, sleeps) and, if asked, the top 4
// by time as "name calls ms" text.
struct SegmentImports {
  uint64_t import_ns = 0, blocked_ns = 0;
  std::string text;
};
void svc_segment_start();
SegmentImports svc_segment_end(bool want_text);

// DCP_LOCK_STATS=1 only (hle/sync.cpp contention report; "" otherwise): the import the given host thread / the
// calling thread is in right now, "" = guest code. *blocking = one of the blocking imports (waits, locks, sleeps).
std::string svc_current_import(std::thread::id thread, bool* blocking);
std::string svc_current_import_self(bool* blocking);

// DCP_IMPORT_CALLERS=name,name: per named import the top 5 guest callers (LR) since the last call, calls and ms per
// frame, one "  callers ..." line each. Empty when the switch is off.
std::string import_callers_report(double frames);

class Hle {
 public:
  Hle(const Module& mod, HleOptions opt) : mod_(mod), opt_(opt) {}
  // Optional "nid name" text file (one per line, hex nid) for log names of imports we don't implement yet.
  void load_names(const std::string& path);
  void on_svc(Cpu& cpu, uint32_t imm);
  std::string describe(const ImportFunc& f) const;

  std::atomic<size_t> unimplemented_calls{0};

 private:
  const Module& mod_;
  HleOptions opt_;
  std::vector<char> traced_;  // per import, built on first use
  std::once_flag traced_once_;
  bool traced(uint32_t imm);
  std::unordered_map<uint32_t, std::string> names_;
  std::mutex mu_;  // guards reported_ (any guest thread can hit an unimplemented import)
  std::vector<bool> reported_;
};

}  // namespace guest
