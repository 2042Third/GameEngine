#include "Include/Common.slang"

#include "Include/IBL.slang"

// Diffuse irradiance cubemap: cosine-weighted average of the environment's radiance over the hemisphere around each
// direction (Lambert diffuse = albedo * value). Filtered importance sampling reads lower environment mips for
// sparse samples, so few samples suffice without noise.

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
	float sourceSize = float(u_Parameters.SourceSize);
	float texelSolidAngle = 4.0 * PI / (6.0 * sourceSize * sourceSize);
	float maxMip = log2(sourceSize);

	float3 sum = float3(0.0);
	for (uint index = 0u; index < u_Parameters.SampleCount; index++)
	{
		float3 direction = SampleCosineHemisphere(Hammersley(index, u_Parameters.SampleCount), normal);
		float pdf = max(dot(normal, direction), 1e-4) * INV_PI;
		float sampleSolidAngle = 1.0 / (float(u_Parameters.SampleCount) * pdf);
		float mip = clamp(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1.0, 0.0, maxMip);
		sum += u_Environment.SampleLevel(u_Sampler, direction, mip).rgb;
	}
	o_Output[int3(id)] = float4(sum / float(u_Parameters.SampleCount), 1.0);
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
