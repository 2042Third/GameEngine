#include "stpch.h"
#include "Strata/Scene/PhysicsLayerSettings.h"

#include "Strata/Core/StringUtils.h"

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

}
