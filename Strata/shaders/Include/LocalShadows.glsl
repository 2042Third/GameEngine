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

#endif
