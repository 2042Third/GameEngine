#include "Editor/AssetEdit.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PrefabSync.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// prefab.apply's undo step: the prefab's file and the scene's links and updated instances (a group), and the snapshot
		// the editor's PrefabSync compares instances with, which follows the file both ways.
		class PrefabApplyAction final : public EditorAction
		{
		public:
			PrefabApplyAction(EditorContext& context, Scope<EditorActionGroup> group, AssetHandle prefab, nlohmann::json before, nlohmann::json after)
				: m_Context(context), m_Group(std::move(group)), m_Prefab(prefab), m_Before(std::move(before)), m_After(std::move(after))
			{
			}

			const std::string& GetName() const override { return m_Group->GetName(); }
			bool ChangesScene() const override { return m_Group->ChangesScene(); }

			bool Execute(std::string* outError) override
			{
				if (!m_Group->Execute(outError))
					return false;
				m_Context.GetPrefabSync().SetBaseline(m_Context, m_Prefab, m_After);
				return true;
			}

			bool Undo(std::string* outError) override
			{
				if (!m_Group->Undo(outError))
					return false;
				m_Context.GetPrefabSync().SetBaseline(m_Context, m_Prefab, m_Before);
				return true;
			}
		private:
			EditorContext& m_Context;
			Scope<EditorActionGroup> m_Group;
			AssetHandle m_Prefab;
			nlohmann::json m_Before;
			nlohmann::json m_After;
		};

		// The instance root of the "entity" parameter (an entity of the instance, its root or below). Records the problem
		// in the arguments when it is not linked to a prefab.
		Entity GetInstanceRoot(CommandArguments& arguments, Scene& scene)
		{
			const Entity entity = arguments.GetEntity(scene, "entity");
			if (!arguments.IsValid())
				return {};
			const Entity root = PrefabInstances::FindInstanceRoot(entity);
			if (!root)
				arguments.SetError(fmt::format("Parameter 'entity': '{}' is not part of a prefab instance", entity.GetName()));
			return root;
		}

		std::string DescribeAssetName(const EditorContext& context, AssetHandle asset)
		{
			const EditorAssetManager* assets = context.GetAssetManager();
			const std::optional<AssetMetadata> metadata = assets ? assets->GetMetadata(asset) : std::nullopt;
			return metadata ? metadata->Path : asset.ToString();
		}

		nlohmann::json ToJson(const std::vector<UUID>& ids)
		{
			nlohmann::json array = nlohmann::json::array();
			for (const UUID id : ids)
				array.push_back(UUIDToJson(id));
			return array;
		}

		// The answer of a command that changed the active scene: while it runs, the change belongs to the running copy, which
		// play.stop discards, and the answer says so.
		EditorCommandResult FinishSceneChange(const EditorContext& context, nlohmann::json result)
		{
			if (context.IsPlaying())
			{
				result["warning"] = fmt::format("The scene is running ({}): this change applies to the running copy and is discarded by play.stop",
					SceneStateToString(context.GetSceneState()));
			}
			return EditorCommandResult::Ok(std::move(result));
		}

	}

	void RegisterPrefabWorkflowCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "prefab.status",
			"Whether an entity belongs to a prefab instance and how the instance differs from its prefab as stored now (the root's transform and name "
			"are the instance's own and never count): {linked, root, prefab, path, modified, changed: [entity ids whose values differ], added: [entities "
			"the prefab lacks], removed: [ids of prefab entities the instance lacks]}; {linked: false} for entities of no instance.",
			ObjectSchema({ { "entity", EntitySchema("An entity of the active scene") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity entity = arguments.GetEntity(scene, "entity");
				if (!arguments.IsValid())
					return arguments.Fail();
				const Entity root = PrefabInstances::FindInstanceRoot(entity);
				if (!root)
					return EditorCommandResult::Ok({ { "linked", false } });
				const AssetHandle prefab = PrefabInstances::GetPrefab(root);
				EditorAssetManager* assets = context.GetAssetManager();
				std::string error;
				const std::optional<nlohmann::json> snapshot = assets ? PrefabSyncUtils::ReadTemplate(*assets, prefab, &error) : std::nullopt;
				if (!snapshot)
					return EditorCommandResult::Fail(fmt::format("The prefab {} cannot be read: {}", prefab.ToString(), assets ? error : std::string("no project is open")));
				const PrefabInstanceDifference difference = PrefabInstances::Compare(scene, root, *snapshot);
				return EditorCommandResult::Ok({
					{ "linked", true },
					{ "root", UUIDToJson(root.GetUUID()) },
					{ "prefab", UUIDToJson(prefab) },
					{ "path", DescribeAssetName(context, prefab) },
					{ "modified", !difference.IsEmpty() },
					{ "changed", ToJson(difference.Changed) },
					{ "added", ToJson(difference.Added) },
					{ "removed", ToJson(difference.Removed) } });
			} });

		registry.Register({ "prefab.apply",
			"Writes a prefab instance (the instance the entity belongs to: its root's subtree, also entities added to it) back into its prefab, keeping "
			"the prefab's own root transform and name. Other instances whose values equalled the prefab's take the new values; entities added to the "
			"instance are linked. One undo step for the file and the scene (edit.undo restores both). Edit mode only; imported models cannot be changed "
			"(prefab.unpack them, or prefab.create a prefab). Returns {prefab, path, root, updatedInstances, linked}.",
			ObjectSchema({ { "entity", EntitySchema("An entity of the instance (its root or below)") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				if (context.IsPlaying())
					return EditorCommandResult::Fail("Stop playing first: prefab.apply writes the prefab from the edited scene");
				Scene& scene = *context.GetEditScene();
				CommandArguments arguments(parameters);
				const Entity root = GetInstanceRoot(arguments, scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetHandle prefab = PrefabInstances::GetPrefab(root);
				const std::optional<AssetMetadata> metadata = assets->GetMetadata(prefab);
				if (!metadata || metadata->Type != AssetType::Prefab)
				{
					return EditorCommandResult::Fail(fmt::format("'{}' comes from {}, which is {}: unpack it (prefab.unpack) or save it as a prefab (prefab.create)",
						root.GetName(), DescribeAssetName(context, prefab), metadata ? "an imported model and cannot be changed" : "no asset of the project"));
				}

				std::string error;
				std::vector<uint8_t> before;
				if (!AssetEdit::ReadDocument(*assets, prefab, before, &error))
					return EditorCommandResult::Fail(error);
				std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(before.data()), before.size()), &error);
				if (!document || !document->contains("Prefab"))
					return EditorCommandResult::Fail(fmt::format("'{}' cannot be read: {}", metadata->Path, error.empty() ? std::string("no prefab document") : error));
				const nlohmann::json snapshot = (*document)["Prefab"];

				// The instance as the prefab's part, with the prefab's own root transform and name.
				const UUID templateRoot = root.GetComponent<PrefabInstanceComponent>().PrefabEntityID;
				const nlohmann::json part = PrefabInstances::ExtractTemplate(snapshot, templateRoot);
				const nlohmann::json* rootComponents = part["Entities"].empty() ? nullptr : JsonUtils::Find(part["Entities"][0], "Components");
				const nlohmann::json* rootTransform = rootComponents ? JsonUtils::Find(*rootComponents, "Transform") : nullptr;
				const nlohmann::json* rootName = rootComponents ? JsonUtils::Find(*rootComponents, "Name") : nullptr;
				std::unordered_map<UUID, UUID> ids;
				const nlohmann::json newPart = PrefabInstances::MakeTemplate(scene, root, rootTransform, rootName, ids);
				const nlohmann::json newSnapshot = PrefabInstances::ReplaceTemplate(snapshot, templateRoot, newPart);
				(*document)["Prefab"] = newSnapshot;
				if (!Prefab::FromJson(*document, &error))
					return EditorCommandResult::Fail(fmt::format("The instance cannot be stored as '{}': {}", metadata->Path, error));
				const std::string text = JsonUtils::Dump(*document, 1, '\t') + "\n";
				std::vector<uint8_t> after(text.begin(), text.end());

				// The scene: what was added to the instance is linked, and the other instances that still had the prefab's values
				// take the new ones.
				SceneEditTransaction transaction(scene, fmt::format("Apply Prefab {}", metadata->Name), {});
				transaction.TrackSubtree(root.GetUUID());
				std::vector<UUID> others;
				for (const Entity instance : PrefabInstances::FindInstanceRoots(scene, prefab))
				{
					if (instance != root && PrefabInstances::Compare(scene, instance, snapshot).IsEmpty())
					{
						others.push_back(instance.GetUUID());
						transaction.TrackSubtree(instance.GetUUID());
					}
				}
				uint32_t linked = 0;
				for (const auto& [id, templateId] : ids)
				{
					Entity entity = scene.GetEntityByUUID(id);
					const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
					if (!link || link->Prefab != prefab || link->PrefabEntityID != templateId)
					{
						entity.AddOrReplaceComponent<PrefabInstanceComponent>(PrefabInstanceComponent { prefab, templateId });
						linked++;
					}
				}
				for (const UUID other : others)
				{
					std::vector<UUID> created;
					if (!PrefabInstances::Replace(scene, scene.GetEntityByUUID(other), newSnapshot, &created, &error))
					{
						transaction.Rollback();
						return EditorCommandResult::Fail(fmt::format("Updating another instance failed: {}", error));
					}
					for (const UUID entity : created)
						transaction.TrackCreated(entity);
				}

				// The file, then one undo step for both.
				if (!AssetEdit::ApplyDocument(*assets, prefab, after, &error))
				{
					transaction.Rollback();
					return EditorCommandResult::Fail(error);
				}
				Scope<EditorActionGroup> group = CreateScope<EditorActionGroup>(fmt::format("Apply Prefab {}", metadata->Name));
				group->Add(CreateScope<AssetDocumentAction>(*assets, prefab, fmt::format("Apply Prefab {}", metadata->Name), std::move(before), std::move(after)));
				group->Add(transaction.TakeAction());
				context.GetUndoStack().Record(CreateScope<PrefabApplyAction>(context, std::move(group), prefab, snapshot, newSnapshot));
				context.GetPrefabSync().SetBaseline(context, prefab, newSnapshot);
				context.PruneSelection();
				return EditorCommandResult::Ok({ { "prefab", UUIDToJson(prefab) }, { "path", metadata->Path }, { "root", UUIDToJson(root.GetUUID()) },
					{ "updatedInstances", others.size() }, { "linked", linked } });
			} });

		registry.Register({ "prefab.revert",
			"Gives a prefab instance (the instance the entity belongs to) its prefab's values again: entities keep their IDs (matched by the prefab's "
			"entity ids), entities the prefab lacks go, entities the instance lacks are made, and the root keeps its parent, place, transform and name. "
			"One undo step (while playing it changes the running copy only). Returns {root, prefab, created}.",
			ObjectSchema({ { "entity", EntitySchema("An entity of the instance (its root or below)") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity root = GetInstanceRoot(arguments, scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetHandle prefab = PrefabInstances::GetPrefab(root);
				std::string error;
				const std::optional<nlohmann::json> snapshot = PrefabSyncUtils::ReadTemplate(*assets, prefab, &error);
				if (!snapshot)
					return EditorCommandResult::Fail(fmt::format("The prefab {} cannot be read: {}", DescribeAssetName(context, prefab), error));

				const UUID rootId = root.GetUUID();
				SceneEditTransaction transaction(scene, fmt::format("Revert {}", root.GetName()), {});
				transaction.TrackSubtree(rootId);
				std::vector<UUID> created;
				if (!PrefabInstances::Replace(scene, root, *snapshot, &created, &error))
				{
					transaction.Rollback();
					return EditorCommandResult::Fail(error);
				}
				for (const UUID entity : created)
					transaction.TrackCreated(entity);
				context.CommitEdit(transaction);
				context.PruneSelection();
				return FinishSceneChange(context, { { "root", UUIDToJson(rootId) }, { "prefab", UUIDToJson(prefab) }, { "created", ToJson(created) } });
			} });

		registry.Register({ "prefab.unpack",
			"Ends the link between a prefab instance (the instance the entity belongs to) and its prefab: its entities stay as they are, as plain "
			"entities. One undo step (while playing it changes the running copy only). Returns {root, entities (how many were unlinked)}.",
			ObjectSchema({ { "entity", EntitySchema("An entity of the instance (its root or below)") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity root = GetInstanceRoot(arguments, scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetHandle prefab = PrefabInstances::GetPrefab(root);
				SceneEditTransaction transaction(scene, fmt::format("Unpack {}", root.GetName()), {});
				transaction.TrackSubtree(root.GetUUID());
				size_t unlinked = 0;
				for (const UUID id : SceneEdit::CollectSubtree(scene, root.GetUUID()))
				{
					const Entity entity = scene.GetEntityByUUID(id);
					const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
					unlinked += link && link->Prefab == prefab ? 1 : 0;
				}
				PrefabInstances::Unpack(scene, root);
				context.CommitEdit(transaction);
				return FinishSceneChange(context, { { "root", UUIDToJson(root.GetUUID()) }, { "entities", unlinked } });
			} });

		registry.Register({ "prefab.select",
			"Selects the root of the prefab instance the entity belongs to, or with asset: true shows its prefab in the asset inspector (the Content "
			"Browser reveals it). Returns {root, prefab}.",
			ObjectSchema({ { "entity", EntitySchema("An entity of the instance (its root or below)") },
				{ "asset", BoolSchema("Inspect the prefab asset instead of selecting the root (default false)") } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity root = GetInstanceRoot(arguments, scene);
				const bool asset = arguments.GetBool("asset", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetHandle prefab = PrefabInstances::GetPrefab(root);
				if (asset)
				{
					if (!context.SetInspectedAsset(prefab))
						return EditorCommandResult::Fail(fmt::format("The prefab {} is no asset of the project", prefab.ToString()));
				}
				else
				{
					context.Select(root.GetUUID());
				}
				return EditorCommandResult::Ok({ { "root", UUIDToJson(root.GetUUID()) }, { "prefab", UUIDToJson(prefab) } });
			} });
	}

}
