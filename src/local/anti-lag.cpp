#include "local/anti-lag.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Libs::Graphics::AntiLag {

namespace {

PFN_vkAntiLagUpdateAMD g_update = nullptr;
vk::Device             g_device = nullptr;
bool                   g_feature = false;           // the GPU offers the feature: ChainFeature enabled it
std::atomic<uint64_t>  g_flips {0};                 // guest flips (Flip)
std::atomic<uint64_t>  g_marked_flips {~uint64_t {0}}; // the flips counted at the last input mark
std::atomic<uint64_t>  g_input_frame {0};           // the frame of the last input mark
std::atomic<uint64_t>  g_presented {0};             // the newest frame whose present was marked

void Update(VkAntiLagStageAMD stage, uint64_t frame) {
	// KYTY_ANTI_LAG_STAGES=input|present|none (a diagnostic): only those marks reach the driver.
	static const int stages = [] {
		const char* value = std::getenv("KYTY_ANTI_LAG_STAGES");
		if (value == nullptr) return 3;
		return (std::strstr(value, "input") != nullptr ? 1 : 0) | (std::strstr(value, "present") != nullptr ? 2 : 0);
	}();
	if ((stages & (stage == VK_ANTI_LAG_STAGE_INPUT_AMD ? 1 : 2)) == 0) return;
	VkAntiLagPresentationInfoAMD presentation {};
	presentation.sType      = VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD;
	presentation.stage      = stage;
	presentation.frameIndex = frame;
	VkAntiLagDataAMD data {};
	data.sType             = VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD;
	data.mode              = VK_ANTI_LAG_MODE_ON_AMD;
	data.maxFPS            = 0;
	data.pPresentationInfo = &presentation;
	g_update(static_cast<VkDevice>(g_device), &data);
}

// KYTY_ANTI_LAG_INPUT=pad: the input mark at the guest's pad read (a guest thread, beside the submissions); default
// "flip": at the guest flip on the GPU thread, before it records the next frame.
bool MarkAtPad() {
	static const bool pad = [] {
		const char* value = std::getenv("KYTY_ANTI_LAG_INPUT");
		return value != nullptr && std::strcmp(value, "pad") == 0;
	}();
	return pad;
}

} // namespace

bool Requested() {
	static const bool requested = [] {
		const char* low_latency = std::getenv("KYTY_LOW_LATENCY");
		const char* anti_lag    = std::getenv("KYTY_ANTI_LAG");
		return low_latency != nullptr && std::atoi(low_latency) != 0 && anti_lag != nullptr && std::atoi(anti_lag) != 0;
	}();
	return requested;
}

void BeforeInstance() {
#if !defined(_WIN32)
	if (Requested()) setenv("ENABLE_LAYER_MESA_ANTI_LAG", "1", 0);
#endif
}

void AddDeviceExtension(const std::vector<vk::ExtensionProperties>& available, std::vector<const char*>& extensions) {
	if (!Requested()) return;
	const auto has = [](const char* name) {
		return [name](const vk::ExtensionProperties& item) { return std::strcmp(item.extensionName.data(), name) == 0; };
	};
	if (std::none_of(available.begin(), available.end(), has(VK_AMD_ANTI_LAG_EXTENSION_NAME))) {
		std::printf("Low latency: no %s on this GPU or driver (Mesa: VK_LAYER_MESA_anti_lag); the one-frame flip queue only\n",
		            VK_AMD_ANTI_LAG_EXTENSION_NAME);
		std::fflush(stdout);
		return;
	}
	if (std::none_of(extensions.begin(), extensions.end(),
	                 [](const char* name) { return std::strcmp(name, VK_AMD_ANTI_LAG_EXTENSION_NAME) == 0; }))
		extensions.push_back(VK_AMD_ANTI_LAG_EXTENSION_NAME);
}

void ChainFeature(vk::PhysicalDevice physical_device, const std::vector<const char*>& extensions,
                  vk::DeviceCreateInfo& create_info, VkPhysicalDeviceAntiLagFeaturesAMD& storage) {
	if (std::none_of(extensions.begin(), extensions.end(),
	                 [](const char* name) { return std::strcmp(name, VK_AMD_ANTI_LAG_EXTENSION_NAME) == 0; }))
		return;
	VkPhysicalDeviceAntiLagFeaturesAMD supported {};
	supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
	VkPhysicalDeviceFeatures2 features {};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &supported;
	VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFeatures2(static_cast<VkPhysicalDevice>(physical_device), &features);
	if (supported.antiLag == VK_FALSE) return;
	storage         = {};
	storage.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;
	storage.antiLag = VK_TRUE;
	storage.pNext   = const_cast<void*>(create_info.pNext);
	create_info.pNext = &storage;
	g_feature         = true;
}

