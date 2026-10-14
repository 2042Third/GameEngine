#include "stpch.h"
#include "Strata/Renderer/TextLayout.h"

#include "Strata/Renderer/FontAtlas.h"
#include "Strata/Renderer/FontFace.h"

#include <algorithm>
#include <limits>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_ReplacementCharacter = 0xFFFD;
		constexpr uint32_t c_EllipsisCharacter = 0x2026;
		constexpr uint32_t c_TabWidth = 4;      // In spaces
		constexpr float c_FitTolerance = 1e-4f; // Em a line may exceed its width by (rounding)

		// A code point of a paragraph, as the line breaker sees it.
		struct ShapeItem
		{
			uint32_t Glyph = 0;
			float Advance = 0.0f;
			float Origin = 0.0f;   // Pen position of the glyph in its line (kerning applied), set while lines are broken
			bool Skip = false;     // A control character: no glyph, no advance, no break
			bool Tab = false;
			bool Space = false;    // Whitespace the line may break at (spaces and tabs), dropped at a break
			bool Hyphen = false;   // The line may break after it
			bool Drawable = false; // Has a shape
		};

		// Scratch of one shaping, kept to reuse its allocations (main thread only, like the fonts).
		struct ShapeScratch
		{
			std::vector<uint32_t> Codepoints;
			std::vector<ShapeItem> Items;
			ShapedText Shaped;
		};

		ShapeScratch& GetScratch()
		{
			thread_local ShapeScratch t_Scratch;
			return t_Scratch;
		}

		bool IsBreakingSpace(uint32_t codepoint)
		{
			return codepoint == ' ' || codepoint == '\t' || codepoint == 0x3000; // Ideographic space too
		}

		bool IsHyphen(uint32_t codepoint)
		{
			return codepoint == '-' || codepoint == 0x2010 || codepoint == 0x2013; // Hyphen-minus, hyphen, en dash
		}

		template<typename Kerning>
		class Shaper
		{
		public:
			Shaper(const FontFace& face, const TextShapeOptions& options, Kerning& kerning, ShapedText& out)
				: m_Face(face), m_Options(options), m_Kerning(kerning), m_Out(out)
			{
				const FontMetrics& metrics = face.GetMetrics();
				m_LineHeight = metrics.LineHeight * std::max(options.LineSpacing, 0.0f);
				m_SpaceAdvance = face.GetGlyphMetrics(face.GetGlyphIndex(' ')).Advance;
				const bool cuts = options.Overflow != TextOverflowMode::Overflow;
				// Lines that fit the box: the first line's descent must fit, each further line adds a line height.
				if (cuts && options.BoxHeight > 0.0f)
				{
					const float firstLine = metrics.Ascent - metrics.Descent;
					if (options.BoxHeight + c_FitTolerance < firstLine)
						m_MaxLines = 0;
					else if (m_LineHeight > 0.0f)
						m_MaxLines = static_cast<uint32_t>(std::min((options.BoxHeight - firstLine + c_FitTolerance) / m_LineHeight + 1.0f, 1.0e6f));
				}
				// The width lines are cut at.
				if (cuts && options.MaxWidth > 0.0f)
					m_CutWidth = options.MaxWidth;
				else if (cuts && options.WrapWidth > 0.0f)
					m_CutWidth = options.WrapWidth;
			}

			// Lays out the paragraphs of `codepoints` ('\n' separates them).
			void Run(const std::vector<uint32_t>& codepoints, std::vector<ShapeItem>& items)
			{
				size_t paragraphStart = 0;
				while (paragraphStart <= codepoints.size() && !m_Full)
				{
					size_t paragraphEnd = paragraphStart;
					while (paragraphEnd < codepoints.size() && codepoints[paragraphEnd] != '\n')
						paragraphEnd++;
					m_MoreParagraphs = paragraphEnd < codepoints.size();
					BuildItems(codepoints, paragraphStart, paragraphEnd, items);
					BreakParagraph(items);
					paragraphStart = paragraphEnd + 1;
				}
				// Text left out because the box was full.
				if (m_StoppedEarly || (m_Full && paragraphStart <= codepoints.size()))
					m_Out.Truncated = true;
				Finish();
			}
		private:
			struct Line
			{
				size_t FirstGlyph = 0; // In m_Out.Glyphs
				size_t EndGlyph = 0;
				float Width = 0.0f;
			};

			void BuildItems(const std::vector<uint32_t>& codepoints, size_t begin, size_t end, std::vector<ShapeItem>& items)
			{
				items.clear();
				items.reserve(end - begin);
				for (size_t index = begin; index < end; index++)
				{
					const uint32_t codepoint = codepoints[index];
					ShapeItem& item = items.emplace_back();
					if (codepoint == '\t')
					{
						item.Tab = true;
						item.Space = true;
						item.Advance = m_SpaceAdvance * static_cast<float>(c_TabWidth);
						continue;
					}
					if (codepoint < 0x20 || codepoint == 0x7F)
					{
						item.Skip = true;
						continue;
					}
					item.Glyph = m_Face.GetGlyphIndex(codepoint);
					const FontGlyphMetrics& metrics = m_Face.GetGlyphMetrics(item.Glyph);
					item.Advance = metrics.Advance;
					item.Drawable = metrics.Drawable;
					item.Space = IsBreakingSpace(codepoint);
					item.Hyphen = IsHyphen(codepoint);
				}
			}

			// Breaks a paragraph into lines (greedily, at the last opportunity before the wrap width) and emits them.
			void BreakParagraph(std::vector<ShapeItem>& items)
			{
				const float wrap = m_Options.WrapWidth;
				size_t lineStart = 0;
				do
				{
					if (m_Full)
					{
						m_StoppedEarly = true;
						return;
					}
					float pen = 0.0f;
					bool hasPrevious = false;
					uint32_t previous = 0;
					// The last break opportunity: the line ends before End (at Width) and the next starts at Next.
					bool canBreak = false;
					size_t breakEnd = 0;
					size_t breakNext = 0;
					float breakWidth = 0.0f;
					bool inSpaceRun = false;

					size_t lineEnd = items.size();
					size_t nextStart = items.size();
					float lineWidth = 0.0f;
					bool wrapped = false;
					for (size_t index = lineStart; index < items.size(); index++)
					{
						ShapeItem& item = items[index];
						if (item.Skip)
							continue;
						float kerning = 0.0f;
						if (!item.Tab && hasPrevious)
							kerning = m_Kerning(previous, item.Glyph);
						const float advance = kerning + item.Advance;
						// Whitespace hangs past the width; anything else wraps once the line holds something.
						if (wrap > 0.0f && !item.Space && index > lineStart && pen + advance > wrap + c_FitTolerance)
						{
							if (canBreak)
							{
								lineEnd = breakEnd;
								nextStart = breakNext;
								lineWidth = breakWidth;
							}
							else
							{
								lineEnd = index;
								nextStart = index;
								lineWidth = pen;
							}
							wrapped = true;
							break;
						}

						item.Origin = pen + kerning;
						if (item.Space)
						{
							if (!inSpaceRun)
							{
								breakEnd = index;
								breakWidth = pen;
							}
							breakNext = index + 1;
							canBreak = true;
							inSpaceRun = true;
						}
						else
						{
							inSpaceRun = false;
						}
						pen += advance;
						if (item.Hyphen)
						{
							canBreak = true;
							breakEnd = index + 1;
							breakNext = index + 1;
							breakWidth = pen;
						}
						hasPrevious = !item.Tab;
						previous = item.Glyph;
					}
					if (!wrapped)
						lineWidth = pen; // The paragraph's last line keeps its trailing spaces, as typed
					EmitLine(items, lineStart, lineEnd, lineWidth, nextStart < items.size());
					lineStart = nextStart;
				} while (lineStart < items.size());
			}

			float GetEllipsisWidth() const
			{
				if (m_Face.HasGlyph(c_EllipsisCharacter))
					return m_Face.GetGlyphMetrics(m_Face.GetGlyphIndex(c_EllipsisCharacter)).Advance;
				return 3.0f * m_Face.GetGlyphMetrics(m_Face.GetGlyphIndex('.')).Advance;
			}

			// The end of a line's items whose glyphs end within `width` (trailing whitespace dropped), and that end's pen.
			size_t FitItems(const std::vector<ShapeItem>& items, size_t begin, size_t end, float width, float& outPen) const
			{
				size_t fitEnd = begin;
				outPen = 0.0f;
				for (size_t index = begin; index < end; index++)
				{
					const ShapeItem& item = items[index];
					if (item.Skip)
						continue;
					const float itemEnd = item.Origin + item.Advance;
					if (itemEnd > width + c_FitTolerance)
						break;
					if (!item.Space)
					{
						fitEnd = index + 1;
						outPen = itemEnd;
					}
				}
				return fitEnd;
			}

			void EmitLine(const std::vector<ShapeItem>& items, size_t begin, size_t end, float width, bool moreFollow)
			{
				if (m_LinesEmitted >= m_MaxLines)
				{
					// The box holds no line at all.
					m_Full = true;
					m_StoppedEarly = true;
					return;
				}
				m_LinesEmitted++;
				// The last line the box holds: when more text follows (in this paragraph or later ones), it ends the text shown.
				const bool lastAllowed = m_MaxLines != std::numeric_limits<uint32_t>::max() && m_LinesEmitted >= m_MaxLines;

				// The line's glyphs, without trailing whitespace, decide whether it is too wide.
				float contentWidth = 0.0f;
				for (size_t index = begin; index < end; index++)
				{
					if (!items[index].Skip && !items[index].Space)
						contentWidth = items[index].Origin + items[index].Advance;
				}
				bool ellipsis = false;
				size_t glyphEnd = end;
				float lineWidth = width;
				const bool cutByWidth = m_CutWidth > 0.0f && contentWidth > m_CutWidth + c_FitTolerance;
				if (cutByWidth || (lastAllowed && (moreFollow || m_MoreParagraphs)))
				{
					// Glyphs are cut where the line is too wide, or this is the last line shown of a longer text.
					m_Out.Truncated = true;
					ellipsis = m_Options.Overflow == TextOverflowMode::Ellipsis;
					const float limit = m_CutWidth > 0.0f ? m_CutWidth : std::numeric_limits<float>::max();
					const float ellipsisWidth = ellipsis ? GetEllipsisWidth() : 0.0f;
					float pen = 0.0f;
					glyphEnd = FitItems(items, begin, end, std::max(limit - ellipsisWidth, 0.0f), pen);
					lineWidth = pen + ellipsisWidth;
				}

				Line line;
				line.FirstGlyph = m_Out.Glyphs.size();
				const float baseline = -static_cast<float>(m_LinesEmitted - 1) * m_LineHeight;
				for (size_t index = begin; index < glyphEnd; index++)
				{
					const ShapeItem& item = items[index];
					if (item.Drawable && !item.Skip)
						m_Out.Glyphs.push_back(ShapedGlyph { item.Glyph, glm::vec2(item.Origin, baseline) });
				}
				if (ellipsis)
				{
					float pen = lineWidth - GetEllipsisWidth();
					if (m_Face.HasGlyph(c_EllipsisCharacter))
					{
						AppendGlyph(m_Face.GetGlyphIndex(c_EllipsisCharacter), pen, baseline);
					}
					else
					{
						const uint32_t period = m_Face.GetGlyphIndex('.');
						for (int dot = 0; dot < 3; dot++)
						{
							AppendGlyph(period, pen, baseline);
							pen += m_Face.GetGlyphMetrics(period).Advance;
						}
					}
				}
				line.EndGlyph = m_Out.Glyphs.size();
				line.Width = lineWidth;
				m_Lines.push_back(line);
				if (lastAllowed)
					m_Full = true;
			}

			void AppendGlyph(uint32_t glyph, float pen, float baseline)
			{
				if (m_Face.GetGlyphMetrics(glyph).Drawable)
					m_Out.Glyphs.push_back(ShapedGlyph { glyph, glm::vec2(pen, baseline) });
			}

			// Aligns the lines and places the block vertically.
			void Finish()
			{
				const FontMetrics& metrics = m_Face.GetMetrics();
				const uint32_t lineCount = static_cast<uint32_t>(m_Lines.size());
				m_Out.LineCount = lineCount;
				if (lineCount == 0)
					return;
				const float height = metrics.Ascent - metrics.Descent + static_cast<float>(lineCount - 1) * m_LineHeight;
				float top = 0.0f; // The block's top relative to the box's top
				const float box = m_Options.BoxHeight;
				switch (m_Options.VerticalAlignment)
				{
				case TextVerticalAlignment::Top:
					top = 0.0f;
					break;
				case TextVerticalAlignment::Middle:
					top = box > 0.0f ? -(box - height) * 0.5f : height * 0.5f;
					break;
				case TextVerticalAlignment::Bottom:
					top = box > 0.0f ? -(box - height) : height;
					break;
				}
				const float firstBaseline = top - metrics.Ascent;

				float left = std::numeric_limits<float>::max();
				float right = std::numeric_limits<float>::lowest();
				float widest = 0.0f;
				for (const Line& line : m_Lines)
				{
					const TextAlignment alignment = m_Options.Alignment;
					const float shift = alignment == TextAlignment::Left ? 0.0f : (alignment == TextAlignment::Center ? -line.Width * 0.5f : -line.Width);
					for (size_t glyph = line.FirstGlyph; glyph < line.EndGlyph; glyph++)
					{
						m_Out.Glyphs[glyph].Pen.x += shift;
						m_Out.Glyphs[glyph].Pen.y += firstBaseline;
					}
					left = std::min(left, shift);
					right = std::max(right, shift + line.Width);
					widest = std::max(widest, line.Width);
				}
				m_Out.Width = widest;
				m_Out.Height = height;
				m_Out.Min = glm::vec2(left, top - height);
				m_Out.Max = glm::vec2(right, top);
			}
		private:
			const FontFace& m_Face;
			const TextShapeOptions& m_Options;
			Kerning& m_Kerning;
			ShapedText& m_Out;
			float m_LineHeight = 0.0f;
			float m_SpaceAdvance = 0.0f;
			float m_CutWidth = 0.0f;
			uint32_t m_MaxLines = std::numeric_limits<uint32_t>::max();
			uint32_t m_LinesEmitted = 0;
			bool m_Full = false;        // The box holds no more lines
			bool m_StoppedEarly = false; // Text was left out because the box was full
			bool m_MoreParagraphs = false; // Paragraphs follow the one being broken
			std::vector<Line> m_Lines;
		};

		template<typename Kerning>
		void ShapeWith(const FontFace& face, std::string_view text, const TextShapeOptions& options, Kerning& kerning, ShapedText& outText)
		{
			outText.Glyphs.clear();
			outText.Min = glm::vec2(0.0f);
			outText.Max = glm::vec2(0.0f);
			outText.Width = 0.0f;
			outText.Height = 0.0f;
			outText.LineCount = 0;
			outText.Truncated = false;

			ShapeScratch& scratch = GetScratch();
			DecodeUTF8(text, scratch.Codepoints);
			Shaper<Kerning> shaper(face, options, kerning, outText);
			shaper.Run(scratch.Codepoints, scratch.Items);
		}

	}

	void DecodeUTF8(std::string_view text, std::vector<uint32_t>& outCodepoints)
	{
		outCodepoints.clear();
		size_t index = 0;
		while (index < text.size())
		{
			const uint8_t lead = static_cast<uint8_t>(text[index]);
			uint32_t length = 0;
			uint32_t codepoint = 0;
			uint32_t minimum = 0;
			if (lead < 0x80)
			{
				outCodepoints.push_back(lead);
				index++;
				continue;
			}
			if ((lead & 0xE0) == 0xC0)
			{
				length = 2;
				codepoint = lead & 0x1F;
				minimum = 0x80;
			}
			else if ((lead & 0xF0) == 0xE0)
			{
				length = 3;
				codepoint = lead & 0x0F;
				minimum = 0x800;
			}
			else if ((lead & 0xF8) == 0xF0)
			{
				length = 4;
				codepoint = lead & 0x07;
				minimum = 0x10000;
			}
			else
			{
				outCodepoints.push_back(c_ReplacementCharacter); // Continuation byte or invalid lead
				index++;
				continue;
			}

			// A truncated sequence replaces only the bytes that belong to it.
			uint32_t consumed = 1;
			bool valid = true;
			for (; consumed < length; consumed++)
			{
				if (index + consumed >= text.size() || (static_cast<uint8_t>(text[index + consumed]) & 0xC0) != 0x80)
				{
					valid = false;
					break;
				}
				codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index + consumed]) & 0x3F);
			}
			if (!valid || codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
				codepoint = c_ReplacementCharacter;
			outCodepoints.push_back(codepoint);
			index += consumed;
		}
	}

	void ShapeText(const FontFace& face, std::string_view text, const TextShapeOptions& options, ShapedText& outText)
	{
		auto kerning = [&face](uint32_t left, uint32_t right) { return face.GetKerning(left, right); };
		ShapeWith(face, text, options, kerning, outText);
	}

	void LayoutText(FontAtlas& atlas, std::string_view text, TextAlignment alignment, TextLayout& outLayout)
	{
		// The atlas's own kerning cache, so that what one atlas looks up is counted there (FontAtlas::GetCachedKerningCount).
		auto kerning = [&atlas](uint32_t left, uint32_t right) { return atlas.GetKerning(left, right); };
		TextShapeOptions options;
		options.Alignment = alignment;
		ShapedText& shaped = GetScratch().Shaped;
		ShapeWith(atlas.GetFace(), text, options, kerning, shaped);
		atlas.Resolve(shaped, outLayout);
	}

}
