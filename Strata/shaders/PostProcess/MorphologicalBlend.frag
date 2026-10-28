#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"
#include "Include/Dither.glsl"

// Morphological anti-aliasing, pass 3 of 3: each pixel mixes, in linear light, with the neighbors across its edges by the
// weights of pass 2 (its own top and left edges' r and b; the g of the pixel below and the a of the pixel right of it, the
// shares their edges give this pixel), and the result is dithered into the display target. A pixel without weights keeps the
// tone mapping's value exactly.

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Color; // The tone mapping's display-encoded image (10 bits)
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_Weights;

// RenderData::MorphologicalParameters (Renderer/PostProcessData.h).
layout(push_constant) uniform MorphologicalParameters
{
	float Threshold;
	float DepthThreshold;
	float DitherLevels; // 2^bits - 1 of the target this pass writes; 0: no dither
	float Padding0;
} u_Parameters;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

void main()
{
	ivec2 size = textureSize(u_Color, 0);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec3 color = texelFetch(u_Color, pixel, 0).rgb;
	vec4 own = texelFetch(u_Weights, pixel, 0);
	float up = own.r;
	float left = own.b;
	float down = pixel.y + 1 < size.y ? texelFetch(u_Weights, pixel + ivec2(0, 1), 0).g : 0.0;
	float right = pixel.x + 1 < size.x ? texelFetch(u_Weights, pixel + ivec2(1, 0), 0).a : 0.0;
	float total = up + down + left + right;
	if (total > 0.0)
	{
		// Shares above a whole pixel (a pixel in a corner of two runs) are scaled down to one.
		float scale = total > 1.0 ? 1.0 / total : 1.0;
		vec3 mixed = SRGBToLinear(color) * (1.0 - total * scale);
		mixed += SRGBToLinear(texelFetch(u_Color, max(pixel - ivec2(0, 1), ivec2(0)), 0).rgb) * (up * scale);
		mixed += SRGBToLinear(texelFetch(u_Color, min(pixel + ivec2(0, 1), size - 1), 0).rgb) * (down * scale);
		mixed += SRGBToLinear(texelFetch(u_Color, max(pixel - ivec2(1, 0), ivec2(0)), 0).rgb) * (left * scale);
		mixed += SRGBToLinear(texelFetch(u_Color, min(pixel + ivec2(1, 0), size - 1), 0).rgb) * (right * scale);
		color = LinearToSRGB(mixed);
	}
	o_Color = vec4(DitherToCodes(color, uvec2(pixel), u_Parameters.DitherLevels), 1.0);
}
