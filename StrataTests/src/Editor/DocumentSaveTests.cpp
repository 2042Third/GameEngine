#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/DocumentSaver.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "Editor/SaveTestUtils.h"
#include "Editor/WorldSaveMeasurement.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Events/ApplicationEvent.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneDocumentWriter.h>
#include <Strata/Scene/SceneSerializer.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct SaverJobSystem
	{
		SaverJobSystem()
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = 4;
			specification.IOThreadCount = 2;
			specification.BackgroundThreadCount = 2;
			JobSystem::Init(specification);
		}

		~SaverJobSystem()
		{
			JobSystem::Shutdown();
		}

		SaverJobSystem(const SaverJobSystem&) = delete;
		SaverJobSystem& operator=(const SaverJobSystem&) = delete;
	};

	// A project folder with an asset manager over it.
	struct SaveProject
	{
		std::filesystem::path Root;
		std::filesystem::path Assets;
		Ref<EditorAssetManager> Manager;

		explicit SaveProject(const std::string& name, bool watchFiles = false)
		{
			Root = CreateTemporaryDirectory(name);
			Assets = Root / "Assets";
			REQUIRE(FileSystem::CreateDirectories(Assets));
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = Assets;
			specification.CacheDirectory = Root / ".strata" / "Cache";
			specification.WatchFiles = watchFiles;
			Manager = CreateRef<EditorAssetManager>(specification);
			Manager->Scan();
		}

		// A scene document of the project made the old way (an existing asset to write over).
		AssetHandle CreateScene(const std::string& path, const std::string& name)
		{
			const Scene scene(name);
			const std::string text = JsonUtils::Dump(SceneSerializer::Serialize(scene));
			std::string error;
			const AssetHandle handle = Manager->CreateNativeAsset(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &error);
			INFO(error);
			REQUIRE(handle.IsValid());
			return handle;
		}
	};

	// Three groups, each a root with two children.
	struct GroupedScene
	{
		Ref<Scene> Edited = CreateRef<Scene>("Groups");
		std::vector<UUID> Roots;

		GroupedScene()
		{
			for (int group = 0; group < 3; group++)
			{
				Entity root = Edited->CreateEntity(fmt::format("Group {}", group));
				root.AddComponent<MeshRendererComponent>().Mesh = UUID(1 + static_cast<uint64_t>(group));
				Edited->CreateChildEntity(root, fmt::format("Child {}a", group));
				Edited->CreateChildEntity(root, fmt::format("Child {}b", group));
				Roots.push_back(root.GetUUID());
			}
		}

		// The IDs of a group's entities.
		std::set<UUID> GetGroup(size_t group) const
		{
			std::set<UUID> ids;
			const Entity root = Edited->GetEntityByUUID(Roots[group]);
			ids.insert(root.GetUUID());
			for (const Entity child : root.GetChildren())
				ids.insert(child.GetUUID());
			return ids;
		}
	};

	DocumentSaveItem SubsetItem(AssetHandle asset, UUID root, const std::string& newPath = {})
	{
		DocumentSaveItem item;
		item.Asset = asset;
		item.NewPath = newPath;
		item.Write.Roots = { root };
		return item;
	}

	// The entity IDs of a scene document on disk.
	std::set<UUID> ReadEntityIDs(const std::filesystem::path& path)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE(text.has_value());
		const std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document.has_value());
		Scene scene;
		std::string error;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(scene, *document, &error), error);
		std::set<UUID> ids;
		for (const auto& [entity, id] : scene.GetRegistry().view<IDComponent>().each())
			ids.insert(id.ID);
		return ids;
	}

	// Updates the saver (as frames do) until the request ended.
	DocumentSaveStatus RunUntilEnded(DocumentSaver& saver, uint64_t id)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (true)
		{
			saver.Update();
			const std::optional<DocumentSaveStatus> status = saver.GetStatus(id);
			REQUIRE(status.has_value());
			if (status->IsFinished())
				return *status;
			REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline, "The save did not end in time");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	std::string MaterialDocument(const glm::vec4& color)
	{
		MaterialProperties properties;
		properties.BaseColor = color;
		return JsonUtils::Dump(Material::Create(properties)->Serialize(), 1, '\t') + "\n";
	}


	// An editor with a project whose scene is saved as Scenes/Main.stscene.
	struct EditorSaves
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;

		explicit EditorSaves(const std::string& name)
			: Directory(CreateTemporaryDirectory(name))
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory / "Game") }, { "name", "Saves" } });
			Run("entity.create", { { "name", "First" } });
			Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		// Updates the editor until no save is in flight.
		void FinishSaves()
		{
			REQUIRE(WaitUntil([&]()
			{
				Context.Update(Timestep(1.0f / 60.0f));
				return !Context.HasSavesInFlight();
			}, std::chrono::minutes(1)));
		}

		std::filesystem::path GetSceneFile() const { return Context.GetSceneFile(Context.GetSceneHandle()).first; }

		// The names of the entities the scene's file holds.
		std::set<std::string> ReadSavedNames() const
		{
			std::string error;
			const Ref<Scene> saved = SceneSerializer::LoadFromFile(GetSceneFile(), &error);
			REQUIRE_MESSAGE(saved, error);
			std::set<std::string> names;
			for (const auto [entity, name] : saved->GetRegistry().view<NameComponent>().each())
				names.insert(name.Name);
			return names;
		}
	};

}

