#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace Strata
{

	// Assets of the project, referenced by handle. Paths are relative to the project's asset directory with '/'
	// separators ("Prefabs/Bullet.stprefab"). Loading is asynchronous: request assets early (e.g. in OnCreate).
	class Assets
	{
	public:
		// The asset at a path, or a null handle if there is none.
		static AssetHandle Find(std::string_view path)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? AssetHandle(host->FindAsset(Detail::GetContext(), Detail::ToABIString(path))) : AssetHandle();
		}

		static bool IsLoaded(AssetHandle asset)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && asset && host->IsAssetLoaded(Detail::GetContext(), asset.ID);
		}

		// Starts loading the asset in the background and keeps it loaded while the scene plays: the engine does not evict it
		// to stay within its memory budgets until Release (or the end of play). Returns false for unknown assets.
		static bool RequestLoad(AssetHandle asset)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && asset && host->RequestAssetLoad(Detail::GetContext(), asset.ID);
		}

		// Ends the scene's request for the asset (requests do not add up): the engine may unload it again when memory is
		// needed, and loads it again when something uses it. Returns false when the scene holds no request for the asset,
		// and with engines that predate releasing assets (they keep requested assets loaded).
		static bool Release(AssetHandle asset)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(ReleaseAsset);
			return host && asset && host->ReleaseAsset(Detail::GetContext(), asset.ID);
		}
	};

	// The scene the script runs in.
	class Scene
	{
	public:
		// Creates an empty entity (with a Transform) under `parent`, or as a root when the parent is null.
		static Entity CreateEntity(std::string_view name = "Entity", Entity parent = Entity())
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? Entity(host->CreateEntity(Detail::GetContext(), Detail::ToABIString(name), parent.GetID())) : Entity();
		}

		// The entity with this UUID, or a null entity if it does not exist.
		static Entity GetEntity(uint64_t id)
		{
			const Entity entity(id);
			return entity.IsValid() ? entity : Entity();
		}

		// The first entity with this name in hierarchy order.
		static Entity FindEntityByName(std::string_view name)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? Entity(host->FindEntityByName(Detail::GetContext(), Detail::ToABIString(name))) : Entity();
		}

		// Every entity with this tag, in hierarchy order.
		static std::vector<Entity> FindEntitiesByTag(std::string_view tag)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			if (!host)
				return {};
			return ToEntities(Detail::ReadHostIDs([&](uint64_t* buffer, uint32_t capacity)
			{
				return host->FindEntitiesByTag(Detail::GetContext(), Detail::ToABIString(tag), buffer, capacity);
			}));
		}

		static std::vector<Entity> GetRootEntities()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			if (!host)
				return {};
			return ToEntities(Detail::ReadHostIDs([&](uint64_t* buffer, uint32_t capacity)
			{
				return host->GetRootEntities(Detail::GetContext(), buffer, capacity);
			}));
		}

		// The camera the game view renders from (the first active primary camera), or a null entity.
		static Entity GetPrimaryCamera()
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? Entity(host->GetPrimaryCamera(Detail::GetContext())) : Entity();
		}

		// Instantiates a prefab or model under `parent` (or as a root) and returns its root entity. Scripts on the new
		// entities exist right away (GetScript works); their OnCreate runs before their first update.
		// Never waits for loading: if the asset is not loaded yet, this starts loading it and returns a null entity. Request
		// assets early (Assets::RequestLoad, e.g. in OnCreate) and instantiate once Assets::IsLoaded(asset) is true.
		static Entity Instantiate(AssetHandle asset, Entity parent = Entity())
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host ? Entity(host->Instantiate(Detail::GetContext(), asset.ID, parent.GetID(), nullptr)) : Entity();
		}

		// Instantiates with the given local transform for the root (relative to the parent, or the world for roots).
		static Entity Instantiate(AssetHandle asset, const glm::vec3& translation, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
			const glm::vec3& scale = glm::vec3(1.0f), Entity parent = Entity())
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			if (!host)
				return Entity();
			StrataScriptTransform transform = {};
			Detail::ToABIVector3(translation, transform.Translation);
			Detail::ToABIQuat(rotation, transform.Rotation);
			Detail::ToABIVector3(scale, transform.Scale);
			return Entity(host->Instantiate(Detail::GetContext(), asset.ID, parent.GetID(), &transform));
		}

		static Entity Instantiate(std::string_view assetPath, Entity parent = Entity())
		{
			return Instantiate(Assets::Find(assetPath), parent);
		}

		static Entity Instantiate(std::string_view assetPath, const glm::vec3& translation, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
			const glm::vec3& scale = glm::vec3(1.0f), Entity parent = Entity())
		{
			return Instantiate(Assets::Find(assetPath), translation, rotation, scale, parent);
		}
	private:
		static std::vector<Entity> ToEntities(const std::vector<uint64_t>& ids)
		{
			std::vector<Entity> entities;
			entities.reserve(ids.size());
			for (uint64_t id : ids)
				entities.emplace_back(id);
			return entities;
		}
	};

}
