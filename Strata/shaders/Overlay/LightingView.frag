#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/LightingView.glsl"

// The lighting debug views that show values (SceneRenderOptions::LightingView: AmbientOcclusion, Albedo, Luminance) over the
// finished image, which they replace: from the frame's own HDR target (never the temporal history, so a value is never
// blended with an earlier frame's), as display colors (sRGB).
// - AmbientOcclusion: the occlusion as gray; pixels without a surface the background color.
// - Albedo: the base color; luminances below ST_ALBEDO_LOW mixed toward the low mark, above ST_ALBEDO_HIGH toward the high
//   mark in diagonal stripes (so a marked bright surface is not taken for a bright one); no surface: the background color.
// - Luminance: the luminance's EV100 band, one EV wide from LuminanceFirstEV100 on, the background (the sky's light) included.

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_HDR;
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_Depth;

layout(location = 0) out vec4 o_Color;

const float c_MarkStrength = 0.7;

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec3 hdr = texelFetch(u_HDR, pixel, 0).rgb;
	bool surface = texelFetch(u_Depth, pixel, 0).r > 0.0;
	vec3 color;
	if (u_LightingView.View == ST_LIGHTING_VIEW_LUMINANCE)
	{
		int band = int(clamp(floor(GetPixelEV100(hdr) - u_LightingView.LuminanceFirstEV100), 0.0, float(ST_LIGHTING_VIEW_LUMINANCE_COLORS - 1)));
		color = GetLuminanceBandColor(band);
	}
	else if (!surface)
	{
		color = UnpackDisplayColor(u_LightingView.Background);
	}
	else if (u_LightingView.View == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION)
	{
		color = LinearToSRGB(vec3(clamp(hdr.r, 0.0, 1.0)));
	}
	else
	{
		color = LinearToSRGB(clamp(hdr, vec3(0.0), vec3(1.0)));
		float albedo = Luminance(hdr);
		if (albedo < ST_ALBEDO_LOW)
			color = mix(color, UnpackDisplayColor(u_LightingView.AlbedoLow), c_MarkStrength);
		else if (albedo > ST_ALBEDO_HIGH && ((pixel.x + pixel.y) / 4) % 2 == 0)
			color = mix(color, UnpackDisplayColor(u_LightingView.AlbedoHigh), c_MarkStrength);
	}
	o_Color = vec4(color, 1.0);
}
