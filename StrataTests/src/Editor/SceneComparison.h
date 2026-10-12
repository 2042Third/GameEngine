#pragma once

#include <Strata/Core/FileSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
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

	inline std::string ToUpperId(const std::string& text)
	{
		std::string key = text;
		std::transform(key.begin(), key.end(), key.begin(), [](char character) { return static_cast<char>(std::toupper(static_cast<unsigned char>(character))); });
		return key;
	}

	// The handles of a project's assets (from the .meta files below its asset directory) as "asset:<path relative to the
	// asset directory>", by handle (upper case). Nullopt when a .meta file cannot be read.
	inline std::optional<std::unordered_map<std::string, std::string>> ReadAssetNames(const std::filesystem::path& assetDirectory)
	{
		std::unordered_map<std::string, std::string> names;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(assetDirectory, error), end; !error && it != end; it.increment(error))
		{
			std::error_code fileError;
			if (!it->is_regular_file(fileError) || FileSystem::ToUTF8(it->path().extension()) != ".meta")
				continue;
			const std::optional<std::string> text = FileSystem::ReadText(it->path());
			const nlohmann::json meta = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json(nlohmann::json::value_t::discarded);
			if (!meta.is_object() || !meta.contains("Handle") || !meta["Handle"].is_string())
				return std::nullopt;
			// The asset is the .meta file's path without its extension.
			std::filesystem::path asset = it->path();
			asset.replace_extension();
			names.emplace(ToUpperId(meta["Handle"].get<std::string>()), "asset:" + FileSystem::ToUTF8(asset.lexically_relative(assetDirectory)));
		}
		if (error)
			return std::nullopt;
		return names;
	}

	// Normalizes the documents of one project for comparisons with another's, such as what a session made and what its replay
	// on a copy of the project made: references to the project's assets become "asset:<path>" (so a reference to the wrong
	// asset differs even where both projects use their assets alike), and the other IDs become "#<n>", n counting the distinct
	// IDs in the order the documents first mention them (objects in key order, arrays in order) across every document it
	// normalizes (so references between documents, such as prefab instances to their prefab's entities, must agree too).
	// Names, hierarchy (scene documents list entities in hierarchy order), component values and references to entities and
	// assets must then all agree, whatever IDs the entities and assets got. Normalize both projects' documents in the same
	// order.
	class ProjectNormalizer
	{
	public:
		// assetNames: ReadAssetNames of the project's asset directory (empty: every ID is numbered).
		explicit ProjectNormalizer(std::unordered_map<std::string, std::string> assetNames = {})
			: m_Mapping(std::move(assetNames)), m_AssetCount(m_Mapping.size())
		{
		}

		nlohmann::json Normalize(const nlohmann::json& value)
		{
			if (value.is_string())
			{
				const std::string& text = value.get_ref<const std::string&>();
				if (!IsIdText(text))
					return value;
				const auto [entry, added] = m_Mapping.try_emplace(ToUpperId(text), "#" + std::to_string(m_Mapping.size() - m_AssetCount));
				return entry->second;
			}
			if (value.is_object())
			{
				nlohmann::json normalized = nlohmann::json::object();
				for (const auto& [key, member] : value.items())
					normalized[key] = Normalize(member);
				return normalized;
			}
			if (value.is_array())
			{
				nlohmann::json normalized = nlohmann::json::array();
				for (const nlohmann::json& element : value)
					normalized.push_back(Normalize(element));
				return normalized;
			}
			return value;
		}
	private:
		std::unordered_map<std::string, std::string> m_Mapping; // Asset names, then the numbers given so far
		size_t m_AssetCount = 0;
	};

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
