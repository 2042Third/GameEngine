#ifndef STRATA_SHADOW_VIEW_GLSL
#define STRATA_SHADOW_VIEW_GLSL

// The shadows debug view (SceneRenderOptions::ShowShadowView): for each pixel the scene drew, the sun's cascade that shades it
// and how many screen pixels one of that cascade's shadow texels spans there. Overlay/ShadowView.frag colors pixels by it and
// Debug/ShadowViewStats.comp counts them, so the picture and the numbers describe the same pixels.
//
// The ratio is a texel's world size over a screen pixel's world size facing the camera at the receiver's view distance:
// independent of the surface's orientation, so it measures how fine the cascades are where the receivers are, not how a
// slope stretches the texels.

#include "Include/Shadows.glsl"

struct ShadowViewSample
{
	int Cascade;          // -1: no cascade covers the pixel (beyond the shadow distance, outside every square, no shadowed sun)
	float PixelsPerTexel; // Screen pixels per shadow texel
	float BlendWeight;    // How far the pixel is into its cascade's blend band (CascadeBlendWeight)
	vec2 ShadowUV;        // Its position in the cascade's shadow map
};

// The world size of a screen pixel facing the camera at a view distance.
float GetPixelWorldSize(float viewDepth)
{
	float distance = u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth;
	return 2.0 * distance / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
}

// The sample of the pixel at `pixel` whose depth (reversed-Z) is `depth`; false where nothing was drawn (depth 0) or no cascade
// covers the pixel (sample.Cascade -1).
bool GetShadowViewSample(ivec2 pixel, float depth, out ShadowViewSample viewSample)
{
	viewSample.Cascade = -1;
	viewSample.PixelsPerTexel = 0.0;
	viewSample.BlendWeight = 0.0;
	viewSample.ShadowUV = vec2(0.0);
	if (!(depth > 0.0))
		return false;

	// Camera-relative, as the shading reconstructs it.
	vec2 uv = (vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	vec3 position = point.xyz / point.w;
	float viewDepth = -(u_Frame.View * vec4(position, 1.0)).z;
	int cascade = SelectCascade(viewDepth);
	if (cascade < 0)
		return false;
	vec4 lightClip = u_Cascades[cascade].ViewProjection * vec4(position, 1.0);
	vec2 shadowUV = NDCToUV(lightClip.xy / lightClip.w);
	if (any(lessThan(shadowUV, vec2(0.0))) || any(greaterThan(shadowUV, vec2(1.0))))
		return false;

	viewSample.Cascade = cascade;
	viewSample.PixelsPerTexel = u_Cascades[cascade].Parameters.x / GetPixelWorldSize(viewDepth);
	viewSample.BlendWeight = CascadeBlendWeight(cascade, viewDepth);
	viewSample.ShadowUV = shadowUV;
	return true;
}

// The bin of a ratio (RenderData::c_ShadowViewBins eighths of a stop from 1/16 on; outside the range, the first or last bin).
int GetShadowViewBin(float pixelsPerTexel)
{
	return clamp(int(floor((log2(max(pixelsPerTexel, 1e-30)) + 4.0) * 8.0)), 0, 63);
}

#endif
