#include <doctest/doctest.h>

#include "Renderer/FontTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/FontValidation.h"
#include "Strata/Renderer/TextLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	// Patches the tables of a TrueType font in place (big-endian fields addressed by absolute file offsets).
	class FontPatcher
	{
	public:
		explicit FontPatcher(std::vector<uint8_t> data)
			: Data(std::move(data))
		{
			const uint32_t tableCount = U16(4);
			for (uint32_t index = 0; index < tableCount; index++)
			{
				const size_t record = 12 + 16 * static_cast<size_t>(index);
				m_Records.emplace(std::string(reinterpret_cast<const char*>(Data.data() + record), 4), record);
			}
		}

		std::vector<uint8_t> Data;

		uint16_t U16(size_t offset) const { return static_cast<uint16_t>((Data[offset] << 8) | Data[offset + 1]); }
		uint32_t U32(size_t offset) const { return (static_cast<uint32_t>(U16(offset)) << 16) | U16(offset + 2); }
		void SetU16(size_t offset, uint16_t value)
		{
			Data[offset] = static_cast<uint8_t>(value >> 8);
			Data[offset + 1] = static_cast<uint8_t>(value);
		}
		void SetU32(size_t offset, uint32_t value)
		{
			SetU16(offset, static_cast<uint16_t>(value >> 16));
			SetU16(offset + 2, static_cast<uint16_t>(value));
		}

		bool HasTable(const std::string& tag) const { return m_Records.count(tag) != 0; }
		size_t Record(const std::string& tag) const
		{
			REQUIRE(HasTable(tag));
			return m_Records.at(tag);
		}
		size_t Table(const std::string& tag) const { return U32(Record(tag) + 8); }
		uint32_t TableLength(const std::string& tag) const { return U32(Record(tag) + 12); }
		void SetTableLength(const std::string& tag, uint32_t length) { SetU32(Record(tag) + 12, length); }
		std::vector<std::string> GetTags() const
		{
			std::vector<std::string> tags;
			for (const auto& [tag, record] : m_Records)
				tags.push_back(tag);
			return tags;
		}

		uint32_t GlyphCount() const { return U16(Table("maxp") + 4); }
		// Absolute start and end of a glyph's outline data.
		std::pair<size_t, size_t> Glyph(uint32_t glyph) const
		{
			const bool longOffsets = U16(Table("head") + 50) == 1;
			const size_t loca = Table("loca");
			const size_t first = longOffsets ? U32(loca + 4 * static_cast<size_t>(glyph)) : 2 * static_cast<size_t>(U16(loca + 2 * static_cast<size_t>(glyph)));
			const size_t last = longOffsets ? U32(loca + 4 * static_cast<size_t>(glyph) + 4) : 2 * static_cast<size_t>(U16(loca + 2 * static_cast<size_t>(glyph) + 2));
			return { Table("glyf") + first, Table("glyf") + last };
		}
		int16_t Contours(uint32_t glyph) const
		{
			const auto [start, end] = Glyph(glyph);
			return end > start ? static_cast<int16_t>(U16(start)) : int16_t(0);
		}
		std::vector<uint32_t> GlyphsWhere(bool composite, int16_t minimumContours = 1) const
		{
			std::vector<uint32_t> glyphs;
			for (uint32_t glyph = 0; glyph < GlyphCount(); glyph++)
			{
				const int16_t contours = Contours(glyph);
				if (composite ? contours < 0 : contours >= minimumContours)
					glyphs.push_back(glyph);
			}
			return glyphs;
		}

		// The character map record stb_truetype uses: the last Microsoft Unicode or Unicode-platform record.
		size_t SelectedCmapRecord() const
		{
			const size_t cmap = Table("cmap");
			size_t selected = 0;
			for (uint32_t index = 0; index < U16(cmap + 2); index++)
			{
				const size_t record = cmap + 4 + 8 * static_cast<size_t>(index);
				const uint16_t platform = U16(record);
				const uint16_t encoding = U16(record + 2);
				if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
					selected = record;
			}
			REQUIRE(selected != 0);
			return selected;
		}
		size_t SelectedCmapSubtable() const { return Table("cmap") + U32(SelectedCmapRecord() + 4); }

		// Absolute offsets of a composite glyph's component records (flags, then the component's glyph index).
		std::vector<size_t> Components(uint32_t glyph) const
		{
			std::vector<size_t> components;
			size_t cursor = Glyph(glyph).first + 10;
			bool more = true;
			while (more)
			{
				const uint16_t flags = U16(cursor);
				components.push_back(cursor);
				cursor += 4 + ((flags & 0x0001) ? 4 : 2);
				cursor += (flags & 0x0008) ? 2 : ((flags & 0x0040) ? 4 : ((flags & 0x0080) ? 8 : 0));
				more = (flags & 0x0020) != 0;
			}
			return components;
		}

		// The first pair adjustment subtable of the GPOS table, as stb_truetype finds it.
		size_t FirstPairAdjustment() const
		{
			const size_t lookupList = Table("GPOS") + U16(Table("GPOS") + 8);
			for (uint32_t lookup = 0; lookup < U16(lookupList); lookup++)
			{
				const size_t lookupTable = lookupList + U16(lookupList + 2 + 2 * static_cast<size_t>(lookup));
				if (U16(lookupTable) == 2 && U16(lookupTable + 4) > 0)
					return lookupTable + U16(lookupTable + 6);
			}
			FAIL("The font has no pair adjustment lookup");
			return 0;
		}
	private:
		std::map<std::string, size_t> m_Records;
	};

	FontPatcher DefaultFont()
	{
		return FontPatcher(Font::GetDefault()->GetData());
	}

	// The font with one of its tables replaced by `table`, appended to the file.
	std::vector<uint8_t> ReplaceTable(const FontPatcher& font, const std::string& tag, const std::vector<uint8_t>& table)
	{
		FontPatcher patched = font;
		patched.Data.resize((patched.Data.size() + 3) & ~size_t(3), 0);
		patched.SetU32(patched.Record(tag) + 8, static_cast<uint32_t>(patched.Data.size()));
		patched.SetU32(patched.Record(tag) + 12, static_cast<uint32_t>(table.size()));
		patched.Data.insert(patched.Data.end(), table.begin(), table.end());
		return patched.Data;
	}

	void Write16(std::vector<uint8_t>& data, size_t offset, uint32_t value)
	{
		data[offset] = static_cast<uint8_t>(value >> 8);
		data[offset + 1] = static_cast<uint8_t>(value);
	}

	void Write32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
	{
		Write16(data, offset, value >> 16);
		Write16(data, offset + 2, value & 0xFFFF);
	}

	// A GPOS table whose lookup list points `lookups` times at one pair adjustment lookup, whose subtable list points
	// `subtables` times at one subtable. Its coverage has `ranges` ranges, one of them `first`, the others glyphs the font
	// lacks; its one pair set kerns `first` followed by `second` by `advance` font units.
	std::vector<uint8_t> SharedPairPositioning(uint32_t lookups, uint32_t subtables, uint32_t ranges, uint32_t first, uint32_t second, int16_t advance)
	{
		const size_t lookupList = 10;
		const size_t lookup = lookupList + 2 + 2 * static_cast<size_t>(lookups);
		const size_t subtable = lookup + 6 + 2 * static_cast<size_t>(subtables);
		const size_t coverage = subtable + 14;
		const size_t pairSet = coverage + 4 + 6 * static_cast<size_t>(ranges);
		REQUIRE(lookup - lookupList <= 0xFFFF);
		REQUIRE(subtable - lookup <= 0xFFFF);
		REQUIRE(pairSet - subtable <= 0xFFFF);
		std::vector<uint8_t> gpos(pairSet + 6, 0);
		Write16(gpos, 0, 1); // Version 1.0
		Write16(gpos, 8, static_cast<uint32_t>(lookupList));
		Write16(gpos, lookupList, lookups);
		for (size_t index = 0; index < lookups; index++)
			Write16(gpos, lookupList + 2 + 2 * index, static_cast<uint32_t>(lookup - lookupList));
		Write16(gpos, lookup, 2); // Pair adjustment
		Write16(gpos, lookup + 4, subtables);
		for (size_t index = 0; index < subtables; index++)
			Write16(gpos, lookup + 6 + 2 * index, static_cast<uint32_t>(subtable - lookup));
		Write16(gpos, subtable, 1);      // Pairs of glyphs
		Write16(gpos, subtable + 2, static_cast<uint32_t>(coverage - subtable));
		Write16(gpos, subtable + 4, 4);  // X advance of the first glyph only
		Write16(gpos, subtable + 8, 1);  // One pair set
		Write16(gpos, subtable + 10, static_cast<uint32_t>(pairSet - subtable));
		Write16(gpos, coverage, 2);      // Glyph ranges, sorted
		Write16(gpos, coverage + 2, ranges);
		for (size_t range = 0; range < ranges; range++)
		{
			const uint32_t glyph = range == 0 ? first : 60000 + static_cast<uint32_t>(range);
			Write16(gpos, coverage + 4 + 6 * range, glyph);
			Write16(gpos, coverage + 6 + 6 * range, glyph);
		}
		Write16(gpos, pairSet, 1);
		Write16(gpos, pairSet + 2, second);
		Write16(gpos, pairSet + 4, static_cast<uint16_t>(advance));
		return gpos;
	}

	// Font::Create must reject the bytes, with an error containing `reason`.
	void CheckRejected(std::vector<uint8_t> data, const std::string& reason)
	{
		CAPTURE(reason);
		std::string error;
		CHECK_FALSE(Font::Create(std::move(data), &error));
		CAPTURE(error);
		CHECK(error.find(reason) != std::string::npos);
	}

	// Lowers the log level for the duration of a test that provokes many warnings on purpose.
	class QuietLog
	{
	public:
		QuietLog() { Log::SetLevel(LogLevel::Critical); }
		~QuietLog() { Log::SetLevel(LogLevel::Warn); }

		QuietLog(const QuietLog&) = delete;
		QuietLog& operator=(const QuietLog&) = delete;
	};

	const char* const c_VendoredTrueTypeFonts[] = {
		"Strata/vendor/imgui/misc/fonts/Roboto-Medium.ttf",
		"Strata/vendor/imgui/misc/fonts/Cousine-Regular.ttf",
		"Strata/vendor/imgui/misc/fonts/DroidSans.ttf",
		"Strata/vendor/imgui/misc/fonts/Karla-Regular.ttf",
		"Strata/vendor/imgui/misc/fonts/ProggyClean.ttf",
		"Strata/vendor/imgui/misc/fonts/ProggyTiny.ttf",
		"Strata/vendor/JoltPhysics/Assets/Fonts/Roboto-Regular.ttf",
		"Strata/vendor/tracy/profiler/src/font/FiraCode-Retina.ttf",
		"Strata/vendor/tracy/profiler/src/font/Roboto-Bold.ttf",
		"Strata/vendor/tracy/profiler/src/font/Roboto-Italic.ttf",
		"Strata/vendor/tracy/profiler/src/font/NotoEmoji-Regular.ttf"
	};

}

