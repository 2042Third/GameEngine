#include "stpch.h"
#include "Strata/Core/JsonUtils.h"

#include "Strata/Core/JsonEvents.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace Strata
{

	namespace
	{

		std::optional<nlohmann::json> Discarded(std::string* outError)
		{
			if (outError)
				*outError = "Invalid JSON";
			return std::nullopt;
		}

	}

	std::optional<nlohmann::json> JsonUtils::Parse(std::string_view text, std::string* outError)
	{
		nlohmann::json json = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
		if (json.is_discarded())
			return Discarded(outError);
		return json;
	}

	std::optional<nlohmann::json> JsonUtils::Parse(std::string_view text, const JsonParseProgress& progress, std::string* outError)
	{
		if (!progress.Callback)
			return Parse(text, outError);

		// nlohmann's parser over an input of Strata's own (its public parse functions take only nlohmann's inputs), as
		// nlohmann::json::parse runs it: strict (the text holds one value and nothing after it), without exceptions.
		nlohmann::json json;
		nlohmann::detail::parser<nlohmann::json, JsonProgressInput>(JsonProgressInput(text, &progress), nullptr, false, false).parse(true, json);
		if (json.is_discarded())
			return Discarded(outError);
		return json;
	}

	std::string JsonUtils::Dump(const nlohmann::json& json, int indent, char indentCharacter)
	{
		return json.dump(indent, indentCharacter, false, nlohmann::json::error_handler_t::replace);
	}

	void JsonUtils::AppendString(std::string& out, std::string_view text)
	{
		// Printable ASCII but the quote and the backslash is written as it is; anything else (escapes, UTF-8, invalid bytes)
		// goes through the serializer, so the rules stay nlohmann's.
		const bool plain = std::all_of(text.begin(), text.end(), [](char character)
		{
			const unsigned char byte = static_cast<unsigned char>(character);
			return byte >= 0x20 && byte < 0x7F && byte != '"' && byte != '\\';
		});
		if (!plain)
		{
			out += Dump(nlohmann::json(std::string(text)));
			return;
		}
		out += '"';
		out.append(text);
		out += '"';
	}

	const nlohmann::json* JsonUtils::Find(const nlohmann::json& object, std::string_view key)
	{
		if (!object.is_object())
			return nullptr;
		auto it = object.find(std::string(key));
		return it != object.end() ? &*it : nullptr;
	}

	std::string JsonUtils::GetString(const nlohmann::json& object, std::string_view key, std::string fallback)
	{
		const nlohmann::json* value = Find(object, key);
		return value && value->is_string() ? value->get<std::string>() : fallback;
	}

	bool JsonUtils::GetBool(const nlohmann::json& object, std::string_view key, bool fallback)
	{
		const nlohmann::json* value = Find(object, key);
		return value && value->is_boolean() ? value->get<bool>() : fallback;
	}

	int64_t JsonUtils::GetInt(const nlohmann::json& object, std::string_view key, int64_t fallback)
	{
		const nlohmann::json* value = Find(object, key);
		if (!value)
			return fallback;
		if (value->is_number_integer())
			return value->get<int64_t>();
		if (value->is_number_unsigned())
		{
			const uint64_t unsignedValue = value->get<uint64_t>();
			return unsignedValue <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? static_cast<int64_t>(unsignedValue) : fallback;
		}
		return fallback;
	}

	uint64_t JsonUtils::GetUInt(const nlohmann::json& object, std::string_view key, uint64_t fallback)
	{
		const nlohmann::json* value = Find(object, key);
		if (!value)
			return fallback;
		if (value->is_number_unsigned())
			return value->get<uint64_t>();
		if (value->is_number_integer())
		{
			const int64_t signedValue = value->get<int64_t>();
			return signedValue >= 0 ? static_cast<uint64_t>(signedValue) : fallback;
		}
		return fallback;
	}

	float JsonUtils::GetFloat(const nlohmann::json& object, std::string_view key, float fallback)
	{
		const nlohmann::json* value = Find(object, key);
		if (!value || !value->is_number())
			return fallback;
		const double number = value->get<double>();
		if (!std::isfinite(number) || std::abs(number) > static_cast<double>(std::numeric_limits<float>::max()))
			return fallback;
		return static_cast<float>(number);
	}

}
