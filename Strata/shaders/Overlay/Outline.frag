#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Selection outline over the finished image: pixels outside a selected entity's visible silhouette, within Width
// pixels of it, take the outline color. Selection comes from the prepass entity-id buffer.

layout(set = 0, binding = ST_SRV(0)) uniform utexture2D u_EntityIDs;
layout(set = 0, binding = ST_SRV(1), std430) readonly buffer SelectionBuffer
{
	uint u_Selected[]; // Entity ids + 1, sorted ascending
};

layout(push_constant) uniform OutlineParameters
{
	vec4 Color;
	int Width;
	uint SelectedCount;
	uint Padding0;
	uint Padding1;
} u_Outline;

layout(location = 0) out vec4 o_Color;

bool IsSelected(uint id)
{
	if (id == 0u)
		return false;
	uint low = 0u;
	uint high = u_Outline.SelectedCount;
	while (low < high)
	{
		uint middle = (low + high) / 2u;
		uint value = u_Selected[middle];
		if (value == id)
			return true;
		if (value < id)
			low = middle + 1u;
		else
			high = middle;
	}
	return false;
}

void main()
{
	ivec2 size = textureSize(u_EntityIDs, 0);
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	if (IsSelected(texelFetch(u_EntityIDs, pixel, 0).r))
		discard;

	int width = u_Outline.Width;
	for (int y = -width; y <= width; y++)
	{
		for (int x = -width; x <= width; x++)
		{
			ivec2 neighbor = pixel + ivec2(x, y);
			if (x * x + y * y > width * width || any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size)))
				continue;
			if (IsSelected(texelFetch(u_EntityIDs, neighbor, 0).r))
			{
				o_Color = u_Outline.Color;
				return;
			}
		}
	}
	discard;
}
