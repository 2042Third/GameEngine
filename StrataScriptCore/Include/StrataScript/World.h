#pragma once

#include "StrataScript/Host.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace Strata
{

	// What World::RequestCellsAround asked for: the cells around a point stream, before every source's, until the request
	// is released or the scene stops. An invalid request (ID 0) is one the engine refused.
	class WorldCellRequest
	{
	public:
		WorldCellRequest() = default;
		explicit WorldCellRequest(uint64_t id)
			: m_ID(id)
		{
		}

		uint64_t GetID() const { return m_ID; }
		bool IsValid() const { return m_ID != 0; }
		// Lets the cells go: they unload once no source wants them. False for a request the engine does not hold (refused,
		// released already, or made in a scene that stopped).
		bool Release() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WorldReleaseCells);
			return host && m_ID != 0 && host->WorldReleaseCells(Detail::GetContext(), m_ID);
		}
	private:
		uint64_t m_ID = 0;
	};

	// The streaming of the world the playing scene is the persistent scene of (its World component): its cells load and
	// unload around the streaming sources as they move. Prefetch before a teleport or a spawn: request the cells around
	// the destination, wait until IsLoadedAround says they are in (the ground there has collision then), move, and release
	// the request. In a scene that streams no world every call fails (the engine reports it once).
	class World
	{
	public:
		// Streams the cells within `radius` of `position` on every enabled layer (radius 0: the cells at the position).
		static WorldCellRequest RequestCellsAround(const glm::vec3& position, float radius)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WorldRequestCellsAround);
			const float values[3] = { position.x, position.y, position.z };
			return WorldCellRequest(host ? host->WorldRequestCellsAround(Detail::GetContext(), values, radius) : 0);
		}

		// Whether every existing cell of every enabled layer within `radius` of `position` is loaded.
		static bool IsLoadedAround(const glm::vec3& position, float radius)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WorldIsLoadedAround);
			const float values[3] = { position.x, position.y, position.z };
			return host && host->WorldIsLoadedAround(Detail::GetContext(), values, radius);
		}

		// Turns a layer of the world on or off by its name: a disabled layer's cells unload, and it streams again once
		// enabled. Layers start as the world defines them (EnabledByDefault). False for a name the world does not have.
		static bool SetLayerEnabled(std::string_view layer, bool enabled)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WorldSetLayerEnabled);
			return host && host->WorldSetLayerEnabled(Detail::GetContext(), Detail::ToABIString(layer), enabled);
		}

		// Whether a layer streams; nullopt for a name the world does not have (or a scene that streams no world).
		static std::optional<bool> IsLayerEnabled(std::string_view layer)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(WorldIsLayerEnabled);
			const int32_t enabled = host ? host->WorldIsLayerEnabled(Detail::GetContext(), Detail::ToABIString(layer)) : -1;
			if (enabled < 0)
				return std::nullopt;
			return enabled != 0;
		}
	};

}
