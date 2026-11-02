#include "Include/Common.slang"

#include "Include/Common.slang"

// Morphological anti-aliasing, pass 2 of 3: the blend weights of each pixel's top edge (rg) and left edge (ba). For an edge, the
// run of that edge's pixels is searched both ways (at most c_MaxSearch pixels each), the crossing edges at the run's two ends
// say how its staircase turns (into the row or column across the edge, into the pixel's own, both or neither), and the area
// table (Renderer/MorphologicalAreaTable.inl, Tools/GenerateAreaTable.py) gives the share of the pixel the reconstructed line
// gives to the color across the edge (r, b) and the share of the pixel across the edge it gives to this pixel's color (g, a).

Texture2D<float4> u_Edges : register(t0);
Texture2D<float4> u_AreaTable : register(t1);

static float2 v_UV;
static float4 o_Weights;

static const int c_MaxSearch = 32;
static const int c_TableSize = c_MaxSearch + 1;

static int2 g_Size;

float2 Edges(int2 pixel)
{
	if (any((pixel < int2(0))) || any((pixel >= g_Size)))
		return float2(0.0);
	return u_Edges.Load(int3(pixel, 0)).rg;
}

// The crossing at a run's end: 1 across the edge, 2 on the pixel's own side, 3 both ways, 0 none.
int Crossing(bool across, bool own)
{
	return (across ? 1 : 0) + (own ? 2 : 0);
}

float2 Areas(int first, int second, int before, int after)
{
	return u_AreaTable.Load(int3(int2(first * c_TableSize + before, second * c_TableSize + after), 0)).rg;
}

void st_Main()
{
	g_Size = TextureSize(u_Edges, 0);
	int2 pixel = int2(st_FragCoord.xy);
	float2 edges = Edges(pixel);
	float4 weights = float4(0.0);
	if (edges.y > 0.5)
	{
		// The top edge: a horizontal run between this row and the one above. A run that goes on beyond the search has no
		// known end there (no crossing).
		int left = 0;
		while (left < c_MaxSearch && Edges(pixel - int2(left + 1, 0)).y > 0.5)
			left++;
		int right = 0;
		while (right < c_MaxSearch && Edges(pixel + int2(right + 1, 0)).y > 0.5)
			right++;
		int2 first = pixel - int2(left, 0);
		int2 last = pixel + int2(right, 0);
		int firstCrossing = left < c_MaxSearch ? Crossing(Edges(first + int2(0, -1)).x > 0.5, Edges(first).x > 0.5) : 0;
		int lastCrossing = right < c_MaxSearch ? Crossing(Edges(last + int2(1, -1)).x > 0.5, Edges(last + int2(1, 0)).x > 0.5) : 0;
		weights.rg = Areas(firstCrossing, lastCrossing, left, right);
	}
	if (edges.x > 0.5)
	{
		// The left edge: a vertical run between this column and the one left of it.
		int up = 0;
		while (up < c_MaxSearch && Edges(pixel - int2(0, up + 1)).x > 0.5)
			up++;
		int down = 0;
		while (down < c_MaxSearch && Edges(pixel + int2(0, down + 1)).x > 0.5)
			down++;
		int2 first = pixel - int2(0, up);
		int2 last = pixel + int2(0, down);
		int firstCrossing = up < c_MaxSearch ? Crossing(Edges(first + int2(-1, 0)).y > 0.5, Edges(first).y > 0.5) : 0;
		int lastCrossing = down < c_MaxSearch ? Crossing(Edges(last + int2(-1, 1)).y > 0.5, Edges(last + int2(0, 1)).y > 0.5) : 0;
		weights.ba = Areas(firstCrossing, lastCrossing, up, down);
	}
	o_Weights = weights;
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Weights : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_UV = input.v_UV;
	st_Main();
	ST_Output output;
	output.o_Weights = o_Weights;
	return output;
}
