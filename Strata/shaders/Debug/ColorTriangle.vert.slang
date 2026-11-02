#include "Include/Common.slang"

// Diagnostic shader used by GPU tests: one triangle covering the upper half of the viewport, colored by
// vertex, with no vertex buffer. Verifies pipeline creation, NDC orientation and depth conventions.

static float3 v_Color;

static const float2 c_Positions[3] = { float2(-1.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0) };
static const float3 c_Colors[3] = { float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0), float3(1.0, 0.0, 0.0) };

void st_Main()
{
	v_Color = c_Colors[st_VertexIndex];
	st_Position = float4(c_Positions[st_VertexIndex], 0.5, 1.0);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float3 v_Color : TEXCOORD0;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	st_Main();
	ST_Output output;
	output.v_Color = v_Color;
	output.st_Position = st_Position;
	return output;
}
