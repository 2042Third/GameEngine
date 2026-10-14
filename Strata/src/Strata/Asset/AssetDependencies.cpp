#include "stpch.h"
#include "Strata/Asset/AssetDependencies.h"

#include "Strata/Animation/AnimationController.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/EntitySink.h"
#include "Strata/Scene/SceneDocumentReader.h"

#include <optional>
#include <string_view>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// Appends handles once each, in the order they first appear.
		class ReferenceList
		{
		public:
			explicit ReferenceList(std::vector<AssetHandle>& references)
				: m_References(references), m_Seen(references.begin(), references.end())
			{
			}

			void Add(AssetHandle handle)
			{
				if (handle.IsValid() && m_Seen.insert(handle).second)
					m_References.push_back(handle);
			}
		private:
			std::vector<AssetHandle>& m_References;
			std::unordered_set<AssetHandle> m_Seen;
		};

		// The asset properties of a reflected object's JSON (a component, a material).
		void ScanProperties(const std::vector<PropertyInfo>& properties, const nlohmann::json& object, ReferenceList& references)
		{
			for (const PropertyInfo& property : properties)
			{
				if (property.Type != PropertyType::Asset || HasFlag(property.Flags, PropertyFlags::Transient))
					continue;
				const nlohmann::json* value = JsonUtils::Find(object, property.Name);
				if (!value)
					continue;
				const std::optional<PropertyValue> parsed = PropertyValueFromJson(property, *value);
				if (parsed && std::holds_alternative<UUID>(*parsed))
					references.Add(std::get<UUID>(*parsed));
			}
		}

		// The key of a document type's content object; empty for types whose stored form is no document.
		std::string_view GetDocumentKey(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:               return "Scene";
				case AssetType::Prefab:              return "Prefab";
				case AssetType::Model:               return "Model";
				case AssetType::Material:            return "Material";
				case AssetType::AnimationController: return "AnimationController";
				default:                             return {};
			}
		}

		// The references of one entity object of a document.
		void ScanEntityInto(const nlohmann::json& entity, const ComponentInfo* scriptInfo, ReferenceList& references)
		{
			const nlohmann::json* components = JsonUtils::Find(entity, "Components");
			if (!components || !components->is_object())
				return;
			for (const auto& [name, component] : components->items())
			{
				// Components this build does not register are kept, but what they reference is unknown.
				const ComponentInfo* info = ComponentRegistry::Find(name);
				if (!info || !component.is_object())
					continue;
				ScanProperties(info->Properties, component, references);
				// Script fields are data the properties cannot express: read them with the component's own reader.
				if (info == scriptInfo && info->DeserializeExtra)
				{
					ScriptComponent scripts;
					if (!info->DeserializeExtra(&scripts, component, nullptr))
						continue;
					for (const ScriptEntry& script : scripts.Scripts)
					{
						for (const ScriptFieldValue& field : script.Fields)
						{
							if (field.Type == PropertyType::Asset && std::holds_alternative<UUID>(field.Value))
								references.Add(std::get<UUID>(field.Value));
						}
					}
				}
			}
		}

		void ScanEntitiesInto(const nlohmann::json& snapshot, ReferenceList& references)
		{
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (!entities || !entities->is_array())
				return;
			const ComponentInfo* scriptInfo = ComponentRegistry::Find<ScriptComponent>();
			for (const nlohmann::json& entity : *entities)
				ScanEntityInto(entity, scriptInfo, references);
		}

		// Collects the references of a document's entities as SceneDocumentReader hands them over, one entity's DOM at a time:
		// a scan of a scene of a million entities holds its text and its references, never a DOM of it. A scan refuses no
		// entity data: it finds what it can.
		class ReferenceSink final : public EntitySink
		{
		public:
			explicit ReferenceSink(ReferenceList& references)
				: m_References(references), m_ScriptInfo(ComponentRegistry::Find<ScriptComponent>())
			{
			}

			bool ReadEntity(const nlohmann::json& entity, std::string*) override
			{
				ScanEntityInto(entity, m_ScriptInfo, m_References);
				return true;
			}

			bool Finish(std::string*) override { return true; }
		private:
			ReferenceList& m_References;
			const ComponentInfo* m_ScriptInfo = nullptr;
		};

		std::optional<SceneDocumentKind> GetEntityDocumentKind(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:  return SceneDocumentKind::Scene;
				case AssetType::Prefab: return SceneDocumentKind::Prefab;
				case AssetType::Model:  return SceneDocumentKind::Model;
				default:                return std::nullopt;
			}
		}

	}

	void AssetDependencies::ScanEntities(const nlohmann::json& snapshot, std::vector<AssetHandle>& outReferences)
	{
		ReferenceList references(outReferences);
		ScanEntitiesInto(snapshot, references);
	}

	bool AssetDependencies::CanReference(AssetType type)
	{
		return !GetDocumentKey(type).empty();
	}

	bool AssetDependencies::Scan(AssetType type, std::span<const uint8_t> storedForm, std::vector<AssetHandle>& outReferences, std::string* outError)
	{
		// Types that are not documents reference nothing.
		const std::string_view key = GetDocumentKey(type);
		if (key.empty())
			return true;

		const std::string_view text(reinterpret_cast<const char*>(storedForm.data()), storedForm.size());
		// Documents of entities are read as streams (their references found entity by entity), others whole.
		if (const std::optional<SceneDocumentKind> kind = GetEntityDocumentKind(type))
		{
			ReferenceList references(outReferences);
			ReferenceSink sink(references);
			return SceneDocumentReader::Read(text, *kind, sink, {}, nullptr, outError);
		}
		std::string error;
		const std::optional<nlohmann::json> document = JsonUtils::Parse(text, &error);
		if (!document)
		{
			if (outError)
				*outError = fmt::format("Invalid JSON: {}", error);
			return false;
		}
		return ScanDocument(type, *document, outReferences, outError);
	}

	bool AssetDependencies::ScanDocument(AssetType type, const nlohmann::json& document, std::vector<AssetHandle>& outReferences, std::string* outError)
	{
		const std::string_view key = GetDocumentKey(type);
		if (key.empty())
			return true;
		const nlohmann::json* content = JsonUtils::Find(document, key);
		if (!content || !content->is_object())
		{
			if (outError)
				*outError = fmt::format("The document has no \"{}\" object", key);
			return false;
		}

		ReferenceList references(outReferences);
		if (type == AssetType::Material)
		{
			ScanProperties(Material::GetPropertyInfos(), *content, references);
		}
		else if (type == AssetType::AnimationController)
		{
			// The clips of its states.
			const std::optional<AnimationControllerData> controller = AnimationController::DataFromJson(*content, outError);
			if (!controller)
				return false;
			for (const AnimationState& state : controller->States)
				references.Add(state.Clip);
		}
		else
		{
			ScanEntitiesInto(*content, references);
		}
		return true;
	}

}
