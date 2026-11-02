#include "Include/Common.slang"

#include "Include/Common.slang"
#include "Include/SDFText.slang"
#include "GameUI/UIQuad.slang"

// The game UI's quads, blended in display values over the finished image: images (a texture from the bindless table, or a
// solid color) with rounded corners and borders from the rect's signed distance, and distance field glyphs with an
// optional outline, both clipped by the masks above them.

Texture2DArray<float4> u_Atlas0 : register(t3); // The glyph atlases of the draw's fonts
Texture2DArray<float4> u_Atlas1 : register(t4);
Texture2DArray<float4> u_Atlas2 : register(t5);
Texture2DArray<float4> u_Atlas3 : register(t6);
SamplerState u_LinearClamp : register(s0);
Texture2D<float4> u_Textures[] : register(t0, space1); // Bindless table (BindlessTextureTable)

static float2 v_Local;
static float2 v_HalfSize;
static float3 v_TexCoord;
static float4 v_Color;
static float4 v_BorderColor;
static float4 v_Parameters;
static uint v_Texture;
static float4 v_ClipRect;
static float v_ClipSoftness;
static float2 v_Position;
static uint v_World;

static float4 o_Color;

static const float c_AtlasPageSize = 512.0; // FontAtlas::c_PageSize

float SampleAtlas(int slot, float3 texel)
{
	// One level per atlas: an explicit level samples the same in every branch.
	float3 coordinates = float3(texel.xy / c_AtlasPageSize, texel.z);
	if (slot == 0)
		return u_Atlas0.SampleLevel(u_LinearClamp, coordinates, 0.0).r;
	if (slot == 1)
		return u_Atlas1.SampleLevel(u_LinearClamp, coordinates, 0.0).r;
	if (slot == 2)
		return u_Atlas2.SampleLevel(u_LinearClamp, coordinates, 0.0).r;
	return u_Atlas3.SampleLevel(u_LinearClamp, coordinates, 0.0).r;
}

// How much of the pixel the masks leave: 1 inside their clip rect, 0 outside (pixel centers decide, like the rects' edges),
// fading over the softness inside its edges.
float ClipCoverage()
{
	if (v_ClipRect.z < v_ClipRect.x)
		return 1.0;
	// On the screen the pixel's center, in the scene the point of the canvas the pixel shows.
	float2 pixel = v_World != 0u ? v_Position : st_FragCoord.xy;
	float inside = min(min(pixel.x - v_ClipRect.x, v_ClipRect.z - pixel.x), min(pixel.y - v_ClipRect.y, v_ClipRect.w - pixel.y));
	if (v_ClipSoftness > 0.0)
		return clamp(inside / v_ClipSoftness, 0.0, 1.0);
	return inside > 0.0 ? 1.0 : 0.0;
}

// A glyph's color and coverage: the distance field's fill, and the outline band around it in its color.
float4 ShadeGlyph()
{
	float distanceValue = SampleAtlas(int(v_Parameters.w + 0.5), v_TexCoord);
	float fill = SDFCoverage(distanceValue, c_SDFOnEdgeValue);
	float outline = v_Parameters.y;
	if (outline <= 0.0)
		return float4(v_Color.rgb, v_Color.a * fill);
	// The outline band around the glyph in its color, the glyph over it.
	float outer = SDFCoverage(distanceValue, c_SDFOnEdgeValue - outline);
	float4 color = lerp(v_BorderColor, v_Color, outer > 0.0 ? fill / outer : 0.0);
	return float4(color.rgb, lerp(v_BorderColor.a * outer, v_Color.a, fill));
}

// An image's color and coverage: its texture (or solid color) inside the rounded rect, with its border.
float4 ShadeImage()
{
	float4 color = v_Color;
	if (v_Texture != 0u)
	{
		uint packedSlot = u_TextureSlots[v_Texture - 1u];
		float4 texel = u_Textures[NonUniformResourceIndex(packedSlot & 0xFFFFFFu)].Sample(u_LinearClamp, v_TexCoord.xy);
		if ((packedSlot & UI_TEXTURE_GRAYSCALE) != 0u)
			texel = float4(texel.rrr, 1.0);
		// Blending is in display values: sRGB textures sample as linear values, converted back.
		if ((packedSlot & UI_TEXTURE_SRGB) != 0u)
			texel.rgb = LinearToSRGB(texel.rgb);
		color *= texel;
	}

	// The rect's signed distance in pixels (negative inside), with rounded corners; coverage over one pixel at the edge. A
	// world-space canvas measures in canvas units: its distances become pixels through how many units a pixel spans.
	float unitsPerPixel = v_World != 0u ? max(max(fwidth(v_Local.x), fwidth(v_Local.y)), 1e-6) : 1.0;
	float radius = min(v_Parameters.x, min(v_HalfSize.x, v_HalfSize.y));
	float2 q = abs(v_Local) - (v_HalfSize - radius);
	float distanceToEdge = (length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - radius) / unitsPerPixel;
	float coverage = clamp(0.5 - distanceToEdge, 0.0, 1.0);
	float border = v_Parameters.y / unitsPerPixel;
	if (border > 0.0)
	{
		// The border's band inside the edge in its color, the fill inside it.
		float inner = clamp(0.5 - (distanceToEdge + border), 0.0, 1.0);
		color = lerp(v_BorderColor, color, inner);
	}
	return float4(color.rgb, color.a * coverage);
}

void st_Main()
{
	// Derivatives (the glyphs' coverage, the images' mip level, a world-space canvas's units per pixel) need every pixel of
	// the 2x2 quad: a discard that differs per pixel, as the masks' clip does, leaves them undefined for the rest of the
	// shader. So the clip multiplies the shaded result, and the one discard comes last. The kind is the same for the whole
	// quad (flat), so either branch runs in uniform control flow.
	float clipCoverage = ClipCoverage();
	float4 shaded = uint(v_Parameters.z + 0.5) == UI_QUAD_GLYPH ? ShadeGlyph() : ShadeImage();
	float alpha = shaded.a * clipCoverage;
	if (alpha <= 0.0)
		discard;
	o_Color = float4(shaded.rgb, alpha);
}

// --- Entry point ---
struct ST_Input
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
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_Local = input.v_Local;
	v_HalfSize = input.v_HalfSize;
	v_TexCoord = input.v_TexCoord;
	v_Color = input.v_Color;
	v_BorderColor = input.v_BorderColor;
	v_Parameters = input.v_Parameters;
	v_Texture = input.v_Texture;
	v_ClipRect = input.v_ClipRect;
	v_ClipSoftness = input.v_ClipSoftness;
	v_Position = input.v_Position;
	v_World = input.v_World;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
