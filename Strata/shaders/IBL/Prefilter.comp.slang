#include "Include/Common.slang"

#include "Include/IBL.slang"
#include "Include/PBR.slang"

// Specular prefiltered environment (split-sum approximation): one mip per roughness level, GGX importance sampling
// with N = V = R, filtered importance sampling from the environment's mips.

TextureCube<float4> u_Environment : register(t0);
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

	float3 normal = CubeTexelDirection(id.xy, id.z, u_Parameters.OutputSize);
	if (u_Parameters.Roughness <= 0.0)
	{
		o_Output[int3(id)] = float4(u_Environment.SampleLevel(u_Sampler, normal, 0.0).rgb, 1.0);
		return;
	}

	float alpha = u_Parameters.Roughness * u_Parameters.Roughness;
	float sourceSize = float(u_Parameters.SourceSize);
	float texelSolidAngle = 4.0 * PI / (6.0 * sourceSize * sourceSize);
	float maxMip = log2(sourceSize);

	float3 sum = float3(0.0);
	float weight = 0.0;
	for (uint index = 0u; index < u_Parameters.SampleCount; index++)
	{
		float3 halfVector = ImportanceSampleGGX(Hammersley(index, u_Parameters.SampleCount), normal, alpha);
		float3 light = reflect(-normal, halfVector);
		float NoL = dot(normal, light);
		if (NoL <= 0.0)
			continue;

		// With N = V the half-vector pdf reduces to D(NoH) / 4.
		float NoH = clamp(dot(normal, halfVector), 0.0, 1.0);
		float pdf = DistributionGGX(NoH, alpha) * 0.25;
		float sampleSolidAngle = 1.0 / (float(u_Parameters.SampleCount) * pdf + 1e-4);
		float mip = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, maxMip);
		sum += u_Environment.SampleLevel(u_Sampler, light, mip).rgb * NoL;
		weight += NoL;
	}
	o_Output[int3(id)] = float4(sum / max(weight, 1e-4), 1.0);
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
