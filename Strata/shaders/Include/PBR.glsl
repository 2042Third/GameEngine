#ifndef STRATA_PBR_GLSL
#define STRATA_PBR_GLSL

// Physically based shading (glTF 2.0 metallic-roughness model): GGX distribution, height-correlated Smith
// visibility, Schlick Fresnel and Lambertian diffuse.

#include "Include/Common.glsl"

const float c_MinRoughness = 0.045; // Avoids aliasing and division issues on mirror-like surfaces

float DistributionGGX(float NoH, float alpha)
{
	float alpha2 = alpha * alpha;
	float f = (NoH * alpha2 - NoH) * NoH + 1.0;
	return alpha2 / (PI * f * f);
}

float VisibilitySmithGGXCorrelated(float NoV, float NoL, float alpha)
{
	float alpha2 = alpha * alpha;
	float lambdaV = NoL * sqrt((NoV - NoV * alpha2) * NoV + alpha2);
	float lambdaL = NoV * sqrt((NoL - NoL * alpha2) * NoL + alpha2);
	return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

vec3 FresnelSchlick(vec3 f0, float VoH)
{
	float f = pow(1.0 - VoH, 5.0);
	return f + f0 * (1.0 - f);
}

vec3 FresnelSchlickRoughness(vec3 f0, float NoV, float roughness)
{
	return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - NoV, 5.0);
}

struct SurfaceData
{
	vec3 Position;
	vec3 Normal;
	vec3 View;        // Surface to camera, normalized
	vec3 Albedo;
	float Alpha;
	float Metallic;
	float Roughness;  // Perceptual roughness
	float Occlusion;
	vec3 Emissive;
};

// Radiance reflected toward the viewer from light arriving along `lightDirection` (surface to light) with the
// given irradiance at normal incidence.
vec3 EvaluateLight(SurfaceData surface, vec3 lightDirection, vec3 radiance)
{
	vec3 H = normalize(surface.View + lightDirection);
	float NoL = clamp(dot(surface.Normal, lightDirection), 0.0, 1.0);
	if (NoL <= 0.0)
		return vec3(0.0);
	float NoV = max(dot(surface.Normal, surface.View), 1e-4);
	float NoH = clamp(dot(surface.Normal, H), 0.0, 1.0);
	float VoH = clamp(dot(surface.View, H), 0.0, 1.0);

	float roughness = max(surface.Roughness, c_MinRoughness);
	float alpha = roughness * roughness;
	vec3 f0 = mix(vec3(0.04), surface.Albedo, surface.Metallic);

	vec3 F = FresnelSchlick(f0, VoH);
	vec3 specular = DistributionGGX(NoH, alpha) * VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * F;
	vec3 diffuse = (1.0 - F) * (1.0 - surface.Metallic) * surface.Albedo * INV_PI;
	return (diffuse + specular) * radiance * NoL;
}

// Smooth distance falloff for punctual lights: inverse square, windowed to reach zero at the range.
float DistanceAttenuation(float distanceSquared, float inverseRangeSquared)
{
	float ratio = distanceSquared * inverseRangeSquared;
	float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
	return window * window / max(distanceSquared, 1e-4);
}

float SpotAttenuation(vec3 lightToSurface, vec3 spotDirection, float cosInner, float cosOuter)
{
	float cd = dot(normalize(lightToSurface), spotDirection);
	float t = clamp((cd - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
	return t * t;
}

#endif
