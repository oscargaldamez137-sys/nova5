#pragma once
// Progress of the start-up work that holds up the game's first frame (pipeline cache loading,
// shader and pipeline warmup): the window shows it until the first present (startupProgress.cpp
// installs the painter). Reports from threads other than the window's are not shown.

#include <atomic>
#include <cstdint>

namespace StartupProgress {

// `text` (UTF-8) and `done` of `total` (total 0: no count).
using Painter = void (*)(const char* text, uint64_t done, uint64_t total);

inline std::atomic<Painter> g_painter {nullptr};

inline void Report(const char* text, uint64_t done, uint64_t total) {
	if (const auto painter = g_painter.load(std::memory_order_acquire)) painter(text, done, total);
}

} // namespace StartupProgress
