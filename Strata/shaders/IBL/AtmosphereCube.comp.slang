#include "Include/Common.slang"

#include "Include/AtmospherePass.slang"
#include "Include/IBL.slang"

// Level 0 of an atmosphere's environment cube from the sky-view table, a face per layer of the dispatch from
// u_Pass.FirstFace on (one face per frame while it refreshes, or all six at once), scaled as the table is
// (ST_ATMOSPHERE_STORAGE_SCALE: readers multiply by EnvironmentParams.x). The sun's disk is not in it: the background draws
// it, and the directional light lights the scene with it.

[vk::image_format("rgba16f")] RWTexture2DArray<float4> o_Cube : register(u0);

void st_Main()
{
	uint3 id = st_GlobalInvocationID;
	uint size = u_Pass.FaceSize;
	if (id.x >= size || id.y >= size)
		return;
	uint face = u_Pass.FirstFace + id.z;
	float3 direction = CubeTexelDirection(id.xy, face, size);
	float3 color = u_SkyViewTable.SampleLevel(u_TableSampler, DirectionToSkyViewUV(direction), 0.0).rgb;
	o_Cube[int3(id.xy, face)] = float4(clamp(color, float3(0.0), float3(65000.0)), 1.0);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
