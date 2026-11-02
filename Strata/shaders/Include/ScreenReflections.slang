#ifndef ST_SCREEN_REFLECTIONS_SLANG
#define ST_SCREEN_REFLECTIONS_SLANG

// What both screen-space reflection traces share (Effects/ScreenSpaceReflections.comp and ScreenSpaceReflectionsSpatial.comp):
// a reflected ray as a straight line in screen space, the depth's view distance, how thick a surface is taken to be, and the
// confidence of a hit. The includer declares u_Depth (the prepass's depth), u_Normals (its geometric view-space normals,
// octahedral) and Include/Scene.slang's frame constants first.

static const float c_FarDistance = 10000.0; // How far rays away from the camera reach before the screen's edges cut them
// How far behind a surface a ray may be at the first point behind it and still hit it: what the ray's depth changed by
// since its previous point (it crossed the surface in between), plus the larger of these margins (the surface's own depth
// changes between the two points' pixels) in meters and as a fraction of the ray's distance.
static const float c_MinThickness = 0.05;
static const float c_ThicknessScale = 0.02;
static const float c_EdgeFade = 0.1; // Hits within this fraction of the screen's size from its edges fade

// The view distance (along the camera's forward axis) of a depth: the inverse projection's z and w rows do not depend on x and
// y (the jitter only offsets them). Only for depths in front of the far plane (above 0, reversed-Z).
float GetViewDistance(float depth)
{
	float4x4 inverse = u_Frame.InverseProjection;
	return -(transpose(inverse)[2].z * depth + transpose(inverse)[3].z) / (transpose(inverse)[2].w * depth + transpose(inverse)[3].w);
}

// A reflected ray in screen space: UV and depth at its start and their change to its end (the screen's edge, the far plane
// or the near plane, whichever it meets first; the depth changes linearly along it), its length in pixels, and its view-space
// direction.
struct ScreenRay
{
	float3 Start;
	float3 Delta;
	float Pixels;
	float3 Direction;
};

// The ray reflected at a pixel (its depth, above 0) about a view-space normal: false when it goes into the geometric surface
// (a normal map turned it so), reaches nowhere, or spans less than a pixel.
bool BeginScreenRay(int2 pixel, float depth, float3 normal, out ScreenRay ray)
{
	ray.Start = float3(0.0);
	ray.Delta = float3(0.0);
	ray.Pixels = 0.0;
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float4 viewPoint = mul(u_Frame.InverseProjection, float4(UVToNDC(uv), depth, 1.0));
	float3 position = viewPoint.xyz / viewPoint.w;
	bool orthographic = u_Frame.CameraForward.w > 0.5;
	float3 incident = orthographic ? float3(0.0, 0.0, -1.0) : normalize(position);
	float3 direction = reflect(incident, normal);
	ray.Direction = direction;
	// A normal map may turn the reflection into the surface itself: nothing in front of it to reflect.
	if (dot(direction, OctahedralDecode(TexelFetch(u_Normals, pixel, 0).rg)) <= 0.0)
		return false;

	// The ray's far end: far away from the camera, or (toward it) just before the near plane.
	float rayLength = c_FarDistance;
	if (direction.z > 1e-6)
		rayLength = min(rayLength, 0.999 * (-u_Frame.CameraPosition.w - position.z) / direction.z);
	if (rayLength <= 0.0)
		return false;
	float4 startClip = mul(u_Frame.Projection, float4(position, 1.0));
	float4 endClip = mul(u_Frame.Projection, float4(position + direction * rayLength, 1.0));
	// In screen space (UV and depth) the ray is a straight line along which the depth changes linearly.
	float3 start = float3(NDCToUV(startClip.xy / startClip.w), startClip.z / startClip.w);
	float3 delta = float3(NDCToUV(endClip.xy / endClip.w), endClip.z / endClip.w) - start;
	// Cut where it leaves the screen or passes the far plane (depth 0).
	float extent = 1.0;
	for (int axis = 0; axis < 2; axis++)
	{
		if (delta[axis] > 0.0)
			extent = min(extent, (1.0 - start[axis]) / delta[axis]);
		else if (delta[axis] < 0.0)
			extent = min(extent, -start[axis] / delta[axis]);
	}
	if (delta.z < 0.0)
		extent = min(extent, -start.z / delta.z);
	delta *= max(extent, 0.0);
	ray.Start = start;
	ray.Delta = delta;
	ray.Pixels = length(delta.xy * u_Frame.ViewportSize.xy);
	return ray.Pixels >= 1.0;
}

// How much a hit at `hitUV` (pixel `hitPixel`) of a ray along `direction` is believed: the depth buffer holds front faces
// only, so a surface the ray meets from behind is not what it would see, and hits near the screen's edges fade.
float GetHitConfidence(float2 hitUV, int2 hitPixel, float3 direction)
{
	float facing = clamp(-4.0 * dot(OctahedralDecode(TexelFetch(u_Normals, hitPixel, 0).rg), direction), 0.0, 1.0);
	float2 edges = min(hitUV, 1.0 - hitUV);
	float edgeFade = clamp(min(edges.x, edges.y) / c_EdgeFade, 0.0, 1.0);
	return facing * edgeFade;
}

#endif
