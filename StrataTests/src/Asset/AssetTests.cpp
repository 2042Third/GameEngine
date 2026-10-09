#include <doctest/doctest.h>

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "TestHelpers.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	class VersionedTestImporter final : public AssetImporter
	{
	public:
		explicit VersionedTestImporter(uint32_t version)
			: m_Version(version)
		{
		}

		AssetType GetType() const override { return AssetType::Font; }
		std::vector<std::string> GetExtensions() const override { return { ".sttestfont" }; }
		uint32_t GetVersion() const override { return m_Version; }
		bool Import(const ImportContext&, ImportResult&, std::string*) const override { return true; }
	private:
		uint32_t m_Version;
	};

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	AssetMetadata MakeMetadata(uint64_t handle, AssetType type, std::string path, std::string name = {})
	{
		AssetMetadata metadata;
		metadata.Handle = UUID(handle);
		metadata.Type = type;
		metadata.Path = std::move(path);
		metadata.Name = name.empty() ? metadata.Path : std::move(name);
		return metadata;
	}

	// Writes a pack holding the given assets with their stored bytes.
	std::filesystem::path WritePack(const std::string& name, const std::vector<std::pair<AssetMetadata, std::vector<uint8_t>>>& assets)
	{
		const std::filesystem::path path = Tests::CreateTemporaryDirectory(name) / "Game.stpak";
		std::vector<AssetMetadata> metadata;
		std::map<uint64_t, std::vector<uint8_t>> data;
		for (const auto& [assetMetadata, bytes] : assets)
		{
			metadata.push_back(assetMetadata);
			data[static_cast<uint64_t>(assetMetadata.Handle)] = bytes;
		}

		std::string error;
		const bool written = AssetPack::Write(path, metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}, &error);
		REQUIRE_MESSAGE(written, error);
		return path;
	}

	std::vector<uint8_t> CreateMaterialBytes(float roughness)
	{
		MaterialProperties properties;
		properties.Roughness = roughness;
		return ToBytes(Material::Create(properties)->Serialize());
	}

}

