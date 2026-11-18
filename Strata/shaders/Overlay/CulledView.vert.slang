#include "Include/Common.slang"

#include "Include/Scene.slang"

// The culled debug view (SceneRenderOptions::ShowCulled): per instance a box of twelve lines (24 vertices), the world bounds of
// the proxy its entry of the frame's culled list names (the index below ST_CULLED_REASON_SHIFT, why the frame left it out
// above: RenderData::c_CulledReasonShift; Scene/Cull.comp or the CPU's extraction wrote it). A draw shows the entries of the push
// constant's reason only, so crossing lines of two colors do not depend on the order the GPU recorded them in; the others'
// lines lie outside the clip volume. A box smaller on screen than MinPixels is drawn that wide about its center, so that the
// proxies culled by their size show. Positions are the bounds minus the camera's position, as Scene/Cull.comp tests them,
// drawn with the overlays' camera-relative matrices; Overlay/DebugLine.frag writes the color.

struct CulledViewProxy
{
	float3 LocalMin;
	uint FirstInstance;
	float3 LocalMax;
	uint InstanceCount;
	float3 WorldMin;
	uint MeshSlot;
	float3 WorldMax;
	uint Flags;
};
// Registers Include/Scene.slang leaves free.
StructuredBuffer<CulledViewProxy> u_CulledViewProxies : register(t22);
StructuredBuffer<uint> u_CulledViewEntries : register(t23);

struct CulledViewParameters
{
	uint Color;      // RGBA8, sRGB-encoded, packed (red in the low byte)
	uint Reason;     // The entries this draw shows
	float MinPixels; // The smallest box drawn, in pixels across its diagonal (0: every box as it is)
	uint Padding;
};
[[vk::push_constant]] ConstantBuffer<CulledViewParameters> u_CulledView;

#define ST_CULLED_REASON_SHIFT 30

static float4 v_Color;

// The box's twelve edges as pairs of corners (bit 0: the maximum's x, bit 1 its y, bit 2 its z).
static const uint c_EdgeCorners[24] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0u, 2u, 1u, 3u, 4u, 6u, 5u, 7u, 0u, 4u, 1u, 5u, 2u, 6u, 3u, 7u };

void st_Main()
{
	v_Color = UnpackUnorm4x8(u_CulledView.Color);
	uint entry = u_CulledViewEntries[st_InstanceIndex];
	if ((entry >> ST_CULLED_REASON_SHIFT) != u_CulledView.Reason)
	{
		// Right of the view (x beyond w): both ends at one point outside it, so the line is clipped away.
		st_Position = float4(2.0, 0.0, 0.5, 1.0);
		return;
	}
	CulledViewProxy proxy = u_CulledViewProxies[entry & ((1u << ST_CULLED_REASON_SHIFT) - 1u)];
	float3 boxMin = proxy.WorldMin - u_Frame.CameraPosition.xyz;
	float3 boxMax = proxy.WorldMax - u_Frame.CameraPosition.xyz;
	float3 center = (boxMin + boxMax) * 0.5;
	float3 halfSize = (boxMax - boxMin) * 0.5;
	if (u_CulledView.MinPixels > 0.0)
	{
		// Pixels per world unit at the box's center: the projection's vertical scale over the clip w (1 for orthographic views).
		float4 clip = mul(u_Frame.ViewProjection, float4(center, 1.0));
		float pixelsPerUnit = abs(u_Frame.Projection[1][1]) * 0.5 * u_Frame.ViewportSize.y / max(abs(clip.w), 1e-6);
		float halfDiagonal = length(halfSize);
		float wanted = 0.5 * u_CulledView.MinPixels / max(pixelsPerUnit, 1e-12);
		if (halfDiagonal < wanted)
			halfSize = halfDiagonal > 1e-12 ? halfSize * (wanted / halfDiagonal) : float3(wanted * 0.57735027, wanted * 0.57735027, wanted * 0.57735027);
	}
	uint corner = c_EdgeCorners[st_VertexIndex];
	float3 side = float3((corner & 1u) != 0u ? 1.0 : -1.0, (corner & 2u) != 0u ? 1.0 : -1.0, (corner & 4u) != 0u ? 1.0 : -1.0);
	st_Position = mul(u_Frame.ViewProjection, float4(center + halfSize * side, 1.0));
}

// --- Entry point ---
struct ST_Output
{
	[[vk::location(0)]] float4 v_Color : TEXCOORD0;
	float4 st_Position : SV_Position;
};
ST_Output main(int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	st_Main();
	ST_Output output;
	output.v_Color = v_Color;
	output.st_Position = st_Position;
	return output;
}
