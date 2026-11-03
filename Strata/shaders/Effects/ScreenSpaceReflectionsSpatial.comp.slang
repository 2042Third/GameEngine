#include "Include/Common.slang"

#include "Include/Scene.slang"

// Screen-space reflections at RendererQualitySettings::Reflections Spatial and SpatialHigh (SceneRenderer::
// TraceSpatialReflections, in GPU.Reflections, after the prepass): every pixel at full resolution, the same in every frame (no
// TAA needed). A ray along the view reflected about the surface target's normal is marched in screen space against the
// frame's depth in Steps steps over its whole length, closer together near its start; a step whose segment crossed a
// surface's depth is a candidate, refined by bisection and kept only when the crossing is valid (behind its own surface by
// no more than a tolerance, on one continuous surface), else the march goes on. Its steps are offset by a fixed 4x4
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
	uint MaxCandidates;  // Refined candidates a ray may reject before it finds nothing
	uint Padding;
};
[[vk::push_constant]] ConstantBuffer<SpatialReflectionParameters> u_Reflections;

// The 4x4 ordered-dither order (GTAO.comp's): every 4x4 block of pixels holds each of the 16 offsets once.
static const uint c_PatternOrder[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };
static const float c_SharpRoughness = 0.05; // Up to this roughness the full-resolution copy alone
static const float c_BlurredRoughness = 0.15; // From this roughness its blurred levels alone

static const float c_SkyDistance = 1.0e30; // The depth buffer's 0 (nothing drawn): nothing to cross

// A point of the march: where along the ray (0 to 1), the view distance of the ray's point and that of the depth buffer's
// surface at its pixel (c_SkyDistance where nothing was drawn).
struct MarchPoint
{
	float Along;
	float RayDistance;
	float SceneDistance;
};

float SceneDistanceAt(float3 point, int2 size)
{
	int2 pixel = clamp(int2(point.xy * u_Frame.ViewportSize.xy), int2(0), size - 1);
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	return depth > 0.0 ? GetViewDistance(depth) : c_SkyDistance;
}

MarchPoint MarchPointAt(ScreenRay ray, float along, int2 size)
{
	float3 point = ray.Start + ray.Delta * along;
	MarchPoint result;
	result.Along = along;
	result.RayDistance = GetViewDistance(point.z);
	result.SceneDistance = SceneDistanceAt(point, size);
	return result;
}

// The thickness tolerance of a surface at a view distance: what the march assumes lies behind a depth.
float GetThickness(float distance)
{
	return max(c_MinThickness, c_ThicknessScale * distance);
}

// The view-space position of the ray's point `along` it.
float3 GetRayPosition(ScreenRay ray, float along)
{
	float3 point = ray.Start + ray.Delta * along;
	float4 position = mul(u_Frame.InverseProjection, float4(UVToNDC(point.xy), point.z, 1.0));
	return position.xyz / position.w;
}

// Bisects a segment along which the ray passes behind the plane of a surface it was in front of (a line crosses a plane once)
// and keeps the crossing where the depth buffer shows a surface at the ray's depth: that surface, not beyond its edge (the sky,
// or what lies far behind a post's side) and not hidden behind something nearer. Where along the ray the hit is, -1 if none.
float RefinePlaneCrossing(ScreenRay ray, MarchPoint low, MarchPoint high, float3 planePoint, float3 planeNormal, int2 size)
{
	for (uint index = 0u; index < u_Reflections.RefineSteps; index++)
	{
		float along = 0.5 * (low.Along + high.Along);
		MarchPoint middle = MarchPointAt(ray, along, size);
		if (dot(GetRayPosition(ray, along) - planePoint, planeNormal) < 0.0)
			high = middle;
		else
			low = middle;
	}
	float interval = abs(high.RayDistance - low.RayDistance);
	if (high.SceneDistance < c_SkyDistance && abs(high.RayDistance - high.SceneDistance) <= GetThickness(high.SceneDistance) + interval)
		return high.Along;
	if (low.SceneDistance < c_SkyDistance && abs(low.RayDistance - low.SceneDistance) <= GetThickness(low.SceneDistance) + interval)
		return low.Along;
	return -1.0;
}

// Whether the ray at a point is behind its pixel's surface by more than the thickness tolerance: a surface it passes behind.
bool IsPassingBehind(MarchPoint point)
{
	return point.SceneDistance < c_SkyDistance && point.RayDistance > point.SceneDistance + GetThickness(point.SceneDistance);
}

// Bisects a segment from a point in front of a surface (low) to one behind it (high) onto the crossing. A point over the sky
// counts as behind while the low end is on a surface it is not passing behind (the crossing, or that surface's edge, lies
// before it) and as in front otherwise. Comparing it with the low end's depth instead would lose the crossing at a reflected
// wall's top edge: the depth of a wall seen from below its top shrinks toward the top, so a point just beyond the edge is
// nearer than the wall's depth further down where the low end was. While the low end is behind an occluder, a point on a
// surface nearer than the receding ray already was there counts as in front: the ray passes behind that surface and cannot
// have crossed it since.
void BisectCrossing(ScreenRay ray, inout MarchPoint low, inout MarchPoint high, int2 size)
{
	for (uint index = 0u; index < u_Reflections.RefineSteps; index++)
	{
		MarchPoint middle = MarchPointAt(ray, 0.5 * (low.Along + high.Along), size);
		bool behind;
		if (middle.SceneDistance >= c_SkyDistance)
			behind = low.SceneDistance < c_SkyDistance && !IsPassingBehind(low);
		else
		{
			bool passedBefore = IsPassingBehind(low) && middle.RayDistance >= low.RayDistance
				&& middle.SceneDistance + GetThickness(middle.SceneDistance) < low.RayDistance;
			behind = middle.RayDistance > middle.SceneDistance && !passedBefore;
		}
		if (behind)
			high = middle;
		else
			low = middle;
	}
}

