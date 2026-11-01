#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <compare>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

class BlitHelper final {
public:
	inline static constexpr auto ColorToMsDepthLayout =
	    vk::ImageLayout::eDepthStencilAttachmentOptimal;

	BlitHelper(GraphicContext& graphics, CommandScheduler& scheduler);
	~BlitHelper();
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& source, Image& destination);

	// Bit-exact D32 depth <-> 32-bit colour copies without a staging buffer (Jetsku's U59 reinterpretation passes): the
	// depth sampled into an R32_UINT storage view of the colour image, and that view's bits drawn as the depth (values
	// outside [0, 1] clamp there). Single-sample 2D images of the same base extent.
	[[nodiscard]] bool SupportsDepthColor32(const Image& depth, const Image& color) const;
	void               CopyDepthToColor32(Image& source, Image& destination);
	void               CopyColor32ToDepth(Image& source, Image& destination);

private:
	struct PipelineKey {
		uint32_t   samples = 1;
		vk::Format format  = vk::Format::eUndefined;

		auto operator<=>(const PipelineKey&) const = default;
	};

	struct Pipeline {
		PipelineKey  key;
		vk::Pipeline handle = nullptr;
	};

	[[nodiscard]] vk::ShaderModule CreateShader(const uint32_t* code, size_t words) const;
	[[nodiscard]] vk::Pipeline     GetPipeline(PipelineKey key);

	GraphicContext&         m_graphics;
	CommandScheduler&       m_scheduler;
	vk::DescriptorSetLayout m_descriptor_layout = nullptr;
	vk::PipelineLayout      m_pipeline_layout   = nullptr;
	vk::ShaderModule        m_vertex_shader     = nullptr;
	vk::ShaderModule        m_fragment_shader   = nullptr;
	std::vector<Pipeline>   m_pipelines;

	bool                    m_reinterpret_supported = false;
	vk::DescriptorSetLayout m_d2c_descriptor_layout = nullptr;
	vk::PipelineLayout      m_d2c_pipeline_layout   = nullptr;
	vk::Pipeline            m_d2c_pipeline          = nullptr;
	vk::DescriptorSetLayout m_c2d_descriptor_layout = nullptr;
	vk::PipelineLayout      m_c2d_pipeline_layout   = nullptr;
	vk::ShaderModule        m_c2d_fragment_shader   = nullptr;
	vk::Pipeline            m_c2d_pipeline          = nullptr; // (D32_SFLOAT, made at its first use)
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_BLITHELPER_H_
