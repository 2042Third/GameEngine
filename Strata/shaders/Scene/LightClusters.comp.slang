#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/Clusters.slang"
#include "Include/OrderedFloat.slang"

// Clustered light assignment: one thread per cluster tests the bounding sphere of every visible light against the
// cluster's view-space bounding box and lists the lights that reach it (see Include/Clusters.glsl for the layout), in
// buffer order. When more reach a cluster than it holds, it keeps the most relevant ones (GetLightRelevance): the CPU path
// sorts its light buffer by relevance, so its first lights are those; the GPU-driven path's buffer is in record order
// (Scene/LightCompact.comp), so a full cluster finds the relevance its last kept light has by bisection and keeps those
// at least that relevant, the lower buffer positions first among equals. Workgroups stage the light bounds through shared
// memory.

// View-space bounding spheres (xyz: center, w: radius), parallel to the light buffer.
StructuredBuffer<float4> u_LightBounds : register(t13);
RWStructuredBuffer<uint> o_ClusterLights : register(u0);
// The visible lights' count where the GPU culled them (LightCounts.y), written by Scene/LightCompact.comp.
StructuredBuffer<uint> u_LightCount : register(t21);

groupshared float4 s_LightBounds[64];
groupshared uint s_Overflow;

// View-space point on the ray through `ndc` at a view depth (distance along the view axis); valid for perspective and
// orthographic projections alike.
float3 ViewPointAtDepth(float2 ndc, float viewDepth)
{
	float4 nearPoint = mul(u_Frame.InverseProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(u_Frame.InverseProjection, float4(ndc, 0.5, 1.0));
	float3 origin = nearPoint.xyz / nearPoint.w;
	float3 direction = farPoint.xyz / farPoint.w - origin;
	float t = (-viewDepth - origin.z) / direction.z;
	return origin + direction * t;
}

bool SphereIntersectsBox(float4 sphere, float3 boxMin, float3 boxMax)
{
	float3 offset = sphere.xyz - clamp(sphere.xyz, boxMin, boxMax);
	return dot(offset, offset) <= sphere.w * sphere.w;
}

void st_Main()
{
	uint3 grid = u_Frame.ClusterGrid.xyz;
	uint cluster = st_GlobalInvocationID.x;
	bool valid = cluster < grid.x * grid.y * grid.z;

	float3 boxMin = float3(0.0);
	float3 boxMax = float3(0.0);
	if (valid)
	{
		uint x = cluster % grid.x;
		uint y = (cluster / grid.x) % grid.y;
		uint slice = cluster / (grid.x * grid.y);
		// Tile row 0 is at the top of the screen (NDC +Y).
		float2 ndcMin = float2(float(x) / float(grid.x) * 2.0 - 1.0, 1.0 - float(y + 1u) / float(grid.y) * 2.0);
		float2 ndcMax = float2(float(x + 1u) / float(grid.x) * 2.0 - 1.0, 1.0 - float(y) / float(grid.y) * 2.0);
		float depths[2] = { GetClusterSliceStart(slice), GetClusterSliceStart(slice + 1u) };

		boxMin = float3(3.0e38);
		boxMax = float3(-3.0e38);
		for (int corner = 0; corner < 8; corner++)
		{
			float2 ndc = float2((corner & 1) != 0 ? ndcMax.x : ndcMin.x, (corner & 2) != 0 ? ndcMax.y : ndcMin.y);
			float3 point = ViewPointAtDepth(ndc, depths[corner >> 2]);
			boxMin = min(boxMin, point);
			boxMax = max(boxMax, point);
		}
	}

	uint capacity = u_Frame.ClusterGrid.w;
	uint base = cluster * (capacity + 1u);
	uint lightCount = u_Frame.LightCounts.y != 0u ? u_LightCount[0] : u_Frame.LightCounts.x;

	// Every light reaching the cluster, the first `capacity` of them listed.
	uint count = 0u;
	uint reaching = 0u;
	for (uint first = 0u; first < lightCount; first += 64u)
	{
		uint light = first + st_LocalInvocationIndex;
		s_LightBounds[st_LocalInvocationIndex] = light < lightCount ? u_LightBounds[light] : float4(0.0, 0.0, 0.0, -1.0);
		GroupMemoryBarrierWithGroupSync();

		if (valid)
		{
			uint batch = min(64u, lightCount - first);
			for (uint index = 0u; index < batch; index++)
			{
				if (SphereIntersectsBox(s_LightBounds[index], boxMin, boxMax))
				{
					if (count < capacity)
					{
						o_ClusterLights[base + 1u + count] = first + index;
						count++;
					}
					reaching++;
				}
			}
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Full clusters of a buffer not sorted by relevance keep the most relevant lights. The passes stage bounds for the whole
	// work group, so every thread runs them when any cluster of the group overflowed.
	if (st_LocalInvocationIndex == 0u)
		s_Overflow = 0u;
	GroupMemoryBarrierWithGroupSync();
	bool overflow = valid && reaching > capacity && u_Frame.LightCounts.y != 0u;
	if (overflow)
		InterlockedOr(s_Overflow, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (s_Overflow != 0u)
	{
		// The largest relevance (as ordered bits) that at least `capacity` of the cluster's lights reach, bit by bit.
		uint threshold = 0u;
		for (int bit = 31; bit >= 0; bit--)
		{
			uint candidate = threshold | (1u << uint(bit));
			uint atLeast = 0u;
			for (uint first = 0u; first < lightCount; first += 64u)
			{
				uint light = first + st_LocalInvocationIndex;
				s_LightBounds[st_LocalInvocationIndex] = light < lightCount ? u_LightBounds[light] : float4(0.0, 0.0, 0.0, -1.0);
				GroupMemoryBarrierWithGroupSync();
				if (overflow)
				{
					uint batch = min(64u, lightCount - first);
					for (uint index = 0u; index < batch; index++)
					{
						if (SphereIntersectsBox(s_LightBounds[index], boxMin, boxMax) && EncodeOrderedFloat(GetLightRelevance(s_LightBounds[index])) >= candidate)
							atLeast++;
					}
				}
				GroupMemoryBarrierWithGroupSync();
			}
			if (overflow && atLeast >= capacity)
				threshold = candidate;
		}
		// The lights above it, then those at it in buffer order until the cluster is full, all in buffer order.
		uint above = 0u;
		for (uint first = 0u; first < lightCount; first += 64u)
		{
			uint light = first + st_LocalInvocationIndex;
			s_LightBounds[st_LocalInvocationIndex] = light < lightCount ? u_LightBounds[light] : float4(0.0, 0.0, 0.0, -1.0);
			GroupMemoryBarrierWithGroupSync();
			if (overflow)
			{
				uint batch = min(64u, lightCount - first);
				for (uint index = 0u; index < batch; index++)
				{
					if (SphereIntersectsBox(s_LightBounds[index], boxMin, boxMax) && EncodeOrderedFloat(GetLightRelevance(s_LightBounds[index])) > threshold)
						above++;
				}
			}
			GroupMemoryBarrierWithGroupSync();
		}
		uint equalRoom = capacity - min(above, capacity);
		uint kept = 0u;
		for (uint first = 0u; first < lightCount; first += 64u)
		{
			uint light = first + st_LocalInvocationIndex;
			s_LightBounds[st_LocalInvocationIndex] = light < lightCount ? u_LightBounds[light] : float4(0.0, 0.0, 0.0, -1.0);
			GroupMemoryBarrierWithGroupSync();
			if (overflow)
			{
				uint batch = min(64u, lightCount - first);
				for (uint index = 0u; index < batch && kept < capacity; index++)
				{
					if (!SphereIntersectsBox(s_LightBounds[index], boxMin, boxMax))
						continue;
					uint bits = EncodeOrderedFloat(GetLightRelevance(s_LightBounds[index]));
					bool keep = bits > threshold;
					if (!keep && bits == threshold && equalRoom > 0u)
					{
						keep = true;
						equalRoom--;
					}
					if (keep)
					{
						o_ClusterLights[base + 1u + kept] = first + index;
						kept++;
					}
				}
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (overflow)
			count = kept;
	}

	if (valid)
		o_ClusterLights[base] = count;
}

// --- Entry point ---
[numthreads(64, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
