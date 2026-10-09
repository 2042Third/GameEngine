#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Debug lines (DebugDraw) in world space with display colors.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color; // RGBA8 UNORM, sRGB-encoded

layout(location = 0) out vec4 v_Color;

// Lines drawn on a surface (e.g. a collider around its mesh) are pulled toward the camera by this fraction of their
// depth so they do not flicker against it (reversed-Z: larger is nearer).
const float c_DepthBias = 1.0e-4;

void main()
{
	v_Color = a_Color;
	gl_Position = u_Frame.ViewProjection * vec4(a_Position, 1.0);
	gl_Position.z = min(gl_Position.z * (1.0 + c_DepthBias), gl_Position.w);
}
