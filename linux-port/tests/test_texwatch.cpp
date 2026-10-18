// texwatch stress test: many threads write to one freshly protected page at the same moment. Each write faults; the
// first handler lifts the protection, the others must retry their store instead of being reported as a crash.
#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "render/texwatch.h"

int main(int argc, char** argv) {
    const int kThreads = argc > 1 ? std::atoi(argv[1]) : 8;  // 1 = no concurrent faults (control)
    constexpr int kRounds = 20000;
    void* mem = mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return std::perror("mmap"), 1;
    auto* page = static_cast<volatile uint64_t*>(mem) + 0x4000 / 8;  // a page inside the block
    const uint64_t base = reinterpret_cast<uint64_t>(page);

    std::atomic<int> round{-1}, done{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            for (int r = 0; r < kRounds; ++r) {
                while (round.load(std::memory_order_acquire) < r) {}  // all threads start the round together
                page[t] = uint64_t(r);                                 // faults on the protected page
                done.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    int failed = 0;
    for (int r = 0; r < kRounds; ++r) {
        uint64_t seq = 0;
        if (!dcp::texwatch::protect(base, 0x1000, &seq)) return std::fprintf(stderr, "protect failed\n"), 1;
        round.store(r, std::memory_order_release);
        while (done.load(std::memory_order_acquire) < (r + 1) * kThreads) {}
        if (!dcp::texwatch::written_since(base, 0x1000, seq)) ++failed;  // the writes must have been recorded
    }
    for (auto& t : ts) t.join();
    for (int t = 0; t < kThreads; ++t)
        if (page[t] != uint64_t(kRounds - 1)) ++failed;
    std::printf("texwatch: %d rounds x %d threads, %d failures\n", kRounds, kThreads, failed);
    return failed ? 1 : 0;
}
