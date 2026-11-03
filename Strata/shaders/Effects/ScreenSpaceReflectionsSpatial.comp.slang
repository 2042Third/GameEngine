#include "Include/Common.slang"

#include "Include/Scene.slang"

// Screen-space reflections at RendererQualitySettings::Reflections Spatial and SpatialHigh (SceneRenderer::
// TraceSpatialReflections, in GPU.Reflections, after the prepass): every pixel at full resolution, the same in every frame (no
// TAA needed). A ray along the view reflected about the surface target's normal is marched in screen space against the
// frame's depth in Steps steps over its whole length, closer together near its start; where it passed behind a surface by
// less than its step's change of distance plus a margin it hit it, refined by bisection. Its steps are offset by a fixed 4x4
// pattern (GTAO's order), so neighboring pixels sample between each other's steps and the resolve
// (Effects/ReflectionResolve.comp) averages them. (A hierarchical march over a nearest-depth pyramid ran out of steps on
// rays grazing a floor, whose nearer pixels below the ray fill every coarse cell: at 3840x2160 a polished floor reflected
// a wall as broken stripes.)
//
// What a ray sees is the previous frame's color before its transparent surfaces (the scene-color copy, made every frame at
// these qualities after the sky), fetched where the hit surface was along its motion vector and scaled to this frame's
// pre-exposure: last frame's light moved to where it is now, not an accumulation, so each frame's reflections are complete in
// themselves. Smooth surfaces read the copy at full resolution; rougher ones the half-resolution copy's blurred levels at
// roughness x (levels - 1), as glass does, blended over roughness 0.05 to 0.15. A frame without a previous copy (the first,
// a camera cut, a reset) is not traced. Output: rgb the reflected luminance (pre-exposed) times the confidence, a the
// confidence (as the Checkerboard trace's).

// Slots of this pass's own layout (its frame constants and linear clamp sampler at Include/Scene.slang's slots).
Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Surface : register(t23); // xy: octahedral view-space normal (normal-mapped), z: roughness
Texture2D<float4> u_Velocity : register(t24);
Texture2D<float4> u_PreviousColor : register(t25);       // Full resolution
Texture2D<float4> u_Normals : register(t26);             // The prepass's geometric view-space normals (octahedral)
Texture2D<float4> u_PreviousColorLevels : register(t27); // Half resolution, blurred levels
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Reflections : register(u0);

#include "Include/ScreenReflections.slang"

struct SpatialReflectionParameters
{
	float Intensity;     // The confidence's scale
	float MaxRoughness;  // Rougher surfaces reflect nothing; the confidence fades over the last quarter below it
	float HistoryScale;  // The frame's pre-exposure over the copy's
	uint Steps;          // Steps of the march along the ray
	uint RefineSteps;    // Bisections of a hit
	uint ColorLevels;    // Levels of u_PreviousColorLevels
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<SpatialReflectionParameters> u_Reflections;

// The 4x4 ordered-dither order (GTAO.comp's): every 4x4 block of pixels holds each of the 16 offsets once.
static const uint c_PatternOrder[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };
static const float c_SharpRoughness = 0.05; // Up to this roughness the full-resolution copy alone
static const float c_BlurredRoughness = 0.15; // From this roughness its blurred levels alone

// Whether the ray's point (UV and depth) lies behind the depth buffer's surface there (reversed-Z: smaller is farther).
bool IsBehindSurface(float3 point, int2 size)
{
	int2 pixel = clamp(int2(point.xy * u_Frame.ViewportSize.xy), int2(0), size - 1);
	float sceneDepth = u_Depth.Load(int3(pixel, 0)).r;
	return sceneDepth > 0.0 && point.z < sceneDepth;
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

	// Steps along t in [0, 1] (the ray's start to its end), closer together near the start (contact reflections), the first a
	// pixel or more away (the surface itself is not a hit). Every pixel of a 4x4 block starts at another of 16 offsets (GTAO's
	// order), so neighbors sample between each other's steps and the resolve averages them.
	uint2 phase = uint2(pixel) & 3u;
	float offset = (float(c_PatternOrder[phase.y * 4u + phase.x]) + 0.5) / 16.0;
	float minStep = 1.0 / ray.Pixels;
	float previousStep = 0.0;
	float previousDistance = GetViewDistance(ray.Start.z);
	float hitStep = -1.0;
	for (uint index = 0u; index < u_Reflections.Steps; index++)
	{
		float fraction = (float(index) + offset) / float(u_Reflections.Steps);
		float along = max(fraction * fraction, minStep);
		float3 point = ray.Start + ray.Delta * along;
		int2 samplePixel = clamp(int2(point.xy * u_Frame.ViewportSize.xy), int2(0), size - 1);
		float sceneDepth = u_Depth.Load(int3(samplePixel, 0)).r;
		float rayDistance = GetViewDistance(point.z);
		// A hit where the ray passed behind a surface by less than its step's change of distance plus the surface's margin (it
		// crossed the surface since its previous point); nothing drawn (the sky) is never hit.
		float thickness = abs(rayDistance - previousDistance) + max(c_MinThickness, c_ThicknessScale * rayDistance);
		float behind = sceneDepth > 0.0 ? rayDistance - GetViewDistance(sceneDepth) : -1.0;
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
	for (uint index = 0u; index < u_Reflections.RefineSteps; index++)
	{
		float middle = 0.5 * (low + high);
		if (IsBehindSurface(ray.Start + ray.Delta * middle, size))
			high = middle;
		else
			low = middle;
	}
	float2 hitUV = (ray.Start + ray.Delta * high).xy;
	int2 hitPixel = clamp(int2(hitUV * u_Frame.ViewportSize.xy), int2(0), size - 1);

	// The hit surface where the previous frame showed it.
	float2 previousUV = hitUV - u_Velocity.Load(int3(hitPixel, 0)).rg;
	if (any((previousUV < float2(0.0))) || any((previousUV > float2(1.0))))
		return float4(0.0);
	float3 sharp = u_PreviousColor.SampleLevel(u_LinearClampSampler, previousUV, 0.0).rgb;
	float colorLevel = surface.z * float(max(u_Reflections.ColorLevels, 1u) - 1u);
	float3 blurred = u_PreviousColorLevels.SampleLevel(u_LinearClampSampler, previousUV, colorLevel).rgb;
	float3 color = lerp(sharp, blurred, smoothstep(c_SharpRoughness, c_BlurredRoughness, surface.z)) * u_Reflections.HistoryScale;
	float confidence = roughnessFade * GetHitConfidence(hitUV, hitPixel, ray) * min(u_Reflections.Intensity, 1.0);
	return float4(SanitizeHDR(color) * confidence, confidence);
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = int2(u_Frame.ViewportSize.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;
	o_Reflections[pixel] = TraceReflection(pixel, size);
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