TEST_SUITE("Editor.Saves")
{
	TEST_CASE("One request writes several documents of one snapshot, new ones with their reserved handles, with one completion")
	{
		SaverJobSystem jobs;
		SaveProject project("SaverMulti");
		const AssetHandle existing = project.CreateScene("Scenes/Existing.stscene", "Existing");
		const GroupedScene groups;

		std::string error;
		const AssetHandle given = UUID(0x00C0FFEE00001234ull);
		REQUIRE(project.Manager->ReserveNativeAsset("Cells/B.stscene", given, &error) == given);
		const AssetHandle fresh = project.Manager->ReserveNativeAsset("Cells/C.stscene", UUID::Null(), &error);
		REQUIRE(fresh.IsValid());
		const AssetHandle material = project.Manager->ReserveNativeAsset("Materials/Index.stmat", UUID::Null(), &error);
		REQUIRE(material.IsValid());
		// Reserved places and handles are nobody else's.
		CHECK_FALSE(project.Manager->ReserveNativeAsset("Cells/B.stscene", UUID::Null(), &error).IsValid());
		CHECK_FALSE(project.Manager->ReserveNativeAsset("Cells/Other.stscene", given, &error).IsValid());
		CHECK_FALSE(project.Manager->ReserveNativeAsset("Scenes/Existing.stscene", UUID::Null(), &error).IsValid());
		CHECK_FALSE(project.Manager->ReserveNativeAsset("../Outside.stscene", UUID::Null(), &error).IsValid());
		CHECK_FALSE(project.Manager->ReserveNativeAsset("Textures/Image.png", UUID::Null(), &error).IsValid());

		DocumentSaver saver;
		saver.SetAssetManager(project.Manager);
		DocumentSaveRequest request;
		request.Snapshot = Scene::Copy(groups.Edited, SceneCopyOptions { true });
		request.Label = "Groups";
		request.Items.push_back(SubsetItem(existing, groups.Roots[0]));
		request.Items.push_back(SubsetItem(given, groups.Roots[1], "Cells/B.stscene"));
		request.Items.push_back(SubsetItem(fresh, groups.Roots[2], "Cells/C.stscene"));
		DocumentSaveItem document;
		document.Asset = material;
		document.NewPath = "Materials/Index.stmat";
		document.Document = MaterialDocument(glm::vec4(0.5f, 0.25f, 1.0f, 1.0f));
		request.Items.push_back(document);

		int completions = 0;
		const uint64_t id = saver.Submit(std::move(request), [&completions](const DocumentSaveStatus& status)
		{
			completions++;
			CHECK(status.State == DocumentSaveState::Done);
		}, &error);
		INFO(error);
		REQUIRE(id != 0);
		CHECK(saver.IsSaving(existing));
		CHECK(saver.IsSaving(material));
		const DocumentSaveStatus status = RunUntilEnded(saver, id);
		CHECK(completions == 1);
		CHECK(status.State == DocumentSaveState::Done);
		CHECK(status.WrittenID == id);
		CHECK(status.Label == "Groups");
		REQUIRE(status.Items.size() == 4);
		CHECK_FALSE(saver.IsBusy());
		CHECK_FALSE(saver.IsSaving(existing));

		// Each document holds exactly its group.
		CHECK(ReadEntityIDs(project.Assets / "Scenes" / "Existing.stscene") == groups.GetGroup(0));
		CHECK(ReadEntityIDs(project.Assets / "Cells" / "B.stscene") == groups.GetGroup(1));
		CHECK(ReadEntityIDs(project.Assets / "Cells" / "C.stscene") == groups.GetGroup(2));
		CHECK(FileSystem::ReadText(project.Assets / "Materials" / "Index.stmat") == document.Document);
		for (size_t index = 0; index < 3; index++)
		{
			CAPTURE(index);
			CHECK(status.Items[index].Error.empty());
			CHECK(status.Items[index].Entities == 3);
			CHECK(status.Items[index].References == std::vector<AssetHandle> { UUID(1 + index) });
			CHECK(status.Items[index].Bytes == FileSystem::GetFileSize(project.Assets / FileSystem::FromUTF8(status.Items[index].Path)));
		}

		// The new assets are registered with their reserved handles, without a scan, and their .meta files hold them.
		CHECK(project.Manager->FindAssetByPath("Cells/B.stscene") == given);
		CHECK(project.Manager->FindAssetByPath("Cells/C.stscene") == fresh);
		CHECK(project.Manager->FindAssetByPath("Materials/Index.stmat") == material);
		CHECK(project.Manager->GetAssetType(given) == AssetType::Scene);
		CHECK(project.Manager->GetAssetType(material) == AssetType::Material);
		CHECK(project.Manager->GetReservedPath(given).empty());
		CHECK(project.Manager->HasFolder("Cells"));
		const std::optional<std::string> meta = FileSystem::ReadText(project.Assets / "Cells" / "B.stscene.meta");
		REQUIRE(meta.has_value());
		CHECK(UUIDFromJson((*JsonUtils::Parse(*meta))["Handle"]) == given);
		// What they reference is recorded.
		CHECK(project.Manager->GetDependencies(given) == std::vector<AssetHandle> { UUID(2) });
		CHECK(project.Manager->GetDependencies(existing) == std::vector<AssetHandle> { UUID(1) });

		// A scan of the project afterwards finds the same assets with the same handles.
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = project.Assets;
		specification.CacheDirectory = project.Root / ".strata" / "Cache";
		specification.WatchFiles = false;
		const Ref<EditorAssetManager> rescanned = CreateRef<EditorAssetManager>(specification);
		rescanned->Scan();
		CHECK(rescanned->FindAssetByPath("Cells/B.stscene") == given);
		CHECK(rescanned->FindAssetByPath("Materials/Index.stmat") == material);
		CHECK(rescanned->GetDependencies(given) == std::vector<AssetHandle> { UUID(2) });
	}

	TEST_CASE("An item that fails fails the request naming it, and the other items are written")
	{
		SaverJobSystem jobs;
		SaveProject project("SaverFailure");
		const AssetHandle existing = project.CreateScene("Scenes/Existing.stscene", "Existing");
		const GroupedScene groups;
		// The folder of the new document is a file: its file cannot be created.
		REQUIRE(FileSystem::WriteText(project.Assets / "Blocked", "not a folder"));
		std::string error;
		const AssetHandle blocked = project.Manager->ReserveNativeAsset("Blocked/Cell.stscene", UUID::Null(), &error);
		REQUIRE(blocked.IsValid());
		const AssetHandle invalid = project.Manager->ReserveNativeAsset("Materials/Broken.stmat", UUID::Null(), &error);
		REQUIRE(invalid.IsValid());

		DocumentSaver saver;
		saver.SetAssetManager(project.Manager);
		DocumentSaveRequest request;
		request.Snapshot = Scene::Copy(groups.Edited, SceneCopyOptions { true });
		request.Items.push_back(SubsetItem(blocked, groups.Roots[1], "Blocked/Cell.stscene"));
		request.Items.push_back(SubsetItem(existing, groups.Roots[0]));
		DocumentSaveItem broken;
		broken.Asset = invalid;
		broken.Document = "{ not a material";
		request.Items.push_back(broken);
		const uint64_t id = saver.Submit(std::move(request), {}, &error);
		REQUIRE(id != 0);
		const DocumentSaveStatus status = RunUntilEnded(saver, id);
		CHECK(status.State == DocumentSaveState::Failed);
		CHECK(status.Error.find("Blocked/Cell.stscene") != std::string::npos);
		CHECK(status.Error.find("Materials/Broken.stmat") != std::string::npos);
		CHECK(status.Error.find("Existing") == std::string::npos);
		REQUIRE(status.Items.size() == 3);
		CHECK_FALSE(status.Items[0].Error.empty());
		CHECK(status.Items[1].Error.empty());
		CHECK(status.Items[2].Error.find("Invalid Material data") != std::string::npos);
		CHECK(ReadEntityIDs(project.Assets / "Scenes" / "Existing.stscene") == groups.GetGroup(0));
		// Failed new documents leave nothing behind and give their places back.
		CHECK_FALSE(project.Manager->FindAssetByPath("Blocked/Cell.stscene").IsValid());
		CHECK_FALSE(FileSystem::Exists(project.Assets / "Materials" / "Broken.stmat"));
		CHECK_FALSE(FileSystem::Exists(project.Assets / "Materials" / "Broken.stmat.meta"));
		CHECK(project.Manager->GetReservedPath(invalid).empty());
		CHECK(project.Manager->ReserveNativeAsset("Materials/Broken.stmat", invalid, &error) == invalid);

		// Requests that cannot be saved are refused at once.
		DocumentSaveRequest empty;
		CHECK(saver.Submit(std::move(empty), {}, &error) == 0);
		DocumentSaveRequest unknown;
		unknown.Snapshot = Scene::Copy(groups.Edited, SceneCopyOptions { true });
		unknown.Items.push_back(SubsetItem(UUID(424242), groups.Roots[0]));
		CHECK(saver.Submit(std::move(unknown), {}, &error) == 0);
		CHECK(error.find("not reserved") != std::string::npos);
		DocumentSaveRequest noSnapshot;
		noSnapshot.Items.push_back(SubsetItem(existing, groups.Roots[0]));
		CHECK(saver.Submit(std::move(noSnapshot), {}, &error) == 0);
		DocumentSaver withoutProject;
		DocumentSaveRequest anything;
		anything.Items.push_back(SubsetItem(existing, groups.Roots[0]));
		CHECK(withoutProject.Submit(std::move(anything), {}, &error) == 0);
	}

	TEST_CASE("A second save queues behind the first, and a queued save of the same documents is replaced by the newer one")
	{
		SaverJobSystem jobs;
		SaveProject project("SaverQueue");
		const AssetHandle scene = project.CreateScene("Scenes/Queued.stscene", "Queued");
		DocumentSaver saver;
		saver.SetAssetManager(project.Manager);

		// Three versions of the scene: one entity, two, three.
		const Ref<Scene> edited = CreateRef<Scene>("Queued");
		auto submit = [&](std::vector<uint64_t>& calls, uint64_t serial)
		{
			edited->CreateEntity("Entity");
			DocumentSaveRequest request;
			request.Snapshot = Scene::Copy(edited, SceneCopyOptions { true });
			request.SavePoint.Serial = serial;
			request.Label = "Queued";
			DocumentSaveItem item;
			item.Asset = scene;
			request.Items.push_back(item);
			std::string error;
			const uint64_t id = saver.Submit(std::move(request), [&calls](const DocumentSaveStatus& status) { calls.push_back(status.WrittenID); }, &error);
			REQUIRE(id != 0);
			return id;
		};
		std::vector<uint64_t> first;
		std::vector<uint64_t> second;
		std::vector<uint64_t> third;
		const uint64_t a = submit(first, 1);
		saver.Update(); // A writes
		const uint64_t b = submit(second, 2);
		const uint64_t c = submit(third, 3);
		CHECK(saver.GetStatus(b)->State == DocumentSaveState::Queued);
		CHECK(saver.GetStatus(b)->WrittenID == c);
		CHECK(saver.GetActiveStatuses().size() == 2);

		const DocumentSaveStatus statusA = RunUntilEnded(saver, a);
		CHECK(statusA.SavePoint.Serial == 1);
		const DocumentSaveStatus statusB = RunUntilEnded(saver, b);
		const DocumentSaveStatus statusC = RunUntilEnded(saver, c);
		CHECK(first == std::vector<uint64_t> { a });
		CHECK(second == std::vector<uint64_t> { c });
		CHECK(third == std::vector<uint64_t> { c });
		CHECK(statusB.SavePoint.Serial == 3);
		CHECK(statusC.SavePoint.Serial == 3);
		CHECK(statusB.State == DocumentSaveState::Done);
		// The newest version is on disk.
		CHECK(ReadEntityIDs(project.Assets / "Scenes" / "Queued.stscene").size() == 3);
	}

	TEST_CASE("The file watcher neither searches nor reloads a document its own save wrote")
	{
		SaverJobSystem jobs;
		SaveProject project("SaverWatcher", true);
		const AssetHandle scene = project.CreateScene("Scenes/Watched.stscene", "Watched");
		const GroupedScene groups;
		const uint64_t revision = project.Manager->GetSourceRevisions(scene).Current;
		DocumentSaver saver;
		saver.SetAssetManager(project.Manager);
		DocumentSaveRequest request;
		request.Snapshot = Scene::Copy(groups.Edited, SceneCopyOptions { true });
		request.Items.push_back(SubsetItem(scene, groups.Roots[0]));
		std::string error;
		const uint64_t id = saver.Submit(std::move(request), {}, &error);
		REQUIRE(id != 0);
		saver.Update();
		REQUIRE(WaitUntil([&]() { return saver.GetStatus(id)->State == DocumentSaveState::Publishing; }, std::chrono::seconds(30)));

		// The file was replaced but not published yet: the watcher reports it meanwhile. A folder made after it shows when
		// the watcher has caught up.
		REQUIRE(FileSystem::CreateDirectories(project.Assets / "Sentinel"));
		bool searched = false;
		CHECK(WaitUntil([&]()
		{
			project.Manager->Update();
			searched = searched || project.Manager->HasPendingImports();
			return project.Manager->HasFolder("Sentinel");
		}, std::chrono::seconds(15)));
		CHECK_FALSE(searched);
		CHECK(project.Manager->GetSourceRevisions(scene).Current == revision);
		const DocumentSaveStatus status = RunUntilEnded(saver, id);
		CHECK(status.State == DocumentSaveState::Done);
		// After the publication the reports of the write are no news either.
		const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (std::chrono::steady_clock::now() < settle)
		{
			project.Manager->Update();
			searched = searched || project.Manager->HasPendingImports();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		CHECK_FALSE(searched);
		CHECK(project.Manager->GetSourceRevisions(scene).Current == revision);
		CHECK(project.Manager->GetDependencies(scene) == std::vector<AssetHandle> { UUID(1) });
	}

	TEST_CASE("The frame that starts a save serializes nothing: a snapshot is all it takes")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveFrame");
		// About 50,000 entities.
		std::string error;
		REQUIRE(BuildWorldScene(*editor.Context.GetEditScene(), { "Large", 20 * c_WorldDistrictCubes, false }, {}, error));
		REQUIRE(editor.Context.GetEditScene()->GetEntityCount() > 50000);

		const uint64_t serialized = SceneDocumentWriter::GetThreadEntityCount();
		const EditorCommandResult started = editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } });
		REQUIRE(started.Success);
		CHECK_FALSE(started.IsPending());
		CHECK(started.Value["state"] == "Writing");
		CHECK(started.Value["path"] == "Scenes/Main.stscene");
		const uint64_t id = started.Value["saveId"].get<uint64_t>();
		// The frame took the snapshot only; the job writes.
		CHECK(SceneDocumentWriter::GetThreadEntityCount() == serialized);
		const nlohmann::json status = editor.Run("editor.status");
		REQUIRE(status["saving"].size() == 1);
		CHECK(status["saving"][0]["saveId"] == id);
		CHECK(status["saving"][0]["label"] == "Main");
		CHECK(status["saving"][0]["documents"] == 1);
		editor.FinishSaves();
		CHECK(SceneDocumentWriter::GetThreadEntityCount() == serialized);
		CHECK(editor.Run("editor.status")["saving"].empty());
		CHECK(editor.Context.GetDocumentSaver().GetStatus(id)->Items.front().Entities == editor.Context.GetEditScene()->GetEntityCount());
		CHECK_FALSE(editor.Context.IsSceneModified());
	}

	TEST_CASE("A save that is waited for answers with what it wrote and the frames meanwhile")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveWait");
		editor.Run("entity.create", { { "name", "Second" } });
		const nlohmann::json saved = editor.Run("scene.save");
		CHECK(saved["path"] == "Scenes/Main.stscene");
		CHECK(saved["entities"].get<uint64_t>() == editor.Context.GetEditScene()->GetEntityCount());
		CHECK(saved["bytes"].get<uint64_t>() == FileSystem::GetFileSize(editor.GetSceneFile()));
		CHECK(saved["frames"].get<uint64_t>() >= 1);
		CHECK(saved["seconds"].get<double>() > 0.0);
		CHECK(saved["memory"]["peakPrivateMBDuring"].get<double>() >= saved["memory"]["privateMBBefore"].get<double>());
		CHECK(editor.ReadSavedNames() == std::set<std::string> { "First", "Second" });
		// The file has the streamed layout, an entity per line.
		const std::optional<std::string> text = FileSystem::ReadText(editor.GetSceneFile());
		REQUIRE(text.has_value());
		CHECK(text->find("\"EntityCount\":2,\"Entities\":[\n") != std::string::npos);
	}

	TEST_CASE("Edits made while a save writes stay unsaved, and a continuous edit begun before it does not merge into the saved step")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveEdits");
		Entity first = editor.Context.GetEditScene()->FindEntityByName("First");
		const ComponentInfo* transform = ComponentRegistry::Find("Transform");
		REQUIRE(transform);
		const PropertyInfo* translation = transform->FindProperty("Translation");
		REQUIRE(translation);

		// A drag goes on across the save: its frames before the save are saved, those after are a step of their own.
		REQUIRE(SetPropertyWithUndo(editor.Context, first, *transform, *translation, glm::vec3(1.0f, 0.0f, 0.0f)));
		REQUIRE(editor.Context.IsSceneModified());
		{
			BackgroundGate gate;
			REQUIRE(editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } }).Success);
			REQUIRE(SetPropertyWithUndo(editor.Context, first, *transform, *translation, glm::vec3(2.0f, 0.0f, 0.0f)));
			editor.Run("entity.create", { { "name", "During" } });
			editor.Context.Update(Timestep(1.0f / 60.0f));
			CHECK(editor.Context.HasSavesInFlight());
		}
		editor.FinishSaves();
		// The file holds the scene as the save found it.
		CHECK(editor.ReadSavedNames() == std::set<std::string> { "First" });
		CHECK(editor.Context.IsSceneModified());
		editor.Run("edit.undo"); // "During"
		CHECK(editor.Context.IsSceneModified());
		editor.Run("edit.undo"); // The drag's frame after the save
		CHECK_FALSE(editor.Context.IsSceneModified());
		CHECK(first.GetComponent<TransformComponent>().Translation.x == doctest::Approx(1.0f));
	}

	TEST_CASE("Commands that replace the scene, close the project or quit wait for a save in flight, and lose nothing")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveReplace");
		editor.Run("entity.create", { { "name", "Second" } });

		// scene.new without force waits for the save, after which nothing is unsaved: it needs no force.
		{
			BackgroundGate gate;
			REQUIRE(editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } }).Success);
			EditorCommandResult replaced = editor.Commands.Execute(editor.Context, "scene.new");
			REQUIRE(replaced.IsPending());
			editor.Context.Update(Timestep(1.0f / 60.0f));
			REQUIRE_FALSE(PollCommandResult(replaced, editor.Context));
			CHECK(editor.Context.GetEditScene()->GetName() == "Main");
			gate.Open();
			replaced = CompleteCommand(editor.Context, std::move(replaced));
			INFO(replaced.Error);
			REQUIRE(replaced.Success);
			CHECK(replaced.Value["discardedChanges"] == false);
		}
		CHECK(editor.Context.GetEditScene()->GetName() == "Untitled");

		// Reopened and edited again: an edit made after the save's snapshot is still unsaved when the open decides.
		editor.Run("scene.open", { { "scene", "Scenes/Main.stscene" } });
		CHECK(editor.Context.GetEditScene()->FindEntityByName("Second"));
		editor.Run("entity.create", { { "name", "Third" } });
		{
			BackgroundGate gate;
			REQUIRE(editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } }).Success);
			editor.Run("entity.create", { { "name", "Fourth" } });
			EditorCommandResult open = editor.Commands.Execute(editor.Context, "scene.open", { { "scene", "Scenes/Main.stscene" } });
			REQUIRE(open.IsPending());
			gate.Open();
			open = CompleteCommand(editor.Context, std::move(open));
			CHECK_FALSE(open.Success);
			CHECK(open.Error.find("unsaved changes") != std::string::npos);
		}
		CHECK(editor.ReadSavedNames() == std::set<std::string> { "First", "Second", "Third" });

		// project.close and editor.quit, also forced, wait for the save; the file is written whatever they discard.
		{
			BackgroundGate gate;
			REQUIRE(editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } }).Success);
			EditorCommandResult quit = editor.Commands.Execute(editor.Context, "editor.quit", { { "force", true } });
			REQUIRE(quit.IsPending());
			editor.Context.Update(Timestep(1.0f / 60.0f));
			CHECK_FALSE(editor.Context.IsQuitRequested());
			gate.Open();
			quit = CompleteCommand(editor.Context, std::move(quit));
			REQUIRE(quit.Success);
			CHECK(editor.Context.IsQuitRequested());
			CHECK(quit.Value["discardedChanges"] == false);
		}
		CHECK(editor.ReadSavedNames() == std::set<std::string> { "First", "Second", "Third", "Fourth" });
		editor.Run("entity.create", { { "name", "Fifth" } });
		{
			BackgroundGate gate;
			REQUIRE(editor.Commands.Execute(editor.Context, "scene.save", { { "wait", false } }).Success);
			EditorCommandResult close = editor.Commands.Execute(editor.Context, "project.close", { { "force", true } });
			REQUIRE(close.IsPending());
			gate.Open();
			close = CompleteCommand(editor.Context, std::move(close));
			REQUIRE(close.Success);
			CHECK(close.Value["discardedChanges"] == false);
		}
		CHECK_FALSE(editor.Context.HasProject());
		std::string error;
		const Ref<Scene> saved = SceneSerializer::LoadFromFile(editor.Directory / "Game" / "Assets" / "Scenes" / "Main.stscene", &error);
		REQUIRE(saved);
		CHECK(saved->FindEntityByName("Fifth"));
	}

	TEST_CASE("A save whose scene was replaced meanwhile writes its file and marks nothing in the scene edited now")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveReplaced");
		editor.Run("entity.create", { { "name", "Saved" } });
		{
			BackgroundGate gate;
			std::string error;
			REQUIRE(editor.Context.BeginSaveScene(&error) != 0);
			// Replaced through the context directly, as only code (never a command) can while a save runs.
			REQUIRE(editor.Context.NewScene("Other"));
			editor.Run("entity.create", { { "name", "Unsaved" } });
			CHECK(editor.Context.IsSceneModified());
		}
		editor.FinishSaves();
		CHECK(editor.Context.GetEditScene()->GetName() == "Other");
		CHECK(editor.Context.IsSceneModified());
		std::string error;
		const Ref<Scene> saved = SceneSerializer::LoadFromFile(editor.Directory / "Game" / "Assets" / "Scenes" / "Main.stscene", &error);
		REQUIRE(saved);
		CHECK(saved->FindEntityByName("Saved"));
		CHECK_FALSE(saved->FindEntityByName("Unsaved"));
	}

	TEST_CASE("Saves queue behind the one that writes, a queued save of the scene is replaced by a newer one, and all of them answer")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveQueue");
		std::vector<EditorCommandResult> saves;
		{
			BackgroundGate gate;
			for (const char* name : { "A", "B", "C" })
			{
				editor.Run("entity.create", { { "name", name } });
				saves.push_back(editor.Commands.Execute(editor.Context, "scene.save"));
				REQUIRE(saves.back().IsPending());
			}
			const nlohmann::json saving = editor.Run("editor.status")["saving"];
			REQUIRE(saving.size() == 2); // The first writes; the third replaced the second
			CHECK(saving[0]["state"] == "Writing");
			CHECK(saving[1]["state"] == "Queued");
		}
		for (EditorCommandResult& save : saves)
		{
			const EditorCommandResult done = CompleteCommand(editor.Context, std::move(save));
			CHECK(done.Success);
		}
		CHECK(editor.ReadSavedNames() == std::set<std::string> { "First", "A", "B", "C" });
		CHECK_FALSE(editor.Context.IsSceneModified());
	}

	TEST_CASE("A failed save as a new asset gives the scene its previous handle and name back and says why")
	{
		GatedJobSystem jobs;
		EditorSaves editor("SaveAsFailure");
		const AssetHandle previous = editor.Context.GetSceneHandle();
		editor.Run("entity.create", { { "name", "Unsaved" } });
		// The new file's folder is a file: the save fails once it writes.
		REQUIRE(FileSystem::WriteText(editor.Context.GetProject()->GetAssetDirectory() / "Blocked", "not a folder"));
		EditorCommandResult result;
		{
			BackgroundGate gate;
			result = editor.Commands.Execute(editor.Context, "scene.saveAs", { { "path", "Blocked/Level.stscene" } });
			REQUIRE(result.IsPending());
			// At once, the scene is the new asset with the file's name.
			CHECK(editor.Context.GetSceneHandle() != previous);
			CHECK(editor.Context.GetEditScene()->GetName() == "Level");
		}
		result = CompleteCommand(editor.Context, std::move(result));
		CHECK_FALSE(result.Success);
		CHECK(result.Announced);
		CHECK(editor.Context.GetSceneHandle() == previous);
		CHECK(editor.Context.GetEditScene()->GetName() == "Main");
		CHECK(editor.Context.IsSceneModified());
		bool posted = false;
		for (const EditorNotification& notification : editor.Context.GetNotifications().GetActive())
			posted = posted || (notification.Level == NotificationLevel::Error && notification.Title.find("Saving 'Level' failed") != std::string::npos);
		CHECK(posted);
		// The scene still saves where it was.
		editor.Run("scene.save");
		CHECK(editor.ReadSavedNames().count("Unsaved") == 1);
		// Refused at once: no scene file, or a taken place.
		CHECK_FALSE(CompleteCommand(editor.Context, editor.Commands.Execute(editor.Context, "scene.saveAs", { { "path", "Materials/Not.stmat" } })).Success);
		CHECK_FALSE(CompleteCommand(editor.Context, editor.Commands.Execute(editor.Context, "scene.saveAs", { { "path", "Scenes/Main.stscene" } })).Success);
		CHECK(editor.Context.GetSceneHandle() == previous);
	}

	TEST_CASE("The perf gates' save of a world, measured in a helper process, reports what it wrote and the frames meanwhile")
	{
		const WorldSceneSpec world { "Small", 3000, true };
		std::optional<WorldProject> project;
		{
			SaverJobSystem jobs;
			std::string error;
			WorldProjectSpec spec;
			spec.Scenes.push_back(world);
			project = WriteWorldProject(CreateTemporaryDirectory("WorldSaveHelper") / "World", spec, error);
			REQUIRE_MESSAGE(project.has_value(), error);
		}
		std::string error;
		const std::optional<WorldSaveMeasurement> measurement = MeasureWorldSaveInHelper(project->Directory, project->Scenes.front().Handle, error);
		REQUIRE_MESSAGE(measurement.has_value(), error);
		// The world and the edit's entity, in the document the save wrote.
		CHECK(measurement->Entities == GetWorldEntityCount(world) + 1);
		const std::optional<std::string> text = FileSystem::ReadText(project->Scenes.front().Path);
		REQUIRE(text.has_value());
		CHECK(measurement->Bytes == text->size());
		CHECK(text->find("Saved Edit") != std::string::npos);
		CHECK(measurement->Frames >= 1);
		CHECK(measurement->MaxFrameMs >= measurement->SaveFrameMs);
		CHECK(measurement->Seconds * 1000.0 >= measurement->SaveFrameMs);
		CHECK(measurement->PrivateMBBefore > 0.0);
		CHECK(measurement->PeakPrivateIncreaseMB >= 0.0);
		CHECK(measurement->OpenSeconds > 0.0);

		// A scene the project lacks fails with the reason.
		CHECK_FALSE(MeasureWorldSaveInHelper(project->Directory, UUID(0x5C000000002FFFFFull), error).has_value());
		CHECK(error.find("scene.open") != std::string::npos);
	}
}

