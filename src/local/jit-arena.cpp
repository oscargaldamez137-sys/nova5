#include "jit-arena.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace JitArena {

#if defined(_WIN32)

namespace {

constexpr size_t ChunkSize = size_t {4} << 20u;

struct Chunk {
	uint8_t* write = nullptr; // read-write view
	uint8_t* run   = nullptr; // read-execute view of the same pages
	size_t   used  = 0;
};

std::mutex g_mutex;
Chunk      g_chunk;
bool       g_failed = false;
size_t     g_used   = 0;

bool MapChunk(Chunk& chunk) {
	HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0,
	                                    static_cast<DWORD>(ChunkSize), nullptr);
	if (section == nullptr) return false;
	void* write = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, ChunkSize);
	void* run   = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_EXECUTE, 0, 0, ChunkSize);
	CloseHandle(section); // (the views keep it)
	if (write == nullptr || run == nullptr) {
		if (write != nullptr) UnmapViewOfFile(write);
		if (run != nullptr) UnmapViewOfFile(run);
		return false;
	}
	chunk = {static_cast<uint8_t*>(write), static_cast<uint8_t*>(run), 0};
	return true;
}

} // namespace

const void* Copy(const void* code, size_t size) {
	// KYTY_JIT_ARENA=0: every function keeps its own buffer, as before.
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_JIT_ARENA");
		return value == nullptr || std::string_view(value) != "0";
	}();
	if (!enabled || code == nullptr || size == 0 || size > ChunkSize) return nullptr;
	const size_t aligned = (size + 15u) & ~size_t {15};
	uint8_t*     write   = nullptr;
	uint8_t*     run     = nullptr;
	{
		std::lock_guard lock(g_mutex);
		if (g_failed) return nullptr;
		if (g_chunk.write == nullptr || ChunkSize - g_chunk.used < aligned) {
			// (A full chunk stays mapped: the code in it runs for the life of the process.)
			if (!MapChunk(g_chunk)) {
				g_failed = true;
				return nullptr;
			}
		}
		write = g_chunk.write + g_chunk.used;
		run   = g_chunk.run + g_chunk.used;
		g_chunk.used += aligned;
		g_used += aligned;
	}
	std::memcpy(write, code, size);
	FlushInstructionCache(GetCurrentProcess(), run, size);
	return run;
}

size_t Used() {
	std::lock_guard lock(g_mutex);
	return g_used;
}

#else

const void* Copy(const void*, size_t) {
	return nullptr;
}

size_t Used() {
	return 0;
}

#endif

} // namespace JitArena
