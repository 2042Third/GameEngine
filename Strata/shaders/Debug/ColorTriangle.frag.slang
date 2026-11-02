#include "Include/Common.slang"

static float3 v_Color;
static float4 o_Color;

void st_Main()
{
	o_Color = float4(v_Color, 1.0);
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 v_Color : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_Color = input.v_Color;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
