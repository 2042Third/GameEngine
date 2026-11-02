#include "Include/Common.slang"

#include "Include/Common.slang"
#include "GameUI/UIQuad.slang"

// The game UI's quads (GameUIRenderer): instanced, six vertices per quad from gl_VertexIndex, the quad from
// gl_InstanceIndex. Canvas units become pixels at the canvas's scale; edges (images) and baselines (glyphs) snap to whole
// pixels when the canvas asks for it. A world-space canvas stays in canvas units, placed in the scene by its transform.

static float2 v_Local;          // Pixels (world space: canvas units) from the quad's center (images: the rounded rect's distance)
static float2 v_HalfSize;  // Pixels (world space: canvas units)
static float3 v_TexCoord;       // Images: texture coordinates; glyphs: atlas texels and page
static float4 v_Color;
static float4 v_BorderColor;
static float4 v_Parameters; // x: corner radius (px), y: border (px) or outline (distance), z: kind, w: atlas slot
static uint v_Texture;   // Images: UI texture index + 1 (0: a solid color)
static float4 v_ClipRect;  // Pixels (world space: canvas units): x0, y0, x1, y1 of the masks' clip rect (x1 < x0: none)
static float v_ClipSoftness; // Pixels (world space: canvas units)
static float2 v_Position;       // Pixels (world space: canvas units): where the clip rect is tested
static uint v_World;    // 1 for a world-space canvas

static const float2 c_Corners[6] = { float2(0.0, 0.0), float2(1.0, 0.0), float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 1.0), float2(0.0, 1.0) };

void Hide()
{
	// A degenerate triangle: nothing is rasterized.
	st_Position = float4(0.0, 0.0, 0.0, 1.0);
	v_Local = float2(0.0);
	v_HalfSize = float2(0.0);
	v_TexCoord = float3(0.0);
	v_Color = float4(0.0);
	v_BorderColor = float4(0.0);
	v_Parameters = float4(0.0);
	v_Texture = 0u;
	v_ClipRect = float4(0.0, 0.0, -1.0, -1.0);
	v_ClipSoftness = 0.0;
	v_Position = float2(0.0);
	v_World = 0u;
}

void st_Main()
{
	UIQuad quad = u_Quads[st_InstanceIndex];
	uint kind = quad.Flags & UI_QUAD_KIND_MASK;
	if (kind == UI_QUAD_HIDDEN)
	{
		Hide();
		return;
	}
	float2 corner = c_Corners[st_VertexIndex % 6];
	// A world-space canvas stays in canvas units (its transform places it) and never snaps.
	bool world = (u_Draw.Flags & UI_DRAW_WORLD) != 0u;
	bool snap = !world && (u_Draw.Flags & UI_DRAW_PIXEL_SNAP) != 0u;
	float scale = world ? 1.0 : u_Draw.Scale;
	float2 origin = world ? float2(0.0) : u_Draw.Origin;
	float2 minimum = quad.Rect.xy * scale + origin;
	float2 maximum = quad.Rect.zw * scale + origin;

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
		uint2 placement = u_Glyphs[u_Draw.GlyphBases[slot] + quad.Resource];
		uint width = placement.y & 0xFFu;
		uint height = (placement.y >> 8) & 0xFFu;
		if (width == 0u)
		{
			Hide(); // Not in the atlas yet
			return;
		}
		float2 texel = float2(float(placement.x & 0xFFFFu), float(placement.x >> 16));
		v_TexCoord = float3(texel + corner * float2(float(width), float(height)), float(placement.y >> 16));
		if (snap)
		{
			// The whole glyph moves with its baseline onto a whole pixel, so glyphs keep their shapes.
			float baseline = quad.UV.y * scale + origin.y;
			float offset = round(baseline) - baseline;
			minimum.y += offset;
			maximum.y += offset;
		}
		// The outline's edge value: OutlineWidth em of distance field texels (FontAtlas: 32 per em) below the outline's.
		v_Parameters = float4(0.0, quad.BorderWidth * 32.0 * c_UIValuePerTexel, float(kind), float(slot));
		v_Texture = 0u;
	}
	else
	{
		if (snap)
		{
			minimum = round(minimum);
			maximum = round(maximum);
		}
		v_TexCoord = float3(lerp(quad.UV.xy, quad.UV.zw, corner), 0.0);
		v_Parameters = float4(quad.CornerRadius * scale, quad.BorderWidth * scale, float(kind), 0.0);
		v_Texture = quad.Resource;
	}

	// Masks: the quad's clip rect in pixels (on whole pixels like the rects); a quad entirely outside it draws nothing.
	v_ClipRect = float4(0.0, 0.0, -1.0, -1.0);
	v_ClipSoftness = 0.0;
	if (quad.Clip != 0u)
	{
		UIClipRect clip = u_Clips[u_Draw.FirstClip + quad.Clip - 1u];
		float2 clipMin = clip.Rect.xy * scale + origin;
		float2 clipMax = clip.Rect.zw * scale + origin;
		if (snap)
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
		v_ClipRect = float4(clipMin, clipMax);
		v_ClipSoftness = max(clip.Parameters.x, 0.0) * scale;
	}

	float2 size = maximum - minimum;
	float2 position = lerp(minimum, maximum, corner);
	v_HalfSize = size * 0.5;
	v_Local = (corner - 0.5) * size;
	v_Color = UnpackColor(quad.Color);
	v_BorderColor = UnpackColor(quad.BorderColor);
	v_Position = position;
	v_World = world ? 1u : 0u;
	if (world)
		st_Position = mul(u_Transforms[u_Draw.Flags >> UI_DRAW_TRANSFORM_SHIFT], float4(position, 0.0, 1.0));
	else
		st_Position = float4(UVToNDC(position / u_Draw.ViewportSize), 0.5, 1.0);
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float2 v_Local : TEXCOORD0;
	[[vk::location(1)]] nointerpolation float2 v_HalfSize : TEXCOORD1;
	[[vk::location(2)]] float3 v_TexCoord : TEXCOORD2;
	[[vk::location(3)]] nointerpolation float4 v_Color : TEXCOORD3;
	[[vk::location(4)]] nointerpolation float4 v_BorderColor : TEXCOORD4;
	[[vk::location(5)]] nointerpolation float4 v_Parameters : TEXCOORD5;
	[[vk::location(6)]] nointerpolation uint v_Texture : TEXCOORD6;
	[[vk::location(7)]] nointerpolation float4 v_ClipRect : TEXCOORD7;
	[[vk::location(8)]] nointerpolation float v_ClipSoftness : TEXCOORD8;
	[[vk::location(9)]] float2 v_Position : TEXCOORD9;
	[[vk::location(10)]] nointerpolation uint v_World : TEXCOORD10;
	float4 st_Position : SV_Position;
};
ST_Output main(ST_Input input, int vertexIndex : SV_VulkanVertexID, int instanceIndex : SV_VulkanInstanceID)
{
	st_VertexIndex = vertexIndex;
	st_InstanceIndex = instanceIndex;
	st_Main();
	ST_Output output;
	output.v_Local = v_Local;
	output.v_HalfSize = v_HalfSize;
	output.v_TexCoord = v_TexCoord;
	output.v_Color = v_Color;
	output.v_BorderColor = v_BorderColor;
	output.v_Parameters = v_Parameters;
	output.v_Texture = v_Texture;
	output.v_ClipRect = v_ClipRect;
	output.v_ClipSoftness = v_ClipSoftness;
	output.v_Position = v_Position;
	output.v_World = v_World;
	output.st_Position = st_Position;
	return output;
}
