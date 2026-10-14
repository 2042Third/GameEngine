#include <doctest/doctest.h>

#include "Editor/DocumentSaver.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
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
}
