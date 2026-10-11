#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

namespace Strata::Tests
{

	// Whether text is an ID as Strata writes them (16 hexadecimal digits).
	inline bool IsIdText(const std::string& text)
	{
		return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char character)
		{
			return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
		});
	}

	// A copy of a document with every ID replaced by "#<n>", n counting the distinct IDs in the order the document first
	// mentions them (objects in key order, arrays in order). Two scenes built by the same steps then compare equal whatever
	// IDs their entities and the assets the steps created got: names, hierarchy (scene documents list entities in hierarchy
	// order), component values and references to entities and assets must all agree.
	inline nlohmann::json NormalizeIds(const nlohmann::json& value, std::unordered_map<std::string, std::string>& mapping)
	{
		if (value.is_string())
		{
			const std::string& text = value.get_ref<const std::string&>();
			if (!IsIdText(text))
				return value;
			std::string key = text;
			std::transform(key.begin(), key.end(), key.begin(), [](char character) { return static_cast<char>(std::toupper(static_cast<unsigned char>(character))); });
			const auto [entry, added] = mapping.try_emplace(key, "#" + std::to_string(mapping.size()));
			return entry->second;
		}
		if (value.is_object())
		{
			nlohmann::json normalized = nlohmann::json::object();
			for (const auto& [key, member] : value.items())
				normalized[key] = NormalizeIds(member, mapping);
			return normalized;
		}
		if (value.is_array())
		{
			nlohmann::json normalized = nlohmann::json::array();
			for (const nlohmann::json& element : value)
				normalized.push_back(NormalizeIds(element, mapping));
			return normalized;
		}
		return value;
	}

	inline nlohmann::json NormalizeIds(const nlohmann::json& value)
	{
		std::unordered_map<std::string, std::string> mapping;
		return NormalizeIds(value, mapping);
	}

	// The first place two documents differ, as a JSON pointer with both values (empty when they are equal).
	inline std::string DescribeFirstDifference(const nlohmann::json& a, const nlohmann::json& b, const std::string& pointer = {})
	{
		if (a == b)
			return {};
		if (a.is_object() && b.is_object())
		{
			for (const auto& [key, member] : a.items())
			{
				if (!b.contains(key))
					return pointer + "/" + key + ": only in the first";
				if (std::string difference = DescribeFirstDifference(member, b[key], pointer + "/" + key); !difference.empty())
					return difference;
			}
			for (const auto& [key, member] : b.items())
			{
				if (!a.contains(key))
					return pointer + "/" + key + ": only in the second";
			}
		}
		if (a.is_array() && b.is_array())
		{
			for (size_t index = 0; index < std::min(a.size(), b.size()); index++)
			{
				if (std::string difference = DescribeFirstDifference(a[index], b[index], pointer + "/" + std::to_string(index)); !difference.empty())
					return difference;
			}
			if (a.size() != b.size())
				return pointer + ": " + std::to_string(a.size()) + " elements against " + std::to_string(b.size());
		}
		return (pointer.empty() ? std::string("/") : pointer) + ": " + a.dump() + " against " + b.dump();
	}

}
