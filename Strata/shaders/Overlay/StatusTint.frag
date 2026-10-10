#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Debug-view tint over the finished image (SceneRenderOptions::EntityTints): a pixel whose entity in the prepass
// entity-id buffer is listed takes the listed color, blended by its alpha; every other pixel is discarded, so it stays
// exactly as it was.

layout(set = 0, binding = ST_SRV(0)) uniform utexture2D u_EntityIDs;
layout(set = 0, binding = ST_SRV(1), std430) readonly buffer TintBuffer
{
	uvec2 u_Tints[]; // (entity id + 1, RGBA8 color packed like packUnorm4x8), sorted by id ascending
};

layout(push_constant) uniform TintParameters
{
	uint Count;
	uint Padding0;
	uint Padding1;
	uint Padding2;
} u_Tint;

layout(location = 0) out vec4 o_Color;

void main()
{
	uint id = texelFetch(u_EntityIDs, ivec2(gl_FragCoord.xy), 0).r;
	if (id == 0u)
		discard;

	uint low = 0u;
	uint high = u_Tint.Count;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		uvec2 tint = u_Tints[middle];
		if (tint.x == id)
		{
			o_Color = unpackUnorm4x8(tint.y);
			return;
		}
		if (tint.x < id)
			low = middle + 1u;
		else
			high = middle;
	}
	discard;
}
