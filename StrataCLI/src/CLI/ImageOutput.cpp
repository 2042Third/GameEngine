#include "CLI/ImageOutput.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"

#include <array>

namespace Strata::CLI
{

	namespace
	{

		constexpr int8_t c_InvalidDigit = -1;

		constexpr std::array<int8_t, 256> MakeDecodeTable()
		{
			std::array<int8_t, 256> table = {};
			for (int8_t& entry : table)
				entry = c_InvalidDigit;
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			for (size_t index = 0; index < alphabet.size(); index++)
				table[static_cast<uint8_t>(alphabet[index])] = static_cast<int8_t>(index);
			return table;
		}

		constexpr std::array<int8_t, 256> c_DecodeTable = MakeDecodeTable();

		bool IsWhitespace(char character)
		{
			return character == ' ' || character == '\t' || character == '\r' || character == '\n';
		}

	}

	std::optional<std::vector<uint8_t>> DecodeBase64(std::string_view text)
	{
		std::vector<uint8_t> bytes;
		bytes.reserve(text.size() / 4 * 3 + 3);

		uint32_t accumulator = 0;
		size_t digits = 0;  // Digits in the current group of four
		size_t padding = 0; // '=' seen at the end
		for (char character : text)
		{
			if (IsWhitespace(character))
				continue;
			if (character == '=')
			{
				padding++;
				continue;
			}
			// Nothing but padding may follow padding.
			if (padding > 0)
				return std::nullopt;

			const int8_t value = c_DecodeTable[static_cast<uint8_t>(character)];
			if (value == c_InvalidDigit)
				return std::nullopt;
			accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
			if (++digits == 4)
			{
				bytes.push_back(static_cast<uint8_t>(accumulator >> 16));
				bytes.push_back(static_cast<uint8_t>(accumulator >> 8));
				bytes.push_back(static_cast<uint8_t>(accumulator));
				accumulator = 0;
				digits = 0;
			}
		}

		// A final group of two or three digits carries one or two bytes; a single digit carries none and is invalid, and
		// padding must complete the group if present.
		if (digits == 1 || padding > 2 || (padding > 0 && digits + padding != 4))
			return std::nullopt;
		if (digits == 2)
		{
			bytes.push_back(static_cast<uint8_t>(accumulator >> 4));
		}
		else if (digits == 3)
		{
			bytes.push_back(static_cast<uint8_t>(accumulator >> 10));
			bytes.push_back(static_cast<uint8_t>(accumulator >> 2));
		}
		return bytes;
	}

	bool SaveResultImage(nlohmann::json& result, const std::filesystem::path& path, std::string& error)
	{
		const auto image = result.is_object() ? result.find("Image") : result.end();
		if (image == result.end() || !image->is_object())
		{
			error = "The result contains no image (expected {\"Image\": {\"MimeType\", \"Data\"}})";
			return false;
		}
		const auto mimeType = image->find("MimeType");
		const auto data = image->find("Data");
		if (mimeType == image->end() || !mimeType->is_string() || data == image->end() || !data->is_string())
		{
			error = "The result's image has no MimeType and base64 Data";
			return false;
		}

		const std::optional<std::vector<uint8_t>> bytes = DecodeBase64(data->get_ref<const std::string&>());
		if (!bytes)
		{
			error = "The result's image data is not valid base64";
			return false;
		}

		std::error_code pathError;
		std::filesystem::path absolute = std::filesystem::absolute(path, pathError);
		if (pathError)
			absolute = path;
		absolute = absolute.lexically_normal();
		if (!FileSystem::WriteBytes(absolute, *bytes))
		{
			error = fmt::format("Cannot write the image to '{}'", FileSystem::ToUTF8(absolute));
			return false;
		}

		nlohmann::json saved = nlohmann::json::object();
		saved["MimeType"] = *mimeType;
		saved["File"] = FileSystem::ToUTF8(absolute);
		saved["Size"] = bytes->size();
		(*image) = std::move(saved);
		return true;
	}

}
