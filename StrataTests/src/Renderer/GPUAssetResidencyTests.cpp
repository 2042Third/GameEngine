#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Runtime/GameRenderer.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/SceneSerializer.h"

#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The megabytes of the acceptance criteria this suite checks (64 MB budget, 150 MB less device memory) are decimal.
	constexpr uint64_t c_MB = 1000 * 1000;

	// Serves cooked textures from memory: many handles may share the same bytes.
	class TextureManager final : public AssetManagerBase
	{
	public:
		~TextureManager() override
		{
			WaitForInFlightLoads();
		}

		AssetHandle Add(uint64_t handle, Ref<const std::vector<uint8_t>> cooked)
		{
			AssetMetadata metadata;
			metadata.Handle = UUID(handle);
			metadata.Type = AssetType::Texture;
			metadata.Path = "Textures/" + std::to_string(handle) + ".png";
			metadata.Name = metadata.Path;
			metadata.StoredSize = cooked->size();
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Data[metadata.Handle] = std::move(cooked);
			}
			RegisterAsset(metadata);
			return metadata.Handle;
		}
	protected:
		bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outError) override
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Data.find(metadata.Handle);
			if (it == m_Data.end())
			{
				if (outError)
					*outError = "No data";
				return false;
			}
			outData = *it->second;
			return true;
		}
	private:
		std::mutex m_Mutex;
		std::unordered_map<AssetHandle, Ref<const std::vector<uint8_t>>> m_Data;
	};

	// A square RGBA8 texture of one color with its full mip chain, cooked.
	std::vector<uint8_t> CookSolidTexture(uint32_t size, const glm::u8vec4& color)
	{
		TextureMip level0;
		level0.Width = size;
		level0.Height = size;
		level0.Data.resize(static_cast<size_t>(size) * size * 4);
		for (size_t texel = 0; texel < level0.Data.size(); texel += 4)
		{
			level0.Data[texel + 0] = color.r;
			level0.Data[texel + 1] = color.g;
			level0.Data[texel + 2] = color.b;
			level0.Data[texel + 3] = color.a;
		}
		std::vector<TextureMip> mips = { std::move(level0) };
		REQUIRE(TextureUtils::GenerateMips(mips, TextureFormat::RGBA8, false));
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "ResidencyTexture";
		const Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);
		return texture->Serialize();
	}

	// GPU bytes of a square RGBA8 texture with its full mip chain.
	uint64_t GetTextureBytes(uint32_t size)
	{
		uint64_t bytes = 0;
		for (uint32_t level = size; level > 0; level /= 2)
			bytes += static_cast<uint64_t>(level) * level * 4;
		return bytes;
	}

	// One frame the way the application runs it: the device's frame, the renderer's (bindless slots are recycled),
	// then the work in between.
	template<typename Work>
	void RunDeviceFrame(GPUContext& gpu, Work&& work)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		work();
		gpu.GetDevice().EndFrame();
	}

	// Frames without work, until releases deferred for frames in flight have happened.
	void SettleDevice(GPUContext& gpu)
	{
		for (uint32_t frame = 0; frame < gpu.GetDevice().GetMaxFramesInFlight() + 2; frame++)
			RunDeviceFrame(gpu, []() {});
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

	struct SlidingRun
	{
		uint64_t MaxResidentTextureBytes = 0;
		uint64_t FinalResidentTextureBytes = 0;
		uint64_t DeviceUsage = 0; // Device-local memory in use at the end (GraphicsMemoryBudget::Usage)
		uint64_t Evictions = 0;
		bool BindlessCountsMatched = true;
	};

	// 40 textures of 1024x1024; every frame the textures [frame, frame + 8) (modulo 40) are requested, so the set in use
	// slides by one texture per frame and every texture is used in turn.
	SlidingRun RunSlidingTextures(GPUContext& gpu, const Ref<const std::vector<uint8_t>>& cooked, std::optional<uint64_t> textureBudget)
	{
		constexpr uint32_t c_TextureCount = 40;
		constexpr uint32_t c_InUse = 8;
		constexpr uint32_t c_Frames = 120;

		SlidingRun run;
		BindlessTextureTable& bindless = Renderer::GetBindlessTextures();
		const uint32_t slotsBefore = bindless.GetAllocatedCount();
		{
			Ref<TextureManager> manager = CreateRef<TextureManager>();
			std::vector<AssetHandle> textures;
			for (uint64_t index = 0; index < c_TextureCount; index++)
				textures.push_back(manager->Add(0x10000 + index, cooked));
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = textureBudget.value_or(AssetResidencyBudgets::c_Unlimited);
			manager->SetResidencyBudgets(budgets);

			for (uint32_t frame = 0; frame < c_Frames; frame++)
			{
				RunDeviceFrame(gpu, [&]()
				{
					manager->Update();
					const AssetManagerStats stats = manager->GetStats();
					run.MaxResidentTextureBytes = std::max(run.MaxResidentTextureBytes, stats.Resident.GpuTextures);
					uint32_t readyTextures = 0;
					for (AssetHandle texture : textures)
						readyTextures += manager->GetAssetState(texture) == AssetState::Ready ? 1 : 0;
					// Every resident texture owns one bindless slot; evicted ones gave theirs back.
					run.BindlessCountsMatched &= bindless.GetAllocatedCount() == slotsBefore + readyTextures;

					for (uint32_t offset = 0; offset < c_InUse; offset++)
						manager->GetAsset(textures[(frame + offset) % c_TextureCount], AssetPriority::High);
				});
			}
			// Everything requested last arrives: finalization takes 4 ms per frame, and an arrival that made room waits for
			// the frames in flight first.
			uint32_t settleFrames = 0;
			for (; settleFrames < 120 && manager->HasPendingLoads(); settleFrames++)
				RunDeviceFrame(gpu, [&]() { manager->Update(); });
			INFO("Frames until every load was finalized: ", settleFrames);
			REQUIRE_FALSE(manager->HasPendingLoads());

			SettleDevice(gpu);
			const AssetManagerStats stats = manager->GetStats();
			run.FinalResidentTextureBytes = stats.Resident.GpuTextures;
			run.Evictions = stats.Evictions;
			run.DeviceUsage = gpu.GetDevice().GetMemoryBudget().Usage;
		}
		// The manager is gone: its textures are released once the frames that might use them are done.
		SettleDevice(gpu);
		CHECK(bindless.GetAllocatedCount() == slotsBefore);
		return run;
	}

	// Renders like the window's back buffer: BGRA8 UNORM.
	struct WindowTarget
	{
		nvrhi::TextureHandle Texture;
		nvrhi::FramebufferHandle Framebuffer;

		explicit WindowTarget(nvrhi::IDevice* device, uint32_t size)
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size;
			desc.format = nvrhi::Format::BGRA8_UNORM;
			desc.isRenderTarget = true;
			desc.debugName = "ResidencyTarget";
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			Texture = device->createTexture(desc);
			REQUIRE(Texture);
			Framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(Texture));
			REQUIRE(Framebuffer);
		}
	};

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// A scene of textured cubes in a row in front of its camera.
	nlohmann::json CreateCubeRowScene(const std::string& name, const std::vector<AssetHandle>& materials)
	{
		Scene scene(name);
		Entity camera = scene.CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 12.0f);
		camera.AddComponent<CameraComponent>();
		scene.CreateEntity("Sun").AddComponent<DirectionalLightComponent>().Intensity = 2.0f;
		for (size_t index = 0; index < materials.size(); index++)
		{
			const float x = (static_cast<float>(index) - static_cast<float>(materials.size() - 1) * 0.5f) * 1.5f;
			AddMesh(scene, BuiltinAssets::CubeMesh, materials[index], glm::vec3(x, 0.0f, 0.0f), "Cube " + std::to_string(index));
		}
		return SceneSerializer::Serialize(scene);
	}

}

