#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Infinite editor grid on the y = 0 plane, drawn over the finished image. Each pixel intersects its view ray with the
// plane and writes the intersection's depth, so scene geometry hides the grid; lines are anti-aliased with
// screen-space derivatives and fade out with distance and where they get denser than the pixels.

layout(push_constant) uniform GridParameters
{
	vec4 MinorColor;
	vec4 MajorColor;
	vec4 AxisXColor;    // The X axis (z = 0)
	vec4 AxisZColor;    // The Z axis (x = 0)
	float Spacing;      // World units between minor lines
	float MajorEvery;   // Minor lines per major line
	float FadeDistance; // The grid fades out between half this distance and this distance from the camera
	float Padding;
} u_Grid;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

// Depth bias toward the camera, in depth slopes per pixel: a surface in the grid's plane (a floor at y = 0) is rasterized
// with vertex positions snapped to the subpixel grid, so its depth differs from the exact plane by up to about a pixel's
// worth of slope, most at grazing angles. Geometry further than that above the plane still hides the grid.
const float c_DepthSlopeBias = 2.0;

// Coverage (0-1) of one pixel wide lines at whole grid coordinates.
float LineCoverage(vec2 coordinate, vec2 derivative)
{
	vec2 pixels = abs(fract(coordinate - 0.5) - 0.5) / max(derivative, vec2(1e-6));
	return 1.0 - min(min(pixels.x, pixels.y), 1.0);
}

// Non-premultiplied "over" compositing.
vec4 Over(vec4 top, vec4 bottom)
{
	float alpha = top.a + bottom.a * (1.0 - top.a);
	if (alpha <= 0.0)
		return vec4(0.0);
	return vec4((top.rgb * top.a + bottom.rgb * bottom.a * (1.0 - top.a)) / alpha, alpha);
}

void main()
{
	// The pixel's view ray (valid for perspective and orthographic projections) meets the plane at t.
	vec2 ndc = UVToNDC(v_UV);
	vec4 nearPoint = u_Frame.InverseViewProjection * vec4(ndc, 1.0, 1.0);
	vec4 farPoint = u_Frame.InverseViewProjection * vec4(ndc, 0.5, 1.0);
	vec3 origin = nearPoint.xyz / nearPoint.w;
	vec3 direction = farPoint.xyz / farPoint.w - origin;
	float t = -origin.y / direction.y;
	vec3 position = origin + direction * t;

	// Derivatives need uniform control flow: take them before discarding.
	vec2 coordinate = position.xz / u_Grid.Spacing;
	vec2 derivative = fwidth(coordinate);
	vec2 axisDistance = abs(position.zx) / max(fwidth(position.zx), vec2(1e-6)); // In pixels

	vec4 clip = u_Frame.ViewProjection * vec4(position, 1.0);
	float depth = clip.z / clip.w;
	// Neighbors whose rays miss the plane give no usable slope (NaN, infinite or the whole depth range).
	float depthSlope = fwidth(depth);
	depthSlope = depthSlope < 1.0 ? depthSlope : 0.0;
	if (!(t > 0.0) || !(depth > 0.0 && depth <= 1.0))
		discard;

	float minorFade = 1.0 - smoothstep(0.3, 0.6, max(derivative.x, derivative.y));
	float majorFade = 1.0 - smoothstep(0.3, 0.6, max(derivative.x, derivative.y) / u_Grid.MajorEvery);
	vec4 color = vec4(u_Grid.MinorColor.rgb, u_Grid.MinorColor.a * LineCoverage(coordinate, derivative) * minorFade);
	color = Over(vec4(u_Grid.MajorColor.rgb, u_Grid.MajorColor.a * LineCoverage(coordinate / u_Grid.MajorEvery, derivative / u_Grid.MajorEvery) * majorFade), color);
	color = Over(vec4(u_Grid.AxisZColor.rgb, u_Grid.AxisZColor.a * (1.0 - min(axisDistance.y, 1.0))), color);
	color = Over(vec4(u_Grid.AxisXColor.rgb, u_Grid.AxisXColor.a * (1.0 - min(axisDistance.x, 1.0))), color);

	float distanceToCamera = length(position - u_Frame.CameraPosition.xyz);
	color.a *= 1.0 - smoothstep(0.5 * u_Grid.FadeDistance, u_Grid.FadeDistance, distanceToCamera);
	if (color.a <= 0.0)
		discard;
	o_Color = color;
	gl_FragDepth = min(depth + depthSlope * c_DepthSlopeBias, 1.0); // Reversed-Z: larger is nearer
}
