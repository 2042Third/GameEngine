#include "Include/Common.slang"

#include "Include/Common.slang"

// Copies an image of the target's size texel for texel (format conversion only).

Texture2D<float4> u_Source : register(t0);

static float4 o_Color;

void st_Main()
{
	o_Color = u_Source.Load(int3(int2(st_FragCoord.xy), 0));
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
