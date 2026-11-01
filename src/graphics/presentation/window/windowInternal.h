#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_

#include "SDL_events.h"
#include "SDL_video.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace Libs::Graphics {

class Presenter;
class RenderContext;

struct SurfaceCapabilities {
	vk::SurfaceCapabilitiesKHR        capabilities {};
	std::vector<vk::SurfaceFormatKHR> formats;
	std::vector<vk::PresentModeKHR>   present_modes;
};

struct WindowLoopState {
	SDL_Event        event {};
	bool             need_exit = false;
	std::atomic_bool paused    = false;
};

struct WindowContext {
	WindowContext();
	~WindowContext();
	KYTY_CLASS_NO_COPY(WindowContext);

	[[nodiscard]] static vk::PhysicalDeviceVulkan12Features RequiredVulkan12Features() noexcept;
	[[nodiscard]] static vk::PhysicalDeviceVulkan13Features RequiredVulkan13Features() noexcept;
	[[nodiscard]] static uint32_t InitialWindowFlags(bool fullscreen) noexcept;
	void                                                    CreateVulkan();
	void                                                    RecreateSurface();
	void                                                    RefreshSurfaceCapabilities();
	void                                                    UpdateIcon();
	// Present thread, after a game frame: `presented` images shown for it (with frame generation more than one, the title
	// then also shows their rate, named `generator`).
	void                                                    UpdateTitle(uint32_t presented = 1, const char* generator = nullptr);
	void                                                    Resize(uint32_t width, uint32_t height);
	void ProcessWindowEvent(const SDL_WindowEvent& event);
	void ProcessDisplayEvent(const SDL_DisplayEvent& event);
	void ProcessEvent(double time_seconds);
	void Run();
	// SDL window operations must complete on the main thread.
	void RunOnMainThread(std::function<void()> task);
	// The same without waiting for the main thread to run it (the present thread's title updates: the main loop can sit
	// in SDL_WaitEvent for long, and a present that waited for it dropped the frames made meanwhile).
	void PostToMainThread(std::function<void()> task);
	void DrainMainThreadTasks();

	GraphicContext                 graphic_ctx;
	SDL_Window*                    window        = nullptr;
	vk::SurfaceKHR                 surface       = nullptr;
	SurfaceCapabilities            surface_capabilities;
	std::unique_ptr<RenderContext> render_context;
	std::unique_ptr<Presenter>     presenter;
	WindowLoopState                loop;

	Common::Mutex mutex;

	Common::Mutex                      main_task_mutex;
	Common::CondVar                    main_task_done;
	std::vector<std::function<void()>> main_tasks;            // guarded by main_task_mutex
	uint64_t                           main_tasks_queued = 0; // guarded by main_task_mutex
	uint64_t                           main_tasks_run    = 0; // guarded by main_task_mutex
	std::atomic_bool                   title_posted      = false; // a title update waits for the main thread
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_WINDOW_WINDOWINTERNAL_H_
