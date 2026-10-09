#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shadow map rendering: positions in the light space of one cascade.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) flat out uint v_InstanceIndex;

layout(push_constant) uniform ShadowParameters
{
	uint Cascade;
} u_Shadow;

void main()
{
	InstanceData instance = u_Instances[gl_InstanceIndex];
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = gl_InstanceIndex;
	vec4 position = u_Frame.CascadeViewProjection[u_Shadow.Cascade] * (instance.World * vec4(a_Position, 1.0));
	// Pancaking: casters between the light and the cascade's near plane are flattened onto it (reversed-Z: the near
	// plane is depth 1) instead of being clipped, so they still shadow the cascade. The projection is orthographic.
	position.z = min(position.z, position.w);
	gl_Position = position;
}
