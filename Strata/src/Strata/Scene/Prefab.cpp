#include "stpch.h"
#include "Strata/Scene/Prefab.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Scene/SceneSerializer.h"

namespace Strata
{

	namespace
	{

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

		// Heap memory of a string beyond the string object (short strings live inside it).
		uint64_t GetStringHeapBytes(const std::string& text)
		{
			return text.capacity() > sizeof(std::string) - 1 ? text.capacity() + 1 : 0;
		}

		// About what a parsed document occupies: every value, plus the containers' storage (std::map nodes for objects,
		// vector storage for arrays) and string storage. Walks with an explicit stack: documents can nest deeper than the
		// thread's stack would allow recursion to.
		uint64_t EstimateJsonBytes(const nlohmann::json& document)
		{
			// A red-black tree node: three links and a color, before the key and the value.
			constexpr uint64_t c_MapNodeOverhead = 4 * sizeof(void*);

			uint64_t bytes = sizeof(nlohmann::json);
			std::vector<const nlohmann::json*> pending = { &document };
			while (!pending.empty())
			{
				const nlohmann::json& value = *pending.back();
				pending.pop_back();
				switch (value.type())
				{
					case nlohmann::json::value_t::object:
						bytes += sizeof(nlohmann::json::object_t);
						for (const auto& [key, member] : value.get_ref<const nlohmann::json::object_t&>())
						{
							bytes += c_MapNodeOverhead + sizeof(std::string) + GetStringHeapBytes(key) + sizeof(nlohmann::json);
							pending.push_back(&member);
						}
						break;
					case nlohmann::json::value_t::array:
					{
						const nlohmann::json::array_t& array = value.get_ref<const nlohmann::json::array_t&>();
						bytes += sizeof(nlohmann::json::array_t) + array.capacity() * sizeof(nlohmann::json);
						for (const nlohmann::json& element : array)
							pending.push_back(&element);
						break;
					}
					case nlohmann::json::value_t::string:
						bytes += sizeof(std::string) + GetStringHeapBytes(value.get_ref<const std::string&>());
						break;
					case nlohmann::json::value_t::binary:
						bytes += sizeof(nlohmann::json::binary_t) + value.get_binary().capacity();
						break;
					default:
						break; // Numbers, booleans and null live inside the value
				}
			}
			return bytes;
		}

		// Instantiates the snapshot into a scratch scene, so broken entity data fails at load time rather than when
		// the prefab or model is first used.
		bool ValidateSnapshot(const nlohmann::json& snapshot, std::string_view format, std::string* outError)
		{
			Scene validation;
			std::string error;
			SceneSerializer::DeserializeEntities(validation, snapshot, EntityInstantiationOptions(), &error);
			if (error.empty())
				return true;
			if (outError)
				*outError = fmt::format("Invalid {} entities: {}", format, error);
			return false;
		}

	}

	std::vector<Entity> EntityTemplate::Instantiate(Scene& scene, Entity parent, std::string* outError) const
	{
		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = true;
		options.Parent = parent;
		options.SourcePrefab = Handle;
		std::string error;
		std::vector<std::string> warnings;
		std::vector<Entity> roots = SceneSerializer::DeserializeEntities(scene, m_Snapshot, options, &error, &warnings);
		if (!error.empty())
		{
			ST_CORE_ERROR("Failed to instantiate {}: {}", AssetTypeToString(GetType()), error);
			if (outError)
				*outError = error;
		}
		for (const std::string& warning : warnings)
			ST_CORE_WARN("Instantiating {}: {}", AssetTypeToString(GetType()), warning);
		return roots;
	}

	size_t EntityTemplate::GetEntityCount() const
	{
		const nlohmann::json* entities = JsonUtils::Find(m_Snapshot, "Entities");
		return entities && entities->is_array() ? entities->size() : 0;
	}

	void EntityTemplate::SetSnapshot(nlohmann::json snapshot)
	{
		m_Snapshot = std::move(snapshot);
		m_SnapshotBytes = EstimateJsonBytes(m_Snapshot);
	}

	Ref<Prefab> Prefab::CreateFromEntities(const Scene& scene, const std::vector<Entity>& roots)
	{
		return CreateFromSnapshot(SceneSerializer::SerializeEntities(scene, roots));
	}

	Ref<Prefab> Prefab::CreateFromSnapshot(nlohmann::json snapshot)
	{
		Ref<Prefab> prefab = CreateRef<Prefab>();
		prefab->SetSnapshot(std::move(snapshot));
		return prefab;
	}

	nlohmann::json Prefab::Serialize() const
	{
		return nlohmann::json { { "Strata", { { "Format", "Prefab" }, { "Version", c_FormatVersion } } }, { "Prefab", m_Snapshot } };
	}

	Ref<Prefab> Prefab::FromJson(const nlohmann::json& json, std::string* outError)
	{
		const nlohmann::json* payload = ReadDocument(json, "Prefab", "Prefab", c_FormatVersion, outError);
		if (!payload || !ValidateSnapshot(*payload, "prefab", outError))
			return nullptr;
		return CreateFromSnapshot(*payload);
	}

	Ref<Prefab> Prefab::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = ParseBytes(data, outError);
		return json ? FromJson(*json, outError) : nullptr;
	}

	Ref<Model> Model::CreateFromSnapshot(nlohmann::json snapshot)
	{
		Ref<Model> model = CreateRef<Model>();
		model->SetSnapshot(std::move(snapshot));
		return model;
	}

	nlohmann::json Model::Serialize() const
	{
		return nlohmann::json { { "Strata", { { "Format", "Model" }, { "Version", c_FormatVersion } } }, { "Model", m_Snapshot } };
	}

	Ref<Model> Model::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = ParseBytes(data, outError);
		if (!json)
			return nullptr;
		const nlohmann::json* payload = ReadDocument(*json, "Model", "Model", c_FormatVersion, outError);
		if (!payload || !ValidateSnapshot(*payload, "model", outError))
			return nullptr;
		return CreateFromSnapshot(*payload);
	}

	Ref<SceneAsset> SceneAsset::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = ParseBytes(data, outError);
		if (!json)
			return nullptr;

		// Validate eagerly so a broken scene fails at load time, not when instantiated.
		Scene validation;
		if (!SceneSerializer::Deserialize(validation, *json, outError))
			return nullptr;

		Ref<SceneAsset> asset = CreateRef<SceneAsset>();
		asset->m_Document = std::move(*json);
		asset->m_DocumentBytes = EstimateJsonBytes(asset->m_Document);
		return asset;
	}

	Ref<Scene> SceneAsset::CreateScene(std::string* outError) const
	{
		Ref<Scene> scene = CreateRef<Scene>();
		std::vector<std::string> warnings;
		if (!SceneSerializer::Deserialize(*scene, m_Document, outError, &warnings))
			return nullptr;
		for (const std::string& warning : warnings)
			ST_CORE_WARN("Scene '{}': {}", scene->GetName(), warning);
		return scene;
	}

}
