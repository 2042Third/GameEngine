#include "stpch.h"
#include "Strata/Scene/PhysicsLayerSettings.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Strata
{

	namespace
	{

		// "Layer 5" (any case): the name an unnamed layer shows, which other layers cannot take.
		std::optional<uint32_t> ParseDefaultName(std::string_view name)
		{
			constexpr std::string_view c_Prefix = "layer ";
			if (name.size() <= c_Prefix.size() || !StringUtils::EqualsIgnoreCase(name.substr(0, c_Prefix.size()), c_Prefix))
				return std::nullopt;
			uint32_t index = 0;
			for (char character : name.substr(c_Prefix.size()))
			{
				if (character < '0' || character > '9' || index > c_PhysicsLayerCount)
					return std::nullopt;
				index = index * 10 + static_cast<uint32_t>(character - '0');
			}
			return index < c_PhysicsLayerCount ? std::optional<uint32_t>(index) : std::nullopt;
		}

		// A key of "Names": a layer index without leading zeros, so that no layer is named twice ("3" and "03").
		std::optional<uint32_t> ParseNameKey(std::string_view key)
		{
			if (key.empty() || key.size() > 2 || (key.size() == 2 && key[0] == '0'))
				return std::nullopt;
			uint32_t layer = 0;
			for (char character : key)
			{
				if (character < '0' || character > '9')
					return std::nullopt;
				layer = layer * 10 + static_cast<uint32_t>(character - '0');
			}
			return layer < c_PhysicsLayerCount ? std::optional<uint32_t>(layer) : std::nullopt;
		}

		// A layer index of a pair: an integer from 0 to c_PhysicsLayerCount - 1 (parsed files hold unsigned numbers, values built
		// in code signed ones).
		std::optional<uint32_t> ReadLayerIndex(const nlohmann::json& value)
		{
			if (!value.is_number_integer())
				return std::nullopt;
			const bool inRange = value.is_number_unsigned() ? value.get<uint64_t>() < c_PhysicsLayerCount
				: (value.get<int64_t>() >= 0 && value.get<int64_t>() < static_cast<int64_t>(c_PhysicsLayerCount));
			if (!inRange)
				return std::nullopt;
			return static_cast<uint32_t>(value.get<int64_t>());
		}

	}

	bool PhysicsLayerSettings::IsDefault() const
	{
		return IsDefaultMatrix() && std::all_of(Names.begin(), Names.end(), [](const std::string& name) { return name.empty(); });
	}

	std::optional<uint32_t> PhysicsLayerSettings::FindLayer(std::string_view name) const
	{
		if (name.empty())
			return std::nullopt;
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
		{
			if (!Names[layer].empty() && StringUtils::EqualsIgnoreCase(Names[layer], name))
				return layer;
		}
		if (const std::optional<uint32_t> index = ParseDefaultName(name); index && Names[*index].empty())
			return index;
		return std::nullopt;
	}

	std::string PhysicsLayerSettings::GetDisplayName(uint32_t layer) const
	{
		if (layer >= c_PhysicsLayerCount)
			return fmt::format("Layer {}", layer);
		return Names[layer].empty() ? fmt::format("Layer {}", layer) : Names[layer];
	}

	std::optional<std::string> PhysicsLayerSettings::ValidateName(uint32_t layer, std::string_view name) const
	{
		if (layer >= c_PhysicsLayerCount)
			return fmt::format("Layer {} does not exist: layers range from 0 to {}", layer, c_PhysicsLayerCount - 1);
		if (name.empty())
			return std::nullopt;
		if (name.size() > c_MaxNameLength)
			return fmt::format("Layer names have at most {} characters", c_MaxNameLength);
		for (char character : name)
		{
			const unsigned char value = static_cast<unsigned char>(character);
			if (value < 0x20 || value == 0x7F)
				return std::string("Layer names cannot contain control characters");
		}
		if (name.front() == ' ' || name.back() == ' ')
			return std::string("Layer names cannot start or end with a space");
		if (const std::optional<uint32_t> index = ParseDefaultName(name); index && *index != layer)
			return fmt::format("'{}' is the name layer {} shows without a name of its own", name, *index);
		for (uint32_t other = 0; other < c_PhysicsLayerCount; other++)
		{
			if (other != layer && StringUtils::EqualsIgnoreCase(Names[other], name))
				return fmt::format("Layer {} is named '{}' already", other, Names[other]);
		}
		return std::nullopt;
	}

	nlohmann::json PhysicsLayerSettings::ToJson() const
	{
		nlohmann::json names = nlohmann::json::object();
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
		{
			if (!Names[layer].empty())
				names[std::to_string(layer)] = Names[layer];
		}
		nlohmann::json noCollisions = nlohmann::json::array();
		for (uint32_t a = 0; a < c_PhysicsLayerCount; a++)
		{
			for (uint32_t b = a; b < c_PhysicsLayerCount; b++)
			{
				if (!DoCollide(a, b))
					noCollisions.push_back({ a, b });
			}
		}
		return { { "Names", std::move(names) }, { "NoCollisions", std::move(noCollisions) } };
	}

	PhysicsLayerSettings PhysicsLayerSettings::FromJson(const nlohmann::json& json, std::vector<std::string>* outProblems)
	{
		auto report = [outProblems](std::string message)
		{
			if (outProblems)
				outProblems->push_back(std::move(message));
		};
		PhysicsLayerSettings layers;
		if (!json.is_object())
		{
			report("it is not an object of Names and NoCollisions (no layer is named, every layer collides)");
			return layers;
		}
		if (const nlohmann::json* names = JsonUtils::Find(json, "Names"))
		{
			if (!names->is_object())
			{
				report("'Names' is not an object of names by layer index (no layer is named)");
			}
			else
			{
				for (const auto& [key, name] : names->items())
				{
					const std::optional<uint32_t> layer = ParseNameKey(key);
					if (!layer || !name.is_string())
					{
						report(fmt::format("the name '{}' is ignored: names are strings keyed by layer index (0 to {})", key, c_PhysicsLayerCount - 1));
						continue;
					}
					if (std::optional<std::string> problem = layers.ValidateName(*layer, name.get<std::string>()))
					{
						report(fmt::format("the name of layer {} is ignored: {}", *layer, *problem));
						continue;
					}
					layers.Names[*layer] = name.get<std::string>();
				}
			}
		}
		if (const nlohmann::json* pairs = JsonUtils::Find(json, "NoCollisions"))
		{
			if (!pairs->is_array())
			{
				report("'NoCollisions' is not an array of layer pairs (every layer collides)");
				return layers;
			}
			for (const nlohmann::json& pair : *pairs)
			{
				const std::optional<uint32_t> a = pair.is_array() && pair.size() == 2 ? ReadLayerIndex(pair[0]) : std::nullopt;
				const std::optional<uint32_t> b = pair.is_array() && pair.size() == 2 ? ReadLayerIndex(pair[1]) : std::nullopt;
				if (!a || !b)
				{
					report(fmt::format("the pair {} is ignored: pairs are two layer indices (0 to {})", JsonUtils::Dump(pair), c_PhysicsLayerCount - 1));
					continue;
				}
				layers.SetCollide(*a, *b, false);
			}
		}
		return layers;
	}

}
