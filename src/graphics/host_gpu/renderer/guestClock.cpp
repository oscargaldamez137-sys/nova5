#include "graphics/host_gpu/renderer/guestClock.h"

#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "kernel/memory.h"

#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

namespace Libs::Graphics {

bool GuestClock::Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_TIMESTAMPS");
		if (value && std::strcmp(value, "0") != 0 && std::strcmp(value, "1") != 0)
			EXIT("KYTY_GPU_TIMESTAMPS must be 0 or 1\n");
		const bool result = !value || std::strcmp(value, "0") != 0;
		std::fprintf(stderr, "[GPU_TIMESTAMPS] mode=%s\n", result ? "native" : "cpu");
		return result;
	}();
	return enabled;
}

bool GuestClock::ParseScale(const char* text, uint32_t& percent) {
	if (text == nullptr) {
		percent = 100;
		return true;
	}
	if (*text == '\0') return false;
	uint32_t value = 0;
	for (; *text != '\0'; ++text) {
		if (*text < '0' || *text > '9' || value > 200) return false;
		value = value * 10 + static_cast<uint32_t>(*text - '0');
	}
	if (value < 10 || value > 200) return false;
	percent = value;
	return true;
}

const GuestClock::ScaleOptions& GuestClock::GetScaleOptions() {
	static const ScaleOptions options = [] {
		uint32_t percent = 100;
		if (!ParseScale(std::getenv("KYTY_GPU_TIMESTAMP_SCALE"), percent))
			std::fprintf(stderr, "[GPU_TIMESTAMPS] invalid KYTY_GPU_TIMESTAMP_SCALE: expected integer 10..200; using 100\n");
		const auto base = Sync::ReadReferenceClock();
		std::fprintf(stderr, "[GPU_TIMESTAMPS] scale_percent=%u scale_base=0x%llx\n",
		             percent, (unsigned long long)base);
		return ScaleOptions {base, percent};
	}();
	return options;
}

bool GuestClock::Scale(uint64_t value, uint64_t base, uint32_t percent, uint64_t& result) {
	if (percent < 10 || percent > 200) return false;
	const bool before = value < base;
	const auto delta = before ? base - value : value - base;
	// Integer arithmetic preserves large epochs and avoids overflow in delta * percent.
	// Round the signed offset down, including samples just before the session base.
	const auto whole = delta / 100;
	const auto fraction = ((delta % 100) * percent + (before ? 99u : 0u)) / 100;
	if (whole > (UINT64_MAX - fraction) / percent) return false;
	const auto ticks = whole * percent + fraction;
	if (before ? ticks > base : ticks > UINT64_MAX - base) return false;
	result = before ? base - ticks : base + ticks;
	return true;
}

uint64_t GuestClock::AdjustTimestamp(uint64_t value) {
	if (!Enabled()) return value;
	const auto& options = GetScaleOptions();
	if (options.percent == 100) return value;
	uint64_t result = 0;
	if (!Scale(value, options.base, options.percent, result))
		EXIT("scaled GPU reference clock out of range\n");
	return result;
}


GuestClock::~GuestClock() {
	if (m_recorded || m_fallback) std::fprintf(stderr,
	    "[GPU_TIMESTAMPS] recorded=%llu published=%llu late=%llu conflicts=%llu failed=%llu fallback=%llu\n",
	    (unsigned long long)m_recorded, (unsigned long long)m_published, (unsigned long long)m_late,
	    (unsigned long long)m_conflicts, (unsigned long long)m_failed, (unsigned long long)m_fallback);
	if (m_pool) m_context.GetGraphics().device.destroyQueryPool(m_pool);
}

