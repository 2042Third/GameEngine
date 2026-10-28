#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Morphological anti-aliasing, pass 2 of 3: the blend weights of each pixel's top edge (rg) and left edge (ba). For an edge, the
// run of that edge's pixels is searched both ways (at most c_MaxSearch pixels each), the crossing edges at the run's two ends
// say how its staircase turns (into the row or column across the edge, into the pixel's own, both or neither), and the area
// table (Renderer/MorphologicalAreaTable.inl, Tools/GenerateAreaTable.py) gives the share of the pixel the reconstructed line
// gives to the color across the edge (r, b) and the share of the pixel across the edge it gives to this pixel's color (g, a).

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Edges;
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_AreaTable;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Weights;

const int c_MaxSearch = 32;
const int c_TableSize = c_MaxSearch + 1;

ivec2 g_Size;

vec2 Edges(ivec2 pixel)
{
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, g_Size)))
		return vec2(0.0);
	return texelFetch(u_Edges, pixel, 0).rg;
}

// The crossing at a run's end: 1 across the edge, 2 on the pixel's own side, 3 both ways, 0 none.
int Crossing(bool across, bool own)
{
	return (across ? 1 : 0) + (own ? 2 : 0);
}

vec2 Areas(int first, int second, int before, int after)
{
	return texelFetch(u_AreaTable, ivec2(first * c_TableSize + before, second * c_TableSize + after), 0).rg;
}

void main()
{
	g_Size = textureSize(u_Edges, 0);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 edges = Edges(pixel);
	vec4 weights = vec4(0.0);
	if (edges.y > 0.5)
	{
		// The top edge: a horizontal run between this row and the one above. A run that goes on beyond the search has no
		// known end there (no crossing).
		int left = 0;
		while (left < c_MaxSearch && Edges(pixel - ivec2(left + 1, 0)).y > 0.5)
			left++;
		int right = 0;
		while (right < c_MaxSearch && Edges(pixel + ivec2(right + 1, 0)).y > 0.5)
			right++;
		ivec2 first = pixel - ivec2(left, 0);
		ivec2 last = pixel + ivec2(right, 0);
		int firstCrossing = left < c_MaxSearch ? Crossing(Edges(first + ivec2(0, -1)).x > 0.5, Edges(first).x > 0.5) : 0;
		int lastCrossing = right < c_MaxSearch ? Crossing(Edges(last + ivec2(1, -1)).x > 0.5, Edges(last + ivec2(1, 0)).x > 0.5) : 0;
		weights.rg = Areas(firstCrossing, lastCrossing, left, right);
	}
	if (edges.x > 0.5)
	{
		// The left edge: a vertical run between this column and the one left of it.
		int up = 0;
		while (up < c_MaxSearch && Edges(pixel - ivec2(0, up + 1)).x > 0.5)
			up++;
		int down = 0;
		while (down < c_MaxSearch && Edges(pixel + ivec2(0, down + 1)).x > 0.5)
			down++;
		ivec2 first = pixel - ivec2(0, up);
		ivec2 last = pixel + ivec2(0, down);
		int firstCrossing = up < c_MaxSearch ? Crossing(Edges(first + ivec2(-1, 0)).y > 0.5, Edges(first).y > 0.5) : 0;
		int lastCrossing = down < c_MaxSearch ? Crossing(Edges(last + ivec2(-1, 1)).y > 0.5, Edges(last + ivec2(0, 1)).y > 0.5) : 0;
		weights.ba = Areas(firstCrossing, lastCrossing, up, down);
	}
	o_Weights = weights;
}
