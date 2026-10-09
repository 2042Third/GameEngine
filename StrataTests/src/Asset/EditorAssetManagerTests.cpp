#include <doctest/doctest.h>

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Prefab.h"
#include "TestHelpers.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json, 1, '\t');
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	std::vector<uint8_t> CreateMaterialBytes(float roughness)
	{
		MaterialProperties properties;
		properties.Roughness = roughness;
		return ToBytes(Material::Create(properties)->Serialize());
	}

	// Test model format (".sttestmodel"): settings { "MeshCount": n, "Fail": bool }. Produces a model plus n mesh
	// sub-assets ("Mesh/<index>") and one material sub-asset.
	class TestModelImporter final : public AssetImporter
	{
	public:
		static inline std::atomic<uint32_t> s_ImportCount = 0;

		AssetType GetType() const override { return AssetType::Model; }
		std::vector<std::string> GetExtensions() const override { return { ".sttestmodel" }; }
		uint32_t GetVersion() const override { return 1; }

		nlohmann::json GetDefaultSettings(const std::filesystem::path&) const override
		{
			return { { "MeshCount", 2 }, { "Fail", false }, { "DelayMs", 0 } };
		}

		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
		{
			s_ImportCount++;
			if (const int64_t delay = JsonUtils::GetInt(context.Settings, "DelayMs", 0); delay > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(delay));
			if (JsonUtils::GetBool(context.Settings, "Fail", false))
			{
				*outError = "Requested failure";
				return false;
			}

			result.Data = ToBytes(Model::CreateFromSnapshot({ { "Entities", nlohmann::json::array() } })->Serialize());
			const int64_t meshCount = JsonUtils::GetInt(context.Settings, "MeshCount", 2);
			for (int64_t index = 0; index < meshCount; index++)
			{
				ImportedSubAsset mesh;
				mesh.Key = fmt::format("Mesh/{}", index);
				mesh.Name = fmt::format("Part{}", index);
				mesh.Type = AssetType::Mesh;
				mesh.Data = MeshFactory::CreateCube(1.0f + static_cast<float>(index))->Serialize();
				result.SubAssets.push_back(std::move(mesh));
			}
			ImportedSubAsset material;
			material.Key = "Material/0";
			material.Type = AssetType::Material;
			material.Data = CreateMaterialBytes(0.6f);
			result.SubAssets.push_back(std::move(material));
			result.Warnings.push_back("Test importer warning");
			return true;
		}
	};

	void RegisterTestModelImporter()
	{
		static bool s_Registered = false;
		if (!s_Registered)
		{
			AssetImporterRegistry::Register(CreateScope<TestModelImporter>());
			s_Registered = true;
		}
	}

	struct TestProject
	{
		std::filesystem::path Root;
		std::filesystem::path Assets;
		std::filesystem::path Cache;

		explicit TestProject(const std::string& name)
		{
			Root = Tests::CreateTemporaryDirectory(name);
			Assets = Root / "Assets";
			Cache = Root / ".strata" / "Cache";
			FileSystem::CreateDirectories(Assets);
		}

		Ref<EditorAssetManager> Open(bool watchFiles = false) const
		{
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = Assets;
			specification.CacheDirectory = Cache;
			specification.WatchFiles = watchFiles;
			Ref<EditorAssetManager> manager = CreateRef<EditorAssetManager>(specification);
			manager->Scan();
			return manager;
		}

		void Write(const std::string& relativePath, const std::vector<uint8_t>& data) const
		{
			REQUIRE(FileSystem::WriteBytes(Assets / FileSystem::FromUTF8(relativePath), data));
		}
	};

	// Initializes the job system for a test, so imports and loads run on worker threads.
	struct ScopedJobSystem
	{
		ScopedJobSystem()
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = 4;
			specification.IOThreadCount = 2;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}
	};

	int64_t GetWriteTime(const std::filesystem::path& path)
	{
		return FileSystem::GetLastWriteTime(path).value_or(-1);
	}

	Ref<Texture> LoadTexture(EditorAssetManager& manager, AssetHandle handle)
	{
		Ref<Asset> asset = manager.LoadAssetSync(handle);
		return asset && asset->GetType() == AssetType::Texture ? std::static_pointer_cast<Texture>(asset) : nullptr;
	}

}

