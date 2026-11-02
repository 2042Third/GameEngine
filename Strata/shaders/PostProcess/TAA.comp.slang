#include "Include/Common.slang"

#include "Include/Exposure.slang"
#include "Include/Motion.slang"

// Temporal anti-aliasing's resolve (SceneRenderer, in GPU.TAA): blends the frame's jittered HDR image with the renderer's
// history of earlier frames into the history's other texture. The history is fetched where the pixel's surface was in the
// previous frame - along the motion of the nearest surface in the pixel's 3x3 neighborhood, so an edge moves with its
// foreground, or the camera's where nothing was drawn - with a Catmull-Rom filter (a still pixel reads its own texel), then
// clamped to the colors of the neighborhood in this frame (in YCoCg, luma and chroma; with variance clipping, clipped toward
// their mean), which rejects what the history holds of surfaces that moved away or were uncovered. Both colors are weighted
// by their exposed luminance (1 / (1 + L)), so a bright sample among dark ones does not flicker as the jitter moves it. The
// current frame's weight is the largest of CurrentWeight (1 / the frames in the history: the frames after a reset are
// averaged evenly), MinWeight and a weight growing with the motion's length in pixels; a pixel whose surface was outside the
// previous frame (or, with disocclusion rejection, hidden in it) takes the current frame alone.
// The settings (TemporalAntiAliasingSettings) choose further terms, each in a branch on a push constant. The history's alpha
// holds the pixel's signed view distance (whatever the settings), which the next frame's resolve may compare with.

// Slots above the scene set's (Include/Scene.glsl, whose frame constants and linear clamp sampler this pass binds too).
Texture2D<float4> u_Current : register(t22);
Texture2D<float4> u_Depth : register(t23);
Texture2D<float4> u_Velocity : register(t24);
Texture2D<float4> u_History : register(t25);
StructuredBuffer<float> u_Exposure : register(t26);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_History : register(u0);

struct TemporalParameters
{
	float CurrentWeight;     // 1 / the frames the history holds with this one (1: the history is not read)
	float MinWeight;         // The current frame's least weight
	float MotionWeightScale; // The current frame's weight per pixel of motion...
	float MaxMotionWeight;   // ...up to this
	float HistoryScale;      // The frame's pre-exposure over the history's: the history at the frame's scale (exact)
	float InversePreExposure; // 1 / the frame's pre-exposure: the luminance weights are of exposed values
	uint ClampMode;           // TemporalHistoryClamp
	float ClipWidth;          // Variance clipping's half width, in standard deviations
	float TransparentWeight;  // The current frame's least weight under blended transparent surfaces; 0 = off
	float DisocclusionThreshold; // A surface nearer by this fraction of the distance in the previous frame covered the pixel; 0 = off
	float PreviousDepthOffset;   // dot(camera position - the previous camera's, PreviousForward.xyz)
	float ResampleWeight;        // The current frame's least weight halfway between the history's texels; 0 = off
	float4 PreviousForward;        // xyz: the previous camera's forward; w: the depth scale of the rejection's margin
};
[[vk::push_constant]] ConstantBuffer<TemporalParameters> u_Temporal;

// The history's alpha for the sky (nothing drawn), and the bound of every distance it holds: the largest half float.
static const float c_SkyDistance = 65504.0;

// The signed distance along the camera's forward axis of the surface at `uv` and `depth` (camera-relative, as the jittered
// raster drew it); the sky's for depth 0.
float GetViewDistance(float2 uv, float depth)
{
	if (depth <= 0.0)
		return c_SkyDistance;
	float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(uv), depth, 1.0));
	return clamp(dot(point.xyz / point.w, u_Frame.CameraForward.xyz), -c_SkyDistance, c_SkyDistance);
}

float3 RGBToYCoCg(float3 color)
{
	return float3(0.25 * color.r + 0.5 * color.g + 0.25 * color.b, 0.5 * color.r - 0.5 * color.b, -0.25 * color.r + 0.5 * color.g - 0.25 * color.b);
}

float3 YCoCgToRGB(float3 color)
{
	float base = color.x - color.z;
	return float3(base + color.y, color.x + color.z, base - color.y);
}

// A color in the space of the luminance weights (each divided by 1 + its exposed luminance), in YCoCg: there one bright
// texel (a glint) widens the spread of its neighborhood far less than it does in linear values.
float3 CompressColor(float3 color, float exposure)
{
	return RGBToYCoCg(color / (1.0 + Luminance(color) * exposure));
}

// The linear color of a compressed one: the inverse of CompressColor wherever its exposed luminance is below 1.
float3 ExpandColor(float3 compressed, float exposure)
{
	float3 color = YCoCgToRGB(compressed);
	return color / max(1.0 - exposure * Luminance(color), 1e-6);
}

