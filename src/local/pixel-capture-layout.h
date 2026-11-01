#pragma once
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace PixelCaptureLayout {
inline constexpr uint64_t ImageLimit = 128ull << 20;
inline constexpr uint64_t FrameLimit = 512ull << 20;
struct Plane {
    uint32_t mip, layer, width, height;
    uint64_t offset, row_bytes, size;
};
// Tightly packed rows, planes aligned for vkCmdCopyImageToBuffer. Native mip
// dimensions, not the guest descriptor's extent. No compression or 3D slices.
inline bool Plan(uint32_t width, uint32_t height, uint32_t mips, uint32_t layers,
                 uint32_t first_mip, uint32_t mip_count, uint32_t first_layer,
                 uint32_t layer_count, uint32_t texel_bytes,
                 std::vector<Plane>& planes, uint64_t& bytes) {
    planes.clear(); bytes = 0;
    if (!width || !height || !mips || mips > 32 || !layers || !texel_bytes || texel_bytes > 32 ||
        first_mip >= mips || !mip_count || mip_count > mips - first_mip ||
        first_layer >= layers || !layer_count || layer_count > layers - first_layer ||
        uint64_t(mip_count) * layer_count > 4096) return false;
    const auto alignment = std::lcm(uint64_t(texel_bytes), uint64_t(4));
    for (uint32_t mip = first_mip; mip < first_mip + mip_count; ++mip) {
        const auto w = std::max(width >> mip, 1u), h = std::max(height >> mip, 1u);
        const uint64_t row = uint64_t(w) * texel_bytes;
        if (row > ImageLimit || h > ImageLimit / row) { planes.clear(); bytes = 0; return false; }
        const auto size = row * h;
        for (uint32_t layer = first_layer; layer < first_layer + layer_count; ++layer) {
            const auto offset = ((bytes + alignment - 1) / alignment) * alignment;
            if (offset > ImageLimit - size) { planes.clear(); bytes = 0; return false; }
            planes.push_back({mip, layer, w, h, offset, row, size});
            bytes = offset + size;
        }
    }
    return true;
}
} // namespace PixelCaptureLayout
