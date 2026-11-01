#pragma once
// Live pixdump: native images already bound to a selected call. No FindImage,
// guest readback, detiling, CPU wait, or mutation of image content ownership.
#include "local/frame-capture.h"
#include "local/pixel-capture-layout.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include <nlohmann/json.hpp>
#include <vulkan/vulkan_format_traits.hpp>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <queue>

namespace Libs::Graphics::PixelCapture {
using Json = nlohmann::json;
inline std::string Hex(uint64_t value) {
    char text[32]; std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
    return text;
}
struct Output {
    std::filesystem::path stem;
    Json metadata;
    std::vector<uint8_t> bytes;
};
// Only CPU-owned bytes cross to the writer. All VMA buffers are released by the
// scheduler completion callback, before the device can be destroyed.
class Writer {
public:
    Writer(): worker([this] { Run(); }) {}
    ~Writer() {
        { std::lock_guard lock(mutex); stop = true; }
        ready.notify_one(); worker.join();
    }
    void Push(Output output) {
        { std::lock_guard lock(mutex); queue.push(std::move(output)); }
        ready.notify_one();
    }
private:
    void Run() {
        for (;;) {
            Output output;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stop || !queue.empty(); });
                if (queue.empty()) return;
                output = std::move(queue.front()); queue.pop();
            }
            try {
                std::filesystem::create_directories(output.stem.parent_path());
                if (!output.bytes.empty()) {
                    std::ofstream file(output.stem.string() + ".bin", std::ios::binary);
                    file.exceptions(std::ios::badbit | std::ios::failbit);
                    file.write(reinterpret_cast<const char*>(output.bytes.data()), output.bytes.size());
                    file.close();
                }
                std::ofstream file(output.stem.string() + ".json", std::ios::binary);
                file.exceptions(std::ios::badbit | std::ios::failbit);
                file << output.metadata.dump(2) << '\n'; file.close();
                if (output.bytes.empty()) ++FrameCapture::g_pixel_skipped;
                else ++FrameCapture::g_pixel_saved;
            } catch (const std::exception& e) {
                ++FrameCapture::g_pixel_failed;
                std::fprintf(stderr, "[PIXDUMP] write failed %s: %s\n", output.stem.string().c_str(), e.what());
            }
            FrameCapture::g_pixel_pending.fetch_sub(1, std::memory_order_release);
        }
    }
    std::mutex mutex;
    std::condition_variable ready;
    std::queue<Output> queue;
    bool stop = false;
    std::thread worker;
};
inline Writer& DiskWriter() { static Writer writer; return writer; }