TEST_SUITE("Editor.UI.Saves")
{
	TEST_CASE("While a save writes, the status bar shows it, and replacing the scene waits in the saving dialog until it is written")
	{
		GatedJobSystem jobs;
		HarnessEditor editor({}, WithFeatureProject("SaveDialog"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Unsaved" } });
		REQUIRE(editor.Context().IsSceneModified());
		const std::string sceneName = editor.Context().GetEditScene()->GetName();
		// Idle: no pill.
		CHECK_FALSE(UI::ItemProbe::Find("Status.Saving").has_value());
		{
			BackgroundGate gate;
			// File > Save starts the save through the command runner.
			REQUIRE(editor.Click("Menu.File"));
			editor.Frames(1);
			REQUIRE(editor.Click("Menu.File.SaveScene"));
			editor.Frames(1);
			REQUIRE(editor.Context().HasSavesInFlight());
			const std::string pill = editor.GetPillText("Status.Saving");
			CHECK(pill.find("Saving '" + sceneName + "'") != std::string::npos);
			CHECK(pill.find('%') != std::string::npos);

			// File > New Scene waits for it in the saving dialog; Cancel closes the dialog and gives up the new scene, never the
			// save.
			REQUIRE(editor.Click("Menu.File"));
			editor.Frames(1);
			REQUIRE(editor.Click("Menu.File.NewScene"));
			editor.Frames(2);
			REQUIRE(UI::ItemProbe::Find("Saving.Dialog").has_value());
			REQUIRE(editor.Click("Saving.Cancel"));
			editor.Frames(2);
			CHECK_FALSE(UI::ItemProbe::Find("Saving.Dialog").has_value());
			CHECK(editor.Context().HasSavesInFlight());

			// Asked again, it waits until the save is written.
			REQUIRE(editor.Click("Menu.File"));
			editor.Frames(1);
			REQUIRE(editor.Click("Menu.File.NewScene"));
			editor.Frames(2);
			REQUIRE(UI::ItemProbe::Find("Saving.Dialog").has_value());
			CHECK(editor.Context().GetEditScene()->GetName() == sceneName);
		}
		// Written: the new scene follows without a question, the saved scene having nothing unsaved.
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetEditScene()->GetName() == "Untitled"; }));
		CHECK_FALSE(UI::ItemProbe::Find("UnsavedChanges.Save").has_value());
		CHECK_FALSE(editor.Context().HasSavesInFlight());
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Saving").has_value());
	}

	TEST_CASE("Closing the window while a save writes shows the saving dialog, also when the scene is unmodified meanwhile")
	{
		GatedJobSystem jobs;
		HarnessEditor editor({}, WithFeatureProject("SaveWindowClose"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Saved Then Undone" } });
		{
			BackgroundGate gate;
			editor.Run("scene.save", { { "wait", false } });
			// Undone back to the file's state: unmodified, while the save of the edit writes.
			editor.Run("edit.undo");
			REQUIRE_FALSE(editor.Context().IsSceneModified());
			REQUIRE(editor.Context().HasSavesInFlight());
			WindowCloseEvent close;
			editor.Layer->OnEvent(close);
			CHECK(close.Handled);
			editor.Frames(2);
			CHECK(UI::ItemProbe::Find("Saving.Dialog").has_value());
			CHECK(editor.Host->Running);
		}
		// Written: the file holds the entity the scene no longer has, and closing asks about that difference.
		REQUIRE(editor.FramesUntil([&]() { return UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value(); }));
		CHECK(editor.Host->Running);
		// A dialog that sizes itself to its contents settles over its first frames.
		editor.Frames(2);
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		editor.Frames(1);
		CHECK_FALSE(editor.Host->Running);
	}

	TEST_CASE("The unsaved-changes question's Save goes on with what was asked once the save is written")
	{
		GatedJobSystem jobs;
		HarnessEditor editor({}, WithFeatureProject("SaveQuestion"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Saved Before New" } });
		const std::filesystem::path sceneFile = editor.Context().GetSceneFile(editor.Context().GetSceneHandle()).first;
		{
			BackgroundGate gate;
			REQUIRE(editor.Click("Menu.File"));
			editor.Frames(1);
			REQUIRE(editor.Click("Menu.File.NewScene"));
			editor.Frames(2);
			REQUIRE(editor.Click("UnsavedChanges.Save"));
			editor.Frames(2);
			// Still writing: the scene stays until the save is done.
			CHECK(editor.Context().HasSavesInFlight());
			CHECK(editor.Context().GetEditScene()->FindEntityByName("Saved Before New"));
		}
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetEditScene()->GetName() == "Untitled"; }));
		const std::optional<std::string> text = FileSystem::ReadText(sceneFile);
		REQUIRE(text.has_value());
		CHECK(text->find("Saved Before New") != std::string::npos);
	}

	TEST_CASE("New Project waits over its dialog for a save an agent started while it was open, then creates the project")
	{
		GatedJobSystem jobs;
		const std::filesystem::path location = CreateTemporaryDirectory("SaveNewProject");
		HarnessEditor editor({}, WithFeatureProject("SaveNewProject"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		const std::filesystem::path sceneFile = editor.Context().GetSceneFile(editor.Context().GetSceneHandle()).first;
		editor.Run("entity.create", { { "name", "Saved Meanwhile" } });
		// The person chooses to discard the change and fills in the dialog.
		editor.Layer->ShowNewProjectDialog({});
		editor.Frames(2);
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		editor.Frames(3);
		REQUIRE(editor.Layer->GetProjectDialogs().IsOpen());
		REQUIRE(editor.ReplaceText("NewProject.Name", "After Save"));
		REQUIRE(editor.ReplaceText("ProjectDialogs.Location", FileSystem::ToUTF8(location)));
		editor.Frames(1);
		{
			BackgroundGate gate;
			// An agent saves the scene without waiting while the dialog is open: that save is not lost.
			editor.Run("scene.save", { { "wait", false } });
			REQUIRE(editor.Context().HasSavesInFlight());
			REQUIRE(editor.Click("NewProject.Create"));
			editor.Frames(2);
			// The saving dialog shows over the New Project dialog, which stays open; nothing is created yet.
			CHECK(UI::ItemProbe::Find("Saving.Dialog").has_value());
			CHECK(editor.Layer->GetProjectDialogs().IsOpen());
			CHECK_FALSE(std::filesystem::exists(location / "After Save"));
		}
		// Written: the project is created without another question (the saved scene has nothing unsaved).
		const std::filesystem::path projectFile = location / "After Save" / "After Save.stproj";
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetProject() && editor.Context().GetProject()->GetProjectFile() == projectFile; }));
		CHECK_FALSE(editor.Layer->GetProjectDialogs().IsOpen());
		const std::optional<std::string> text = FileSystem::ReadText(sceneFile);
		REQUIRE(text.has_value());
		CHECK(text->find("Saved Meanwhile") != std::string::npos);
	}

	TEST_CASE("A save that fails says so in a notification")
	{
		GatedJobSystem jobs;
		HarnessEditor editor({}, WithFeatureProject("SaveFailureToast"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("entity.create", { { "name", "Unsaved" } });
		// The scene's folder is a file by the time the save writes.
		const std::filesystem::path sceneFile = editor.Context().GetSceneFile(editor.Context().GetSceneHandle()).first;
		const std::filesystem::path folder = sceneFile.parent_path();
		const std::filesystem::path moved = folder.parent_path() / "MovedAway";
		{
			BackgroundGate gate;
			REQUIRE(editor.Click("Menu.File"));
			editor.Frames(1);
			REQUIRE(editor.Click("Menu.File.SaveScene"));
			editor.Frames(1);
			std::error_code error;
			std::filesystem::rename(folder, moved, error);
			REQUIRE_FALSE(error);
			REQUIRE(FileSystem::WriteText(folder, "not a folder"));
		}
		REQUIRE(editor.FramesUntil([&]() { return !editor.Context().HasSavesInFlight(); }));
		editor.Frames(2);
		bool toast = false;
		for (const EditorNotification& notification : editor.Context().GetNotifications().GetActive())
		{
			if (notification.Level == NotificationLevel::Error && notification.Title.find("Saving '") != std::string::npos)
			{
				toast = true;
				CHECK(notification.ShowConsole);
			}
		}
		CHECK(toast);
		// One toast: the command's own outcome was announced by the save's.
		size_t errors = 0;
		for (const EditorNotification& notification : editor.Context().GetNotifications().GetActive())
			errors += notification.Level == NotificationLevel::Error ? 1 : 0;
		CHECK(errors == 1);
		CHECK(editor.Context().IsSceneModified());
	}
}

