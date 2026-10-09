#version 450

// Diagnostic shader used by GPU tests: one triangle covering the upper half of the viewport, colored by
// vertex, with no vertex buffer. Verifies pipeline creation, NDC orientation and depth conventions.

layout(location = 0) out vec3 v_Color;

const vec2 c_Positions[3] = vec2[](vec2(-1.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0));
const vec3 c_Colors[3] = vec3[](vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0), vec3(1.0, 0.0, 0.0));

void main()
{
	v_Color = c_Colors[gl_VertexIndex];
	gl_Position = vec4(c_Positions[gl_VertexIndex], 0.5, 1.0);
}
