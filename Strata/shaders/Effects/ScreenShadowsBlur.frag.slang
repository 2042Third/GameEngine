#include "Include/Common.slang"

#include "Include/Scene.slang"

// One direction of the sun's screen-space shadow mask's blur (SceneRenderer::RenderScreenShadows, run horizontally then
// vertically): each pixel whose radius (the mask's green, Effects/ScreenShadows.frag) is at least half a pixel takes a
// Gaussian of its taps along the direction, weighted by how close each tap lies to the pixel's own plane (exact for a plane at
// any angle, so grazing floors keep their weight, while another surface a percent of the distance away gets none: no light
// crosses a silhouette) and how alike their normals are. Pixels without a radius (lit, umbra, narrow penumbrae) keep their
// value exactly. The blur averages visibility, not its square-root encoding; the green passes the pixel's radius on.

Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Normals : register(t23); // The prepass's view-space geometric normal, octahedral
Texture2D<float4> u_Source : register(t24);

struct ScreenShadowBlurParameters
{
	int2 Direction;
	float PlaneTolerance; // Of the view distance
	uint Reserved;
};
[[vk::push_constant]] ConstantBuffer<ScreenShadowBlurParameters> u_Blur;

static float2 o_Mask;

static const float c_ScreenShadowMaxBlurPixels = 8.0;

float3 NormalAt(int2 pixel)
{
	return OctahedralDecode(u_Normals.Load(int3(pixel, 0)).xy); // View space: only compared with each other
}

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float2 center = u_Source.Load(int3(pixel, 0)).rg;
	float radius = center.g * c_ScreenShadowMaxBlurPixels;
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	if (radius < 0.5 || !(depth > 0.0))
	{
		o_Mask = center;
		return;
	}
	// The camera-relative position the depth holds, as the shading reconstructs it.
	float2 ndc = UVToNDC((float2(pixel) + 0.5) * u_Frame.ViewportSize.zw);
	float4 point = mul(u_Frame.InverseViewProjection, float4(ndc, depth, 1.0));
	float3 position = point.xyz / point.w;
	float3 normal = NormalAt(pixel);
	float3 worldNormal = mul(((float3x3)(u_Frame.InverseView)), normal);
	float tolerance = u_Blur.PlaneTolerance * max(-(mul(u_Frame.View, float4(position, 1.0))).z, 1e-3);
	float sigma = max(radius * 0.5, 0.5);
	int taps = min(int(ceil(radius)), int(c_ScreenShadowMaxBlurPixels));
	int2 last = int2(u_Frame.ViewportSize.xy) - 1;

	// A tap's distance from the pixel's plane, n . (p' - p), without reconstructing its position: with p' the projection of
	// x = (ndc', depth', 1) through InverseViewProjection M, it is dot(plane, M x) / (M x).w for plane = (n, -n . p), that is
	// dot(M^T plane, x) / dot(w row of M, x), and along the blur's direction both are linear in the tap's offset and depth.
	float4x4 toWorld = u_Frame.InverseViewProjection;
	float4 plane = mul(transpose(toWorld), float4(worldNormal, -dot(worldNormal, position)));
	float4 rowW = float4(toWorld[3][0], toWorld[3][1], toWorld[3][2], toWorld[3][3]);
	float2 ndcStep = float2(u_Blur.Direction) * float2(2.0, -2.0) * u_Frame.ViewportSize.zw; // One pixel along the direction
	float planeBase = dot(plane.xy, ndc) + plane.w;
	float planeStep = dot(plane.xy, ndcStep);
	float wBase = dot(rowW.xy, ndc) + rowW.w;
	float wStep = dot(rowW.xy, ndcStep);

	float weightSum = 1.0;
	float visibilitySum = center.r * center.r;
	for (int tap = -taps; tap <= taps; tap++)
	{
		if (tap == 0)
			continue;
		int2 other = pixel + tap * u_Blur.Direction;
		if (any((other < int2(0))) || any((other > last)))
			continue;
		float otherDepth = u_Depth.Load(int3(other, 0)).r;
		if (!(otherDepth > 0.0))
			continue;
		float offset = float(tap);
		float distance = (planeBase + offset * planeStep + plane.z * otherDepth) / (wBase + offset * wStep + rowW.z * otherDepth);
		float onPlane = clamp(1.0 - abs(distance) / tolerance, 0.0, 1.0);
		float facing = max(dot(normal, NormalAt(other)), 0.0);
		facing *= facing;
		facing *= facing;
		facing *= facing; // The eighth power
		float weight = exp(-offset * offset / (2.0 * sigma * sigma)) * onPlane * facing;
		if (weight <= 0.0)
			continue;
		float encoded = u_Source.Load(int3(other, 0)).r;
		visibilitySum += weight * encoded * encoded;
		weightSum += weight;
	}
	o_Mask = float2(sqrt(clamp(visibilitySum / weightSum, 0.0, 1.0)), center.g);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float2 o_Mask : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Mask = o_Mask;
	return output;
}
