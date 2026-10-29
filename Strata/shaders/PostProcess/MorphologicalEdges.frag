#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Morphological anti-aliasing, pass 1 of 3 (SceneRendererPost.cpp, RenderPostDisplay): each pixel's left (r) and top (g) edges.
// An edge separates two pixels whose display-encoded luma differs by Threshold or more, kept only where that difference is at
// least half the largest of the pixel's other neighbors' (local contrast adaptation: the strong edge of a pair of nearby
// edges is the one aliasing shows). Depth discontinuities count as edges too: the depth (linear in screen space on any
// plane, reversed-Z perspective and orthographic alike) changes between the two pixels by more than DepthThreshold of its
// value and by more than twice the neighboring steps (a slope's steady change is no edge, nor the step beside a jump).

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Color; // The tone mapping's display-encoded image (10 bits)
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_Depth;

// RenderData::MorphologicalParameters (Renderer/PostProcessData.h).
layout(push_constant) uniform MorphologicalParameters
{
	float Threshold;      // Luma difference (display-encoded) that makes an edge
	float DepthThreshold; // Relative depth difference (beyond the neighbors' steps) that makes an edge
	float DitherLevels;   // The blend pass's: 2^bits - 1 of the target it writes; 0: no dither
	float FilmGrain;      // The blend's grain (Include/Grain.glsl), added after the blend; 0 = off
	uint GrainFrame;      // The frame's number: the grain changes every frame
	float Padding0;
	float Padding1;
	float Padding2;
} u_Parameters;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec2 o_Edges;

ivec2 g_Size;

float Luma(ivec2 pixel)
{
	return dot(texelFetch(u_Color, clamp(pixel, ivec2(0), g_Size - 1), 0).rgb, vec3(0.2126, 0.7152, 0.0722));
}

float Depth(ivec2 pixel)
{
	return texelFetch(u_Depth, clamp(pixel, ivec2(0), g_Size - 1), 0).r;
}

// Whether the depth jumps between `pixel - step` and `pixel`: by more than DepthThreshold of its value, and by more than twice
// the steps on either side (a slope's steady change is no edge, nor is the step next to a jump).
bool DepthEdge(ivec2 pixel, ivec2 step)
{
	float before = Depth(pixel - 2 * step);
	float previous = Depth(pixel - step);
	float center = Depth(pixel);
	float after = Depth(pixel + step);
	float jump = abs(center - previous);
	return jump > u_Parameters.DepthThreshold * max(center, previous) && jump > 2.0 * max(abs(previous - before), abs(after - center));
}

void main()
{
	g_Size = textureSize(u_Color, 0);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float center = Luma(pixel);
	float left = Luma(pixel - ivec2(1, 0));
	float top = Luma(pixel - ivec2(0, 1));
	vec2 delta = abs(vec2(center - left, center - top));
	vec2 edges = step(vec2(u_Parameters.Threshold), delta);
	if (edges.x + edges.y > 0.0)
	{
		float right = Luma(pixel + ivec2(1, 0));
		float bottom = Luma(pixel + ivec2(0, 1));
		float leftLeft = Luma(pixel - ivec2(2, 0));
		float topTop = Luma(pixel - ivec2(0, 2));
		float largest = max(max(max(delta.x, delta.y), max(abs(center - right), abs(center - bottom))), max(abs(left - leftLeft), abs(top - topTop)));
		edges *= step(vec2(0.5 * largest), delta);
	}
	edges = max(edges, vec2(DepthEdge(pixel, ivec2(1, 0)) ? 1.0 : 0.0, DepthEdge(pixel, ivec2(0, 1)) ? 1.0 : 0.0));
	// The image's first column and row have no pixel left of or above them.
	if (pixel.x == 0)
		edges.x = 0.0;
	if (pixel.y == 0)
		edges.y = 0.0;
	o_Edges = edges;
}
