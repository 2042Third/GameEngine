#include "Include/Common.slang"

#include "Include/BlueNoise.slang"

// Diagnostic shader used by GPU tests: BlueNoise() at every pixel of a 64x64 block, shifted by Shift, as the sun shadows'
// shaders read it, for comparison with RenderData::c_BlueNoise.

RWStructuredBuffer<float> o_Values : register(u0);

struct BlueNoiseParameters
{
	int2 Shift;
	uint Reserved0;
	uint Reserved1;
};
[[vk::push_constant]] ConstantBuffer<BlueNoiseParameters> u_Parameters;

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	o_Values[pixel.y * 64 + pixel.x] = BlueNoise(pixel, u_Parameters.Shift);
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
