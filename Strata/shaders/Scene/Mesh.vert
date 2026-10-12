#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shared by the depth prepass and the forward pass: both must produce bit-identical positions, because the
// forward pass draws opaque geometry with an EQUAL depth test. With ST_SKINNED (registered as Scene/MeshSkinned.vert)
// it poses skinned meshes: the bind-pose vertex is blended by its joints' palette entries first (both passes of a
// skinned mesh use that variant, so they agree as well).

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
#ifdef ST_SKINNED
layout(location = 4) in uvec4 a_Joints;
layout(location = 5) in vec4 a_Weights;
#endif

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
#ifdef ST_SKINNED
	JointMatrix skin = BlendJoints(instance, a_Joints, a_Weights);
	vec3 position = SkinPosition(skin, a_Position);
	vec3 normal = SkinNormal(skin, a_Normal);
	vec3 tangent = SkinDirection(skin, a_Tangent.xyz);
#else
	vec3 position = a_Position;
	vec3 normal = a_Normal;
	vec3 tangent = a_Tangent.xyz;
#endif
	vec3 worldPosition = TransformPosition(instance, position);
	v_WorldPosition = worldPosition;
	v_Normal = normalize(TransformNormal(instance, normal));
	// A mirroring transform reverses the handedness of the tangent frame: cross(N, T) then points opposite to the
	// transformed bitangent, so its sign flips.
	float handedness = (a_Tangent.w < 0.0 ? -1.0 : 1.0) * ((instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -1.0 : 1.0);
	v_Tangent = vec4(normalize(TransformDirection(instance, tangent)), handedness);
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
	gl_Position = u_Frame.ViewProjection * vec4(worldPosition, 1.0);
}
