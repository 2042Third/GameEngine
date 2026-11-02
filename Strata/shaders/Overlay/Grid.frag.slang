#include "Include/Common.slang"

#include "Include/Scene.slang"

// Infinite editor grid on the y = 0 plane, drawn over the finished image. Each pixel intersects its view ray with the
// plane and writes the intersection's depth, so scene geometry hides the grid; lines are anti-aliased with
// screen-space derivatives and fade out with distance and where they get denser than the pixels.

struct GridParameters
{
	float4 MinorColor;
	float4 MajorColor;
	float4 AxisXColor;    // The X axis (z = 0)
	float4 AxisZColor;    // The Z axis (x = 0)
	float Spacing;      // World units between minor lines
	float MajorEvery;   // Minor lines per major line
	float FadeDistance; // The grid fades out between half this distance and this distance from the camera
	float Padding;
};
[[vk::push_constant]] ConstantBuffer<GridParameters> u_Grid;

static float2 v_UV;
static float4 o_Color;

// Depth bias toward the camera, in depth slopes per pixel: a surface in the grid's plane (a floor at y = 0) is rasterized
// with vertex positions snapped to the subpixel grid, so its depth differs from the exact plane by up to about a pixel's
// worth of slope, most at grazing angles. Geometry further than that above the plane still hides the grid. With temporal
// anti-aliasing the scene's depth was drawn with the frame's jitter (JitterParams; the grid is not): its step is added.
static const float c_DepthSlopeBias = 2.0;

// Coverage (0-1) of one pixel wide lines at whole grid coordinates.
float LineCoverage(float2 coordinate, float2 derivative)
{
	float2 pixels = abs(frac(coordinate - 0.5) - 0.5) / max(derivative, float2(1e-6));
	return 1.0 - min(min(pixels.x, pixels.y), 1.0);
}

// Non-premultiplied "over" compositing.
float4 Over(float4 top, float4 bottom)
{
	float alpha = top.a + bottom.a * (1.0 - top.a);
	if (alpha <= 0.0)
		return float4(0.0);
	return float4((top.rgb * top.a + bottom.rgb * bottom.a * (1.0 - top.a)) / alpha, alpha);
}

void st_Main()
{
	// The pixel's view ray (valid for perspective and orthographic projections) meets the plane at t, in camera-relative
	// space (the plane lies at the camera's height below it); the lines are drawn at world coordinates.
	float2 ndc = UVToNDC(v_UV);
	float4 nearPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 0.5, 1.0));
	float3 origin = nearPoint.xyz / nearPoint.w;
	float3 direction = farPoint.xyz / farPoint.w - origin;
	float t = (-u_Frame.CameraPosition.y - origin.y) / direction.y;
	float3 relative = origin + direction * t;
	float3 position = relative + u_Frame.CameraPosition.xyz;

	// Derivatives need uniform control flow: take them before discarding.
	float2 coordinate = position.xz / u_Grid.Spacing;
	float2 derivative = fwidth(coordinate);
	float2 axisDistance = abs(position.zx) / max(fwidth(position.zx), float2(1e-6)); // In pixels

	float4 clip = mul(u_Frame.ViewProjection, float4(relative, 1.0));
	float depth = clip.z / clip.w;
	// Neighbors whose rays miss the plane give no usable slope (NaN, infinite or the whole depth range).
	float depthSlope = fwidth(depth);
	depthSlope = depthSlope < 1.0 ? depthSlope : 0.0;
	if (!(t > 0.0) || !(depth > 0.0 && depth <= 1.0))
		discard;

	float minorFade = 1.0 - smoothstep(0.3, 0.6, max(derivative.x, derivative.y));
	float majorFade = 1.0 - smoothstep(0.3, 0.6, max(derivative.x, derivative.y) / u_Grid.MajorEvery);
	float4 color = float4(u_Grid.MinorColor.rgb, u_Grid.MinorColor.a * LineCoverage(coordinate, derivative) * minorFade);
	color = Over(float4(u_Grid.MajorColor.rgb, u_Grid.MajorColor.a * LineCoverage(coordinate / u_Grid.MajorEvery, derivative / u_Grid.MajorEvery) * majorFade), color);
	color = Over(float4(u_Grid.AxisZColor.rgb, u_Grid.AxisZColor.a * (1.0 - min(axisDistance.y, 1.0))), color);
	color = Over(float4(u_Grid.AxisXColor.rgb, u_Grid.AxisXColor.a * (1.0 - min(axisDistance.x, 1.0))), color);

	float distanceToCamera = length(relative);
	color.a *= 1.0 - smoothstep(0.5 * u_Grid.FadeDistance, u_Grid.FadeDistance, distanceToCamera);
	if (color.a <= 0.0)
		discard;
	o_Color = color;
	float2 jitterPixels = abs(u_Frame.JitterParams.xy) * u_Frame.ViewportSize.xy * 0.5;
	st_FragDepth = min(depth + depthSlope * (c_DepthSlopeBias + max(jitterPixels.x, jitterPixels.y)), 1.0); // Reversed-Z: larger is nearer
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
	float st_FragDepth : SV_Depth;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_UV = input.v_UV;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	output.st_FragDepth = st_FragDepth;
	return output;
}
