#include "Include/Common.slang"

#include "Include/Lights.slang"

// The first of the GPU's light culling passes: one thread per light record tests whether the light is drawn and its
// bounding sphere reaches the view's frustum (camera-relative, as Frustum::IsSphereVisible tests on the CPU path), and each
// work group counts its visible lights (and adds its drawn ones to the frame's counters). Scene/LightCompact.comp then
// packs them, in record order.

RWStructuredBuffer<uint> o_LightVisible : register(u1);
RWStructuredBuffer<uint> o_LightGroupCounts : register(u2);
RWStructuredBuffer<uint> u_Counters : register(u6);

groupshared uint s_Visible;
groupshared uint s_Drawn;

void st_Main()
{
	uint record = st_GlobalInvocationID.x;
	if (st_LocalInvocationIndex == 0u)
	{
		s_Visible = 0u;
		s_Drawn = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	bool visible = false;
	bool drawn = false;
	if (record < u_LightCull.RecordCount)
	{
		LightRecord light = u_LightRecords[record];
		drawn = light.SpotAngles.z != 0.0;
		if (drawn)
		{
			float3 center = light.Bounds.xyz - u_Frame.CameraPosition.xyz;
			visible = true;
			for (int plane = 0; plane < 6; plane++)
			{
				if (dot(u_LightCull.Planes[plane].xyz, center) + u_LightCull.Planes[plane].w < -light.Bounds.w)
					visible = false;
			}
		}
		o_LightVisible[record] = visible ? 1u : 0u;
	}
	if (visible)
		InterlockedAdd(s_Visible, 1u);
	if (drawn)
		InterlockedAdd(s_Drawn, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (st_LocalInvocationIndex == 0u)
	{
		o_LightGroupCounts[st_WorkGroupID.x] = s_Visible;
		if (s_Drawn > 0u)
			InterlockedAdd(u_Counters[ST_COUNTER_DRAWN_LIGHTS], s_Drawn);
	}
}

// --- Entry point ---
[numthreads(ST_LIGHT_GROUP_SIZE, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
