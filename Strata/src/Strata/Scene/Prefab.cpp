#include "stpch.h"
#include "Strata/Scene/Prefab.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/CookedScene.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/SceneDocumentReader.h"
#include "Strata/Scene/SceneDocumentWriter.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <string_view>
#include <unordered_map>

namespace Strata
{

	namespace
	{

		// The shares of reading a scene document (SceneAsset::Deserialize) its steps take, as measured on a scene of a million
		// entities (Release): parsing the text while the entities are created, then linking them; the rest goes to computing the
		// world transforms.
		constexpr float c_SceneReadStreamShare = 0.80f;
		constexpr float c_SceneReadLinkShare = 0.12f;

		// Validates a {"Strata": {"Format": format, "Version": n}, key: {...}} document and returns the payload.
		const nlohmann::json* ReadDocument(const nlohmann::json& json, std::string_view format, std::string_view key, uint32_t maxVersion, std::string* outError)
		{
			const nlohmann::json* header = JsonUtils::Find(json, "Strata");
			if (!header || JsonUtils::GetString(*header, "Format") != format)
			{
				if (outError)
					*outError = fmt::format("Not a Strata {} document", format);
				return nullptr;
			}
			const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
			if (version == 0 || version > maxVersion)
			{
				if (outError)
					*outError = fmt::format("Unsupported {} version {}", format, version);
				return nullptr;
			}
			const nlohmann::json* payload = JsonUtils::Find(json, key);
			const nlohmann::json* entities = payload ? JsonUtils::Find(*payload, "Entities") : nullptr;
			if (!entities || !entities->is_array())
			{
				if (outError)
					*outError = fmt::format("{} document has no entity list", format);
				return nullptr;
			}
			return payload;
		}

		// A value of an instance's property as its prototype entity would have it: a reference to an entity of the instance
		// names the prototype entity it was made from.
		PropertyValue ToPrototypeValue(PropertyType type, PropertyValue value, const std::unordered_map<UUID, UUID>& instanceToPrototype)
		{
			if (const UUID* entity = std::get_if<UUID>(&value); entity && type == PropertyType::Entity)
			{
				if (auto mapped = instanceToPrototype.find(*entity); mapped != instanceToPrototype.end())
					return PropertyValue(mapped->second);
			}
			return value;
		}


	}

	std::vector<Entity> EntityTemplate::Instantiate(Scene& scene, Entity parent, std::string* outError) const
	{
		LogWarnings();
		InstantiationOptions options;
		options.Parent = parent;
		options.SourcePrefab = Handle;
		std::string error;
		std::vector<Entity> roots;
		if (m_Prototype)
			roots = scene.Instantiate(m_Prototype, options, &error);
		else
			error = "The template has no entities";
		if (!error.empty())
		{
			ST_CORE_ERROR("Failed to instantiate {}: {}", AssetTypeToString(GetType()), error);
			if (outError)
				*outError = error;
		}
		return roots;
	}

