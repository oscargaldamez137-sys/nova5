#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::NativeIndirect {
inline bool Enabled() {
	static const bool value = [] { const auto* s = std::getenv("KYTY_NATIVE_INDIRECT"); return !s || std::strcmp(s, "0") != 0; }();
	return value;
}
inline bool EmptyMeshEnabled() {
	static const bool value = [] { const auto* s = std::getenv("KYTY_NATIVE_INDIRECT_MESH"); return !s || std::strcmp(s, "0") != 0; }();
	return Enabled() && value;
}
// Jetsku meshIndirect.cpp, AlwaysEmpty (0cd821d0). No argument read needed:
// min(indexCount, INDEX_BUFFER_SIZE) cannot form even one primitive.
constexpr bool AlwaysEmpty(bool indexed, uint32_t index_buffer_size, uint32_t primitive_size) {
	return indexed && index_buffer_size != 0 && index_buffer_size < primitive_size;
}
enum class Outcome { Disabled, Arguments, Topology, Index, Mapping, CpuClean, ImageOwner,
                     TargetOp, Restart, HostLimit, Native, MeshEmpty, MeshFallback, InstanceRead, Count };
inline void Count(Outcome outcome, uint64_t address = 0, uint32_t size = 0) {
	static const bool log = [] { const auto* s = std::getenv("KYTY_INDIRECT_LOG"); return s && std::strcmp(s, "0") != 0; }();
	if (!log) return;
	// These sites execute on the serialized GPU/CP thread, not on guest threads.
	static thread_local uint64_t totals[static_cast<unsigned>(Outcome::Count)] {};
	constexpr const char* names[] = {"disabled", "arguments", "topology", "index", "mapping", "cpu-clean", "image-owner",
	                                "target-op", "restart", "host-limit", "native", "mesh-empty", "mesh-fallback", "instance-read"};
	const auto i = static_cast<unsigned>(outcome);
	const auto n = ++totals[i];
	if ((n & (n - 1)) == 0)
		std::printf("[INDIRECT] %s n=%llu args=0x%llx index_limit=%u\n", names[i],
		            (unsigned long long)n, (unsigned long long)address, size);
}
}
