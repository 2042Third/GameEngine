#ifndef STRATA_LIGHTS_SLANG
#define STRATA_LIGHTS_SLANG

// The GPU-driven path's lights (Strata/Renderer/RenderLights.h): a scene's light records, culled into the light buffer
// (Scene/LightCull.comp, Scene/LightCompact.comp) that the light clusters (Scene/LightClusters.comp) and the forward pass
// read. Layouts mirror RenderData::LightRecord and LightCullParameters (SceneRenderData.h); keep them in sync.

#include "Include/Scene.slang"

#define ST_LIGHT_GROUP_SIZE 64u
#define ST_COUNTER_VISIBLE_LIGHTS 21
#define ST_COUNTER_DRAWN_LIGHTS   22

struct LightRecord
{
	float4 PositionRange; // xyz: world position, w: range
	float4 Color;         // rgb: color * intensity, w: 1 / range^2
	float4 DirectionType; // xyz: direction, w: type
	float4 SpotAngles;    // x: cos(inner), y: cos(outer), z: 1 when drawn
	float4 Bounds;        // xyz: world center, w: radius
};

struct LightCullParameters
{
	float4 Planes[6];   // The view frustum in camera-relative space, inward
	uint RecordCount; // Light records of the scene's GPU copy
	uint GroupCount;  // Work groups of ST_LIGHT_GROUP_SIZE records
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<LightCullParameters> u_LightCull;

StructuredBuffer<LightRecord> u_LightRecords : register(t18);

#endif
