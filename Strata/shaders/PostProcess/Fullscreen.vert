#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"

// Fullscreen triangle without vertex buffers: draw 3 vertices.
layout(location = 0) out vec2 v_UV;

void main()
{
	vec2 ndc = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2)) * 2.0 - 1.0;
	v_UV = NDCToUV(ndc);
	gl_Position = vec4(ndc, 0.0, 1.0);
}