inline void Copy(RenderContext& context, ImageId id, vk::ImageView view,
                 const char* role, uint32_t stage, uint32_t slot, const char* phase) {
    if (!FrameCapture::PixelSelected()) return;
    auto* image = context.GetTextureCache().TryGetImage(id);
    Output output;
    char name[128];
    std::snprintf(name, sizeof(name), "f%u-call%u-%u-%s-s%u-slot%u",
                  FrameCapture::g_frame, FrameCapture::g_seq, FrameCapture::g_pixel_serial++, role, stage, slot);
    output.stem = std::filesystem::path(FrameCapture::g_dir) / "pixels" / name;
    auto& meta = output.metadata;
    meta = {{"schema", 1}, {"frame", FrameCapture::g_frame}, {"call", FrameCapture::g_seq},
            {"phase", phase}, {"role", role}, {"stage", stage}, {"slot", slot},
            {"image_id", id.index}, {"image_generation", id.generation},
            {"view", Hex(reinterpret_cast<uintptr_t>(static_cast<VkImageView>(view)))},
            {"vs", Hex(FrameCapture::g_call.vs)}, {"ps", Hex(FrameCapture::g_call.ps)},
            {"cs", Hex(FrameCapture::g_call.cs)}};
    const auto skip = [&](const char* reason) {
        meta["status"] = "skipped"; meta["reason"] = reason;
        auto& writer = DiskWriter();
        ++FrameCapture::g_pixel_pending;
        try { writer.Push(std::move(output)); }
        catch (...) { --FrameCapture::g_pixel_pending; throw; }
    };
    if (!image || !image->backing.image) { skip("missing-native-image"); return; }
    const auto& native = image->backing;
    meta["image"] = Hex(reinterpret_cast<uintptr_t>(static_cast<VkImage>(native.image)));
    meta["guest_address"] = Hex(image->info.data.address);
    meta["guest_size"] = image->info.data.size;
    meta["guest_extent"] = {image->info.extent.width, image->info.extent.height, image->info.extent.depth};
    meta["native_extent"] = {native.extent.width, native.extent.height, native.extent.depth};
    meta["format"] = static_cast<uint32_t>(native.format);
    meta["format_name"] = vk::to_string(native.format);
    meta["samples"] = native.samples;
    meta["native_usage"] = static_cast<uint32_t>(native.usage);
    meta["native_create_flags"] = static_cast<uint32_t>(native.flags);
    meta["flags"] = {{"registered", image->registered}, {"gpu_modified", image->IsGpuModified()},
                     {"cpu_dirty", image->IsCpuDirty()}, {"buffer_modified", image->IsBufferModified()},
                     {"needs_rebind", image->binding.needs_rebind}};
    const auto found = std::ranges::find(image->views, view, &CachedImageView::view);
    if (found == image->views.end()) { skip("view-not-owned-by-bound-image"); return; }
    const auto& range = found->info;
    meta["view_format"] = static_cast<uint32_t>(range.format);
    meta["view_range"] = {range.base_level, range.level_count, range.base_layer, range.layer_count};
    meta["swizzle"] = {static_cast<uint32_t>(range.mapping.r), static_cast<uint32_t>(range.mapping.g),
                       static_cast<uint32_t>(range.mapping.b), static_cast<uint32_t>(range.mapping.a)};
    // A combined depth/stencil attachment is copied as its depth aspect only.
    // Vulkan's transfer representation has 2/4 bytes per depth texel, not the
    // blockSize of the combined native format. Never include stencil padding.
    const bool depth = static_cast<bool>(range.aspect & vk::ImageAspectFlagBits::eDepth);
    const auto aspect = depth ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor;
    const auto transfer_format = depth ? DepthAspectTransferFormat(native.format) : native.format;
    const auto texel_bytes = depth ? DepthAspectTransferBytes(native.format) : vk::blockSize(native.format);
    meta["transfer_aspect"] = depth ? "depth" : "color";
    meta["transfer_format"] = static_cast<uint32_t>(transfer_format);
    meta["transfer_format_name"] = vk::to_string(transfer_format);
    if (native.samples != 1 || native.image_type != vk::ImageType::e2D || image->info.IsBlock() ||
        (!depth && range.aspect != vk::ImageAspectFlagBits::eColor) || texel_bytes == 0 ||
        !(native.usage & vk::ImageUsageFlagBits::eTransferSrc)) {
        skip("requires-single-sample-uncompressed-color-or-depth-2D-transfer-source"); return;
    }
    if (range.base_level >= native.mip_levels || range.base_layer >= native.layers) {
        skip("view-outside-native-image"); return;
    }
    const auto levels = range.level_count == VK_REMAINING_MIP_LEVELS ? native.mip_levels - range.base_level : range.level_count;
    const auto layers = range.layer_count == VK_REMAINING_ARRAY_LAYERS ? native.layers - range.base_layer : range.layer_count;
    std::vector<PixelCaptureLayout::Plane> planes;
    uint64_t bytes = 0;
    if (!PixelCaptureLayout::Plan(native.extent.width, native.extent.height, native.mip_levels, native.layers,
                                 range.base_level, levels, range.base_layer, layers, texel_bytes, planes, bytes)) {
        skip("invalid-range-or-image-exceeds-128-MiB"); return;
    }
    if (bytes > PixelCaptureLayout::FrameLimit - FrameCapture::g_pixel_bytes) {
        skip("frame-exceeds-512-MiB"); return;
    }
    meta["planes"] = Json::array();
    std::vector<vk::BufferImageCopy> copies;
    for (const auto& p: planes) {
        meta["planes"].push_back({{"mip", p.mip}, {"layer", p.layer}, {"width", p.width}, {"height", p.height},
                                  {"offset", p.offset}, {"row_bytes", p.row_bytes}, {"size", p.size}});
        vk::BufferImageCopy copy {};
        copy.bufferOffset = p.offset;
        copy.imageSubresource = {aspect, p.mip, p.layer, 1};
        copy.imageExtent = {p.width, p.height, 1};
        copies.push_back(copy);
    }
    auto& scheduler = context.GetCommandScheduler();
    auto staging = std::make_shared<Buffer>(context.GetGraphics(), scheduler, MemoryUsage::Download, 0,
                                            vk::BufferUsageFlagBits::eTransferDst, bytes);
    if (staging->Mapped().size() < bytes) { skip("staging-not-mapped"); return; }
    // Register staging lifetime before recording any GPU work, including when a
    // later host allocation throws. The callback cannot run before this tick retires.
    output.bytes.resize(bytes);
    meta["status"] = "saved";
    meta["byte_order"] = "little-endian";
    meta["encoding"] = depth ? "depth-aspect-transfer-format-texels" : "native-VkFormat-texels-no-swizzle-no-tonemap";
    meta["bytes"] = bytes;
    meta["tick"] = scheduler.CurrentTick();
    auto& writer = DiskWriter();
    const auto old_state = native.state;
    const auto old_subresources = native.subresource_states;
    meta["layout_before"] = static_cast<uint32_t>(old_state.layout);
    struct Pending { std::shared_ptr<Buffer> staging; Output output; bool recorded = false; };
    auto pending = std::make_shared<Pending>(Pending {std::move(staging), std::move(output), false});
    ++FrameCapture::g_pixel_pending;
    try {
        scheduler.DeferOperation([pending, &writer]() mutable {
            try {
                if (pending->recorded) {
                    pending->staging->Invalidate(0, pending->output.bytes.size());
                    std::memcpy(pending->output.bytes.data(), pending->staging->Mapped().data(), pending->output.bytes.size());
                } else {
                    pending->output.bytes.clear();
                    pending->output.metadata["status"] = "skipped";
                    pending->output.metadata["reason"] = "recording-failed";
                }
                writer.Push(std::move(pending->output));
            } catch (const std::exception& e) {
                ++FrameCapture::g_pixel_failed;
                --FrameCapture::g_pixel_pending;
                std::fprintf(stderr, "[PIXDUMP] completion failed: %s\n", e.what());
            }
        });
    } catch (...) { --FrameCapture::g_pixel_pending; throw; }
    scheduler.EndRendering();
    auto command = scheduler.Current().Handle(); // closes pending compute-chain dependencies
    image->Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                   ImageSubresourceRange {range.base_level, levels, range.base_layer, layers}, command);
    command.copyImageToBuffer(native.image, vk::ImageLayout::eTransferSrcOptimal, pending->staging->Handle(), copies);
    vk::BufferMemoryBarrier2 host {};
    host.srcStageMask = vk::PipelineStageFlagBits2::eCopy;
    host.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    host.dstStageMask = vk::PipelineStageFlagBits2::eHost;
    host.dstAccessMask = vk::AccessFlagBits2::eHostRead;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = pending->staging->Handle(); host.size = bytes;
    vk::DependencyInfo dependency {};
    dependency.bufferMemoryBarrierCount = 1; dependency.pBufferMemoryBarriers = &host;
    command.pipelineBarrier2(dependency);
    image->RestoreState(old_state, old_subresources, command);
    FrameCapture::g_pixel_bytes += bytes;
    pending->recorded = true;
}

// A diagnostic failure must not abort the emulator. Vulkan/device failures remain
// the renderer's responsibility; ordinary host allocation/I/O errors are reported.
inline void Capture(RenderContext& context, ImageId id, vk::ImageView view,
                    const char* role, uint32_t stage, uint32_t slot, const char* phase = "after-call") {
    try { Copy(context, id, view, role, stage, slot, phase); }
    catch (const std::exception& e) {
        ++FrameCapture::g_pixel_failed;
        std::fprintf(stderr, "[PIXDUMP] capture failed: %s\n", e.what());
    }
}
} // namespace Libs::Graphics::PixelCapture
