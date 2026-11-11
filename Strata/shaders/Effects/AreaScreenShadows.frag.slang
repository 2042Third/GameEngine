#include "Include/Common.slang"

#include "Include/AreaShadows.slang"

// A shadowed rect light's screen-space shadow mask (SceneRenderer::RenderAreaShadowMasks), as the sun's
// (Effects/ScreenShadows.frag): every opaque pixel of the depth prepass within the light's reach on screen (the pass's
// scissor) gets the light's filter (Include/AreaShadows.slang) at the surface the forward pass shades there, from the
// prepass's depth and geometric normal, and the radius Effects/ScreenShadowsBlur.frag smooths it over: none where the depth
// ranges classified it (lit, umbra) or the fixed tent alone filtered, 0.35 of the penumbra's half-width in pixels where the
// lattice's taps did, enough to take their grain. Output: red the square root of the visibility, green the blur radius over
// c_ScreenShadowMaxBlurPixels, at least 1/255 wherever the filter compared depths (a blur takes no radius under half a pixel:
// the mark only counts them, SceneRenderer::GetAreaShadowEvaluation). Pixels behind a one-sided light are lit (it lights
// nothing there).

Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Normals : register(t23); // The prepass's view-space geometric normal, octahedral

// RenderData::AreaShadowMaskParameters
struct AreaShadowMaskParameters
{
	float4 Center; // xyz: the face's camera-relative center, w: 1 when two-sided
	float4 AxisX;  // xyz: its half axis along its width
	float4 AxisY;
	float4 Normal; // xyz: the way it shines, w: its shadow slot
};
[[vk::push_constant]] ConstantBuffer<AreaShadowMaskParameters> u_Mask;

static float2 o_Mask;

static const float c_ScreenShadowMaxBlurPixels = 16.0; // Effects/ScreenShadowsBlur.frag with ST_AREA_SHADOW_BLUR

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	if (!(depth > 0.0))
	{
		o_Mask = float2(1.0, 0.0); // Nothing drawn
		return;
	}
	// The surface the forward pass shades at the pixel's center: the jittered matrices the prepass rasterized with.
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(uv), depth, 1.0));
	float3 position = point.xyz / point.w;
	float viewDepth = -(mul(u_Frame.View, float4(position, 1.0))).z;
	float3 normal = normalize(mul(((float3x3)(u_Frame.InverseView)), OctahedralDecode(u_Normals.Load(int3(pixel, 0)).xy)));

	AreaLight light;
	light.Center = u_Mask.Center.xyz;
	light.Normal = u_Mask.Normal.xyz;
	light.AxisX = u_Mask.AxisX.xyz;
	light.AxisY = u_Mask.AxisY.xyz;
	light.Radiance = float3(1.0);
	light.InverseRangeSquared = 0.0;
	light.TwoSided = u_Mask.Center.w > 0.5;
	if (GetAreaLightSide(position, light) == 0.0)
	{
		o_Mask = float2(1.0, 0.0);
		return;
	}
	AreaShadowSample shadow = EvaluateAreaLightShadow(uint(u_Mask.Normal.w + 0.5), position, normal, pixel, light);
	float blur = 0.0;
	if (shadow.DiskWeight > 0.0)
	{
		float pixelWorld = 2.0 * (u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth) / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
		float radiusPixels = shadow.PenumbraWorld / max(pixelWorld, 1e-12);
		blur = clamp(0.35 * radiusPixels * shadow.DiskWeight, 1.0, c_ScreenShadowMaxBlurPixels);
	}
	float radius = blur / c_ScreenShadowMaxBlurPixels;
	if (shadow.Compared)
		radius = max(radius, 1.0 / 255.0);
	o_Mask = float2(sqrt(clamp(shadow.Visibility, 0.0, 1.0)), radius);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float2 o_Mask : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Mask = o_Mask;
	return output;
}