// The history clipped toward the mean of the compressed colors of the pixel's 3x3 neighborhood, into a box `ClipWidth`
// standard deviations wide around the mean within their range. The history moves along the line toward the mean, so its hue
// relative to the mean is kept; a history inside the box is kept. The taps are fetched again (from the cache) after the
// history's filter rather than in the range clamp's loop: on Apple GPUs the moments gathered there, kept across the filter,
// cost more than the second fetches. The same fetches give the reactive mask's coverage (GetTransparentCoverage) in
// `coverage`.
float3 ClipHistoryToVariance(float3 history, int2 pixel, int2 size, float exposure, out float coverage)
{
	float3 sum = float3(0.0);
	float3 squares = float3(0.0);
	float3 low = float3(3.0e38);
	float3 high = float3(-3.0e38);
	coverage = 0.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			float4 texel = u_Current.Load(int3(clamp(pixel + int2(x, y), int2(0), size - 1), 0));
			coverage = max(coverage, 1.0 - texel.a);
			float3 compressed = CompressColor(texel.rgb, exposure);
			sum += compressed;
			squares += compressed * compressed;
			low = min(low, compressed);
			high = max(high, compressed);
		}
	}
	float3 mean = sum / 9.0;
	float3 deviation = sqrt(max(squares / 9.0 - mean * mean, float3(0.0)));
	float3 boxLow = max(mean - u_Temporal.ClipWidth * deviation, low);
	float3 boxHigh = min(mean + u_Temporal.ClipWidth * deviation, high);
	float3 offset = CompressColor(history, exposure) - mean;
	// Compared with 0, not an epsilon: compressed colors scale exactly with a power-of-two pre-exposure, so the same
	// components are chosen at every pre-exposure.
	float fraction = 1.0;
	for (int component = 0; component < 3; component++)
	{
		if (offset[component] != 0.0)
			fraction = min(fraction, ((offset[component] > 0.0 ? boxHigh[component] : boxLow[component]) - mean[component]) / offset[component]);
	}
	return ExpandColor(mean + max(fraction, 0.0) * offset, exposure);
}

// Whether the pixel's surface was uncovered since the previous frame: the history at `previousUV` stored (in its alpha) a
// distance along the previous camera's forward axis shorter than this surface's from that camera, by more than the
// threshold's fraction of it. The surface is the nearest of the neighborhood (whose motion the pixel takes), against the
// farthest of the four stored texels around the previous position: one-sided, so the jitter at still edges and the depth
// gradient of grazing surfaces are not mistaken for it. Uncovered sky counts; sky over sky does not.
bool IsDisoccluded(float2 previousUV, int2 nearestPixel, float nearestDepth)
{
	float expected = c_SkyDistance;
	if (nearestDepth > 0.0)
	{
		float2 nearestUV = (float2(nearestPixel) + 0.5) * u_Frame.ViewportSize.zw;
		float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(nearestUV), nearestDepth, 1.0));
		expected = min(dot(point.xyz / point.w, u_Temporal.PreviousForward.xyz) + u_Temporal.PreviousDepthOffset, c_SkyDistance);
	}
	float4 stored = u_History.GatherAlpha(u_LinearClampSampler, previousUV);
	float farthest = max(max(stored.x, stored.y), max(stored.z, stored.w));
	return farthest < expected - u_Temporal.DisocclusionThreshold * max(abs(expected), u_Temporal.PreviousForward.w);
}

// How much of the pixel's 3x3 neighborhood blended transparent surfaces cover: the largest of 1 - alpha, the HDR image's alpha
// being the transmittance through them (1 without them).
float GetTransparentCoverage(int2 pixel, int2 size)
{
	float coverage = 0.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
			coverage = max(coverage, 1.0 - u_Current.Load(int3(clamp(pixel + int2(x, y), int2(0), size - 1), 0)).a);
	}
	return coverage;
}

float3 SampleHistoryBilinear(float2 position)
{
	return u_History.SampleLevel(u_LinearClampSampler, position * u_Frame.ViewportSize.zw, 0.0).rgb;
}

