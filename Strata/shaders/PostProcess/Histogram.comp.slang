#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/Exposure.slang"

// Luminance histogram of the HDR scene color (input of Exposure.comp). Each workgroup counts its 16x16 pixels in
// shared memory and adds them to the global histogram, which is cleared before the pass. The scene color is pre-exposed:
// the luminance is its value divided by the pre-exposure (exact, a power of two).

 // One thread per bin

Texture2D<float4> u_SceneColor : register(t0);
RWStructuredBuffer<uint> u_Histogram : register(u0);

// RenderData::ExposureParameters (the pre-exposure and the metering are read here).
struct ExposureParameters
{
	float MinLog2;
	float MaxLog2;
	float AdaptLight;
	float AdaptDark;
	float Compensation;
	float LowPercentile;
	float HighPercentile;
	float InversePreExposure;
	float KeyStrength;
	float KeyReferenceLog2;
	float KeyMinStops;
	float KeyMaxStops;
	float MeteringAspect;
	uint Metering; // ExposureMetering: 0 every pixel alike, 1 center-weighted
	float Padding0;
	float Padding1;
};
[[vk::push_constant]] ConstantBuffer<ExposureParameters> u_Parameters;

#define ST_METERING_CENTER_WEIGHTED 1u

// A pixel's weight in the histogram. Average: 1 (every pixel alike). CenterWeighted: 16 at the image's center falling to 4 at
// its edges, a Gaussian of 0.25 image heights (r, the distance from the center in image heights): the middle third of the
// frame decides, as a camera's center-weighted meter. At 3840x2160 the sum stays far below 2^32 (8.3 million pixels times 16).
uint MeteringWeight(int2 pixel, int2 size)
{
	if (u_Parameters.Metering != ST_METERING_CENTER_WEIGHTED)
		return 1u;
	float2 uv = (float2(pixel) + 0.5) / float2(size);
	float2 offset = (uv - 0.5) * float2(u_Parameters.MeteringAspect, 1.0);
	float falloff = exp(-dot(offset, offset) / (2.0 * 0.25 * 0.25));
	return uint(round(16.0 * (0.25 + 0.75 * falloff)));
}

groupshared uint s_Bins[ST_HISTOGRAM_BINS];

void st_Main()
{
	s_Bins[st_LocalInvocationIndex] = 0u;
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = TextureSize(u_SceneColor, 0);
	if (all((pixel < size)))
	{
		uint bin = LuminanceToHistogramBin(Luminance(u_SceneColor.Load(int3(pixel, 0)).rgb) * u_Parameters.InversePreExposure);
		InterlockedAdd(s_Bins[bin], MeteringWeight(pixel, size));
	}
	GroupMemoryBarrierWithGroupSync();

	uint count = s_Bins[st_LocalInvocationIndex];
	if (count > 0u)
		InterlockedAdd(u_Histogram[st_LocalInvocationIndex], count);
}

// --- Entry point ---
[numthreads(16, 16, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
