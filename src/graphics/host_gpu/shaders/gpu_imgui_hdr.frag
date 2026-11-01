#version 450

// The system overlay's ImGui fragment shader on an HDR10 swapchain: its sRGB colors as BT.2100 PQ
// (Dear ImGui's own shader, the color converted). The 250-nit reference white matches upstream.

layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 1, binding = 0) uniform sampler image_sampler;
layout(location = 0) in struct {
	vec4 Color;
	vec2 UV;
} In;

vec3 SdrToHdr10(vec3 rgb) {
	rgb         = clamp(rgb, 0.0, 1.0);
	vec3 linear = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)), greaterThan(rgb, vec3(0.04045)));
	// BT.709 to BT.2020 primaries, scaled so 1.0 is 250 nits of the PQ curve's 10,000.
	const mat3 to_rec2020 = (250.0 / 10000.0) * mat3(0.627404, 0.069097, 0.016391,
	                                                 0.329283, 0.919540, 0.088013,
	                                                 0.043313, 0.011362, 0.895595);
	vec3 powered = pow(to_rec2020 * linear, vec3(2610.0 / 16384.0));
	return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * powered) / (1.0 + (2392.0 / 128.0) * powered),
	           vec3(2523.0 / 32.0));
}

void main() {
	vec4 sampled = In.Color * texture(sampler2D(image, image_sampler), In.UV);
	color        = vec4(SdrToHdr10(sampled.rgb), sampled.a);
}
