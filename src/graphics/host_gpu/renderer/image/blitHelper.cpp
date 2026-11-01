#include "graphics/host_gpu/renderer/image/blitHelper.h"

#include "common/assert.h"
#include "gpu_blit_shaders/gpu_blit_color32_to_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_color_to_ms_depth_spv.h"
#include "gpu_blit_shaders/gpu_blit_depth_to_color32_spv.h"
#include "gpu_blit_shaders/gpu_blit_fs_triangle_spv.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/renderTarget.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <vulkan/vulkan_format_traits.hpp>

namespace Libs::Graphics {

namespace {

struct DepthToColorPush {
	uint32_t width;
	uint32_t height;
	uint32_t level;
};

struct ColorToDepthPush {
	uint32_t layer;
};

} // namespace

BlitHelper::BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics), m_scheduler(scheduler) {
	vk::DescriptorSetLayoutBinding texture_binding {};
	texture_binding.binding         = 0;
	texture_binding.descriptorType  = vk::DescriptorType::eSampledImage;
	texture_binding.descriptorCount = 1;
	texture_binding.stageFlags      = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo descriptor_info {};
	descriptor_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	descriptor_info.bindingCount = 1;
	descriptor_info.pBindings    = &texture_binding;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_descriptor_layout),
	                     "create BlitHelper descriptor layout");

	vk::PipelineLayoutCreateInfo layout_info {};
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts    = &m_descriptor_layout;
	RequireVulkanSuccess(
	    m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_pipeline_layout),
	    "create BlitHelper pipeline layout");

	m_vertex_shader = CreateShader(GPU_BLIT_FS_TRIANGLE_SPV, std::size(GPU_BLIT_FS_TRIANGLE_SPV));
	m_fragment_shader =
	    CreateShader(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV, std::size(GPU_BLIT_COLOR_TO_MS_DEPTH_SPV));

	// Depth <-> 32-bit colour reinterpretation passes.
	const auto r32uint = m_graphics.GetFormatProperties(vk::Format::eR32Uint);
	m_reinterpret_supported =
	    static_cast<bool>(r32uint.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage);
	if (!m_reinterpret_supported) {
		return;
	}
	const std::array<vk::DescriptorSetLayoutBinding, 2> d2c_bindings {{
	    {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	}};
	descriptor_info.bindingCount = static_cast<uint32_t>(d2c_bindings.size());
	descriptor_info.pBindings    = d2c_bindings.data();
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_d2c_descriptor_layout),
	                     "create depth-to-color descriptor layout");
	const vk::PushConstantRange d2c_push {vk::ShaderStageFlagBits::eCompute, 0, sizeof(DepthToColorPush)};
	layout_info.pSetLayouts            = &m_d2c_descriptor_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges    = &d2c_push;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_d2c_pipeline_layout),
	                     "create depth-to-color pipeline layout");
	{
		const auto module = CreateShader(GPU_BLIT_DEPTH_TO_COLOR32_SPV, std::size(GPU_BLIT_DEPTH_TO_COLOR32_SPV));
		vk::ComputePipelineCreateInfo create {};
		create.stage.stage  = vk::ShaderStageFlagBits::eCompute;
		create.stage.module = module;
		create.stage.pName  = "main";
		create.layout       = m_d2c_pipeline_layout;
		const auto result =
		    m_graphics.device.createComputePipelines(nullptr, 1, &create, nullptr, &m_d2c_pipeline);
		m_graphics.device.destroyShaderModule(module, nullptr);
		RequireVulkanSuccess(result, "create depth-to-color pipeline");
	}
	const vk::DescriptorSetLayoutBinding c2d_binding {0, vk::DescriptorType::eStorageImage, 1,
	                                                  vk::ShaderStageFlagBits::eFragment, nullptr};
	descriptor_info.bindingCount = 1;
	descriptor_info.pBindings    = &c2d_binding;
	RequireVulkanSuccess(m_graphics.device.createDescriptorSetLayout(&descriptor_info, nullptr,
	                                                                 &m_c2d_descriptor_layout),
	                     "create color-to-depth descriptor layout");
	const vk::PushConstantRange c2d_push {vk::ShaderStageFlagBits::eFragment, 0, sizeof(ColorToDepthPush)};
	layout_info.pSetLayouts         = &m_c2d_descriptor_layout;
	layout_info.pPushConstantRanges = &c2d_push;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&layout_info, nullptr, &m_c2d_pipeline_layout),
	                     "create color-to-depth pipeline layout");
	m_c2d_fragment_shader =
	    CreateShader(GPU_BLIT_COLOR32_TO_DEPTH_SPV, std::size(GPU_BLIT_COLOR32_TO_DEPTH_SPV));
}

