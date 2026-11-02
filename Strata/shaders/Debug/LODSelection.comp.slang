#include "Include/Common.slang"

#include "Include/Common.slang"

// Diagnostic shader used by GPU tests: the level of detail of each fraction, as GPU culling chooses it, for comparison
// with the CPU's LODSelection::SelectLOD (halving, Mode 0) and SelectLODByError (Mode 1, with the levels' relative errors).

StructuredBuffer<float> u_Fractions : register(t0);
StructuredBuffer<float> u_RelativeErrors : register(t1);
RWStructuredBuffer<uint> o_Levels : register(u0);

#define ST_LOD_RELATIVE_ERROR(record) u_RelativeErrors[record]
#include "Include/LODSelection.slang"

struct LODSelectionParameters
{
	uint Count;
	float Threshold; // Mode 0: the LOD threshold; mode 1: the error threshold
	uint LODCount;
	uint Mode;
};
[[vk::push_constant]] ConstantBuffer<LODSelectionParameters> u_Parameters;

void st_Main()
{
	uint index = st_GlobalInvocationID.x;
	if (index >= u_Parameters.Count)
		return;
	if (u_Parameters.Mode == 0u)
		o_Levels[index] = SelectLOD(u_Fractions[index], u_Parameters.Threshold, u_Parameters.LODCount);
	else
		o_Levels[index] = SelectLODByError(u_Fractions[index], 0u, u_Parameters.LODCount, u_Parameters.Threshold);
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
