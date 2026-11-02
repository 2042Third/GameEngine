#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/Exposure.slang"

// Lens flares at a quarter of the resolution (SceneRenderer::RenderLensFlare): what a camera's lens adds around very bright
// light. Ghosts: the lens's inner reflections image the bright part of the bloom chain's level 1 (its downsample, before the
// upsamples add the deeper levels) again, read through a cubic B-spline (soft, round shapes), mirrored through the
// image's center and scaled (four of them, each tinted by its coatings), fading toward the image's edges. A halo: a ring
// a little inside the frame where bright light near the center lands. The tone mapping adds the result, tent-filtered, at
// PostProcessComponent::LensFlare * c_LensFlareScale. Alpha holds the share of the directional light's disk that sees the
// sky (the same in every texel; 0 without a disk in front of the camera): the tone mapping draws the disk's starburst at
// full resolution with it, its rays being thinner near the disk than these texels.

Texture2D<float4> u_Bloom : register(t0); // The bloom chain's level 1 (quarter resolution, exposed)
SamplerState u_LinearClamp : register(s0);
Texture2D<float4> u_Depth : register(t1); // The frame's depth (reversed-Z: 0 is the sky)
StructuredBuffer<float> u_Exposure : register(t2);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Flare : register(u0);

// RenderData::LensFlareParameters (Renderer/PostProcessData.h).
struct LensFlareParameters
{
	float2 Sun;           // The directional light's disk, in UV (valid with ST_FLARE_SUN)
	float Aspect;       // Width / height
	uint Flags;         // ST_FLARE_SUN: the light's disk is in front of the camera
	float SunTapRadius; // The radius, in image heights, of the depth taps that see whether the disk is covered
	float Padding0;
	float Padding1;
	float Padding2;
};
[[vk::push_constant]] ConstantBuffer<LensFlareParameters> u_Parameters;

#define ST_FLARE_SUN 1u

// The ghosts: their scale through the center (a source at uv shows at 0.5 + (0.5 - uv) * k) and their tints.
static const float c_GhostScales[4] = { -1.6, -0.7, 0.45, 1.2 };
static const float3 c_GhostTints[4] = { float3(0.75, 0.55, 1.0), float3(0.55, 1.0, 0.7), float3(1.0, 0.7, 0.4), float3(0.45, 0.65, 1.0) };
// Only exposed light above this many times middle gray's display white makes ghosts and the halo, and its excess is
// compressed toward c_FlareCeiling: the sun (thousands) leaves ghosts as faint as a lamp's.
static const float c_FlareThreshold = 4.0;
static const float c_FlareCeiling = 16.0;
// The halo: a ring this far (in image heights) toward the center from where the light is.
static const float c_HaloRadius = 0.45;
static const float3 c_HaloTint = float3(1.0, 0.85, 0.7);

// The level at uv through a cubic B-spline of its texels, from four bilinear taps: each pair of texels along an axis is read
// by one tap placed where its weights' ratio puts it, and the pairs are weighted by their sums. Bilinear reads magnified
// into ghosts would show their texel grid as diamonds.
float3 SampleSmooth(float2 uv)
{
	float2 size = float2(TextureSize(u_Bloom, 0));
	float2 position = uv * size - 0.5; // Texel centers at whole numbers
	float2 base = floor(position);
	float2 f = position - base;
	float2 f2 = f * f;
	float2 f3 = f2 * f;
	float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	float2 w3 = f3 / 6.0;
	float2 g0 = w0 + w1;
	float2 g1 = w2 + w3;
	float2 uv0 = (base - 1.0 + w1 / g0 + 0.5) / size;
	float2 uv1 = (base + 1.0 + w3 / g1 + 0.5) / size;
	return g0.x * g0.y * u_Bloom.SampleLevel(u_LinearClamp, float2(uv0.x, uv0.y), 0.0).rgb
		+ g1.x * g0.y * u_Bloom.SampleLevel(u_LinearClamp, float2(uv1.x, uv0.y), 0.0).rgb
		+ g0.x * g1.y * u_Bloom.SampleLevel(u_LinearClamp, float2(uv0.x, uv1.y), 0.0).rgb
		+ g1.x * g1.y * u_Bloom.SampleLevel(u_LinearClamp, float2(uv1.x, uv1.y), 0.0).rgb;
}

float3 BrightPart(float2 uv)
{
	if (any((uv < float2(0.0))) || any((uv > float2(1.0))))
		return float3(0.0); // Light from outside the image is not known (a clamped edge would streak)
	float3 color = SampleSmooth(uv);
	float luminance = Luminance(color);
	if (!(luminance > c_FlareThreshold))
		return float3(0.0);
	float excess = luminance - c_FlareThreshold;
	return color * (excess / (1.0 + excess / c_FlareCeiling) / luminance);
}

// 1 near the image's center, falling to 0 at its corners (distance in image heights).
float EdgeFade(float2 uv)
{
	float distanceToCenter = length((uv - 0.5) * float2(u_Parameters.Aspect, 1.0));
	float corner = length(float2(0.5 * u_Parameters.Aspect, 0.5));
	return 1.0 - smoothstep(0.3 * corner, corner, distanceToCenter);
}

// The share of the disk's surroundings that sees the sky: 16 depth taps on two rings around it (every thread reads the same
// few texels).
float SunVisibility()
{
	if ((u_Parameters.Flags & ST_FLARE_SUN) == 0u)
		return 0.0;
	int2 depthSize = TextureSize(u_Depth, 0);
	float seen = 0.0;
	for (int tap = 0; tap < 16; tap++)
	{
		float angle = float(tap) * (TWO_PI / 8.0) + (tap < 8 ? 0.0 : PI / 8.0);
		float radius = u_Parameters.SunTapRadius * (tap < 8 ? 1.0 : 0.5);
		float2 tapUV = u_Parameters.Sun + float2(cos(angle) / u_Parameters.Aspect, sin(angle)) * radius;
		bool inside = all((tapUV >= float2(0.0))) && all((tapUV <= float2(1.0)));
		int2 tapPixel = clamp(int2(tapUV * float2(depthSize)), int2(0), depthSize - 1);
		seen += inside && u_Depth.Load(int3(tapPixel, 0)).r <= 0.0 ? 1.0 : 0.0;
	}
	return seen / 16.0;
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = ImageSize(o_Flare);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;
	float2 uv = (float2(pixel) + 0.5) / float2(size);

	float3 flare = float3(0.0);
	float fade = EdgeFade(uv);
	for (int ghost = 0; ghost < 4; ghost++)
	{
		float2 source = 0.5 + (0.5 - uv) / c_GhostScales[ghost];
		flare += BrightPart(source) * c_GhostTints[ghost] * fade;
	}
	// The halo gathers from the point c_HaloRadius toward the center, weighted by how near that point is to the center.
	float2 towardCenter = (0.5 - uv) * float2(u_Parameters.Aspect, 1.0);
	float centerDistance = length(towardCenter);
	if (centerDistance > 1e-4)
	{
		float2 source = uv + towardCenter / centerDistance * c_HaloRadius / float2(u_Parameters.Aspect, 1.0);
		float sourceDistance = length((source - 0.5) * float2(u_Parameters.Aspect, 1.0));
		flare += BrightPart(source) * c_HaloTint * (1.0 - smoothstep(0.0, 0.35, sourceDistance)) * fade;
	}
	o_Flare[pixel] = float4(min(flare, float3(c_MaxHDRValue)), SunVisibility());
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
