#ifndef STRATA_UI_QUAD_SLANG
#define STRATA_UI_QUAD_SLANG

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
static const float c_UIValuePerTexel = 32.0 / 255.0;

struct UIQuad
{
	float4 Rect; // x0, y0, x1, y1 in canvas units
	float4 UV;   // Images: u0, v0, u1, v1; glyphs: the pen on the baseline (canvas units)
	uint Color;
	uint Resource; // Images: UI texture index + 1; glyphs: glyph index
	uint Flags;
	uint BorderColor;
	float CornerRadius;
	float BorderWidth;
	uint Clip;
	uint Padding;
};

StructuredBuffer<UIQuad> u_Quads : register(t0);

StructuredBuffer<uint> u_TextureSlots : register(t1);

StructuredBuffer<uint2> u_Glyphs : register(t2);

struct UIClipRect
{
	float4 Rect;       // x0, y0, x1, y1 in canvas units
	float4 Parameters; // x: softness in canvas units
};

StructuredBuffer<UIClipRect> u_Clips : register(t7);

// World-space canvases: canvas units to clip space, one per canvas the frame draws in the scene.
StructuredBuffer<float4x4> u_Transforms : register(t8);

#define UI_DRAW_PIXEL_SNAP 1u
#define UI_DRAW_WORLD 2u
#define UI_DRAW_TRANSFORM_SHIFT 8u

struct DrawParameters
{
	float2 ViewportSize;
	float2 Origin;
	float Scale;
	uint Flags; // UI_DRAW_PIXEL_SNAP, UI_DRAW_WORLD and the world transform's index
	uint FirstQuad;
	uint FirstClip; // The canvas's clip rects: a quad's Clip n (n > 0) is u_Clips[FirstClip + n - 1]
	uint4 Fonts;
	uint4 GlyphBases;
};
[[vk::push_constant]] ConstantBuffer<DrawParameters> u_Draw;

float4 UnpackColor(uint color)
{
	return float4(float(color & 0xFFu), float((color >> 8) & 0xFFu), float((color >> 16) & 0xFFu), float(color >> 24)) / 255.0;
}

#endif