void OnDevice(vk::Device device) {
	if (!g_feature || device == nullptr) return;
	g_update = reinterpret_cast<PFN_vkAntiLagUpdateAMD>(
	    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(static_cast<VkDevice>(device), "vkAntiLagUpdateAMD"));
	g_device = device;
	std::printf("Low latency: AMD Anti-Lag %s\n", g_update != nullptr ? "on (VK_AMD_anti_lag)" : "not available");
	std::fflush(stdout);
}

bool Enabled() {
	return g_update != nullptr;
}

void Input() {
	if (g_update == nullptr || !MarkAtPad()) return;
	// (The first read after a flip: Demon's Souls reads the pad once a tick, other games may read it more often.)
	const auto flips = g_flips.load(std::memory_order_acquire);
	auto       marked = g_marked_flips.load(std::memory_order_relaxed);
	if (marked == flips || !g_marked_flips.compare_exchange_strong(marked, flips, std::memory_order_acq_rel)) return;
	Update(VK_ANTI_LAG_STAGE_INPUT_AMD, g_input_frame.fetch_add(1, std::memory_order_acq_rel) + 1);
}

uint64_t Flip() {
	if (g_update == nullptr) return 0;
	if (!MarkAtPad()) {
		// The flip's frame is presented next; the input of the frame after it starts here.
		const auto frame = g_input_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
		Update(VK_ANTI_LAG_STAGE_INPUT_AMD, frame + 1);
		return frame;
	}
	g_flips.fetch_add(1, std::memory_order_acq_rel);
	return g_input_frame.load(std::memory_order_acquire);
}

VkResult QueueSubmit(VkQueue queue, const VkSubmitInfo& info, PFN_vkQueueSubmit submit, PFN_vkQueueSubmit2 submit2) {
	if (g_update == nullptr || submit2 == nullptr) return submit(queue, 1, &info, VK_NULL_HANDLE);
	const VkTimelineSemaphoreSubmitInfo* timeline = nullptr;
	for (const auto* next = static_cast<const VkBaseInStructure*>(info.pNext); next != nullptr; next = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
			timeline = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
		}
	}
	thread_local std::vector<VkSemaphoreSubmitInfo>     waits;
	thread_local std::vector<VkSemaphoreSubmitInfo>     signals;
	thread_local std::vector<VkCommandBufferSubmitInfo> commands;
	waits.assign(info.waitSemaphoreCount, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO});
	for (uint32_t i = 0; i < info.waitSemaphoreCount; ++i) {
		waits[i].semaphore = info.pWaitSemaphores[i];
		waits[i].value     = timeline != nullptr && i < timeline->waitSemaphoreValueCount ? timeline->pWaitSemaphoreValues[i] : 0;
		waits[i].stageMask = info.pWaitDstStageMask[i]; // (the same bits)
	}
	signals.assign(info.signalSemaphoreCount, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO});
	for (uint32_t i = 0; i < info.signalSemaphoreCount; ++i) {
		signals[i].semaphore = info.pSignalSemaphores[i];
		signals[i].value =
		    timeline != nullptr && i < timeline->signalSemaphoreValueCount ? timeline->pSignalSemaphoreValues[i] : 0;
		signals[i].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	}
	commands.assign(info.commandBufferCount, {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO});
	for (uint32_t i = 0; i < info.commandBufferCount; ++i) commands[i].commandBuffer = info.pCommandBuffers[i];
	VkSubmitInfo2 info2 {};
	info2.sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
	info2.waitSemaphoreInfoCount   = info.waitSemaphoreCount;
	info2.pWaitSemaphoreInfos      = waits.data();
	info2.commandBufferInfoCount   = info.commandBufferCount;
	info2.pCommandBufferInfos      = commands.data();
	info2.signalSemaphoreInfoCount = info.signalSemaphoreCount;
	info2.pSignalSemaphoreInfos    = signals.data();
	return submit2(queue, 1, &info2, VK_NULL_HANDLE);
}

void Present(uint64_t frame) {
	// (Each frame's present once, in order: a frame shown again or older than one presented is not marked.)
	if (g_update == nullptr || frame == 0) return;
	auto presented = g_presented.load(std::memory_order_relaxed);
	if (frame <= presented || !g_presented.compare_exchange_strong(presented, frame, std::memory_order_acq_rel)) return;
	Update(VK_ANTI_LAG_STAGE_PRESENT_AMD, frame);
}

} // namespace Libs::Graphics::AntiLag
