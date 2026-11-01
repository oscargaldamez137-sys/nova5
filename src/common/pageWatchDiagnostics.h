#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

// Process-wide high water mark, available even when termination bypasses the
// PageManager destructor. No page/renderer locks, allocations or live pointers.
namespace Common::PageWatchDiagnostics {
inline constexpr uint64_t AddressMask = (uint64_t {1} << 40) - 1;
inline std::atomic<uint64_t> peak {0}; // count: upper bits; guest page: lower 40
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct Snapshot { uint32_t count; uint64_t address; };
inline Snapshot Get() noexcept {
    const auto value = peak.load(std::memory_order_relaxed);
    return {static_cast<uint32_t>(value >> 40), value & AddressMask};
}

inline void Observe(uint32_t count, uint64_t address) noexcept {
    const auto value = (uint64_t {count} << 40) | (address & AddressMask);
    auto previous = peak.load(std::memory_order_relaxed);
    while (count > (previous >> 40)) {
        if (peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
            if (count >= 128 && (count & (count - 1)) == 0) {
                std::printf("[PAGE_WATCH] write-count=%u page=0x%016llx\n", count,
                            static_cast<unsigned long long>(address));
                std::fflush(stdout);
            }
            break;
        }
    }
}

inline void Report(const char* reason) noexcept {
    const auto value = Get();
    std::printf("[PAGE_WATCH] summary reason=%s max-write-count=%u page=0x%016llx\n",
                reason, value.count, static_cast<unsigned long long>(value.address));
    std::fflush(stdout);
}
} // namespace Common::PageWatchDiagnostics
