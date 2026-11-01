#version 450
// Diagnostic replacement: no guest inputs, resources, EXEC, discard or depth export.
layout(location = 0) out vec4 color;
void main() { color = vec4(1.0, 0.0, 1.0, 1.0); }
