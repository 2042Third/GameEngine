#include "Include/Common.slang"

#include "Include/Lights.slang"

// The second of the GPU's light culling passes: packs the visible lights (Scene/LightCull.comp) into the light buffer in
// record order, so that what the clusters list and the forward pass adds up does not depend on scheduling. Each work group
// finds where its lights go from the counts of the groups before it and a scan of its own; group 0 also writes the total.
// The lights go camera-relative (the shaders' space) with their view-space bounding spheres, as CollectLights uploads them.

StructuredBuffer<uint> u_LightVisible : register(t19);
StructuredBuffer<uint> u_LightGroupCounts : register(t20);
// Per light record its shadow slot plus one (0: no shadow): the renderer's shadowed point and spot lights (SceneRenderer's
// local shadows).
StructuredBuffer<uint> u_LightShadowSlots : register(t21);
RWStructuredBuffer<LightData> o_Lights : register(u3);
RWStructuredBuffer<float4> o_LightBounds : register(u4);
RWStructuredBuffer<uint> o_LightCount : register(u5);
RWStructuredBuffer<uint> u_Counters : register(u6);

groupshared uint s_Values[ST_LIGHT_GROUP_SIZE];

// The sum of u_LightGroupCounts[0, end) over the work group (every thread gets it).
uint SumGroupCounts(uint end)
{
	uint local = st_LocalInvocationIndex;
	uint partial = 0u;
	for (uint group = local; group < end; group += ST_LIGHT_GROUP_SIZE)
		partial += u_LightGroupCounts[group];
	s_Values[local] = partial;
	GroupMemoryBarrierWithGroupSync();
	for (uint stride = ST_LIGHT_GROUP_SIZE / 2u; stride > 0u; stride >>= 1u)
	{
		if (local < stride)
			s_Values[local] += s_Values[local + stride];
		GroupMemoryBarrierWithGroupSync();
	}
	uint sum = s_Values[0];
	GroupMemoryBarrierWithGroupSync();
	return sum;
}

void st_Main()
{
	uint local = st_LocalInvocationIndex;
	uint record = st_GlobalInvocationID.x;
	uint offset = SumGroupCounts(st_WorkGroupID.x);

	// The work group's inclusive scan of its visible flags.
	uint flag = record < u_LightCull.RecordCount ? u_LightVisible[record] : 0u;
	s_Values[local] = flag;
	GroupMemoryBarrierWithGroupSync();
	for (uint step = 1u; step < ST_LIGHT_GROUP_SIZE; step <<= 1u)
	{
		uint value = local >= step ? s_Values[local - step] : 0u;
		GroupMemoryBarrierWithGroupSync();
		s_Values[local] += value;
		GroupMemoryBarrierWithGroupSync();
	}
	uint position = offset + s_Values[local] - flag;
	GroupMemoryBarrierWithGroupSync();

	if (flag != 0u)
	{
		LightRecord light = u_LightRecords[record];
		LightData data;
		data.PositionRange = float4(light.PositionRange.xyz - u_Frame.CameraPosition.xyz, light.PositionRange.w);
		data.Color = light.Color;
		data.DirectionType = light.DirectionType;
		data.SpotAngles = float4(light.SpotAngles.xy, float(u_LightShadowSlots[record]), 0.0);
		o_Lights[position] = data;
		float3 center = light.Bounds.xyz - u_Frame.CameraPosition.xyz;
		o_LightBounds[position] = float4(mul(((float3x3)(u_Frame.View)), center), light.Bounds.w);
	}

	if (st_WorkGroupID.x == 0u)
	{
		uint total = SumGroupCounts(u_LightCull.GroupCount);
		if (local == 0u)
		{
			o_LightCount[0] = total;
			u_Counters[ST_COUNTER_VISIBLE_LIGHTS] = total;
		}
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
