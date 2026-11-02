#include "Include/Common.slang"

#include "Include/IBL.slang"

// Resamples an equirectangular environment map into mip 0 of a cubemap.

Texture2D<float4> u_Source : register(t0);
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
	float3 direction = CubeTexelDirection(id.xy, id.z, u_Parameters.OutputSize);
	// A face spans a quarter of the equirect's width: pick the source mip whose texel density matches the face.
	float lod = max(0.0, log2(float(u_Parameters.SourceSize) / (4.0 * float(u_Parameters.OutputSize))));
	float3 color = u_Source.SampleLevel(u_Sampler, DirectionToEquirect(direction), lod).rgb;
	// Keep the result finite and within half-float range (sun disks in HDRIs can exceed it).
	color = clamp(color, float3(0.0), float3(65000.0));
	o_Output[int3(id)] = float4(color, 1.0);
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
