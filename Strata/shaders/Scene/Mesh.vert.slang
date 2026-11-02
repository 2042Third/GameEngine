#include "Include/Common.slang"

#include "Include/Scene.slang"

// Shared by the depth prepass and the forward pass: both must produce bit-identical positions, because the
// forward pass draws opaque geometry with an EQUAL depth test. With ST_SKINNED (registered as Scene/MeshSkinned.vert)
// it poses skinned meshes: the bind-pose vertex is blended by its joints' palette entries first (both passes of a
// skinned mesh use that variant, so they agree as well). With ST_VELOCITY (Scene/MeshVelocity.vert, MeshSkinnedVelocity.vert)
// it also passes the vertex's unjittered clip positions in this frame and in the renderer's previous one, for motion vectors:
// a frame with motion draws both passes with it (the forward pass ignores them), so they still agree.

static float3 a_Position;
static float3 a_Normal;
static float4 a_Tangent;
static float2 a_TexCoord;
#ifdef ST_SKINNED
static uint4 a_Joints;
static float4 a_Weights;
#endif

static float3 v_RelativePosition;
static float3 v_Normal;
static float4 v_Tangent;
static float2 v_TexCoord;
static uint v_InstanceIndex;
static uint v_LODFade; // The draw's cross-fade code (IsLODFadeDrawn)
#ifdef ST_VELOCITY
static float4 v_CurrentClip;
static float4 v_PreviousClip;
#endif

// The position is invariant (the depth prepass's EQUAL test): Slang cannot say it, so the build decorates it
// (INVARIANT_POSITION in Strata/CMakeLists.txt).

void st_Main()
{
	uint entry = u_DrawList[st_InstanceIndex];
	uint instanceIndex = entry & ST_DRAW_ENTRY_INSTANCE_MASK;
	InstanceData instance = u_Instances[instanceIndex];
#ifdef ST_SKINNED
	JointMatrix skin = BlendJoints(instance, a_Joints, a_Weights);
	float3 position = SkinPosition(skin, a_Position);
	float3 normal = SkinNormal(skin, a_Normal);
	float3 tangent = SkinDirection(skin, a_Tangent.xyz);
#else
	float3 position = a_Position;
	float3 normal = a_Normal;
	float3 tangent = a_Tangent.xyz;
#endif
	float3 relativePosition = TransformPositionRelative(instance, position);
	v_RelativePosition = relativePosition;
	v_Normal = normalize(TransformNormal(instance, normal));
	// A mirroring transform reverses the handedness of the tangent frame: cross(N, T) then points opposite to the
	// transformed bitangent, so its sign flips.
	float handedness = (a_Tangent.w < 0.0 ? -1.0 : 1.0) * ((instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -1.0 : 1.0);
	v_Tangent = float4(normalize(TransformDirection(instance, tangent)), handedness);
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
	v_LODFade = entry >> ST_DRAW_ENTRY_INSTANCE_BITS;
	st_Position = mul(u_Frame.ViewProjection, float4(relativePosition, 1.0));
#ifdef ST_VELOCITY
	// The instance as the previous frame drew it: the history's record when it is the same entity's (a slot another entity
	// held, or a new instance, moves with the camera only), posed by its palette as then when that is the same range. What did
	// not change takes this frame's values, so it does not move however the two would round.
	InstanceData previous = u_PreviousInstances[instanceIndex];
	bool sameEntity = u_Frame.TemporalParams.x > 0.5 && previous.EntityID == instance.EntityID;
	float3 previousPosition = position;
#ifdef ST_SKINNED
	if (sameEntity && (previous.Flags & ST_INSTANCE_SKINNED) != 0u && previous.SkinPalette == instance.SkinPalette)
	{
		JointMatrix previousSkin = BlendPreviousJoints(previous, a_Joints, a_Weights);
		if (any(previousSkin.Rows[0] != skin.Rows[0]) || any(previousSkin.Rows[1] != skin.Rows[1]) || any(previousSkin.Rows[2] != skin.Rows[2]))
			previousPosition = SkinPosition(previousSkin, a_Position);
	}
#endif
	float3 previousRelative = relativePosition;
	if (sameEntity && (any(previous.WorldRows[0] != instance.WorldRows[0]) || any(previous.WorldRows[1] != instance.WorldRows[1])
		|| any(previous.WorldRows[2] != instance.WorldRows[2]) || any(previousPosition != position)))
		previousRelative = TransformPositionRelative(previous, previousPosition);
	v_CurrentClip = mul(u_Frame.UnjitteredViewProjection, float4(relativePosition, 1.0));
	v_PreviousClip = mul(u_Frame.PreviousViewProjection, float4(previousRelative, 1.0));
#endif
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 a_Position : ATTRIBUTE0;
	[[vk::location(1)]] float3 a_Normal : ATTRIBUTE1;
	[[vk::location(2)]] float4 a_Tangent : ATTRIBUTE2;
	[[vk::location(3)]] float2 a_TexCoord : ATTRIBUTE3;
#ifdef ST_SKINNED
	[[vk::location(4)]] uint4 a_Joints : ATTRIBUTE4;
	[[vk::location(5)]] float4 a_Weights : ATTRIBUTE5;
#endif
};
struct ST_Output
{
	[[vk::location(0)]] float3 v_RelativePosition : TEXCOORD0;
	[[vk::location(1)]] float3 v_Normal : TEXCOORD1;
	[[vk::location(2)]] float4 v_Tangent : TEXCOORD2;
	[[vk::location(3)]] float2 v_TexCoord : TEXCOORD3;
	[[vk::location(4)]] nointerpolation uint v_InstanceIndex : TEXCOORD4;
	[[vk::location(5)]] nointerpolation uint v_LODFade : TEXCOORD5;
#ifdef ST_VELOCITY
	[[vk::location(6)]] float4 v_CurrentClip : TEXCOORD6;
	[[vk::location(7)]] float4 v_PreviousClip : TEXCOORD7;
#endif
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	a_Position = input.a_Position;
	a_Normal = input.a_Normal;
	a_Tangent = input.a_Tangent;
	a_TexCoord = input.a_TexCoord;
#ifdef ST_SKINNED
	a_Joints = input.a_Joints;
	a_Weights = input.a_Weights;
#endif
	st_Main();
	ST_Output output;
	output.v_RelativePosition = v_RelativePosition;
	output.v_Normal = v_Normal;
	output.v_Tangent = v_Tangent;
	output.v_TexCoord = v_TexCoord;
	output.v_InstanceIndex = v_InstanceIndex;
	output.v_LODFade = v_LODFade;
#ifdef ST_VELOCITY
	output.v_CurrentClip = v_CurrentClip;
	output.v_PreviousClip = v_PreviousClip;
#endif
	output.st_Position = st_Position;
	return output;
}
