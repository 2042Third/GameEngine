#pragma once

#include "Strata/Core/Base.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Exception-free helpers for reading untrusted JSON (files, automation requests). nlohmann::json throws on
	// type mismatches (e.g. value() of a key holding the wrong type) and on invalid UTF-8 when dumping; engine code
	// uses these helpers instead so malformed input produces errors, never crashes.
	class JsonUtils
	{
	public:
		static std::optional<nlohmann::json> Parse(std::string_view text, std::string* outError = nullptr);
		// Serializes with invalid UTF-8 replaced by U+FFFD. indent < 0 produces compact single-line output.
		static std::string Dump(const nlohmann::json& json, int indent = -1, char indentCharacter = ' ');

		// Typed member access: return the fallback when the key is missing or holds a different type.
		static std::string GetString(const nlohmann::json& object, std::string_view key, std::string fallback = {});
		static bool GetBool(const nlohmann::json& object, std::string_view key, bool fallback);
		static int64_t GetInt(const nlohmann::json& object, std::string_view key, int64_t fallback);
		static uint64_t GetUInt(const nlohmann::json& object, std::string_view key, uint64_t fallback);
		static float GetFloat(const nlohmann::json& object, std::string_view key, float fallback);
		// Pointer to the member, or null when missing (or when `object` is not an object).
		static const nlohmann::json* Find(const nlohmann::json& object, std::string_view key);
	};

}