void GuestClock::TraceFrame() {
	InitializeTrace();
	if (!m_trace || m_trace_flip++ % 120 != 0) return;
	std::fprintf(stderr, "[GPU_TIMESTAMPS] flip=%llu enabled=%u ready=%u recorded=%llu published=%llu "
	             "late=%llu conflicts=%llu failed=%llu fallback=%llu pending=%u\n",
	             static_cast<unsigned long long>(m_trace_flip), Enabled() ? 1u : 0u, m_pool ? 1u : 0u,
	             static_cast<unsigned long long>(m_recorded), static_cast<unsigned long long>(m_published),
	             static_cast<unsigned long long>(m_late), static_cast<unsigned long long>(m_conflicts),
	             static_cast<unsigned long long>(m_failed), static_cast<unsigned long long>(m_fallback), m_pending);
	std::fprintf(stderr, "[GPU_TIMESTAMP_BLOCKS] acquired=%llu continued=%llu pressure_waits=%llu\n",
	             (unsigned long long)m_block_acquires, (unsigned long long)m_block_reuses, (unsigned long long)m_block_waits);
	std::fprintf(stderr, "[GPU_TIMESTAMPS_CAUSES] no_block=%llu block_full=%llu unaligned=%llu unmapped=%llu "
	             "no_backing=%llu conflict_buffer=%llu conflict_downloads=%llu conflict_texture=%llu "
	             "downloads_unrelated_bytes=%llu downloads_unrelated_published=%llu\n",
	             (unsigned long long)m_fallback_reasons[0], (unsigned long long)m_fallback_reasons[1],
	             (unsigned long long)m_fallback_reasons[2], (unsigned long long)m_fallback_reasons[3],
	             (unsigned long long)m_fallback_reasons[4], (unsigned long long)m_conflict_reasons[0],
	             (unsigned long long)m_conflict_reasons[1], (unsigned long long)m_conflict_reasons[2],
	             (unsigned long long)m_downloads_unrelated, (unsigned long long)m_downloads_unrelated_published);
	std::fprintf(stderr, "[GPU_TIMESTAMP_SOURCES] slots=%zu mixed_slots=%llu address_overflow=%llu\n",
	             m_trace_sources.size(), (unsigned long long)m_mixed_sources, (unsigned long long)m_trace_address_overflow);
	if (m_pair_trace.Size() == 0) {
		std::fprintf(stderr, "[GPU_TIMESTAMP_PAIRS] configured=0 mixed=unknown\n");
	} else {
		const auto pairs = m_pair_trace.Snapshot();
		std::fprintf(stderr, "[GPU_TIMESTAMP_PAIRS] configured=%u gpu=%llu cpu=%llu mixed=%llu unresolved=%llu\n",
		             m_pair_trace.Size(), (unsigned long long)pairs.gpu, (unsigned long long)pairs.cpu,
		             (unsigned long long)pairs.mixed, (unsigned long long)pairs.unresolved);
	}
}

void GuestClock::InitializeTrace() {
	if (m_trace_initialized) return;
	m_trace_initialized = true;
	const char* trace = std::getenv("KYTY_GPU_TIMESTAMPS_TRACE");
	m_trace = trace != nullptr && std::strcmp(trace, "1") == 0;
	if (!m_trace) return;
	// Explicit ring bases for the measured game version; no addresses from another eboot.
	const char* rings = std::getenv("KYTY_GPU_TIMESTAMPS_RINGS");
	while (rings != nullptr && *rings != '\0' && m_pair_trace.Size() < 128) {
		char* end = nullptr;
		errno = 0;
		const auto base = std::strtoull(rings, &end, 0);
		if (end == rings || errno == ERANGE || base == 0 || (base & 7u) != 0 ||
		    base > UINT64_MAX - 0x200u || (*end != ',' && *end != '\0')) {
			std::fprintf(stderr, "[GPU_TIMESTAMP_PAIRS] invalid ring base: %s\n", rings);
			break;
		}
		for (uint32_t i = 0; i < 8; ++i) m_pair_trace.Add(base + 0x10 + 32u * i, base + 0x110 + 32u * i);
		std::fprintf(stderr, "[GPU_TIMESTAMP_PAIRS] ring=0x%llx slots=8 begin=+0x10 end=+0x110 stride=32\n", base);
		rings = *end == ',' ? end + 1 : end;
	}
}

bool GuestClock::Fallback(uint64_t address, uint32_t reason) {
	++m_fallback;
	++m_fallback_reasons[reason];
	if (m_trace && m_fallback_logs[reason]++ < 8) {
		static constexpr const char* names[] = {"no_block", "block_full", "unaligned", "unmapped", "no_backing"};
		std::fprintf(stderr, "[GPU_TIMESTAMP_FALLBACK] reason=%s address=0x%llx active=%d used=%u pending=%u\n",
		             names[reason], (unsigned long long)address, m_active,
		             m_active < 0 ? 0u : m_blocks[m_active].used, m_pending);
	}
	return false;
}

void GuestClock::TraceCpuWrite(uint64_t address) {
	TraceWriteSource(address, GuestClockPairTrace::Source::Cpu);
}