BlitHelper::~BlitHelper() {
	for (const auto& pipeline: m_pipelines) {
		m_graphics.device.destroyPipeline(pipeline.handle, nullptr);
	}
	for (const auto pipeline: {m_d2c_pipeline, m_c2d_pipeline}) {
		if (pipeline != nullptr) m_graphics.device.destroyPipeline(pipeline, nullptr);
	}
	for (const auto layout: {m_d2c_pipeline_layout, m_c2d_pipeline_layout}) {
		if (layout != nullptr) m_graphics.device.destroyPipelineLayout(layout, nullptr);
	}
	for (const auto layout: {m_d2c_descriptor_layout, m_c2d_descriptor_layout}) {
		if (layout != nullptr) m_graphics.device.destroyDescriptorSetLayout(layout, nullptr);
	}
	if (m_c2d_fragment_shader != nullptr) {
		m_graphics.device.destroyShaderModule(m_c2d_fragment_shader, nullptr);
	}
	if (m_fragment_shader != nullptr) {
		m_graphics.device.destroyShaderModule(m_fragment_shader, nullptr);
	}
	if (m_vertex_shader != nullptr) {
		m_graphics.device.destroyShaderModule(m_vertex_shader, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_descriptor_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_descriptor_layout, nullptr);
	}
}

vk::ShaderModule BlitHelper::CreateShader(const uint32_t* code, size_t words) const {
	EXIT_IF(code == nullptr || words == 0);
	vk::ShaderModuleCreateInfo create {};
	create.codeSize         = words * sizeof(uint32_t);
	create.pCode            = code;
	vk::ShaderModule module = nullptr;
	RequireVulkanSuccess(m_graphics.device.createShaderModule(&create, nullptr, &module),
	                     "create BlitHelper shader module");
	return module;
}

vk::Pipeline BlitHelper::GetPipeline(PipelineKey key) {
	const auto cached = std::ranges::find(m_pipelines, key, &Pipeline::key);
	if (cached != m_pipelines.end()) {
		return cached->handle;
	}

	const auto samples = vulkan_sample_count(key.samples);
	EXIT_IF(samples == vk::SampleCountFlagBits {} || key.format == vk::Format::eUndefined);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
	stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
	stages[0].module = m_vertex_shader;
	stages[0].pName  = "main";
	stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
	stages[1].module = m_fragment_shader;
	stages[1].pName  = "main";

	vk::PipelineVertexInputStateCreateInfo vertex_input {};
	vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
	input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
	vk::PipelineViewportStateCreateInfo viewport {};
	viewport.viewportCount = 1;
	viewport.scissorCount  = 1;
	vk::PipelineRasterizationStateCreateInfo rasterization {};
	rasterization.lineWidth = 1.0f;
	vk::PipelineMultisampleStateCreateInfo multisample {};
	multisample.rasterizationSamples = samples;
	vk::PipelineDepthStencilStateCreateInfo depth {};
	depth.depthTestEnable  = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp   = vk::CompareOp::eAlways;
	vk::PipelineColorBlendStateCreateInfo color_blend {};
	const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
	vk::PipelineDynamicStateCreateInfo dynamic {};
	dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
	dynamic.pDynamicStates    = dynamic_states.data();

	vk::PipelineRenderingCreateInfo rendering {};
	rendering.depthAttachmentFormat = key.format;

	vk::GraphicsPipelineCreateInfo create {};
	create.pNext               = &rendering;
	create.stageCount          = static_cast<uint32_t>(stages.size());
	create.pStages             = stages.data();
	create.pVertexInputState   = &vertex_input;
	create.pInputAssemblyState = &input_assembly;
	create.pViewportState      = &viewport;
	create.pRasterizationState = &rasterization;
	create.pMultisampleState   = &multisample;
	create.pDepthStencilState  = &depth;
	create.pColorBlendState    = &color_blend;
	create.pDynamicState       = &dynamic;
	create.layout              = m_pipeline_layout;

	vk::Pipeline pipeline = nullptr;
	RequireVulkanSuccess(
	    m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &pipeline),
	    "create color-to-MS-depth pipeline");
	m_pipelines.push_back({key, pipeline});
	return pipeline;
}

