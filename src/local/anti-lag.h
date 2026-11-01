#ifndef EMULATOR_SRC_LOCAL_ANTI_LAG_H_
#define EMULATOR_SRC_LOCAL_ANTI_LAG_H_
// AMD Anti-Lag (VK_AMD_anti_lag: AMD's driver, or Mesa's VK_LAYER_MESA_anti_lag on RADV), with KYTY_LOW_LATENCY=1 and
// KYTY_ANTI_LAG=1. The driver delays the start of a frame, its input, so that the frame reaches the
// display with less waiting in between. The input mark is the guest flip on the GPU thread (KYTY_ANTI_LAG_INPUT=pad: the
// guest's first pad read after a flip), the present mark the present of the frame that flip made.
// Mesa's layer (26.2) breaks a vkQueueSubmit with timeline values once it adds its timestamps (after the first input
// mark), so the emulator's submissions go through QueueSubmit below. KYTY_LOW_LATENCY=1 alone keeps the one-frame flip
// queue (videoOut.cpp), on any GPU.
#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics::AntiLag {

// KYTY_LOW_LATENCY=1 and KYTY_ANTI_LAG=1.
[[nodiscard]] bool Requested();
// Before the Vulkan instance: Mesa's layer is an implicit one that an environment variable turns on.
void BeforeInstance();
// The device extension, when the GPU (or the layer) offers it.
void AddDeviceExtension(const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions);
// Device creation: its feature chained into `create_info` (`storage` keeps it alive until the device is made).
void ChainFeature(vk::PhysicalDevice physical_device, const std::vector<const char*>& extensions,
                  vk::DeviceCreateInfo& create_info, VkPhysicalDeviceAntiLagFeaturesAMD& storage);
void OnDevice(vk::Device device);
[[nodiscard]] bool Enabled();

// A guest thread reads the pad (the input mark of a new frame after a flip; the driver may sleep here).
void Input();
// GPU thread, at a guest flip: the frame that flip shows (0: none marked).
[[nodiscard]] uint64_t Flip();
// Present thread, before the present of the frame Flip numbered.
void Present(uint64_t frame);

// The emulator's queue submissions (one VkSubmitInfo, its timeline values chained): `submit`, or `submit2` while
// Anti-Lag is on. Mesa's layer (26.2, anti_lag_QueueSubmit) appends its own timeline semaphore to a vkQueueSubmit's
// signals but copies the chained VkTimelineSemaphoreSubmitInfo values one slot too far (to &semaphore_values[1]): the
// emulator's timeline was signaled with 0 and the layer's with the emulator's tick, the waits on that tick never
// finished and the device was lost (deferred vkQueueSubmit -4 a few frames after the first input mark). Its
// vkQueueSubmit2 path appends a whole VkSemaphoreSubmitInfo.
[[nodiscard]] VkResult QueueSubmit(VkQueue queue, const VkSubmitInfo& info, PFN_vkQueueSubmit submit,
                                   PFN_vkQueueSubmit2 submit2);

} // namespace Libs::Graphics::AntiLag

#endif // EMULATOR_SRC_LOCAL_ANTI_LAG_H_
