#pragma once

#include <glm/glm.hpp>

#include <cstdint>

// The game UI's GPU data, shared with Strata/shaders/GameUI/UIQuad.vert and UIQuad.frag (keep them in step).
namespace Strata::RenderData
{

	// UIQuad::Flags: the kind in bits 0-1, the scene font index of a glyph in bits 8-15.
	constexpr uint32_t c_UIQuadHidden = 0u; // Draws nothing (an empty slot, an inactive element)
	constexpr uint32_t c_UIQuadImage = 1u;  // A solid color, or a texture (UIQuad::Resource is its UI texture index + 1)
	constexpr uint32_t c_UIQuadGlyph = 2u;  // A glyph of a font's distance field atlas (UIQuad::Resource is its glyph index)
	constexpr uint32_t c_UIQuadKindMask = 3u;
	constexpr uint32_t c_UIQuadFontShift = 8u;
	constexpr uint32_t c_UIQuadFontMask = 0xFFu;
	// Fonts one scene's UI can use at once (scene font indices fit the flags' font bits).
	constexpr uint32_t c_MaxUIFonts = 256;

	// The UI texture slot table (one uint per UI texture index): the bindless slot in bits 0-23, and these bits.
	constexpr uint32_t c_UITextureGrayscale = 1u << 31; // One channel standing for gray, sampled as (r, r, r, 1)
	constexpr uint32_t c_UITextureSRGB = 1u << 30;      // Sampled values are linear: converted back to display values

	// The glyph table (two uints per glyph of each font in use): x | y << 16 and width | height << 8 | page << 16, all in
	// atlas texels; a width of 0 means the glyph is not in the atlas (yet): its quad draws nothing.

	// One quad of a canvas, in canvas units (+Y down), built by GameUIScene in hierarchy (draw) order. Images: Rect, UV,
	// Color, the rounded-rect corner radius and border (BorderColor, BorderWidth). Glyphs: Rect (the glyph's distance
	// field quad), UV.xy the pen on the baseline (pixel snapping moves the glyph by its baseline), Color, and the outline
	// (BorderColor, BorderWidth in em). 64 bytes.
	struct UIQuad
	{
		glm::vec4 Rect = glm::vec4(0.0f); // x0, y0, x1, y1
		glm::vec4 UV = glm::vec4(0.0f);   // u0, v0, u1, v1
		uint32_t Color = 0;               // RGBA8, display color
		uint32_t Resource = 0;
		uint32_t Flags = c_UIQuadHidden;
		uint32_t BorderColor = 0;         // RGBA8, display color
		float CornerRadius = 0.0f;        // Canvas units
		float BorderWidth = 0.0f;         // Canvas units (images), em (glyph outlines)
		uint32_t Clip = 0;                // Reserved for masks: 0 clips nothing
		uint32_t Padding = 0;
	};
	static_assert(sizeof(UIQuad) == 64);

	// Per draw (push constants): how a canvas maps to the viewport, and the fonts whose atlases the draw binds.
	struct UIDrawParameters
	{
		glm::vec2 ViewportSize = glm::vec2(1.0f); // Pixels
		glm::vec2 Origin = glm::vec2(0.0f);       // The canvas's top-left corner, in pixels
		float Scale = 1.0f;                       // Pixels per canvas unit
		uint32_t PixelSnap = 1;
		uint32_t FirstQuad = 0;                   // Of the draw, in the quad buffer
		uint32_t Padding = 0;
		glm::uvec4 Fonts = glm::uvec4(~0u);       // Scene font index bound to each atlas slot (~0: none)
		glm::uvec4 GlyphBases = glm::uvec4(0u);   // Each slot's font's first entry in the glyph table
	};
	static_assert(sizeof(UIDrawParameters) == 64);

	// Atlases bound per draw: a canvas using up to this many fonts draws in one call.
	constexpr uint32_t c_MaxBoundAtlases = 4;

}
