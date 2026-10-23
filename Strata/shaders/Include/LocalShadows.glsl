#ifndef STRATA_LOCAL_SHADOWS_GLSL
#define STRATA_LOCAL_SHADOWS_GLSL

// Shadows of point and spot lights (SceneRenderer's local shadows, DeveloperGuide "Local light shadows"): the shadowed
// lights' views are tiles of one depth atlas, a spot light one view, a point light six (a cube's faces), each a perspective
// view from the light (reversed-Z). Layouts mirror RenderData::LocalShadowData (SceneRenderData.h); keep them in sync.

#include "Include/Scene.glsl"

#define ST_LOCAL_SHADOW_FACES 6u

struct LocalShadowData
{
	mat4 ViewProjection[6]; // Camera-relative world space to each view's clip space: a spot light's view, or +X, -X, +Y, -Y, +Z, -Z
	// Per view: xy its tile's corner and z its size in atlas UV, w the world size of a texel at unit depth along the view
	vec4 Tiles[6];
	vec4 Parameters;        // x: depth bias, y: normal bias (texels), z: filter radius (texels), w: strength (0: unshadowed)
	vec4 Light;             // xyz: the light's camera-relative position, w: 1 for a point light, 0 for a spot light
};

// Per shadow slot (LightData::SpotAngles.z of a shadowed light is its slot plus one).
layout(set = 0, binding = ST_SRV(13), std430) readonly buffer LocalShadowBuffer
{
	LocalShadowData u_LocalShadows[];
};

#ifdef ST_LOCAL_SHADOW_SAMPLING
#include "Include/Shadows.glsl"

// The views' depth atlas (reversed-Z: larger is nearer to the light), sampled through the cascades' comparison sampler.
layout(set = 0, binding = ST_SRV(8)) uniform texture2D u_LocalShadowAtlas;

// The view of a shadowed light that sees a camera-relative position: a spot light's only view, or the cube face of a point
// light's major axis toward it (+X, -X, +Y, -Y, +Z, -Z, as SceneRenderer lays the faces out).
uint SelectLocalShadowView(uint slot, vec3 relativePosition)
{
	if (u_LocalShadows[slot].Light.w < 0.5)
		return 0u;
	vec3 direction = relativePosition - u_LocalShadows[slot].Light.xyz;
	vec3 magnitude = abs(direction);
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
float SampleLocalShadow(uint slot, vec3 relativePosition, vec3 normal, vec2 pixel)
{
	vec4 parameters = u_LocalShadows[slot].Parameters;
	uint view = SelectLocalShadowView(slot, relativePosition);
	mat4 viewProjection = u_LocalShadows[slot].ViewProjection[view];
	vec4 tile = u_LocalShadows[slot].Tiles[view];
	vec3 toLight = u_LocalShadows[slot].Light.xyz - relativePosition;
	float lightDistance = length(toLight);
	if (!(lightDistance > 0.0))
		return 1.0;
	float texelWorld = max((viewProjection * vec4(relativePosition, 1.0)).w, 0.0) * tile.w;
	vec3 offsetPosition = relativePosition + normal * (parameters.y * texelWorld) + toLight * (parameters.x * texelWorld / lightDistance);
	vec4 clip = viewProjection * vec4(offsetPosition, 1.0);
	if (!(clip.w > 0.0))
		return 1.0;
	vec3 ndc = clip.xyz / clip.w;
	vec2 tileUV = NDCToUV(ndc.xy);
	if (any(lessThan(tileUV, vec2(0.0))) || any(greaterThan(tileUV, vec2(1.0))))
		return 1.0;
	vec2 atlasTexel = 1.0 / vec2(textureSize(u_LocalShadowAtlas, 0));
	vec2 uv = tile.xy + tileUV * tile.z;
	vec2 lowest = tile.xy + 0.5 * atlasTexel;
	vec2 highest = tile.xy + vec2(tile.z) - 0.5 * atlasTexel;
	vec2 radius = max(parameters.z, 0.0) * atlasTexel;
	mat2 rotation = ShadowSampleRotation(pixel);
	// The comparison sampler tests reference < stored: true where a caster is nearer to the light (reversed-Z).
	float shadowed = 0.0;
	for (int index = 0; index < 16; index++)
	{
		vec2 tap = clamp(uv + rotation * c_PoissonDisk[index] * radius, lowest, highest);
		// An explicit level: lights are shaded in non-uniform control flow, where implicit derivatives are undefined.
		shadowed += textureLod(sampler2DShadow(u_LocalShadowAtlas, u_ShadowCompareSampler), vec3(tap, ndc.z), 0.0);
	}
	return mix(1.0, 1.0 - shadowed / 16.0, clamp(parameters.w, 0.0, 1.0));
}
#endif

#endif
