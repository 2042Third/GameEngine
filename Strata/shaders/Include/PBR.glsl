#ifndef STRATA_PBR_GLSL
#define STRATA_PBR_GLSL

// Physically based shading (glTF 2.0 metallic-roughness model): GGX distribution, height-correlated Smith
// visibility, Schlick Fresnel and Lambertian diffuse, with the specular lobe compensated for the light GGX loses to
// multiple scattering between microfacets (GetSpecularEnergy).

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

// The smallest directional albedo the compensation divides by. The BRDF table's x + y stays above 0.2 for every NoV and
// roughness it holds; this only keeps a broken table finite.
const float c_MinSpecularAlbedo = 0.05;

// Multiple-scattering compensation of the specular lobe: the factor its light is multiplied by. `dfg` is the BRDF table
// (IBL/BRDFLut.comp) at the surface's NoV and roughness: its x + y is the single-scattering lobe's directional albedo E for
// F0 = 1, which reflects only E of the light; the rest scatters again between the microfacets and leaves after further
// bounces, each of which keeps F0 of it. 1 + F0 (1 / E - 1) restores that light: exact for F0 = 1 (the lobe then reflects
// everything), less for colored metals, whose further bounces absorb 1 - F0 each.
vec3 GetSpecularEnergy(vec3 f0, vec2 dfg)
{
	return 1.0 + f0 * (1.0 / max(dfg.x + dfg.y, c_MinSpecularAlbedo) - 1.0);
}

// Geometric specular anti-aliasing: a highlight narrower than a pixel lands on some pixels and misses their neighbors, and
// sparkles as the surface moves. The shading normal's change over a pixel's footprint is a spread of normals within it, which
// a lobe widened by that variance integrates (it keeps the highlight's energy and spreads it over the pixels it covers).
// Screen-space variance of the normal over a pixel's filter footprint, per unit of its squared derivatives.
const float c_SpecularAAVariance = 0.25;
// The most the lobe's alpha^2 is widened: silhouettes and seams, where the normal jumps, would otherwise turn matte.
const float c_SpecularAAMaxKernel = 0.18;

// The perceptual roughness widened by the variance of the normal over the pixel: `normalDx` and `normalDy` are the screen
// derivatives of the unit shading normal; `strength` 0 returns the roughness unchanged (MaterialParams.z). Derivatives are
// taken only of the primitive's own interpolants and textures, so both draw paths shade alike (Docs/DeveloperGuide.md,
// "Lighting").
float WidenRoughness(float perceptualRoughness, vec3 normalDx, vec3 normalDy, float strength)
{
	float variance = c_SpecularAAVariance * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
	float kernel = min(2.0 * variance, c_SpecularAAMaxKernel) * strength;
	float alpha = perceptualRoughness * perceptualRoughness;
	return sqrt(sqrt(clamp(alpha * alpha + kernel, 0.0, 1.0)));
}

// How far below the geometric surface image light may still be reflected: a normal map can turn the reflected vector into
// the surface, where it would see the surface itself; the fade reaches 0 about 15 degrees below it.
const float c_HorizonFadeWidth = 0.25;

// The share of image light a reflection along `reflected` keeps against the surface's `geometricNormal` (both unit).
float GetHorizonFade(vec3 reflected, vec3 geometricNormal)
{
	float fade = clamp(1.0 + dot(reflected, geometricNormal) / c_HorizonFadeWidth, 0.0, 1.0);
	return fade * fade;
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
	vec3 SpecularEnergy; // GetSpecularEnergy at the surface's NoV and roughness: multiplies its specular light
};

// EvaluateLight with its diffuse part weighted by `diffuseWeight`: a transmissive surface passes the rest of the light its
// diffuse part would scatter through itself (Scene/Forward.frag's glass).
vec3 EvaluateLightWeighted(SurfaceData surface, vec3 lightDirection, vec3 radiance, float diffuseWeight)
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
	vec3 specular = DistributionGGX(NoH, alpha) * VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * F * surface.SpecularEnergy;
	// Diffuse keeps 1 - F: the compensated light leaves a dielectric's specular under 0.5% brighter, below what the diffuse
	// lobe's approximation already is off by.
	vec3 diffuse = (1.0 - F) * (1.0 - surface.Metallic) * surface.Albedo * INV_PI * diffuseWeight;
	return (diffuse + specular) * radiance * NoL;
}

// Radiance reflected toward the viewer from light arriving along `lightDirection` (surface to light) with the
// given irradiance at normal incidence.
vec3 EvaluateLight(SurfaceData surface, vec3 lightDirection, vec3 radiance)
{
	return EvaluateLightWeighted(surface, lightDirection, radiance, 1.0);
}

// A clear coat over a surface (MaterialProperties::Clearcoat): a dielectric layer of index 1.5 (reflecting 4% at normal
// incidence) with its own roughness and normal.
struct ClearcoatData
{
	float Strength;  // 0: no coat
	float Roughness; // Perceptual
	vec3 Normal;
	float SpecularEnergy; // GetSpecularEnergy of the coat's lobe (F0 c_ClearcoatF0) at its NoV and roughness
};

const float c_ClearcoatF0 = 0.04;

// The coat's Fresnel reflectance at the angle `cosine` (between its normal, or the half vector, and the view), times its
// strength: what the coat reflects, and what of the light no longer reaches the base below.
float ClearcoatFresnel(ClearcoatData coat, float cosine)
{
	return coat.Strength * (c_ClearcoatF0 + (1.0 - c_ClearcoatF0) * pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0));
}

// Radiance the coat reflects toward the viewer (`view`, surface to camera) from light arriving along `lightDirection`; with
// `baseWeight` what of that light passes the coat to the base (1 - the coat's Fresnel at the half vector).
vec3 EvaluateClearcoat(ClearcoatData coat, vec3 view, vec3 lightDirection, vec3 radiance, out float baseWeight)
{
	vec3 H = normalize(view + lightDirection);
	float fresnel = ClearcoatFresnel(coat, dot(view, H));
	baseWeight = 1.0 - fresnel;
	float NoL = clamp(dot(coat.Normal, lightDirection), 0.0, 1.0);
	if (NoL <= 0.0)
		return vec3(0.0);
	float NoV = max(dot(coat.Normal, view), 1e-4);
	float NoH = clamp(dot(coat.Normal, H), 0.0, 1.0);
	float roughness = max(coat.Roughness, c_MinRoughness);
	float alpha = roughness * roughness;
	return DistributionGGX(NoH, alpha) * VisibilitySmithGGXCorrelated(NoV, NoL, alpha) * fresnel * coat.SpecularEnergy * radiance * NoL;
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
