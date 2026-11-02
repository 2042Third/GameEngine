#include "Include/Common.slang"

#include "Include/Common.slang"

// Fullscreen triangle without vertex buffers: draw 3 vertices.
static float2 v_UV;

void st_Main()
{
	float2 ndc = float2(float((st_VertexIndex << 1) & 2), float(st_VertexIndex & 2)) * 2.0 - 1.0;
	v_UV = NDCToUV(ndc);
	st_Position = float4(ndc, 0.0, 1.0);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	st_Main();
	ST_Output output;
	output.v_UV = v_UV;
	output.st_Position = st_Position;
	return output;
}
