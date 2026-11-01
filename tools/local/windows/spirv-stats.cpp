// Driver statistics of compute shaders (VK_KHR_pipeline_executable_properties): the register count,
// instruction count and spills the installed driver compiles a SPIR-V module to. Offline, no game:
// compare shader code generation variants before measuring them in a run.
//
//   spirv-stats.exe <module.spv> <layout.txt> [<module.spv> <layout.txt> ...]
//
// layout.txt (tools/local/spirv-stats.py writes it from spirv-cross --reflect): one line per
// descriptor binding of set 0, "binding <index> <VkDescriptorType> <count>".
//
// Build (x64 Native Tools prompt or after vcvars64):
//   clang-cl /O2 /EHsc /std:c++20 /I %VULKAN_SDK%\Include tools\local\windows\spirv-stats.cpp
//            /link /LIBPATH:%VULKAN_SDK%\Lib vulkan-1.lib /OUT:_Build\windows-tools\spirv-stats.exe
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static std::vector<char> ReadFile(const char* path) {
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static void Check(VkResult result, const char* what) {
	if (result != VK_SUCCESS) {
		std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
		std::exit(1);
	}
}

int main(int argc, char* argv[]) {
	if (argc < 3 || argc % 2 == 0) {
		std::fprintf(stderr, "usage: spirv-stats <module.spv> <layout.txt> [...]\n");
		return 2;
	}
	VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo instance_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_info.pApplicationInfo = &app;
	VkInstance instance = VK_NULL_HANDLE;
	Check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance");
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	for (auto device: devices) {
		VkPhysicalDeviceProperties properties {};
		vkGetPhysicalDeviceProperties(device, &properties);
		if (physical == VK_NULL_HANDLE || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
			physical = device;
			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
				std::printf("device: %s\n", properties.deviceName);
				break;
			}
		}
	}
	// Every supported feature on, as on the emulator's device (robust buffer access 1 and 2 change
	// the bounds checks the driver compiles in).
	VkPhysicalDeviceRobustness2FeaturesEXT robustness2 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable {
	    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
	executable.pNext = &robustness2;
	VkPhysicalDeviceVulkan13Features features13 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	features13.pNext = &executable;
	VkPhysicalDeviceVulkan12Features features12 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
	features12.pNext = &features13;
	VkPhysicalDeviceVulkan11Features features11 {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
	features11.pNext = &features12;
	VkPhysicalDeviceFeatures2 features {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	features.pNext = &features11;
	vkGetPhysicalDeviceFeatures2(physical, &features);
	if (!executable.pipelineExecutableInfo) {
		std::fprintf(stderr, "pipelineExecutableInfo is not supported\n");
		return 1;
	}
	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queue {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;
	const char* extensions[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
	                            VK_EXT_ROBUSTNESS_2_EXTENSION_NAME};
	VkDeviceCreateInfo device_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	device_info.pNext                   = &features;
	device_info.queueCreateInfoCount    = 1;
	device_info.pQueueCreateInfos       = &queue;
	device_info.enabledExtensionCount   = 2;
	device_info.ppEnabledExtensionNames = extensions;
	VkDevice device = VK_NULL_HANDLE;
	Check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice");
	const auto get_executables = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
	    vkGetDeviceProcAddr(device, "vkGetPipelineExecutablePropertiesKHR"));
	const auto get_statistics = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
	    vkGetDeviceProcAddr(device, "vkGetPipelineExecutableStatisticsKHR"));

	for (int arg = 1; arg + 1 < argc; arg += 2) {
		const auto code = ReadFile(argv[arg]);
		std::vector<VkDescriptorSetLayoutBinding> bindings;
		std::ifstream layout(argv[arg + 1]);
		for (std::string word; layout >> word;) {
			if (word != "binding") continue;
			VkDescriptorSetLayoutBinding binding {};
			uint32_t type = 0;
			layout >> binding.binding >> type >> binding.descriptorCount;
			binding.descriptorType = static_cast<VkDescriptorType>(type);
			binding.stageFlags     = VK_SHADER_STAGE_COMPUTE_BIT;
			bindings.push_back(binding);
		}
		VkShaderModuleCreateInfo module_info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		module_info.codeSize = code.size();
		module_info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
		VkShaderModule module = VK_NULL_HANDLE;
		Check(vkCreateShaderModule(device, &module_info, nullptr, &module), "vkCreateShaderModule");
		VkDescriptorSetLayoutCreateInfo set_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		set_info.bindingCount = static_cast<uint32_t>(bindings.size());
		set_info.pBindings    = bindings.data();
		VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
		Check(vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout), "vkCreateDescriptorSetLayout");
		const VkPushConstantRange push {VK_SHADER_STAGE_COMPUTE_BIT, 0, 256};
		VkPipelineLayoutCreateInfo pipeline_layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		pipeline_layout_info.setLayoutCount         = 1;
		pipeline_layout_info.pSetLayouts            = &set_layout;
		pipeline_layout_info.pushConstantRangeCount = 1;
		pipeline_layout_info.pPushConstantRanges    = &push;
		VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
		Check(vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout),
		      "vkCreatePipelineLayout");
		VkComputePipelineCreateInfo pipeline_info {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		pipeline_info.flags        = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
		pipeline_info.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		pipeline_info.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
		pipeline_info.stage.module = module;
		pipeline_info.stage.pName  = "main";
		pipeline_info.layout       = pipeline_layout;
		VkPipeline pipeline = VK_NULL_HANDLE;
		Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
		      "vkCreateComputePipelines");
		VkPipelineInfoKHR info {VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
		info.pipeline       = pipeline;
		uint32_t executables = 0;
		get_executables(device, &info, &executables, nullptr);
		std::printf("%s:", argv[arg]);
		for (uint32_t index = 0; index < executables; ++index) {
			VkPipelineExecutableInfoKHR executable_info {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
			executable_info.pipeline        = pipeline;
			executable_info.executableIndex = index;
			uint32_t statistics = 0;
			get_statistics(device, &executable_info, &statistics, nullptr);
			std::vector<VkPipelineExecutableStatisticKHR> values(
			    statistics, VkPipelineExecutableStatisticKHR {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
			get_statistics(device, &executable_info, &statistics, values.data());
			for (const auto& value: values) {
				std::printf(" [%s=", value.name);
				switch (value.format) {
					case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("%u", value.value.b32); break;
					case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
						std::printf("%lld", static_cast<long long>(value.value.i64));
						break;
					case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
						std::printf("%llu", static_cast<unsigned long long>(value.value.u64));
						break;
					case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: std::printf("%.3f", value.value.f64); break;
					default: std::printf("?");
				}
				std::printf("]");
			}
		}
		std::printf("\n");
		std::fflush(stdout);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		vkDestroyShaderModule(device, module, nullptr);
	}
	vkDestroyDevice(device, nullptr);
	vkDestroyInstance(instance, nullptr);
	return 0;
}