// The history at `uv` through a Catmull-Rom filter of the 4x4 texels around it, in nine bilinear fetches: along each axis the
// two middle texels' weights are positive, so one fetch between them, placed by their ratio, reads both. `f` returns the
// position past the texel center at or before it, in [0, 1) along each axis.
float3 SampleHistory(float2 uv, out float2 f)
{
	float2 position = uv * u_Frame.ViewportSize.xy;
	float2 center = floor(position - 0.5) + 0.5; // The texel center at or before the position
	f = position - center;
	float2 weight0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	float2 weight1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	float2 weight2 = f * (0.5 + f * (2.0 - 1.5 * f));
	float2 weight3 = f * f * (-0.5 + 0.5 * f);
	float2 weight12 = weight1 + weight2;
	float2 position0 = center - 1.0;
	float2 position12 = center + weight2 / weight12;
	float2 position3 = center + 2.0;

	float3 color = SampleHistoryBilinear(float2(position0.x, position0.y)) * (weight0.x * weight0.y)
		+ SampleHistoryBilinear(float2(position12.x, position0.y)) * (weight12.x * weight0.y)
		+ SampleHistoryBilinear(float2(position3.x, position0.y)) * (weight3.x * weight0.y)
		+ SampleHistoryBilinear(float2(position0.x, position12.y)) * (weight0.x * weight12.y)
		+ SampleHistoryBilinear(float2(position12.x, position12.y)) * (weight12.x * weight12.y)
		+ SampleHistoryBilinear(float2(position3.x, position12.y)) * (weight3.x * weight12.y)
		+ SampleHistoryBilinear(float2(position0.x, position3.y)) * (weight0.x * weight3.y)
		+ SampleHistoryBilinear(float2(position12.x, position3.y)) * (weight12.x * weight3.y)
		+ SampleHistoryBilinear(float2(position3.x, position3.y)) * (weight3.x * weight3.y);
	// The filter's negative lobes may undershoot next to an edge.
	return max(color, float3(0.0));
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = int2(u_Frame.ViewportSize.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;

	float3 current = u_Current.Load(int3(pixel, 0)).rgb;
	float3 low = RGBToYCoCg(current);
	float3 high = low;
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	// The nearest surface (reversed-Z: the largest depth) of the neighborhood, the pixel's own on ties.
	float nearestDepth = depth;
	int2 nearestPixel = pixel;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			if (x == 0 && y == 0)
				continue;
			int2 neighbor = clamp(pixel + int2(x, y), int2(0), size - 1);
			float3 color = RGBToYCoCg(u_Current.Load(int3(neighbor, 0)).rgb);
			low = min(low, color);
			high = max(high, color);
			float neighborDepth = u_Depth.Load(int3(neighbor, 0)).r;
			if (neighborDepth > nearestDepth)
			{
				nearestDepth = neighborDepth;
				nearestPixel = neighbor;
			}
		}
	}

	float3 result = current;
	if (u_Temporal.CurrentWeight < 1.0)
	{
		// Nothing drawn in the neighborhood (the sky, which has no geometry): the camera's motion of its direction.
		float2 motion = nearestDepth > 0.0 ? u_Velocity.Load(int3(nearestPixel, 0)).rg : GetCameraMotion(uv, 0.0);
		float2 previousUV = uv - motion;
		// Outside the previous frame, or uncovered since (disocclusion rejection): the current frame alone.
		bool inside = all((previousUV >= float2(0.0))) && all((previousUV <= float2(1.0)));
		if (inside && !(u_Temporal.DisocclusionThreshold > 0.0 && IsDisoccluded(previousUV, nearestPixel, nearestDepth)))
		{
			// A still pixel reads its own texel; otherwise the filter reads the history between texels by `fraction`.
			float2 fraction = float2(0.0);
			float3 history = (all(motion == float2(0.0)) ? u_History.Load(int3(pixel, 0)).rgb : SampleHistory(previousUV, fraction)) * u_Temporal.HistoryScale;
			// The luminance weights' exposure: of exposed values.
			float exposure = u_Exposure[ST_EXPOSURE_MULTIPLIER] * u_Temporal.InversePreExposure;
			// The reactive mask's coverage: from variance clipping's fetches when it runs (negative: still to be found).
			float coverage = -1.0;
			if (u_Temporal.ClampMode == 1u)
				history = ClipHistoryToVariance(history, pixel, size, exposure, coverage);
			else
				history = YCoCgToRGB(clamp(RGBToYCoCg(history), low, high));
			float motionPixels = length(motion * u_Frame.ViewportSize.xy);
			float weight = max(max(u_Temporal.CurrentWeight, u_Temporal.MinWeight), min(motionPixels * u_Temporal.MotionWeightScale, u_Temporal.MaxMotionWeight));
			// Resampling: the filter softens what it reads between texels (most halfway, nothing at a texel), and a slow pan
			// reads every frame so; the current frame weighs more there, up to ResampleWeight halfway. Integer motion reads
			// texels and adds nothing.
			if (u_Temporal.ResampleWeight > 0.0)
			{
				float2 between = fraction * (1.0 - fraction);
				weight = max(weight, u_Temporal.ResampleWeight * 4.0 * max(between.x, between.y));
			}
			// Transparent surfaces (no motion of their own): the current frame weighs at least TransparentWeight where one covers
			// the neighborhood, a surface of alpha 0.5 or more fully (dilated over its aliased edges). Never a bypass: they would
			// alias and shimmer with the jitter.
			if (u_Temporal.TransparentWeight > 0.0)
			{
				if (coverage < 0.0)
					coverage = GetTransparentCoverage(pixel, size);
				weight = max(weight, min(1.0, 2.0 * coverage) * u_Temporal.TransparentWeight);
			}
			float currentWeight = weight / (1.0 + Luminance(current) * exposure);
			float historyWeight = (1.0 - weight) / (1.0 + Luminance(max(history, float3(0.0))) * exposure);
			result = (current * currentWeight + history * historyWeight) / max(currentWeight + historyWeight, 1e-6);
		}
	}
	o_History[pixel] = float4(SanitizeHDR(result), GetViewDistance(uv, depth));
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
