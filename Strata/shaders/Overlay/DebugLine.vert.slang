#include "Include/Common.slang"

#include "Include/Scene.slang"

// Debug lines (DebugDraw) in camera-relative world space (SceneRenderer subtracts the camera's position) with display colors.

static float3 a_Position;
static float4 a_Color; // RGBA8 UNORM, sRGB-encoded

static float4 v_Color;

// Lines drawn on a surface (e.g. a collider around its mesh) are pulled toward the camera by this fraction of their
// depth so they do not flicker against it (reversed-Z: larger is nearer).
static const float c_DepthBias = 1.0e-4;

void st_Main()
{
	v_Color = a_Color;
	st_Position = mul(u_Frame.ViewProjection, float4(a_Position, 1.0));
	// Scaling z is a linear change of clip space applied to every vertex alike, so lines crossing the near plane or reaching
	// behind the camera (w < 0) are still clipped where they leave the view (clamping z would move their ends).
	st_Position.z *= 1.0 + c_DepthBias;
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 a_Position : ATTRIBUTE0;
	[[vk::location(1)]] float4 a_Color : ATTRIBUTE1;
};
struct ST_Output
{
	[[vk::location(0)]] float4 v_Color : TEXCOORD0;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	a_Position = input.a_Position;
	a_Color = input.a_Color;
	st_Main();
	ST_Output output;
	output.v_Color = v_Color;
	output.st_Position = st_Position;
	return output;
}
