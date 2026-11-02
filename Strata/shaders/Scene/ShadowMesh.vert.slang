#include "Include/Common.slang"

#include "Include/Scene.slang"
#ifdef ST_LOCAL_SHADOW
#include "Include/LocalShadows.slang"
#endif

// Shadow map rendering: positions in the light space of one cascade. With ST_SKINNED (registered as
// Scene/ShadowMeshSkinned.vert) skinned meshes cast the shadow of their pose. With ST_LOCAL_SHADOW (Scene/LocalShadowMesh.vert,
// Scene/LocalShadowMeshSkinned.vert) the view is a point or spot light's shadow view instead: a perspective view whose near
// plane clips (casters that close to a light are inside it).

// Shadow passes read position, UV and the skin only: their own input layout (SceneRenderer), so that every attribute the
// layout gives is read.
static float3 a_Position;
static float2 a_TexCoord;
#ifdef ST_SKINNED
static uint4 a_Joints;
static float4 a_Weights;
#endif

static float2 v_TexCoord;
static uint v_InstanceIndex;

struct ShadowParameters
{
	uint Cascade; // ST_LOCAL_SHADOW: the shadow view, its light's slot times ST_LOCAL_SHADOW_FACES plus its face
};
[[vk::push_constant]] ConstantBuffer<ShadowParameters> u_Shadow;

void st_Main()
{
	uint instanceIndex = u_DrawList[st_InstanceIndex] & ST_DRAW_ENTRY_INSTANCE_MASK; // Shadow draws never fade
	InstanceData instance = u_Instances[instanceIndex];
	v_TexCoord = a_TexCoord;
	v_InstanceIndex = instanceIndex;
#ifdef ST_SKINNED
	float3 localPosition = SkinPosition(BlendJoints(instance, a_Joints, a_Weights), a_Position);
#else
	float3 localPosition = a_Position;
#endif
#ifdef ST_LOCAL_SHADOW
	uint view = u_Shadow.Cascade;
	st_Position = mul(u_LocalShadows[view / ST_LOCAL_SHADOW_FACES].ViewProjection[view % ST_LOCAL_SHADOW_FACES],
		float4(TransformPositionRelative(instance, localPosition), 1.0));
#else
	float4 position = mul(u_Cascades[u_Shadow.Cascade].ViewProjection, float4(TransformPositionRelative(instance, localPosition), 1.0));
	// Pancaking: casters between the light and the cascade's near plane are flattened onto it (reversed-Z: the near
	// plane is depth 1) instead of being clipped, so they still shadow the cascade. The projection is orthographic.
	position.z = min(position.z, position.w);
	st_Position = position;
#endif
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 a_Position : ATTRIBUTE0;
	[[vk::location(1)]] float2 a_TexCoord : ATTRIBUTE1;
#ifdef ST_SKINNED
	[[vk::location(2)]] uint4 a_Joints : ATTRIBUTE2;
	[[vk::location(3)]] float4 a_Weights : ATTRIBUTE3;
#endif
};
struct ST_Output
{
	[[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
	[[vk::location(1)]] nointerpolation uint v_InstanceIndex : TEXCOORD1;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	a_Position = input.a_Position;
	a_TexCoord = input.a_TexCoord;
#ifdef ST_SKINNED
	a_Joints = input.a_Joints;
	a_Weights = input.a_Weights;
#endif
	st_Main();
	ST_Output output;
	output.v_TexCoord = v_TexCoord;
	output.v_InstanceIndex = v_InstanceIndex;
	output.st_Position = st_Position;
	return output;
}
