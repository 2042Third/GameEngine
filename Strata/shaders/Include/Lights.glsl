#ifndef STRATA_LIGHTS_GLSL
#define STRATA_LIGHTS_GLSL

// The GPU-driven path's lights (Strata/Renderer/RenderLights.h): a scene's light records, culled into the light buffer
// (Scene/LightCull.comp, Scene/LightCompact.comp) that the light clusters (Scene/LightClusters.comp) and the forward pass
// read. Layouts mirror RenderData::LightRecord and LightCullParameters (SceneRenderData.h); keep them in sync.

#include "Include/Scene.glsl"

#define ST_LIGHT_GROUP_SIZE 64u
#define ST_COUNTER_VISIBLE_LIGHTS 21
#define ST_COUNTER_DRAWN_LIGHTS   22

struct LightRecord
{
	vec4 PositionRange; // xyz: world position, w: range
	vec4 Color;         // rgb: color * intensity, w: 1 / range^2
	vec4 DirectionType; // xyz: direction, w: type
	vec4 SpotAngles;    // x: cos(inner), y: cos(outer), z: 1 when drawn
	vec4 Bounds;        // xyz: world center, w: radius
};

layout(push_constant) uniform LightCullParameters
{
	vec4 Planes[6];   // The view frustum in camera-relative space, inward
	uint RecordCount; // Light records of the scene's GPU copy
	uint GroupCount;  // Work groups of ST_LIGHT_GROUP_SIZE records
	uint Padding0;
	uint Padding1;
} u_LightCull;

layout(set = 0, binding = ST_SRV(18), std430) readonly buffer LightRecordBuffer
{
	LightRecord u_LightRecords[];
};

#endif