void GuestClock::TraceWriteSource(uint64_t address, GuestClockPairTrace::Source source) {
	if (!m_trace) return;
	m_pair_trace.Written(address, source);
	if (m_trace_sources.size() >= 16384 && !m_trace_sources.contains(address)) {
		++m_trace_address_overflow;
		return;
	}
	auto& sources = m_trace_sources[address];
	const auto previous = sources;
	sources |= source == GuestClockPairTrace::Source::Gpu ? 2u : 1u;
	if (sources == 3 && previous != 3) {
		++m_mixed_sources;
		if (m_mixed_sources <= 16) std::fprintf(stderr,
		    "[GPU_TIMESTAMP_SOURCE_SWITCH] address=0x%llx now=%s\n", (unsigned long long)address,
		    source == GuestClockPairTrace::Source::Gpu ? "gpu" : "cpu");
	}
}

bool GuestClock::Convert(uint64_t native, uint64_t native_anchor, uint64_t guest_anchor,
                         uint64_t mask, double period_ns, uint64_t& result) {
	if (!mask || !std::isfinite(period_ns) || period_ns <= 0) return false;
	const auto delta = (native - native_anchor) & mask;
	// A backwards sample (or an ambiguous half-wrap) must not become a huge time.
	if (delta > (mask >> 1)) return false;
	const auto scaled = std::floor(static_cast<long double>(delta) * period_ns / 10.0L);
	if (scaled < 0 || scaled >= 18446744073709551616.0L) return false;
	const auto ticks = static_cast<uint64_t>(scaled);
	if (ticks > UINT64_MAX - guest_anchor) return false;
	result = guest_anchor + ticks;
	return true;
}

bool GuestClock::Initialize() {
	if (m_pool) return true;
	if (m_unavailable) return false;
	m_unavailable = true;
	auto& graphics = m_context.GetGraphics();
	if (!graphics.calibrated_timestamps_enabled) {
		std::fprintf(stderr, "[GPU_TIMESTAMPS] unavailable=calibrated_timestamps\n");
		return false;
	}
	uint32_t count = 0;
	graphics.physical_device.getQueueFamilyProperties(&count, nullptr);
	std::vector<vk::QueueFamilyProperties> families(count);
	graphics.physical_device.getQueueFamilyProperties(&count, families.data());
	const auto bits = graphics.queue_family < count ? families[graphics.queue_family].timestampValidBits : 0u;
	m_mask = bits == 0 ? 0 : bits >= 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
	m_period_ns = graphics.physical_device_properties.limits.timestampPeriod;
	if (!m_mask || !std::isfinite(m_period_ns) || m_period_ns <= 0) return false;

	// A calibrated DEVICE sample lies inside this CPU-clock bracket. Choose the
	// narrowest bracket, once per device lifetime; never anchor to callback time.
	// Warm ReadReferenceClock first: initial TSC-frequency discovery may sleep.
	(void)Sync::ReadReferenceClock();
	uint64_t best_error_ns = UINT64_MAX;
	vk::CalibratedTimestampInfoEXT info {};
	info.timeDomain = vk::TimeDomainEXT::eDevice;
	for (unsigned i = 0; i < 8; ++i) {
		uint64_t native = 0, deviation = 0;
		const auto before = Sync::ReadReferenceClock();
		const auto status = graphics.device.getCalibratedTimestampsEXT(1, &info, &native, &deviation);
		const auto after = Sync::ReadReferenceClock();
		if (status != vk::Result::eSuccess || after < before || after - before > 1000000) continue;
		const auto bracket_ns = (after - before) * 10;
		if (deviation > UINT64_MAX - bracket_ns) continue;
		const auto error = bracket_ns + deviation;
		if (error < best_error_ns) {
			best_error_ns = error;
			m_native_anchor = native & m_mask;
			m_guest_anchor = before + (after - before) / 2;
		}
	}
	if (best_error_ns == UINT64_MAX) {
		std::fprintf(stderr, "[GPU_TIMESTAMPS] unavailable=calibration\n");
		return false;
	}
	vk::QueryPoolCreateInfo create {};
	create.queryType = vk::QueryType::eTimestamp;
	create.queryCount = BlockCount * QueriesPerBlock;
	if (graphics.device.createQueryPool(&create, nullptr, &m_pool) != vk::Result::eSuccess) return false;
	m_unavailable = false;
	std::fprintf(stderr, "[GPU_TIMESTAMPS] ready=1 ref_hz=100000000 period_ns=%.9g calibration_bound_ns=%llu\n",
	             m_period_ns, static_cast<unsigned long long>(best_error_ns));
	return true;
}

