#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/SphericalHarmonics.slang"

// Projects a reflection probe's capture (a level of the cube it was converted into) onto the real spherical harmonics of
// bands 0 to 2 and writes its slot's irradiance coefficients: each radiance coefficient times the clamped cosine's band
// factor (1, 2/3, 1/4 of pi, over pi), so that EvaluateProbeIrradiance gives the irradiance over pi. One work group.

#define ST_GROUP_SIZE 64u
#define ST_REFLECTION_PROBE_SH 9u

Texture2DArray<float4> u_Source : register(t0);
RWStructuredBuffer<float4> o_ProbeIrradiance : register(u0);

struct Parameters
{
	uint SourceSize;
	uint Slot;
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<Parameters> u_Parameters;

groupshared float3 s_Sums[ST_GROUP_SIZE][9];
groupshared float s_Weights[ST_GROUP_SIZE];

// The direction through a texel's center of cube face `face` (+X, -X, +Y, -Y, +Z, -Z), unnormalized, as IBL.glsl lays faces out.
float3 FaceDirection(float2 st, uint face)
{
	switch (face)
	{
		case 0u: return float3(1.0, -st.y, -st.x);
		case 1u: return float3(-1.0, -st.y, st.x);
		case 2u: return float3(st.x, 1.0, st.y);
		case 3u: return float3(st.x, -1.0, -st.y);
		case 4u: return float3(st.x, -st.y, 1.0);
		default: return float3(-st.x, -st.y, -1.0);
	}
}

void st_Main()
{
	uint thread = st_LocalInvocationIndex;
	uint size = u_Parameters.SourceSize;
	uint texels = size * size * 6u;
	float3 sums[9];
	for (uint index = 0u; index < 9u; index++)
		sums[index] = float3(0.0);
	float weight = 0.0;
	for (uint texel = thread; texel < texels; texel += ST_GROUP_SIZE)
	{
		uint face = texel / (size * size);
		uint inFace = texel - face * size * size;
		uint2 xy = uint2(inFace % size, inFace / size);
		float2 st = (float2(xy) + 0.5) / float(size) * 2.0 - 1.0;
		float3 direction = FaceDirection(st, face);
		// The texel's solid angle: its area on the face over the cube of its distance from the center.
		float distanceSquared = dot(direction, direction);
		float solidAngle = (4.0 / float(size * size)) / (distanceSquared * sqrt(distanceSquared));
		float3 radiance = u_Source.Load(int4(int3(xy, face), 0)).rgb;
		float basis[9];
		ProbeSHBasis(direction * (1.0 / sqrt(distanceSquared)), basis);
		for (uint index = 0u; index < 9u; index++)
			sums[index] += radiance * (basis[index] * solidAngle);
		weight += solidAngle;
	}
	for (uint index = 0u; index < 9u; index++)
		s_Sums[thread][index] = sums[index];
	s_Weights[thread] = weight;
	GroupMemoryBarrierWithGroupSync();

	if (thread < 9u)
	{
		float3 sum = float3(0.0);
		float total = 0.0;
		for (uint other = 0u; other < ST_GROUP_SIZE; other++)
		{
			sum += s_Sums[other][thread];
			total += s_Weights[other];
		}
		// The texels' solid angles sum to about 4 pi: normalized to it exactly. Then the clamped cosine's band factors.
		sum *= (4.0 * PI) / max(total, 1e-6);
		float band = thread == 0u ? 1.0 : (thread < 4u ? 2.0 / 3.0 : 0.25);
		o_ProbeIrradiance[u_Parameters.Slot * ST_REFLECTION_PROBE_SH + thread] = float4(sum * band, 0.0);
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
