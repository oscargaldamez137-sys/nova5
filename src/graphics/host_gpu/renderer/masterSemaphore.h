#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <thread>

namespace Libs::Graphics {

struct GraphicContext;

class MasterSemaphore {
public:
	explicit MasterSemaphore(GraphicContext& graphics);
	~MasterSemaphore();
	KYTY_CLASS_NO_COPY(MasterSemaphore);

	[[nodiscard]] uint64_t CurrentTick() const noexcept {
		return m_current_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] uint64_t KnownGpuTick() const noexcept {
		return m_gpu_tick.load(std::memory_order_acquire);
	}
	[[nodiscard]] bool     IsFree(uint64_t tick) const noexcept { return KnownGpuTick() >= tick; }
	[[nodiscard]] uint64_t NextTick() noexcept {
		return m_current_tick.fetch_add(1, std::memory_order_release);
	}
	[[nodiscard]] vk::Semaphore Handle() const noexcept { return m_semaphore; }

	void Refresh();
	void Wait(uint64_t tick);

private:
	void Publish(uint64_t counter) noexcept;
	void Watch(const std::stop_token& stop);

	GraphicContext&       m_graphics;
	vk::Semaphore         m_semaphore = nullptr;
	std::atomic<uint64_t> m_gpu_tick {0};
	std::atomic<uint64_t> m_current_tick {1};
	// Where a timeline read is a system call (Linux' RADV, ~9 us; issue #80: ~1,500 reads a frame at 1-1, most of them
	// the render thread's polls of whether the GPU is still busy), a thread waits for each tick and publishes it, and
	// Refresh reads nothing. KYTY_TIMELINE_WATCHER=0|1 (default: when a read takes 1 us or more).
	bool         m_watched = false;
	std::jthread m_watcher;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MASTERSEMAPHORE_H_
