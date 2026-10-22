#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Selection outline over the finished image: pixels outside a selected entity's visible silhouette, within Width
// pixels of it, take the outline color. Selection comes from the prepass entity-id buffer, or with ST_OUTLINE_MASK
// (Overlay/OutlineMask.frag) from the selection mask (Scene/SelectionMask.frag), which temporal anti-aliasing's frames draw
// with the camera's own matrices so the outline does not move with the jitter.

#ifdef ST_OUTLINE_MASK
layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_SelectionMask;
#else
layout(set = 0, binding = ST_SRV(0)) uniform utexture2D u_EntityIDs;
layout(set = 0, binding = ST_SRV(1), std430) readonly buffer SelectionBuffer
{
	uint u_Selected[]; // Entity ids + 1, sorted ascending
};
#endif

layout(push_constant) uniform OutlineParameters
{
	vec4 Color;
	int Width;
	uint SelectedCount;
	uint Padding0;
	uint Padding1;
} u_Outline;

layout(location = 0) out vec4 o_Color;

#ifndef ST_OUTLINE_MASK
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
#endif

bool IsSelectedPixel(ivec2 pixel)
{
#ifdef ST_OUTLINE_MASK
	return texelFetch(u_SelectionMask, pixel, 0).r > 0.5;
#else
	return IsSelected(texelFetch(u_EntityIDs, pixel, 0).r);
#endif
}

void main()
{
#ifdef ST_OUTLINE_MASK
	ivec2 size = textureSize(u_SelectionMask, 0);
#else
	ivec2 size = textureSize(u_EntityIDs, 0);
#endif
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	if (IsSelectedPixel(pixel))
		discard;

	int width = u_Outline.Width;
	for (int y = -width; y <= width; y++)
	{
		for (int x = -width; x <= width; x++)
		{
			ivec2 neighbor = pixel + ivec2(x, y);
			if (x * x + y * y > width * width || any(lessThan(neighbor, ivec2(0))) || any(greaterThanEqual(neighbor, size)))
				continue;
			if (IsSelectedPixel(neighbor))
			{
				o_Color = u_Outline.Color;
				return;
			}
		}
	}
	discard;
}