	std::vector<Entity> EntityTemplate::InstantiateMany(Scene& scene, std::span<const glm::mat4> transforms, Entity parent, std::string* outError) const
	{
		ST_PROFILE_FUNCTION();
		auto fail = [&](std::string message)
		{
			ST_CORE_ERROR("Failed to instantiate {}: {}", AssetTypeToString(GetType()), message);
			if (outError)
				*outError = std::move(message);
			return std::vector<Entity>();
		};

		LogWarnings();
		if (!m_Prototype || m_Prototype->GetTopLevelIndices().empty())
			return fail("The template has no entities");
		if (parent && parent.GetScene() != &scene)
			return fail("The parent entity belongs to a different scene");
		// Everything that can fail is checked before the first instance exists.
		const size_t total = m_Prototype->GetEntityCount() * transforms.size();
		const size_t existing = std::min(scene.GetRegistryEntityCount(), Scene::c_MaxEntities);
		if (transforms.size() > Scene::c_MaxEntities || total > Scene::c_MaxEntities - existing)
		{
			return fail(fmt::format("Cannot add {} instances of {} entities to a scene with {}: a scene holds at most {} entities", transforms.size(),
				m_Prototype->GetEntityCount(), existing, Scene::c_MaxEntities));
		}
		const size_t firstRoot = m_Prototype->GetTopLevelIndices().front();
		const TransformComponent* prototypeTransform = m_Prototype->FindComponent<TransformComponent>(firstRoot);
		std::vector<TransformComponent> rootTransforms(transforms.size(), prototypeTransform ? *prototypeTransform : TransformComponent());
		for (size_t index = 0; index < transforms.size(); index++)
		{
			if (!rootTransforms[index].SetTransform(transforms[index]))
				return fail(fmt::format("Transform {} cannot be represented as a translation, rotation and scale", index));
		}

		InstantiationOptions options;
		options.Parent = parent;
		options.SourcePrefab = Handle;
		std::vector<Entity> instances;
		instances.reserve(transforms.size());
		// One instantiation object for all instances, so that its tables are allocated once.
		PrototypeInstantiation instantiation;
		for (size_t index = 0; index < transforms.size(); index++)
		{
			std::string error;
			if (!scene.BeginInstantiation(m_Prototype, options, instantiation, &error)
				|| scene.ContinueInstantiation(instantiation, m_Prototype->GetEntityCount(), &error, &rootTransforms[index]) == 0)
			{
				// Checked above; reaching this is a programmer error, and what was created stays (each instance is complete).
				return fail(fmt::format("Instance {} could not be created: {}", index, error));
			}
			instances.push_back(instantiation.GetRoots().front());
		}
		return instances;
	}

	nlohmann::json EntityTemplate::GetSnapshot() const
	{
		return m_Prototype ? m_Prototype->Serialize() : nlohmann::json { { "Entities", nlohmann::json::array() } };
	}

