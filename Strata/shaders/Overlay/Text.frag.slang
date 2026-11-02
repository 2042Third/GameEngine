#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/SDFText.slang"

// Signed distance field text (see FontAtlas): the glyph outline is where the distance crosses the atlas's on-edge
// value; coverage ramps over about one screen pixel around it, so text stays sharp and smooth at any size.

Texture2DArray<float4> u_Atlas : register(t15); // One layer per atlas page
SamplerState u_AtlasSampler : register(s15);

static float3 v_TexCoord; // Atlas texels and page (the same for a whole glyph)
static float4 v_Color;

static float4 o_Color;

void st_Main()
{
	float2 pageSize = float2(TextureSize(u_Atlas, 0).xy);
	// Array layers are selected by rounding, so interpolation error in the page coordinate does not matter.
	float distance = u_Atlas.Sample(u_AtlasSampler, float3(v_TexCoord.xy / pageSize, v_TexCoord.z)).r;
	float coverage = SDFCoverage(distance, c_SDFOnEdgeValue);
	if (coverage <= 0.0)
		discard;
	o_Color = float4(v_Color.rgb, v_Color.a * coverage);
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 v_TexCoord : TEXCOORD0;
	[[vk::location(1)]] float4 v_Color : TEXCOORD1;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_TexCoord = input.v_TexCoord;
	v_Color = input.v_Color;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
