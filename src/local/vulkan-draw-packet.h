#pragma once

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include <span>

namespace Libs::Graphics::LocalDrawRecording {
// Host values only. These small adapters share the normal state expansion
// implementation without retaining the command buffer, image or guest memory.
struct Registers {
    HW::ScreenViewport viewport;
    HW::ClipControl clip;
    HW::ScanModeControl scan;
    HW::ModeControl mode;
    HW::PolyOffset offset;
    HW::BlendColor blend;
    float line_width = 1.0f;
    uint32_t target_mask = 0;
    uint32_t shader_mask = ~0u; // CB_SHADER_MASK
    struct ShaderRegisters { uint32_t m_cbShaderMask; };
    ShaderRegisters GetShaderRegisters() const { return {shader_mask}; }
    const auto& GetScreenViewport() const { return viewport; }
    const auto& GetClipControl() const { return clip; }
    const auto& GetScanModeControl() const { return scan; }
    const auto& GetModeControl() const { return mode; }
    const auto& GetPolyOffset() const { return offset; }
    const auto& GetBlendColor() const { return blend; }
    float GetLineWidth() const { return line_width; }
    uint32_t GetRenderTargetMask() const { return target_mask; }
};
struct Limits {
    uint32_t maxFramebufferWidth = 0, maxFramebufferHeight = 0;
    uint32_t maxViewportDimensions[2] {};
};
struct Graphics {
    struct Properties { Limits limits; } properties;
    const auto& GetPhysicalDeviceProperties() const { return properties; }
};
struct BufferState {
    Registers registers;
    Graphics graphics;
    const auto& GetRegisters() const { return registers; }
    const auto& GetGraphics() const { return graphics; }
};
struct Color {
    vk::Extent2D extent;
    uint32_t target_slot = 0;
    bool image_id = false;
    auto Extent() const { return extent; }
};
struct Depth {
    struct Desc {
        struct Info { vk::Extent2D extent; } info;
        struct View { vk::Format format = vk::Format::eUndefined; } view_info;
    } desc;
    PipelineStencilDynamicState stencil_dynamic_front, stencil_dynamic_back;
    vk::CompareOp depth_compare_op = vk::CompareOp::eNever;
    bool image_id = false;
    bool depth_test_enable = false, depth_write_enable = false, stencil_test_enable = false;
};
struct DynamicState {
    BufferState buffer;
    Color colors[RENDER_COLOR_ATTACHMENTS_MAX] {};
    Depth depth;
    uint32_t color_count = 0;
    bool indexed_viewports = false;
    bool feedback_enabled = false;
    vk::ImageAspectFlags feedback_aspects;
};
struct Draw {
    DynamicState state;
    vk::CommandBuffer command;
    vk::Pipeline pipeline;
    vk::Buffer index_buffer;
    vk::DeviceSize index_offset = 0;
    vk::IndexType index_type = vk::IndexType::eUint16;
    bool indexed = false;
    uint64_t debug_ps = 0; // Only for an explicitly selected raster diagnostic.
    uint32_t debug_render_width = 0, debug_render_height = 0;
};
// Each non-indexed record uses indexCount as vertexCount and firstIndex as
// firstVertex. No native multi-draw reordering or guest indirect reads occur.
void Record(const Draw& draw, std::span<const vk::Buffer> vertex_buffers,
            std::span<const vk::DeviceSize> vertex_offsets,
            std::span<const vk::DrawIndexedIndirectCommand> commands);
} // namespace Libs::Graphics::LocalDrawRecording
