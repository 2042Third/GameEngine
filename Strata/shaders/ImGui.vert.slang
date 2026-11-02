#include "Include/Common.slang"

// Dear ImGui geometry: pixel-space positions (y down) mapped to NDC by a scale/translate pair.

static float2 a_Position;
static float2 a_TexCoord;
static float4 a_Color;

struct PushConstants
{
	float2 Scale;
	float2 Translate;
	uint TextureDisplay; // Read by ImGui.frag
};
[[vk::push_constant]] ConstantBuffer<PushConstants> u_Push;

static float2 v_TexCoord;
static float4 v_Color;

void st_Main()
{
	v_TexCoord = a_TexCoord;
	v_Color = a_Color;
	st_Position = float4(a_Position * u_Push.Scale + u_Push.Translate, 0.0, 1.0);
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 a_Position : ATTRIBUTE0;
	[[vk::location(1)]] float2 a_TexCoord : ATTRIBUTE1;
	[[vk::location(2)]] float4 a_Color : ATTRIBUTE2;
};
struct ST_Output
{
	[[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
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
