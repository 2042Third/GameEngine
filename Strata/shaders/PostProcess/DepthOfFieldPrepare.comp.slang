#include "Include/Common.slang"

#include "Include/DepthOfField.slang"

// Depth of field's half-resolution image: each texel the mean color of its 2x2 full-resolution pixels and, in alpha, a signed
// circle of confusion in half-resolution pixels: the nearest pixel's when one lies in the near field (a near silhouette keeps
// its blur), else the farthest's.

Texture2D<float4> u_Color : register(t0);
Texture2D<float4> u_Depth : register(t1);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Half : register(u0);

void st_Main()
{
	int2 size = TextureSize(u_Color, 0);
	int2 halfSize = (size + 1) / 2;
	int2 texel = int2(st_GlobalInvocationID.xy);
	if (any((texel >= halfSize)))
		return;
	float3 color = float3(0.0);
	float nearest = 1e30;
	float farthest = -1e30;
	for (int index = 0; index < 4; index++)
	{
		int2 pixel = min(2 * texel + int2(index & 1, index >> 1), size - 1);
		color += u_Color.Load(int3(pixel, 0)).rgb;
		float coc = GetCircleOfConfusion((float2(pixel) + 0.5) * u_Frame.ViewportSize.zw, u_Depth.Load(int3(pixel, 0)).r);
		nearest = min(nearest, coc);
		farthest = max(farthest, coc);
	}
	float coc = nearest < -2.0 * c_DepthOfFieldInFocus ? nearest : farthest;
	o_Half[texel] = float4(color * 0.25, 0.5 * coc);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
