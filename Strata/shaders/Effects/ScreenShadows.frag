#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Shadows.glsl"

// The sun's screen-space shadow mask (SceneRenderer::RenderScreenShadows): every opaque pixel of the depth prepass gets the
// cascades' filter (Include/Shadows.glsl) at the surface the forward pass shades there, from the prepass's depth and
// geometric normal, and the radius Effects/ScreenShadowsBlur.frag smooths it over: none where the fixed tent alone filtered
// (lit, umbra, narrow penumbrae), 0.35 of the penumbra's half-width in pixels where the blue-noise-turned disk did, enough
// to take its grain without widening the penumbra by more than about 6%. Output: red the square root of the visibility (8
// bits spent where a step would show, near the umbra), green the blur radius over c_ScreenShadowMaxBlurPixels. Opaque
// surfaces read the red (Scene/Forward.frag); transparent ones filter per pixel.

layout(set = 0, binding = ST_SRV(22)) uniform texture2D u_Depth;
layout(set = 0, binding = ST_SRV(23)) uniform texture2D u_Normals; // The prepass's view-space geometric normal, octahedral

layout(push_constant) uniform ScreenShadowParameters
{
	float ContactLength;
	uint ContactSteps;
	float ContactMaxPixels;
	float ContactFadeStart;
	float ContactFadeEnd;
	float ContactThickness;
	float ContactThicknessScale;
	uint Reserved;
} u_ScreenShadows;

layout(location = 0) out vec2 o_Mask;

const float c_ScreenShadowMaxBlurPixels = 8.0;

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(u_Depth, pixel, 0).r;
	if (!(depth > 0.0))
	{
		o_Mask = vec2(1.0, 0.0); // Nothing drawn
		return;
	}
	// The surface the forward pass shades at the pixel's center: the jittered matrices the prepass rasterized with.
	vec2 uv = (vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	vec3 position = point.xyz / point.w;
	float viewDepth = -(u_Frame.View * vec4(position, 1.0)).z;
	vec3 normal = normalize(mat3(u_Frame.InverseView) * OctahedralDecode(texelFetch(u_Normals, pixel, 0).xy));

	ShadowSample shadow = EvaluateDirectionalShadow(position, normal, viewDepth, pixel);
	float blur = 0.0;
	if (shadow.DiskWeight > 0.0 && shadow.Cascade >= 0)
	{
		float pixelWorld = 2.0 * (u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth) / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
		float radiusPixels = shadow.PenumbraTexels * u_Cascades[shadow.Cascade].Parameters.x / pixelWorld;
		blur = clamp(0.35 * radiusPixels * shadow.DiskWeight, 1.0, c_ScreenShadowMaxBlurPixels);
	}
	o_Mask = vec2(sqrt(clamp(shadow.Visibility, 0.0, 1.0)), blur / c_ScreenShadowMaxBlurPixels);
}
