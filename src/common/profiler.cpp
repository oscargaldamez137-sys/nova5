#include "common/profiler.h"

#include "common/emulatorConfig.h"

#include <algorithm>
#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <tracy/Tracy.hpp>
#include <vector>

namespace {

thread_local std::vector<Profiler::ScopedBlock*> g_block_stack;

void RemoveBlock(Profiler::ScopedBlock* block) {
	auto block_it = std::find(g_block_stack.rbegin(), g_block_stack.rend(), block);
	if (block_it != g_block_stack.rend()) {
		g_block_stack.erase(std::next(block_it).base());
	}
}

} // namespace

namespace Profiler {

void ScopedBlock::Begin(const tracy::SourceLocationData* source_location) {
	if (tracy::ProfilerAvailable()) {
		m_zone.emplace(source_location, TRACY_CALLSTACK, true);
		g_block_stack.push_back(this);
	}
}

void ScopedBlock::End() {
	if (m_zone.has_value()) {
		m_zone.reset();
		RemoveBlock(this);
	}
}

void EndBlock() {
	if (!g_block_stack.empty()) {
		g_block_stack.back()->End();
	}
}

void SetThreadName(const char* name) {
	if (tracy::ProfilerAvailable() && name != nullptr) {
		tracy::SetThreadName(name);
	}
}

void Initialize() {
	switch (Config::GetProfilerDirection()) {
		case Config::ProfilerDirection::Network:
			if (!tracy::ProfilerAvailable()) {
				tracy::StartupProfiler();
				g_active.store(true, std::memory_order_relaxed);
				TracySetProgramName("Nova5");
				::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
				         "broadcast %u, connect to 127.0.0.1:8086\n",
				         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
				         tracy::ProtocolVersion, tracy::BroadcastVersion);
			}
			break;
		case Config::ProfilerDirection::None:
		default: break;
	}
}

void Shutdown() {
	g_active.store(false, std::memory_order_relaxed);
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

} // namespace Profiler
