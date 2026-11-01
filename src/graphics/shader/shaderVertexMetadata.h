#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_

#include "graphics/shader/shader.h"

#include <array>
#include <cstring>
#include <string>

namespace Libs::Graphics {

// Probe exact bytes, never a cached value. A rejected probe retains the normal
// guest read/fault path and overwrites any partial output from the probe.
template <size_t Words, typename Probe>
std::array<uint32_t, Words> ShaderReadMetadataWords(const uint32_t* source, bool enabled,
                                                   Probe&& probe, bool& backing) {
    std::array<uint32_t, Words> result;
    backing = enabled && probe(reinterpret_cast<uint64_t>(source), result.data(), sizeof(result));
    if (!backing) std::memcpy(result.data(), source, sizeof(result));
    return result;
}

struct ShaderVertexMetadata {
	int                                                        vertex_buffer_reg = -1;
	int                                                        vertex_attrib_reg = -1;
	std::array<ShaderSemantic, ShaderVertexInputInfo::RES_MAX> input_semantics {};
	uint32_t                                                   input_semantics_count = 0;
};

// Copies the small AGC metadata subset used by the vertex path after validating every guest range.
bool ShaderReadVertexMetadata(const ShaderMappedData& data, uint32_t max_user_sgprs,
                              ShaderVertexMetadata& metadata, std::string* error);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXMETADATA_H_ */
