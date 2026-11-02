#include "Include/Common.slang"

#include "Include/Common.slang"

// Debug-view tint over the finished image (SceneRenderOptions::EntityTints): a pixel whose entity in the prepass
// entity-id buffer is listed takes the listed color, blended by its alpha; every other pixel is discarded, so it stays
// exactly as it was.

Texture2D<uint4> u_EntityIDs : register(t0);
StructuredBuffer<uint2> u_Tints : register(t1);

struct TintParameters
{
	uint Count;
	uint Padding0;
	uint Padding1;
	uint Padding2;
};
[[vk::push_constant]] ConstantBuffer<TintParameters> u_Tint;

static float4 o_Color;

void st_Main()
{
	uint id = u_EntityIDs.Load(int3(int2(st_FragCoord.xy), 0)).r;
	if (id == 0u)
		discard;

	uint low = 0u;
	uint high = u_Tint.Count;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		uint2 tint = u_Tints[middle];
		if (tint.x == id)
		{
			o_Color = UnpackUnorm4x8(tint.y);
			return;
		}
		if (tint.x < id)
			low = middle + 1u;
		else
			high = middle;
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
