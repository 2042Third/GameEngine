#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"
#include "Include/Exposure.glsl"

// Exposure, bloom, tone mapping and color grading from the HDR scene color into a display-referred, sRGB-encoded
// target.

#define ST_TONEMAP_NONE            0
#define ST_TONEMAP_REINHARD        1
#define ST_TONEMAP_ACES            2
#define ST_TONEMAP_AGX             3
#define ST_TONEMAP_KHRONOS_NEUTRAL 4

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_SceneColor;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_PointSampler;
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_Bloom;
layout(set = 0, binding = ST_SAMPLER(1)) uniform sampler u_LinearSampler;
layout(set = 0, binding = ST_SRV(2), std430) readonly buffer ExposureBuffer
{
	float u_Exposure[];
};

layout(push_constant) uniform TonemapParameters
{
	float BloomIntensity; // Fraction of the light scattered into the glow; 0 = off
	float BloomScale;     // Normalizes the sum of the bloom levels
	float Saturation;
	float Vignette;
	float Contrast;
	int Operator;
	uint BloomAdditive;   // 1: the bloom holds only the light above a threshold and is added
	float Padding;
} u_Parameters;

// Largest exposed value passed to the tone curves: far beyond where every curve saturates, small enough that their
// intermediate products stay finite in 32-bit floats.
const float c_MaxExposedValue = 1.0e8;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

// ACES filmic curve fitted by Stephen Hill (RRT + ODT, sRGB primaries).
vec3 TonemapACES(vec3 color)
{
	const mat3 inputMatrix = mat3(
		0.59719, 0.07600, 0.02840,
		0.35458, 0.90834, 0.13383,
		0.04823, 0.01566, 0.83777);
	const mat3 outputMatrix = mat3(
		1.60475, -0.10208, -0.00327,
		-0.53108, 1.10813, -0.07276,
		-0.07367, -0.00605, 1.07602);
	color = inputMatrix * color;
	vec3 a = color * (color + 0.0245786) - 0.000090537;
	vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
	return clamp(outputMatrix * (a / b), 0.0, 1.0);
}

// AgX base transform with a polynomial fit of the default contrast curve.
vec3 AgXContrast(vec3 x)
{
	vec3 x2 = x * x;
	vec3 x4 = x2 * x2;
	return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 TonemapAgX(vec3 color)
{
	const mat3 inset = mat3(
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outset = mat3(
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	const float minEV = -12.47393;
	const float maxEV = 4.026069;

	color = inset * max(color, vec3(1e-10));
	color = clamp((log2(color) - minEV) / (maxEV - minEV), 0.0, 1.0);
	color = AgXContrast(color);
	color = outset * color;
	// The curve output is display-encoded (gamma 2.2); return linear values for the shared sRGB encode.
	return pow(max(color, vec3(0.0)), vec3(2.2));
}

// Khronos PBR Neutral tone mapper (keeps base colors accurate up to the highlights).
vec3 TonemapKhronosNeutral(vec3 color)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;
	float x = min(color.r, min(color.g, color.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	color -= offset;
	float peak = max(color.r, max(color.g, color.b));
	if (peak < startCompression)
		return color;
	float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	color *= newPeak / peak;
	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return mix(color, vec3(newPeak), g);
}

void main()
{
	// Inf or NaN (from a broken input or an extreme exposure) would make every tone curve return NaN.
	vec3 color = texture(sampler2D(u_SceneColor, u_PointSampler), v_UV).rgb * u_Exposure[ST_EXPOSURE_MULTIPLIER];
	color = clamp(mix(color, vec3(0.0), isnan(color)), vec3(0.0), vec3(c_MaxExposedValue));
	if (u_Parameters.BloomIntensity > 0.0)
	{
		// The bloom is already exposed. Without a threshold it holds all the light, and a small fraction of it
		// scatters into the glow (energy-conserving mix). With a threshold it holds only the bright excess, which is
		// added on top: mixing would darken everything that does not bloom.
		vec3 bloom = texture(sampler2D(u_Bloom, u_LinearSampler), v_UV).rgb * u_Parameters.BloomScale;
		if (u_Parameters.BloomAdditive != 0u)
			color += bloom * u_Parameters.BloomIntensity;
		else
			color = mix(color, bloom, u_Parameters.BloomIntensity);
		color = min(color, vec3(c_MaxExposedValue));
	}
	// Contrast around middle gray, in log space before the tone curve.
	if (u_Parameters.Contrast != 1.0)
		color = 0.18 * pow(color / 0.18, vec3(u_Parameters.Contrast));

	switch (u_Parameters.Operator)
	{
		case ST_TONEMAP_REINHARD:        color = color / (1.0 + color); break;
		case ST_TONEMAP_ACES:            color = TonemapACES(color); break;
		case ST_TONEMAP_AGX:             color = TonemapAgX(color); break;
		case ST_TONEMAP_KHRONOS_NEUTRAL: color = TonemapKhronosNeutral(color); break;
		default:                         color = clamp(color, 0.0, 1.0); break;
	}

	float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
	color = max(mix(vec3(luminance), color, u_Parameters.Saturation), vec3(0.0));

	vec2 centered = v_UV - 0.5;
	color *= 1.0 - u_Parameters.Vignette * smoothstep(0.2, 0.8, length(centered) * 1.41421356);

	o_Color = vec4(LinearToSRGB(clamp(color, 0.0, 1.0)), 1.0);
}
