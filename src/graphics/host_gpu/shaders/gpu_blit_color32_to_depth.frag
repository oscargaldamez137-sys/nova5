#version 450

// Bit-exact 32-bit color -> D32 depth reinterpretation (texture cache alias copies). The source
// is read through an R32_UINT storage view of any 32-bit color image; the raw bits become the
// fragment depth, exactly as a copy through a buffer would store them.

layout(set = 0, binding = 0, r32ui) uniform readonly uimage2DArray color_source;

layout(push_constant, std430) uniform Push {
	uint layer;
} params;

void main() {
	const ivec3 coord = ivec3(ivec2(gl_FragCoord.xy), int(params.layer));
	gl_FragDepth      = uintBitsToFloat(imageLoad(color_source, coord).r);
}
