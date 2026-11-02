#pragma once

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/host_gpu/renderer/guestClockTrace.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {
class CommandScheduler;
class CommandBuffer;
class RenderContext;

// Port of Nova5 native EOP reference clocks. KYTY_GPU_TIMESTAMPS=0 opts out.
// Stage one covers reference clocks only. Completion labels retain
// their existing semantics. All state, including publication, belongs to the
// GPU command lane (normal scheduler completions, not the priority thread).
class GuestClock {
public:
	explicit GuestClock(RenderContext& context): m_context(context) {}
	~GuestClock();
	GuestClock(const GuestClock&) = delete;
	GuestClock& operator=(const GuestClock&) = delete;
	void OnBegin(CommandScheduler& scheduler, CommandBuffer& command);
	bool Record(uint64_t address);
	static bool Enabled();
	// KYTY_GPU_TIMESTAMP_SCALE=10..200 (100 by default), fixed for the process.
	// All PM4 reference-clock writes share this transform; kernel clocks do not.
	// KYTY_GPU_TIMESTAMPS=0 bypasses the adjustment as well.
	static uint64_t AdjustTimestamp(uint64_t value);
	// Called before immediate PM4 writes even after native clocks are disabled.
	void Cancel(uint64_t address, uint64_t size);
	bool HasPending() const { return m_pending != 0; }
	// KYTY_GPU_TIMESTAMPS_TRACE=1: counters every 120 flips, without waiting for queries.
	void TraceFrame();
	void TraceCpuWrite(uint64_t address);

	// Convert a forward device-clock interval; subtract before scaling so a
	// large native epoch never loses precision in floating-point conversion.
	static bool Convert(uint64_t native, uint64_t native_anchor, uint64_t guest_anchor,
	                    uint64_t mask, double period_ns, uint64_t& result);

private:
	friend struct GuestClockTestAccess;
	struct ScaleOptions { uint64_t base; uint32_t percent; };
	static const ScaleOptions& GetScaleOptions();
	static bool ParseScale(const char* text, uint32_t& percent);
	static bool Scale(uint64_t value, uint64_t base, uint32_t percent, uint64_t& result);
	static constexpr uint32_t BlockCount = 64, QueriesPerBlock = 1024;
	struct Sample {
		uint64_t address, serial, previous;
		uint32_t batch_size = 1;
	};
	struct Block {
		uint32_t used = 0, pending = 0, last_batch = 0;
		uint64_t operation_sequence = 0, batch_tick = 0;
		std::vector<Sample> samples;
	};
	bool Initialize();
	bool AcquireBlock(CommandBuffer& command);
	bool EnsureBlock();
	void InitializeTrace();
	void TraceWriteSource(uint64_t address, GuestClockPairTrace::Source source);
	bool Fallback(uint64_t address, uint32_t reason);
	void Complete(uint32_t block, uint32_t first);
	void Publish(const Sample& sample, uint64_t native, bool ready);
	RenderContext& m_context;
	vk::QueryPool m_pool;
	std::array<Block, BlockCount> m_blocks {};
	std::unordered_map<uint64_t, uint64_t> m_latest;
	uint64_t m_native_anchor = 0, m_guest_anchor = 0, m_mask = 0, m_serial = 0;
	double m_period_ns = 0;
	uint32_t m_next = 0, m_pending = 0;
	int32_t m_active = -1;
	bool m_unavailable = false;
	uint64_t m_recorded = 0, m_published = 0, m_late = 0, m_conflicts = 0, m_failed = 0, m_fallback = 0;
	uint64_t m_wake_tick = 0, m_block_acquires = 0, m_block_reuses = 0, m_block_waits = 0;
	uint64_t m_trace_flip = 0;
	bool m_trace_initialized = false, m_trace = false;
	std::array<uint64_t, 5> m_fallback_reasons {};
	std::array<uint64_t, 3> m_conflict_reasons {};
	std::array<uint32_t, 5> m_fallback_logs {};
	std::array<uint32_t, 3> m_conflict_logs {};
	uint64_t m_downloads_unrelated = 0, m_downloads_unrelated_published = 0;
	std::unordered_map<uint64_t, uint8_t> m_trace_sources;
	uint64_t m_mixed_sources = 0, m_trace_address_overflow = 0;
	GuestClockPairTrace m_pair_trace;
};
} // namespace Libs::Graphics