	std::vector<PrefabInstanceEntity> EntityTemplate::CompareInstance(const Scene& scene, Entity root) const
	{
		std::vector<PrefabInstanceEntity> entities;
		if (!m_Prototype || !root || root.GetScene() != &scene)
			return entities;

		// The instance's entities made from this template, parents first, and what each prototype ID became.
		std::unordered_map<UUID, UUID> instanceToPrototype;
		std::vector<Entity> stack = { root };
		while (!stack.empty())
		{
			const Entity entity = stack.back();
			stack.pop_back();
			if (const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>(); link && link->Prefab == Handle)
			{
				PrefabInstanceEntity& instance = entities.emplace_back();
				instance.Instance = entity;
				instance.PrototypeIndex = m_Prototype->FindEntity(link->PrefabEntityID);
				instanceToPrototype.emplace(entity.GetUUID(), link->PrefabEntityID);
			}
			const std::vector<Entity> children = entity.GetChildren();
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}

		const entt::registry& instanceRegistry = scene.GetRegistry();
		const entt::id_type linkType = entt::type_id<PrefabInstanceComponent>().hash();
		// Entity references of the instance named as the prototype's entities.
		const ComponentExtraReferences::Remap toPrototype = [&instanceToPrototype](UUID entity)
		{
			auto mapped = instanceToPrototype.find(entity);
			return mapped != instanceToPrototype.end() ? mapped->second : entity;
		};
		for (PrefabInstanceEntity& instance : entities)
		{
			if (instance.PrototypeIndex == EntityPrototype::c_NotFound)
				continue;
			const entt::entity instanceHandle = instance.Instance.GetHandle();
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize) || info->TypeId == linkType)
					continue;
				const bool inInstance = info->Has(instanceRegistry, instanceHandle);
				const void* b = m_Prototype->FindComponent(instance.PrototypeIndex, *info);
				const bool inPrototype = b != nullptr;
				if (!inInstance && !inPrototype)
					continue;
				bool differs = inInstance != inPrototype;
				if (!differs && info->Size > 0)
				{
					const void* a = instanceRegistry.storage(info->TypeId)->value(instanceHandle);
					for (const PropertyInfo& property : info->Properties)
					{
						if (!property.IsTransient() && ToPrototypeValue(property.Type, property.GetValue(a), instanceToPrototype) != property.GetValue(b))
						{
							differs = true;
							break;
						}
					}
					// What the properties do not express (script fields), compared by the component's own rule.
					if (!differs && info->ExtraReferences.Equals)
						differs = !info->ExtraReferences.Equals(a, b, toPrototype);
				}
				if (differs)
					instance.Differences.push_back(info);
			}
		}
		return entities;
	}

	bool EntityTemplate::Compile(const nlohmann::json& snapshot, std::string* outError)
	{
		std::string error;
		m_Warnings.clear();
		m_Prototype = EntityPrototype::FromSnapshot(snapshot, &error, &m_Warnings);
		if (m_Prototype)
			return true;
		if (outError)
			*outError = fmt::format("Invalid {} entities: {}", GetType() == AssetType::Model ? "model" : "prefab", error);
		return false;
	}

	bool EntityTemplate::Read(std::span<const uint8_t> data, const std::function<bool()>& isCancelled, std::string* outError)
	{
		const bool model = GetType() == AssetType::Model;
		m_Warnings.clear();
		// Exports cook documents into packs: those are read without parsing JSON.
		if (CookedScene::IsCooked(data))
		{
			CookedSceneReadOptions options;
			options.IsCancelled = isCancelled;
			std::optional<CookedSceneContents> contents = CookedSceneReader::Read(data, options, outError);
			if (!contents)
				return false;
			if (contents->Kind != (model ? SceneDocumentKind::Model : SceneDocumentKind::Prefab))
			{
				if (outError)
					*outError = fmt::format("The cooked document is a {}, not a {}", SceneDocumentKindToString(contents->Kind), model ? "model" : "prefab");
				return false;
			}
			m_Prototype = std::move(contents->Prototype);
			m_Warnings = std::move(contents->Warnings);
			return true;
		}
		EntityPrototypeBuilder builder(&m_Warnings);
		SceneReadOptions options;
		options.IsCancelled = isCancelled;
		SceneReadResult result;
		std::string error;
		if (!SceneDocumentReader::Read(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), model ? SceneDocumentKind::Model : SceneDocumentKind::Prefab,
			builder, options, &result, &error))
		{
			if (outError)
				*outError = result.EntityError ? fmt::format("Invalid {} entities: {}", model ? "model" : "prefab", error) : std::move(error);
			return false;
		}
		m_Prototype = builder.TakePrototype();
		return true;
	}

	void EntityTemplate::LogWarnings() const
	{
		if (m_WarningsLogged)
			return;
		m_WarningsLogged = true;
		for (const std::string& warning : m_Warnings)
			ST_CORE_WARN("Instantiating {}: {}", AssetTypeToString(GetType()), warning);
	}

	Ref<Prefab> Prefab::CreateFromEntities(const Scene& scene, const std::vector<Entity>& roots)
	{
		return CreateFromSnapshot(SceneSerializer::SerializeEntities(scene, roots));
	}

	Ref<Prefab> Prefab::CreateFromSnapshot(const nlohmann::json& snapshot, std::string* outError)
	{
		Ref<Prefab> prefab = CreateRef<Prefab>();
		return prefab->Compile(snapshot, outError) ? prefab : nullptr;
	}

	nlohmann::json Prefab::Serialize() const
	{
		return nlohmann::json { { "Strata", { { "Format", "Prefab" }, { "Version", c_FormatVersion } } }, { "Prefab", GetSnapshot() } };
	}

	Ref<Prefab> Prefab::FromJson(const nlohmann::json& json, std::string* outError)
	{
		const nlohmann::json* payload = ReadDocument(json, "Prefab", "Prefab", c_FormatVersion, outError);
		return payload ? CreateFromSnapshot(*payload, outError) : nullptr;
	}

	bool Prefab::WriteDocument(DocumentOutput& output, std::string* outError) const
	{
		if (!GetPrototype())
		{
			if (outError)
				*outError = "The prefab has no entities to write";
			return false;
		}
		SceneWriteOptions options;
		options.Kind = SceneDocumentKind::Prefab;
		if (const ComponentInfo* link = ComponentRegistry::Find<PrefabInstanceComponent>())
			options.LeaveOut.push_back(link);
		return SceneDocumentWriter::WritePrototype(*GetPrototype(), output, options, nullptr, outError);
	}

	Ref<Prefab> Prefab::Deserialize(std::span<const uint8_t> data, std::string* outError, const std::function<bool()>& isCancelled)
	{
		Ref<Prefab> prefab = CreateRef<Prefab>();
		return prefab->Read(data, isCancelled, outError) ? prefab : nullptr;
	}

	Ref<Model> Model::CreateFromSnapshot(const nlohmann::json& snapshot, std::string* outError)
	{
		Ref<Model> model = CreateRef<Model>();
		return model->Compile(snapshot, outError) ? model : nullptr;
	}

	nlohmann::json Model::Serialize() const
	{
		return nlohmann::json { { "Strata", { { "Format", "Model" }, { "Version", c_FormatVersion } } }, { "Model", GetSnapshot() } };
	}

	Ref<Model> Model::Deserialize(std::span<const uint8_t> data, std::string* outError, const std::function<bool()>& isCancelled)
	{
		Ref<Model> model = CreateRef<Model>();
		return model->Read(data, isCancelled, outError) ? model : nullptr;
	}

	Ref<SceneAsset> SceneAsset::Deserialize(std::span<const uint8_t> data, std::string* outError, const std::function<void(float share)>& progress,
		const std::function<bool()>& isCancelled)
	{
		Ref<SceneAsset> asset = CreateRef<SceneAsset>();
		{
			// The document is read as a stream into the scene the asset keeps, which validates it too (a broken scene fails here,
			// at load time): the text and one entity's DOM at a time, never a DOM of the whole document.
			SceneReadOptions options;
			options.IsCancelled = isCancelled;
			EntityReadProgress linkProgress;
			if (progress)
			{
				options.Progress = [&progress](float share) { progress(c_SceneReadStreamShare * share); };
				linkProgress = [&progress](float share) { progress(c_SceneReadStreamShare + c_SceneReadLinkShare * share); };
			}
			if (CookedScene::IsCooked(data))
			{
				// Exports cook scenes into packs: those are read without parsing JSON.
				CookedSceneReadOptions cookedOptions;
				cookedOptions.IsCancelled = isCancelled;
				if (progress)
					cookedOptions.Progress = [&progress](float share) { progress((c_SceneReadStreamShare + c_SceneReadLinkShare) * share); };
				asset->m_Prepared = CookedSceneReader::ReadScene(data, cookedOptions, &asset->m_Warnings, outError);
				if (!asset->m_Prepared)
					return nullptr;
			}
			else
			{
				Ref<Scene> scene = CreateRef<Scene>();
				if (!SceneSerializer::DeserializeText(*scene, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), options, outError,
					&asset->m_Warnings, linkProgress))
					return nullptr;
				asset->m_Prepared = std::move(scene);
			}
		}
		// The world transforms too are computed here rather than in the frame that starts the scene.
		asset->m_Prepared->UpdateWorldTransforms();
		asset->m_PreparedBytes = asset->m_Prepared->EstimateMemoryUsage();
		if (progress)
			progress(1.0f);
		return asset;
	}

	void SceneAsset::LogWarnings(const Scene& scene) const
	{
		if (m_WarningsLogged)
			return;
		m_WarningsLogged = true;
		for (const std::string& warning : m_Warnings)
			ST_CORE_WARN("Scene '{}': {}", scene.GetName(), warning);
	}

	Ref<Scene> SceneAsset::TakePreparedScene(std::string* outError)
	{
		if (!m_Prepared)
		{
			if (outError)
				*outError = fmt::format("The scene of asset {} was taken already; load the asset again", Handle.ToString());
			return nullptr;
		}
		LogWarnings(*m_Prepared);
		Ref<Scene> scene = std::move(m_Prepared);
		scene->SetSourceAsset(Handle);
		// The project's layers replace the document's here, on the main thread: the loader's worker never reads the project.
		scene->ApplyProjectLayers();
		return scene;
	}

	Ref<Scene> SceneAsset::CreateScene(std::string* outError) const
	{
		if (!m_Prepared)
		{
			if (outError)
				*outError = fmt::format("The scene of asset {} was taken already; load the asset again", Handle.ToString());
			return nullptr;
		}
		LogWarnings(*m_Prepared);
		Ref<Scene> scene = Scene::Copy(m_Prepared);
		scene->SetSourceAsset(Handle);
		scene->ApplyProjectLayers();
		return scene;
	}

}
