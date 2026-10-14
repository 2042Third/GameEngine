#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"
#include "GameUI/UIQuad.glsl"

// The game UI's quads (GameUIRenderer): instanced, six vertices per quad from gl_VertexIndex, the quad from
// gl_InstanceIndex. Canvas units become pixels at the canvas's scale; edges (images) and baselines (glyphs) snap to whole
// pixels when the canvas asks for it.

layout(location = 0) out vec2 v_Local;          // Pixels from the quad's center (images: the rounded rect's distance)
layout(location = 1) flat out vec2 v_HalfSize;  // Pixels
layout(location = 2) out vec3 v_TexCoord;       // Images: texture coordinates; glyphs: atlas texels and page
layout(location = 3) flat out vec4 v_Color;
layout(location = 4) flat out vec4 v_BorderColor;
layout(location = 5) flat out vec4 v_Parameters; // x: corner radius (px), y: border (px) or outline (distance), z: kind, w: atlas slot
layout(location = 6) flat out uint v_Texture;   // Images: UI texture index + 1 (0: a solid color)
layout(location = 7) flat out vec4 v_ClipRect;  // Pixels: x0, y0, x1, y1 of the masks' clip rect (x1 < x0: none)
layout(location = 8) flat out float v_ClipSoftness; // Pixels

const vec2 c_Corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void Hide()
{
	// A degenerate triangle: nothing is rasterized.
	gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
	v_Local = vec2(0.0);
	v_HalfSize = vec2(0.0);
	v_TexCoord = vec3(0.0);
	v_Color = vec4(0.0);
	v_BorderColor = vec4(0.0);
	v_Parameters = vec4(0.0);
	v_Texture = 0u;
	v_ClipRect = vec4(0.0, 0.0, -1.0, -1.0);
	v_ClipSoftness = 0.0;
}

void main()
{
	UIQuad quad = u_Quads[gl_InstanceIndex];
	uint kind = quad.Flags & UI_QUAD_KIND_MASK;
	if (kind == UI_QUAD_HIDDEN)
	{
		Hide();
		return;
	}
	vec2 corner = c_Corners[gl_VertexIndex % 6];
	vec2 minimum = quad.Rect.xy * u_Draw.Scale + u_Draw.Origin;
	vec2 maximum = quad.Rect.zw * u_Draw.Scale + u_Draw.Origin;

	if (kind == UI_QUAD_GLYPH)
	{
		// The glyph's place in the atlas of its font, from the glyph table of the slot its font is bound to.
		uint font = (quad.Flags >> UI_QUAD_FONT_SHIFT) & UI_QUAD_FONT_MASK;
		int slot = -1;
		for (int index = 0; index < 4; index++)
		{
			if (u_Draw.Fonts[index] == font)
				slot = index;
		}
		if (slot < 0)
		{
			Hide();
			return;
		}
		uvec2 placement = u_Glyphs[u_Draw.GlyphBases[slot] + quad.Resource];
		uint width = placement.y & 0xFFu;
		uint height = (placement.y >> 8) & 0xFFu;
		if (width == 0u)
		{
			Hide(); // Not in the atlas yet
			return;
		}
		vec2 texel = vec2(float(placement.x & 0xFFFFu), float(placement.x >> 16));
		v_TexCoord = vec3(texel + corner * vec2(float(width), float(height)), float(placement.y >> 16));
		if (u_Draw.PixelSnap != 0u)
		{
			// The whole glyph moves with its baseline onto a whole pixel, so glyphs keep their shapes.
			float baseline = quad.UV.y * u_Draw.Scale + u_Draw.Origin.y;
			float offset = round(baseline) - baseline;
			minimum.y += offset;
			maximum.y += offset;
		}
		// The outline's edge value: OutlineWidth em of distance field texels (FontAtlas: 32 per em) below the outline's.
		v_Parameters = vec4(0.0, quad.BorderWidth * 32.0 * c_UIValuePerTexel, float(kind), float(slot));
		v_Texture = 0u;
	}
	else
	{
		if (u_Draw.PixelSnap != 0u)
		{
			minimum = round(minimum);
			maximum = round(maximum);
		}
		v_TexCoord = vec3(mix(quad.UV.xy, quad.UV.zw, corner), 0.0);
		v_Parameters = vec4(quad.CornerRadius * u_Draw.Scale, quad.BorderWidth * u_Draw.Scale, float(kind), 0.0);
		v_Texture = quad.Resource;
	}

	// Masks: the quad's clip rect in pixels (on whole pixels like the rects); a quad entirely outside it draws nothing.
	v_ClipRect = vec4(0.0, 0.0, -1.0, -1.0);
	v_ClipSoftness = 0.0;
	if (quad.Clip != 0u)
	{
		UIClipRect clip = u_Clips[u_Draw.FirstClip + quad.Clip - 1u];
		vec2 clipMin = clip.Rect.xy * u_Draw.Scale + u_Draw.Origin;
		vec2 clipMax = clip.Rect.zw * u_Draw.Scale + u_Draw.Origin;
		if (u_Draw.PixelSnap != 0u)
		{
			clipMin = round(clipMin);
			clipMax = round(clipMax);
		}
		if (!(clipMax.x > clipMin.x && clipMax.y > clipMin.y) || maximum.x <= clipMin.x || minimum.x >= clipMax.x || maximum.y <= clipMin.y
			|| minimum.y >= clipMax.y)
		{
			Hide();
			return;
		}
		v_ClipRect = vec4(clipMin, clipMax);
		v_ClipSoftness = max(clip.Parameters.x, 0.0) * u_Draw.Scale;
	}

	vec2 size = maximum - minimum;
	vec2 position = mix(minimum, maximum, corner);
	v_HalfSize = size * 0.5;
	v_Local = (corner - 0.5) * size;
	v_Color = UnpackColor(quad.Color);
	v_BorderColor = UnpackColor(quad.BorderColor);
	gl_Position = vec4(UVToNDC(position / u_Draw.ViewportSize), 0.5, 1.0);
}