// Where along the ray a bisected crossing is, -1 if it is not valid. The hit must be on a surface, behind it by no more than
// the thickness tolerance plus the last interval's change of the ray's distance, and on one continuous surface (the two ends'
// depths as close): a ray that passed behind a thin post fails (its ends sit on the post and on what is far behind it), one
// that met a wall passes. Where the low end is itself behind an occluder, the surface the ray met is hidden there and only the
// high end is checked. Where the interval ends on a surface's last pixel and the sky beyond it, the ray met that surface at
// its edge if it had passed its depth there.
float ValidateCrossing(MarchPoint low, MarchPoint high)
{
	float interval = abs(high.RayDistance - low.RayDistance);
	if (high.SceneDistance >= c_SkyDistance)
	{
		if (low.SceneDistance >= c_SkyDistance || IsPassingBehind(low))
			return -1.0;
		float edgeSlack = GetThickness(low.SceneDistance) + interval;
		return high.RayDistance > low.SceneDistance && high.RayDistance - low.SceneDistance <= edgeSlack ? low.Along : -1.0;
	}
	float slack = GetThickness(high.SceneDistance) + interval;
	bool valid = high.RayDistance > high.SceneDistance && high.RayDistance - high.SceneDistance <= slack
		&& (IsPassingBehind(low) || abs(high.SceneDistance - low.SceneDistance) <= slack);
	return valid ? high.Along : -1.0;
}

// Refines a candidate segment and validates the crossing: where along the ray the hit is, -1 for a false candidate. When the
// bisection closed in on an occluder inside the segment (a thin post between two points on the wall behind it: the ray passes
// behind the post), what lies beyond the occluder is bisected once more from there, so the ray still meets the wall; a segment
// that starts behind an occluder (after a rejected candidate) passes over it the same way.
float RefineCrossing(ScreenRay ray, MarchPoint low, MarchPoint high, int2 size)
{
	MarchPoint segmentEnd = high;
	BisectCrossing(ray, low, high, size);
	float hitAlong = ValidateCrossing(low, high);
	if (hitAlong >= 0.0 || !IsPassingBehind(high) || high.Along >= segmentEnd.Along)
		return hitAlong;
	low = high;
	high = segmentEnd;
	BisectCrossing(ray, low, high, size);
	return ValidateCrossing(low, high);
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
	// A candidate is a step whose segment from the previous point crossed the surface at either end's pixel: in front of its
	// depth at the previous point, behind it at this one. Of the previous pixel's surface it is its plane (the geometric
	// normal's) that must be crossed: that finds the crossing between two samples on it (a reflected wall's top edge, the
	// face of a post narrower than a step), while a ray running in front of a receding floor, beyond each earlier pixel's depth,
	// crosses nothing. A false candidate does not end the march, up to MaxCandidates of them; after that the ray finds nothing
	// (the probes or the sky fill in), which bounds the cost in clutter. After its Steps steps the march takes one more point,
	// half a pixel before the ray's end: the offset steps stop short of the end by up to a step, and on a ray toward the far plane
	// that last step spans everything beyond some tens of meters (a far wall, or the wall behind a post whose image the last step
	// landed on, would show the sky).
	MarchPoint previous;
	previous.Along = 0.0;
	previous.RayDistance = GetViewDistance(ray.Start.z);
	previous.SceneDistance = c_SkyDistance; // The start crosses no surface of its own
	uint candidates = 0u;
	float hitAlong = -1.0;
	for (uint index = 0u; index <= u_Reflections.Steps && hitAlong < 0.0; index++)
	{
		float fraction = (float(index) + offset) / float(u_Reflections.Steps);
		float along = index < u_Reflections.Steps ? max(fraction * fraction, minStep) : 1.0 - 0.5 * minStep;
		if (index == u_Reflections.Steps && along <= previous.Along)
			break; // A short ray's last step is already past that point
		MarchPoint current = MarchPointAt(ray, along, size);
		bool candidate = false;
		if (previous.RayDistance <= current.SceneDistance && current.RayDistance > current.SceneDistance)
		{
			candidate = true;
			hitAlong = RefineCrossing(ray, previous, current, size);
		}
		else if (previous.SceneDistance < c_SkyDistance && previous.RayDistance <= previous.SceneDistance
			&& current.RayDistance > previous.SceneDistance - GetThickness(previous.SceneDistance))
		{
			float2 previousUV = (ray.Start + ray.Delta * previous.Along).xy;
			int2 previousPixel = clamp(int2(previousUV * u_Frame.ViewportSize.xy), int2(0), size - 1);
			float4 planeView = mul(u_Frame.InverseProjection, float4(UVToNDC((float2(previousPixel) + 0.5) * u_Frame.ViewportSize.zw), u_Depth.Load(int3(previousPixel, 0)).r, 1.0));
			float3 planePoint = planeView.xyz / planeView.w;
			float3 planeNormal = OctahedralDecode(TexelFetch(u_Normals, previousPixel, 0).rg);
			if (dot(GetRayPosition(ray, current.Along) - planePoint, planeNormal) < 0.0)
			{
				candidate = true;
				hitAlong = RefinePlaneCrossing(ray, previous, current, planePoint, planeNormal, size);
			}
		}
		if (candidate && hitAlong < 0.0)
		{
			candidates++;
			if (candidates >= u_Reflections.MaxCandidates)
				break;
		}
		previous = current;
	}
	if (hitAlong < 0.0)
		return float4(0.0);
	float2 hitUV = (ray.Start + ray.Delta * hitAlong).xy;
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
