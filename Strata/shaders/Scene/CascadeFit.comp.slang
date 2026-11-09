#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/OrderedFloat.slang"

// The directional shadow cascades' projections (RenderData::CascadeData), one thread per cascade: each covers its square in
// light view space, from the far side of its view slice's bounding sphere to the nearest caster toward the light (at least
// the sphere's near side, at most MaxCasterDistance beyond it; ShadowMesh.vert flattens casters beyond onto the near
// plane). Both paths fit here: the CPU path with the casters it found, the GPU-driven path with those its culling found,
// the same frame (Cull.comp's shadow pass writes u_CasterDepths). The projection is Math::OrthographicReverseZ.

struct CascadeFitBuffer
{
	float4x4 LightView;
	float4 Boxes[5];       // Light view space: min.xy, max.xy (RenderData::c_MaxCascades)
	// x: the far side's z, y: the sphere's near side's z, z: world units per texel, w: the nearest drawn caster's z, as the CPU
	// found it
	float4 Depths[5];
	uint CascadeCount;
	uint CasterDepthsFromGPU;
	float MaxCasterDistance;
	uint Reserved;
};
ConstantBuffer<CascadeFitBuffer> u_Fit : register(b0);

RWStructuredBuffer<uint> u_CasterDepths : register(u0);

struct CascadeData
{
	float4x4 ViewProjection;
	float4 Parameters;
};
RWStructuredBuffer<CascadeData> u_Cascades : register(u1);

void st_Main()
{
	uint cascade = st_LocalInvocationIndex;
	if (cascade >= u_Fit.CascadeCount)
		return;
	float4 box = u_Fit.Boxes[cascade];
	float4 depths = u_Fit.Depths[cascade];
	float casterNearZ = depths.y;
	if (u_Fit.CasterDepthsFromGPU != 0u)
	{
		uint encoded = u_CasterDepths[cascade];
		if (encoded != 0u)
			casterNearZ = max(casterNearZ, DecodeOrderedFloat(encoded));
	}
	else
	{
		casterNearZ = max(casterNearZ, depths.w);
	}
	float nearZ = clamp(casterNearZ, depths.y, depths.y + u_Fit.MaxCasterDistance);
	float nearPlane = -nearZ;
	float farPlane = -depths.x;

	// glm::orthoRH_ZO with the planes swapped (reversed-Z: the near plane at depth 1).
	float zNear = farPlane;
	float zFar = nearPlane;
	float4x4 projection = float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0);
	projection[0][0] = 2.0 / (box.z - box.x);
	projection[1][1] = 2.0 / (box.w - box.y);
	projection[2][2] = -1.0 / (zFar - zNear);
	projection[0][3] = -(box.z + box.x) / (box.z - box.x);
	projection[1][3] = -(box.w + box.y) / (box.w - box.y);
	projection[2][3] = -zNear / (zFar - zNear);
	u_Cascades[cascade].ViewProjection = mul(projection, u_Fit.LightView);
	// z: 0 (the penumbra's search distance follows the receiver's view distance alike in every cascade: Include/Shadows.slang,
	// ShadowSearchDistance); w: the near plane's light view z, for the cascades' occlusion culling (Cull.comp's
	// IsOccludedInCascade).
	u_Cascades[cascade].Parameters = float4(depths.z, farPlane - nearPlane, 0.0, nearZ);
}

// --- Entry point ---
[numthreads(5, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
