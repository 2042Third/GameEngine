#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorWorld.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// Named layers by index, and the pairs of layers that do not collide by their names ("Layer <index>" without one).
		nlohmann::json DescribeLayers(const PhysicsLayerSettings& layers)
		{
			nlohmann::json names = nlohmann::json::object();
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				if (!layers.Names[layer].empty())
					names[std::to_string(layer)] = layers.Names[layer];
			}
			nlohmann::json noCollisions = nlohmann::json::array();
			for (uint32_t a = 0; a < c_PhysicsLayerCount; a++)
			{
				for (uint32_t b = a; b < c_PhysicsLayerCount; b++)
				{
					if (!layers.DoCollide(a, b))
						noCollisions.push_back({ layers.GetDisplayName(a), layers.GetDisplayName(b) });
				}
			}
			return { { "names", std::move(names) }, { "noCollisions", std::move(noCollisions) } };
		}

		nlohmann::json DescribeSettings(const SceneSettings& settings)
		{
			return {
				{ "gravity", { settings.Gravity.x, settings.Gravity.y, settings.Gravity.z } },
				{ "fixedTimestep", settings.FixedTimestep },
				{ "maxFixedStepsPerFrame", settings.MaxFixedStepsPerFrame },
				{ "maxEntitiesInstantiatedPerFrame", settings.MaxEntitiesInstantiatedPerFrame },
				{ "maxEntitiesDestroyedPerFrame", settings.MaxEntitiesDestroyedPerFrame },
				{ "physicsCapacity", {
					{ "maxBodies", settings.PhysicsCapacity.MaxBodies },
					{ "maxContactConstraints", settings.PhysicsCapacity.MaxContactConstraints } } },
				{ "physicsLayers", DescribeLayers(settings.PhysicsLayers) }
			};
		}

		// A layer given by index (0 to c_PhysicsLayerCount - 1) or by name (its name, or "Layer <index>" without one).
		std::optional<uint32_t> ReadLayer(const nlohmann::json& value, const PhysicsLayerSettings& layers)
		{
			if (value.is_number_unsigned() || (value.is_number_integer() && value.get<int64_t>() >= 0))
			{
				const uint64_t index = value.get<uint64_t>();
				return index < c_PhysicsLayerCount ? std::optional<uint32_t>(static_cast<uint32_t>(index)) : std::nullopt;
			}
			if (value.is_string())
				return layers.FindLayer(value.get<std::string>());
			return std::nullopt;
		}

		// Reads scene.setSettings' "layerNames" ({"<index>": name}; "" or null clears a name) into `layers`. The names are
		// checked once all of them are in place, so layers can swap names in one call. Returns an error message, empty on
		// success.
		std::string ReadLayerNames(const nlohmann::json& value, PhysicsLayerSettings& layers)
		{
			if (!value.is_object())
				return R"(Parameter 'layerNames' must be an object of layer indices to names, e.g. {"3": "Player"})";
			std::vector<std::pair<uint32_t, std::string>> names;
			for (const auto& [key, name] : value.items())
			{
				// Indices without leading zeros, so that one layer cannot be named twice ("3" and "03").
				const bool isIndex = !key.empty() && key.size() <= 2 && (key.size() == 1 || key[0] != '0')
					&& std::all_of(key.begin(), key.end(), [](char character) { return character >= '0' && character <= '9'; });
				uint32_t layer = c_PhysicsLayerCount;
				if (isIndex)
				{
					layer = 0;
					for (const char character : key)
						layer = layer * 10 + static_cast<uint32_t>(character - '0');
				}
				if (layer >= c_PhysicsLayerCount)
					return fmt::format("Parameter 'layerNames': '{}' is no layer index (0 to {})", key, c_PhysicsLayerCount - 1);
				if (!name.is_string() && !name.is_null())
					return fmt::format("Parameter 'layerNames.{}' must be a string (empty or null clears the name)", key);
				names.emplace_back(layer, name.is_string() ? name.get<std::string>() : std::string());
				layers.Names[layer].clear();
			}
			for (const auto& [layer, name] : names)
			{
				if (std::optional<std::string> error = layers.ValidateName(layer, name))
					return fmt::format("Parameter 'layerNames.{}': {}", layer, *error);
				layers.Names[layer] = name;
			}
			return {};
		}

		// Reads scene.setSettings' "layerCollisions" ([{"layers": [a, b], "collide": bool}], layers by index or name, applied
		// in order) into `layers`. Returns an error message, empty on success.
		std::string ReadLayerCollisions(const nlohmann::json& value, PhysicsLayerSettings& layers)
		{
			constexpr std::string_view c_Form = R"({"layers": [a, b], "collide": true | false})";
			if (!value.is_array())
				return fmt::format("Parameter 'layerCollisions' must be an array of {}", c_Form);
			for (const nlohmann::json& entry : value)
			{
				if (!entry.is_object())
					return fmt::format("Parameter 'layerCollisions': {} is not of the form {}", entry.dump(), c_Form);
				for (const auto& [key, unused] : entry.items())
				{
					if (key != "layers" && key != "collide")
						return fmt::format("Parameter 'layerCollisions': unknown key '{}' (entries are {})", key, c_Form);
				}
				const auto pair = entry.find("layers");
				const auto collide = entry.find("collide");
				if (pair == entry.end() || !pair->is_array() || pair->size() != 2 || collide == entry.end() || !collide->is_boolean())
					return fmt::format("Parameter 'layerCollisions': {} is not of the form {}", entry.dump(), c_Form);
				std::optional<uint32_t> found[2];
				for (size_t side = 0; side < 2; side++)
				{
					found[side] = ReadLayer((*pair)[side], layers);
					if (!found[side])
					{
						return fmt::format("Parameter 'layerCollisions': no layer {} (layers are indices from 0 to {} or their names)", (*pair)[side].dump(),
							c_PhysicsLayerCount - 1);
					}
				}
				layers.SetCollide(*found[0], *found[1], collide->get<bool>());
			}
			return {};
		}

		// Reads scene.setSettings' "physicsCapacity" object into `capacity` (only the fields it names change). Returns an error
		// message for an invalid object, empty on success; `outWarning` gets what was accepted but ignored.
		std::string ReadPhysicsCapacity(const nlohmann::json& value, PhysicsCapacitySettings& capacity, std::string& outWarning)
		{
			if (!value.is_object())
				return "Parameter 'physicsCapacity' must be an object";
			const struct
			{
				const char* Name;
				uint32_t PhysicsCapacitySettings::*Field;
				uint32_t Limit;
			} fields[] = {
				{ "maxBodies", &PhysicsCapacitySettings::MaxBodies, PhysicsCapacitySettings::c_MaxBodiesLimit },
				{ "maxContactConstraints", &PhysicsCapacitySettings::MaxContactConstraints, PhysicsCapacitySettings::c_MaxContactConstraintsLimit } };
			for (const auto& [key, entry] : value.items())
			{
				// Captured through a name of its own: Apple Clang 15 cannot capture structured bindings.
				const std::string& name = key;
				if (name == "maxBodyPairs")
				{
					// 0.4.0's capacity of its physics library's pair buffer: command scripts written for it keep working, told that it
					// does nothing.
					outWarning = "physicsCapacity.maxBodyPairs is ignored: since 0.5.0 (box3d) the physics world has no buffer of body pairs to size";
					continue;
				}
				const auto field = std::find_if(std::begin(fields), std::end(fields), [&name](const auto& candidate) { return name == candidate.Name; });
				if (field == std::end(fields))
					return fmt::format("Unknown parameter 'physicsCapacity.{}' (known: maxBodies, maxContactConstraints)", key);
				const bool isCount = entry.is_number_unsigned() || (entry.is_number_integer() && entry.get<int64_t>() >= 0);
				if (!isCount || entry.get<uint64_t>() > field->Limit)
					return fmt::format("Parameter 'physicsCapacity.{}' must be an integer from 0 (automatic) to {}", key, field->Limit);
				capacity.*(field->Field) = static_cast<uint32_t>(entry.get<uint64_t>());
			}
			return {};
		}

		// The load of scene.open: starts opening the scene and answers once it replaced the edited scene, with the frames that
		// ran meanwhile (the frame that swapped it in included). `discardsChanges`: the edited scene has unsaved changes the open
		// discards (the caller's guard let it), reported as discardedChanges.
		EditorCommandResult OpenSceneAndWait(EditorContext& context, AssetHandle handle, bool discardsChanges)
		{
			std::string error;
			if (!context.BeginOpenScene(handle, &error))
				return EditorCommandResult::Fail(error);
			const uint64_t id = context.GetSceneLoadStatus().ID;
			// Frames are measured from the one that started the load, which the editor records at the start of the next frame.
			const uint64_t firstMeasured = context.GetRecordedFrameCount() + 1;
			return EditorCommandResult::Defer([id, discardsChanges, frames = uint64_t(0), frameTimes = FrameTimeRecorder(firstMeasured, c_FrameTimeReserve),
				finishedAt = std::optional<uint64_t>()](EditorContext& context) mutable -> std::optional<EditorCommandResult>
			{
				frames++;
				const uint64_t recorded = context.GetRecordedFrameCount();
				frameTimes.Update(context);
				const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
				if (status.ID != id)
					return EditorCommandResult::Fail("Opening the scene was cancelled: another scene is being opened", EditorCommandError::Cancelled);
				if (!status.IsFinished())
					return std::nullopt;
				if (status.State == SceneLoadState::Cancelled)
					return EditorCommandResult::Fail(status.Error, EditorCommandError::Cancelled);
				if (status.State == SceneLoadState::Failed)
					return EditorCommandResult::Fail(status.Error);
				// The frame that swapped the scene in is recorded at the start of the next one: one more poll reports it.
				if (!finishedAt)
				{
					finishedAt = recorded;
					return std::nullopt;
				}
				const Ref<Scene>& scene = context.GetEditScene();
				nlohmann::json value = {
					{ "scene", UUIDToJson(status.Scene) },
					{ "name", scene->GetName() },
					{ "entities", scene->GetEntityCount() },
					{ "discardedChanges", discardsChanges },
					{ "seconds", status.Seconds },
					{ "frames", frames } };
				if (!frameTimes.IsEmpty())
					value["frameTimes"] = frameTimes.Describe();
				return EditorCommandResult::Ok(std::move(value));
			});
		}

		// Starts a save of the edited scene (`begin`: EditorContext::BeginSaveScene or BeginSaveSceneAs) for scene.save and
		// scene.saveAs: answered at once without `wait`, else once the save ended, with what it wrote and the frames meanwhile.
		EditorCommandResult StartSceneSave(EditorContext& context, bool wait, const std::function<uint64_t(EditorContext&, std::string*)>& begin)
		{
			const std::optional<ProcessMemoryInfo> before = Platform::GetProcessMemory();
			const auto started = std::chrono::steady_clock::now();
			std::string error;
			const uint64_t id = begin(context, &error);
			if (id == 0)
				return EditorCommandResult::Fail(error);
			const AssetHandle scene = context.GetSceneHandle();
			std::string path = context.GetSceneFile(scene).second;
			if (path.empty() && context.GetAssetManager())
				path = context.GetAssetManager()->GetReservedPath(scene);
			if (!wait)
			{
				const std::optional<DocumentSaveStatus> status = context.GetDocumentSaver().GetStatus(id);
				return EditorCommandResult::Ok({ { "scene", UUIDToJson(scene) }, { "path", path }, { "saveId", id },
					{ "state", status ? DocumentSaveStateToString(status->State) : "Done" } });
			}
			// Frames are measured from the one that started the save (its snapshot), which the editor records at the start of the
			// next frame.
			const uint64_t privateBefore = before ? before->PrivateBytes : 0;
			const uint64_t firstMeasured = context.GetRecordedFrameCount() + 1;
			return EditorCommandResult::Defer([id, scene, path, started, privateBefore, peakPrivate = privateBefore, frames = uint64_t(0),
				frameTimes = FrameTimeRecorder(firstMeasured, c_FrameTimeReserve)](EditorContext& context) mutable -> std::optional<EditorCommandResult>
			{
				frames++;
				frameTimes.Update(context);
				if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
					peakPrivate = std::max(peakPrivate, memory->PrivateBytes);
				const std::optional<DocumentSaveStatus> status = context.GetDocumentSaver().GetStatus(id);
				if (!status)
					return EditorCommandResult::Fail("The save's outcome is no longer known");
				if (!status->IsFinished())
					return std::nullopt;
				if (status->State == DocumentSaveState::Failed)
				{
					// The editor told the person already (EditorContext posts a notification of a failed save).
					EditorCommandResult failed = EditorCommandResult::Fail(status->Error);
					failed.Announced = true;
					return failed;
				}
				// A world's save ends with its World document: what it wrote is listed then.
				if (context.IsWorldSaveWriting(id))
					return std::nullopt;
				// Its cells are written, but the World document that lists them is not: the scene stays modified.
				if (std::string worldError = context.GetWorldSaveError(id); !worldError.empty())
				{
					EditorCommandResult failed = EditorCommandResult::Fail(std::move(worldError));
					failed.Announced = true;
					return failed;
				}
				const std::optional<std::vector<WorldSavedDocument>> documents = context.GetWorldSaveDocuments(id);
				constexpr double c_Megabyte = 1024.0 * 1024.0;
				const DocumentSaveItemResult* written = status->Items.empty() ? nullptr : &status->Items.front();
				nlohmann::json value = {
					{ "scene", UUIDToJson(scene) },
					{ "path", written ? written->Path : path },
					{ "bytes", written ? written->Bytes : 0 },
					{ "entities", written ? written->Entities : 0 },
					{ "seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() },
					{ "frames", frames },
					{ "memory", { { "privateMBBefore", static_cast<double>(privateBefore) / c_Megabyte },
						{ "peakPrivateMBDuring", static_cast<double>(peakPrivate) / c_Megabyte } } } };
				if (!frameTimes.IsEmpty())
					value["frameTimes"] = frameTimes.Describe();
				if (documents)
				{
					nlohmann::json list = nlohmann::json::array();
					uint64_t bytes = 0;
					uint64_t entities = 0;
					for (const WorldSavedDocument& document : *documents)
					{
						list.push_back({ { "path", document.Path }, { "kind", document.Kind }, { "bytes", document.Bytes }, { "entities", document.Entities } });
						bytes += document.Bytes;
						entities += document.Entities;
					}
					value["path"] = path;
					value["bytes"] = bytes;
					value["entities"] = entities;
					value["documents"] = std::move(list);
				}
				return EditorCommandResult::Ok(std::move(value));
			});
		}

		nlohmann::json DescribeSubScene(const SubSceneStatus& status)
		{
			nlohmann::json value = {
				{ "subScene", status.ID },
				{ "scene", status.Scene.IsValid() ? UUIDToJson(status.Scene) : nlohmann::json(nullptr) },
				{ "state", SubSceneStateToString(status.State) },
				{ "progress", status.Progress },
				{ "createdEntities", status.CreatedEntities },
				{ "skippedEntities", status.SkippedEntities },
				{ "totalEntities", status.TotalEntities },
				{ "slices", status.Slices },
				{ "error", status.Error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.Error) } };
			// Being unloaded: its entities are inactive and go a slice per frame.
			if (status.State == SubSceneState::Unloading)
				value["destroyedEntities"] = status.DestroyedEntities;
			return value;
		}

		// What scene.loadStatus reports: the scene being opened or the last one, the running game's switch and its sub-scenes.
		nlohmann::json DescribeSceneLoads(const EditorContext& context)
		{
			const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
			nlohmann::json open = nullptr;
			nlohmann::json lastOpen = nullptr;
			if (context.IsSceneLoading())
			{
				open = { { "scene", UUIDToJson(status.Scene) }, { "startScene", status.StartScene }, { "state", SceneLoadStateToString(status.State) },
					{ "progress", status.Progress } };
			}
			else if (status.ID != 0)
			{
				lastOpen = { { "scene", UUIDToJson(status.Scene) }, { "startScene", status.StartScene }, { "state", SceneLoadStateToString(status.State) },
					{ "error", status.Error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.Error) }, { "seconds", status.Seconds } };
			}
			nlohmann::json play = nullptr;
			if (context.IsRuntimeSceneLoading())
				play = { { "scene", UUIDToJson(context.GetRuntimeSceneLoadTarget()) }, { "progress", context.GetRuntimeSceneLoadProgress() } };
			nlohmann::json subScenes = nlohmann::json::array();
			for (const SubSceneStatus& subScene : context.GetActiveScene()->GetSubScenes())
				subScenes.push_back(DescribeSubScene(subScene));
			return { { "open", std::move(open) }, { "lastOpen", std::move(lastOpen) }, { "play", std::move(play) }, { "subScenes", std::move(subScenes) },
				{ "world", DescribeWorldLoads(context) } };
		}

		// The load of scene.loadAdditive: starts loading the scene asset into the running scene and answers once every entity
		// of it exists (or it failed), with its roots.
		EditorCommandResult LoadSubSceneAndWait(EditorContext& context, AssetHandle handle)
		{
			const Ref<Scene>& running = context.GetActiveScene();
			std::string error;
			const SubSceneID id = running->LoadSubScene(handle, {}, &error);
			if (id == 0)
				return EditorCommandResult::Fail(error);
			const auto started = std::chrono::steady_clock::now();
			return EditorCommandResult::Defer([id, scene = std::weak_ptr<Scene>(running), started, frames = uint64_t(0)](EditorContext& context) mutable
				-> std::optional<EditorCommandResult>
			{
				frames++;
				const Ref<Scene> target = scene.lock();
				if (!target || target != context.GetActiveScene())
					return EditorCommandResult::Fail("Play mode stopped or switched scenes before the sub-scene loaded", EditorCommandError::Cancelled);
				const SubSceneStatus* status = target->GetSubSceneStatus(id);
				if (!status)
					return EditorCommandResult::Fail(fmt::format("Sub-scene {} was unloaded before it loaded", id), EditorCommandError::Cancelled);
				if (status->State == SubSceneState::Failed)
					return EditorCommandResult::Fail(fmt::format("Sub-scene {} failed: {}", id, status->Error));
				if (status->State != SubSceneState::Loaded)
					return std::nullopt;
				nlohmann::json value = DescribeSubScene(*status);
				nlohmann::json roots = nlohmann::json::array();
				for (const Entity root : target->GetSubSceneRoots(id))
					roots.push_back(UUIDToJson(root.GetUUID()));
				value["roots"] = std::move(roots);
				value["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
				value["frames"] = frames;
				// Sub-scenes belong to the running copy, which play.stop discards: replays leave the load out. Their only target is
				// the running game, so no warning says so.
				EditorCommandResult result = EditorCommandResult::Ok(std::move(value));
				result.RunningCopy = true;
				return result;
			});
		}

	}

	void RegisterSceneCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Scene
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "scene.info", "Name, asset, modification and play state, entity count and settings of the edited scene, and what runs "
			"while playing (running: the running scene, which a game's scene switch makes another than the edited one; null in edit mode; see "
			"play.state). entityCount counts the active scene's entities (the running one while playing).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const Ref<Scene>& scene = context.GetActiveScene();
				return EditorCommandResult::Ok({
					{ "name", context.GetEditScene()->GetName() },
					{ "scene", context.GetSceneHandle().IsValid() ? UUIDToJson(context.GetSceneHandle()) : nlohmann::json(nullptr) },
					{ "modified", context.IsSceneModified() },
					{ "state", SceneStateToString(context.GetSceneState()) },
					{ "paused", context.IsPaused() },
					{ "entityCount", scene->GetEntityCount() },
					{ "settings", DescribeSettings(scene->GetSettings()) },
					{ "running", DescribeRunningScene(context) } });
			}, EditorCommandKind::Query });

		registry.Register({ "scene.hierarchy", "Every entity of the active scene in hierarchy order, with parent, depth and component names.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				Scene& scene = *context.GetActiveScene();
				entt::registry& registry = scene.GetRegistry();
				nlohmann::json entities = nlohmann::json::array();
				// Depth-first with the depth carried along (walking up per entity would be quadratic on deep chains).
				std::vector<std::pair<UUID, uint32_t>> stack;
				const std::vector<UUID>& roots = scene.GetRootEntities();
				for (auto it = roots.rbegin(); it != roots.rend(); ++it)
					stack.emplace_back(*it, 0);
				while (!stack.empty())
				{
					const auto [id, depth] = stack.back();
					stack.pop_back();
					Entity entity = scene.GetEntityByUUID(id);
					if (!entity)
						continue;
					nlohmann::json components = nlohmann::json::array();
					for (const ComponentInfo* info : ComponentRegistry::GetAll())
					{
						if (!info->IsHidden() && !HasFlag(info->Flags, ComponentFlags::NoSerialize) && info->Has(registry, entity.GetHandle()))
							components.push_back(info->Name);
					}
					const RelationshipComponent& relationship = entity.GetComponent<RelationshipComponent>();
					entities.push_back({
						{ "id", UUIDToJson(id) },
						{ "name", entity.GetName() },
						{ "parent", relationship.Parent.IsValid() ? UUIDToJson(relationship.Parent) : nlohmann::json(nullptr) },
						{ "depth", depth },
						{ "active", entity.IsActive() },
						{ "components", std::move(components) } });
					for (auto it = relationship.Children.rbegin(); it != relationship.Children.rend(); ++it)
						stack.emplace_back(*it, depth + 1);
				}
				return EditorCommandResult::Ok({ { "entities", std::move(entities) } });
			}, EditorCommandKind::Query });

		registry.Register(MayDefer({ "scene.new",
			"Replaces the edited scene with a new, unsaved one: empty by default, or with a template's content, e.g. \"basic3d\" (a lit start: camera, "
			"sun, procedural sky, ground, post-processing; the editor camera then looks through the scene's camera; the ground's material "
			"Materials/Ground.stmat is added to a project that lacks it). Fails while the edited scene has unsaved changes unless force is true (save "
			"first). Save the new scene with scene.saveAs. Waits for saves in flight (scene.save) first. Returns discardedChanges (whether unsaved "
			"changes were discarded).",
			ObjectSchema({
				{ "name", StringSchema("Scene name") },
				{ "template", { { "type", "string" }, { "enum", { ProjectTemplates::c_Empty, ProjectTemplates::c_Basic3D } },
					{ "description", "What the scene starts with (default \"empty\"; see project.templates)" } } },
				{ "force", ForceSchema() } }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("name", "Untitled");
				const std::string templateId = arguments.GetString("template", std::string(ProjectTemplates::c_Empty));
				const bool force = arguments.GetBool("force", false);
				if (arguments.IsValid() && !ProjectTemplates::Find(templateId))
					arguments.SetError(fmt::format("Unknown template '{}' (templates: {})", templateId, ProjectTemplates::ListIds()));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, "starting a new scene"))
					return std::move(*refused);
				const bool discarded = context.IsSceneModified();
				if (!context.NewScene(name, templateId))
					return EditorCommandResult::Fail(fmt::format("The template '{}' could not be applied", templateId));
				return EditorCommandResult::Ok({ { "discardedChanges", discarded } });
			}) }));

		registry.Register(MayDefer({ "scene.open",
			"Opens a scene asset for editing. Fails while the edited scene has unsaved changes unless force is true (save first); waits for saves in "
			"flight (scene.save) before it decides. The scene is "
			"read and loaded with everything it uses in the background while the editor goes on showing the edited scene, which stays read-only "
			"meanwhile (edits and play.start fail); the command answers once the opened scene replaced it: {scene, name, entities, "
			"discardedChanges (whether unsaved changes were discarded), seconds, frames, frameTimes (the CPU time of the frames that ran "
			"meanwhile: count, medianMs, meanMs, p95Ms, maxMs)}. Opening another scene, scene.new or closing the project cancels it. "
			"scene.loadStatus (and editor.status's sceneLoad) report its progress.",
			ObjectSchema({ { "scene", AssetSchema("Scene to open") }, { "force", ForceSchema() } }, { "scene" }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "scene", AssetType::Scene);
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, fmt::format("opening the scene {}", parameters["scene"].dump())))
					return std::move(*refused);
				return OpenSceneAndWait(context, handle, context.IsSceneModified());
			}) }));

		registry.Register(MayDefer({ "scene.loadStatus",
			"The scene loads under way: open (the scene scene.open, or project.open for the project's start scene, is opening: {scene, startScene, "
			"state: LoadingScene | LoadingDependencies | Preparing (its render proxies are built for the viewport), progress 0 to 1}, null when "
			"none), lastOpen (the latest of those that finished: {scene, "
			"startScene, state: Ready | Failed | Cancelled, error, seconds}, null before the first), play (the scene a running game asked for "
			"while it loads, {scene, progress}, null when none) and subScenes (the running scene's sub-scenes, scene.loadAdditive: {subScene, "
			"scene, state: Loading | Instantiating | Loaded | Failed, progress, createdEntities, skippedEntities (not created because an "
			"ancestor was deleted while the sub-scene loaded), totalEntities, slices, error}) and world (the cells the open world streams around "
			"the editor camera, editor, and while playing those of the running game, game: each {cellsWanted, cellsLoading, cellsLoaded, "
			"cellsUnloading, cellsPinned, cellsFailed} or null; null when no world streams). With wait, it "
			"answers once no scene is being opened, e.g. after starting an editor with a window, which opens its project's start scene in the "
			"background (scene edits and play.start fail until then).",
			ObjectSchema({ { "wait", BoolSchema("Answer once no scene is being opened (default false: at once)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool wait = arguments.GetBool("wait", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!wait || !context.IsSceneLoading())
					return EditorCommandResult::Ok(DescribeSceneLoads(context));
				return EditorCommandResult::Defer([](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					if (context.IsSceneLoading())
						return std::nullopt;
					return EditorCommandResult::Ok(DescribeSceneLoads(context));
				});
			}, EditorCommandKind::Query, EditorCommandResults::Session }));

		registry.Register(MayDefer({ "scene.loadAdditive",
			"Loads a scene asset into the running game next to what it holds (a sub-scene, as scripts' Scene::LoadAdditive does; play mode "
			"only: play.stop ends it with the game). The scene and everything it uses load in the background, "
			"then its entities are created over the following frames (at most the scene's maxEntitiesInstantiatedPerFrame per frame), keeping "
			"their IDs: a sub-scene whose IDs the running scene has already (the same scene twice) fails. Answers once every entity exists: "
			"{subScene (its number, for scene.unloadAdditive), scene, state, progress, createdEntities, skippedEntities, totalEntities, slices, roots (the "
			"top-level entities), seconds, frames}. scene.loadStatus reports sub-scenes while they load.",
			ObjectSchema({ { "scene", AssetSchema("Scene asset to load into the running scene") } }, { "scene" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "scene", AssetType::Scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPlaying())
					return EditorCommandResult::Fail("Sub-scenes load into a running game: start play mode first (play.start)");
				return LoadSubSceneAndWait(context, handle);
			} }));

		registry.Register({ "scene.unloadAdditive",
			"Removes a sub-scene from the running game (play mode only): stops its load, releasing what it held loaded, and takes its entities out "
			"of the game at once (inactive: no rendering, scripts' updates, physics or audio); they are destroyed with their descendants over the "
			"following frames (at most the scene's maxEntitiesDestroyedPerFrame a frame; their scripts get OnDestroy; scene.loadStatus shows the "
			"sub-scene Unloading with destroyedEntities until its last entity is gone). Returns {subScene, entities (the running scene's count now), "
			"unloadingEntities (those still to destroy)}.",
			ObjectSchema({ { "subScene", IntegerSchema("The sub-scene's number (scene.loadAdditive, scene.loadStatus)", 1, std::numeric_limits<int64_t>::max()) } },
				{ "subScene" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				if (!arguments.Has("subScene"))
					arguments.SetError("Missing parameter 'subScene'");
				const int64_t id = arguments.GetInt("subScene", 0, 1, std::numeric_limits<int64_t>::max());
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPlaying())
					return EditorCommandResult::Fail("Sub-scenes exist in a running game only: start play mode first (play.start)");
				Scene& scene = *context.GetActiveScene();
				if (!scene.GetSubSceneStatus(static_cast<SubSceneID>(id)))
					return EditorCommandResult::InvalidParameters(fmt::format("The running scene has no sub-scene {} (see scene.loadStatus)", id));
				scene.UnloadSubScene(static_cast<SubSceneID>(id));
				size_t unloading = 0;
				for (const Entity root : scene.GetSubSceneRoots(static_cast<SubSceneID>(id)))
					unloading += scene.CountSubtree(root);
				// Only the running game has sub-scenes: no warning that play.stop discards this; replays leave it out.
				EditorCommandResult result = EditorCommandResult::Ok({ { "subScene", id }, { "entities", scene.GetEntityCount() },
					{ "unloadingEntities", unloading } });
				result.RunningCopy = true;
				return result;
			} });

		registry.Register(MayDefer({ "scene.save",
			"Saves the edited scene to its asset in the background and answers once the file is written (wait false: at once). The frame "
			"only takes a snapshot of the scene, the document is written on a background thread, and edits made meanwhile stay unsaved (the "
			"save holds the scene as it was when the command ran). With wait (default true) the answer comes once the file is written and "
			"published: {scene, path, bytes, entities, seconds, frames, "
			"frameTimes (the editor's frames meanwhile: count, medianMs, meanMs, p95Ms, maxMs), memory {privateMBBefore, peakPrivateMBDuring}}. "
			"With wait false it answers at once with {scene, path, saveId, state: Writing | Queued}; editor.status's saving lists the saves "
			"in flight. A save queues behind one that writes. New scenes need scene.saveAs first. A world's persistent scene saves "
			"the documents changed since the last save, in the background too: the persistent scene (without the cells' entities), each dirty "
			"cell and then the world's document with those cells' index entries; documents lists them ({path, kind: persistent, cell or world, "
			"bytes, entities}; bytes and entities are their sums), and a world's save waits for its previous one.",
			ObjectSchema({ { "wait", BoolSchema("Answer once the document is written (default true); false answers at once with the save's saveId") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool wait = arguments.GetBool("wait", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				const auto start = [wait](EditorContext& context, const nlohmann::json&)
				{
					return StartSceneSave(context, wait, [](EditorContext& context, std::string* outError) { return context.BeginSaveScene(outError); });
				};
				// A world saves once its previous save ended (its documents and its index are written by one save at a time).
				if (context.GetWorld() && context.GetWorld()->IsSaving())
					return AfterSaves(start)(context, parameters);
				return start(context, parameters);
			} }));

		registry.Register(MayDefer({ "scene.saveAs",
			"Saves the edited scene as a new .stscene asset and continues editing it: the scene takes the file's name and the new asset at once (a "
			"following scene.save writes it), and gets the previous ones back when the save fails. The save runs in the background like scene.save's "
			"and answers like it (wait, default true: once written; false: at once with its saveId).",
			ObjectSchema({ { "path", StringSchema("Path relative to the asset directory, e.g. \"Scenes/Level1.stscene\"") },
				{ "wait", BoolSchema("Answer once the document is written (default true); false answers at once with the save's saveId") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				const bool wait = arguments.GetBool("wait", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				return StartSceneSave(context, wait, [path](EditorContext& context, std::string* outError) { return context.BeginSaveSceneAs(path, outError); });
			} }));

		nlohmann::json capacitySchema = ObjectSchema({
			{ "maxBodies", IntegerSchema("Bodies the physics world has room for when play starts (each about 2 KiB); 0 sizes it from the bodies the "
				"scene starts with. The world grows beyond it while bodies are created", 0, PhysicsCapacitySettings::c_MaxBodiesLimit) },
			{ "maxContactConstraints", IntegerSchema("Contacts the physics world has room for when play starts (each about 250 bytes); 0: automatic", 0,
				PhysicsCapacitySettings::c_MaxContactConstraintsLimit) },
			// Accepted with any value and ignored with a warning (ReadPhysicsCapacity), so that 0.4.0's command scripts keep
			// working; listed so that clients validating requests against the schema send them too.
			{ "maxBodyPairs", { { "deprecated", true },
				{ "description", "Ignored since 0.5.0 (box3d has no buffer of body pairs to size); accepted for older command scripts, with a warning" } } } });
		capacitySchema["description"] = "Physics world room (only the given values change), reserved when play starts (up to 131072 bodies and "
			"262144 contacts at once; beyond that it counts for growth only); raise it for scenes that create many bodies while they play, so "
			"that the world does not grow during play";
		const nlohmann::json layerNamesSchema = {
			{ "type", "object" },
			{ "description", R"(Names of collision layers by index ("0" to "31"), e.g. {"1": "Player", "2": "Debris"}; "" or null clears )"
				"a name. Names are unique (ignoring case), at most 64 characters, and RigidBody.Layer, CharacterController.Layer and "
				"layerCollisions accept them" },
			{ "propertyNames", { { "pattern", "^([0-9]|[1-2][0-9]|3[0-1])$" } } },
			{ "additionalProperties", { { "type", nlohmann::json::array({ "string", "null" }) } } } };
		const nlohmann::json layerCollisionsSchema = {
			{ "type", "array" },
			{ "description", "Changes of the layer matrix, applied in order after layerNames: whether two layers (indices 0-31 or names) collide. "
				"Every pair collides by default; bodies and character controllers collide only when their layers do" },
			{ "items", {
				{ "type", "object" },
				{ "properties", {
					{ "layers", { { "type", "array" }, { "minItems", 2 }, { "maxItems", 2 }, { "description", "The two layers (the same twice: a layer with itself)" },
						{ "items", { { "type", nlohmann::json::array({ "integer", "string" }) } } } } },
					{ "collide", BoolSchema("Whether they collide") } } },
				{ "required", nlohmann::json::array({ "layers", "collide" }) },
				{ "additionalProperties", false } } } };
		registry.Register({ "scene.setSettings",
			"Changes settings of the edited scene: gravity, the fixed step, the physics world's capacities, the collision layers' names and "
			"matrix, and how many entities additive loads (sub-scenes) create per frame (undoable, one step). Capacities take effect when a "
			"physics world is created, at the next play start, so set them in edit mode; the layer matrix applies at once, also to a running "
			"game. While playing, changes go to the running copy only and play.stop discards them (the result then has a warning). Returns the "
			"settings (scene.info shows them too).",
			ObjectSchema({
				{ "gravity", Vec3Schema("Gravity in m/s^2") },
				{ "fixedTimestep", NumberSchema("Fixed simulation step in seconds (1/1000 to 1/10)") },
				{ "maxFixedStepsPerFrame", IntegerSchema("Upper bound of fixed steps per frame", 1, 64) },
				{ "maxEntitiesInstantiatedPerFrame", IntegerSchema("Entities sub-scenes loading into the scene (scene.loadAdditive, scripts' "
					"Scene::LoadAdditive) may create per frame, all together (default 5000); each frame also stops creating after about 2 ms", 1,
					SceneSettings::c_MaxEntitiesInstantiatedLimit) },
				{ "maxEntitiesDestroyedPerFrame", IntegerSchema("Entities sub-scenes being unloaded (scene.unloadAdditive, scripts' SubScene::Unload, a "
					"world's cells) may destroy per frame, all together (default 5000); creation and destruction together stop after about 2 ms a frame",
					1, SceneSettings::c_MaxEntitiesDestroyedLimit) },
				{ "physicsCapacity", std::move(capacitySchema) },
				{ "layerNames", layerNamesSchema },
				{ "resetLayerCollisions", BoolSchema("Lets every pair of layers collide again before layerCollisions applies") },
				{ "layerCollisions", layerCollisionsSchema } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				const SceneSettings before = scene.GetSettings();
				SceneSettings after = before;
				if (auto gravity = parameters.find("gravity"); gravity != parameters.end())
				{
					if (!gravity->is_array() || gravity->size() != 3 || !(*gravity)[0].is_number() || !(*gravity)[1].is_number() || !(*gravity)[2].is_number())
						return EditorCommandResult::InvalidParameters("Parameter 'gravity' must be an array of three numbers");
					after.Gravity = { (*gravity)[0].get<float>(), (*gravity)[1].get<float>(), (*gravity)[2].get<float>() };
					if (!std::isfinite(after.Gravity.x) || !std::isfinite(after.Gravity.y) || !std::isfinite(after.Gravity.z))
						return EditorCommandResult::InvalidParameters("Parameter 'gravity' must be finite");
				}
				if (auto step = parameters.find("fixedTimestep"); step != parameters.end())
				{
					if (!step->is_number() || !(step->get<float>() >= 0.001f && step->get<float>() <= 0.1f))
						return EditorCommandResult::InvalidParameters("Parameter 'fixedTimestep' must be between 0.001 and 0.1");
					after.FixedTimestep = step->get<float>();
				}
				std::string capacityWarning;
				if (auto capacity = parameters.find("physicsCapacity"); capacity != parameters.end())
				{
					if (std::string error = ReadPhysicsCapacity(*capacity, after.PhysicsCapacity, capacityWarning); !error.empty())
						return EditorCommandResult::InvalidParameters(error);
				}
				if (auto names = parameters.find("layerNames"); names != parameters.end())
				{
					if (std::string error = ReadLayerNames(*names, after.PhysicsLayers); !error.empty())
						return EditorCommandResult::InvalidParameters(error);
				}
				CommandArguments arguments(parameters);
				after.MaxFixedStepsPerFrame = static_cast<uint32_t>(arguments.GetInt("maxFixedStepsPerFrame", before.MaxFixedStepsPerFrame, 1, 64));
				after.MaxEntitiesInstantiatedPerFrame = static_cast<uint32_t>(arguments.GetInt("maxEntitiesInstantiatedPerFrame", before.MaxEntitiesInstantiatedPerFrame,
					1, SceneSettings::c_MaxEntitiesInstantiatedLimit));
				after.MaxEntitiesDestroyedPerFrame = static_cast<uint32_t>(arguments.GetInt("maxEntitiesDestroyedPerFrame", before.MaxEntitiesDestroyedPerFrame,
					1, SceneSettings::c_MaxEntitiesDestroyedLimit));
				if (arguments.GetBool("resetLayerCollisions", false))
					after.PhysicsLayers.ResetMatrix();
				if (!arguments.IsValid())
					return arguments.Fail();
				if (auto collisions = parameters.find("layerCollisions"); collisions != parameters.end())
				{
					if (std::string error = ReadLayerCollisions(*collisions, after.PhysicsLayers); !error.empty())
						return EditorCommandResult::InvalidParameters(error);
				}
				if (std::string error; !context.CheckSceneEditable(&error))
					return EditorCommandResult::Fail(std::move(error));

				const bool changed = after.Gravity != before.Gravity || after.FixedTimestep != before.FixedTimestep
					|| after.MaxFixedStepsPerFrame != before.MaxFixedStepsPerFrame || after.PhysicsCapacity != before.PhysicsCapacity
					|| after.MaxEntitiesInstantiatedPerFrame != before.MaxEntitiesInstantiatedPerFrame
					|| after.MaxEntitiesDestroyedPerFrame != before.MaxEntitiesDestroyedPerFrame || after.PhysicsLayers != before.PhysicsLayers;
				scene.GetSettings() = after;
				if (changed && !context.IsPlaying())
					context.GetUndoStack().Record(CreateScope<SceneSettingsAction>(scene, before, after));
				nlohmann::json result = { { "settings", DescribeSettings(after) } };
				if (context.IsPlaying())
				{
					// Like the other mutating commands; capacities do not even reach the running copy's world, which exists already.
					std::string warning = GetRunningCopyWarning(context);
					if (after.PhysicsCapacity != before.PhysicsCapacity)
						warning += "; physics capacities apply to worlds created later, not to the running one, so set them in edit mode";
					if (!capacityWarning.empty())
						warning += "; " + capacityWarning;
					result["warning"] = std::move(warning);
				}
				else if (!capacityWarning.empty())
				{
					result["warning"] = std::move(capacityWarning);
				}
				EditorCommandResult outcome = EditorCommandResult::Ok(std::move(result));
				outcome.RunningCopy = context.IsPlaying();
				return outcome;
			} });
	}

}
