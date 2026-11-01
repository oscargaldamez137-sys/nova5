#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

// Per-pixel-shader diagnostics. No wildcard and no guest state persists after a draw.
namespace RasterDebug {
constexpr uint64_t ParseHash(std::string_view text) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.empty() || text.size() > 16) return 0;
    uint64_t value = 0;
    for (const char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        value = (value << 4) | digit;
    }
    return value;
}
inline uint64_t ReadHash(const char* name) {
    const char* value = std::getenv(name);
    return value ? ParseHash(value) : 0;
}
struct Options {
    uint64_t trace {}, no_depth {}, no_cull {}, no_z_clip {}, ps_color {}, vs_fullscreen {};
};
struct Selection {
    bool trace {}, no_depth {}, no_cull {}, no_z_clip {}, ps_color {}, vs_fullscreen {};
};
constexpr Selection Select(const Options& options, uint64_t hash) {
    if (!hash) return {};
    const bool depth = options.no_depth == hash;
    const bool cull = options.no_cull == hash;
    const bool clip = options.no_z_clip == hash;
    const bool color = options.ps_color == hash;
    const bool vertex = options.vs_fullscreen == hash;
    return {options.trace == hash || depth || cull || clip || color || vertex, depth, cull, clip, color, vertex};
}
inline Selection For(uint64_t hash) {
    static const Options options {ReadHash("KYTY_DEBUG_RASTER"),
        ReadHash("KYTY_DEBUG_FORCE_NO_DEPTH"), ReadHash("KYTY_DEBUG_FORCE_CULL_NONE"),
        ReadHash("KYTY_DEBUG_FORCE_NO_Z_CLIP"), ReadHash("KYTY_DEBUG_FORCE_PS_COLOR"),
        ReadHash("KYTY_DEBUG_FORCE_VS_FULLSCREEN")};
    return Select(options, hash);
}
template <typename Depth> class ScopedDepth {
    Depth& depth;
    bool test, write, changed;
public:
    ScopedDepth(Depth& value, bool disable) : depth(value), test(value.depth_test_enable),
                                            write(value.depth_write_enable), changed(disable) {
        if (disable) { depth.depth_test_enable = false; depth.depth_write_enable = false; }
    }
    ~ScopedDepth() {
        if (changed) { depth.depth_test_enable = test; depth.depth_write_enable = write; }
    }
    ScopedDepth(const ScopedDepth&) = delete;
    ScopedDepth& operator=(const ScopedDepth&) = delete;
};
template <typename StaticParams> void ApplyStatic(StaticParams& state, Selection selection) {
    if (selection.no_cull) { state.cull_front = false; state.cull_back = false; }
    if (selection.no_z_clip) state.depth_clip_enable = false;
}
template <typename StaticParams> void ApplyColor(StaticParams& state, Selection selection) {
    // Keep guest write masks: the experiment also tests the color-output path.
    if (selection.ps_color) state.blend_enable[0] = false;
}
inline void AppendColorPipelineKey(std::vector<uint32_t>& words, bool enabled) {
    if (enabled) words.push_back(0x4d414731u); // MAG1: replacement SPIR-V version, isolated from guest blobs.
}
inline void AppendVertexPipelineKey(std::vector<uint32_t>& words, bool enabled) {
    if (enabled) words.push_back(0x46535631u); // FSV1: fixed fullscreen vertex shader.
}
} // namespace RasterDebug
