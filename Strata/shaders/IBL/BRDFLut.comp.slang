#include "Include/Common.slang"

#include "Include/IBL.slang"
#include "Include/PBR.slang"

// Split-sum environment BRDF: for (NoV, roughness), the scale (x) and bias (y) applied to F0 to integrate the
// specular lobe against a white environment. Uses the same height-correlated Smith visibility as direct lighting.

[vk::image_format("rg16f")] RWTexture2D<float4> o_Output : register(u0);

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
	uint2 id = st_GlobalInvocationID.xy;
	if (id.x >= u_Parameters.OutputSize || id.y >= u_Parameters.OutputSize)
		return;

	float NoV = (float(id.x) + 0.5) / float(u_Parameters.OutputSize);
	float roughness = (float(id.y) + 0.5) / float(u_Parameters.OutputSize);
	float alpha = roughness * roughness;
	float3 view = float3(sqrt(1.0 - NoV * NoV), 0.0, NoV);
	float3 normal = float3(0.0, 0.0, 1.0);

	float2 sum = float2(0.0);
	for (uint index = 0u; index < u_Parameters.SampleCount; index++)
	{
		float3 halfVector = ImportanceSampleGGX(Hammersley(index, u_Parameters.SampleCount), normal, alpha);
		float3 light = reflect(-view, halfVector);
		float NoL = clamp(light.z, 0.0, 1.0);
		float NoH = clamp(halfVector.z, 0.0, 1.0);
		float VoH = clamp(dot(view, halfVector), 0.0, 1.0);
		if (NoL <= 0.0)
			continue;

		// pdf = D * NoH / (4 VoH); BRDF * NoL / pdf = 4 * V * NoL * VoH / NoH (D cancels).
		float visibility = VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * 4.0 * NoL * VoH / max(NoH, 1e-4);
		float fresnel = pow(1.0 - VoH, 5.0);
		sum += float2(1.0 - fresnel, fresnel) * visibility;
	}
	o_Output[int2(id)] = float4(sum / float(u_Parameters.SampleCount), 0.0, 0.0);
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
