#ifndef STRATA_UI_QUAD_GLSL
#define STRATA_UI_QUAD_GLSL

// The game UI's GPU data: Strata/src/Strata/GameUI/GameUIRenderData.h (keep them in step).

#define UI_QUAD_HIDDEN 0u
#define UI_QUAD_IMAGE 1u
#define UI_QUAD_GLYPH 2u
#define UI_QUAD_KIND_MASK 3u
#define UI_QUAD_FONT_SHIFT 8u
#define UI_QUAD_FONT_MASK 0xFFu
#define UI_TEXTURE_GRAYSCALE 0x80000000u
#define UI_TEXTURE_SRGB 0x40000000u

// Distance field value per atlas texel (FontAtlas: 128 / 255 over 4 texels).
const float c_UIValuePerTexel = 32.0 / 255.0;

struct UIQuad
{
	vec4 Rect; // x0, y0, x1, y1 in canvas units
	vec4 UV;   // Images: u0, v0, u1, v1; glyphs: the pen on the baseline (canvas units)
	uint Color;
	uint Resource; // Images: UI texture index + 1; glyphs: glyph index
	uint Flags;
	uint BorderColor;
	float CornerRadius;
	float BorderWidth;
	uint Clip;
	uint Padding;
};

layout(set = 0, binding = ST_SRV(0), std430) readonly buffer QuadBuffer
{
	UIQuad u_Quads[];
};

layout(set = 0, binding = ST_SRV(1), std430) readonly buffer TextureSlotBuffer
{
	uint u_TextureSlots[];
};

layout(set = 0, binding = ST_SRV(2), std430) readonly buffer GlyphBuffer
{
	uvec2 u_Glyphs[];
};

struct UIClipRect
{
	vec4 Rect;       // x0, y0, x1, y1 in canvas units
	vec4 Parameters; // x: softness in canvas units
};

layout(set = 0, binding = ST_SRV(7), std430) readonly buffer ClipBuffer
{
	UIClipRect u_Clips[];
};

// World-space canvases: canvas units to clip space, one per canvas the frame draws in the scene.
layout(set = 0, binding = ST_SRV(8), std430) readonly buffer TransformBuffer
{
	mat4 u_Transforms[];
};

#define UI_DRAW_PIXEL_SNAP 1u
#define UI_DRAW_WORLD 2u
#define UI_DRAW_TRANSFORM_SHIFT 8u

layout(push_constant) uniform DrawParameters
{
	vec2 ViewportSize;
	vec2 Origin;
	float Scale;
	uint Flags; // UI_DRAW_PIXEL_SNAP, UI_DRAW_WORLD and the world transform's index
	uint FirstQuad;
	uint FirstClip; // The canvas's clip rects: a quad's Clip n (n > 0) is u_Clips[FirstClip + n - 1]
	uvec4 Fonts;
	uvec4 GlyphBases;
} u_Draw;

vec4 UnpackColor(uint color)
{
	return vec4(float(color & 0xFFu), float((color >> 8) & 0xFFu), float((color >> 16) & 0xFFu), float(color >> 24)) / 255.0;
}

#endif
