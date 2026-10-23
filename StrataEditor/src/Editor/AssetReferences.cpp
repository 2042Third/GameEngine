#include "Editor/AssetReferences.h"

#include "Editor/EditorContext.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <algorithm>
#include <optional>
#include <unordered_map>

namespace Strata
{

	namespace
	{

		// Whether a component type can name assets: an asset property that is serialized, or script fields.
		bool CanReferenceAssets(const ComponentInfo& info)
		{
			if (info.TypeId == entt::type_id<ScriptComponent>().hash())
				return true;
			return std::any_of(info.Properties.begin(), info.Properties.end(), [](const PropertyInfo& property)
			{
				return property.Type == PropertyType::Asset && !property.IsTransient();
			});
		}

		// What of the project names any of the targets: its start scene, its loading scene, the game's icon, the settings sections
		// whose validated values name one; nothing when none does.
		std::optional<AssetReferencer> FindProjectReferencer(const EditorContext& context, const std::unordered_set<AssetHandle>& targets)
		{
			const Ref<Project>& project = context.GetProject();
			if (!project)
				return std::nullopt;
			AssetReferencer entry;
			std::vector<std::string> names;
			const ProjectConfig& config = project->GetConfig();
			if (config.StartScene.IsValid() && targets.contains(config.StartScene))
			{
				entry.ProjectSettings.push_back("startScene");
				names.push_back("its start scene");
			}
			if (config.Export.LoadingScene.IsValid() && targets.contains(config.Export.LoadingScene))
			{
				entry.ProjectSettings.push_back("loadingScene");
				names.push_back("its loading scene");
			}
			if (config.Game.Icon.IsValid() && targets.contains(config.Game.Icon))
			{
				entry.ProjectSettings.push_back("gameIcon");
				names.push_back("the game's icon");
			}
			const SettingsRegistry& registry = context.GetSettingsRegistry();
			const ProjectSettingsValues& settings = project->GetSettings();
			for (const std::string& id : settings.GetStoredSections())
			{
				const SettingsSection* section = registry.Find(id);
				const nlohmann::json* value = settings.FindSection(id);
				if (!section || !value)
					continue;
				const std::vector<UUID> named = section->FindReferences(*value);
				if (std::any_of(named.begin(), named.end(), [&targets](UUID handle) { return targets.contains(handle); }))
				{
					entry.ProjectSettings.push_back(id);
					names.push_back(fmt::format("its settings '{}'", section->Title));
				}
			}
			if (entry.ProjectSettings.empty())
				return std::nullopt;
			entry.Project = true;
			entry.Path = FileSystem::ToUTF8(project->GetProjectFile().filename());
			for (const std::string& name : names)
				entry.Name += (entry.Name.empty() ? "" : ", ") + name;
			return entry;
		}

	}

	namespace AssetReferences
	{

		std::vector<SceneEntityReference> FindInScene(Scene& scene, const std::unordered_set<AssetHandle>& assets, size_t limit)
		{
			std::vector<SceneEntityReference> found;
			if (assets.empty())
				return found;
			entt::registry& registry = scene.GetRegistry();
			const entt::id_type scriptType = entt::type_id<ScriptComponent>().hash();
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (!CanReferenceAssets(*info))
					continue;
				const entt::sparse_set* storage = registry.storage(info->TypeId);
				if (!storage)
					continue;
				for (const entt::entity handle : *storage)
				{
					if (found.size() >= limit)
						return found;
					const void* component = info->Get(registry, handle);
					if (!component)
						continue;
					auto add = [&](std::string property, AssetHandle asset)
					{
						const Entity entity(handle, &scene);
						found.push_back({ entity.GetUUID(), entity.GetName(), info->Name, std::move(property), asset });
					};
					if (info->TypeId == scriptType)
					{
						for (const ScriptEntry& script : static_cast<const ScriptComponent*>(component)->Scripts)
						{
							for (const ScriptFieldValue& field : script.Fields)
							{
								if (field.Type == PropertyType::Asset && std::holds_alternative<UUID>(field.Value) && assets.contains(std::get<UUID>(field.Value)))
									add(fmt::format("{}.{}", script.ClassName, field.Name), std::get<UUID>(field.Value));
							}
						}
						continue;
					}
					for (const PropertyInfo& property : info->Properties)
					{
						if (property.Type != PropertyType::Asset || property.IsTransient())
							continue;
						const PropertyValue value = property.GetValue(component);
						if (std::holds_alternative<UUID>(value) && assets.contains(std::get<UUID>(value)))
							add(property.Name, std::get<UUID>(value));
					}
				}
			}
			return found;
		}

		std::vector<AssetHandle> WithSubAssets(const EditorContext& context, const std::vector<AssetHandle>& targets)
		{
			std::vector<AssetHandle> all = targets;
			if (const EditorAssetManager* assets = context.GetAssetManager())
			{
				for (AssetHandle target : targets)
				{
					// Only a file brings its sub-assets: a sub-asset's import info is its file's, whose other sub-assets (a model's
					// other meshes) are not the sub-asset's.
					const std::optional<AssetMetadata> metadata = assets->GetMetadata(target);
					if (!metadata || metadata->IsSubAsset())
						continue;
					const AssetImportInfo info = assets->GetImportInfo(target);
					all.insert(all.end(), info.SubAssets.begin(), info.SubAssets.end());
				}
			}
			return all;
		}

