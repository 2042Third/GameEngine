#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shared by the depth prepass and the forward pass: both must produce bit-identical positions, because the
// forward pass draws opaque geometry with an EQUAL depth test.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;

layout(location = 0) out vec3 v_WorldPosition;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec4 v_Tangent;
layout(location = 3) out vec2 v_TexCoord;
layout(location = 4) flat out uint v_InstanceIndex;

invariant gl_Position;

void main()
{
	uint instanceIndex = u_DrawList[gl_InstanceIndex];
	InstanceData instance = u_Instances[instanceIndex];
	vec3 worldPosition = TransformPosition(instance, a_Position);
	v_WorldPosition = worldPosition;
	v_Normal = normalize(TransformNormal(instance, a_Normal));
	// A mirroring transform reverses the handedness of the tangent frame: cross(N, T) then points opposite to the
	// transformed bitangent, so its sign flips.
	float handedness = (a_Tangent.w < 0.0 ? -1.0 : 1.0) * ((instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -1.0 : 1.0);
	v_Tangent = vec4(normalize(TransformDirection(instance, a_Tangent.xyz)), handedness);
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
	gl_Position = u_Frame.ViewProjection * vec4(worldPosition, 1.0);
}