TEST_SUITE("Asset")
{
	TEST_CASE("Asset types convert to names and back")
	{
		for (AssetType type : { AssetType::None, AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Mesh, AssetType::Material,
			AssetType::Texture, AssetType::AudioClip, AssetType::Font })
			CHECK(AssetTypeFromString(AssetTypeToString(type)) == type);
		CHECK_FALSE(AssetTypeFromString("Shader").has_value());

		CHECK(GetNativeAssetExtension(AssetType::Scene) == ".stscene");
		CHECK(GetNativeAssetExtension(AssetType::Prefab) == ".stprefab");
		CHECK(GetNativeAssetExtension(AssetType::Material) == ".stmat");
		CHECK(GetNativeAssetExtension(AssetType::Texture).empty());
		CHECK(AssetStateToString(AssetState::Ready) == std::string("Ready"));
	}

	TEST_CASE("Sub-asset handles are deterministic and never collide with built-ins")
	{
		const AssetHandle parent = UUID(0x123456789ABCDEF0ull);
		CHECK(DeriveSubAssetHandle(parent, "Mesh/0") == DeriveSubAssetHandle(parent, "Mesh/0"));
		CHECK(DeriveSubAssetHandle(parent, "Mesh/0") != DeriveSubAssetHandle(parent, "Mesh/1"));
		CHECK(DeriveSubAssetHandle(parent, "Mesh/0") != DeriveSubAssetHandle(UUID(0x42), "Mesh/0"));

		std::set<uint64_t> handles;
		for (int index = 0; index < 1000; index++)
		{
			const AssetHandle handle = DeriveSubAssetHandle(UUID(static_cast<uint64_t>(index)), "Key");
			CHECK(static_cast<uint64_t>(handle) >= 0x100);
			handles.insert(static_cast<uint64_t>(handle));
		}
		CHECK(handles.size() == 1000);
	}

	TEST_CASE("Importers are found by extension and later registrations win")
	{
		const AssetImporter* png = AssetImporterRegistry::FindByExtension(".PNG");
		REQUIRE(png);
		CHECK(png->GetType() == AssetType::Texture);
		REQUIRE(AssetImporterRegistry::FindByExtension(".stmat"));
		CHECK(AssetImporterRegistry::FindByExtension(".stmat")->StoresSourceDirectly());
		CHECK(AssetImporterRegistry::FindByExtension(".wav")->GetType() == AssetType::AudioClip);
		CHECK(AssetImporterRegistry::FindByExtension(".ttf")->GetType() == AssetType::Font);
		CHECK(AssetImporterRegistry::FindByExtension(".unknown") == nullptr);
		CHECK(AssetImporterRegistry::FindByExtension("") == nullptr);

		AssetImporterRegistry::Register(CreateScope<VersionedTestImporter>(1));
		AssetImporterRegistry::Register(CreateScope<VersionedTestImporter>(2));
		const AssetImporter* custom = AssetImporterRegistry::FindByExtension(".STTESTFONT");
		REQUIRE(custom);
		CHECK(custom->GetVersion() == 2);
		CHECK(AssetImporterRegistry::GetAll().size() >= 8);
	}

	TEST_CASE("Every asset type has a loader")
	{
		for (AssetType type : { AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Mesh, AssetType::Material, AssetType::Texture,
			AssetType::AudioClip, AssetType::Font })
		{
			CAPTURE(AssetTypeToString(type));
			CHECK(AssetLoaderRegistry::Find(type) != nullptr);
		}
		CHECK(AssetLoaderRegistry::Find(AssetType::None) == nullptr);
	}

	TEST_CASE("Asset packs round trip metadata and data")
	{
		AssetMetadata parent = MakeMetadata(0x1000, AssetType::Model, "Models/Robot.gltf", "Robot");
		AssetMetadata child = MakeMetadata(0x1001, AssetType::Mesh, "Models/Robot.gltf", "Body");
		child.Parent = parent.Handle;
		child.SubAssetKey = "Mesh/0";
		const std::vector<uint8_t> empty;
		const std::vector<uint8_t> payload = { 1, 2, 3, 4, 5 };
		const std::filesystem::path path = WritePack("PackRoundTrip", { { parent, payload }, { child, empty } });

		std::string error;
		Scope<AssetPack> pack = AssetPack::Open(path, &error);
		REQUIRE_MESSAGE(pack, error);
		REQUIRE(pack->GetEntries().size() == 2);
		const AssetPackEntry& first = pack->GetEntries()[0];
		CHECK(first.Metadata.Handle == parent.Handle);
		CHECK(first.Metadata.Type == AssetType::Model);
		CHECK(first.Metadata.Path == "Models/Robot.gltf");
		CHECK(first.Metadata.Name == "Robot");
		const AssetPackEntry& second = pack->GetEntries()[1];
		CHECK(second.Metadata.Parent == parent.Handle);
		CHECK(second.Metadata.SubAssetKey == "Mesh/0");
		CHECK(second.Size == 0);

		std::vector<uint8_t> data;
		REQUIRE(pack->ReadData(first, data, &error));
		CHECK(data == payload);
		REQUIRE(pack->ReadData(second, data, &error));
		CHECK(data.empty());

		// A provider failure aborts the write and leaves no file behind.
		const std::filesystem::path failedPath = path.parent_path() / "Failed.stpak";
		CHECK_FALSE(AssetPack::Write(failedPath, { parent }, [](const AssetMetadata&, std::vector<uint8_t>&, std::string* outError)
		{
			*outError = "missing";
			return false;
		}, &error));
		CHECK(error.find("missing") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(failedPath));
		CHECK_FALSE(FileSystem::Exists(std::filesystem::path(failedPath).concat(".tmp")));
	}

	TEST_CASE("Corrupt asset packs are rejected")
	{
		const std::filesystem::path path = WritePack("PackCorrupt", { { MakeMetadata(0x2000, AssetType::Material, "A.stmat"), CreateMaterialBytes(0.5f) } });
		std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE(bytes);

		std::string error;
		CHECK_FALSE(AssetPack::Open(path.parent_path() / "Missing.stpak", &error));
		CHECK_FALSE(error.empty());

		const std::filesystem::path corruptPath = path.parent_path() / "Corrupt.stpak";
		auto openModified = [&](const std::vector<uint8_t>& data)
		{
			REQUIRE(FileSystem::WriteBytes(corruptPath, data));
			error.clear();
			const bool opened = AssetPack::Open(corruptPath, &error) != nullptr;
			CHECK((opened || !error.empty()));
			return opened;
		};

		for (size_t length = 0; length < bytes->size(); length += 3)
		{
			CAPTURE(length);
			CHECK_FALSE(openModified(std::vector<uint8_t>(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(length))));
		}

		std::vector<uint8_t> wrongMagic = *bytes;
		wrongMagic[0] = 'X';
		CHECK_FALSE(openModified(wrongMagic));
		std::vector<uint8_t> wrongVersion = *bytes;
		wrongVersion[4] = 99;
		CHECK_FALSE(openModified(wrongVersion));
		std::vector<uint8_t> hugeEntryCount = *bytes;
		hugeEntryCount[8] = 0xFF;
		hugeEntryCount[9] = 0xFF;
		CHECK_FALSE(openModified(hugeEntryCount));

		// Single-byte corruption anywhere never crashes.
		for (size_t offset = 0; offset < bytes->size(); offset++)
		{
			std::vector<uint8_t> corrupted = *bytes;
			corrupted[offset] ^= 0xA5;
			openModified(corrupted);
		}

		// Duplicate handles are rejected.
		const std::filesystem::path duplicates = WritePack("PackDuplicates", {
			{ MakeMetadata(0x3000, AssetType::Material, "A.stmat"), CreateMaterialBytes(0.1f) },
			{ MakeMetadata(0x3000, AssetType::Material, "B.stmat"), CreateMaterialBytes(0.2f) } });
		CHECK_FALSE(AssetPack::Open(duplicates, &error));
	}

	TEST_CASE("The runtime asset manager loads assets from a pack")
	{
		Ref<Mesh> cube = MeshFactory::CreateCube();
		const std::filesystem::path path = WritePack("RuntimeManager", {
			{ MakeMetadata(0x4000, AssetType::Material, "Materials/Rough.stmat", "Rough"), CreateMaterialBytes(0.9f) },
			{ MakeMetadata(0x4001, AssetType::Mesh, "Meshes/Cube.mesh", "Cube"), cube->Serialize() },
			{ MakeMetadata(0x4002, AssetType::Mesh, "Meshes/Broken.mesh", "Broken"), { 1, 2, 3 } },
			{ MakeMetadata(BuiltinAssets::CubeMesh, AssetType::Material, "Override.stmat"), CreateMaterialBytes(0.1f) } });

		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path, &error);
		REQUIRE_MESSAGE(manager, error);
		CHECK(manager->GetPack().GetEntries().size() == 4);

		CHECK(manager->FindAssetByPath("Materials/Rough.stmat") == UUID(0x4000));
		CHECK(manager->GetAssetType(UUID(0x4001)) == AssetType::Mesh);
		CHECK(manager->GetAssetState(UUID(0x4000)) == AssetState::Unloaded);

		Ref<Asset> material = manager->LoadAssetSync(UUID(0x4000));
		REQUIRE(material);
		CHECK(material->Handle == UUID(0x4000));
		CHECK(std::static_pointer_cast<Material>(material)->GetProperties().Roughness == doctest::Approx(0.9f));
		CHECK(manager->GetAssetState(UUID(0x4000)) == AssetState::Ready);

		Ref<Asset> mesh = manager->LoadAssetSync(UUID(0x4001));
		REQUIRE(mesh);
		CHECK(std::static_pointer_cast<Mesh>(mesh)->GetIndices() == cube->GetIndices());

		CHECK_FALSE(manager->LoadAssetSync(UUID(0x4002)));
		CHECK(manager->GetAssetState(UUID(0x4002)) == AssetState::Failed);
		CHECK_FALSE(manager->GetAssetError(UUID(0x4002)).empty());

		CHECK_FALSE(manager->LoadAssetSync(UUID(0x9999)));
		CHECK_FALSE(manager->IsHandleValid(UUID(0x9999)));

		// Packs cannot replace built-in assets.
		CHECK(manager->GetAssetType(BuiltinAssets::CubeMesh) == AssetType::Mesh);

		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.LoadedAssets >= 2 + BuiltinAssets::GetAll().size());
		CHECK(stats.FailedAssets == 1);

		CHECK_FALSE(RuntimeAssetManager::Create(path.parent_path() / "Missing.stpak", &error));
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Built-in assets are registered in every manager")
	{
		const std::filesystem::path path = WritePack("BuiltinAssets", {});
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);

		for (const BuiltinAssetInfo& info : BuiltinAssets::GetAll())
		{
			CAPTURE(std::string(info.Name));
			CHECK(BuiltinAssets::IsBuiltin(info.Handle));
			CHECK(manager->GetAssetType(info.Handle) == info.Type);
			CHECK(manager->GetAssetState(info.Handle) == AssetState::Ready);
			CHECK(manager->FindAssetByPath("Builtin/" + std::string(info.Name)) == info.Handle);
			Ref<Asset> asset = manager->GetAsset(info.Handle);
			REQUIRE(asset);
			CHECK(asset->GetType() == info.Type);
		}

		// Memory assets are never unloaded or reloaded.
		manager->UnloadAsset(BuiltinAssets::SphereMesh);
		manager->ReloadAsset(BuiltinAssets::SphereMesh);
		CHECK(manager->GetAssetState(BuiltinAssets::SphereMesh) == AssetState::Ready);
		CHECK_FALSE(BuiltinAssets::IsBuiltin(UUID::Null()));
		CHECK_FALSE(BuiltinAssets::IsBuiltin(UUID(0x100)));

		Ref<Mesh> sphere = std::static_pointer_cast<Mesh>(manager->GetAsset(BuiltinAssets::SphereMesh));
		CHECK(sphere->GetSubmeshes()[0].Material == BuiltinAssets::DefaultMaterial);
	}

	TEST_CASE("Assets load asynchronously on the job system")
	{
		JobSystemSpecification specification;
		specification.WorkerThreadCount = 4;
		specification.IOThreadCount = 2;
		JobSystem::Init(specification);

		std::vector<std::pair<AssetMetadata, std::vector<uint8_t>>> assets;
		for (uint64_t index = 0; index < 64; index++)
		{
			Ref<Mesh> mesh = MeshFactory::CreateSphere(0.5f + static_cast<float>(index) * 0.01f, 16, 8);
			assets.push_back({ MakeMetadata(0x5000 + index, AssetType::Mesh, "Meshes/Sphere" + std::to_string(index) + ".mesh"), mesh->Serialize() });
		}
		const std::filesystem::path path = WritePack("AsyncLoading", assets);

		{
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
			REQUIRE(manager);
			for (uint64_t index = 0; index < 64; index++)
			{
				const AssetPriority priority = index % 3 == 0 ? AssetPriority::High : (index % 3 == 1 ? AssetPriority::Normal : AssetPriority::Low);
				CHECK(manager->GetAsset(UUID(0x5000 + index), priority) == nullptr); // Never blocks
			}
			REQUIRE(manager->WaitForPendingLoads(std::chrono::milliseconds(30000)));
			CHECK_FALSE(manager->HasPendingLoads());
			for (uint64_t index = 0; index < 64; index++)
			{
				CHECK(manager->GetAssetState(UUID(0x5000 + index)) == AssetState::Ready);
				Ref<Mesh> mesh = std::static_pointer_cast<Mesh>(manager->GetAsset(UUID(0x5000 + index)));
				REQUIRE(mesh);
				CHECK(mesh->GetBounds().GetSize().x == doctest::Approx(1.0f + static_cast<float>(index) * 0.02f).epsilon(0.01));
			}
			CHECK(manager->GetStats().TotalLoadsCompleted == 64);

			// Destroying the manager with loads in flight waits for them.
			for (uint64_t index = 0; index < 64; index++)
				manager->UnloadAsset(UUID(0x5000 + index));
			for (uint64_t index = 0; index < 64; index++)
				manager->RequestLoad(UUID(0x5000 + index));
		}

		JobSystem::Shutdown();
	}

	TEST_CASE("Reloading keeps serving the old object until the new one is ready")
	{
		const std::filesystem::path path = WritePack("Reload", { { MakeMetadata(0x6000, AssetType::Material, "M.stmat"), CreateMaterialBytes(0.25f) } });
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);

		Ref<Asset> first = manager->LoadAssetSync(UUID(0x6000));
		REQUIRE(first);
		manager->ReloadAsset(UUID(0x6000));
		CHECK(manager->GetAsset(UUID(0x6000)) == first); // Still served while the new version loads
		REQUIRE(manager->WaitForPendingLoads());
		Ref<Asset> second = manager->GetAsset(UUID(0x6000));
		REQUIRE(second);
		CHECK(second != first);

		manager->UnloadAsset(UUID(0x6000));
		CHECK(manager->GetAssetState(UUID(0x6000)) == AssetState::Unloaded);
		CHECK(manager->GetAsset(UUID(0x6000)) == nullptr); // Requests the load again
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(UUID(0x6000)) == AssetState::Ready);

		// Reloading an asset that was never requested does nothing.
		const std::filesystem::path otherPath = WritePack("ReloadIdle", { { MakeMetadata(0x6001, AssetType::Material, "N.stmat"), CreateMaterialBytes(0.5f) } });
		Ref<RuntimeAssetManager> other = RuntimeAssetManager::Create(otherPath);
		REQUIRE(other);
		other->ReloadAsset(UUID(0x6001));
		CHECK(other->GetAssetState(UUID(0x6001)) == AssetState::Unloaded);
	}

	TEST_CASE("The content version changes whenever loaded objects change")
	{
		const std::filesystem::path path = WritePack("ContentVersion", {
			{ MakeMetadata(0x6100, AssetType::Material, "M.stmat"), CreateMaterialBytes(0.25f) },
			{ MakeMetadata(0x6101, AssetType::Mesh, "Broken.mesh"), { 1, 2, 3 } } });
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);
		uint64_t version = manager->GetContentVersion();
		const auto changed = [&]()
		{
			const uint64_t current = manager->GetContentVersion();
			const bool result = current != version;
			version = current;
			return result;
		};

		// Failed loads and requests publish nothing.
		CHECK_FALSE(manager->LoadAssetSync(UUID(0x6101)));
		CHECK_FALSE(changed());
		CHECK(manager->GetAsset(UUID(0x6100)) == nullptr);
		CHECK_FALSE(changed());

		REQUIRE(manager->WaitForPendingLoads());
		CHECK(changed());
		CHECK(manager->GetAsset(UUID(0x6100)) != nullptr);
		CHECK_FALSE(changed());

		manager->ReloadAsset(UUID(0x6100));
		CHECK_FALSE(changed()); // The previous object is served until the new one is ready
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(changed());

		manager->UnloadAsset(UUID(0x6100));
		CHECK(changed());
		manager->UnloadAsset(UUID(0x6100));
		CHECK_FALSE(changed());

		AssetMetadata metadata;
		metadata.Name = "Runtime";
		manager->AddMemoryAsset(Material::Create(), metadata);
		CHECK(changed());
	}

	TEST_CASE("Content changes name the changed assets until they are forgotten")
	{
		const std::filesystem::path path = WritePack("ContentChanges", {
			{ MakeMetadata(0x6200, AssetType::Material, "A.stmat"), CreateMaterialBytes(0.25f) },
			{ MakeMetadata(0x6201, AssetType::Material, "B.stmat"), CreateMaterialBytes(0.5f) } });
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);
		const uint64_t start = manager->GetContentVersion();
		std::vector<AssetHandle> changes;
		CHECK(manager->GetContentChanges(start, changes));
		CHECK(changes.empty());

		REQUIRE(manager->LoadAssetSync(UUID(0x6200)));
		const uint64_t loaded = manager->GetContentVersion();
		CHECK(manager->GetContentChanges(start, changes));
		CHECK(changes == std::vector<AssetHandle> { UUID(0x6200) });

		manager->UnloadAsset(UUID(0x6200));
		REQUIRE(manager->LoadAssetSync(UUID(0x6201)));
		changes.clear();
		CHECK(manager->GetContentChanges(loaded, changes));
		std::sort(changes.begin(), changes.end());
		CHECK(changes == std::vector<AssetHandle> { UUID(0x6200), UUID(0x6201) });

		// Only the latest changes are remembered.
		const uint64_t before = manager->GetContentVersion();
		for (size_t index = 0; index <= AssetManagerBase::c_MaxContentChanges; index++)
			manager->AddMemoryAsset(Material::Create(), AssetMetadata());
		changes.clear();
		CHECK_FALSE(manager->GetContentChanges(before, changes));
		CHECK(changes.empty());
		CHECK(manager->GetContentChanges(manager->GetContentVersion() - 1, changes));
		CHECK(changes.size() == 1);
	}

	TEST_CASE("The active asset manager serves typed requests")
	{
		const std::filesystem::path path = WritePack("ActiveManager", { { MakeMetadata(0x7000, AssetType::Material, "M.stmat"), CreateMaterialBytes(0.75f) } });
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);

		CHECK_FALSE(AssetManager::HasActive());
		CHECK(AssetManager::GetAsset<Material>(UUID(0x7000)) == nullptr);
		AssetManager::SetActive(manager);
		CHECK(AssetManager::IsHandleValid(UUID(0x7000)));
		CHECK(AssetManager::GetAssetType(UUID(0x7000)) == AssetType::Material);
		Ref<Material> material = AssetManager::LoadAssetSync<Material>(UUID(0x7000));
		REQUIRE(material);
		CHECK(material->GetProperties().Roughness == doctest::Approx(0.75f));
		CHECK(AssetManager::GetAsset<Mesh>(UUID(0x7000)) == nullptr); // Wrong type
		CHECK(AssetManager::GetAsset<Material>(UUID::Null()) == nullptr);
		CHECK(AssetManager::GetAssetState(UUID(0x7000)) == AssetState::Ready);
		AssetManager::SetActive(nullptr);
		CHECK_FALSE(AssetManager::HasActive());
	}
}