TEST_SUITE("Renderer.Font")
{
	TEST_CASE("Fonts with TrueType outlines are accepted and rasterize")
	{
		for (const char* path : c_VendoredTrueTypeFonts)
		{
			CAPTURE(path);
			std::string error;
			Ref<Font> font = Font::Create(Tests::ReadSourceFile(path), &error);
			REQUIRE_MESSAGE(font, error);
			CHECK(font->GetGlyphCount() > 0);
			CHECK(font->GetMemoryUsage() >= font->GetData().size());
			Scope<FontAtlas> atlas = FontAtlas::Create(font, &error);
			REQUIRE_MESSAGE(atlas, error);
			CHECK(atlas->GetGlyph('A').Visible);
			CHECK(font->HasUsableKerning());
		}

		const Ref<Font>& roboto = Font::GetDefault();
		CHECK(roboto->HasUsableKerning());
		CHECK(roboto->GetFontOffset() == 0);
		CHECK(roboto->GetGlyphPointCount(roboto->GetGlyphCount()) == 0); // Out of range
	}

	TEST_CASE("Font collections use their first font")
	{
		// A collection header in front of the font; its table offsets move along.
		FontPatcher font = DefaultFont();
		for (const std::string& tag : font.GetTags())
			font.SetU32(font.Record(tag) + 8, static_cast<uint32_t>(font.Table(tag)) + 16);
		std::vector<uint8_t> collection = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 16 };
		collection.insert(collection.end(), font.Data.begin(), font.Data.end());

		std::string error;
		Ref<Font> loaded = Font::Create(collection, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetFontOffset() == 16);
		Scope<FontAtlas> atlas = FontAtlas::Create(loaded, &error);
		REQUIRE(atlas);
		CHECK(atlas->GetGlyph('A').Visible);

		collection[12] = 0xF0; // First font outside the file
		CheckRejected(collection, "collection");
		collection[12] = 0;
		collection[7] = 9; // Unknown collection version
		CheckRejected(collection, "collection");
	}

	TEST_CASE("Unsupported and invalid font files are rejected")
	{
		CheckRejected({}, "truncated");
		CheckRejected(std::vector<uint8_t>(FontLimits::c_MaxFontSize + 1), "larger than");
		CheckRejected({ 'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't', '!', '!' }, "Not a TrueType font");

		// CFF outlines are not supported: a real OpenType/CFF font and the default font relabelled as one.
		CheckRejected(Tests::ReadSourceFile("Strata/vendor/tracy/profiler/src/font/Font Awesome 7 Free-Solid-900.otf"), "CFF");
		FontPatcher relabelled = DefaultFont();
		std::memcpy(relabelled.Data.data(), "OTTO", 4);
		CheckRejected(relabelled.Data, "CFF");

		FontPatcher noTables = DefaultFont();
		noTables.SetU16(4, 0);
		CheckRejected(noTables.Data, "directory");
		FontPatcher manyTables = DefaultFont();
		manyTables.SetU16(4, 0xFFFF);
		CheckRejected(manyTables.Data, "directory");

		FontPatcher missingGlyf = DefaultFont();
		std::memcpy(missingGlyf.Data.data() + missingGlyf.Record("glyf"), "glyX", 4);
		CheckRejected(missingGlyf.Data, "required tables");
	}

	TEST_CASE("Metric tables are checked against what the rasterizer reads")
	{
		auto patched = [](auto patch)
		{
			FontPatcher font = DefaultFont();
			patch(font);
			return font.Data;
		};
		CheckRejected(patched([](FontPatcher& font) { font.SetTableLength("head", 53); }), "head table is too short");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("head") + 18, 0); }), "units per em");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("head") + 50, 2); }), "loca format");
		CheckRejected(patched([](FontPatcher& font) { font.SetTableLength("maxp", 5); }), "maxp table is too short");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("maxp") + 4, 0); }), "no glyphs");
		CheckRejected(patched([](FontPatcher& font) { font.SetTableLength("hhea", 35); }), "hhea table is too short");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("hhea") + 34, 0); }), "numberOfHMetrics");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("hhea") + 34, static_cast<uint16_t>(font.GlyphCount() + 1)); }), "numberOfHMetrics");
		CheckRejected(patched([](FontPatcher& font) { font.SetTableLength("hmtx", font.TableLength("hmtx") - 2); }), "hmtx");
		CheckRejected(patched([](FontPatcher& font) { font.SetTableLength("loca", font.TableLength("loca") - 2); }), "loca table is too short");
		// Glyph offsets must increase and stay inside glyf (short loca offsets are stored halved).
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("loca") + 2 * 10, 0); }), "decrease");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("loca") + 2 * static_cast<size_t>(font.GlyphCount()), 0xFFFF); }), "outside the font's glyf");
	}

	TEST_CASE("The character map stb_truetype selects is bounded and of a supported format")
	{
		auto patched = [](auto patch)
		{
			FontPatcher font = DefaultFont();
			patch(font);
			return font.Data;
		};
		// The reviewer's crash: the selected subtable's offset far outside the file.
		CheckRejected(patched([](FontPatcher& font) { font.SetU32(font.SelectedCmapRecord() + 4, 0x7FFFFF00); }), "outside its cmap");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.Table("cmap") + 2, 0x7FFF); }), "cmap records");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.SelectedCmapSubtable(), 2); }), "format 2");
		CheckRejected(patched([](FontPatcher& font) { font.SetU16(font.SelectedCmapSubtable(), 14); }), "format 14");
		CheckRejected(patched([](FontPatcher& font) { font.SetU32(font.SelectedCmapSubtable() + 12, 0x10000000); }), "format 12");
		CheckRejected(patched([](FontPatcher& font) { font.SetU32(font.SelectedCmapSubtable() + 16 + 4, 0x7FFFFFFF); }), "invalid group");
		// Without Unicode records there is nothing to map characters with.
		CheckRejected(patched([](FontPatcher& font)
		{
			for (uint32_t index = 0; index < font.U16(font.Table("cmap") + 2); index++)
				font.SetU16(font.Table("cmap") + 4 + 8 * static_cast<size_t>(index), 1);
		}), "no Unicode character map");

		// The font's format 4 subtable, selected by hiding the later format 12 record from stb_truetype.
		auto format4 = [](FontPatcher& font)
		{
			font.SetU16(font.SelectedCmapRecord(), 1); // Macintosh platform: ignored
			REQUIRE(font.U16(font.SelectedCmapSubtable()) == 4);
		};
		FontPatcher segments = DefaultFont();
		format4(segments);
		std::string error;
		CHECK_MESSAGE(Font::Create(segments.Data, &error), error);
		CheckRejected(patched([&](FontPatcher& font)
		{
			format4(font);
			const size_t subtable = font.SelectedCmapSubtable();
			font.SetU16(subtable + 10, static_cast<uint16_t>(font.U16(subtable + 10) + 1)); // entrySelector
		}), "search parameters");
		CheckRejected(patched([&](FontPatcher& font)
		{
			format4(font);
			const size_t subtable = font.SelectedCmapSubtable();
			const size_t segments = font.U16(subtable + 6) / 2;
			font.SetU16(subtable + 16 + 6 * segments, 0xFFFE); // idRangeOffset of the first segment
		}), "points outside");
		CheckRejected(patched([&](FontPatcher& font)
		{
			format4(font);
			font.SetU16(font.SelectedCmapSubtable() + 6, 0xFFFE); // segCountX2
		}), "format 4");
	}

	TEST_CASE("Glyph outlines are checked before stb_truetype reads them")
	{
		FontPatcher original = DefaultFont();
		const std::vector<uint32_t> composites = original.GlyphsWhere(true);
		const std::vector<uint32_t> multiContour = original.GlyphsWhere(false, 2);
		REQUIRE(composites.size() >= 10);
		REQUIRE_FALSE(multiContour.empty());
		auto patched = [&](auto patch)
		{
			FontPatcher font = original;
			patch(font);
			return font.Data;
		};
		auto setComponent = [](FontPatcher& font, uint32_t glyph, size_t component, uint32_t target)
		{
			const std::vector<size_t> components = font.Components(glyph);
			REQUIRE(component < components.size());
			font.SetU16(components[component] + 2, static_cast<uint16_t>(target));
		};

		// Composite glyphs: cycles (which would recurse until the stack overflows), deep nesting, missing glyphs and
		// components positioned by matching points (which stb_truetype asserts on).
		CheckRejected(patched([&](FontPatcher& font) { setComponent(font, composites[0], 0, composites[0]); }), "contains itself");
		CheckRejected(patched([&](FontPatcher& font)
		{
			setComponent(font, composites[0], 0, composites[1]);
			setComponent(font, composites[1], 0, composites[0]);
		}), "contains itself");
		CheckRejected(patched([&](FontPatcher& font)
		{
			for (size_t index = 0; index + 1 < 10; index++)
				setComponent(font, composites[index], 0, composites[index + 1]);
		}), "nested");
		// The same chain in the other direction: each link is validated before the glyph that uses it.
		CheckRejected(patched([&](FontPatcher& font)
		{
			for (size_t index = 0; index + 1 < 10; index++)
				setComponent(font, composites[index + 1], 0, composites[index]);
		}), "nested");
		CheckRejected(patched([&](FontPatcher& font) { setComponent(font, composites[0], 0, font.GlyphCount()); }), "does not exist");
		CheckRejected(patched([&](FontPatcher& font)
		{
			const size_t flags = font.Components(composites[0])[0];
			font.SetU16(flags, static_cast<uint16_t>(font.U16(flags) & ~0x0002u));
		}), "matching points");

		// Shallow but wide: two components of each glyph of a chain use the next glyph, doubling the expanded outline at
		// every level (the chain ends with a composite of simple glyphs, so it stays within the nesting limit).
		std::vector<uint32_t> pairs;
		for (uint32_t glyph : composites)
		{
			const std::vector<size_t> components = original.Components(glyph);
			bool simpleComponents = true;
			for (size_t component : components)
				simpleComponents = simpleComponents && original.Contours(original.U16(component + 2)) >= 0;
			if (components.size() >= 2 && simpleComponents)
				pairs.push_back(glyph);
		}
		REQUIRE(pairs.size() >= 8);
		CheckRejected(patched([&](FontPatcher& font)
		{
			for (size_t index = 0; index + 1 < 8; index++)
			{
				setComponent(font, pairs[index], 0, pairs[index + 1]);
				setComponent(font, pairs[index], 1, pairs[index + 1]);
			}
		}), "expands to more than");

		// A component moved so far that its coordinates no longer fit stb_truetype's 16-bit vertices.
		CheckRejected(patched([&](FontPatcher& font)
		{
			for (uint32_t glyph : composites)
			{
				for (size_t component : font.Components(glyph))
				{
					const uint16_t flags = font.U16(component);
					if ((flags & 0x0001) && !(flags & (0x0008 | 0x0040 | 0x0080))) // Offsets stored as words, not scaled
					{
						font.SetU16(component + 4, 32000);
						return;
					}
				}
			}
			FAIL("No component with word offsets");
		}), "beyond the coordinate range");

		// Simple glyphs: contour end points, instructions and point data stay inside the glyph.
		const uint32_t glyph = multiContour[0];
		CheckRejected(patched([&](FontPatcher& font)
		{
			const size_t ends = font.Glyph(glyph).first + 10;
			font.SetU16(ends, font.U16(ends + 2));
		}), "do not increase");
		CheckRejected(patched([&](FontPatcher& font)
		{
			const size_t contours = static_cast<size_t>(font.Contours(glyph));
			font.SetU16(font.Glyph(glyph).first + 10 + 2 * contours, 0xFFFF);
		}), "instructions exceed");
		CheckRejected(patched([&](FontPatcher& font)
		{
			// The last end point far beyond the glyph's data: its flags and coordinates cannot all be there.
			const size_t contours = static_cast<size_t>(font.Contours(glyph));
			font.SetU16(font.Glyph(glyph).first + 10 + 2 * (contours - 1), 0xFFF0);
		}), "exceed its data");
		CheckRejected(patched([&](FontPatcher& font)
		{
			// A first contour of one off-curve point: stb_truetype would read the point after it as the contour's start.
			const size_t start = font.Glyph(glyph).first;
			const size_t contours = static_cast<size_t>(font.Contours(glyph));
			font.SetU16(start + 10, 0);
			const size_t flags = start + 10 + 2 * contours + 2 + font.U16(start + 10 + 2 * contours);
			font.Data[flags] = static_cast<uint8_t>(font.Data[flags] & ~0x01u);
		}), "single off-curve point");
		CheckRejected(patched([&](FontPatcher& font)
		{
			// Shorten the glyph to less than its header.
			const bool longOffsets = font.U16(font.Table("head") + 50) == 1;
			REQUIRE_FALSE(longOffsets);
			const size_t entry = font.Table("loca") + 2 * static_cast<size_t>(glyph + 1);
			font.SetU16(entry, static_cast<uint16_t>(font.U16(entry - 2) + 2));
		}), "shorter than its header");
	}

	TEST_CASE("Fonts with more outline points than the limit are rejected")
	{
		// 300 glyphs of 65535 points, two flag bytes per 256 points (on the curve, repeated, coordinates unchanged):
		// almost 20 million points in 160 KB.
		FontPatcher font = DefaultFont();
		constexpr uint32_t c_LargeGlyphs = 300;
		std::vector<uint8_t> glyph(14 + 512, 0);
		Write16(glyph, 0, 1);      // One contour
		Write16(glyph, 10, 65534); // Ending at point 65534; no instructions
		for (size_t flag = 0; flag < 256; flag++)
		{
			glyph[14 + 2 * flag] = 0x39;
			glyph[15 + 2 * flag] = 255;
		}
		std::vector<uint8_t> glyf;
		std::vector<uint8_t> loca(4 * (static_cast<size_t>(font.GlyphCount()) + 1), 0);
		for (uint32_t index = 0; index <= font.GlyphCount(); index++)
		{
			Write32(loca, 4 * static_cast<size_t>(index), static_cast<uint32_t>(glyf.size()));
			if (index < c_LargeGlyphs)
				glyf.insert(glyf.end(), glyph.begin(), glyph.end());
		}
		font.SetU16(font.Table("head") + 50, 1); // Long glyph offsets
		const std::vector<uint8_t> data = ReplaceTable(FontPatcher(ReplaceTable(font, "glyf", glyf)), "loca", loca);
		CHECK(data.size() < 1024 * 1024);
		CheckRejected(data, "outline points");
	}

	TEST_CASE("Glyphs too large to rasterize are skipped instead of allocated")
	{
		// The reviewer's heap overrun: few units per em and huge glyph boxes make the distance field gigantic.
		FontPatcher font = DefaultFont();
		font.SetU16(font.Table("head") + 18, 16);
		for (uint32_t glyph = 0; glyph < font.GlyphCount(); glyph++)
		{
			const auto [start, end] = font.Glyph(glyph);
			if (end - start < 10)
				continue;
			font.SetU16(start + 2, static_cast<uint16_t>(-32767));
			font.SetU16(start + 4, static_cast<uint16_t>(-32767));
			font.SetU16(start + 6, 32767);
			font.SetU16(start + 8, 32767);
		}
		QuietLog quiet;
		std::string error;
		Ref<Font> loaded = Font::Create(font.Data, &error);
		REQUIRE_MESSAGE(loaded, error);
		Scope<FontAtlas> atlas = FontAtlas::Create(loaded, &error);
		REQUIRE(atlas);
		for (uint32_t codepoint : { uint32_t('A'), uint32_t('g'), uint32_t(0xE9) })
		{
			const GlyphInfo& glyph = atlas->GetGlyph(codepoint);
			CHECK_FALSE(glyph.Visible);
			CHECK(glyph.Advance > 0.0f);
		}
		CHECK(atlas->GetPageCount() == 0); // Nothing was allocated
	}

	TEST_CASE("Malformed kerning disables kerning instead of rejecting the font")
	{
		Scope<FontAtlas> reference = FontAtlas::Create(Font::GetDefault());
		REQUIRE(reference);
		CHECK(reference->GetKerning(reference->GetGlyph('A'), reference->GetGlyph('V')) < 0.0f); // Roboto kerns "AV"

		auto checkKerningDisabled = [](const std::vector<uint8_t>& data)
		{
			std::string error;
			Ref<Font> loaded = Font::Create(data, &error);
			REQUIRE_MESSAGE(loaded, error);
			CHECK_FALSE(loaded->HasUsableKerning());
			Scope<FontAtlas> atlas = FontAtlas::Create(loaded);
			REQUIRE(atlas);
			CHECK(atlas->GetKerning(atlas->GetGlyph('A'), atlas->GetGlyph('V')) == 0.0f);
			TextLayout layout;
			LayoutText(*atlas, "AVATAR", TextAlignment::Left, layout);
			CHECK(layout.Quads.size() == 6);
		};

		QuietLog quiet;
		const FontPatcher original = DefaultFont();
		for (uint16_t lookupList : { uint16_t(0xFFFF), uint16_t(0xFFF0) })
		{
			CAPTURE(lookupList);
			FontPatcher font = original;
			font.SetU16(font.Table("GPOS") + 8, lookupList);
			checkKerningDisabled(font.Data);
		}

		// A short GPOS table: its header alone is out of bounds.
		FontPatcher shortTable = original;
		shortTable.SetTableLength("GPOS", 6);
		checkKerningDisabled(shortTable.Data);

		// Inside the pair adjustment subtable: the coverage table's glyph count, and the first pair set (format 1) or the
		// second class definition (format 2).
		const size_t pairAdjustment = original.FirstPairAdjustment();
		FontPatcher coverage = original;
		coverage.SetU16(pairAdjustment + coverage.U16(pairAdjustment + 2) + 2, 0xFFFF);
		checkKerningDisabled(coverage.Data);
		FontPatcher pairs = original;
		pairs.SetU16(pairAdjustment + 10, 0xFFFF);
		checkKerningDisabled(pairs.Data);

		// Without GPOS, the kern table is read: the pair count must fit it.
		FontPatcher kern(Tests::ReadSourceFile("Strata/vendor/JoltPhysics/Assets/Fonts/Roboto-Regular.ttf"));
		std::memcpy(kern.Data.data() + kern.Record("GPOS"), "GPOX", 4);
		std::string error;
		Ref<Font> kernFont = Font::Create(kern.Data, &error);
		REQUIRE_MESSAGE(kernFont, error);
		CHECK(kernFont->HasUsableKerning());
		Scope<FontAtlas> kernAtlas = FontAtlas::Create(kernFont);
		REQUIRE(kernAtlas);
		CHECK(kernAtlas->GetKerning(kernAtlas->GetGlyph('A'), kernAtlas->GetGlyph('V')) < 0.0f);
		kern.SetU16(kern.Table("kern") + 10, 0xFFFF);
		checkKerningDisabled(kern.Data);
	}

	TEST_CASE("Kerning that stb_truetype would search or validate without bound is disabled")
	{
		const FontPatcher font = DefaultFont();
		const float unitsPerEm = static_cast<float>(font.U16(font.Table("head") + 18));
		Scope<FontAtlas> reference = FontAtlas::Create(Font::GetDefault());
		REQUIRE(reference);
		const uint32_t a = reference->GetGlyph('A').GlyphIndex;
		const uint32_t v = reference->GetGlyph('V').GlyphIndex;
		QuietLog quiet;

		// Shared offsets are fine while the search stays short, and stb_truetype finds the pair through them.
		std::vector<uint8_t> shared = ReplaceTable(font, "GPOS", SharedPairPositioning(8, 16, 20, a, v, -200));
		TrueTypeFontFacts facts;
		std::string error;
		REQUIRE_MESSAGE(ValidateTrueTypeFont(shared, facts, error), error);
		CHECK(facts.KerningUsable);
		CHECK(facts.KerningLookupVisits == 8 + 8 * 16);
		Ref<Font> loaded = Font::Create(std::move(shared), &error);
		REQUIRE_MESSAGE(loaded, error);
		Scope<FontAtlas> atlas = FontAtlas::Create(loaded);
		REQUIRE(atlas);
		CHECK(atlas->GetKerning(atlas->GetGlyph('A'), atlas->GetGlyph('V')) == doctest::Approx(-200.0f / unitsPerEm));
		CHECK(atlas->GetKerning(atlas->GetGlyph('V'), atlas->GetGlyph('A')) == 0.0f);

		// Every lookup offset, or every subtable offset, pointing at one target: stb_truetype would search 30000 lookups,
		// or 30000 subtables of 2000 coverage ranges, for every glyph pair (and validating each visit took billions of
		// steps). Validation stops at once and the font has no kerning.
		struct Case
		{
			uint32_t Lookups;
			uint32_t Subtables;
			const char* Reason;
		};
		for (const Case& sharedCase : { Case { 30000, 30000, "too many lookups" }, Case { 1, 30000, "too many pair adjustment subtables" } })
		{
			CAPTURE(sharedCase.Reason);
			std::vector<uint8_t> data = ReplaceTable(font, "GPOS", SharedPairPositioning(sharedCase.Lookups, sharedCase.Subtables, 2000, a, v, -200));
			REQUIRE_MESSAGE(ValidateTrueTypeFont(data, facts, error), error);
			CHECK_FALSE(facts.KerningUsable);
			CHECK(facts.KerningIssue.find(sharedCase.Reason) != std::string::npos);
			Ref<Font> unbounded = Font::Create(std::move(data), &error);
			REQUIRE_MESSAGE(unbounded, error);
			CHECK_FALSE(unbounded->HasUsableKerning());
			Scope<FontAtlas> unboundedAtlas = FontAtlas::Create(unbounded);
			REQUIRE(unboundedAtlas);
			CHECK(unboundedAtlas->GetKerning(unboundedAtlas->GetGlyph('A'), unboundedAtlas->GetGlyph('V')) == 0.0f);
		}

		// 200 distinct subtables (a short search) that each cover every glyph, so each has 131071 pair set offsets to
		// check: validation stops when its budget is spent.
		constexpr uint32_t c_Lookups = 200;
		const size_t lookupList = 10;
		const size_t firstLookup = lookupList + 2 + 2 * c_Lookups;
		const size_t firstSubtable = firstLookup + 8 * c_Lookups;
		const size_t coverage = firstSubtable + 16 * c_Lookups;
		std::vector<uint8_t> gpos(coverage + 10 + 2 * 131071 + 0x10000, 0);
		Write16(gpos, 0, 1);
		Write16(gpos, 8, static_cast<uint32_t>(lookupList));
		Write16(gpos, lookupList, c_Lookups);
		for (size_t index = 0; index < c_Lookups; index++)
		{
			const size_t lookup = firstLookup + 8 * index;
			const size_t subtable = firstSubtable + 16 * index;
			Write16(gpos, lookupList + 2 + 2 * index, static_cast<uint32_t>(lookup - lookupList));
			Write16(gpos, lookup, 2);
			Write16(gpos, lookup + 4, 1);
			Write16(gpos, lookup + 6, static_cast<uint32_t>(subtable - lookup));
			Write16(gpos, subtable, 1);
			Write16(gpos, subtable + 2, static_cast<uint32_t>(coverage - subtable));
			Write16(gpos, subtable + 4, 4);
		}
		Write16(gpos, coverage, 2);
		Write16(gpos, coverage + 2, 1);
		Write16(gpos, coverage + 6, 0xFFFF); // Glyphs 0 to 65535
		Write16(gpos, coverage + 8, 0xFFFF); // From coverage index 65535
		REQUIRE_MESSAGE(ValidateTrueTypeFont(ReplaceTable(font, "GPOS", gpos), facts, error), error);
		CHECK(facts.KerningLookupVisits == 2 * c_Lookups);
		CHECK_FALSE(facts.KerningUsable);
		CHECK(facts.KerningIssue.find("too large to validate") != std::string::npos);

		// The vendored fonts search a few lookups.
		for (const char* path : c_VendoredTrueTypeFonts)
		{
			CAPTURE(path);
			REQUIRE(ValidateTrueTypeFont(Tests::ReadSourceFile(path), facts, error));
			CHECK(facts.KerningUsable);
			CHECK(facts.KerningLookupVisits <= 16);
		}
	}

	TEST_CASE("Fonts truncated at any table boundary are rejected")
	{
		const FontPatcher font = DefaultFont();
		std::vector<size_t> cuts = { 0, 4, 11, 12, 13, 12 + 16 * static_cast<size_t>(font.U16(4)) - 1 };
		for (const std::string& tag : font.GetTags())
		{
			const size_t offset = font.Table(tag);
			const size_t length = font.TableLength(tag);
			for (size_t cut : { offset, offset + 1, offset + length / 2, offset + length - 1 })
				cuts.push_back(cut);
		}
		for (size_t cut : cuts)
		{
			if (cut >= font.Data.size())
				continue;
			CAPTURE(cut);
			std::vector<uint8_t> truncated(font.Data.begin(), font.Data.begin() + static_cast<std::ptrdiff_t>(cut));
			std::string error;
			CHECK_FALSE(Font::Create(std::move(truncated), &error));
			CHECK_FALSE(error.empty());
		}
	}

	TEST_CASE("Randomly corrupted fonts are rejected or render without crashing")
	{
		// Deterministic fuzzing: a few bytes of the default font are overwritten, mostly inside the structures stb_truetype
		// reads. Every font Create accepts must survive layout, kerning and rasterization.
		const FontPatcher original = DefaultFont();
		const std::vector<std::string> tags = original.GetTags();
		std::mt19937 random(0x5EED2026u);
		QuietLog quiet;
		uint32_t accepted = 0;
		constexpr uint32_t c_Iterations = 1500;
		for (uint32_t iteration = 0; iteration < c_Iterations; iteration++)
		{
			std::vector<uint8_t> data = original.Data;
			const uint32_t corruptions = 1 + random() % 6;
			for (uint32_t corruption = 0; corruption < corruptions; corruption++)
			{
				size_t position = 0;
				const uint32_t area = random() % 4;
				if (area == 0)
				{
					position = random() % data.size();
				}
				else if (area == 1)
				{
					position = random() % (12 + 16 * static_cast<size_t>(original.U16(4))); // Table directory
				}
				else
				{
					// The first bytes of a table hold its headers and offsets.
					const std::string& tag = tags[random() % tags.size()];
					const size_t length = std::max<size_t>(original.TableLength(tag), 1);
					position = original.Table(tag) + random() % std::min<size_t>(length, area == 2 ? 64 : 4096);
				}
				data[position] = static_cast<uint8_t>(random());
			}

			std::string error;
			Ref<Font> font = Font::Create(std::move(data), &error);
			if (!font)
			{
				CHECK_FALSE(error.empty());
				continue;
			}
			accepted++;
			Scope<FontAtlas> atlas = FontAtlas::Create(font, &error);
			REQUIRE_MESSAGE(atlas, error);
			TextLayout layout;
			LayoutText(*atlas, "AVa@1\xC3\xA9\xE2\x9C\x93", TextAlignment::Center, layout);
			CHECK(std::isfinite(layout.Max.x - layout.Min.x));
			CHECK(std::isfinite(atlas->GetMetrics().LineHeight));
		}
		CAPTURE(accepted);
		CHECK(accepted > c_Iterations / 10);   // Many corruptions are harmless...
		CHECK(accepted < c_Iterations);        // ...and some are caught
	}
}
