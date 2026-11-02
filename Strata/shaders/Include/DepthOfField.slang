#ifndef STRATA_DEPTH_OF_FIELD_SLANG
#define STRATA_DEPTH_OF_FIELD_SLANG

#include "Include/Scene.slang"

// Depth of field's passes (PostProcess/DepthOfField*.comp; SceneRendererPost.cpp, RenderDepthOfField): the frame constants at b0
// (Include/Scene.slang: the depth's view distance), the parameters below as push constants. SRV 0-3 alias Include/Scene.slang's
// declarations, which these shaders never use, as the motion blur's do; the composite's bilinear reads use its linear clamping
// sampler (s8).

// RenderData::DepthOfFieldParameters (Renderer/PostProcessData.h).
struct DepthOfFieldParameters
{
	float FocusDistance; // Meters
	float CoCScale;      // Full-resolution pixels: the circle of confusion of a point infinitely far (CameraLens)
	float MaxCoC;        // Full-resolution pixels: the largest circle of confusion either way (2% of the height)
	uint TileSize;       // Half-resolution pixels each way of a tile of the near field's maxima
	uint SampleCount;    // The gather's samples per field
	uint Padding0;
	uint Padding1;
	uint Padding2;
};
[[vk::push_constant]] ConstantBuffer<DepthOfFieldParameters> u_DepthOfField;

// The signed circle of confusion, in full-resolution pixels, of what a pixel shows: negative in front of the focus, the sky
// (nothing drawn, infinitely far) the largest behind it; clamped to MaxCoC either way.
float GetCircleOfConfusion(float2 uv, float depth)
{
	float coc = u_DepthOfField.CoCScale;
	if (depth > 0.0)
	{
		float4 point = mul(u_Frame.InverseProjection, float4(UVToNDC(uv), depth, 1.0));
		float distance = max(-point.z / point.w, 1e-4);
		coc = u_DepthOfField.CoCScale * (1.0 - u_DepthOfField.FocusDistance / distance);
	}
	return clamp(coc, -u_DepthOfField.MaxCoC, u_DepthOfField.MaxCoC);
}

// Circles of confusion under this many half-resolution pixels (one full-resolution pixel) count as in focus.
static const float c_DepthOfFieldInFocus = 0.5;

#endif
