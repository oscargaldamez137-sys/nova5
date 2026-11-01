#pragma once

#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace RasterDepthClamp {
// Match Jetsku 174f8d2f: clamp raster depth independently of Z clipping.
// The latter remains controlled by VK_EXT_depth_clip_enable and guest state.
inline bool Requested() {
    static const bool enabled = [] {
        const char* value = std::getenv("KYTY_RASTER_DEPTH_CLAMP");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

constexpr bool Enable(bool requested, bool supported) { return requested && supported; }

// The old blob identity did not include depthClampEnable. Preserve legacy keys
// with the switch off, and separate corrected pipelines from those old blobs.
inline void AppendPipelineKey(std::vector<uint32_t>& words, bool enabled) {
    if (enabled) words.push_back(0x44434c31u); // DCL1
}
} // namespace RasterDepthClamp