		std::vector<AssetReferencer> Find(EditorContext& context, const std::vector<AssetHandle>& targets, const std::unordered_set<AssetHandle>& ignored)
		{
			std::vector<AssetReferencer> referencers;
			const EditorAssetManager* assets = context.GetAssetManager();
			if (!assets)
				return referencers;

			// The assets whose recorded references name a target, each once.
			std::unordered_map<AssetHandle, size_t> listed;
			for (AssetHandle target : targets)
			{
				for (AssetHandle referencer : assets->GetReferencers(target))
				{
					if (ignored.contains(referencer) || listed.contains(referencer))
						continue;
					const std::optional<AssetMetadata> metadata = assets->GetMetadata(referencer);
					if (!metadata)
						continue;
					// Sub-assets that reference (a model's mesh using a material) stand for their file, unless it goes too.
					if (metadata->IsSubAsset() && ignored.contains(metadata->Parent))
						continue;
					AssetReferencer entry;
					entry.Asset = referencer;
					entry.Type = metadata->Type;
					entry.Path = metadata->Path;
					entry.Name = metadata->IsSubAsset() ? metadata->SubAssetKey : std::string();
					listed.emplace(referencer, referencers.size());
					referencers.push_back(std::move(entry));
				}
			}
			std::sort(referencers.begin(), referencers.end(), [](const AssetReferencer& a, const AssetReferencer& b)
			{
				return a.Path != b.Path ? a.Path < b.Path : a.Name < b.Name;
			});

			// The edited scene as it is now: its file's references may be out of date while it has unsaved changes.
			const std::unordered_set<AssetHandle> wanted(targets.begin(), targets.end());
			std::vector<SceneEntityReference> entities = FindInScene(*context.GetEditScene(), wanted);
			const AssetHandle scene = context.GetSceneHandle();
			auto open = std::find_if(referencers.begin(), referencers.end(), [scene](const AssetReferencer& entry)
			{
				return scene.IsValid() && entry.Asset == scene;
			});
			if (open != referencers.end())
			{
				open->OpenScene = true;
				open->Entities = std::move(entities);
			}
			else if (!entities.empty() && !(scene.IsValid() && ignored.contains(scene)))
			{
				AssetReferencer entry;
				entry.Asset = scene;
				entry.Type = AssetType::Scene;
				if (const std::optional<AssetMetadata> metadata = scene.IsValid() ? assets->GetMetadata(scene) : std::nullopt)
					entry.Path = metadata->Path;
				entry.OpenScene = true;
				entry.Entities = std::move(entities);
				referencers.insert(referencers.begin(), std::move(entry));
			}

			if (std::optional<AssetReferencer> project = FindProjectReferencer(context, wanted))
				referencers.insert(referencers.begin(), std::move(*project));
			return referencers;
		}

		void IgnoreSavedOpenScene(std::vector<AssetReferencer>& referencers)
		{
			std::erase_if(referencers, [](const AssetReferencer& entry) { return entry.OpenScene && entry.Entities.empty(); });
		}

		nlohmann::json ToJson(const std::vector<AssetReferencer>& referencers)
		{
			nlohmann::json list = nlohmann::json::array();
			for (const AssetReferencer& referencer : referencers)
			{
				nlohmann::json entry = {
					{ "asset", referencer.Asset.IsValid() ? UUIDToJson(referencer.Asset) : nlohmann::json(nullptr) },
					{ "path", referencer.Path },
					{ "type", referencer.Project ? "Project" : AssetTypeToString(referencer.Type) } };
				if (!referencer.Name.empty())
					entry["name"] = referencer.Name;
				if (referencer.Project)
				{
					entry["project"] = true;
					entry["settings"] = referencer.ProjectSettings;
				}
				if (referencer.OpenScene)
				{
					entry["openScene"] = true;
					nlohmann::json entities = nlohmann::json::array();
					for (const SceneEntityReference& entity : referencer.Entities)
					{
						entities.push_back({ { "id", UUIDToJson(entity.Entity) }, { "name", entity.Name }, { "component", entity.Component },
							{ "property", entity.Property }, { "asset", UUIDToJson(entity.Asset) } });
					}
					entry["entities"] = std::move(entities);
				}
				list.push_back(std::move(entry));
			}
			return list;
		}

		std::string Describe(const std::vector<AssetReferencer>& referencers, size_t maxNames)
		{
			std::string text;
			const size_t shown = std::min(referencers.size(), maxNames);
			for (size_t index = 0; index < shown; index++)
			{
				const AssetReferencer& referencer = referencers[index];
				if (!text.empty())
					text += ", ";
				text += referencer.Project ? fmt::format("the project {}", referencer.Path)
					: referencer.Path.empty() ? std::string("the open scene (never saved)") : referencer.Path;
				if (!referencer.Name.empty())
					text += fmt::format(" ({})", referencer.Name);
				if (referencer.OpenScene && !referencer.Entities.empty())
				{
					std::string names;
					for (size_t entity = 0; entity < std::min<size_t>(referencer.Entities.size(), 3); entity++)
						names += (names.empty() ? "" : ", ") + referencer.Entities[entity].Name;
					if (referencer.Entities.size() > 3)
						names += fmt::format(" and {} more", referencer.Entities.size() - 3);
					text += fmt::format(" (open, used by {})", names);
				}
			}
			if (referencers.size() > shown)
				text += fmt::format(" and {} more", referencers.size() - shown);
			return text;
		}

	}

}