TEST_SUITE("GPU.Assets.Residency")
{
	TEST_CASE("Textures stay within their budget while the textures in use change")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(1024, glm::u8vec4(200, 120, 40, 255)));
		const uint64_t textureBytes = GetTextureBytes(1024);
		CHECK(textureBytes == 5592404);

		const SlidingRun budgeted = RunSlidingTextures(gpu, cooked, 64 * c_MB);
		const SlidingRun unbudgeted = RunSlidingTextures(gpu, cooked, std::nullopt);

		// Within the budget after every frame (the 8 textures in use and those requested in the grace window fit).
		CHECK(budgeted.MaxResidentTextureBytes <= 64 * c_MB);
		CHECK(budgeted.BindlessCountsMatched);
		CHECK(budgeted.Evictions >= 80);
		// Without a budget every texture stays.
		CHECK(unbudgeted.FinalResidentTextureBytes == 40 * textureBytes);
		CHECK(unbudgeted.BindlessCountsMatched);
		CHECK(unbudgeted.Evictions == 0);
		// The device sees the difference.
		INFO("Device memory in use: ", budgeted.DeviceUsage / c_MB, " MB with the budget, ", unbudgeted.DeviceUsage / c_MB, " MB without");
		CHECK(unbudgeted.DeviceUsage >= budgeted.DeviceUsage + 150 * c_MB);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Finalization keeps to the upload budget, and idle staging memory is returned")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(1024, glm::u8vec4(10, 200, 30, 255)));
		const uint64_t textureBytes = GetTextureBytes(1024);
		Ref<TextureManager> manager = CreateRef<TextureManager>();
		std::vector<AssetHandle> textures;
		for (uint64_t index = 0; index < 4; index++)
			textures.push_back(manager->Add(0x20000 + index, cooked));

		// 5 MB per frame. A texture of 5.6 MB uploads in two steps (level 0 is one 4 MiB band, the smaller levels the
		// other), and a frame takes no step beyond its budget except its first: the four textures need five frames.
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.UploadBytesPerFrame = 5 * c_MB;
		budgets.FinalizeMsPerFrame = 1000.0f; // Only the byte budget limits these frames, however slow the machine
		manager->SetResidencyBudgets(budgets);
		for (AssetHandle texture : textures)
			manager->RequestLoad(texture);
		uint64_t uploadedTotal = 0;
		uint32_t frames = 0;
		uint32_t previouslyReady = 0;
		while (frames < 10)
		{
			manager->Update();
			frames++;
			const uint64_t uploaded = manager->GetStats().UploadedBytesLastFrame;
			CHECK(uploaded > 0);
			CHECK(uploaded <= budgets.UploadBytesPerFrame + c_AssetUploadStepBytes);
			uploadedTotal += uploaded;
			uint32_t ready = 0;
			for (AssetHandle texture : textures)
				ready += manager->GetAssetState(texture) == AssetState::Ready ? 1 : 0;
			CHECK(ready >= previouslyReady);
			CHECK(ready - previouslyReady <= 1); // Never more than a texture's worth in a frame
			previouslyReady = ready;
			if (ready == textures.size())
				break;
		}
		CHECK(frames == 5);
		CHECK(uploadedTotal == 4 * textureBytes);
		AssetManagerStats stats = manager->GetStats();
		CHECK(stats.UploadedBytesWindowMax <= budgets.UploadBytesPerFrame + c_AssetUploadStepBytes);
		CHECK(stats.FinalizeMsWindowMax > 0.0f);
		CHECK(stats.StagingReleases == 0);

		// 120 frames after the last upload, the upload command list (and the staging memory it pools) goes.
		for (uint32_t frame = 1; frame < AssetManagerBase::c_StagingReleaseFrames; frame++)
			manager->Update();
		CHECK(manager->GetStats().StagingReleases == 0);
		CHECK(manager->GetStats().UploadedBytesLastFrame == 0);
		manager->Update();
		CHECK(manager->GetStats().StagingReleases == 1);
		for (uint32_t frame = 0; frame < 2 * AssetManagerBase::c_StagingReleaseFrames; frame++)
			manager->Update();
		CHECK(manager->GetStats().StagingReleases == 1); // Released once; nothing to release until the next upload

		// The next upload uses a new command list, and the texture is right.
		manager->UnloadAsset(textures[0]);
		Ref<Texture> reloaded = std::static_pointer_cast<Texture>(manager->LoadAssetSync(textures[0]));
		REQUIRE(reloaded);
		REQUIRE(reloaded->GetGPUTexture());
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(reloaded->GetGPUTexture(), image, 9));
		CHECK(GetPixelRGBA8(image, 1, 1) == glm::u8vec4(10, 200, 30, 255));
		reloaded.reset();
		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A texture that needs room waits until what was evicted for it is released")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(1024, glm::u8vec4(90, 90, 200, 255)));
		const uint64_t textureBytes = GetTextureBytes(1024);
		Ref<TextureManager> manager = CreateRef<TextureManager>();
		const AssetHandle old = manager->Add(0x40000, cooked);
		const AssetHandle arrival = manager->Add(0x40001, cooked);
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.GpuTextures = textureBytes + textureBytes / 2; // One of them fits
		manager->SetResidencyBudgets(budgets);

		// The old texture is used for a while, then not any more.
		for (uint32_t frame = 0; frame < 10 && manager->GetAssetState(old) != AssetState::Ready; frame++)
			RunDeviceFrame(gpu, [&]() { manager->Update(); manager->GetAsset(old); });
		REQUIRE(manager->GetAssetState(old) == AssetState::Ready);
		for (uint32_t frame = 0; frame <= manager->GetEvictionGraceFrames() + 1; frame++)
			RunDeviceFrame(gpu, [&]() { manager->Update(); });
		REQUIRE(manager->GetAssetState(old) == AssetState::Ready); // Within the budget, unused or not

		// The new one is used every frame from now on: the old one goes to make room for it, and the new one is uploaded
		// once the frames that might have used the old one are done, so the device never holds both.
		uint64_t evictedFrame = 0;
		uint64_t readyFrame = 0;
		for (uint32_t frame = 0; frame < 20 && readyFrame == 0; frame++)
		{
			RunDeviceFrame(gpu, [&]()
			{
				manager->Update();
				if (evictedFrame == 0 && manager->GetAssetState(old) == AssetState::Unloaded)
					evictedFrame = manager->GetFrameIndex();
				if (readyFrame == 0 && manager->GetAssetState(arrival) == AssetState::Ready)
					readyFrame = manager->GetFrameIndex();
				manager->GetAsset(arrival);
			});
		}
		REQUIRE(evictedFrame > 0);
		REQUIRE(readyFrame > 0);
		CHECK(readyFrame - evictedFrame == gpu.GetDevice().GetMaxFramesInFlight() + 1);
		CHECK(manager->GetStats().Resident.GpuTextures == textureBytes);

		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A scene switch in a game releases the textures only the previous scene used")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());

		// Textures 0-3 belong to World, 5-8 to Big, 4 to both; one material per texture.
		std::vector<std::pair<AssetMetadata, std::vector<uint8_t>>> assets;
		std::vector<AssetHandle> textures;
		std::vector<AssetHandle> materials;
		for (uint64_t index = 0; index < 9; index++)
		{
			AssetMetadata texture;
			texture.Handle = UUID(0x30000 + index);
			texture.Type = AssetType::Texture;
			texture.Path = "Textures/T" + std::to_string(index) + ".png";
			texture.Name = "T" + std::to_string(index);
			assets.emplace_back(texture, CookSolidTexture(256, glm::u8vec4(static_cast<uint8_t>(20 * index), 100, 200, 255)));
			textures.push_back(texture.Handle);

			MaterialProperties properties;
			properties.BaseColorMap = texture.Handle;
			AssetMetadata material;
			material.Handle = UUID(0x31000 + index);
			material.Type = AssetType::Material;
			material.Path = "Materials/M" + std::to_string(index) + ".stmat";
			material.Name = "M" + std::to_string(index);
			assets.emplace_back(material, ToBytes(Material::Create(properties)->Serialize()));
			materials.push_back(material.Handle);
		}
		AssetMetadata world;
		world.Handle = UUID(0x32000);
		world.Type = AssetType::Scene;
		world.Path = "Scenes/World.stscene";
		world.Name = "World";
		assets.emplace_back(world, ToBytes(CreateCubeRowScene("World", { materials[0], materials[1], materials[2], materials[3], materials[4] })));
		AssetMetadata big;
		big.Handle = UUID(0x32001);
		big.Type = AssetType::Scene;
		big.Path = "Scenes/Big.stscene";
		big.Name = "Big";
		assets.emplace_back(big, ToBytes(CreateCubeRowScene("Big", { materials[4], materials[5], materials[6], materials[7], materials[8] })));

		const std::filesystem::path directory = CreateTemporaryDirectory("ResidencySceneSwitch");
		std::map<uint64_t, std::vector<uint8_t>> data;
		std::vector<AssetMetadata> metadata;
		for (auto& [assetMetadata, bytes] : assets)
		{
			metadata.push_back(assetMetadata);
			data[static_cast<uint64_t>(assetMetadata.Handle)] = std::move(bytes);
		}
		std::string error;
		REQUIRE_MESSAGE(AssetPack::Write(directory / "Switch.stpak", metadata, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}, &error), error);
		GameManifest manifest;
		manifest.Name = "Switch";
		manifest.AssetPack = "Switch.stpak";
		manifest.StartScene = world.Handle;
		REQUIRE_MESSAGE(manifest.Save(directory / "Switch.stgame", &error), error);

		Scope<GameRuntime> runtime = GameRuntime::Create(directory / "Switch.stgame", &error);
		REQUIRE_MESSAGE(runtime, error);
		const Ref<RuntimeAssetManager>& manager = runtime->GetAssetManager();
		WindowTarget target(gpu.GetNvrhiDevice(), c_SceneTestSize);
		GameRenderer renderer;
		const auto frame = [&]()
		{
			RunDeviceFrame(gpu, [&]()
			{
				runtime->Update(Timestep(1.0f / 60.0f));
				REQUIRE(renderer.Render(runtime->GetScene(), target.Framebuffer, glm::uvec2(c_SceneTestSize)));
			});
		};
		const auto residentTextureBytes = [&manager](size_t first, size_t last)
		{
			uint64_t bytes = 0;
			for (const AssetResidencyInfo& asset : manager->GetResidencyInfo())
			{
				const uint64_t index = static_cast<uint64_t>(asset.Handle) - 0x30000;
				if (asset.Type == AssetType::Texture && index >= first && index <= last)
					bytes += asset.Usage.GpuTextures;
			}
			return bytes;
		};

		// World streams in and is drawn.
		for (int warmup = 0; warmup < 60 && (renderer.GetStats().PendingAssets > 0 || residentTextureBytes(0, 4) == 0); warmup++)
			frame();
		REQUIRE(renderer.GetStats().PendingAssets == 0);
		CHECK(residentTextureBytes(0, 4) == 5 * GetTextureBytes(256));

		// The game switches to Big, the way scripts ask for it: the textures only World used are gone within five frames.
		runtime->GetScene()->RequestSceneLoad(big.Handle);
		uint32_t framesUntilReleased = 0;
		for (uint32_t switched = 1; switched <= 5; switched++)
		{
			frame();
			if (framesUntilReleased == 0 && residentTextureBytes(0, 3) == 0)
				framesUntilReleased = switched;
		}
		CHECK(runtime->GetSceneHandle() == big.Handle);
		CHECK(framesUntilReleased > 0);
		CHECK(framesUntilReleased <= 5);
		for (size_t index = 0; index < 4; index++)
		{
			CAPTURE(index);
			CHECK(manager->GetAssetState(textures[index]) == AssetState::Unloaded);
			CHECK(manager->GetAssetState(materials[index]) == AssetState::Unloaded);
		}
		// What the new scene uses stays: the shared texture was never released, and Big's own arrived.
		CHECK(manager->GetAssetState(textures[4]) == AssetState::Ready);
		CHECK(residentTextureBytes(4, 8) == 5 * GetTextureBytes(256));
		CHECK(manager->GetStats().Evictions >= 8); // Four textures, four materials, and World's scene document
		CHECK(manager->GetAssetState(world.Handle) == AssetState::Unloaded);

		runtime.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
