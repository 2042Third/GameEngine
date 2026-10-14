#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

#include "Include/Common.glsl"
#include "Include/SDFText.glsl"
#include "GameUI/UIQuad.glsl"

// The game UI's quads, blended in display values over the finished image: images (a texture from the bindless table, or a
// solid color) with rounded corners and borders from the rect's signed distance, and distance field glyphs with an
// optional outline, both clipped by the masks above them.

layout(set = 0, binding = ST_SRV(3)) uniform texture2DArray u_Atlas0; // The glyph atlases of the draw's fonts
layout(set = 0, binding = ST_SRV(4)) uniform texture2DArray u_Atlas1;
layout(set = 0, binding = ST_SRV(5)) uniform texture2DArray u_Atlas2;
layout(set = 0, binding = ST_SRV(6)) uniform texture2DArray u_Atlas3;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_LinearClamp;
layout(set = 1, binding = 0) uniform texture2D u_Textures[]; // Bindless table (BindlessTextureTable)

layout(location = 0) in vec2 v_Local;
layout(location = 1) flat in vec2 v_HalfSize;
layout(location = 2) in vec3 v_TexCoord;
layout(location = 3) flat in vec4 v_Color;
layout(location = 4) flat in vec4 v_BorderColor;
layout(location = 5) flat in vec4 v_Parameters;
layout(location = 6) flat in uint v_Texture;
layout(location = 7) flat in vec4 v_ClipRect;
layout(location = 8) flat in float v_ClipSoftness;
layout(location = 9) in vec2 v_Position;
layout(location = 10) flat in uint v_World;

layout(location = 0) out vec4 o_Color;

const float c_AtlasPageSize = 512.0; // FontAtlas::c_PageSize

float SampleAtlas(int slot, vec3 texel)
{
	// One level per atlas: an explicit level samples the same in every branch.
	vec3 coordinates = vec3(texel.xy / c_AtlasPageSize, texel.z);
	if (slot == 0)
		return textureLod(sampler2DArray(u_Atlas0, u_LinearClamp), coordinates, 0.0).r;
	if (slot == 1)
		return textureLod(sampler2DArray(u_Atlas1, u_LinearClamp), coordinates, 0.0).r;
	if (slot == 2)
		return textureLod(sampler2DArray(u_Atlas2, u_LinearClamp), coordinates, 0.0).r;
	return textureLod(sampler2DArray(u_Atlas3, u_LinearClamp), coordinates, 0.0).r;
}

// How much of the pixel the masks leave: 1 inside their clip rect, 0 outside (pixel centers decide, like the rects' edges),
// fading over the softness inside its edges.
float ClipCoverage()
{
	if (v_ClipRect.z < v_ClipRect.x)
		return 1.0;
	// On the screen the pixel's center, in the scene the point of the canvas the pixel shows.
	vec2 pixel = v_World != 0u ? v_Position : gl_FragCoord.xy;
	float inside = min(min(pixel.x - v_ClipRect.x, v_ClipRect.z - pixel.x), min(pixel.y - v_ClipRect.y, v_ClipRect.w - pixel.y));
	if (v_ClipSoftness > 0.0)
		return clamp(inside / v_ClipSoftness, 0.0, 1.0);
	return inside > 0.0 ? 1.0 : 0.0;
}

void main()
{
	float clipCoverage = ClipCoverage();
	if (clipCoverage <= 0.0)
		discard;
	uint kind = uint(v_Parameters.z + 0.5);
	if (kind == UI_QUAD_GLYPH)
	{
		float distanceValue = SampleAtlas(int(v_Parameters.w + 0.5), v_TexCoord);
		float fill = SDFCoverage(distanceValue, c_SDFOnEdgeValue);
		float outline = v_Parameters.y;
		if (outline > 0.0)
		{
			// The outline band around the glyph in its color, the glyph over it.
			float outer = SDFCoverage(distanceValue, c_SDFOnEdgeValue - outline);
			vec4 color = mix(v_BorderColor, v_Color, outer > 0.0 ? fill / outer : 0.0);
			float alpha = mix(v_BorderColor.a * outer, v_Color.a, fill) * clipCoverage;
			if (alpha <= 0.0)
				discard;
			o_Color = vec4(color.rgb, alpha);
			return;
		}
		if (fill <= 0.0)
			discard;
		o_Color = vec4(v_Color.rgb, v_Color.a * fill * clipCoverage);
		return;
	}

	vec4 color = v_Color;
	if (v_Texture != 0u)
	{
		uint packedSlot = u_TextureSlots[v_Texture - 1u];
		vec4 texel = texture(sampler2D(u_Textures[nonuniformEXT(packedSlot & 0xFFFFFFu)], u_LinearClamp), v_TexCoord.xy);
		if ((packedSlot & UI_TEXTURE_GRAYSCALE) != 0u)
			texel = vec4(texel.rrr, 1.0);
		// Blending is in display values: sRGB textures sample as linear values, converted back.
		if ((packedSlot & UI_TEXTURE_SRGB) != 0u)
			texel.rgb = LinearToSRGB(texel.rgb);
		color *= texel;
	}

	// The rect's signed distance in pixels (negative inside), with rounded corners; coverage over one pixel at the edge. A
	// world-space canvas measures in canvas units: its distances become pixels through how many units a pixel spans.
	float unitsPerPixel = v_World != 0u ? max(max(fwidth(v_Local.x), fwidth(v_Local.y)), 1e-6) : 1.0;
	float radius = min(v_Parameters.x, min(v_HalfSize.x, v_HalfSize.y));
	vec2 q = abs(v_Local) - (v_HalfSize - radius);
	float distanceToEdge = (length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius) / unitsPerPixel;
	float coverage = clamp(0.5 - distanceToEdge, 0.0, 1.0);
	float border = v_Parameters.y / unitsPerPixel;
	if (border > 0.0)
	{
		// The border's band inside the edge in its color, the fill inside it.
		float inner = clamp(0.5 - (distanceToEdge + border), 0.0, 1.0);
		color = mix(v_BorderColor, color, inner);
	}
	float alpha = color.a * coverage * clipCoverage;
	if (alpha <= 0.0)
		discard;
	o_Color = vec4(color.rgb, alpha);
}
