#include "stpch.h"
#include "Strata/Renderer/Font.h"

namespace Strata
{

	namespace
	{

		uint16_t ReadU16(const std::vector<uint8_t>& data, size_t offset)
		{
			return static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
		}

		uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset)
		{
			return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16)
				| (static_cast<uint32_t>(data[offset + 2]) << 8) | static_cast<uint32_t>(data[offset + 3]);
		}

		bool HasTag(const std::vector<uint8_t>& data, size_t offset, const char* tag)
		{
			return offset + 4 <= data.size() && std::memcmp(data.data() + offset, tag, 4) == 0;
		}

		// Validates an sfnt offset table: every table lies inside the file and the tables needed to render glyphs exist.
		bool ValidateFontAt(const std::vector<uint8_t>& data, size_t offset, std::string& outError)
		{
			if (offset > data.size() || data.size() - offset < 12)
			{
				outError = "Font file is truncated";
				return false;
			}

			const uint32_t version = ReadU32(data, offset);
			const bool cff = HasTag(data, offset, "OTTO");
			if (version != 0x00010000 && !HasTag(data, offset, "true") && !cff)
			{
				outError = "Not a TrueType or OpenType font";
				return false;
			}

			const uint16_t tableCount = ReadU16(data, offset + 4);
			if (tableCount == 0 || static_cast<size_t>(tableCount) * 16 > data.size() - offset - 12)
			{
				outError = "Font table directory is invalid";
				return false;
			}

			bool hasCmap = false;
			bool hasHead = false;
			bool hasHhea = false;
			bool hasHmtx = false;
			bool hasMaxp = false;
			bool hasOutlines = false;
			for (uint16_t index = 0; index < tableCount; index++)
			{
				const size_t record = offset + 12 + static_cast<size_t>(index) * 16;
				const uint64_t tableOffset = ReadU32(data, record + 8);
				const uint64_t tableLength = ReadU32(data, record + 12);
				if (tableOffset + tableLength > data.size())
				{
					outError = "A font table lies outside the file";
					return false;
				}

				hasCmap |= HasTag(data, record, "cmap");
				hasHead |= HasTag(data, record, "head");
				hasHhea |= HasTag(data, record, "hhea");
				hasHmtx |= HasTag(data, record, "hmtx");
				hasMaxp |= HasTag(data, record, "maxp");
				hasOutlines |= HasTag(data, record, "glyf") || HasTag(data, record, "CFF ") || HasTag(data, record, "CFF2");
			}

			if (!hasCmap || !hasHead || !hasHhea || !hasHmtx || !hasMaxp || !hasOutlines)
			{
				outError = "Font is missing required tables (cmap, head, hhea, hmtx, maxp and glyph outlines)";
				return false;
			}
			return true;
		}

	}

	Ref<Font> Font::Create(std::vector<uint8_t> fontData, std::string* outError)
	{
		std::string error;
		bool valid = false;
		if (HasTag(fontData, 0, "ttcf"))
		{
			// Font collection: validate the first font, which is the one the text renderer uses.
			if (fontData.size() >= 16 && ReadU32(fontData, 8) > 0)
				valid = ValidateFontAt(fontData, ReadU32(fontData, 12), error);
			else
				error = "Font collection header is invalid";
		}
		else
		{
			valid = ValidateFontAt(fontData, 0, error);
		}

		if (!valid)
		{
			if (outError)
				*outError = error;
			return nullptr;
		}

		Ref<Font> font(new Font());
		font->m_Data = std::move(fontData);
		return font;
	}

}
