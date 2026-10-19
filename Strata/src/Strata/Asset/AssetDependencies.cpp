#include "stpch.h"
#include "Strata/Asset/AssetDependencies.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/Components.h"
#include "Strata/World/WorldAsset.h"

#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationController.h"
#endif

#include <nlohmann/json.hpp>

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
				case AssetType::World:               return "World";
				default:                             return {};
			}
		}

		void ScanEntitiesInto(const nlohmann::json& snapshot, ReferenceList& references)
		{
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (!entities || !entities->is_array())
				return;
			for (const nlohmann::json& entity : *entities)
			{
				const nlohmann::json* components = JsonUtils::Find(entity, "Components");
				if (!components || !components->is_object())
					continue;
				for (const auto& [name, component] : components->items())
				{
					// Components this build does not register are kept, but what they reference is unknown.
					const ComponentInfo* info = ComponentRegistry::Find(name);
					if (!info || !component.is_object())
						continue;
					ScanProperties(info->Properties, component, references);
					// Data the properties cannot express (script fields): its references, as the component reads them.
					if (info->ExtraReferences.VisitJson)
					{
						info->ExtraReferences.VisitJson(component, [&references](PropertyType type, UUID value)
						{
							if (type == PropertyType::Asset)
								references.Add(value);
						});
					}
				}
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

		std::string error;
		const std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(storedForm.data()), storedForm.size()), &error);
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
#if ST_WITH_ANIMATION
			// The clips of its states.
			const std::optional<AnimationControllerData> controller = AnimationController::DataFromJson(*content, outError);
			if (!controller)
				return false;
			for (const AnimationState& state : controller->States)
				references.Add(state.Clip);
#else
			if (outError)
				*outError = "animation controllers cannot be read: the engine has no animation module";
			return false;
#endif
		}
		else if (type == AssetType::World)
		{
			// The persistent scene and every cell's scene. Scenes stop load closures (AssetClosureScope::Load), so loading the
			// persistent scene, which references the world, never pins its cells.
			const std::optional<WorldData> world = WorldData::FromJson(*content, outError);
			if (!world)
				return false;
			references.Add(world->PersistentScene);
			for (const WorldCell& cell : world->Cells)
				references.Add(cell.Scene);
		}
		else
		{
			ScanEntitiesInto(*content, references);
		}
		return true;
	}

}
