#include "Include/Common.slang"

#include "Include/ShadowView.slang"

// The shadows debug view over the finished image (SceneRenderOptions::ShowShadowView): every receiver pixel colored by the
// screen pixels one shadow texel of its cascade spans there (Include/ShadowView.glsl), from Colors[0] at one pixel per texel
// or finer through Colors[1] at two to Colors[2] at four or coarser, linear in stops. Where a texel spans more than about 1.4
// pixels a checker of the shadow map's texels shows them; the colors darken with the sun's shadow (0.4 + 0.6 times the
// visibility of the frame's screen-space mask), so the view shows where the shadows fall on the texels; cascade edges are
// white and blend bands hatched. The pass cannot read the image it blends over: pixels no cascade covers get a gray veil,
// the sky the image darkened.

Texture2D<float4> u_Depth : register(t22);
// The sun's screen-space shadow mask (SceneRenderer::GetBoundScreenShadows: white without one), red the visibility's square
// root.
Texture2D<float4> u_ShadowMask : register(t23);

struct ShadowViewParameters
{
	float4 Colors[3];
};
[[vk::push_constant]] ConstantBuffer<ShadowViewParameters> u_ShadowView;

static float4 o_Color;

int CascadeAt(int2 pixel)
{
	int2 size = int2(u_Frame.ViewportSize.xy);
	if (any((pixel >= size)))
		return -1;
	ShadowViewSample neighbor;
	GetShadowViewSample(pixel, u_Depth.Load(int3(pixel, 0)).r, neighbor);
	return neighbor.Cascade;
}

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	if (!(depth > 0.0))
	{
		o_Color = float4(0.0, 0.0, 0.0, 0.65); // The sky: the image at 35%
		return;
	}
	ShadowViewSample viewSample;
	if (!GetShadowViewSample(pixel, depth, viewSample))
	{
		o_Color = float4(float3(0.25), 0.65); // No cascade: the image at 35% under gray
		return;
	}

	float stops = log2(viewSample.PixelsPerTexel);
	float t = clamp(stops, 0.0, 2.0);
	float3 color = t < 1.0 ? lerp(u_ShadowView.Colors[0].rgb, u_ShadowView.Colors[1].rgb, t) : lerp(u_ShadowView.Colors[1].rgb, u_ShadowView.Colors[2].rgb, t - 1.0);

	// The texels themselves, once they are coarse enough to see (no moire where they are finer than the pixels).
	float checkerStrength = clamp(stops - 0.5, 0.0, 1.0);
	int2 texel = int2(floor(viewSample.ShadowUV / u_Frame.ShadowParams.y));
	float checker = ((texel.x + texel.y) & 1) == 0 ? 1.0 : -1.0;
	color *= 1.0 + 0.12 * checkerStrength * checker;

	if (viewSample.BlendWeight > 0.0 && ((pixel.x + pixel.y) & 7) < 2)
		color *= 0.6;

	float visibility = u_ShadowMask.Load(int3(pixel, 0)).r;
	color *= 0.4 + 0.6 * visibility * visibility;

	// Where the right or the lower neighbor is a receiver of another cascade (not at silhouettes against the sky or pixels no
	// cascade covers).
	int right = CascadeAt(pixel + int2(1, 0));
	int below = CascadeAt(pixel + int2(0, 1));
	if ((right >= 0 && right != viewSample.Cascade) || (below >= 0 && below != viewSample.Cascade))
		color = float3(1.0);
	o_Color = float4(clamp(color, float3(0.0), float3(1.0)), 1.0);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
