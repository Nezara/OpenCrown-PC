// SceUlt (the game-bundled libult.suprx), our own version: a runtime owns N worker guest threads that run queued
// ulthreads to completion, each on its own context (stack) given by the game. Argument layouts from the game's
// call sites (asm): RuntimeCreate 0x813bafac, UlthreadCreate 0x813bae10 / 0x813bafe8, TryJoin 0x813bae30.
// No ULT-level yield exists in the imports, so a ulthread holds its worker until it returns.
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "hle/kobj.h"
#include "hle/threads.h"

namespace hle {
namespace {

constexpr uint32_t ULT_ERROR_BUSY = 0x80810006;  // TryJoin: still running (the game compares with this value)

struct Ulthread {
  std::string name;
  uint32_t entry = 0, arg = 0, ctx = 0, ctx_size = 0;
  bool done = false;
  uint32_t status = 0;
};
struct Runtime {
  std::string name;
  uint32_t workers = 0;
  std::deque<std::shared_ptr<Ulthread>> queue;
  bool stopping = false;
  std::condition_variable cv;
};
std::map<uint32_t, std::shared_ptr<Runtime>> runtimes;  // by guest runtime address; kernel_lock()
std::map<uint32_t, std::shared_ptr<Ulthread>> ulthreads;  // by guest ulthread address
std::condition_variable ulthread_done;

void worker(std::shared_ptr<Runtime> rt, std::string name) {
  guest::Cpu& k = adopt_host_thread(name);
  for (;;) {
    std::shared_ptr<Ulthread> u;
    {
      std::unique_lock lk(kernel_lock());
      rt->cv.wait(lk, [&] { return rt->stopping || !rt->queue.empty(); });
      if (rt->queue.empty()) return;
      u = rt->queue.front();
      rt->queue.pop_front();
    }
    if (!k.invoke(u->entry, {u->arg}, u->ctx + u->ctx_size)) die(k, "ulthread \"" + u->name + "\"");
    std::lock_guard lk(kernel_lock());
    u->status = k.reg(0);
    u->done = true;
    ulthread_done.notify_all();
  }
}

// SceSize sceUltUlthreadRuntimeGetWorkAreaSize(maxUlthreads, workers): our runtime keeps its state on the host.
void GetWorkAreaSize(Cpu& c) { ret(c, 0x100); }

// _sceUltUlthreadRuntimeCreate(runtime*, name, maxUlthreads, workers, workArea, opt*, sdkVersion)
void RuntimeCreate(Cpu& c) {
  uint32_t addr = arg(c, 0), workers = arg(c, 3);
  auto rt = std::make_shared<Runtime>();
  rt->name = str(c, arg(c, 1), 32);
  rt->workers = workers ? workers : 1;
  HLE_NOTE("ULT runtime \"%s\": %u workers, max %u ulthreads\n", rt->name.c_str(), rt->workers, arg(c, 2));
  {
    std::lock_guard lk(kernel_lock());
    runtimes[addr] = rt;
  }
  for (uint32_t i = 0; i < rt->workers; i++)
    std::thread(worker, rt, rt->name + " worker " + std::to_string(i)).detach();
  ret(c, 0);
}

// sceUltUlthreadRuntimeDestroy(runtime*): workers exit once the queue is empty.
void RuntimeDestroy(Cpu& c) {
  std::lock_guard lk(kernel_lock());
  auto it = runtimes.find(arg(c, 0));
  if (it != runtimes.end()) {
    it->second->stopping = true;
    it->second->cv.notify_all();
    runtimes.erase(it);
  }
  ret(c, 0);
}

// _sceUltUlthreadCreate(ulthread*, name, entry, arg, context, contextSize, runtime*, opt*, sdkVersion)
void UlthreadCreate(Cpu& c) {
  auto u = std::make_shared<Ulthread>();
  uint32_t addr = arg(c, 0);
  u->name = str(c, arg(c, 1), 32);
  u->entry = arg(c, 2);
  u->arg = arg(c, 3);
  u->ctx = arg(c, 4);
  u->ctx_size = arg(c, 5);
  std::lock_guard lk(kernel_lock());
  auto it = runtimes.find(arg(c, 6));
  if (it == runtimes.end()) return ret(c, SCE_KERNEL_ERROR_INVALID_ARGUMENT);  // unverified code
  ulthreads[addr] = u;
  it->second->queue.push_back(u);
  it->second->cv.notify_one();
  ret(c, 0);
}

// sceUltUlthreadJoin(ulthread*, int32_t* status) / TryJoin (ULT_ERROR_BUSY while running)
void join(Cpu& c, bool block) {
  uint32_t addr = arg(c, 0), stat = arg(c, 1);
  std::unique_lock lk(kernel_lock());
  auto it = ulthreads.find(addr);
  if (it == ulthreads.end()) return ret(c, SCE_KERNEL_ERROR_INVALID_ARGUMENT);  // unverified code
  std::shared_ptr<Ulthread> u = it->second;
  if (!u->done && !block) return ret(c, ULT_ERROR_BUSY);
  ulthread_done.wait(lk, [&] { return u->done; });
  if (stat) c.mem().write<uint32_t>(stat, u->status);
  ulthreads.erase(addr);
  ret(c, 0);
}
void Join(Cpu& c) { join(c, true); }
void TryJoin(Cpu& c) { join(c, false); }

}  // namespace

void register_ult() {
  REG(sceUltUlthreadRuntimeGetWorkAreaSize, GetWorkAreaSize);
  REG(_sceUltUlthreadRuntimeCreate, RuntimeCreate);
  REG(sceUltUlthreadRuntimeDestroy, RuntimeDestroy);
  REG(_sceUltUlthreadCreate, UlthreadCreate);
  REG(sceUltUlthreadJoin, Join);
  REG(sceUltUlthreadTryJoin, TryJoin);
}

}  // namespace hle
