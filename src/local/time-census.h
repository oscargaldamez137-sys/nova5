#pragma once
// Local diagnostic: which guest code asks for time, how often and how (live `timecensus`).
// Each time-related HLE entry records (source, return address): the guest reaches an import
// through its PLT, so the return address is the instruction after the guest's call. While
// the census is off an entry costs one relaxed load.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace TimeCensus {

enum Source : uint32_t {
	ProcessTime        = 1,
	ProcessTimeCounter = 2,
	ReadTsc            = 3,
	TscFrequency       = 4,
	CounterFrequency   = 5,
	KernelClockGettime = 6, // arg: clock id
	PosixClockGettime  = 7, // arg: clock id
	Gettimeofday       = 8,
	Usleep             = 9,  // arg: microseconds
	Nanosleep          = 10, // arg: microseconds
	FlipStatus         = 11,
	VblankStatus       = 12,
	WaitVblank         = 13,
	WaitEqueue         = 14, // arg: timeout in microseconds (~0 without one)
	BackingRead        = 15, // not a time source: TryReadBacking of 64 KiB or more, arg: bytes
	DirectDrain        = 16, // not a time source: a direct Vulkan call that drains pending packets
	Sources            = 17
};

struct Entry {
	std::atomic<uint64_t> key {0}; // return address << 8 | source
	std::atomic<uint64_t> calls {0};
	std::atomic<uint64_t> arg_sum {0};
	std::atomic<uint64_t> arg_last {0};
};

constexpr size_t        TableSize = 4096;
inline std::atomic_bool g_on {false};
inline Entry            g_table[TableSize];
inline std::atomic_uint64_t g_overflow {0};

inline void Add(Source source, const void* caller, uint64_t arg = 0) {
	if (!g_on.load(std::memory_order_relaxed)) return;
	const uint64_t key  = (reinterpret_cast<uint64_t>(caller) << 8u) | source;
	const uint64_t hash = (key * 0x9e3779b97f4a7c15ull) >> 52u;
	for (size_t probe = 0; probe < TableSize; ++probe) {
		auto&    entry = g_table[(hash + probe) & (TableSize - 1)];
		uint64_t found = entry.key.load(std::memory_order_acquire);
		if (found == 0 && entry.key.compare_exchange_strong(found, key, std::memory_order_acq_rel)) {
			found = key;
		}
		if (found != key) continue;
		entry.calls.fetch_add(1, std::memory_order_relaxed);
		entry.arg_sum.fetch_add(arg, std::memory_order_relaxed);
		entry.arg_last.store(arg, std::memory_order_relaxed);
		return;
	}
	g_overflow.fetch_add(1, std::memory_order_relaxed);
}

} // namespace TimeCensus

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define KYTY_TIME_CENSUS(source, arg) TimeCensus::Add(TimeCensus::source, __builtin_return_address(0), (arg))
