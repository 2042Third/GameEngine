#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Scene.glsl"

// One direction of the sun's screen-space shadow mask's blur (SceneRenderer::RenderScreenShadows, run horizontally then
// vertically): each pixel whose radius (the mask's green, Effects/ScreenShadows.frag) is at least half a pixel takes a
// Gaussian of its taps along the direction, weighted by how close each tap lies to the pixel's own plane (exact for a plane at
// any angle, so grazing floors keep their weight, while another surface a percent of the distance away gets none: no light
// crosses a silhouette) and how alike their normals are. Pixels without a radius (lit, umbra, narrow penumbrae) keep their
// value exactly. The blur averages visibility, not its square-root encoding; the green passes the pixel's radius on.

layout(set = 0, binding = ST_SRV(22)) uniform texture2D u_Depth;
layout(set = 0, binding = ST_SRV(23)) uniform texture2D u_Normals; // The prepass's view-space geometric normal, octahedral
layout(set = 0, binding = ST_SRV(24)) uniform texture2D u_Source;

layout(push_constant) uniform ScreenShadowBlurParameters
{
	ivec2 Direction;
	float PlaneTolerance; // Of the view distance
	uint Reserved;
} u_Blur;

layout(location = 0) out vec2 o_Mask;

const float c_ScreenShadowMaxBlurPixels = 8.0;

vec3 PositionAt(ivec2 pixel, float depth)
{
	vec2 uv = (vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	return point.xyz / point.w;
}

vec3 NormalAt(ivec2 pixel)
{
	return OctahedralDecode(texelFetch(u_Normals, pixel, 0).xy); // View space: only compared with each other
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 center = texelFetch(u_Source, pixel, 0).rg;
	float radius = center.g * c_ScreenShadowMaxBlurPixels;
	float depth = texelFetch(u_Depth, pixel, 0).r;
	if (radius < 0.5 || !(depth > 0.0))
	{
		o_Mask = center;
		return;
	}
	vec3 position = PositionAt(pixel, depth);
	vec3 normal = NormalAt(pixel);
	vec3 worldNormal = mat3(u_Frame.InverseView) * normal;
	float tolerance = u_Blur.PlaneTolerance * max(-(u_Frame.View * vec4(position, 1.0)).z, 1e-3);
	float sigma = max(radius * 0.5, 0.5);
	int taps = min(int(ceil(radius)), int(c_ScreenShadowMaxBlurPixels));
	ivec2 last = ivec2(u_Frame.ViewportSize.xy) - 1;

	float weightSum = 1.0;
	float visibilitySum = center.r * center.r;
	for (int tap = -taps; tap <= taps; tap++)
	{
		if (tap == 0)
			continue;
		ivec2 other = pixel + tap * u_Blur.Direction;
		if (any(lessThan(other, ivec2(0))) || any(greaterThan(other, last)))
			continue;
		float otherDepth = texelFetch(u_Depth, other, 0).r;
		if (!(otherDepth > 0.0))
			continue;
		float plane = clamp(1.0 - abs(dot(worldNormal, PositionAt(other, otherDepth) - position)) / tolerance, 0.0, 1.0);
		float facing = pow(max(dot(normal, NormalAt(other)), 0.0), 8.0);
		float weight = exp(-float(tap * tap) / (2.0 * sigma * sigma)) * plane * facing;
		if (weight <= 0.0)
			continue;
		float encoded = texelFetch(u_Source, other, 0).r;
		visibilitySum += weight * encoded * encoded;
		weightSum += weight;
	}
	o_Mask = vec2(sqrt(clamp(visibilitySum / weightSum, 0.0, 1.0)), center.g);
}
