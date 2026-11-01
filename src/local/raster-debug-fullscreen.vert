#version 450
// Diagnostic: no vertex buffers, guest launch registers or resources.
layout(location = 0) out vec4 param0;
void main() {
    uint i = uint(gl_VertexIndex) % 3u;
    vec2 p = i == 0u ? vec2(-1,-1) : i == 1u ? vec2(3,-1) : vec2(-1,3);
    gl_Position = vec4(p, 0.5, 1.0);
    param0 = vec4((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5, 0.0, 1.0);
}
