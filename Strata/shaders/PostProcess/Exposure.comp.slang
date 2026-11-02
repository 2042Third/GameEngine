#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/Exposure.slang"

// Automatic exposure: the average log2 luminance of the histogram (ignoring the darkest and brightest pixels), clamped to
// the allowed range, adapts over time (faster toward brighter scenes than toward darker ones, as eyes do) and is mapped to
// middle gray, offset by the exposure key: a scene that meters below the sun's gray card shows darker (one above it is not
// brightened: RenderData::c_ExposureKeyMaxStops).

RWStructuredBuffer<uint> u_Histogram : register(u0);
RWStructuredBuffer<float> u_Exposure : register(u1);

// RenderData::ExposureParameters (Renderer/PostProcessData.h).
struct ExposureParameters
{
	float MinLog2;            // Range the average luminance is clamped to
	float MaxLog2;
	float AdaptLight;         // Fraction of the way to a brighter target this frame (light adaptation); 1 = jump to it
	float AdaptDark;          // Fraction of the way to a darker target (dark adaptation)
	float Compensation;       // Linear exposure compensation
	float LowPercentile;      // Pixels below this fraction (darkest) and above HighPercentile (brightest) are ignored
	float HighPercentile;
	float InversePreExposure; // Read by Histogram.comp
	float KeyStrength;        // The exposure key: stops per stop the metered average lies from the sun's gray card; 0 = off
	float KeyReferenceLog2;   // log2 of the luminance of a gray card (albedo 0.18) facing the directional light
	float KeyMinStops;        // The key's range
	float KeyMaxStops;
	float MeteringAspect;     // Width / height of the metered image
	uint Metering;            // ExposureMetering
	float Padding0;
	float Padding1;
};
[[vk::push_constant]] ConstantBuffer<ExposureParameters> u_Parameters;

static const float c_MiddleGray = 0.18;

groupshared uint s_Bins[ST_HISTOGRAM_BINS];

void st_Main()
{
	uint bin = st_LocalInvocationIndex;
	s_Bins[bin] = u_Histogram[bin];
	GroupMemoryBarrierWithGroupSync();
	if (bin != 0u)
		return;

	// Bin 0 holds black pixels (empty background, unlit areas). They say nothing about how bright the visible content
	// is, and metering them would over-expose a small lit object on a black background, so they are left out.
	float total = 0.0;
	for (uint index = 1u; index < ST_HISTOGRAM_BINS; index++)
		total += float(s_Bins[index]);

	bool metered = false;
	float target = 0.0;
	if (total > 0.0)
	{
		// Weighted mean of the bins, counting only the part of each bin inside the percentile window.
		float low = total * u_Parameters.LowPercentile;
		float high = total * u_Parameters.HighPercentile;
		float accumulated = 0.0;
		float sum = 0.0;
		float weight = 0.0;
		for (uint index = 1u; index < ST_HISTOGRAM_BINS; index++)
		{
			float count = float(s_Bins[index]);
			float inside = max(min(accumulated + count, high) - max(accumulated, low), 0.0);
			sum += inside * HistogramBinToLog2(index);
			weight += inside;
			accumulated += count;
		}
		if (weight > 0.0)
		{
			target = sum / weight;
			metered = true;
		}
	}
	target = clamp(target, u_Parameters.MinLog2, u_Parameters.MaxLog2);

	// The previous value is ignored when jumping, so an uninitialized buffer never leaks into the result. A black image
	// keeps the current adaptation (or starts from an average luminance of 1 when jumping).
	float previous = u_Exposure[ST_EXPOSURE_LOG_LUMINANCE];
	// A brighter target is followed at the light adaptation's rate, a darker one at the dark adaptation's.
	float adapt = target > previous ? u_Parameters.AdaptLight : u_Parameters.AdaptDark;
	float adapted;
	if (u_Parameters.AdaptLight >= 1.0 && u_Parameters.AdaptDark >= 1.0)
		adapted = target;
	else if (metered)
		adapted = lerp(previous, target, adapt);
	else
		adapted = previous;
	float keyStops = u_Parameters.KeyStrength > 0.0
		? clamp(u_Parameters.KeyStrength * (adapted - u_Parameters.KeyReferenceLog2), u_Parameters.KeyMinStops, u_Parameters.KeyMaxStops)
		: 0.0;
	u_Exposure[ST_EXPOSURE_LOG_LUMINANCE] = adapted;
	u_Exposure[ST_EXPOSURE_MULTIPLIER] = u_Parameters.Compensation * c_MiddleGray / exp2(adapted) * exp2(keyStops);
	u_Exposure[ST_EXPOSURE_KEY] = keyStops;
}

// --- Entry point ---
[numthreads(256, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
