#pragma once

#include "native-preparation-scratch.h"
#include "graphics/shader/shader.h"

namespace Libs::Graphics {

inline void ResetNativeVertexInput(ShaderVertexInputInfo& info) {
    if (!NativePreparationScratchEnabled()) {
        info = {};
        return;
    }
    // Only [0, resources_num) / [0, buffers_num) may be consumed. Attribute
    // decoding and buffer grouping overwrite every member of every active slot.
    // Preserve the previous owned snapshot as spare capacity for the next
    // transaction; a null program makes it unusable until materialization succeeds.
    info.stage.program = nullptr;
    info.resources_num = info.fetch_attrib_reg = info.fetch_buffer_reg = info.buffers_num = 0;
    info.scratch_size_dwords = info.pa_cl_vs_out_cntl = 0;
    info.clip_space = {};
    info.mesh = {};
    info.fetch_external = info.fetch_embedded = false;
}

template <typename T>
inline void ResetNativeStageInput(T& info) {
    if (!NativePreparationScratchEnabled()) {
        info = {};
        return;
    }
    auto storage = std::move(info.stage.resources);
    info = {};
    info.stage.resources = std::move(storage);
}

} // namespace Libs::Graphics
