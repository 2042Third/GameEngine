#include "Include/Common.slang"

#include "Include/LightingView.slang"

// The lighting debug views' statistics of a frame (SceneRenderer::GetLightingViewStats): every pixel of the HDR target counted
// in histograms, which the CPU reads back frames later. Lighting, Indirect and Luminance count EV100 (surfaces and the
// background apart); AmbientOcclusion and Albedo count the surfaces' values (the occlusion, the albedo's luminance) and the
// albedo's pixels outside [ST_ALBEDO_LOW, ST_ALBEDO_HIGH]. Each group adds its own histogram to the buffer (cleared before the
// dispatch); the first thread also writes the sky's light on a horizontal surface.

Texture2D<float4> u_HDR : register(t0);
Texture2D<float4> u_Depth : register(t1);
RWStructuredBuffer<uint> u_Stats : register(u0);

// The group's histograms: EV bins of surfaces, then of the background; value bins; the four counts.
groupshared uint s_EV[2u * ST_LIGHTING_VIEW_EV_BINS];
groupshared uint s_Value[ST_LIGHTING_VIEW_VALUE_BINS];
groupshared uint s_Counts[4];

bool IsValueView(uint view)
{
	return view == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION || view == ST_LIGHTING_VIEW_ALBEDO;
}

void st_Main()
{
	uint thread = st_LocalInvocationIndex;
	for (uint bin = thread; bin < 2u * ST_LIGHTING_VIEW_EV_BINS; bin += 256u)
		s_EV[bin] = 0u;
	if (thread < ST_LIGHTING_VIEW_VALUE_BINS)
		s_Value[thread] = 0u;
	if (thread < 4u)
		s_Counts[thread] = 0u;
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = int2(u_Frame.ViewportSize.xy);
	if (pixel.x < size.x && pixel.y < size.y)
	{
		float3 hdr = u_HDR.Load(int3(pixel, 0)).rgb;
		bool surface = u_Depth.Load(int3(pixel, 0)).r > 0.0;
		InterlockedAdd(s_Counts[surface ? 0u : 1u], 1u);
		if (IsValueView(u_LightingView.View))
		{
			if (surface)
			{
				float value = u_LightingView.View == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION ? hdr.r : Luminance(hdr);
				uint bin = uint(clamp(value * float(ST_LIGHTING_VIEW_VALUE_BINS), 0.0, float(ST_LIGHTING_VIEW_VALUE_BINS - 1u)));
				InterlockedAdd(s_Value[bin], 1u);
				if (u_LightingView.View == ST_LIGHTING_VIEW_ALBEDO && value < ST_ALBEDO_LOW)
					InterlockedAdd(s_Counts[2], 1u);
				else if (u_LightingView.View == ST_LIGHTING_VIEW_ALBEDO && value > ST_ALBEDO_HIGH)
					InterlockedAdd(s_Counts[3], 1u);
			}
		}
		else
		{
			float position = (GetPixelEV100(hdr) - ST_LIGHTING_VIEW_MIN_EV100) * ST_LIGHTING_VIEW_BINS_PER_EV;
			uint bin = uint(clamp(position, 0.0, float(ST_LIGHTING_VIEW_EV_BINS - 1u)));
			InterlockedAdd(s_EV[(surface ? 0u : ST_LIGHTING_VIEW_EV_BINS) + bin], 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint bin = thread; bin < 2u * ST_LIGHTING_VIEW_EV_BINS; bin += 256u)
	{
		if (s_EV[bin] > 0u)
			InterlockedAdd(u_Stats[ST_LIGHTING_STATS_SURFACE_EV + bin], s_EV[bin]);
	}
	if (thread < ST_LIGHTING_VIEW_VALUE_BINS && s_Value[thread] > 0u)
		InterlockedAdd(u_Stats[ST_LIGHTING_STATS_VALUE + thread], s_Value[thread]);
	if (thread < 4u && s_Counts[thread] > 0u)
		InterlockedAdd(u_Stats[ST_LIGHTING_STATS_SURFACE_PIXELS + thread], s_Counts[thread]);

	if (st_GlobalInvocationID.x == 0u && st_GlobalInvocationID.y == 0u)
	{
		// The irradiance map holds irradiance over pi per unit of the sky's intensity (the forward pass multiplies it by the
		// albedo and EnvironmentParams.x), the constant ambient is a radiance: both are a horizontal surface's illuminance over pi.
		float3 sky = u_Frame.EnvironmentParams.w > 0.0
			? u_IrradianceMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(float3(0.0, 1.0, 0.0)), 0.0).rgb * u_Frame.EnvironmentParams.x
			: u_Frame.AmbientColor.rgb;
		u_Stats[ST_LIGHTING_STATS_SKY_IRRADIANCE + 0u] = asuint(sky.r);
		u_Stats[ST_LIGHTING_STATS_SKY_IRRADIANCE + 1u] = asuint(sky.g);
		u_Stats[ST_LIGHTING_STATS_SKY_IRRADIANCE + 2u] = asuint(sky.b);
		u_Stats[ST_LIGHTING_STATS_SKY_IRRADIANCE + 3u] = 0u;
	}
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
