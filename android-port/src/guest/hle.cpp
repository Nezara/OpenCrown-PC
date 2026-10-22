#include "guest/hle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>

#include "guest/guest_prof.h"

namespace guest {

std::unordered_map<uint32_t, HleEntry>& hle_table() {
  static std::unordered_map<uint32_t, HleEntry> t;
  return t;
}

void hle_register(uint32_t nid, const char* name, HleFn fn) { hle_table()[nid] = {name, fn}; }

namespace {
struct HleVar {
  const char* name;
  HleVarFn make;
  uint32_t address = 0;
};
std::unordered_map<uint32_t, HleVar>& var_table() {
  static std::unordered_map<uint32_t, HleVar> t;
  return t;
}
}  // namespace

void hle_register_var(uint32_t nid, const char* name, HleVarFn make) { var_table()[nid] = {name, std::move(make)}; }

uint32_t hle_var_address(Memory& mem, uint32_t nid) {
  auto it = var_table().find(nid);
  if (it == var_table().end()) return 0;
  if (!it->second.address) it->second.address = it->second.make(mem);
  return it->second.address;
}

// Per host thread: calls and time per import, for the frame timing log. Written by the owning thread (counters only
// grow), read by the timing log on another thread (relaxed atomics; the reporter keeps its own last-seen copy) and by
// the owner's spike-log segments (svc_segment_*; own copy). Each thread's block is in a list the reporter walks.
namespace {
struct SvcCounts {
  std::thread::id owner;
  const Hle* hle = nullptr;
  const Module* mod = nullptr;
  std::unique_ptr<std::atomic<uint32_t>[]> calls;
  std::unique_ptr<std::atomic<uint64_t>[]> ns;
  std::unique_ptr<uint32_t[]> rep_calls, seg_calls;  // reporter's / segment start's copies
  std::unique_ptr<uint64_t[]> rep_ns, seg_ns;
  size_t n = 0;
  std::atomic<int32_t> now{-1};  // DCP_LOCK_STATS: the import running now, -1 = guest code
};
std::mutex svc_counts_lock;
std::vector<SvcCounts*> svc_counts;  // never freed: a few per guest thread
thread_local SvcCounts* my_counts = nullptr;
thread_local int svc_depth = 0;          // imports in progress on this thread (HLE -> guest -> import nests)
thread_local int seg_pending_depth = 0;  // > 0: take the segment snapshot once the import at that depth returned

// Imports that block the caller (waits, locks, sleeps, joins, audio output pacing): time in them is "blocked" in a
// spike-log segment. Prefix match on the function name.
const char* const BLOCKING[] = {"sceKernelWait",        "sceKernelLockMutex",        "sceKernelLockLwMutex",
                                "sceKernelDelayThread", "sceUltUlthreadJoin",        "sceGxmDisplayQueueFinish",
                                "sceAudioOutOutput",    "sceDisplayWaitVblankStart", "sceRudpPollWait"};
std::vector<char> blocking;  // per import index, built once
std::once_flag blocking_once;

// DCP_IMPORT_CALLERS=a,b: guest return address histogram for those imports (exact function names).
struct CallerStat { uint64_t calls = 0, ns = 0; };
std::vector<char> callers_on;  // per import index
std::once_flag callers_once;
std::mutex callers_lock;
std::unordered_map<uint32_t, std::unordered_map<uint32_t, CallerStat>> callers;  // import -> LR -> stats
const Hle* callers_hle = nullptr;
const Module* callers_mod = nullptr;

uint64_t now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string short_name(const Hle& h, const ImportFunc& f) {
  std::string name = h.describe(f);
  if (size_t sp = name.rfind(' '); sp != std::string::npos) name = name.substr(sp + 1);
  return name;
}

bool track_now() {
  static const bool on = [] {
    const char* e = std::getenv("DCP_LOCK_STATS");
    return e && *e == '1';
  }();
  return on;
}

std::string now_name(const SvcCounts& c, bool* blocking_out) {
  int32_t i = c.now.load(std::memory_order_relaxed);
  if (blocking_out) *blocking_out = i >= 0 && size_t(i) < blocking.size() && blocking[i];
  return i < 0 || size_t(i) >= c.n ? std::string() : short_name(*c.hle, c.mod->imports[i]);
}

void take_segment_snapshot(SvcCounts& c) {
  for (size_t i = 0; i < c.n; i++) {
    c.seg_calls[i] = c.calls[i].load(std::memory_order_relaxed);
    c.seg_ns[i] = c.ns[i].load(std::memory_order_relaxed);
  }
}
}  // namespace

std::string svc_report_for(std::thread::id id, double frames) {
  SvcCounts* c = nullptr;
  {
    std::lock_guard lk(svc_counts_lock);
    for (SvcCounts* s : svc_counts)
      if (s->owner == id) c = s;
  }
  if (!c) return {};
  struct Row { size_t i; uint32_t calls; uint64_t ns; };
  std::vector<Row> rows;
  for (size_t i = 0; i < c->n; i++) {
    uint32_t k = c->calls[i].load(std::memory_order_relaxed);
    uint64_t t = c->ns[i].load(std::memory_order_relaxed);
    uint32_t dk = k - c->rep_calls[i];
    uint64_t dt = t - c->rep_ns[i];
    c->rep_calls[i] = k, c->rep_ns[i] = t;
    if (dk) rows.push_back({i, dk, dt});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
  std::string out;
  char b[160];
  for (size_t j = 0; j < rows.size() && j < 3; j++) {
    std::string name = short_name(*c->hle, c->mod->imports[rows[j].i]);
    std::snprintf(b, sizeof b, "%s%s %.0fx %.2fms", j ? ", " : "", name.c_str(), rows[j].calls / frames,
                  rows[j].ns / frames / 1e6);
    out += b;
  }
  return out;
}

std::string svc_current_import(std::thread::id id, bool* blocking_out) {
  if (blocking_out) *blocking_out = false;
  if (!track_now()) return {};
  std::lock_guard lk(svc_counts_lock);
  for (SvcCounts* s : svc_counts)
    if (s->owner == id) return now_name(*s, blocking_out);
  return {};
}

std::string svc_current_import_self(bool* blocking_out) {
  if (blocking_out) *blocking_out = false;
  return track_now() && my_counts ? now_name(*my_counts, blocking_out) : std::string();
}

void svc_segment_start() {
  if (!my_counts) return;  // no import yet: the first one creates the block with a zero snapshot
  if (svc_depth > 0)
    seg_pending_depth = svc_depth;  // called from inside an import: its own time belongs to neither segment
  else
    take_segment_snapshot(*my_counts);
}

SegmentImports svc_segment_end(bool want_text) {
  SegmentImports r;
  SvcCounts* c = my_counts;
  if (!c) return r;
  seg_pending_depth = 0;
  struct Row { size_t i; uint32_t calls; uint64_t ns; };
  std::vector<Row> rows;
  for (size_t i = 0; i < c->n; i++) {
    uint32_t dk = c->calls[i].load(std::memory_order_relaxed) - c->seg_calls[i];
    if (!dk) continue;
    uint64_t dt = c->ns[i].load(std::memory_order_relaxed) - c->seg_ns[i];
    r.import_ns += dt;
    if (blocking.size() > i && blocking[i]) r.blocked_ns += dt;
    if (want_text) rows.push_back({i, dk, dt});
  }
  if (!want_text) return r;
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
  char b[160];
  for (size_t j = 0; j < rows.size() && j < 4; j++) {
    std::snprintf(b, sizeof b, "%s%s %ux %.2fms", j ? ", " : "", short_name(*c->hle, c->mod->imports[rows[j].i]).c_str(),
                  rows[j].calls, rows[j].ns / 1e6);
    r.text += b;
  }
  return r;
}

std::string import_callers_report(double frames) {
  std::vector<std::pair<uint32_t, std::vector<std::pair<uint32_t, CallerStat>>>> all;
  {
    std::lock_guard lk(callers_lock);
    for (auto& [imm, m] : callers) all.push_back({imm, {m.begin(), m.end()}});
    callers.clear();
  }
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out;
  char b[160];
  for (auto& [imm, v] : all) {
    uint64_t total = 0, total_ns = 0;
    for (auto& [lr, s] : v) total += s.calls, total_ns += s.ns;
    std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second.calls > y.second.calls; });
    std::snprintf(b, sizeof b, "  callers %s (%.0fx %.2fms/frame, %zu sites):",
                  short_name(*callers_hle, callers_mod->imports[imm]).c_str(), total / frames, total_ns / frames / 1e6,
                  v.size());
    out += b;
    for (size_t j = 0; j < v.size() && j < 5; j++) {
      std::snprintf(b, sizeof b, " %08x %.0fx %.2fms", v[j].first, v[j].second.calls / frames,
                    v[j].second.ns / frames / 1e6);
      out += b;
    }
    out += "\n";
  }
  return out;
}

