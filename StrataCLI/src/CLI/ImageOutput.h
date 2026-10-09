#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::CLI
{

	// Decodes standard base64 (RFC 4648, "+/" alphabet). Padding is optional and ASCII whitespace is ignored; any other
	// character, or a length that cannot be base64, gives nullopt.
	std::optional<std::vector<uint8_t>> DecodeBase64(std::string_view text);

	// Editor methods return images (e.g. viewport captures) as {"Image": {"MimeType", "Data": <base64>}}. Writes the
	// image of result to path and replaces its Data in result with {"File": <absolute path, UTF-8>, "Size": <bytes>}, so
	// the printed result stays small. Returns false (result unchanged) with the reason in error when result holds no
	// such image, the data is not base64 or the file cannot be written.
	bool SaveResultImage(nlohmann::json& result, const std::filesystem::path& path, std::string& error);

}
