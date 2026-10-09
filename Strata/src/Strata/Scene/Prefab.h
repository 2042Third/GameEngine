#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// An entity hierarchy that can be instantiated into scenes (snapshot format of SceneSerializer::SerializeEntities).
	// Prefabs are authored assets (".stprefab"); models are produced by importing model files (glTF) and are read-only.
	class EntityTemplate : public Asset
	{
	public:
		// Instantiates the hierarchy with fresh UUIDs under `parent` (scene root when invalid). Returns the top-level
		// entities. Every created entity is linked to this asset through a PrefabInstanceComponent.
		std::vector<Entity> Instantiate(Scene& scene, Entity parent = {}) const;

		const nlohmann::json& GetSnapshot() const { return m_Snapshot; }
		size_t GetEntityCount() const;
	protected:
		nlohmann::json m_Snapshot; // {"Entities": [...]}
	};

	class Prefab : public EntityTemplate
	{
	public:
		static AssetType GetStaticType() { return AssetType::Prefab; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_FormatVersion = 1;

		// Captures entities and their descendants.
		static Ref<Prefab> CreateFromEntities(const Scene& scene, const std::vector<Entity>& roots);
		static Ref<Prefab> CreateFromSnapshot(nlohmann::json snapshot);

		// Document: { "Strata": { "Format": "Prefab", "Version": 1 }, "Prefab": { "Entities": [...] } }
		nlohmann::json Serialize() const;
		static Ref<Prefab> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		static Ref<Prefab> FromJson(const nlohmann::json& json, std::string* outError = nullptr);
	};

	class Model : public EntityTemplate
	{
	public:
		static AssetType GetStaticType() { return AssetType::Model; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_FormatVersion = 1;

		static Ref<Model> CreateFromSnapshot(nlohmann::json snapshot);
		// Cooked document: { "Strata": { "Format": "Model", "Version": 1 }, "Model": { "Entities": [...] } }
		nlohmann::json Serialize() const;
		static Ref<Model> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
	};

	// A scene document loaded as an asset (for the runtime and Scene::Load by handle).
	class SceneAsset : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Scene; }
		AssetType GetType() const override { return GetStaticType(); }

		static Ref<SceneAsset> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		// Builds a new scene from the document.
		Ref<Scene> CreateScene(std::string* outError = nullptr) const;
		const nlohmann::json& GetDocument() const { return m_Document; }
	private:
		nlohmann::json m_Document;
	};

}
