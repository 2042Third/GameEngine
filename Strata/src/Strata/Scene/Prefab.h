#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/EntityPrototype.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// One entity of a prefab or model instance (EntityTemplate::CompareInstance): the instance's entity, the prototype entity
	// it was made from, and how it differs from it.
	struct PrefabInstanceEntity
	{
		Entity Instance;
		// The entity's index in the template's prototype (EntityPrototype); c_NotFound when the template no longer has it.
		size_t PrototypeIndex = EntityPrototype::c_NotFound;
		// The registered components whose values differ from the prototype entity's, or that only one of the two has, in
		// registration order. References to entities of the instance count as equal to references to the prototype entities
		// they were made from. The prefab link itself (PrefabInstanceComponent) is not compared.
		std::vector<const ComponentInfo*> Differences;
	};

	// An entity hierarchy that can be instantiated into scenes (snapshot format of SceneSerializer::SerializeEntities).
	// Prefabs are authored assets (".stprefab"); models are produced by importing model files (glTF) and are read-only.
	//
	// The snapshot is compiled once, when the asset is created or loaded (on the loading worker), into a prototype
	// (EntityPrototype): its entities with real components, which instantiation copies (ComponentInfo::Copy) instead of reading
	// JSON again. The JSON is not kept; GetSnapshot and Serialize write it from the prototype.
	class EntityTemplate : public Asset
	{
	public:
		// Instantiates the hierarchy with fresh UUIDs under `parent` (scene root when invalid). Returns the top-level
		// entities. Every created entity is linked to this asset through a PrefabInstanceComponent. Instantiation fails as a
		// whole, creating nothing, when the scene cannot hold its entities: the error is logged and stored in outError. Main
		// thread; what compiling the snapshot could not read as written (GetWarnings) is logged by the first instantiation.
		std::vector<Entity> Instantiate(Scene& scene, Entity parent = {}, std::string* outError = nullptr) const;
		// One instance per transform, each made as Instantiate makes it, with the transform as the local transform of the
		// instance's first top-level entity (relative to `parent`, or to the world for roots). Returns that entity of every
		// instance, in the order of the transforms. Fails as a whole, creating nothing, when a transform cannot be represented
		// as a translation, rotation and scale (TransformComponent::SetTransform) or the scene cannot hold every instance.
		std::vector<Entity> InstantiateMany(Scene& scene, std::span<const glm::mat4> transforms, Entity parent = {}, std::string* outError = nullptr) const;

		// The compiled entities, which instances are copied from (null only for a default-constructed template).
		const Ref<EntityPrototype>& GetPrototype() const { return m_Prototype; }
		// The entities as a snapshot ({"Entities": [...]}), written from the prototype.
		nlohmann::json GetSnapshot() const;
		size_t GetEntityCount() const { return m_Prototype ? m_Prototype->GetEntityCount() : 0; }
		// What compiling the snapshot could not read as written (unknown properties, components this build does not
		// register), in the order it was found.
		const std::vector<std::string>& GetWarnings() const { return m_Warnings; }

		// The entities of the instance at `root` (its subtree, root included) that were made from this template (their
		// PrefabInstanceComponent names this asset), in hierarchy order, each with its prototype entity and the components
		// that differ from it: what applying an instance to its template, or reverting it, works from.
		std::vector<PrefabInstanceEntity> CompareInstance(const Scene& scene, Entity root) const;

		// The prototype (EntityPrototype::GetMemoryUsage).
		AssetMemoryUsage GetMemoryUsage() const override { return AssetMemoryUsage { m_Prototype ? m_Prototype->GetMemoryUsage() : 0 }; }
	protected:
		// Compiles the snapshot into the prototype (EntityPrototype::FromSnapshot). False, with the error, when its entity
		// data is invalid.
		bool Compile(const nlohmann::json& snapshot, std::string* outError);
	private:
		void LogWarnings() const;
	private:
		Ref<EntityPrototype> m_Prototype;
		std::vector<std::string> m_Warnings;
		mutable bool m_WarningsLogged = false;
	};

	class Prefab : public EntityTemplate
	{
	public:
		static AssetType GetStaticType() { return AssetType::Prefab; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_FormatVersion = 1;

		// Captures entities and their descendants.
		static Ref<Prefab> CreateFromEntities(const Scene& scene, const std::vector<Entity>& roots);
		// Compiles a snapshot ({"Entities": [...]}); null, with the error, when its entity data is invalid.
		static Ref<Prefab> CreateFromSnapshot(const nlohmann::json& snapshot, std::string* outError = nullptr);

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

		// Compiles a snapshot ({"Entities": [...]}); null, with the error, when its entity data is invalid.
		static Ref<Model> CreateFromSnapshot(const nlohmann::json& snapshot, std::string* outError = nullptr);
		// Cooked document: { "Strata": { "Format": "Model", "Version": 1 }, "Model": { "Entities": [...] } }
		nlohmann::json Serialize() const;
		static Ref<Model> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
	};

	// A scene document loaded as an asset (SceneLoader, the editor's scenes, exported games). The loader reads the document
	// into a scene on the loading worker, which validates it, and keeps that prepared scene instead of the document (the
	// parsed JSON is dropped once read): a scene is deserialized once per load, and a loaded scene asset holds the scene, not
	// its text as well.
	class SceneAsset : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Scene; }
		AssetType GetType() const override { return GetStaticType(); }

		// Reads the document into the prepared scene, as a stream (SceneSerializer::DeserializeText). `progress` (when given)
		// receives the share of the read done so far, in [0, 1], as it goes: about every megabyte parsed and every few thousand
		// entities linked, and 1 at the end (the asset loader passes it on to SceneReadProgress, which scene loads show).
		// `isCancelled` (when given) is asked every few hundred entities: once it is true the read stops and fails
		// (AssetLoadData::IsCancelled, so that a cancelled load of a large scene ends at once).
		static Ref<SceneAsset> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr, const std::function<void(float share)>& progress = {},
			const std::function<bool()>& isCancelled = {});

		// Main thread. Hands the prepared scene over without copying it; its source asset (Scene::GetSourceAsset) is this
		// asset. The first call takes it: the asset holds no scene afterwards (IsTaken; it reports no memory), and a later call
		// fails, so whoever takes it unloads the asset (AssetManagerBase::UnloadAsset, as SceneLoader does) and a later load
		// reads the document again. A copy kept for later callers would double a large scene's memory; one made from the
		// taken scene would hold whatever the game did to it meanwhile. What the loader could not read as written (unknown
		// properties, components this build does not register) is logged as warnings once, by the first call of this or of
		// CreateScene.
		Ref<Scene> TakePreparedScene(std::string* outError = nullptr);
		// A new scene: a copy (Scene::Copy) of the prepared scene, which the asset keeps, for tools and tests that make
		// several scenes of one asset. Fails once the scene was taken.
		Ref<Scene> CreateScene(std::string* outError = nullptr) const;
		bool IsTaken() const { return m_Prepared == nullptr; }
		// What the loader could not read as written, in the order it found it.
		const std::vector<std::string>& GetWarnings() const { return m_Warnings; }

		// The prepared scene (Scene::EstimateMemoryUsage); nothing once it was taken.
		AssetMemoryUsage GetMemoryUsage() const override { return AssetMemoryUsage { m_Prepared ? m_PreparedBytes : 0 }; }
	private:
		void LogWarnings(const Scene& scene) const;
	private:
		Ref<Scene> m_Prepared;
		uint64_t m_PreparedBytes = 0;
		std::vector<std::string> m_Warnings;
		mutable bool m_WarningsLogged = false;
	};

}
