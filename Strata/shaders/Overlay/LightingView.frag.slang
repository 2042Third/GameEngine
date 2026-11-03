#include "Include/Common.slang"

#include "Include/LightingView.slang"

// The lighting debug views that show values (SceneRenderOptions::LightingView: AmbientOcclusion, Albedo, Luminance) over the
// finished image, which they replace: from the frame's own HDR target (never the temporal history, so a value is never
// blended with an earlier frame's), as display colors (sRGB).
// - AmbientOcclusion: the occlusion as gray; pixels without a surface the background color.
// - Albedo: the base color; luminances below ST_ALBEDO_LOW mixed toward the low mark, above ST_ALBEDO_HIGH toward the high
//   mark in diagonal stripes (so a marked bright surface is not taken for a bright one); no surface: the background color.
// - Luminance: the luminance's EV100 band, one EV wide from LuminanceFirstEV100 on, the background (the sky's light) included.

Texture2D<float4> u_HDR : register(t0);
Texture2D<float4> u_Depth : register(t1);

static float4 o_Color;

static const float c_MarkStrength = 0.7;

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float3 hdr = u_HDR.Load(int3(pixel, 0)).rgb;
	bool surface = u_Depth.Load(int3(pixel, 0)).r > 0.0;
	float3 color;
	if (u_LightingView.View == ST_LIGHTING_VIEW_LUMINANCE)
	{
		int band = int(clamp(floor(GetPixelEV100(hdr) - u_LightingView.LuminanceFirstEV100), 0.0, float(ST_LIGHTING_VIEW_LUMINANCE_COLORS - 1)));
		color = GetLuminanceBandColor(band);
	}
	else if (!surface)
	{
		color = UnpackDisplayColor(u_LightingView.Background);
	}
	else if (u_LightingView.View == ST_LIGHTING_VIEW_AMBIENT_OCCLUSION || u_LightingView.View == ST_LIGHTING_VIEW_OPEN_SHARE)
	{
		color = LinearToSRGB(float3(clamp(hdr.r, 0.0, 1.0)));
	}
	else
	{
		color = LinearToSRGB(clamp(hdr, float3(0.0), float3(1.0)));
		float albedo = Luminance(hdr);
		if (albedo < ST_ALBEDO_LOW)
			color = lerp(color, UnpackDisplayColor(u_LightingView.AlbedoLow), c_MarkStrength);
		else if (albedo > ST_ALBEDO_HIGH && ((pixel.x + pixel.y) / 4) % 2 == 0)
			color = lerp(color, UnpackDisplayColor(u_LightingView.AlbedoHigh), c_MarkStrength);
	}
	o_Color = float4(color, 1.0);
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
