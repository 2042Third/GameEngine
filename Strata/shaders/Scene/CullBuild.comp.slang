#include "Include/Common.slang"

#include "Include/Culling.slang"

// After the counting pass of Cull.comp: one thread per (dense bucket, LOD record). A record with instances gets its range
// of the draw list and an indirect draw in its bucket (DrawIndexedIndirectArguments: the level's indices at the mesh's
// place in the geometry pool, firstInstance the range's start, which the vertex shaders read the draw list from), and
// its bucket's draw count (u_BucketCounts) grows by one. Records the draw list or the arguments have no room for are
// counted as overflow and not drawn. With occlusion culling it runs once per phase, each time over that phase's buckets
// (the view's second-phase buckets, the cascades' second-phase buckets, or all others); the local shadow views' buckets are
// built by a run of their own, once per batch of views.

struct BuildParameters
{
	// Bit 1: the view's second phase (Cull.comp's modes); bit 2: the cascades' second phase; bit 3: the local shadow views' buckets;
	// bit 4: clears the local shadow views' buckets for the next batch of views instead (counts, draws and arguments)
	uint Mode;
};
[[vk::push_constant]] ConstantBuffer<BuildParameters> u_Parameters;

#define ST_BUILD_LOCAL_PHASE 8u
#define ST_BUILD_RESET_LOCAL 16u

void WriteArguments(uint dense, uint slot, uint indexCount, uint instanceCount, uint firstIndex, uint baseVertex, uint firstInstance)
{
	uint argument = (dense * u_Cull.ArgumentsPerBucket + slot) * 5u;
	u_Arguments[argument + 0u] = indexCount;
	u_Arguments[argument + 1u] = instanceCount;
	u_Arguments[argument + 2u] = firstIndex;
	u_Arguments[argument + 3u] = baseVertex;
	u_Arguments[argument + 4u] = firstInstance;
}

// Clears the local shadow views' buckets (word `index` of their counts, arguments and draw counts) for the next batch of views:
// the previous batch's draws were recorded before.
void ResetLocalBuckets(uint index)
{
	uint buckets = u_Cull.BucketCount - u_Cull.FirstLocalBucket;
	if (index < buckets * u_Cull.LODCapacity)
		u_Counts[u_Cull.FirstLocalBucket * u_Cull.LODCapacity + index] = 0u;
	if (index < buckets * u_Cull.ArgumentsPerBucket * 5u)
		u_Arguments[u_Cull.FirstLocalBucket * u_Cull.ArgumentsPerBucket * 5u + index] = 0u;
	if (index < buckets)
		u_BucketCounts[u_Cull.FirstLocalBucket + index] = 0u;
}

// Dispatch (SceneRendererGPUDriven.cpp) fills rows of 65,535 groups of 64 whenever there is more than one row; with one
// row the y term is 0.
static const uint c_CullRowItems = 65535u * 64u;

void st_Main()
{
	if ((u_Parameters.Mode & ST_BUILD_RESET_LOCAL) != 0u)
	{
		// Dispatched in rows of at most 65,535 work groups.
		ResetLocalBuckets(st_GlobalInvocationID.x + st_GlobalInvocationID.y * c_CullRowItems);
		return;
	}
	uint key = st_GlobalInvocationID.x;
	if (key >= u_Cull.BucketCount * u_Cull.LODCapacity)
		return;
	uint count = u_Counts[key];
	if (count == 0u)
		return;
	uint dense = key / u_Cull.LODCapacity;
	uint record = key - dense * u_Cull.LODCapacity;
	uint phase = 0u;
	if (dense >= u_Cull.FirstPhaseViewBuckets && dense < u_Cull.ViewBucketCount)
		phase = 2u;
	else if (dense >= u_Cull.FirstLocalBucket)
		phase = ST_BUILD_LOCAL_PHASE;
	else if (dense >= u_Cull.SecondPhaseShadowBuckets)
		phase = 4u;
	if (phase != (u_Parameters.Mode & 14u))
		return;
	uint st_Atomic1; InterlockedAdd(u_Counters[ST_COUNTER_DRAW_LIST_USED], count, st_Atomic1);
	uint offset = st_Atomic1;
	uint st_Atomic2; InterlockedAdd(u_BucketCounts[dense], 1u, st_Atomic2);
	uint slot = st_Atomic2;
	bool listRoom = count <= u_Cull.DrawListCapacity && offset <= u_Cull.DrawListCapacity - count;
	if (!listRoom || slot >= u_Cull.ArgumentsPerBucket)
	{
		u_Offsets[key] = ST_CULL_NONE;
		InterlockedAdd(u_Counters[ST_COUNTER_OVERFLOW], count);
		if (slot < u_Cull.ArgumentsPerBucket)
			WriteArguments(dense, slot, 0u, 0u, 0u, 0u, 0u);
		return;
	}
	u_Offsets[key] = offset;
	// Beyond the slots the draws read without a GPU-written count: written, but not drawn (the CPU's bound missed it).
	if (u_Cull.DrawsBounded != 0u && slot >= u_BucketTable[ST_CULL_BUCKETS + dense])
		InterlockedAdd(u_Counters[ST_COUNTER_OVERFLOW], count);
	GeometryLOD lod = u_GeometryLODs[record];
	GeometryEntry entry = u_GeometryEntries[lod.Mesh];
	WriteArguments(dense, slot, lod.IndexCount, count, entry.FirstIndex + lod.IndexOffset, entry.FirstVertex + lod.BaseVertex, offset);
	if (dense < u_Cull.ViewBucketCount)
	{
		InterlockedAdd(u_Counters[ST_COUNTER_VIEW_DRAWS], 1u);
		// Triangles, 64 bits: the product's high word, plus the carry of the low word's addition.
		uint high;
		uint low;
		UnsignedMultiplyExtended(count, lod.IndexCount / 3u, high, low);
		uint st_Atomic3; InterlockedAdd(u_Counters[ST_COUNTER_TRIANGLES_LOW], low, st_Atomic3);
		uint previous = st_Atomic3;
		if (previous + low < previous)
			high++;
		if (high != 0u)
			InterlockedAdd(u_Counters[ST_COUNTER_TRIANGLES_HIGH], high);
	}
	else if (phase == ST_BUILD_LOCAL_PHASE)
	{
		InterlockedAdd(u_Counters[ST_COUNTER_LOCAL_SHADOW_DRAWS], 1u);
	}
	else
	{
		InterlockedAdd(u_Counters[ST_COUNTER_SHADOW_DRAWS], 1u);
		if (phase == 4u)
			InterlockedAdd(u_Counters[ST_COUNTER_SECOND_PHASE_SHADOW_DRAWS], 1u);
	}
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
