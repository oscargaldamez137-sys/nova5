#pragma once
#include <atomic>
#include <cstdint>

extern "C" {
// 1: a written buffer range that the exact GPU-modified ranges cover completely
// skips the page tracker query.
extern volatile std::atomic<uint32_t> kyty_local_buffer_residency_mode;
}