void BlitHelper::ReinterpretColorAsMsDepth(Image& source, Image& destination) {
	const auto& source_info      = source.info;
	const auto& destination_info = destination.info;
	EXIT_IF(DepthAspectTransferFormat(source_info.pixel_format) != vk::Format::eUndefined ||
	        DepthAspectTransferFormat(destination_info.pixel_format) == vk::Format::eUndefined ||
	        source_info.samples != 1 || destination_info.samples <= 1 ||
	        destination_info.samples > 4 || source.backing.image_type != vk::ImageType::e2D ||
	        destination.backing.image_type != vk::ImageType::e2D ||
	        source_info.extent.width != destination_info.extent.width ||
	        source_info.extent.height != destination_info.extent.height ||
	        source_info.extent.depth != 1 || destination_info.extent.depth != 1 ||
	        source.backing.image == nullptr || destination.backing.image == nullptr);
	m_scheduler.EndRendering();

	ImageViewInfo source_view_info {};
	source_view_info.format = source_info.pixel_format;
	source_view_info.type   = vk::ImageViewType::e2D;
	source_view_info.aspect = vk::ImageAspectFlagBits::eColor;
	source_view_info.usage  = vk::ImageUsageFlagBits::eSampled;
	const auto source_view  = source.FindView(source_view_info);

	ImageViewInfo destination_view_info {};
	destination_view_info.format = destination_info.pixel_format;
	destination_view_info.type   = vk::ImageViewType::e2D;
	destination_view_info.aspect = vk::ImageAspectFlagBits::eDepth;
	destination_view_info.usage  = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	const auto destination_view  = destination.FindView(destination_view_info);

	auto& command_buffer = m_scheduler.Current();
	command_buffer.InvalidateGraphicsState();
	auto  command        = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
	               command);
	destination.Transit(ColorToMsDepthLayout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {},
	                    command);

	vk::RenderingAttachmentInfo depth_attachment {};
	depth_attachment.imageView               = destination_view;
	depth_attachment.imageLayout             = ColorToMsDepthLayout;
	depth_attachment.loadOp                  = vk::AttachmentLoadOp::eClear;
	depth_attachment.storeOp                 = vk::AttachmentStoreOp::eStore;
	depth_attachment.clearValue.depthStencil = {0.0f, 0};

	vk::RenderingInfo rendering {};
	rendering.renderArea.extent = {destination_info.extent.width, destination_info.extent.height};
	rendering.layerCount        = 1;
	rendering.pDepthAttachment  = &depth_attachment;
	command.beginRendering(&rendering);

	vk::DescriptorImageInfo descriptor_image {};
	descriptor_image.imageView   = source_view;
	descriptor_image.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	vk::WriteDescriptorSet descriptor_write {};
	descriptor_write.dstBinding      = 0;
	descriptor_write.descriptorCount = 1;
	descriptor_write.descriptorType  = vk::DescriptorType::eSampledImage;
	descriptor_write.pImageInfo      = &descriptor_image;
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_pipeline_layout, 0, 1,
	                             &descriptor_write);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics,
	                     GetPipeline({destination_info.samples, destination_info.pixel_format}));

	const vk::Viewport viewport {0.0f,
	                             0.0f,
	                             static_cast<float>(destination_info.extent.width),
	                             static_cast<float>(destination_info.extent.height),
	                             0.0f,
	                             1.0f};
	const vk::Rect2D   scissor {{0, 0},
	                            {destination_info.extent.width, destination_info.extent.height}};
	command.setViewport(0, 1, &viewport);
	command.setScissor(0, 1, &scissor);
	command.draw(3, 1, 0, 0);
	command.endRendering();
}

bool BlitHelper::SupportsDepthColor32(const Image& depth, const Image& color) const {
	const auto& d = depth.backing;
	const auto& c = color.backing;
	return m_reinterpret_supported && d.image != nullptr && c.image != nullptr && d.samples == 1 && c.samples == 1 &&
	       d.image_type == vk::ImageType::e2D && c.image_type == vk::ImageType::e2D &&
	       d.extent.width == c.extent.width && d.extent.height == c.extent.height &&
	       d.format == vk::Format::eD32Sfloat && !color.info.IsDepth() && !color.info.IsBlock() &&
	       vk::blockSize(c.format) == 4 &&
	       static_cast<bool>(d.usage & vk::ImageUsageFlagBits::eSampled) &&
	       static_cast<bool>(d.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) &&
	       static_cast<bool>(c.usage & vk::ImageUsageFlagBits::eStorage) &&
	       (c.format == vk::Format::eR32Uint || static_cast<bool>(c.flags & vk::ImageCreateFlagBits::eMutableFormat));
}

