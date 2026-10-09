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
	InstanceData instance = u_Instances[gl_InstanceIndex];
	vec4 worldPosition = instance.World * vec4(a_Position, 1.0);
	v_WorldPosition = worldPosition.xyz;
	v_Normal = normalize(mat3(instance.NormalMatrix) * a_Normal);
	// A mirroring transform reverses the handedness of the tangent frame: cross(N, T) then points opposite to the
	// transformed bitangent, so its sign flips.
	float handedness = (a_Tangent.w < 0.0 ? -1.0 : 1.0) * ((instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -1.0 : 1.0);
	v_Tangent = vec4(normalize(mat3(instance.World) * a_Tangent.xyz), handedness);
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = gl_InstanceIndex;
	gl_Position = u_Frame.ViewProjection * worldPosition;
}
