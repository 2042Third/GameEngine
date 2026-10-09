#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// What stb_truetype builds for a glyph: its distance field rasterizer visits every outline vertex for every texel, and
	// composite glyphs are assembled by transforming and copying their components' vertices.
	struct GlyphShapeCost
	{
		uint32_t Vertices = 0;        // After expanding composite glyphs (0: nothing to draw), saturating
		uint32_t Curves = 0;          // Of these, quadratic curves (costlier to measure distances to than lines)
		uint64_t CompositeCopies = 0; // Vertices transformed and copied while assembling a composite glyph, saturating
	};

	// What validating a TrueType font established (see ValidateTrueTypeFont).
	struct TrueTypeFontFacts
	{
		uint32_t FontOffset = 0; // Start of the font (collections: of their first font)
		// stb_truetype may read the font's kerning data: either it has none, or the table stb_truetype reads (GPOS when
		// present, otherwise kern) is fully bounded. Fonts with broken kerning are still usable, without kerning.
		bool KerningUsable = true;
		std::string KerningIssue; // Why kerning was disabled
		uint32_t KerningLookupVisits = 0; // GPOS lookups and pair adjustment subtables stb_truetype searches per glyph pair
		// Per glyph: the outline stb_truetype builds.
		std::vector<GlyphShapeCost> GlyphShapes;
	};

	namespace FontLimits
	{
		constexpr size_t c_MaxFontSize = 64ull * 1024 * 1024;
		constexpr uint32_t c_MaxCompositeDepth = 8;          // Nesting of composite glyphs
		constexpr uint32_t c_MaxCompositeComponents = 256;   // Components of a glyph after expanding nested composites
		// Points of all simple glyphs: flag repeats let two bytes describe 256 points, so the file size alone does not bound
		// the time to validate them. Real fonts have far fewer.
		constexpr uint64_t c_MaxOutlinePoints = 16ull * 1024 * 1024;
		constexpr double c_MaxTransformedCoordinate = 32000.0; // Composite transforms must stay within int16 coordinates
		// stb_truetype searches every GPOS lookup, and every subtable of the pair adjustment lookups, for each glyph pair it
		// kerns: fonts whose search is longer, or whose kerning tables take more steps to validate, get no kerning (real fonts
		// stay far below: the vendored ones search at most 13 and validate in under 1000 steps).
		constexpr uint32_t c_MaxKerningLookupVisits = 512;
		constexpr uint64_t c_MaxKerningValidationWork = 1024 * 1024;
	}

	// Verifies, before stb_truetype ever reads the font, everything it can dereference: the table directory and the tables
	// it uses (head, hhea, maxp, hmtx, loca, glyf, cmap, kern, GPOS) with their minimum sizes, numberOfHMetrics against
	// hmtx, loca entries increasing within glyf, the cmap subtable stb_truetype selects (formats 0, 4, 6, 12 and 13 with
	// their arrays bounded), and every glyph: contours, flags and coordinates of simple glyphs within their glyf range (at
	// most c_MaxOutlinePoints points in all), components of composite glyphs (valid indices, positioned by offsets, no
	// cycles, at most c_MaxCompositeDepth deep and c_MaxCompositeComponents components when expanded, transforms that keep
	// coordinates representable). Kerning that is not fully bounded (in what it reads, in the lookups searched per glyph
	// pair and in the work to validate it) is disabled instead of rejecting the font. Rejects CFF-flavoured OpenType fonts
	// ("OTTO"): stb_truetype parses their CFF data unbounded. Returns false with the reason for malformed or unsupported
	// fonts.
	bool ValidateTrueTypeFont(std::span<const uint8_t> data, TrueTypeFontFacts& outFacts, std::string& outError);

}
