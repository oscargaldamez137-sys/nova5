#include "local/raster-depth-clamp.h"
#include "local/raster-debug.h"
#include "local/raster-debug-color.h"
#include "local/raster-debug-fullscreen.h"
#include <spirv-tools/libspirv.hpp>
#include <algorithm>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

int main() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_1);
    validator.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t& pos, const char* text) {
        std::fprintf(stderr, "magenta SPIR-V word %zu: %s\n", pos.index, text);
    });
    if (!validator.Validate(RasterDebug::MagentaSpirv, std::size(RasterDebug::MagentaSpirv))) return 12;
    std::string assembly;
    if (!validator.Disassemble(RasterDebug::MagentaSpirv, std::size(RasterDebug::MagentaSpirv), &assembly)) return 13;
    if (assembly.find("OpEntryPoint Fragment") == std::string::npos ||
        assembly.find("Location 0") == std::string::npos ||
        assembly.find("%float_1 %float_0 %float_1 %float_1") == std::string::npos ||
        assembly.find("OpStore %color") == std::string::npos ||
        assembly.find("OpKill") != std::string::npos || assembly.find("DescriptorSet") != std::string::npos ||
        assembly.find(" Input") != std::string::npos) return 14;
    constexpr uint64_t sky = 0xdb42662a2582dacbull;
    if (!validator.Validate(RasterDebug::FullscreenSpirv, std::size(RasterDebug::FullscreenSpirv))) return 15;
    assembly.clear();
    if (!validator.Disassemble(RasterDebug::FullscreenSpirv, std::size(RasterDebug::FullscreenSpirv), &assembly)) return 16;
    if (assembly.find("OpEntryPoint Vertex") == std::string::npos ||
        assembly.find("BuiltIn VertexIndex") == std::string::npos ||
        assembly.find("BuiltIn Position") == std::string::npos ||
        assembly.find("DescriptorSet") != std::string::npos) return 17;
    constexpr RasterDebug::Options fullscreen {0, 0, 0, 0, 0, sky};
    static_assert(RasterDebug::Select(fullscreen, sky).vs_fullscreen);
    static_assert(!RasterDebug::Select(fullscreen, sky + 1).vs_fullscreen);
    static_assert(!RasterDebug::Select(fullscreen, sky).ps_color);
    std::vector<uint32_t> vs_off {1,2}, vs_on = vs_off;
    RasterDebug::AppendVertexPipelineKey(vs_off, false);
    RasterDebug::AppendVertexPipelineKey(vs_on, true);
    if (vs_off.size() != 2 || vs_on == vs_off) return 18;
    static_assert(RasterDebug::ParseHash("db42662a2582dacb") == sky);
    static_assert(RasterDebug::ParseHash("0xDB42662A2582DACB") == sky);
    static_assert(RasterDebug::ParseHash("*") == 0);
    static_assert(RasterDebug::ParseHash("0x") == 0);
    static_assert(RasterDebug::ParseHash("db42662a2582dacbz") == 0);
    static_assert(RasterDebug::ParseHash("1db42662a2582dacb0") == 0);
    static_assert(!RasterDebug::Select({}, sky).trace);
    static_assert(!RasterDebug::Select({}, 0).trace);
    constexpr RasterDebug::Options options {0, sky, sky, sky};
    constexpr auto selected = RasterDebug::Select(options, sky);
    static_assert(selected.trace && selected.no_depth && selected.no_cull && selected.no_z_clip);
    static_assert(!RasterDebug::Select(options, sky + 1).trace);
    constexpr RasterDebug::Options magenta {0, 0, 0, 0, sky};
    static_assert(RasterDebug::Select(magenta, sky).ps_color);
    static_assert(!RasterDebug::Select(magenta, sky + 1).ps_color);
    static_assert(!RasterDebug::Select(magenta, sky).no_depth);
    struct Color { bool blend_enable[2] = {true, true}; uint32_t color_mask[2] = {7, 15}; } color;
    RasterDebug::ApplyColor(color, RasterDebug::Select(magenta, sky + 1));
    if (!color.blend_enable[0]) return 9;
    RasterDebug::ApplyColor(color, RasterDebug::Select(magenta, sky));
    if (color.blend_enable[0] || !color.blend_enable[1] || color.color_mask[0] != 7) return 10;
    std::vector<uint32_t> guest_key {1, 2, 3}, debug_key = guest_key;
    RasterDebug::AppendColorPipelineKey(guest_key, false);
    RasterDebug::AppendColorPipelineKey(debug_key, true);
    if (guest_key.size() != 3 || debug_key == guest_key) return 11;
    struct Depth { bool depth_test_enable = true, depth_write_enable = true; } depth;
    {
        RasterDebug::ScopedDepth override(depth, true);
        if (depth.depth_test_enable || depth.depth_write_enable) return 2;
    }
    if (!depth.depth_test_enable || !depth.depth_write_enable) return 3;
    {
        RasterDebug::ScopedDepth override(depth, false);
        if (!depth.depth_test_enable || !depth.depth_write_enable) return 4;
    }
    struct Static { bool cull_front = true, cull_back = true, depth_clip_enable = true; } params;
    RasterDebug::ApplyStatic(params, RasterDebug::Select(options, sky + 1));
    if (!params.cull_front || !params.cull_back || !params.depth_clip_enable) return 5;
    RasterDebug::ApplyStatic(params, selected);
    if (params.cull_front || params.cull_back || params.depth_clip_enable) return 6;
    // Each switch is independent: a diagnostic must not accidentally change another test.
    params = {};
    RasterDebug::ApplyStatic(params, {true, false, true, false});
    if (params.cull_front || params.cull_back || !params.depth_clip_enable) return 7;
    params = {};
    RasterDebug::ApplyStatic(params, {true, false, false, true});
    if (!params.cull_front || !params.cull_back || params.depth_clip_enable) return 8;
    static_assert(RasterDepthClamp::Enable(true, true));
    static_assert(!RasterDepthClamp::Enable(true, false));
    static_assert(!RasterDepthClamp::Enable(false, true));
    static_assert(!RasterDepthClamp::Enable(false, false));
    const std::vector<uint32_t> legacy {0, 0, UINT32_MAX, 7, 3, 0};
    auto off = legacy, on = legacy;
    RasterDepthClamp::AppendPipelineKey(off, false);
    RasterDepthClamp::AppendPipelineKey(on, true);
    if (off != legacy || on == legacy || on.size() != legacy.size() + 1 ||
        !std::equal(legacy.begin(), legacy.end(), on.begin())) {
        std::fprintf(stderr, "raster depth clamp: pipeline identities not isolated\n");
        return 1;
    }
    std::puts("raster depth clamp: feature gating and legacy/new cache separation passed (host only)");
    std::puts("raster debug: strict hash selection, independent overrides and state restoration passed");
    std::puts("raster debug: magenta SPIR-V validates for Vulkan 1.1; location 0, no inputs/resources/kill");
}
