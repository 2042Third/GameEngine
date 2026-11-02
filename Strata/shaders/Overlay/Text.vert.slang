#include "Include/Common.slang"

#include "Include/Scene.slang"

// Glyph quads of TextRenderer: camera-relative world-space positions, or pixel positions (+Y down) for screen-space text.

static float3 a_Position;
static float3 a_TexCoord; // Atlas texels and page
static float4 a_Color;    // RGBA8 UNORM, display colors

static float3 v_TexCoord;
static float4 v_Color;

struct TextParameters
{
	float2 ViewportSize;
	uint ScreenSpace;
	uint Padding;
};
[[vk::push_constant]] ConstantBuffer<TextParameters> u_Text;

void st_Main()
{
	v_TexCoord = a_TexCoord;
	v_Color = a_Color;
	if (u_Text.ScreenSpace != 0u)
		st_Position = float4(UVToNDC(a_Position.xy / u_Text.ViewportSize), 0.5, 1.0);
	else
		st_Position = mul(u_Frame.ViewProjection, float4(a_Position, 1.0));
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 a_Position : ATTRIBUTE0;
	[[vk::location(1)]] float3 a_TexCoord : ATTRIBUTE1;
	[[vk::location(2)]] float4 a_Color : ATTRIBUTE2;
};
struct ST_Output
{
	[[vk::location(0)]] float3 v_TexCoord : TEXCOORD0;
	[[vk::location(1)]] float4 v_Color : TEXCOORD1;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	a_Position = input.a_Position;
	a_TexCoord = input.a_TexCoord;
	a_Color = input.a_Color;
	st_Main();
	ST_Output output;
	output.v_TexCoord = v_TexCoord;
	output.v_Color = v_Color;
	output.st_Position = st_Position;
	return output;
}
