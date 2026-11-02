#include "Include/Common.slang"

#include "Include/Common.slang"

// Selection outline over the finished image: pixels outside a selected entity's visible silhouette, within Width
// pixels of it, take the outline color. Selection comes from the prepass entity-id buffer, or with ST_OUTLINE_MASK
// (Overlay/OutlineMask.frag) from the selection mask (Scene/SelectionMask.frag), which temporal anti-aliasing's frames draw
// with the camera's own matrices so the outline does not move with the jitter.

#ifdef ST_OUTLINE_MASK
Texture2D<float4> u_SelectionMask : register(t0);
#else
Texture2D<uint4> u_EntityIDs : register(t0);
StructuredBuffer<uint> u_Selected : register(t1);
#endif

struct OutlineParameters
{
	float4 Color;
	int Width;
	uint SelectedCount;
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<OutlineParameters> u_Outline;

static float4 o_Color;

#ifndef ST_OUTLINE_MASK
bool IsSelected(uint id)
{
	if (id == 0u)
		return false;
	uint low = 0u;
	uint high = u_Outline.SelectedCount;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		uint value = u_Selected[middle];
		if (value == id)
			return true;
		if (value < id)
			low = middle + 1u;
		else
			high = middle;
	}
	return false;
}
#endif

bool IsSelectedPixel(int2 pixel)
{
#ifdef ST_OUTLINE_MASK
	return u_SelectionMask.Load(int3(pixel, 0)).r > 0.5;
#else
	return IsSelected(u_EntityIDs.Load(int3(pixel, 0)).r);
#endif
}

void st_Main()
{
#ifdef ST_OUTLINE_MASK
	int2 size = TextureSize(u_SelectionMask, 0);
#else
	int2 size = TextureSize(u_EntityIDs, 0);
#endif
	int2 pixel = int2(st_FragCoord.xy);
	if (IsSelectedPixel(pixel))
		discard;

	int width = u_Outline.Width;
	for (int y = -width; y <= width; y++)
	{
		for (int x = -width; x <= width; x++)
		{
			int2 neighbor = pixel + int2(x, y);
			if (x * x + y * y > width * width || any((neighbor < int2(0))) || any((neighbor >= size)))
				continue;
			if (IsSelectedPixel(neighbor))
			{
				o_Color = u_Outline.Color;
				return;
			}
		}
	}
	discard;
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
