#include "stpch.h"
#include "Strata/Scene/Prefab.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/Components.h"
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

		std::optional<nlohmann::json> ParseBytes(std::span<const uint8_t> data, std::string* outError)
		{
			return JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), outError);
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

		// Whether two script components hold the same scripts with the same field overrides (entity fields of the instance
		// mapped to the prototype's entities).
		bool ScriptsMatch(const ScriptComponent& instance, const ScriptComponent& prototype, const std::unordered_map<UUID, UUID>& instanceToPrototype)
		{
			if (instance.Scripts.size() != prototype.Scripts.size())
				return false;
			for (size_t script = 0; script < instance.Scripts.size(); script++)
			{
				const ScriptEntry& a = instance.Scripts[script];
				const ScriptEntry& b = prototype.Scripts[script];
				if (a.ClassName != b.ClassName || a.Fields.size() != b.Fields.size())
					return false;
				for (size_t field = 0; field < a.Fields.size(); field++)
				{
					const ScriptFieldValue& x = a.Fields[field];
					const ScriptFieldValue& y = b.Fields[field];
					if (x.Name != y.Name || x.Type != y.Type || ToPrototypeValue(x.Type, x.Value, instanceToPrototype) != y.Value)
						return false;
				}
			}
			return true;
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
		const entt::id_type scriptType = entt::type_id<ScriptComponent>().hash();
		const entt::id_type linkType = entt::type_id<PrefabInstanceComponent>().hash();
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
					if (!differs && info->TypeId == scriptType)
						differs = !ScriptsMatch(*static_cast<const ScriptComponent*>(a), *static_cast<const ScriptComponent*>(b), instanceToPrototype);
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

	Ref<Prefab> Prefab::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = ParseBytes(data, outError);
		return json ? FromJson(*json, outError) : nullptr;
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

	Ref<Model> Model::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = ParseBytes(data, outError);
		if (!json)
			return nullptr;
		const nlohmann::json* payload = ReadDocument(*json, "Model", "Model", c_FormatVersion, outError);
		return payload ? CreateFromSnapshot(*payload, outError) : nullptr;
	}

	Ref<SceneAsset> SceneAsset::Deserialize(std::span<const uint8_t> data, std::string* outError, const std::function<void(float share)>& progress)
	{
		Ref<SceneAsset> asset = CreateRef<SceneAsset>();
		{
			// The document is read as a stream into the scene the asset keeps, which validates it too (a broken scene fails here,
			// at load time): the text and one entity's DOM at a time, never a DOM of the whole document.
			SceneReadOptions options;
			EntityReadProgress linkProgress;
			if (progress)
			{
				options.Progress = [&progress](float share) { progress(c_SceneReadStreamShare * share); };
				linkProgress = [&progress](float share) { progress(c_SceneReadStreamShare + c_SceneReadLinkShare * share); };
			}
			Ref<Scene> scene = CreateRef<Scene>();
			if (!SceneSerializer::DeserializeText(*scene, std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), options, outError,
				&asset->m_Warnings, linkProgress))
				return nullptr;
			asset->m_Prepared = std::move(scene);
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
		return scene;
	}

}