void Hle::load_names(const std::string& path) {
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string nid, name;
    if (line.empty() || line[0] == '#' || !(ss >> nid >> name)) continue;
    char* end = nullptr;
    unsigned long v = std::strtoul(nid.c_str(), &end, 16);
    if (end && *end == '\0') names_[uint32_t(v)] = name;
  }
}

std::string Hle::describe(const ImportFunc& f) const {
  char b[64];
  std::snprintf(b, sizeof b, "%s 0x%08x", f.lib.c_str(), f.nid);
  std::string s = b;
  auto it = hle_table().find(f.nid);
  if (it != hle_table().end()) return s + " " + it->second.name;
  auto n = names_.find(f.nid);
  return n != names_.end() ? s + " " + n->second : s;
}

bool Hle::traced(uint32_t imm) {
  if (opt_.trace_only.empty()) return true;
  std::call_once(traced_once_, [&] {
    traced_.assign(mod_.imports.size(), 0);
    for (size_t i = 0; i < mod_.imports.size(); i++)
      for (const std::string& s : opt_.trace_only)
        if (describe(mod_.imports[i]).find(s) != std::string::npos) traced_[i] = 1;
  });
  return traced_[imm];
}

void Hle::on_svc(Cpu& cpu, uint32_t imm) {
  if (imm >= mod_.imports.size()) {
    char b[64];
    std::snprintf(b, sizeof b, "svc #0x%x at 0x%08x is not an import stub", imm, cpu.pc());
    cpu.set_fault(b);
    return;
  }
  const ImportFunc& f = mod_.imports[imm];
  auto it = hle_table().find(f.nid);
  const uint32_t lr_at_entry = cpu.reg(14) & ~1u;
  if (opt_.trace && traced(imm))
    std::fprintf(stderr, "[trace] core %p %s(%08x, %08x, %08x, %08x) from 0x%08x\n", static_cast<void*>(&cpu),
                 describe(f).c_str(), cpu.reg(0),
                 cpu.reg(1), cpu.reg(2), cpu.reg(3), cpu.reg(14) & ~1u);
  if (it != hle_table().end()) {
    if (!my_counts) {
      std::call_once(blocking_once, [&] {
        blocking.assign(mod_.imports.size(), 0);
        for (size_t i = 0; i < mod_.imports.size(); i++) {
          std::string name = short_name(*this, mod_.imports[i]);
          for (const char* p : BLOCKING)
            if (name.compare(0, std::strlen(p), p) == 0) blocking[i] = 1;
        }
      });
      auto* c = new SvcCounts;
      c->owner = std::this_thread::get_id();
      c->hle = this;
      c->mod = &mod_;
      c->n = mod_.imports.size();
      c->calls.reset(new std::atomic<uint32_t>[c->n]());
      c->ns.reset(new std::atomic<uint64_t>[c->n]());
      c->rep_calls.reset(new uint32_t[c->n]());
      c->seg_calls.reset(new uint32_t[c->n]());
      c->rep_ns.reset(new uint64_t[c->n]());
      c->seg_ns.reset(new uint64_t[c->n]());
      {
        std::lock_guard lk(svc_counts_lock);
        svc_counts.push_back(c);
      }
      my_counts = c;
    }
    std::call_once(callers_once, [&] {
      callers_on.assign(mod_.imports.size(), 0);
      callers_hle = this, callers_mod = &mod_;
      const char* e = std::getenv("DCP_IMPORT_CALLERS");
      if (!e || !*e) return;
      std::string list = e, on;
      for (size_t p = 0; p <= list.size();) {
        size_t q = std::min(list.find(',', p), list.size());
        std::string want = list.substr(p, q - p);
        for (size_t i = 0; i < mod_.imports.size() && !want.empty(); i++)
          if (short_name(*this, mod_.imports[i]) == want) callers_on[i] = 1, on += " " + want;
        p = q + 1;
      }
      std::fprintf(stderr, "[hle] import callers recorded for:%s\n", on.empty() ? " (no import matched)" : on.c_str());
    });
    const bool track = track_now();
    int32_t prev_now = track ? my_counts->now.exchange(int32_t(imm), std::memory_order_relaxed) : -1;
    uint64_t t0 = now_ns();
    svc_depth++;
    {
      prof::Scope in_import{int32_t(imm)};
      it->second.fn(cpu);
    }
    svc_depth--;
    if (track) my_counts->now.store(prev_now, std::memory_order_relaxed);
    uint64_t dt = now_ns() - t0;
    my_counts->calls[imm].fetch_add(1, std::memory_order_relaxed);
    my_counts->ns[imm].fetch_add(dt, std::memory_order_relaxed);
    if (seg_pending_depth > svc_depth) {
      seg_pending_depth = 0;
      take_segment_snapshot(*my_counts);
    }
    if (callers_on[imm]) {
      std::lock_guard lk(callers_lock);
      CallerStat& s = callers[imm][lr_at_entry];
      s.calls++, s.ns += dt;
    }
    if (opt_.trace && traced(imm))
      std::fprintf(stderr, "[trace] core %p   -> %08x\n", static_cast<void*>(&cpu), cpu.reg(0));
    return;
  }
  unimplemented_calls++;
  std::unique_lock lk(mu_);
  if (reported_.empty()) reported_.assign(mod_.imports.size(), false);
  if (!reported_[imm]) {
    reported_[imm] = true;
    std::fprintf(stderr, "[hle] UNIMPL %s  (stub 0x%08x, from 0x%08x, r0=%08x r1=%08x r2=%08x r3=%08x)\n",
                 describe(f).c_str(), f.stub, cpu.reg(14) & ~1u, cpu.reg(0), cpu.reg(1), cpu.reg(2), cpu.reg(3));
  }
  if (opt_.keep_going) {
    cpu.set_reg(0, 0);
    return;
  }
  cpu.set_fault("unimplemented import " + describe(f));
}

}  // namespace guest
