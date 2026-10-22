#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shared by the depth prepass and the forward pass: both must produce bit-identical positions, because the
// forward pass draws opaque geometry with an EQUAL depth test. With ST_SKINNED (registered as Scene/MeshSkinned.vert)
// it poses skinned meshes: the bind-pose vertex is blended by its joints' palette entries first (both passes of a
// skinned mesh use that variant, so they agree as well). With ST_VELOCITY (Scene/MeshVelocity.vert, MeshSkinnedVelocity.vert)
// it also passes the vertex's unjittered clip positions in this frame and in the renderer's previous one, for motion vectors:
// a frame with motion draws both passes with it (the forward pass ignores them), so they still agree.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec4 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
#ifdef ST_SKINNED
layout(location = 4) in uvec4 a_Joints;
layout(location = 5) in vec4 a_Weights;
#endif

layout(location = 0) out vec3 v_RelativePosition;
layout(location = 1) out vec3 v_Normal;
layout(location = 2) out vec4 v_Tangent;
layout(location = 3) out vec2 v_TexCoord;
layout(location = 4) flat out uint v_InstanceIndex;
layout(location = 5) flat out uint v_LODFade; // The draw's cross-fade code (IsLODFadeDrawn)
#ifdef ST_VELOCITY
layout(location = 6) out vec4 v_CurrentClip;
layout(location = 7) out vec4 v_PreviousClip;
#endif

invariant gl_Position;

void main()
{
	uint entry = u_DrawList[gl_InstanceIndex];
	uint instanceIndex = entry & ST_DRAW_ENTRY_INSTANCE_MASK;
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
	vec3 relativePosition = TransformPositionRelative(instance, position);
	v_RelativePosition = relativePosition;
	v_Normal = normalize(TransformNormal(instance, normal));
	// A mirroring transform reverses the handedness of the tangent frame: cross(N, T) then points opposite to the
	// transformed bitangent, so its sign flips.
	float handedness = (a_Tangent.w < 0.0 ? -1.0 : 1.0) * ((instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -1.0 : 1.0);
	v_Tangent = vec4(normalize(TransformDirection(instance, tangent)), handedness);
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
	v_LODFade = entry >> ST_DRAW_ENTRY_INSTANCE_BITS;
	gl_Position = u_Frame.ViewProjection * vec4(relativePosition, 1.0);
#ifdef ST_VELOCITY
	// The instance as the previous frame drew it: the history's record when it is the same entity's (a slot another entity
	// held, or a new instance, moves with the camera only), posed by its palette as then when that is the same range. What did
	// not change takes this frame's values, so it does not move however the two would round.
	InstanceData previous = u_PreviousInstances[instanceIndex];
	bool sameEntity = u_Frame.TemporalParams.x > 0.5 && previous.EntityID == instance.EntityID;
	vec3 previousPosition = position;
#ifdef ST_SKINNED
	if (sameEntity && (previous.Flags & ST_INSTANCE_SKINNED) != 0u && previous.SkinPalette == instance.SkinPalette)
	{
		JointMatrix previousSkin = BlendPreviousJoints(previous, a_Joints, a_Weights);
		if (previousSkin.Rows[0] != skin.Rows[0] || previousSkin.Rows[1] != skin.Rows[1] || previousSkin.Rows[2] != skin.Rows[2])
			previousPosition = SkinPosition(previousSkin, a_Position);
	}
#endif
	vec3 previousRelative = relativePosition;
	if (sameEntity && (previous.WorldRows[0] != instance.WorldRows[0] || previous.WorldRows[1] != instance.WorldRows[1]
		|| previous.WorldRows[2] != instance.WorldRows[2] || previousPosition != position))
		previousRelative = TransformPositionRelative(previous, previousPosition);
	v_CurrentClip = u_Frame.UnjitteredViewProjection * vec4(relativePosition, 1.0);
	v_PreviousClip = u_Frame.PreviousViewProjection * vec4(previousRelative, 1.0);
#endif
}
