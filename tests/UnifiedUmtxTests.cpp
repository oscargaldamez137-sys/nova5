// Actual POSIX HLE wrapper + actual SyncOnAddress; no guest thread or GPU.
#include "common/abi.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

namespace Libs::Posix {
int KYTY_SYSV_ABI UmtxOp(volatile void*, int, uint64_t, void*, const void*);
}
static void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "UnifiedUmtxTests: %s\n", message); std::exit(1); }
}
int main() {
    using namespace Libs;
    uint64_t word = 0;
    for (const LibKernel::KernelTimespec duration : {
            LibKernel::KernelTimespec{-1, 0}, {0, -1}, {0, 1000000000}}) {
        Check(Posix::UmtxOp(&word, 2, 0, nullptr, &duration) == -1 &&
              *Posix::GetErrorAddr() == Posix::POSIX_EINVAL, "invalid timespec errno");
    }
    for (const int64_t nanos : {int64_t{0}, int64_t{1}, int64_t{1000000}}) {
        const LibKernel::KernelTimespec duration{0, nanos};
        const auto start = std::chrono::steady_clock::now();
        Check(Posix::UmtxOp(&word, 2, 0, nullptr, &duration) == -1 &&
              *Posix::GetErrorAddr() == Posix::POSIX_ETIMEDOUT, "timed wait errno");
        Check(std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(nanos),
              "deadline expired early");
        Check(Posix::UmtxOp(&word, 2, 1, nullptr, &duration) == 0, "mismatch must not wait");
    }
    const LibKernel::KernelTimespec huge{std::numeric_limits<int64_t>::max(), 999999999};
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::atomic_ref<uint64_t>(word).store(1, std::memory_order_release);
        Check(Posix::UmtxOp(&word, 3, 1, nullptr, nullptr) == 0, "wake");
    });
    const int result = Posix::UmtxOp(&word, 2, 0, nullptr, &huge);
    waker.join();
    Check(result == 0, "large tv_sec overflowed before wake");
    std::puts("UnifiedUmtxTests: all passed (POSIX errno, ns, overflow, wake)");
}
