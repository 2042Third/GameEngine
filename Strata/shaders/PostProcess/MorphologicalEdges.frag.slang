#include "Include/Common.slang"

#include "Include/Common.slang"

// Morphological anti-aliasing, pass 1 of 3 (SceneRendererPost.cpp, RenderPostDisplay): each pixel's left (r) and top (g) edges.
// An edge separates two pixels whose display-encoded luma differs by Threshold or more, kept only where that difference is at
// least half the largest of the pixel's other neighbors' (local contrast adaptation: the strong edge of a pair of nearby
// edges is the one aliasing shows). Depth discontinuities count as edges too: the depth (linear in screen space on any
// plane, reversed-Z perspective and orthographic alike) changes between the two pixels by more than DepthThreshold of its
// value and by more than twice the neighboring steps (a slope's steady change is no edge, nor the step beside a jump).

Texture2D<float4> u_Color : register(t0); // The tone mapping's display-encoded image (10 bits)
Texture2D<float4> u_Depth : register(t1);

// RenderData::MorphologicalParameters (Renderer/PostProcessData.h).
struct MorphologicalParameters
{
	float Threshold;      // Luma difference (display-encoded) that makes an edge
	float DepthThreshold; // Relative depth difference (beyond the neighbors' steps) that makes an edge
	float DitherLevels;   // The blend pass's: 2^bits - 1 of the target it writes; 0: no dither
	float FilmGrain;      // The blend's grain (Include/Grain.slang), added after the blend; 0 = off
	uint GrainFrame;      // The frame's number: the grain changes every frame
	float Padding0;
	float Padding1;
	float Padding2;
};
[[vk::push_constant]] ConstantBuffer<MorphologicalParameters> u_Parameters;

static float2 v_UV;
static float2 o_Edges;

static int2 g_Size;

float Luma(int2 pixel)
{
	return dot(u_Color.Load(int3(clamp(pixel, int2(0), g_Size - 1), 0)).rgb, float3(0.2126, 0.7152, 0.0722));
}

float Depth(int2 pixel)
{
	return u_Depth.Load(int3(clamp(pixel, int2(0), g_Size - 1), 0)).r;
}

// Whether the depth jumps between `pixel - step` and `pixel`: by more than DepthThreshold of its value, and by more than twice
// the steps on either side (a slope's steady change is no edge, nor is the step next to a jump).
bool DepthEdge(int2 pixel, int2 step)
{
	float before = Depth(pixel - 2 * step);
	float previous = Depth(pixel - step);
	float center = Depth(pixel);
	float after = Depth(pixel + step);
	float jump = abs(center - previous);
	return jump > u_Parameters.DepthThreshold * max(center, previous) && jump > 2.0 * max(abs(previous - before), abs(after - center));
}

void st_Main()
{
	g_Size = TextureSize(u_Color, 0);
	int2 pixel = int2(st_FragCoord.xy);
	float center = Luma(pixel);
	float left = Luma(pixel - int2(1, 0));
	float top = Luma(pixel - int2(0, 1));
	float2 delta = abs(float2(center - left, center - top));
	float2 edges = step(float2(u_Parameters.Threshold), delta);
	if (edges.x + edges.y > 0.0)
	{
		float right = Luma(pixel + int2(1, 0));
		float bottom = Luma(pixel + int2(0, 1));
		float leftLeft = Luma(pixel - int2(2, 0));
		float topTop = Luma(pixel - int2(0, 2));
		float largest = max(max(max(delta.x, delta.y), max(abs(center - right), abs(center - bottom))), max(abs(left - leftLeft), abs(top - topTop)));
		edges *= step(float2(0.5 * largest), delta);
	}
	edges = max(edges, float2(DepthEdge(pixel, int2(1, 0)) ? 1.0 : 0.0, DepthEdge(pixel, int2(0, 1)) ? 1.0 : 0.0));
	// The image's first column and row have no pixel left of or above them.
	if (pixel.x == 0)
		edges.x = 0.0;
	if (pixel.y == 0)
		edges.y = 0.0;
	o_Edges = edges;
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float2 o_Edges : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_UV = input.v_UV;
	st_Main();
	ST_Output output;
	output.o_Edges = o_Edges;
	return output;
}
