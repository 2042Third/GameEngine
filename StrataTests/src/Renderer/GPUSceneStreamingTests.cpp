#include "Renderer/SceneRendererTestUtils.h"

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Scene/Prefab.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_Texture = 0x9100;
	constexpr uint64_t c_Material = 0x9101;
	constexpr uint64_t c_Mesh = 0x9102;
	constexpr uint64_t c_Prefab = 0x9103;

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// A game pack holding a prefab that draws a mesh with a textured material: the prefab depends on the mesh and the
	// material, the material on the texture (recorded as exports record them).
	std::filesystem::path WritePrefabPack()
	{
		TextureMip level0;
		level0.Width = 256;
		level0.Height = 256;
		level0.Data.assign(static_cast<size_t>(level0.Width) * level0.Height * 4, 200);
		std::vector<TextureMip> mips = { std::move(level0) };
		REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA8SRGB, false));
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8SRGB;
		specification.DebugName = "StreamedTexture";
		const Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);

		MaterialProperties properties;
		properties.BaseColorMap = UUID(c_Texture);

		Scene authoring("Authoring");
		Entity root = authoring.CreateEntity("Streamed");
		MeshRendererComponent& renderer = root.AddComponent<MeshRendererComponent>();
		renderer.Mesh = UUID(c_Mesh);
		renderer.Material = UUID(c_Material);
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(authoring, { root });
		REQUIRE(prefab);

		std::map<uint64_t, std::vector<uint8_t>> data;
		std::vector<AssetPackSource> sources;
		const auto add = [&](uint64_t handle, AssetType type, const std::string& path, std::vector<uint8_t> bytes, std::vector<AssetHandle> dependencies)
		{
			AssetPackSource source;
			source.Metadata.Handle = UUID(handle);
			source.Metadata.Type = type;
			source.Metadata.Path = path;
			source.Metadata.Name = path;
			source.Metadata.StoredSize = bytes.size();
			source.Dependencies = std::move(dependencies);
			sources.push_back(std::move(source));
			data[handle] = std::move(bytes);
		};
		add(c_Texture, AssetType::Texture, "Textures/Streamed.png", texture->Serialize(), {});
		add(c_Material, AssetType::Material, "Materials/Streamed.stmat", ToBytes(Material::Create(properties)->Serialize()), { UUID(c_Texture) });
		add(c_Mesh, AssetType::Mesh, "Meshes/Torus.mesh", MeshFactory::CreateTorus()->Serialize(), {});
		add(c_Prefab, AssetType::Prefab, "Prefabs/Streamed.stprefab", ToBytes(prefab->Serialize()), { UUID(c_Mesh), UUID(c_Material) });

		const std::filesystem::path path = CreateTemporaryDirectory("GPUSceneStreaming") / "Game.stpak";
		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(path, sources, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}, {}, nullptr, &error), error);
		return path;
	}

	// A playing scene that views the origin, with the game's manager active.
	struct StreamingStage
	{
		Ref<RuntimeAssetManager> Manager;
		Scene Level { "Level" };
		SceneRenderer Renderer;

		explicit StreamingStage(const std::filesystem::path& pack)
		{
			std::string error;
			Manager = RuntimeAssetManager::Create(pack, &error);
			REQUIRE_MESSAGE(Manager, error);
			AssetManager::SetActive(Manager);
			AddNeutralPostProcess(Level);
			Level.OnRuntimeStart();
			Renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		}

		~StreamingStage()
		{
			Level.OnRuntimeStop();
			Manager->WaitForPendingLoads();
			AssetManager::SetActive(nullptr);
		}

		StreamingStage(const StreamingStage&) = delete;
		StreamingStage& operator=(const StreamingStage&) = delete;

		// A frame as games run it: the device's frame, the asset manager's update (finalizing and uploading within its
		// budgets), the scene's update, then `work`.
		template<typename Work>
		void Frame(GPUContext& gpu, Work&& work)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			Manager->Update();
			Level.OnUpdateRuntime(1.0f / 60.0f);
			work();
			gpu.GetDevice().EndFrame();
		}

		Entity Spawn()
		{
			const Ref<Prefab> prefab = std::static_pointer_cast<Prefab>(Manager->GetAsset(UUID(c_Prefab)));
			REQUIRE(prefab);
			const std::vector<Entity> roots = prefab->Instantiate(Level);
			REQUIRE(roots.size() == 1);
			return roots.front();
		}

		SceneRendererStats Render()
		{
			RenderToImage(Renderer, Level, LookAt(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f)));
			return Renderer.GetStats();
		}
	};

}

TEST_SUITE("GPU.Scene.Streaming")
{
	TEST_CASE("Once a prefab's closure has loaded, an instance draws complete in its first frame")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path pack = WritePrefabPack();

		// Without waiting for what it uses, an instance of the prefab draws with fallbacks first.
		{
			StreamingStage stage(pack);
			REQUIRE(stage.Manager->LoadAssetSync(UUID(c_Prefab)));
			stage.Spawn();
			SceneRendererStats stats;
			stage.Frame(gpu, [&]() { stats = stage.Render(); });
			CHECK(stats.PendingAssets > 0);
		}

		// Waiting until the closure's progress (what scripts read through Assets::GetLoadProgress) reaches 1, the instance
		// draws complete in the frame it was made in.
		StreamingStage stage(pack);
		ScriptSystem& scripts = GetScriptSystem(stage.Level);
		float progress = 0.0f;
		std::vector<float> progressions;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (progress < 1.0f)
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			stage.Frame(gpu, [&]() { progress = scripts.GetAssetLoadProgress(*stage.Manager, UUID(c_Prefab)); });
			progressions.push_back(progress);
		}
		CHECK(progressions.front() < 1.0f);
		for (const uint64_t handle : { c_Texture, c_Material, c_Mesh, c_Prefab })
			CHECK(stage.Manager->GetAssetState(UUID(handle)) == AssetState::Ready);

		SceneRendererStats stats;
		stage.Frame(gpu, [&]()
		{
			stage.Spawn();
			stats = stage.Render();
		});
		CHECK(stats.PendingAssets == 0);
		CHECK(stats.Instances >= 1);
		CHECK(stats.Materials >= 1);
		// Releasing the request leaves the closure to the residency budgets again.
		CHECK(scripts.ReleaseAsset(UUID(c_Prefab)));
		CHECK(stage.Manager->GetPinCount(UUID(c_Texture)) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