TEST_SUITE("Asset.Editor")
{
	TEST_CASE("Scanning registers asset files with handles kept in .meta files")
	{
		TestProject project("EditorScan");
		project.Write("Textures/Brick.png", Tests::CreateSolidPNG(4, 4, 200, 100, 50));
		project.Write("Sounds/Beep.wav", Tests::CreateSineWav(0.25f));
		project.Write("Materials/Red.stmat", CreateMaterialBytes(0.3f));
		project.Write("Notes.txt", { 'h', 'i' });
		project.Write(".hidden/Ignored.png", Tests::CreateSolidPNG(1, 1, 0, 0, 0));

		AssetHandle brick;
		AssetHandle beep;
		AssetHandle red;
		int64_t cachedTime = 0;
		{
			Ref<EditorAssetManager> manager = project.Open();
			brick = manager->FindAssetByAbsolutePath(project.Assets / "Textures" / "Brick.png");
			beep = manager->FindAssetByPath("Sounds/Beep.wav");
			red = manager->FindAssetByPath("Materials/Red.stmat");
			REQUIRE(brick.IsValid());
			REQUIRE(beep.IsValid());
			REQUIRE(red.IsValid());
			CHECK_FALSE(manager->FindAssetByPath("Notes.txt").IsValid());
			CHECK_FALSE(manager->FindAssetByPath(".hidden/Ignored.png").IsValid());
			CHECK_FALSE(BuiltinAssets::IsBuiltin(brick));

			std::optional<AssetMetadata> metadata = manager->GetMetadata(brick);
			REQUIRE(metadata);
			CHECK(metadata->Type == AssetType::Texture);
			CHECK(metadata->Path == "Textures/Brick.png");
			CHECK(metadata->Name == "Brick");
			CHECK(manager->GetAbsolutePath(brick) == (project.Assets / "Textures" / "Brick.png").lexically_normal());

			CHECK(FileSystem::IsRegularFile(project.Assets / "Textures" / "Brick.png.meta"));
			const AssetImportInfo info = manager->GetImportInfo(brick);
			CHECK(info.Imported);
			CHECK(info.Error.empty());

			Ref<Texture> texture = LoadTexture(*manager, brick);
			REQUIRE(texture);
			CHECK(texture->GetWidth() == 4);
			CHECK(texture->GetSpecification().Format == TextureFormat::RGBA8SRGB);

			Ref<Asset> sound = manager->LoadAssetSync(beep);
			REQUIRE(sound);
			REQUIRE(std::static_pointer_cast<AudioClipAsset>(sound)->GetClip());
			CHECK(std::static_pointer_cast<AudioClipAsset>(sound)->GetClip()->GetFrameCount() == 12000);

			Ref<Asset> material = manager->LoadAssetSync(red);
			REQUIRE(material);
			CHECK(std::static_pointer_cast<Material>(material)->GetProperties().Roughness == doctest::Approx(0.3f));

			cachedTime = GetWriteTime(project.Cache / (brick.ToString() + ".bin"));
			CHECK(cachedTime >= 0);
		}

		// Reopening keeps handles and reuses the cached import.
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Textures/Brick.png") == brick);
			CHECK(manager->FindAssetByPath("Sounds/Beep.wav") == beep);
			CHECK(manager->FindAssetByPath("Materials/Red.stmat") == red);
			CHECK(GetWriteTime(project.Cache / (brick.ToString() + ".bin")) == cachedTime);
		}

		// Rewriting identical content (e.g. a checkout) does not re-import; changed content does.
		project.Write("Textures/Brick.png", Tests::CreateSolidPNG(4, 4, 200, 100, 50));
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(GetWriteTime(project.Cache / (brick.ToString() + ".bin")) == cachedTime);
		}
		project.Write("Textures/Brick.png", Tests::CreateSolidPNG(8, 8, 200, 100, 50));
		{
			Ref<EditorAssetManager> manager = project.Open();
			Ref<Texture> texture = LoadTexture(*manager, brick);
			REQUIRE(texture);
			CHECK(texture->GetWidth() == 8);
		}

		// A deleted cache is rebuilt with the same handles.
		REQUIRE(FileSystem::Remove(project.Cache));
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Textures/Brick.png") == brick);
			Ref<Texture> texture = LoadTexture(*manager, brick);
			REQUIRE(texture);
			CHECK(texture->GetWidth() == 8);
		}
	}

	TEST_CASE("Import settings live in the .meta file and re-import the asset")
	{
		TestProject project("EditorSettings");
		project.Write("Brick.png", Tests::CreateSolidPNG(8, 4, 10, 20, 30));
		AssetHandle handle;
		{
			Ref<EditorAssetManager> manager = project.Open();
			handle = manager->FindAssetByPath("Brick.png");
			REQUIRE(handle.IsValid());
			CHECK(manager->GetImportSettings(handle)["Usage"] == "Color");
			REQUIRE(LoadTexture(*manager, handle));

			std::string error;
			REQUIRE_MESSAGE(manager->SetImportSettings(handle, { { "MaxSize", 2 }, { "Usage", "Data" } }, &error), error);
			REQUIRE(manager->WaitForPendingLoads());
			Ref<Texture> texture = LoadTexture(*manager, handle);
			REQUIRE(texture);
			CHECK(texture->GetWidth() == 2);
			CHECK(texture->GetSpecification().Format == TextureFormat::RGBA8);

			CHECK_FALSE(manager->SetImportSettings(handle, nlohmann::json::array(), &error));
			CHECK_FALSE(manager->SetImportSettings(UUID(0x1234), { { "MaxSize", 2 } }, &error));
		}

		std::optional<std::string> meta = FileSystem::ReadText(project.Assets / "Brick.png.meta");
		REQUIRE(meta);
		CHECK(meta->find("\"MaxSize\": 2") != std::string::npos);
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->GetImportSettings(handle)["MaxSize"] == 2);
			Ref<Texture> texture = LoadTexture(*manager, handle);
			REQUIRE(texture);
			CHECK(texture->GetWidth() == 2);
		}
	}

	TEST_CASE("Failed imports report errors and recover after a fix")
	{
		TestProject project("EditorFailures");
		project.Write("Broken.png", { 1, 2, 3, 4, 5, 6, 7, 8 });
		project.Write("Broken.stmat", { '{', 'x' });
		Ref<EditorAssetManager> manager = project.Open();

		const AssetHandle texture = manager->FindAssetByPath("Broken.png");
		REQUIRE(texture.IsValid());
		CHECK_FALSE(manager->GetImportInfo(texture).Error.empty());
		CHECK_FALSE(manager->LoadAssetSync(texture));
		CHECK(manager->GetAssetState(texture) == AssetState::Failed);
		CHECK(manager->GetAssetError(texture).find("Import failed") != std::string::npos);

		// Engine assets are validated when loaded.
		const AssetHandle material = manager->FindAssetByPath("Broken.stmat");
		REQUIRE(material.IsValid());
		CHECK_FALSE(manager->LoadAssetSync(material));

		project.Write("Broken.png", Tests::CreateSolidPNG(2, 2, 1, 2, 3));
		std::string error;
		REQUIRE_MESSAGE(manager->ReimportAsset(texture, &error), error);
		CHECK(manager->GetImportInfo(texture).Error.empty());
		manager->ReloadAsset(texture);
		REQUIRE(manager->WaitForPendingLoads());
		Ref<Texture> loaded = LoadTexture(*manager, texture);
		REQUIRE(loaded);
		CHECK(loaded->GetWidth() == 2);

		CHECK_FALSE(manager->ReimportAsset(UUID(0x777), &error));
	}

	TEST_CASE("Moving, renaming and deleting assets keeps handles")
	{
		TestProject project("EditorFileOps");
		project.Write("Brick.png", Tests::CreateSolidPNG(4, 4, 1, 1, 1));
		project.Write("Stone.png", Tests::CreateSolidPNG(4, 4, 2, 2, 2));
		AssetHandle brick;
		{
			Ref<EditorAssetManager> manager = project.Open();
			brick = manager->FindAssetByPath("Brick.png");
			REQUIRE(brick.IsValid());

			std::string error;
			REQUIRE_MESSAGE(manager->MoveAsset(brick, "Art/Walls/BrickWall.png", &error), error);
			CHECK_FALSE(FileSystem::Exists(project.Assets / "Brick.png"));
			CHECK_FALSE(FileSystem::Exists(project.Assets / "Brick.png.meta"));
			CHECK(FileSystem::IsRegularFile(project.Assets / "Art" / "Walls" / "BrickWall.png"));
			CHECK(FileSystem::IsRegularFile(project.Assets / "Art" / "Walls" / "BrickWall.png.meta"));
			CHECK(manager->FindAssetByPath("Art/Walls/BrickWall.png") == brick);
			CHECK_FALSE(manager->FindAssetByPath("Brick.png").IsValid());
			CHECK(manager->GetMetadata(brick)->Name == "BrickWall");
			REQUIRE(LoadTexture(*manager, brick));

			CHECK_FALSE(manager->MoveAsset(brick, "Stone.png", &error));        // Destination exists
			CHECK_FALSE(manager->MoveAsset(brick, "../Outside.png", &error));   // Outside the asset directory
			CHECK_FALSE(manager->MoveAsset(brick, "Art/Brick.wav", &error));    // Different file type
			CHECK_FALSE(manager->MoveAsset(UUID(0x55), "Other.png", &error));   // Unknown asset
		}
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Art/Walls/BrickWall.png") == brick);

			const AssetHandle stone = manager->FindAssetByPath("Stone.png");
			REQUIRE(stone.IsValid());
			REQUIRE(LoadTexture(*manager, stone));
			std::string error;
			REQUIRE_MESSAGE(manager->DeleteAsset(stone, &error), error);
			CHECK_FALSE(FileSystem::Exists(project.Assets / "Stone.png"));
			CHECK_FALSE(FileSystem::Exists(project.Assets / "Stone.png.meta"));
			CHECK_FALSE(FileSystem::Exists(project.Cache / (stone.ToString() + ".bin")));
			CHECK_FALSE(manager->IsHandleValid(stone));
			CHECK_FALSE(manager->DeleteAsset(stone, &error));
		}
	}

	TEST_CASE("Copies named by file managers never take the original's handle")
	{
		for (const char* copyName : { "Brick - Copy.png", "Brick copy.png", "Brick (1).png" })
		{
			CAPTURE(copyName);
			TestProject project("EditorCopyNames");
			project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
			AssetHandle original;
			{
				Ref<EditorAssetManager> manager = project.Open();
				original = manager->FindAssetByPath("Brick.png");
			}
			REQUIRE(FileSystem::Copy(project.Assets / "Brick.png", project.Assets / copyName));
			REQUIRE(FileSystem::Copy(project.Assets / "Brick.png.meta", project.Assets / (std::string(copyName) + ".meta")));

			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Brick.png") == original);
			CHECK(manager->FindAssetByPath(copyName).IsValid());
			CHECK(manager->FindAssetByPath(copyName) != original);
		}

		// With the persisted index the original keeps its handle even when the copy has the shorter name.
		TestProject project("EditorCopyIndex");
		project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		AssetHandle original;
		{
			Ref<EditorAssetManager> manager = project.Open();
			original = manager->FindAssetByPath("Brick.png");
		}
		REQUIRE(FileSystem::Copy(project.Assets / "Brick.png", project.Assets / "B.png"));
		REQUIRE(FileSystem::Copy(project.Assets / "Brick.png.meta", project.Assets / "B.png.meta"));
		Ref<EditorAssetManager> manager = project.Open();
		CHECK(manager->FindAssetByPath("Brick.png") == original);
		CHECK(manager->FindAssetByPath("B.png") != original);
	}

	TEST_CASE("Invalid .meta files are reported and never overwritten")
	{
		TestProject project("EditorInvalidMeta");
		project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		AssetHandle original;
		std::string originalMeta;
		{
			Ref<EditorAssetManager> manager = project.Open();
			original = manager->FindAssetByPath("Brick.png");
			originalMeta = FileSystem::ReadText(project.Assets / "Brick.png.meta").value_or("");
		}

		// A merge conflict in the .meta: the asset is ignored, the file left untouched.
		const std::string conflicted = "<<<<<<< HEAD\n" + originalMeta + "=======\n{}\n>>>>>>> branch\n";
		REQUIRE(FileSystem::WriteText(project.Assets / "Brick.png.meta", conflicted));
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK_FALSE(manager->FindAssetByPath("Brick.png").IsValid());
			CHECK(FileSystem::ReadText(project.Assets / "Brick.png.meta") == conflicted);
		}

		// A .meta without a usable handle is not replaced either.
		REQUIRE(FileSystem::WriteText(project.Assets / "Brick.png.meta", "{ \"Handle\": \"zz\" }"));
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK_FALSE(manager->FindAssetByPath("Brick.png").IsValid());
		}

		// Once resolved, the original handle is back.
		REQUIRE(FileSystem::WriteText(project.Assets / "Brick.png.meta", originalMeta));
		Ref<EditorAssetManager> manager = project.Open();
		CHECK(manager->FindAssetByPath("Brick.png") == original);
	}

	TEST_CASE("Hidden files and directories are never assets")
	{
		ScopedJobSystem jobs;
		TestProject project("EditorHidden");
		Ref<EditorAssetManager> manager = project.Open(true);
		project.Write("._Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		project.Write(".backup/Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		project.Write("Visible.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		CHECK(Tests::WaitUntil([&]()
		{
			manager->Update();
			return manager->FindAssetByPath("Visible.png").IsValid();
		}, std::chrono::milliseconds(15000)));
		CHECK_FALSE(manager->FindAssetByPath("._Brick.png").IsValid());
		CHECK_FALSE(manager->FindAssetByPath(".backup/Brick.png").IsValid());
		CHECK_FALSE(FileSystem::Exists(project.Assets / "._Brick.png.meta"));
	}

	TEST_CASE("Files moved with their .meta outside the editor keep their handle")
	{
		ScopedJobSystem jobs;
		TestProject project("EditorExternalMove");
		project.Write("Brick.png", Tests::CreateSolidPNG(4, 4, 1, 1, 1));
		Ref<EditorAssetManager> manager = project.Open(true);
		const AssetHandle brick = manager->FindAssetByPath("Brick.png");
		REQUIRE(brick.IsValid());
		REQUIRE(LoadTexture(*manager, brick));

		const std::filesystem::path cachePath = project.Cache / (brick.ToString() + ".bin");
		const int64_t cachedTime = GetWriteTime(cachePath);
		REQUIRE(FileSystem::Rename(project.Assets / "Brick.png", project.Assets / "Moved" / "Brick.png"));
		REQUIRE(FileSystem::Rename(project.Assets / "Brick.png.meta", project.Assets / "Moved" / "Brick.png.meta"));
		CHECK(Tests::WaitUntil([&]()
		{
			manager->Update();
			return manager->FindAssetByPath("Moved/Brick.png") == brick;
		}, std::chrono::milliseconds(15000)));
		CHECK(manager->GetMetadata(brick)->Path == "Moved/Brick.png");
		manager->UnloadAsset(brick);
		CHECK(LoadTexture(*manager, brick) != nullptr);
		// The move kept the cached import instead of importing again.
		manager->WaitForImports();
		CHECK(GetWriteTime(cachePath) == cachedTime);
	}

	TEST_CASE("Assets in a folder named Builtin are ordinary project assets")
	{
		TestProject project("EditorBuiltinFolder");
		project.Write("Builtin/Rock.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		const std::filesystem::path packPath = project.Root / "Game.stpak";
		AssetHandle rock;
		{
			Ref<EditorAssetManager> manager = project.Open();
			rock = manager->FindAssetByPath("Builtin/Rock.png");
			REQUIRE(rock.IsValid());
			CHECK_FALSE(manager->GetMetadata(rock)->IsBuiltin());
			REQUIRE(manager->BuildAssetPack(packPath));
		}
		Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath);
		REQUIRE(runtime);
		CHECK(runtime->LoadAssetSync(rock) != nullptr);
	}

	TEST_CASE("Scans and imports run concurrently on the job system")
	{
		ScopedJobSystem jobs;
		RegisterTestModelImporter();
		TestProject project("EditorConcurrent");
		for (int index = 0; index < 24; index++)
			project.Write(fmt::format("Textures/T{}.png", index), Tests::CreateSolidPNG(16, 16, static_cast<uint8_t>(index * 10), 1, 1));
		for (int index = 0; index < 6; index++)
			project.Write(fmt::format("Models/M{}.sttestmodel", index), { 'm' });

		Ref<EditorAssetManager> manager = project.Open(true);
		std::vector<AssetHandle> handles;
		for (const AssetMetadata& metadata : manager->GetAllMetadata())
		{
			if (!metadata.IsBuiltin())
				handles.push_back(metadata.Handle);
		}
		CHECK(handles.size() == 24 + 6 * 4); // Textures, models and their three sub-assets each
		for (AssetHandle handle : handles)
			manager->RequestLoad(handle);
		REQUIRE(manager->WaitForPendingLoads());
		for (AssetHandle handle : handles)
			CHECK(manager->GetAssetState(handle) == AssetState::Ready);

		// A move waits for the import still running on the moved file.
		const AssetHandle model = manager->FindAssetByPath("Models/M0.sttestmodel");
		REQUIRE(model.IsValid());
		std::optional<std::string> meta = FileSystem::ReadText(project.Assets / "Models" / "M0.sttestmodel.meta");
		REQUIRE(meta);
		std::optional<nlohmann::json> metaJson = JsonUtils::Parse(*meta);
		REQUIRE(metaJson);
		(*metaJson)["ImportSettings"]["DelayMs"] = 400;
		REQUIRE(FileSystem::WriteText(project.Assets / "Models" / "M0.sttestmodel.meta", JsonUtils::Dump(*metaJson, 1, '\t')));
		const uint32_t importsBefore = TestModelImporter::s_ImportCount;
		REQUIRE(Tests::WaitUntil([&]()
		{
			manager->Update();
			return TestModelImporter::s_ImportCount > importsBefore;
		}, std::chrono::milliseconds(15000)));

		std::string error;
		REQUIRE_MESSAGE(manager->MoveAsset(model, "Moved/M0.sttestmodel", &error), error);
		manager->WaitForImports();
		CHECK(manager->GetImportInfo(model).Error.empty());
		manager->UnloadAsset(model);
		CHECK(manager->LoadAssetSync(model) != nullptr);
		CHECK(manager->LoadAssetSync(DeriveSubAssetHandle(model, "Mesh/1")) != nullptr);
	}

	TEST_CASE("Copied .meta files get a new handle")
	{
		TestProject project("EditorDuplicates");
		project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		AssetHandle original;
		{
			Ref<EditorAssetManager> manager = project.Open();
			original = manager->FindAssetByPath("Brick.png");
		}
		REQUIRE(FileSystem::Copy(project.Assets / "Brick.png", project.Assets / "Copy.png"));
		REQUIRE(FileSystem::Copy(project.Assets / "Brick.png.meta", project.Assets / "Copy.png.meta"));

		AssetHandle copy;
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Brick.png") == original);
			copy = manager->FindAssetByPath("Copy.png");
			REQUIRE(copy.IsValid());
			CHECK(copy != original);
		}
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(manager->FindAssetByPath("Copy.png") == copy);
		}
	}

	TEST_CASE("Engine-native assets are created, validated and saved")
	{
		TestProject project("EditorNative");
		Ref<EditorAssetManager> manager = project.Open();

		std::string error;
		const AssetHandle handle = manager->CreateNativeAsset("Materials/Blue.stmat", CreateMaterialBytes(0.4f), &error);
		REQUIRE_MESSAGE(handle.IsValid(), error);
		CHECK(FileSystem::IsRegularFile(project.Assets / "Materials" / "Blue.stmat"));
		CHECK(FileSystem::IsRegularFile(project.Assets / "Materials" / "Blue.stmat.meta"));
		CHECK(manager->GetAssetType(handle) == AssetType::Material);

		Ref<Asset> loaded = manager->LoadAssetSync(handle);
		REQUIRE(loaded);
		CHECK(std::static_pointer_cast<Material>(loaded)->GetProperties().Roughness == doctest::Approx(0.4f));

		REQUIRE_MESSAGE(manager->SaveNativeAsset(handle, CreateMaterialBytes(0.8f), true, &error), error);
		REQUIRE(manager->WaitForPendingLoads());
		Ref<Asset> saved = manager->GetAsset(handle);
		REQUIRE(saved);
		CHECK(saved != loaded);
		CHECK(std::static_pointer_cast<Material>(saved)->GetProperties().Roughness == doctest::Approx(0.8f));

		const std::vector<uint8_t> invalid = { 'n', 'o', 'p', 'e' };
		CHECK_FALSE(manager->SaveNativeAsset(handle, invalid, true, &error));
		CHECK_FALSE(manager->CreateNativeAsset("Materials/Blue.stmat", CreateMaterialBytes(0.1f), &error).IsValid()); // Exists
		CHECK_FALSE(manager->CreateNativeAsset("Materials/Bad.stmat", invalid, &error).IsValid());
		CHECK_FALSE(FileSystem::Exists(project.Assets / "Materials" / "Bad.stmat"));
		CHECK_FALSE(manager->CreateNativeAsset("Textures/Image.png", CreateMaterialBytes(0.1f), &error).IsValid());
		CHECK_FALSE(manager->CreateNativeAsset("../Escape.stmat", CreateMaterialBytes(0.1f), &error).IsValid());

		Ref<Prefab> prefab = Prefab::CreateFromSnapshot({ { "Entities", nlohmann::json::array() } });
		const AssetHandle prefabHandle = manager->CreateNativeAsset("Prefabs/Empty.stprefab", ToBytes(prefab->Serialize()), &error);
		REQUIRE_MESSAGE(prefabHandle.IsValid(), error);
		CHECK(manager->LoadAssetSync(prefabHandle) != nullptr);
	}

	TEST_CASE("External files are copied into the project and imported")
	{
		TestProject project("EditorExternal");
		const std::filesystem::path external = Tests::CreateTemporaryDirectory("EditorExternalSource") / "Logo.png";
		REQUIRE(FileSystem::WriteBytes(external, Tests::CreateSolidPNG(4, 2, 9, 9, 9)));
		Ref<EditorAssetManager> manager = project.Open();

		std::string error;
		const AssetHandle first = manager->ImportExternalFile(external, "UI/Icons", &error);
		REQUIRE_MESSAGE(first.IsValid(), error);
		CHECK(manager->GetMetadata(first)->Path == "UI/Icons/Logo.png");
		Ref<Texture> texture = LoadTexture(*manager, first);
		REQUIRE(texture);
		CHECK(texture->GetHeight() == 2);

		const AssetHandle second = manager->ImportExternalFile(external, "UI/Icons", &error);
		REQUIRE(second.IsValid());
		CHECK(second != first);
		CHECK(manager->GetMetadata(second)->Path == "UI/Icons/Logo (1).png");

		const AssetHandle root = manager->ImportExternalFile(external, "", &error);
		REQUIRE_MESSAGE(root.IsValid(), error);
		CHECK(manager->GetMetadata(root)->Path == "Logo.png");
		const AssetHandle trailing = manager->ImportExternalFile(external, "UI/", &error);
		REQUIRE_MESSAGE(trailing.IsValid(), error);
		CHECK(manager->GetMetadata(trailing)->Path == "UI/Logo.png");

		const std::filesystem::path text = external.parent_path() / "Readme.txt";
		REQUIRE(FileSystem::WriteText(text, "text"));
		CHECK_FALSE(manager->ImportExternalFile(text, "", &error).IsValid());
		CHECK_FALSE(manager->ImportExternalFile(external.parent_path() / "Missing.png", "", &error).IsValid());
		CHECK_FALSE(manager->ImportExternalFile(external, "../Outside", &error).IsValid());
	}

	TEST_CASE("Sub-assets get stable handles and follow re-imports")
	{
		RegisterTestModelImporter();
		TestProject project("EditorSubAssets");
		project.Write("Models/Robot.sttestmodel", { 'r', 'o', 'b', 'o', 't' });

		AssetHandle model;
		{
			const uint32_t importsBefore = TestModelImporter::s_ImportCount;
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(TestModelImporter::s_ImportCount == importsBefore + 1);
			model = manager->FindAssetByPath("Models/Robot.sttestmodel");
			REQUIRE(model.IsValid());

			const AssetImportInfo info = manager->GetImportInfo(model);
			REQUIRE(info.SubAssets.size() == 3);
			CHECK(info.Warnings == std::vector<std::string> { "Test importer warning" });
			const AssetHandle mesh = DeriveSubAssetHandle(model, "Mesh/1");
			std::optional<AssetMetadata> metadata = manager->GetMetadata(mesh);
			REQUIRE(metadata);
			CHECK(metadata->Parent == model);
			CHECK(metadata->SubAssetKey == "Mesh/1");
			CHECK(metadata->Name == "Part1");
			CHECK(metadata->Path == "Models/Robot.sttestmodel");
			CHECK(manager->GetAbsolutePath(mesh) == manager->GetAbsolutePath(model));

			Ref<Asset> meshAsset = manager->LoadAssetSync(mesh);
			REQUIRE(meshAsset);
			CHECK(std::static_pointer_cast<Mesh>(meshAsset)->GetBounds().GetSize().x == doctest::Approx(2.0f));
			CHECK(manager->LoadAssetSync(model) != nullptr);
			CHECK(manager->LoadAssetSync(DeriveSubAssetHandle(model, "Material/0")) != nullptr);
		}

		{
			// Sub-assets come from the import record without importing again.
			const uint32_t importsBefore = TestModelImporter::s_ImportCount;
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(TestModelImporter::s_ImportCount == importsBefore);
			CHECK(manager->IsHandleValid(DeriveSubAssetHandle(model, "Mesh/1")));

			// Fewer meshes: the dropped sub-asset disappears.
			std::string error;
			REQUIRE_MESSAGE(manager->SetImportSettings(model, { { "MeshCount", 1 } }, &error), error);
			CHECK(manager->IsHandleValid(DeriveSubAssetHandle(model, "Mesh/0")));
			CHECK_FALSE(manager->IsHandleValid(DeriveSubAssetHandle(model, "Mesh/1")));
			CHECK_FALSE(FileSystem::Exists(project.Cache / (DeriveSubAssetHandle(model, "Mesh/1").ToString() + ".bin")));

			// A failed import keeps the previous sub-assets registered; loading them reports the failure.
			CHECK_FALSE(manager->SetImportSettings(model, { { "Fail", true } }, &error));
			CHECK(error == "Requested failure");
			const AssetHandle mesh = DeriveSubAssetHandle(model, "Mesh/0");
			CHECK(manager->IsHandleValid(mesh));
			manager->UnloadAsset(mesh);
			CHECK_FALSE(manager->LoadAssetSync(mesh));
			CHECK(manager->GetAssetError(mesh).find("Requested failure") != std::string::npos);

			// Fixing the source reloads the sub-assets that failed.
			REQUIRE(manager->SetImportSettings(model, { { "Fail", false } }, &error));
			REQUIRE(manager->WaitForPendingLoads());
			CHECK(manager->GetAssetState(mesh) == AssetState::Ready);

			// Deleting the model removes its sub-assets.
			REQUIRE(manager->DeleteAsset(model, &error));
			CHECK_FALSE(manager->IsHandleValid(mesh));
			CHECK_FALSE(manager->IsHandleValid(DeriveSubAssetHandle(model, "Material/0")));
		}
	}

	TEST_CASE("Files changed on disk are hot reloaded")
	{
		ScopedJobSystem jobs;
		TestProject project("EditorHotReload");
		project.Write("Brick.png", Tests::CreateSolidPNG(4, 4, 1, 1, 1));
		project.Write("Rough.stmat", CreateMaterialBytes(0.5f));
		Ref<EditorAssetManager> manager = project.Open(true);

		const AssetHandle brick = manager->FindAssetByPath("Brick.png");
		const AssetHandle rough = manager->FindAssetByPath("Rough.stmat");
		REQUIRE(LoadTexture(*manager, brick));
		REQUIRE(manager->LoadAssetSync(rough));

		auto waitFor = [&](const std::function<bool()>& condition)
		{
			return Tests::WaitUntil([&]()
			{
				manager->Update();
				return condition();
			}, std::chrono::milliseconds(15000));
		};

		project.Write("Brick.png", Tests::CreateSolidPNG(16, 16, 1, 1, 1));
		CHECK(waitFor([&]()
		{
			Ref<Asset> asset = manager->GetAsset(brick);
			return asset && std::static_pointer_cast<Texture>(asset)->GetWidth() == 16;
		}));

		// A different file size, so the change is seen even on file systems with coarse timestamps.
		project.Write("Rough.stmat", CreateMaterialBytes(0.875f));
		CHECK(waitFor([&]()
		{
			Ref<Asset> asset = manager->GetAsset(rough);
			return asset && std::static_pointer_cast<Material>(asset)->GetProperties().Roughness > 0.85f;
		}));

		project.Write("New/Added.png", Tests::CreateSolidPNG(2, 2, 3, 3, 3));
		CHECK(waitFor([&]() { return manager->FindAssetByPath("New/Added.png").IsValid(); }));
		CHECK(FileSystem::IsRegularFile(project.Assets / "New" / "Added.png.meta"));

		// A removed file unregisters its asset; its .meta stays, so a restored file keeps its handle.
		REQUIRE(FileSystem::Remove(project.Assets / "Brick.png"));
		CHECK(waitFor([&]() { return !manager->IsHandleValid(brick); }));
		CHECK(FileSystem::Exists(project.Assets / "Brick.png.meta"));
		project.Write("Brick.png", Tests::CreateSolidPNG(4, 4, 1, 1, 1));
		CHECK(waitFor([&]() { return manager->FindAssetByPath("Brick.png") == brick; }));

		// A deleted .meta of an existing asset is restored, keeping its handle.
		REQUIRE(FileSystem::Remove(project.Assets / "Rough.stmat.meta"));
		CHECK(waitFor([&]() { return FileSystem::Exists(project.Assets / "Rough.stmat.meta"); }));
		manager->WaitForImports();
		manager.reset();
		Ref<EditorAssetManager> reopened = project.Open();
		CHECK(reopened->FindAssetByPath("Rough.stmat") == rough);
	}

	TEST_CASE("Files read by an import are dependencies that redo it")
	{
		ScopedJobSystem jobs;
		TestProject project("EditorDependencies");
		const nlohmann::json triangle = {
			{ "asset", { { "version", "2.0" } } },
			{ "scenes", nlohmann::json::array({ { { "nodes", nlohmann::json::array({ 0 }) } } }) },
			{ "nodes", nlohmann::json::array({ { { "mesh", 0 } } }) },
			{ "meshes", nlohmann::json::array({ { { "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 } } } } }) } } }) },
			{ "buffers", nlohmann::json::array({ { { "uri", "Triangle.bin" }, { "byteLength", 36 } } }) },
			{ "bufferViews", nlohmann::json::array({ { { "buffer", 0 }, { "byteLength", 36 } } }) },
			{ "accessors", nlohmann::json::array({ { { "bufferView", 0 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" } } }) }
		};
		// Padding after the declared bytes gives every version a different file size, so changes are seen even on
		// file systems with coarse timestamps.
		auto writeBuffer = [&](float size)
		{
			const std::vector<float> positions = { 0, 0, 0, size, 0, 0, 0, size, 0 };
			std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(positions.data()), reinterpret_cast<const uint8_t*>(positions.data() + positions.size()));
			bytes.resize(bytes.size() + static_cast<size_t>(size) * 4, 0);
			project.Write("Models/Triangle.bin", bytes);
		};
		writeBuffer(1.0f);
		project.Write("Models/Triangle.gltf", ToBytes(triangle));

		AssetHandle model;
		AssetHandle mesh;
		auto getExtent = [&](const Ref<Asset>& asset)
		{
			return asset && asset->GetType() == AssetType::Mesh ? std::static_pointer_cast<Mesh>(asset)->GetSubmeshes()[0].Bounds.Max.x : -1.0f;
		};
		{
			Ref<EditorAssetManager> manager = project.Open();
			model = manager->FindAssetByPath("Models/Triangle.gltf");
			REQUIRE(model.IsValid());
			mesh = DeriveSubAssetHandle(model, "Mesh/0");
			CHECK(getExtent(manager->LoadAssetSync(mesh)) == doctest::Approx(1.0f));
		}

		// Changed while the editor was closed: the scan imports again.
		writeBuffer(2.0f);
		{
			Ref<EditorAssetManager> manager = project.Open();
			CHECK(getExtent(manager->LoadAssetSync(mesh)) == doctest::Approx(2.0f));
		}

		// Changed while the editor runs: hot reload, also after the file was missing for a while.
		Ref<EditorAssetManager> manager = project.Open(true);
		REQUIRE(manager->LoadAssetSync(mesh));
		auto waitFor = [&](const std::function<bool()>& condition)
		{
			return Tests::WaitUntil([&]()
			{
				manager->Update();
				return condition();
			}, std::chrono::milliseconds(15000));
		};
		writeBuffer(3.0f);
		CHECK(waitFor([&]() { return getExtent(manager->GetAsset(mesh)) == 3.0f; }));

		REQUIRE(FileSystem::Remove(project.Assets / "Models" / "Triangle.bin"));
		CHECK(waitFor([&]() { return !manager->GetImportInfo(model).Error.empty(); }));
		writeBuffer(4.0f);
		CHECK(waitFor([&]() { return manager->GetImportInfo(model).Error.empty() && getExtent(manager->GetAsset(mesh)) == 4.0f; }));
		manager->WaitForImports();

		// Moved away from its buffer, the model reads a different (here: missing) file: the cache is not current.
		std::string error;
		REQUIRE_MESSAGE(manager->MoveAsset(model, "Elsewhere/Triangle.gltf", &error), error);
		manager.reset();
		Ref<EditorAssetManager> reopened = project.Open();
		CHECK_FALSE(reopened->GetImportInfo(model).Error.empty());
	}

	TEST_CASE("Scans remove cached data nothing uses")
	{
		TestProject project("EditorCacheCleanup");
		project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		project.Write("Stone.png", Tests::CreateSolidPNG(2, 2, 2, 2, 2));
		AssetHandle brick;
		AssetHandle stone;
		{
			Ref<EditorAssetManager> manager = project.Open();
			brick = manager->FindAssetByPath("Brick.png");
			stone = manager->FindAssetByPath("Stone.png");
			REQUIRE(FileSystem::Exists(project.Cache / (stone.ToString() + ".bin")));
		}

		// Deleted outside the editor, and a write interrupted by a crash.
		REQUIRE(FileSystem::Remove(project.Assets / "Stone.png"));
		REQUIRE(FileSystem::WriteText(project.Cache / (brick.ToString() + ".bin.staged"), "partial"));
		Ref<EditorAssetManager> manager = project.Open();
		CHECK(FileSystem::Exists(project.Cache / (brick.ToString() + ".bin")));
		CHECK(FileSystem::Exists(project.Cache / (brick.ToString() + ".import")));
		CHECK_FALSE(FileSystem::Exists(project.Cache / (brick.ToString() + ".bin.staged")));
		CHECK_FALSE(FileSystem::Exists(project.Cache / (stone.ToString() + ".bin")));
		CHECK_FALSE(FileSystem::Exists(project.Cache / (stone.ToString() + ".import")));
		CHECK(FileSystem::Exists(project.Cache / "AssetIndex.json"));
		CHECK(LoadTexture(*manager, brick));
	}

	TEST_CASE("Import settings are never written over an invalid .meta file")
	{
		TestProject project("EditorSettingsInvalidMeta");
		project.Write("Brick.png", Tests::CreateSolidPNG(2, 2, 1, 1, 1));
		Ref<EditorAssetManager> manager = project.Open();
		const AssetHandle brick = manager->FindAssetByPath("Brick.png");
		const nlohmann::json settings = manager->GetImportSettings(brick);

		// A merge conflict appears while the editor runs (without file watching).
		const std::string conflicted = "<<<<<<< HEAD\n{}\n=======\n{}\n>>>>>>> branch\n";
		REQUIRE(FileSystem::WriteText(project.Assets / "Brick.png.meta", conflicted));
		std::string error;
		CHECK_FALSE(manager->SetImportSettings(brick, { { "MaxSize", 1 } }, &error));
		CHECK(error.find("invalid") != std::string::npos);
		CHECK(FileSystem::ReadText(project.Assets / "Brick.png.meta") == conflicted);
		CHECK(manager->GetImportSettings(brick) == settings);
	}

	TEST_CASE("Asset packs built by the editor load in the runtime")
	{
		RegisterTestModelImporter();
		TestProject project("EditorPack");
		project.Write("Brick.png", Tests::CreateSolidPNG(4, 2, 5, 6, 7));
		project.Write("Rough.stmat", CreateMaterialBytes(0.7f));
		project.Write("Robot.sttestmodel", { 'r' });
		const std::filesystem::path packPath = project.Root / "Build" / "Game.stpak";

		AssetHandle brick;
		AssetHandle rough;
		AssetHandle robot;
		{
			Ref<EditorAssetManager> manager = project.Open();
			brick = manager->FindAssetByPath("Brick.png");
			rough = manager->FindAssetByPath("Rough.stmat");
			robot = manager->FindAssetByPath("Robot.sttestmodel");
			std::string error;
			REQUIRE_MESSAGE(manager->BuildAssetPack(packPath, &error), error);
		}

		std::string error;
		Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(packPath, &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->GetPack().GetEntries().size() == 6); // Texture, material, model and its three sub-assets

		Ref<Asset> texture = runtime->LoadAssetSync(brick);
		REQUIRE(texture);
		CHECK(std::static_pointer_cast<Texture>(texture)->GetWidth() == 4);
		Ref<Asset> material = runtime->LoadAssetSync(rough);
		REQUIRE(material);
		CHECK(std::static_pointer_cast<Material>(material)->GetProperties().Roughness == doctest::Approx(0.7f));
		CHECK(runtime->LoadAssetSync(robot) != nullptr);
		CHECK(runtime->LoadAssetSync(DeriveSubAssetHandle(robot, "Mesh/0")) != nullptr);
		CHECK(runtime->GetMetadata(DeriveSubAssetHandle(robot, "Mesh/0"))->Parent == robot);
		CHECK(runtime->FindAssetByPath("Brick.png") == brick);
	}
}
