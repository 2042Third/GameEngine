#include "Include/Common.slang"

#include "Include/Common.slang"

// Builds one cubemap mip from the previous one (2x2 box filter per face).

Texture2DArray<float4> u_Source : register(t0);
SamplerState u_Sampler : register(s0);
[vk::image_format("rgba16f")] RWTexture2DArray<float4> o_Output : register(u0);

struct Parameters
{
	uint OutputSize;
	uint SourceSize;
	float Roughness;
	uint SampleCount;
};
[[vk::push_constant]] ConstantBuffer<Parameters> u_Parameters;

void st_Main()
{
	uint3 id = st_GlobalInvocationID;
	if (id.x >= u_Parameters.OutputSize || id.y >= u_Parameters.OutputSize)
		return;
	int2 source = int2(id.xy) * 2;
	int last = int(u_Parameters.SourceSize) - 1;
	int layer = int(id.z);
	float4 sum = u_Source.Load(int4(int3(min(source, int2(last)), layer), 0))
		+ u_Source.Load(int4(int3(min(source + int2(1, 0), int2(last)), layer), 0))
		+ u_Source.Load(int4(int3(min(source + int2(0, 1), int2(last)), layer), 0))
		+ u_Source.Load(int4(int3(min(source + int2(1, 1), int2(last)), layer), 0));
	o_Output[int3(id)] = sum * 0.25;
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
