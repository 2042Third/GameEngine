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

		// Starts loading the asset in the background. Returns false for unknown assets.
		static bool RequestLoad(AssetHandle asset)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && asset && host->RequestAssetLoad(Detail::GetContext(), asset.ID);
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
