#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/ShadowView.glsl"

// The shadows debug view over the finished image (SceneRenderOptions::ShowShadowView): every receiver pixel colored by the
// screen pixels one shadow texel of its cascade spans there (Include/ShadowView.glsl), from Colors[0] at one pixel per texel
// or finer through Colors[1] at two to Colors[2] at four or coarser, linear in stops. Where a texel spans more than about 1.4
// pixels a checker of the shadow map's texels shows them; the colors darken with the sun's shadow (0.4 + 0.6 times the
// visibility of the frame's screen-space mask), so the view shows where the shadows fall on the texels; cascade edges are
// white and blend bands hatched. The pass cannot read the image it blends over: pixels no cascade covers get a gray veil,
// the sky the image darkened.

layout(set = 0, binding = ST_SRV(22)) uniform texture2D u_Depth;
// The sun's screen-space shadow mask (SceneRenderer::GetBoundScreenShadows: white without one), red the visibility's square
// root.
layout(set = 0, binding = ST_SRV(23)) uniform texture2D u_ShadowMask;

layout(push_constant) uniform ShadowViewParameters
{
	vec4 Colors[3];
} u_ShadowView;

layout(location = 0) out vec4 o_Color;

int CascadeAt(ivec2 pixel)
{
	ivec2 size = ivec2(u_Frame.ViewportSize.xy);
	if (any(greaterThanEqual(pixel, size)))
		return -1;
	ShadowViewSample neighbor;
	GetShadowViewSample(pixel, texelFetch(u_Depth, pixel, 0).r, neighbor);
	return neighbor.Cascade;
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(u_Depth, pixel, 0).r;
	if (!(depth > 0.0))
	{
		o_Color = vec4(0.0, 0.0, 0.0, 0.65); // The sky: the image at 35%
		return;
	}
	ShadowViewSample viewSample;
	if (!GetShadowViewSample(pixel, depth, viewSample))
	{
		o_Color = vec4(vec3(0.25), 0.65); // No cascade: the image at 35% under gray
		return;
	}

	float stops = log2(viewSample.PixelsPerTexel);
	float t = clamp(stops, 0.0, 2.0);
	vec3 color = t < 1.0 ? mix(u_ShadowView.Colors[0].rgb, u_ShadowView.Colors[1].rgb, t) : mix(u_ShadowView.Colors[1].rgb, u_ShadowView.Colors[2].rgb, t - 1.0);

	// The texels themselves, once they are coarse enough to see (no moire where they are finer than the pixels).
	float checkerStrength = clamp(stops - 0.5, 0.0, 1.0);
	ivec2 texel = ivec2(floor(viewSample.ShadowUV / u_Frame.ShadowParams.y));
	float checker = ((texel.x + texel.y) & 1) == 0 ? 1.0 : -1.0;
	color *= 1.0 + 0.12 * checkerStrength * checker;

	if (viewSample.BlendWeight > 0.0 && ((pixel.x + pixel.y) & 7) < 2)
		color *= 0.6;

	float visibility = texelFetch(u_ShadowMask, pixel, 0).r;
	color *= 0.4 + 0.6 * visibility * visibility;

	// Where the right or the lower neighbor is a receiver of another cascade (not at silhouettes against the sky or pixels no
	// cascade covers).
	int right = CascadeAt(pixel + ivec2(1, 0));
	int below = CascadeAt(pixel + ivec2(0, 1));
	if ((right >= 0 && right != viewSample.Cascade) || (below >= 0 && below != viewSample.Cascade))
		color = vec3(1.0);
	o_Color = vec4(clamp(color, vec3(0.0), vec3(1.0)), 1.0);
}
