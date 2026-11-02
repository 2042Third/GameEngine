#include "Include/Common.slang"

#include "Include/Scene.slang"

// Screen-space reflections at RendererQualitySettings::Reflections Checkerboard (SceneRenderer, in GPU.Reflections, after the
// prepass; the Spatial modes: Effects/ScreenSpaceReflectionsSpatial.comp): for each texel of a half-resolution target,
// a ray from one pixel of its 2x2 block (each in turn, frame by frame) along the view reflected about the surface target's
// normal, marched in screen space against the frame's depth in c_Steps steps (offset by per-pixel noise that turns each frame)
// and refined by bisection. Where it passes behind a surface by less than its step's change of depth plus a margin, it hit
// it: the previous frame's resolved image (the TAA's history) is fetched where that surface was, along the hit pixel's
// motion, at the frame's pre-exposure. a: the confidence, which fades with roughness up to MaxRoughness, toward the screen's
// edges and for surfaces the ray meets from behind; rgb: the reflected luminance, pre-exposed and multiplied by the
// confidence, so that the forward pass's bilinear lookup blends a reflection's edges with what it replaces rather than with
// black. Rays that leave the screen, pass the far plane or meet nothing, and hits whose surface was outside the previous
// frame, have none: Forward.frag keeps the probes' and the sky's specular light there and mixes the reflection over it
// elsewhere. The TAA averages the noise of the frames.

// Slots above the scene set's (Include/Scene.glsl, whose frame constants and linear clamp sampler this pass binds too).
Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Surface : register(t23); // xy: octahedral view-space normal (normal-mapped), z: roughness
Texture2D<float4> u_Velocity : register(t24);
Texture2D<float4> u_History : register(t25);
Texture2D<float4> u_Normals : register(t26); // The prepass's geometric view-space normals (octahedral)
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Reflections : register(u0);

#include "Include/ScreenReflections.slang"

struct ReflectionParameters
{
	float Intensity;    // The confidence's scale
	float MaxRoughness; // Rougher surfaces reflect nothing; the confidence fades over the last quarter below it
	float HistoryScale; // The frame's pre-exposure over the history's
	uint Frame;         // The history's frame count: which pixel of each block traces
};
[[vk::push_constant]] ConstantBuffer<ReflectionParameters> u_Reflections;

static const uint c_Steps = 24u;
static const uint c_RefineSteps = 6u;

// Whether the ray's point (UV and depth) lies behind the depth buffer's surface there (reversed-Z: smaller is farther), and by
// how much (meters).
float GetDistanceBehind(float3 point, int2 size, out float sceneDepth)
{
	int2 pixel = clamp(int2(point.xy * u_Frame.ViewportSize.xy), int2(0), size - 1);
	sceneDepth = u_Depth.Load(int3(pixel, 0)).r;
	if (sceneDepth <= 0.0)
		return -1.0; // Nothing drawn (the sky): nothing to hit
	return GetViewDistance(point.z) - GetViewDistance(sceneDepth);
}

float4 TraceReflection(int2 pixel, int2 size)
{
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	if (depth <= 0.0)
		return float4(0.0);
	float4 surface = u_Surface.Load(int3(pixel, 0));
	float roughnessFade = clamp((u_Reflections.MaxRoughness - surface.z) / max(0.25 * u_Reflections.MaxRoughness, 1e-4), 0.0, 1.0);
	if (roughnessFade <= 0.0 || u_Reflections.Intensity <= 0.0)
		return float4(0.0);

	ScreenRay ray;
	if (!BeginScreenRay(pixel, depth, OctahedralDecode(surface.xy), ray))
		return float4(0.0);
	float3 start = ray.Start;
	float3 delta = ray.Delta;
	float pixels = ray.Pixels;

	// Steps closer together near the start (contact reflections), the first at least a pixel away (the surface itself).
	float noise = frac(InterleavedGradientNoise(float2(pixel)) + u_Frame.TemporalParams.z);
	float minStep = 1.0 / pixels;
	float previousStep = 0.0;
	float previousDistance = GetViewDistance(start.z);
	float hitStep = -1.0;
	for (uint index = 0u; index < c_Steps; index++)
	{
		float fraction = (float(index) + noise) / float(c_Steps);
		float along = max(fraction * fraction, minStep);
		float3 point = start + delta * along;
		float sceneDepth;
		float behind = GetDistanceBehind(point, size, sceneDepth);
		float rayDistance = GetViewDistance(point.z);
		float thickness = abs(rayDistance - previousDistance) + max(c_MinThickness, c_ThicknessScale * rayDistance);
		if (behind > 0.0 && behind < thickness)
		{
			hitStep = along;
			break;
		}
		previousStep = along;
		previousDistance = rayDistance;
	}
	if (hitStep < 0.0)
		return float4(0.0);
	// Where between the last point in front and the first behind the ray crosses the surface.
	float low = previousStep;
	float high = hitStep;
	for (uint index = 0u; index < c_RefineSteps; index++)
	{
		float middle = 0.5 * (low + high);
		float sceneDepth;
		if (GetDistanceBehind(start + delta * middle, size, sceneDepth) > 0.0)
			high = middle;
		else
			low = middle;
	}
	float2 hitUV = (start + delta * high).xy;
	int2 hitPixel = clamp(int2(hitUV * u_Frame.ViewportSize.xy), int2(0), size - 1);

	// The hit surface where the previous frame showed it.
	float2 previousUV = hitUV - u_Velocity.Load(int3(hitPixel, 0)).rg;
	if (any((previousUV < float2(0.0))) || any((previousUV > float2(1.0))))
		return float4(0.0);
	float3 color = u_History.SampleLevel(u_LinearClampSampler, previousUV, 0.0).rgb * u_Reflections.HistoryScale;
	float confidence = roughnessFade * GetHitConfidence(hitUV, hitPixel, ray.Direction) * min(u_Reflections.Intensity, 1.0);
	return float4(SanitizeHDR(color) * confidence, confidence);
}

void st_Main()
{
	int2 texel = int2(st_GlobalInvocationID.xy);
	int2 size = int2(u_Frame.ViewportSize.xy);
	int2 halfSize = (size + 1) / 2;
	if (texel.x >= halfSize.x || texel.y >= halfSize.y)
		return;
	// The pixel of the block traced this frame: each of the four in turn.
	int2 pixel = min(texel * 2 + int2(int(u_Reflections.Frame & 1u), int((u_Reflections.Frame >> 1u) & 1u)), size - 1);
	o_Reflections[texel] = TraceReflection(pixel, size);
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