void BlitHelper::CopyDepthToColor32(Image& source, Image& destination) {
	EXIT_IF(!SupportsDepthColor32(source, destination));
	const uint32_t levels = std::min(source.backing.mip_levels, destination.backing.mip_levels);
	const uint32_t layers = std::min(source.backing.layers, destination.backing.layers);
	m_scheduler.EndRendering();

	ImageViewInfo source_view_info {};
	source_view_info.format      = source.backing.format;
	source_view_info.type        = vk::ImageViewType::e2DArray;
	source_view_info.aspect      = vk::ImageAspectFlagBits::eDepth;
	source_view_info.level_count = levels;
	source_view_info.layer_count = layers;
	source_view_info.usage       = vk::ImageUsageFlagBits::eSampled;
	const auto source_view       = source.FindView(source_view_info);

	auto command = m_scheduler.Current().Handle();
	source.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {}, command);
	destination.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderWrite, {}, command);
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_d2c_pipeline);
	for (uint32_t level = 0; level < levels; level++) {
		ImageViewInfo destination_view_info {};
		destination_view_info.format      = vk::Format::eR32Uint;
		destination_view_info.type        = vk::ImageViewType::e2DArray;
		destination_view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		destination_view_info.base_level  = level;
		destination_view_info.level_count = 1;
		destination_view_info.layer_count = layers;
		destination_view_info.usage       = vk::ImageUsageFlagBits::eStorage;
		const auto destination_view       = destination.FindView(destination_view_info);

		const vk::DescriptorImageInfo source_image {nullptr, source_view, vk::ImageLayout::eShaderReadOnlyOptimal};
		const vk::DescriptorImageInfo destination_image {nullptr, destination_view, vk::ImageLayout::eGeneral};
		std::array<vk::WriteDescriptorSet, 2> writes {};
		writes[0].dstBinding      = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType  = vk::DescriptorType::eSampledImage;
		writes[0].pImageInfo      = &source_image;
		writes[1].dstBinding      = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType  = vk::DescriptorType::eStorageImage;
		writes[1].pImageInfo      = &destination_image;
		command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_d2c_pipeline_layout, 0,
		                             static_cast<uint32_t>(writes.size()), writes.data());
		const DepthToColorPush push {std::max(source.backing.extent.width >> level, 1u),
		                             std::max(source.backing.extent.height >> level, 1u), level};
		command.pushConstants(m_d2c_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
		command.dispatch((push.width + 7u) / 8u, (push.height + 7u) / 8u, layers);
	}
}

