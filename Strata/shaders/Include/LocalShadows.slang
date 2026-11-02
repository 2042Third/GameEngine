#ifndef STRATA_LOCAL_SHADOWS_SLANG
#define STRATA_LOCAL_SHADOWS_SLANG

// Shadows of point and spot lights (SceneRenderer's local shadows, DeveloperGuide "Local light shadows"): the shadowed
// lights' views are tiles of one depth atlas, a spot light one view, a point light six (a cube's faces), each a perspective
// view from the light (reversed-Z). Layouts mirror RenderData::LocalShadowData (SceneRenderData.h); keep them in sync.

#include "Include/Scene.slang"

#define ST_LOCAL_SHADOW_FACES 6u

struct LocalShadowData
{
	float4x4 ViewProjection[6]; // Camera-relative world space to each view's clip space: a spot light's view, or +X, -X, +Y, -Y, +Z, -Z
	// Per view: xy its tile's corner and z its size in atlas UV, w the world size of a texel at unit depth along the view
	float4 Tiles[6];
	float4 Parameters;        // x: depth bias, y: normal bias (texels), z: filter radius (texels), w: strength (0: unshadowed)
	float4 Light;             // xyz: the light's camera-relative position, w: 1 for a point light, 0 for a spot light
};

// Per shadow slot (LightData::SpotAngles.z of a shadowed light is its slot plus one).
StructuredBuffer<LocalShadowData> u_LocalShadows : register(t13);

#ifdef ST_LOCAL_SHADOW_SAMPLING
#include "Include/Shadows.slang"

// The views' depth atlas (reversed-Z: larger is nearer to the light), sampled through the cascades' comparison sampler.
Texture2D<float4> u_LocalShadowAtlas : register(t8);

static const float2 c_LocalShadowPoissonDisk[16] = { float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760), float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379), float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188), float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790) };

// Per-pixel rotation of the filter's taps (interleaved gradient noise) trades banding for fine noise. With temporal
// anti-aliasing every frame turns it further (TemporalParams.z), so the history averages the noise away. (The sun's filter
// uses a fixed blue-noise tile instead: Include/Shadows.slang.)
float2x2 LocalShadowSampleRotation(float2 pixel)
{
	float angle = TWO_PI * frac(frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) + u_Frame.TemporalParams.z);
	float s = sin(angle);
	float c = cos(angle);
	return transpose(float2x2(c, s, -s, c));
}

// The view of a shadowed light that sees a camera-relative position: a spot light's only view, or the cube face of a point
// light's major axis toward it (+X, -X, +Y, -Y, +Z, -Z, as SceneRenderer lays the faces out).
uint SelectLocalShadowView(uint slot, float3 relativePosition)
{
	if (u_LocalShadows[slot].Light.w < 0.5)
		return 0u;
	float3 direction = relativePosition - u_LocalShadows[slot].Light.xyz;
	float3 magnitude = abs(direction);
	if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
		return direction.x >= 0.0 ? 0u : 1u;
	if (magnitude.y >= magnitude.z)
		return direction.y >= 0.0 ? 2u : 3u;
	return direction.z >= 0.0 ? 4u : 5u;
}

// Fraction of a shadowed point or spot light's light reaching a surface (1 = fully lit, also outside its views), at a
// camera-relative position with its geometric normal (facing the viewer): percentage-closer filtering over 16 rotated taps
// within the filter radius, kept inside the view's tile (its guard band holds what the taps reach beyond the view's own
// region). The depth bias moves the lookup toward the light and the normal bias off the surface, both in texels of the tile
// at the surface's depth; the light's strength (its fade) mixes the result toward lit.
float SampleLocalShadow(uint slot, float3 relativePosition, float3 normal, float2 pixel)
{
	float4 parameters = u_LocalShadows[slot].Parameters;
	uint view = SelectLocalShadowView(slot, relativePosition);
	float4x4 viewProjection = u_LocalShadows[slot].ViewProjection[view];
	float4 tile = u_LocalShadows[slot].Tiles[view];
	float3 toLight = u_LocalShadows[slot].Light.xyz - relativePosition;
	float lightDistance = length(toLight);
	if (!(lightDistance > 0.0))
		return 1.0;
	float texelWorld = max((mul(viewProjection, float4(relativePosition, 1.0))).w, 0.0) * tile.w;
	float3 offsetPosition = relativePosition + normal * (parameters.y * texelWorld) + toLight * (parameters.x * texelWorld / lightDistance);
	float4 clip = mul(viewProjection, float4(offsetPosition, 1.0));
	if (!(clip.w > 0.0))
		return 1.0;
	float3 ndc = clip.xyz / clip.w;
	float2 tileUV = NDCToUV(ndc.xy);
	if (any((tileUV < float2(0.0))) || any((tileUV > float2(1.0))))
		return 1.0;
	float2 atlasTexel = 1.0 / float2(TextureSize(u_LocalShadowAtlas, 0));
	float2 uv = tile.xy + tileUV * tile.z;
	float2 lowest = tile.xy + 0.5 * atlasTexel;
	float2 highest = tile.xy + float2(tile.z) - 0.5 * atlasTexel;
	float2 radius = max(parameters.z, 0.0) * atlasTexel;
	float2x2 rotation = LocalShadowSampleRotation(pixel);
	// The comparison sampler tests reference < stored: true where a caster is nearer to the light (reversed-Z).
	float shadowed = 0.0;
	for (int index = 0; index < 16; index++)
	{
		float2 tap = clamp(uv + mul(rotation, c_LocalShadowPoissonDisk[index]) * radius, lowest, highest);
		// An explicit level: lights are shaded in non-uniform control flow, where implicit derivatives are undefined.
		shadowed += u_LocalShadowAtlas.SampleCmpLevelZero(u_ShadowCompareSampler, tap, ndc.z);
	}
	return lerp(1.0, 1.0 - shadowed / 16.0, clamp(parameters.w, 0.0, 1.0));
}
#endif

#endif