void GuestClock::OnBegin(CommandScheduler& scheduler, CommandBuffer& command) {
	if (&scheduler != &m_context.GetCommandScheduler()) return;
	// The A/B switch is fixed for the process: no pool/reset overhead in CPU mode.
	if (!Enabled()) return;
	(void)GetScaleOptions();
	if (!Initialize()) return;
	// Unused queries in the active block remain reset across submissions. Reserving a whole
	// block per command buffer exhausted 64 blocks with only about 65 queries in flight.
	if (m_active >= 0 && m_blocks[m_active].used < QueriesPerBlock) {
		++m_block_reuses;
		return;
	}
	(void)AcquireBlock(command);
}

bool GuestClock::AcquireBlock(CommandBuffer& command) {
	for (uint32_t i = 0; i < BlockCount; ++i) {
		const auto index = (m_next + i) % BlockCount;
		if (m_blocks[index].pending) continue;
		m_blocks[index].used = 0;
		m_blocks[index].samples.clear(); // Keep storage for the next use of this block.
		m_active = static_cast<int32_t>(index);
		m_next = (index + 1) % BlockCount;
		command.EndRendering();
		command.Handle().resetQueryPool(m_pool, index * QueriesPerBlock, QueriesPerBlock);
		++m_block_acquires;
		return true;
	}
	m_active = -1;
	return false;
}

bool GuestClock::EnsureBlock() {
	if (!m_pool) return false;
	if (m_active >= 0 && m_blocks[m_active].used < QueriesPerBlock) return true;
	auto& scheduler = m_context.GetCommandScheduler();
	// EOP is on the command lane. Retire completed samples before reusing their queries.
	scheduler.PopPendingOperations();
	if (m_active >= 0 && m_blocks[m_active].used < QueriesPerBlock) return true;
	if (AcquireBlock(scheduler.Current())) return true;
	// Every block still has pending queries: bound storage and retire them, never substitute CPU time.
	++m_block_waits;
	scheduler.Finish();
	if (m_active >= 0 && m_blocks[m_active].used < QueriesPerBlock) return true;
	if (!AcquireBlock(scheduler.Current())) EXIT("GPU timestamp blocks still pending after completion\n");
	return true;
}

void GuestClock::Cancel(uint64_t address, uint64_t size) {
	if (m_trace) m_pair_trace.Cancel(address, size);
	if (!size || m_latest.empty()) return;
	// Most immediate labels write one aligned dword/qword. Pending clocks are
	// always 8-byte aligned, so only one slot can overlap this common case.
	if (size <= 8 - (address & 7u)) {
		m_latest.erase(address & ~uint64_t{7});
		return;
	}
	for (auto it = m_latest.begin(); it != m_latest.end();) {
		const auto other = it->first;
		const bool overlap = address <= other ? other - address < size : address - other < 8;
		if (overlap) it = m_latest.erase(it); else ++it;
	}
}

bool GuestClock::Record(uint64_t address) {
	InitializeTrace();
	const bool enabled = Enabled();
	const bool available = enabled && EnsureBlock();
	if (m_trace) m_pair_trace.Begin(address);
	if (!enabled) return false;
	if (!available) return Fallback(address, 0);
	if ((address & 7u) != 0) return Fallback(address, 2);
	if (!m_context.GetGpuResources().IsMapped(address, 8)) return Fallback(address, 3);
	uint64_t previous = 0;
	// This address will be written through the backing alias by a completion,
	// which cannot fault or synchronously ask the GPU thread to retire readers.
	if (!LibKernel::Memory::TryReadBacking(address, &previous, sizeof(previous))) {
		return Fallback(address, 4);
	}
	const auto block = static_cast<uint32_t>(m_active);
	auto& state = m_blocks[block];
	const auto index = state.used++;
	const auto query = block * QueriesPerBlock + index;
	const auto serial = ++m_serial;
	m_latest[address] = serial;
	state.samples.push_back({address, serial, previous});
	++state.pending;
	++m_pending;
	auto& scheduler = m_context.GetCommandScheduler();
	scheduler.Current().Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_pool, query);
	// Extend only consecutive clock completions in this tick. An intervening
	// normal callback (e.g. fault processing) must retain its publication order.
	// Metadata is filled on the command lane before this tick can be submitted.
	const auto tick = scheduler.CurrentTick();
	if (index != 0 && state.batch_tick == tick && state.operation_sequence == scheduler.DeferredOperationSequence()) {
		++state.samples[state.last_batch].batch_size;
	} else {
		state.last_batch = index;
		state.batch_tick = tick;
		state.operation_sequence = scheduler.DeferOperation([this, block, index] { Complete(block, index); });
	}
	if (m_wake_tick != tick && m_context.HasGpu()) {
		m_wake_tick = tick;
		// One wake per buffer, queued while recording (not from Submit, which
		// also runs during scheduler shutdown). It waits for this native tick.
		// Otherwise normal completions could starve a guest polling an isolated
		// timestamp without sending another draw. Shutdown's Finish drains it
		// before GuestGpu is destroyed.
		scheduler.DeferPriorityOperation([gpu = &m_context.GetGpu()] { gpu->WakeClockCompletions(); });
	}
	++m_recorded;
	return true;
}

