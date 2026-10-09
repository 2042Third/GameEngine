#include "stpch.h"
#include "Strata/Renderer/FontValidation.h"

#include "Strata/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_SfntTrueType = 0x00010000;
		constexpr uint32_t c_CollectionVersion1 = 0x00010000;
		constexpr uint32_t c_CollectionVersion2 = 0x00020000;
		constexpr uint32_t c_MaxCodepoint = 0x10FFFF;

		// Composite glyph component flags (glyf table).
		constexpr uint16_t c_ArgsAreWords = 0x0001;
		constexpr uint16_t c_ArgsAreXYValues = 0x0002;
		constexpr uint16_t c_HaveScale = 0x0008;
		constexpr uint16_t c_MoreComponents = 0x0020;
		constexpr uint16_t c_HaveXYScale = 0x0040;
		constexpr uint16_t c_HaveTwoByTwo = 0x0080;

		// Simple glyph point flags.
		constexpr uint8_t c_OnCurve = 0x01;
		constexpr uint8_t c_XShort = 0x02;
		constexpr uint8_t c_YShort = 0x04;
		constexpr uint8_t c_Repeat = 0x08;
		constexpr uint8_t c_XSameOrPositive = 0x10;
		constexpr uint8_t c_YSameOrPositive = 0x20;

		// A table of the font: absolute offset and length within the file.
		struct TableRange
		{
			uint64_t Offset = 0;
			uint64_t Length = 0;
			bool Present = false;

			// Whether `size` bytes at `offset` (relative to the table) lie inside it.
			bool Has(uint64_t offset, uint64_t size) const { return offset <= Length && size <= Length - offset; }
		};

		struct GlyphSummary
		{
			enum class Status : uint8_t
			{
				Unvisited = 0,
				Visiting,
				Done
			};
			Status State = Status::Unvisited;
			uint32_t Height = 0;     // Composite nesting below this glyph
			uint32_t Components = 0; // Components after expanding nested composites (0 for simple glyphs)
			uint32_t Points = 0;     // After expanding components, saturating
			double MaxX = 0.0;     // Largest absolute coordinate stb_truetype can produce for the outline
			double MaxY = 0.0;
		};

		uint32_t SaturatingAdd(uint32_t a, uint32_t b)
		{
			return a > std::numeric_limits<uint32_t>::max() - b ? std::numeric_limits<uint32_t>::max() : a + b;
		}

		class Validator
		{
		public:
			Validator(std::span<const uint8_t> data, TrueTypeFontFacts& facts)
				: m_Data(data), m_Facts(facts)
			{
			}

			bool Validate(std::string& outError)
			{
				const bool valid = ReadDirectory() && ValidateMetrics() && ValidateLoca() && ValidateGlyphs() && ValidateCharacterMap();
				if (!valid)
				{
					outError = m_Error;
					return false;
				}
				ValidateKerning();
				return true;
			}
		private:
			bool Fail(std::string message)
			{
				m_Error = std::move(message);
				return false;
			}

			bool Has(uint64_t offset, uint64_t size) const { return offset <= m_Data.size() && size <= m_Data.size() - offset; }
			uint8_t U8(uint64_t offset) const { return m_Data[static_cast<size_t>(offset)]; }
			int8_t S8(uint64_t offset) const { return static_cast<int8_t>(U8(offset)); }
			uint16_t U16(uint64_t offset) const { return static_cast<uint16_t>((U8(offset) << 8) | U8(offset + 1)); }
			int16_t S16(uint64_t offset) const { return static_cast<int16_t>(U16(offset)); }
			uint32_t U32(uint64_t offset) const
			{
				return (static_cast<uint32_t>(U8(offset)) << 24) | (static_cast<uint32_t>(U8(offset + 1)) << 16) | (static_cast<uint32_t>(U8(offset + 2)) << 8)
					| static_cast<uint32_t>(U8(offset + 3));
			}
			bool IsTag(uint64_t offset, const char* tag) const { return std::memcmp(m_Data.data() + offset, tag, 4) == 0; }

			// The table directory, read the way stb_truetype finds tables (the first record with a tag wins).
			bool ReadDirectory()
			{
				if (m_Data.size() > FontLimits::c_MaxFontSize)
					return Fail(fmt::format("Font file is larger than {} MB", FontLimits::c_MaxFontSize / (1024 * 1024)));
				if (!Has(0, 12))
					return Fail("Font file is truncated");

				uint64_t fontStart = 0;
				if (IsTag(0, "ttcf"))
				{
					if (!Has(0, 16))
						return Fail("Font collection header is truncated");
					const uint32_t version = U32(4);
					if ((version != c_CollectionVersion1 && version != c_CollectionVersion2) || U32(8) == 0)
						return Fail("Font collection header is invalid");
					fontStart = U32(12);
					if (!Has(fontStart, 12))
						return Fail("Font collection points outside the file");
				}
				if (IsTag(fontStart, "OTTO"))
					return Fail("OpenType fonts with CFF outlines (OTTO) are not supported; use TrueType outlines");
				if (U32(fontStart) != c_SfntTrueType && !IsTag(fontStart, "true"))
					return Fail("Not a TrueType font");

				const uint32_t tableCount = U16(fontStart + 4);
				const uint64_t directory = fontStart + 12;
				if (tableCount == 0 || !Has(directory, 16ull * tableCount))
					return Fail("Font table directory is invalid");
				const uint64_t directoryEnd = directory + 16ull * tableCount;

				const std::pair<const char*, TableRange*> tables[] = { { "cmap", &m_Cmap }, { "head", &m_Head }, { "hhea", &m_Hhea }, { "hmtx", &m_Hmtx },
					{ "maxp", &m_Maxp }, { "loca", &m_Loca }, { "glyf", &m_Glyf }, { "kern", &m_Kern }, { "GPOS", &m_Gpos } };
				for (uint32_t index = 0; index < tableCount; index++)
				{
					const uint64_t record = directory + 16ull * index;
					const uint64_t offset = U32(record + 8);
					const uint64_t length = U32(record + 12);
					if (!Has(offset, length))
						return Fail(fmt::format("Font table {} lies outside the file", index));
					for (const auto& [tag, range] : tables)
					{
						if (range->Present || !IsTag(record, tag))
							continue;
						if (offset < directoryEnd)
							return Fail(fmt::format("The font's {} table overlaps its table directory", tag));
						*range = TableRange { offset, length, true };
					}
				}
				if (!m_Cmap.Present || !m_Head.Present || !m_Hhea.Present || !m_Hmtx.Present || !m_Maxp.Present || !m_Loca.Present || !m_Glyf.Present)
					return Fail("Font is missing required tables (cmap, head, hhea, hmtx, maxp, loca and glyf)");
				m_Facts.FontOffset = static_cast<uint32_t>(fontStart);
				return true;
			}

			bool ValidateMetrics()
			{
				if (!m_Head.Has(0, 54))
					return Fail("The font's head table is too short");
				const uint16_t unitsPerEm = U16(m_Head.Offset + 18);
				if (unitsPerEm < 16 || unitsPerEm > 16384)
					return Fail(fmt::format("The font's units per em ({}) are outside 16-16384", unitsPerEm));
				m_LocaFormat = U16(m_Head.Offset + 50);
				if (m_LocaFormat > 1)
					return Fail(fmt::format("The font's loca format ({}) is invalid", m_LocaFormat));

				if (!m_Maxp.Has(0, 6))
					return Fail("The font's maxp table is too short");
				m_GlyphCount = U16(m_Maxp.Offset + 4);
				if (m_GlyphCount == 0)
					return Fail("The font has no glyphs");

				if (!m_Hhea.Has(0, 36))
					return Fail("The font's hhea table is too short");
				const uint32_t metricCount = U16(m_Hhea.Offset + 34);
				if (metricCount == 0 || metricCount > m_GlyphCount)
					return Fail(fmt::format("The font's numberOfHMetrics ({}) does not fit its {} glyphs", metricCount, m_GlyphCount));
				if (!m_Hmtx.Has(0, 4ull * metricCount + 2ull * (m_GlyphCount - metricCount)))
					return Fail("The font's hmtx table is too short for its glyphs");
				return true;
			}

			bool ValidateLoca()
			{
				const uint64_t entrySize = m_LocaFormat == 0 ? 2 : 4;
				if (!m_Loca.Has(0, entrySize * (m_GlyphCount + 1ull)))
					return Fail("The font's loca table is too short for its glyphs");
				m_GlyphOffsets.resize(m_GlyphCount + 1ull);
				for (uint32_t index = 0; index <= m_GlyphCount; index++)
				{
					const uint64_t entry = m_Loca.Offset + entrySize * index;
					const uint64_t offset = m_LocaFormat == 0 ? 2ull * U16(entry) : U32(entry);
					if (offset > m_Glyf.Length)
						return Fail(fmt::format("Glyph {} lies outside the font's glyf table", index));
					if (index > 0 && offset < m_GlyphOffsets[index - 1])
						return Fail(fmt::format("The font's loca entries decrease at glyph {}", index));
					m_GlyphOffsets[index] = offset;
				}
				return true;
			}

			bool ValidateGlyphs()
			{
				m_Glyphs.assign(m_GlyphCount, GlyphSummary());
				m_Facts.GlyphPoints.resize(m_GlyphCount);
				for (uint32_t glyph = 0; glyph < m_GlyphCount; glyph++)
				{
					if (!ValidateGlyph(glyph, 0))
						return false;
					m_Facts.GlyphPoints[glyph] = m_Glyphs[glyph].Points;
				}
				return true;
			}

			bool ValidateGlyph(uint32_t glyph, uint32_t depth)
			{
				GlyphSummary& summary = m_Glyphs[glyph];
				if (summary.State == GlyphSummary::Status::Done)
					return true;
				if (summary.State == GlyphSummary::Status::Visiting)
					return Fail(fmt::format("Composite glyph {} contains itself", glyph));
				if (depth > FontLimits::c_MaxCompositeDepth)
					return Fail(fmt::format("Composite glyphs are nested more than {} levels deep", FontLimits::c_MaxCompositeDepth));
				summary.State = GlyphSummary::Status::Visiting;

				const uint64_t start = m_Glyf.Offset + m_GlyphOffsets[glyph];
				const uint64_t end = m_Glyf.Offset + m_GlyphOffsets[glyph + 1];
				if (end > start)
				{
					if (end - start < 10)
						return Fail(fmt::format("Glyph {} is shorter than its header", glyph));
					const int16_t contours = S16(start);
					if (contours > 0 && !ValidateSimpleGlyph(glyph, start, end, static_cast<uint32_t>(contours)))
						return false;
					if (contours < 0 && !ValidateCompositeGlyph(glyph, start, end, depth))
						return false;
				}
				m_Glyphs[glyph].State = GlyphSummary::Status::Done;
				return true;
			}

			// Mirrors stb_truetype's reading of a simple glyph: end points, instructions, flags, then x and y coordinates.
			bool ValidateSimpleGlyph(uint32_t glyph, uint64_t start, uint64_t end, uint32_t contours)
			{
				uint64_t cursor = start + 10;
				if (cursor + 2ull * contours + 2 > end)
					return Fail(fmt::format("Glyph {}'s contour list exceeds its data", glyph));
				m_ContourEnds.resize(contours);
				for (uint32_t contour = 0; contour < contours; contour++)
				{
					m_ContourEnds[contour] = U16(cursor + 2ull * contour);
					if (contour > 0 && m_ContourEnds[contour] <= m_ContourEnds[contour - 1])
						return Fail(fmt::format("Glyph {}'s contour end points do not increase", glyph));
				}
				const uint32_t pointCount = m_ContourEnds.back() + 1u;
				m_TotalPoints += pointCount;
				if (m_TotalPoints > FontLimits::c_MaxOutlinePoints)
					return Fail(fmt::format("The font has more than {} outline points", FontLimits::c_MaxOutlinePoints));
				cursor += 2ull * contours;
				cursor += 2ull + U16(cursor); // Instructions
				if (cursor > end)
					return Fail(fmt::format("Glyph {}'s instructions exceed its data", glyph));

				m_PointFlags.resize(pointCount);
				uint8_t flags = 0;
				uint32_t repeat = 0;
				for (uint32_t point = 0; point < pointCount; point++)
				{
					if (repeat == 0)
					{
						if (cursor >= end)
							return Fail(fmt::format("Glyph {}'s point flags exceed its data", glyph));
						flags = U8(cursor++);
						if (flags & c_Repeat)
						{
							if (cursor >= end)
								return Fail(fmt::format("Glyph {}'s point flags exceed its data", glyph));
							repeat = U8(cursor++);
						}
					}
					else
					{
						repeat--;
					}
					m_PointFlags[point] = flags;
				}

				// stb_truetype reads one point past a contour that starts off the curve; a contour of one such point would
				// make it read past the outline.
				for (uint32_t contour = 0; contour < contours; contour++)
				{
					const uint32_t first = contour == 0 ? 0u : m_ContourEnds[contour - 1] + 1u;
					if (m_ContourEnds[contour] == first && !(m_PointFlags[first] & c_OnCurve))
						return Fail(fmt::format("Glyph {} has a contour of a single off-curve point", glyph));
				}

				double maxX = 0.0;
				double maxY = 0.0;
				for (int axis = 0; axis < 2; axis++)
				{
					const uint8_t shortFlag = axis == 0 ? c_XShort : c_YShort;
					const uint8_t sameFlag = axis == 0 ? c_XSameOrPositive : c_YSameOrPositive;
					int32_t value = 0;
					double& maxAbs = axis == 0 ? maxX : maxY;
					for (uint32_t point = 0; point < pointCount; point++)
					{
						const uint8_t pointFlags = m_PointFlags[point];
						if (pointFlags & shortFlag)
						{
							if (cursor + 1 > end)
								return Fail(fmt::format("Glyph {}'s coordinates exceed its data", glyph));
							const int32_t delta = U8(cursor++);
							value += (pointFlags & sameFlag) ? delta : -delta;
						}
						else if (!(pointFlags & sameFlag))
						{
							if (cursor + 2 > end)
								return Fail(fmt::format("Glyph {}'s coordinates exceed its data", glyph));
							value += S16(cursor);
							cursor += 2;
						}
						// stb_truetype stores each coordinate as a 16-bit integer (wrapping).
						const int16_t stored = static_cast<int16_t>(value);
						maxAbs = std::max(maxAbs, std::abs(static_cast<double>(stored)));
					}
				}

				GlyphSummary& summary = m_Glyphs[glyph];
				summary.Points = pointCount;
				summary.MaxX = maxX;
				summary.MaxY = maxY;
				return true;
			}

			// Mirrors stb_truetype's reading of a composite glyph; components are validated recursively.
			bool ValidateCompositeGlyph(uint32_t glyph, uint64_t start, uint64_t end, uint32_t depth)
			{
				uint64_t cursor = start + 10;
				uint32_t points = 0;
				uint32_t height = 0;
				uint32_t components = 0;
				double maxX = 0.0;
				double maxY = 0.0;
				bool more = true;
				while (more)
				{
					if (cursor + 4 > end)
						return Fail(fmt::format("Composite glyph {}'s components exceed its data", glyph));
					const uint16_t flags = U16(cursor);
					const uint32_t component = U16(cursor + 2);
					cursor += 4;
					if (!(flags & c_ArgsAreXYValues))
						return Fail(fmt::format("Composite glyph {} places a component by matching points, which is not supported", glyph));

					double offsetX = 0.0;
					double offsetY = 0.0;
					const uint64_t argumentSize = (flags & c_ArgsAreWords) ? 4 : 2;
					if (cursor + argumentSize > end)
						return Fail(fmt::format("Composite glyph {}'s components exceed its data", glyph));
					if (flags & c_ArgsAreWords)
					{
						offsetX = S16(cursor);
						offsetY = S16(cursor + 2);
					}
					else
					{
						offsetX = S8(cursor);
						offsetY = S8(cursor + 1);
					}
					cursor += argumentSize;

					// The transform's 2x2 part, read in the order stb_truetype checks the flags.
					double a = 1.0;
					double b = 0.0;
					double c = 0.0;
					double d = 1.0;
					const uint64_t scaleSize = (flags & c_HaveScale) ? 2 : ((flags & c_HaveXYScale) ? 4 : ((flags & c_HaveTwoByTwo) ? 8 : 0));
					if (cursor + scaleSize > end)
						return Fail(fmt::format("Composite glyph {}'s components exceed its data", glyph));
					if (flags & c_HaveScale)
					{
						a = d = S16(cursor) / 16384.0;
					}
					else if (flags & c_HaveXYScale)
					{
						a = S16(cursor) / 16384.0;
						d = S16(cursor + 2) / 16384.0;
					}
					else if (flags & c_HaveTwoByTwo)
					{
						a = S16(cursor) / 16384.0;
						b = S16(cursor + 2) / 16384.0;
						c = S16(cursor + 4) / 16384.0;
						d = S16(cursor + 6) / 16384.0;
					}
					cursor += scaleSize;

					if (component >= m_GlyphCount)
						return Fail(fmt::format("Composite glyph {} uses glyph {}, which does not exist", glyph, component));
					if (!ValidateGlyph(component, depth + 1))
						return false;

					// stb_truetype recurses into every component of every nested composite, and copies the outline gathered
					// so far for each one: bound the nesting and the expanded component count. The nesting is checked here as
					// well because a component validated earlier (as part of another glyph) was reached from a shallower depth.
					const GlyphSummary& child = m_Glyphs[component];
					height = std::max(height, child.Height + 1);
					if (height > FontLimits::c_MaxCompositeDepth)
						return Fail(fmt::format("Composite glyphs are nested more than {} levels deep", FontLimits::c_MaxCompositeDepth));
					components = SaturatingAdd(components, SaturatingAdd(child.Components, 1));
					if (components > FontLimits::c_MaxCompositeComponents)
						return Fail(fmt::format("Composite glyph {} expands to more than {} components", glyph, FontLimits::c_MaxCompositeComponents));

					// stb_truetype transforms the component's coordinates into 16-bit integers: keep them representable.
					const double scaleX = std::sqrt(a * a + b * b);
					const double scaleY = std::sqrt(c * c + d * d);
					const double boundX = scaleX * (std::abs(a) * child.MaxX + std::abs(c) * child.MaxY + std::abs(offsetX));
					const double boundY = scaleY * (std::abs(b) * child.MaxX + std::abs(d) * child.MaxY + std::abs(offsetY));
					if (boundX > FontLimits::c_MaxTransformedCoordinate || boundY > FontLimits::c_MaxTransformedCoordinate)
						return Fail(fmt::format("Composite glyph {} moves a component beyond the coordinate range", glyph));
					maxX = std::max(maxX, boundX);
					maxY = std::max(maxY, boundY);
					points = SaturatingAdd(points, child.Points);
					more = (flags & c_MoreComponents) != 0;
				}

				GlyphSummary& summary = m_Glyphs[glyph];
				summary.Points = points;
				summary.Height = height;
				summary.Components = components;
				summary.MaxX = maxX;
				summary.MaxY = maxY;
				return true;
			}

			// The subtable stb_truetype uses: the last Microsoft Unicode (BMP or full) or Unicode-platform record.
			bool ValidateCharacterMap()
			{
				if (!m_Cmap.Has(0, 4))
					return Fail("The font's cmap table is too short");
				const uint32_t recordCount = U16(m_Cmap.Offset + 2);
				if (!m_Cmap.Has(4, 8ull * recordCount))
					return Fail("The font's cmap records exceed the table");
				std::optional<uint64_t> selected;
				for (uint32_t index = 0; index < recordCount; index++)
				{
					const uint64_t record = m_Cmap.Offset + 4 + 8ull * index;
					const uint16_t platform = U16(record);
					const uint16_t encoding = U16(record + 2);
					if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
						selected = U32(record + 4);
				}
				if (!selected)
					return Fail("The font has no Unicode character map");
				if (!m_Cmap.Has(*selected, 2))
					return Fail("The font's character map lies outside its cmap table");

				const uint64_t subtable = m_Cmap.Offset + *selected;
				const uint64_t available = m_Cmap.Length - *selected;
				auto fits = [&](uint64_t size) { return size <= available; };
				const uint16_t format = U16(subtable);
				switch (format)
				{
					case 0:
					{
						// stb_truetype reads index 6 + code for codes below the subtable's length - 6.
						if (!fits(4) || !fits(std::max<uint64_t>(U16(subtable + 2), 4)))
							return Fail("The font's character map (format 0) exceeds its table");
						return true;
					}
					case 6:
					{
						if (!fits(10) || !fits(10 + 2ull * U16(subtable + 8)))
							return Fail("The font's character map (format 6) exceeds its table");
						return true;
					}
					case 4:
						return ValidateSegmentMap(subtable, available);
					case 12:
					case 13:
					{
						if (!fits(16))
							return Fail(fmt::format("The font's character map (format {}) exceeds its table", format));
						const uint64_t groupCount = U32(subtable + 12);
						if (!fits(16 + 12 * groupCount))
							return Fail(fmt::format("The font's character map (format {}) exceeds its table", format));
						for (uint64_t group = 0; group < groupCount; group++)
						{
							const uint64_t record = subtable + 16 + 12 * group;
							const uint32_t first = U32(record);
							const uint32_t last = U32(record + 4);
							const uint32_t glyph = U32(record + 8);
							if (first > last || last > c_MaxCodepoint || glyph > 0x7FFFFFFFu)
								return Fail(fmt::format("The font's character map (format {}) has an invalid group", format));
						}
						return true;
					}
					case 2:
						return Fail("The font's character map uses format 2 (multi-byte encodings), which is not supported");
					default:
						return Fail(fmt::format("The font's character map uses format {}, which is not supported", format));
				}
			}

			// Format 4: stb_truetype's binary search trusts searchRange, entrySelector and rangeShift, so they must be
			// exactly what the segment count implies; glyph index arrays must lie inside the table.
			bool ValidateSegmentMap(uint64_t subtable, uint64_t available)
			{
				if (available < 14)
					return Fail("The font's character map (format 4) exceeds its table");
				const uint32_t segmentsTimesTwo = U16(subtable + 6);
				if (segmentsTimesTwo == 0 || segmentsTimesTwo % 2 != 0)
					return Fail("The font's character map (format 4) has an invalid segment count");
				const uint64_t segments = segmentsTimesTwo / 2;
				if (16 + 8 * segments > available)
					return Fail("The font's character map (format 4) exceeds its table");
				uint32_t log2 = 0;
				while ((2ull << log2) <= segments)
					log2++;
				const uint32_t searchRange = 2u << log2;
				if (U16(subtable + 8) != searchRange || U16(subtable + 10) != log2 || U16(subtable + 12) != segmentsTimesTwo - searchRange)
					return Fail("The font's character map (format 4) has inconsistent search parameters");
				for (uint64_t segment = 0; segment < segments; segment++)
				{
					const uint16_t last = U16(subtable + 14 + 2 * segment);
					const uint16_t first = U16(subtable + 16 + 2 * segments + 2 * segment);
					const uint64_t rangeOffsetPosition = 16 + 6 * segments + 2 * segment;
					const uint16_t rangeOffset = U16(subtable + rangeOffsetPosition);
					if (rangeOffset != 0 && first <= last && rangeOffsetPosition + rangeOffset + 2ull * (last - first) + 2 > available)
						return Fail("The font's character map (format 4) points outside its table");
				}
				return true;
			}

			// stb_truetype reads GPOS when the font has it, otherwise kern.
			void ValidateKerning()
			{
				std::string issue;
				if (m_Gpos.Present)
					m_Facts.KerningUsable = ValidatePositioning(issue);
				else if (m_Kern.Present)
					m_Facts.KerningUsable = ValidateKern(issue);
				else
					m_Facts.KerningUsable = true;
				m_Facts.KerningIssue = issue;
			}

			bool ValidateKern(std::string& outIssue)
			{
				const TableRange& kern = m_Kern;
				if (!kern.Has(0, 4))
				{
					outIssue = "kern table is too short";
					return false;
				}
				if (U16(kern.Offset + 2) == 0)
					return true; // No subtables: stb_truetype stops here
				if (!kern.Has(0, 10))
				{
					outIssue = "kern table is too short";
					return false;
				}
				if (U16(kern.Offset + 8) != 1)
					return true; // Not a horizontal format 0 subtable: stb_truetype stops here
				if (!kern.Has(0, 12) || !kern.Has(18, 6ull * U16(kern.Offset + 10)))
				{
					outIssue = "kern pairs exceed the table";
					return false;
				}
				return true;
			}

			// GPOS pair adjustment lookups, as stb_truetype reads them (offsets are relative to the GPOS table). For every
			// glyph pair it kerns, stb_truetype walks all lookups and all subtables of the pair adjustment lookups: the walk
			// is bounded, counted with repeats since offsets may share their targets. Each distinct subtable is then
			// validated once, within a total work budget.
			bool ValidatePositioning(std::string& outIssue)
			{
				auto fail = [&](const char* issue)
				{
					outIssue = issue;
					return false;
				};

				if (!GposHas(0, 4))
					return fail("GPOS header is too short");
				if (GposU16(0) != 1 || GposU16(2) != 0)
					return true; // Other versions: stb_truetype stops here
				if (!GposHas(0, 10))
					return fail("GPOS header is too short");
				const uint64_t lookupList = GposU16(8);
				if (!GposHas(lookupList, 2) || !GposHas(lookupList + 2, 2ull * GposU16(lookupList)))
					return fail("GPOS lookup list exceeds the table");
				const uint32_t lookupCount = GposU16(lookupList);
				uint64_t visits = lookupCount;
				if (visits > FontLimits::c_MaxKerningLookupVisits)
					return fail("GPOS has too many lookups to search for every glyph pair");
				for (uint32_t lookup = 0; lookup < lookupCount; lookup++)
				{
					const uint64_t lookupTable = lookupList + GposU16(lookupList + 2 + 2ull * lookup);
					if (!GposHas(lookupTable, 6))
						return fail("GPOS lookup exceeds the table");
					if (GposU16(lookupTable) != 2)
						continue; // Only pair adjustment lookups are searched
					const uint32_t subtableCount = GposU16(lookupTable + 4);
					visits += subtableCount;
					if (visits > FontLimits::c_MaxKerningLookupVisits)
						return fail("GPOS has too many pair adjustment subtables to search for every glyph pair");
					if (!GposHas(lookupTable + 6, 2ull * subtableCount))
						return fail("GPOS lookup exceeds the table");
				}
				m_Facts.KerningLookupVisits = static_cast<uint32_t>(visits);

				std::unordered_set<uint64_t> validated;
				uint64_t work = 0;
				for (uint32_t lookup = 0; lookup < lookupCount; lookup++)
				{
					const uint64_t lookupTable = lookupList + GposU16(lookupList + 2 + 2ull * lookup);
					if (GposU16(lookupTable) != 2)
						continue;
					for (uint32_t index = 0; index < GposU16(lookupTable + 4); index++)
					{
						const uint64_t subtable = lookupTable + GposU16(lookupTable + 6 + 2ull * index);
						if (validated.insert(subtable).second && !ValidatePairSubtable(subtable, work, outIssue))
							return false;
					}
				}
				return true;
			}

			bool ValidatePairSubtable(uint64_t subtable, uint64_t& work, std::string& outIssue)
			{
				auto fail = [&](const char* issue)
				{
					outIssue = issue;
					return false;
				};
				// Steps of the loops below, so that crafted tables cannot make validation run without bound.
				auto spend = [&](uint64_t steps)
				{
					work += steps;
					return work <= FontLimits::c_MaxKerningValidationWork;
				};
				const char* tooLarge = "GPOS kerning data is too large to validate";
				if (!spend(1))
					return fail(tooLarge);
				if (!GposHas(subtable, 4))
					return fail("GPOS pair subtable exceeds the table");

				// The coverage table is searched before the subtable's format is looked at.
				const uint64_t coverage = subtable + GposU16(subtable + 2);
				int64_t maxCoverageIndex = -1;
				if (!GposHas(coverage, 2))
					return fail("GPOS coverage exceeds the table");
				const uint16_t coverageFormat = GposU16(coverage);
				if (coverageFormat == 1)
				{
					if (!GposHas(coverage, 4) || !GposHas(coverage + 4, 2ull * GposU16(coverage + 2)))
						return fail("GPOS coverage exceeds the table");
					maxCoverageIndex = static_cast<int64_t>(GposU16(coverage + 2)) - 1;
				}
				else if (coverageFormat == 2)
				{
					const uint32_t rangeCount = GposU16(coverage + 2);
					if (!GposHas(coverage, 4) || !GposHas(coverage + 4, 6ull * rangeCount))
						return fail("GPOS coverage exceeds the table");
					if (!spend(rangeCount))
						return fail(tooLarge);
					for (uint32_t range = 0; range < rangeCount; range++)
					{
						const uint64_t record = coverage + 4 + 6ull * range;
						if (GposU16(record) <= GposU16(record + 2))
							maxCoverageIndex = std::max<int64_t>(maxCoverageIndex, static_cast<int64_t>(GposU16(record + 4)) + GposU16(record + 2) - GposU16(record));
					}
				}
				if (maxCoverageIndex < 0)
					return true; // No glyph is covered: stb_truetype skips the subtable

				const uint16_t format = GposU16(subtable);
				if (format != 1 && format != 2)
					return true;
				if (!GposHas(subtable, 8))
					return fail("GPOS pair subtable exceeds the table");
				if (GposU16(subtable + 4) != 4 || GposU16(subtable + 6) != 0)
					return true; // Value formats stb_truetype does not read
				if (format == 1)
				{
					// A covered glyph's pair set offset, and the value count it points to, are read before the glyph's
					// coverage index is checked against the pair set count.
					if (!GposHas(subtable, 10))
						return fail("GPOS pair subtable exceeds the table");
					const uint32_t pairSetCount = GposU16(subtable + 8);
					const uint64_t offsetsRead = std::max<uint64_t>(pairSetCount, static_cast<uint64_t>(maxCoverageIndex + 1));
					if (!GposHas(subtable + 10, 2 * offsetsRead))
						return fail("GPOS pair sets exceed the table");
					if (!spend(offsetsRead))
						return fail(tooLarge);
					for (uint64_t pairSet = 0; pairSet < offsetsRead; pairSet++)
					{
						const uint64_t values = subtable + GposU16(subtable + 10 + 2 * pairSet);
						if (!GposHas(values, 2) || (pairSet < pairSetCount && !GposHas(values + 2, 4ull * GposU16(values))))
							return fail("GPOS pair values exceed the table");
					}
					return true;
				}

				if (!GposHas(subtable, 16))
					return fail("GPOS class pair subtable exceeds the table");
				for (uint64_t classDefinition : { subtable + GposU16(subtable + 8), subtable + GposU16(subtable + 10) })
				{
					if (!GposHas(classDefinition, 2))
						return fail("GPOS class definition exceeds the table");
					const uint16_t classFormat = GposU16(classDefinition);
					if (classFormat == 1 && (!GposHas(classDefinition, 6) || !GposHas(classDefinition + 6, 2ull * GposU16(classDefinition + 4))))
						return fail("GPOS class definition exceeds the table");
					if (classFormat == 2 && (!GposHas(classDefinition, 4) || !GposHas(classDefinition + 4, 6ull * GposU16(classDefinition + 2))))
						return fail("GPOS class definition exceeds the table");
				}
				if (!GposHas(subtable + 16, 2ull * GposU16(subtable + 12) * GposU16(subtable + 14)))
					return fail("GPOS class pair values exceed the table");
				return true;
			}

			bool GposHas(uint64_t offset, uint64_t size) const { return m_Gpos.Has(offset, size); }
			uint16_t GposU16(uint64_t offset) const { return U16(m_Gpos.Offset + offset); }
		private:
			std::span<const uint8_t> m_Data;
			TrueTypeFontFacts& m_Facts;
			std::string m_Error;

			TableRange m_Cmap;
			TableRange m_Head;
			TableRange m_Hhea;
			TableRange m_Hmtx;
			TableRange m_Maxp;
			TableRange m_Loca;
			TableRange m_Glyf;
			TableRange m_Kern;
			TableRange m_Gpos;
			uint32_t m_LocaFormat = 0;
			uint32_t m_GlyphCount = 0;
			uint64_t m_TotalPoints = 0; // Of the simple glyphs validated so far
			std::vector<uint64_t> m_GlyphOffsets; // Relative to glyf; one more than glyphs
			std::vector<GlyphSummary> m_Glyphs;
			std::vector<uint16_t> m_ContourEnds; // Scratch
			std::vector<uint8_t> m_PointFlags;   // Scratch
		};

	}

	bool ValidateTrueTypeFont(std::span<const uint8_t> data, TrueTypeFontFacts& outFacts, std::string& outError)
	{
		outFacts = TrueTypeFontFacts();
		Validator validator(data, outFacts);
		if (!validator.Validate(outError))
		{
			outFacts = TrueTypeFontFacts();
			return false;
		}
		return true;
	}

}
