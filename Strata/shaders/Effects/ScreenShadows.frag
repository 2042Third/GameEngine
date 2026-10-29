#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Shadows.glsl"

// The sun's screen-space shadow mask (SceneRenderer::RenderScreenShadows): every opaque pixel of the depth prepass gets the
// cascades' filter (Include/Shadows.glsl) at the surface the forward pass shades there, from the prepass's depth and
// geometric normal, and the radius Effects/ScreenShadowsBlur.frag smooths it over: none where the filter left early (lit,
// umbra) or the fixed tent alone filtered, 0.35 of the penumbra's half-width in pixels where the blue-noise-turned disk did,
// enough to take its grain without widening the penumbra by more than about 6%, and a stretched texel's footprint where the
// tent left an edge partly lit on a receiver the light grazes (ReceiverTexelPixels), whose stair steps it takes. Output:
// red the square root of the visibility (8
// bits spent where a step would show, near the umbra), green the blur radius over c_ScreenShadowMaxBlurPixels. Opaque
// surfaces read the red (Scene/Forward.frag); transparent ones filter per pixel. Contact shadows follow in their own pass
// (Effects/ContactShadows.frag).

layout(set = 0, binding = ST_SRV(22)) uniform texture2D u_Depth;
layout(set = 0, binding = ST_SRV(23)) uniform texture2D u_Normals; // The prepass's view-space geometric normal, octahedral

layout(location = 0) out vec2 o_Mask;

const float c_ScreenShadowMaxBlurPixels = 8.0;
// The tent smooths a texel's stair step over a texel and a half each way of the light's image, which keeps the steps from
// showing where the receiver holds the texels square. One the light grazes (|n.L| below c_StairFacing: at under 30 degrees)
// stretches them across the edge at least twice: there the steps show once a stretched texel covers more screen pixels than
// c_StairTexelPixels.
const float c_StairFacing = 0.5;
const float c_StairTexelPixels = 1.5;
// The stair steps' radius at most: every pixel of radius costs the blur two taps a pass, and 4 takes the steps nearly as
// well as 8 (the Open World's pine tiers) at about half the cost.
const float c_StairMaxBlurPixels = 4.0;

// How many screen pixels one texel of a cascade (`worldPerTexel`) covers at most on a receiver's plane: a step of a texel
// across the light slides along the plane by 1 / |n.L| in the plane's steepest direction (and by 1 across it), and shows on
// screen as its part across the pixel's view ray. A receiver the light grazes stretches the texels, and with them the
// stair steps of every edge the tent filters on it.
float ReceiverTexelPixels(vec3 position, vec3 normal, float worldPerTexel, float pixelWorld)
{
	vec3 light = u_Frame.DirectionalLightDirection.xyz;
	float facing = dot(normal, light);
	vec3 steepest = normal - light * facing; // Across the light, in the plane of the normal and the light
	float steepLength = length(steepest);
	if (!(steepLength > 1e-4))
		return worldPerTexel / pixelWorld; // Facing the light: texels lie on the plane unstretched
	steepest /= steepLength;
	float limitedFacing = facing >= 0.0 ? max(facing, 0.05) : min(facing, -0.05); // At most 20 times stretched
	vec3 along = steepest - light * (steepLength / limitedFacing); // A texel's step across the light, slid onto the plane
	vec3 across = cross(light, steepest);
	vec3 view = GetViewVector(position);
	float stretch = max(length(along - view * dot(along, view)), length(across - view * dot(across, view)));
	return worldPerTexel * stretch / pixelWorld;
}

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
	bool partlyLit = shadow.Visibility > 0.0 && shadow.Visibility < 1.0;
	if (shadow.Cascade >= 0 && (shadow.DiskWeight > 0.0 || partlyLit))
	{
		float pixelWorld = 2.0 * (u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth) / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
		float worldPerTexel = u_Cascades[shadow.Cascade].Parameters.x;
		if (shadow.DiskWeight > 0.0)
		{
			float radiusPixels = shadow.PenumbraTexels * worldPerTexel / pixelWorld;
			blur = clamp(0.35 * radiusPixels * shadow.DiskWeight, 1.0, c_ScreenShadowMaxBlurPixels);
		}
		// On a receiver the light grazes, the tent's edges show the texels' stair steps (saw teeth) once the stretched texels
		// cover more than c_StairTexelPixels: the edge's partly lit pixels take a radius of a texel's footprint, which the blur
		// keeps on the receiver's plane. (Lit ground often takes the tent too: only edges pay.)
		if (partlyLit && abs(dot(normal, u_Frame.DirectionalLightDirection.xyz)) < c_StairFacing)
		{
			float texelPixels = ReceiverTexelPixels(position, normal, worldPerTexel, pixelWorld);
			if (texelPixels > c_StairTexelPixels)
				blur = max(blur, min(texelPixels, c_StairMaxBlurPixels));
		}
	}
	o_Mask = vec2(sqrt(clamp(shadow.Visibility, 0.0, 1.0)), blur / c_ScreenShadowMaxBlurPixels);
}
