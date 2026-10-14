#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shadow map rendering: positions in the light space of one cascade. With ST_SKINNED (registered as
// Scene/ShadowMeshSkinned.vert) skinned meshes cast the shadow of their pose.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
#ifdef ST_SKINNED
layout(location = 4) in uvec4 a_Joints;
layout(location = 5) in vec4 a_Weights;
#endif

layout(location = 0) out vec2 v_TexCoord;
layout(location = 1) flat out uint v_InstanceIndex;

layout(push_constant) uniform ShadowParameters
{
	uint Cascade;
} u_Shadow;

void main()
{
	uint instanceIndex = u_DrawList[gl_InstanceIndex];
	InstanceData instance = u_Instances[instanceIndex];
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
#ifdef ST_SKINNED
	vec3 localPosition = SkinPosition(BlendJoints(instance, a_Joints, a_Weights), a_Position);
#else
	vec3 localPosition = a_Position;
#endif
	vec4 position = u_Cascades[u_Shadow.Cascade].ViewProjection * vec4(TransformPositionRelative(instance, localPosition), 1.0);
	// Pancaking: casters between the light and the cascade's near plane are flattened onto it (reversed-Z: the near
	// plane is depth 1) instead of being clipped, so they still shadow the cascade. The projection is orthographic.
	position.z = min(position.z, position.w);
	gl_Position = position;
}
