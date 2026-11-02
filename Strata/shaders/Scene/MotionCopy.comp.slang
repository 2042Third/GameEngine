#include "Include/Common.slang"

#include "Include/Common.slang"

// A renderer's motion history (SceneRenderer): copies the listed elements of the render scene's instance or palette buffer
// into the renderer's copy of it, a thread per element.

StructuredBuffer<uint> u_Source : register(t0);
StructuredBuffer<uint> u_List : register(t1);
RWStructuredBuffer<uint> u_Destination : register(u0);

// RenderData::MotionCopyParameters
struct MotionCopyParameters
{
	uint First;
	uint Count;
	uint StrideWords;
	uint ListOffset;
};
[[vk::push_constant]] ConstantBuffer<MotionCopyParameters> u_Parameters;

void st_Main()
{
	uint entry = u_Parameters.First + st_GlobalInvocationID.x;
	if (entry >= u_Parameters.Count)
		return;
	uint base = u_List[u_Parameters.ListOffset + entry] * u_Parameters.StrideWords;
	for (uint word = 0u; word < u_Parameters.StrideWords; word++)
		u_Destination[base + word] = u_Source[base + word];
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