void BlitHelper::CopyColor32ToDepth(Image& source, Image& destination) {
	EXIT_IF(!SupportsDepthColor32(destination, source));
	const uint32_t levels = std::min(source.backing.mip_levels, destination.backing.mip_levels);
	const uint32_t layers = std::min(source.backing.layers, destination.backing.layers);
	m_scheduler.EndRendering();

	if (m_c2d_pipeline == nullptr) {
		std::array<vk::PipelineShaderStageCreateInfo, 2> stages {};
		stages[0].stage  = vk::ShaderStageFlagBits::eVertex;
		stages[0].module = m_vertex_shader;
		stages[0].pName  = "main";
		stages[1].stage  = vk::ShaderStageFlagBits::eFragment;
		stages[1].module = m_c2d_fragment_shader;
		stages[1].pName  = "main";
		vk::PipelineVertexInputStateCreateInfo   vertex_input {};
		vk::PipelineInputAssemblyStateCreateInfo input_assembly {};
		input_assembly.topology = vk::PrimitiveTopology::eTriangleList;
		vk::PipelineViewportStateCreateInfo viewport {};
		viewport.viewportCount = 1;
		viewport.scissorCount  = 1;
		vk::PipelineRasterizationStateCreateInfo rasterization {};
		rasterization.lineWidth = 1.0f;
		vk::PipelineMultisampleStateCreateInfo multisample {};
		multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;
		vk::PipelineDepthStencilStateCreateInfo depth {};
		depth.depthTestEnable  = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp   = vk::CompareOp::eAlways;
		vk::PipelineColorBlendStateCreateInfo color_blend {};
		const std::array dynamic_states {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
		vk::PipelineDynamicStateCreateInfo dynamic {};
		dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
		dynamic.pDynamicStates    = dynamic_states.data();
		vk::PipelineRenderingCreateInfo rendering {};
		rendering.depthAttachmentFormat = vk::Format::eD32Sfloat;
		vk::GraphicsPipelineCreateInfo create {};
		create.pNext               = &rendering;
		create.stageCount          = static_cast<uint32_t>(stages.size());
		create.pStages             = stages.data();
		create.pVertexInputState   = &vertex_input;
		create.pInputAssemblyState = &input_assembly;
		create.pViewportState      = &viewport;
		create.pRasterizationState = &rasterization;
		create.pMultisampleState   = &multisample;
		create.pDepthStencilState  = &depth;
		create.pColorBlendState    = &color_blend;
		create.pDynamicState       = &dynamic;
		create.layout              = m_c2d_pipeline_layout;
		RequireVulkanSuccess(m_graphics.device.createGraphicsPipelines(nullptr, 1, &create, nullptr, &m_c2d_pipeline),
		                     "create color-to-depth pipeline");
	}

	auto& command_buffer = m_scheduler.Current();
	command_buffer.InvalidateGraphicsState();
	auto command = command_buffer.Handle();
	source.Transit(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead, {}, command);
	destination.Transit(ColorToMsDepthLayout, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, {}, command);
	command.bindPipeline(vk::PipelineBindPoint::eGraphics, m_c2d_pipeline);
	for (uint32_t level = 0; level < levels; level++) {
		const uint32_t width  = std::max(destination.backing.extent.width >> level, 1u);
		const uint32_t height = std::max(destination.backing.extent.height >> level, 1u);

		ImageViewInfo source_view_info {};
		source_view_info.format      = vk::Format::eR32Uint;
		source_view_info.type        = vk::ImageViewType::e2DArray;
		source_view_info.aspect      = vk::ImageAspectFlagBits::eColor;
		source_view_info.base_level  = level;
		source_view_info.level_count = 1;
		source_view_info.layer_count = layers;
		source_view_info.usage       = vk::ImageUsageFlagBits::eStorage;
		const auto source_view       = source.FindView(source_view_info);
		const vk::DescriptorImageInfo source_image {nullptr, source_view, vk::ImageLayout::eGeneral};

		for (uint32_t layer = 0; layer < layers; layer++) {
			ImageViewInfo destination_view_info {};
			destination_view_info.format     = destination.backing.format;
			destination_view_info.type       = vk::ImageViewType::e2D;
			destination_view_info.aspect     = vk::ImageAspectFlagBits::eDepth;
			destination_view_info.base_level = level;
			destination_view_info.base_layer = layer;
			destination_view_info.usage      = vk::ImageUsageFlagBits::eDepthStencilAttachment;
			const auto destination_view      = destination.FindView(destination_view_info);

			vk::RenderingAttachmentInfo depth_attachment {};
			depth_attachment.imageView   = destination_view;
			depth_attachment.imageLayout = ColorToMsDepthLayout;
			// (The fullscreen triangle writes every texel of the render area.)
			depth_attachment.loadOp  = vk::AttachmentLoadOp::eDontCare;
			depth_attachment.storeOp = vk::AttachmentStoreOp::eStore;
			vk::RenderingInfo rendering {};
			rendering.renderArea.extent = vk::Extent2D {width, height};
			rendering.layerCount        = 1;
			rendering.pDepthAttachment  = &depth_attachment;
			command.beginRendering(&rendering);

			vk::WriteDescriptorSet write {};
			write.dstBinding      = 0;
			write.descriptorCount = 1;
			write.descriptorType  = vk::DescriptorType::eStorageImage;
			write.pImageInfo      = &source_image;
			command.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_c2d_pipeline_layout, 0, 1, &write);
			const ColorToDepthPush push {layer};
			command.pushConstants(m_c2d_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, sizeof(push), &push);
			const vk::Viewport viewport {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
			const vk::Rect2D   scissor {{0, 0}, {width, height}};
			command.setViewport(0, 1, &viewport);
			command.setScissor(0, 1, &scissor);
			command.draw(3, 1, 0, 0);
			command.endRendering();
		}
	}
}

} // namespace Libs::Graphics
