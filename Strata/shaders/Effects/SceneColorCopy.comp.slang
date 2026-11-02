#include "Include/Common.slang"

#include "Include/Common.slang"

// One level of the scene-color copy transmissive surfaces read (SceneRenderer::CopySceneColor): each texel the mean of the
// 2x2 texels it covers in the level above, the HDR target for the first level, so each level is half the size of the one
// above and twice as blurred (rough glass reads coarser levels). A destination of the source's size takes its texels as they
// are: spatial screen-space reflections' full-resolution copy of the HDR target. The copy is B10G11R11 where the device
// stores it; with ST_SCENE_COLOR_FALLBACK (registered as Effects/SceneColorCopyFallback.comp) it is RGBA16F.

Texture2D<float4> u_Source : register(t0);
#ifdef ST_SCENE_COLOR_FALLBACK
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Destination : register(u0);
#else
[vk::image_format("r11f_g11f_b10f")] RWTexture2D<float4> o_Destination : register(u0);
#endif

void st_Main()
{
	int2 texel = int2(st_GlobalInvocationID.xy);
	int2 size = ImageSize(o_Destination);
	if (texel.x >= size.x || texel.y >= size.y)
		return;
	int2 last = TextureSize(u_Source, 0) - 1;
	if (all(size == last + 1))
	{
		o_Destination[texel] = float4(max(u_Source.Load(int3(texel, 0)).rgb, float3(0.0)), 1.0);
		return;
	}
	// A source of odd size has no texel after its last: the last row or column takes it again.
	int2 first = texel * 2;
	float3 sum = u_Source.Load(int3(min(first, last), 0)).rgb + u_Source.Load(int3(min(first + int2(1, 0), last), 0)).rgb
		+ u_Source.Load(int3(min(first + int2(0, 1), last), 0)).rgb + u_Source.Load(int3(min(first + int2(1, 1), last), 0)).rgb;
	// The HDR target holds no negative values (SanitizeHDR); B10G11R11 could not store them.
	o_Destination[texel] = float4(max(sum * 0.25, float3(0.0)), 1.0);
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