void GuestClock::Complete(uint32_t block, uint32_t first) {
	auto& state = m_blocks[block];
	const auto count = state.samples[first].batch_size;
	struct Result { uint64_t value, available; };
	std::array<Result, QueriesPerBlock> native;
	const auto status = m_context.GetGraphics().device.getQueryPoolResults(m_pool,
	    block * QueriesPerBlock + first, count, count * sizeof(Result), native.data(), sizeof(Result),
	    vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
	for (uint32_t i = 0; i < count; ++i) {
		// Do not inspect undefined output if the non-waiting query failed.
		const bool ready = status == vk::Result::eSuccess && native[i].available != 0;
		Publish(state.samples[first + i], ready ? native[i].value : 0, ready);
	}
	state.pending -= count;
	m_pending -= count;
}

void GuestClock::Publish(const Sample& sample, uint64_t native, bool ready) {
	const auto address = sample.address, serial = sample.serial, previous = sample.previous;
	const auto found = m_latest.find(address);
	if (found == m_latest.end() || found->second != serial) {
		// A later timestamp or immediate PM4 write superseded this slot. In
		// particular, do not overwrite the CPU path after disabling native clocks.
		++m_late;
		return;
	}
	m_latest.erase(found);
	uint64_t value = 0;
	if (!ready || !Convert(native, m_native_anchor, m_guest_anchor, m_mask, m_period_ns, value)) {
		++m_failed;
		return;
	}
	value = AdjustTimestamp(value);
	if (!m_context.GetGpuResources().IsMapped(address, 8)) {
		++m_late;
		return;
	}
	// Stage one does not arbitrate with a GPU writer to the same tracked page.
	// Preserve its data rather than starting a readback from a completion or
	// falsely clearing GPU ownership. Clean resident buffers ARE invalidated.
	auto& cache = m_context.GetBufferCache();
	const bool buffer_conflict = cache.IsRegionGpuModified(address, 8);
	const bool download_conflict = (!buffer_conflict || m_trace) && cache.HasPendingBackingPublication(address, 8);
	const bool texture_conflict = (m_trace || (!buffer_conflict && !download_conflict)) &&
	                             m_context.GetTextureCache().HasTrackedDataOverlap(address, 8);
	// Buffer ownership above is page-granular. A readback of neighbouring bytes on the same
	// GPU-dirty page still blocks publication; downloads of other pages do not.
	const bool unrelated_downloads = m_trace && cache.HasUnpublishedDownloads() && !download_conflict;
	if (unrelated_downloads) ++m_downloads_unrelated;
	if (buffer_conflict || download_conflict || texture_conflict) {
		++m_conflicts;
		if (m_trace) {
			const bool causes[] = {buffer_conflict, download_conflict, texture_conflict};
			for (uint32_t i = 0; i < 3; ++i) {
				if (!causes[i]) continue;
				++m_conflict_reasons[i];
				if (m_conflict_logs[i]++ < 8) std::fprintf(stderr,
				    "[GPU_TIMESTAMP_CONFLICT] address=0x%llx buffer=%u downloads=%u texture=%u\n",
				    (unsigned long long)address, buffer_conflict ? 1u : 0u, download_conflict ? 1u : 0u,
				    texture_conflict ? 1u : 0u);
			}
		}
		return;
	}
	// No resident buffer means no stale GPU copy/BDA upload to invalidate.
	// Registration queues its own sync if the address becomes resident later.
	// Keep texture page watchers and descriptor-heap invalidation independent:
	// neither is implied by buffer residency. Unmapping is serialized on this lane.
	if (cache.IsRegionRegistered(address, 8)) {
		cache.InvalidateMemory(address, 8);
	}
	m_context.GetTextureCache().InvalidateMemory(address, 8);
	// The CAS checks the old slot value under the backing lock; a separate read
	// would duplicate that lock and still need the CAS to prevent a racing write.
	const bool published = LibKernel::Memory::TryCompareExchangeBacking64(address, previous, value);
	if (published) {
		++m_published;
		if (unrelated_downloads) ++m_downloads_unrelated_published;
		TraceWriteSource(address, GuestClockPairTrace::Source::Gpu);
	} else ++m_late;
}
} // namespace Libs::Graphics
