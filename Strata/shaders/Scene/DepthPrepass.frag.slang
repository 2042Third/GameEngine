#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/MaterialTextures.slang"
#include "Include/PBR.slang"

// Depth prepass: writes depth, view-space normals (screen-space effects) and entity ids (picking). Alpha-masked
// materials are tested here; the forward pass then shades only the visible surface. With ST_NO_ENTITY_IDS (registered as
// Scene/DepthPrepassNoEntityIDs.frag) it writes no entity ids, for renderers without an entity-ID buffer. With ST_VELOCITY
// (Scene/DepthPrepassVelocity.frag, DepthPrepassNoEntityIDsVelocity.frag) it also writes the motion since the renderer's
// previous frame (Scene/MeshVelocity.vert), in UV units, into the attachment after the others. With ST_SELECTION_MASK
// (Scene/SelectionMask.frag) it writes only 1 into a mask where a surface is drawn, with the same tests: the selection mask
// SceneRenderer draws the selected instances into, with the camera's own matrices, for their outline under temporal
// anti-aliasing. With ST_PREPASS_SURFACE (Scene/DepthPrepassSurface.frag, DepthPrepassNoEntityIDsSurface.frag; always with
// ST_VELOCITY) it also writes the surface target screen-space reflections trace from, after the velocity: the normal-mapped
// view-space normal (octahedral), the roughness and the metallic factor, as the forward pass shades them (unlit surfaces:
// roughness 1, nothing to reflect).

static float3 v_RelativePosition;
static float3 v_Normal;
static float4 v_Tangent;
static float2 v_TexCoord;
static uint v_InstanceIndex;
static uint v_LODFade;
#ifdef ST_VELOCITY
static float4 v_CurrentClip;
static float4 v_PreviousClip;
#endif

#ifdef ST_SELECTION_MASK
static float o_Selected;
#else
static float2 o_Normal;
#ifndef ST_NO_ENTITY_IDS
static uint o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
static float2 o_Velocity;
#elif defined(ST_VELOCITY)
static float2 o_Velocity;
#endif
#if defined(ST_PREPASS_SURFACE) && defined(ST_NO_ENTITY_IDS)
static float4 o_Surface;
#elif defined(ST_PREPASS_SURFACE)
static float4 o_Surface;
#endif
#endif

void st_Main()
{
	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	// Sampled before the discards: a quad whose other pixels discarded has no implicit derivatives (texture levels of
	// detail) left (Architecture.Shaders).
	const bool masked = (material.Flags & ST_MATERIAL_ALPHA_MASK) != 0u;
	float alpha = 1.0;
	if (masked)
		alpha = material.BaseColor.a * SampleMaterialTexture(material.BaseColorMap, TransformUV(material, v_TexCoord)).a;
#ifdef ST_PREPASS_SURFACE
	float4 surface = float4(0.0, 0.0, 1.0, 0.0);
	if ((material.Flags & ST_MATERIAL_UNLIT) == 0u)
	{
		float2 uv = TransformUV(material, v_TexCoord);
		float4 metallicRoughness = SampleMaterialTexture(material.MetallicRoughnessMap, uv);
		float3 shadingNormal = GetShadingNormal(material, uv, v_Normal, v_Tangent, st_FrontFacing);
		// The roughness the forward pass shades (its specular anti-aliasing, the same derivatives of the same normal), so
		// screen-space reflections trace the lobe the surface shows.
		float roughness = WidenRoughness(clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0), ddx(shadingNormal), ddy(shadingNormal),
			u_Frame.MaterialParams.z);
		surface = float4(OctahedralEncode(normalize(mul(((float3x3)(u_Frame.View)), shadingNormal))), roughness, clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0));
	}
#endif
	if (!IsLODFadeDrawn(v_LODFade, st_FragCoord.xy))
		discard;
	if (masked && alpha < material.AlphaCutoff)
		discard;

#ifdef ST_SELECTION_MASK
	o_Selected = 1.0;
#else
	float3 normal = normalize(v_Normal);
	if (!st_FrontFacing)
		normal = -normal;
	o_Normal = OctahedralEncode(normalize(mul(((float3x3)(u_Frame.View)), normal)));
#ifndef ST_NO_ENTITY_IDS
	o_EntityID = instance.EntityID;
#endif
#ifdef ST_VELOCITY
	o_Velocity = GetClipMotion(v_CurrentClip, v_PreviousClip);
#endif
#ifdef ST_PREPASS_SURFACE
	o_Surface = surface;
#endif
#endif
}

// --- Entry point ---
struct ST_Input
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
};
struct ST_Output
{
#ifdef ST_SELECTION_MASK
	[[vk::location(0)]] float o_Selected : SV_Target0;
#else
	[[vk::location(0)]] float2 o_Normal : SV_Target0;
#ifndef ST_NO_ENTITY_IDS
	[[vk::location(1)]] uint o_EntityID : SV_Target1;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
	[[vk::location(1)]] float2 o_Velocity : SV_Target1;
#elif defined(ST_VELOCITY)
	[[vk::location(2)]] float2 o_Velocity : SV_Target2;
#endif
#if defined(ST_PREPASS_SURFACE) && defined(ST_NO_ENTITY_IDS)
	[[vk::location(2)]] float4 o_Surface : SV_Target2;
#elif defined(ST_PREPASS_SURFACE)
	[[vk::location(3)]] float4 o_Surface : SV_Target3;
#endif
#endif
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_RelativePosition = input.v_RelativePosition;
	v_Normal = input.v_Normal;
	v_Tangent = input.v_Tangent;
	v_TexCoord = input.v_TexCoord;
	v_InstanceIndex = input.v_InstanceIndex;
	v_LODFade = input.v_LODFade;
#ifdef ST_VELOCITY
	v_CurrentClip = input.v_CurrentClip;
	v_PreviousClip = input.v_PreviousClip;
#endif
	st_Main();
	ST_Output output;
#ifdef ST_SELECTION_MASK
	output.o_Selected = o_Selected;
#else
	output.o_Normal = o_Normal;
#ifndef ST_NO_ENTITY_IDS
	output.o_EntityID = o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
	output.o_Velocity = o_Velocity;
#elif defined(ST_VELOCITY)
	output.o_Velocity = o_Velocity;
#endif
#if defined(ST_PREPASS_SURFACE) && defined(ST_NO_ENTITY_IDS)
	output.o_Surface = o_Surface;
#elif defined(ST_PREPASS_SURFACE)
	output.o_Surface = o_Surface;
#endif
#endif
	return output;
}
