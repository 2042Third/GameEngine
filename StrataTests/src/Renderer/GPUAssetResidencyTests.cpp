#include "Renderer/SceneRendererTestUtils.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/StagingTexturePool.h"
#include "Strata/Runtime/GameRenderer.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <map>
#include <memory>
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

	// Serves stored assets (cooked textures, unless told otherwise) from memory: many handles may share the same bytes.
	class TextureManager final : public AssetManagerBase
	{
	public:
		~TextureManager() override
		{
			WaitForInFlightLoads();
		}

		AssetHandle Add(uint64_t handle, Ref<const std::vector<uint8_t>> cooked, AssetType type = AssetType::Texture)
		{
			AssetMetadata metadata;
			metadata.Handle = UUID(handle);
			metadata.Type = type;
			metadata.Path = "Assets/" + std::to_string(handle);
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

	constexpr uint32_t c_ArrivalCount = 4;

	struct ArrivalRun
	{
		uint64_t RequestFrame = 0;       // The manager's frame in which the new textures were first requested
		uint64_t ReadyFrame = 0;         // The frame after whose update all of them were ready
		uint64_t EvictionsOnArrival = 0; // Evictions in the update the new textures arrived in
		uint64_t MaxResidentBytes = 0;
		uint64_t Evictions = 0;
	};

	// c_ArrivalCount textures of 256x256 fill a budget that holds that many and go out of use; then as many new ones are
	// used every frame, each of which needs room. Finalization uploads `uploadBytesPerFrame` per frame.
	ArrivalRun RunArrivals(GPUContext& gpu, const Ref<const std::vector<uint8_t>>& cooked, uint64_t uploadBytesPerFrame)
	{
		ArrivalRun run;
		const uint64_t textureBytes = GetTextureBytes(256);
		{
			Ref<TextureManager> manager = CreateRef<TextureManager>();
			std::vector<AssetHandle> previous;
			std::vector<AssetHandle> next;
			for (uint64_t index = 0; index < c_ArrivalCount; index++)
			{
				previous.push_back(manager->Add(0x45000 + index, cooked));
				next.push_back(manager->Add(0x46000 + index, cooked));
			}
			AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
			budgets.GpuTextures = c_ArrivalCount * textureBytes + textureBytes / 2;
			budgets.UploadBytesPerFrame = uploadBytesPerFrame;
			manager->SetResidencyBudgets(budgets);
			const auto allReady = [&manager](const std::vector<AssetHandle>& handles)
			{
				return std::all_of(handles.begin(), handles.end(), [&manager](AssetHandle handle) { return manager->GetAssetState(handle) == AssetState::Ready; });
			};

			for (uint32_t frame = 0; frame < 20 && !allReady(previous); frame++)
			{
				RunDeviceFrame(gpu, [&]()
				{
					manager->Update();
					for (AssetHandle handle : previous)
						manager->GetAsset(handle);
				});
			}
			REQUIRE(allReady(previous));
			for (uint32_t frame = 0; frame <= manager->GetEvictionGraceFrames() + 1; frame++)
				RunDeviceFrame(gpu, [&]() { manager->Update(); });
			REQUIRE(manager->GetStats().Evictions == 0);

			for (uint32_t frame = 0; frame < 60 && run.ReadyFrame == 0; frame++)
			{
				RunDeviceFrame(gpu, [&]()
				{
					manager->Update();
					const AssetManagerStats stats = manager->GetStats();
					run.MaxResidentBytes = std::max(run.MaxResidentBytes, stats.Resident.GpuTextures);
					if (run.RequestFrame > 0 && stats.Frame == run.RequestFrame + 1)
						run.EvictionsOnArrival = stats.Evictions;
					if (run.RequestFrame > 0 && allReady(next))
						run.ReadyFrame = stats.Frame;
					if (run.RequestFrame == 0)
						run.RequestFrame = stats.Frame;
					for (AssetHandle handle : next)
						manager->GetAsset(handle);
				});
			}
			REQUIRE(run.ReadyFrame > 0);
			run.Evictions = manager->GetStats().Evictions;
			for (AssetHandle handle : previous)
				CHECK(manager->GetAssetState(handle) == AssetState::Unloaded);
		}
		SettleDevice(gpu);
		return run;
	}

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
		// The updates below run no device frames, so the staging they use is never recycled: start with none in flight
		// (earlier tests' staging becomes reusable here), so that only the upload budget spreads them over frames.
		SettleDevice(gpu);
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

	TEST_CASE("Arrivals that need room wait for the release together, not one after another")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(256, glm::u8vec4(140, 30, 30, 255)));
		const uint64_t textureBytes = GetTextureBytes(256);
		const uint32_t releaseFrames = gpu.GetDevice().GetMaxFramesInFlight() + 1;

		// Each arrival evicts a previous texture, and all of them are uploaded once the frames that might have used the
		// previous ones are done: they arrive in one update and wait the release out together.
		const ArrivalRun together = RunArrivals(gpu, cooked, 64 * c_MB);
		CHECK(together.EvictionsOnArrival == c_ArrivalCount);
		CHECK(together.ReadyFrame - together.RequestFrame == 1 + releaseFrames);
		CHECK(together.MaxResidentBytes <= c_ArrivalCount * textureBytes + textureBytes / 2); // Room was made before any was published
		CHECK(together.Evictions == c_ArrivalCount);

		// Room is made ahead for at most a frame's upload budget: with one texture's worth, the arrivals behind the first
		// make room only once it is through, each waiting for its own release.
		const ArrivalRun oneByOne = RunArrivals(gpu, cooked, textureBytes);
		CHECK(oneByOne.EvictionsOnArrival == 1);
		CHECK(oneByOne.ReadyFrame - oneByOne.RequestFrame > c_ArrivalCount * releaseFrames);
		CHECK(oneByOne.MaxResidentBytes <= c_ArrivalCount * textureBytes + textureBytes / 2);
		CHECK(oneByOne.Evictions == c_ArrivalCount);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("An arrival that needs no GPU memory never waits for GPU memory to be released")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(256, glm::u8vec4(30, 60, 90, 255)));
		const uint64_t textureBytes = GetTextureBytes(256);
		Ref<TextureManager> manager = CreateRef<TextureManager>();
		const AssetHandle old = manager->Add(0x47000, cooked);
		const AssetHandle inUse = manager->Add(0x47001, cooked);
		const AssetHandle material = manager->Add(0x47002, CreateRef<const std::vector<uint8_t>>(ToBytes(Material::Create()->Serialize())), AssetType::Material);
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.GpuTextures = textureBytes + textureBytes / 2; // One fits: the two in use exceed it
		manager->SetResidencyBudgets(budgets);

		for (uint32_t frame = 0; frame < 10 && (manager->GetAssetState(old) != AssetState::Ready || manager->GetAssetState(inUse) != AssetState::Ready); frame++)
		{
			RunDeviceFrame(gpu, [&]()
			{
				manager->Update();
				manager->GetAsset(old);
				manager->GetAsset(inUse);
			});
		}
		REQUIRE(manager->GetAssetState(old) == AssetState::Ready);
		REQUIRE(manager->GetAssetState(inUse) == AssetState::Ready);

		// The old texture goes out of use. The material is requested in the last frame before the old texture leaves the
		// grace window, so it arrives in the update that evicts the texture: holding only CPU memory, it is published in
		// that update, without waiting for the texture's memory.
		const uint64_t lastOldRequest = manager->GetFrameIndex();
		const uint32_t grace = manager->GetEvictionGraceFrames();
		uint64_t evictedFrame = 0;
		uint64_t materialReadyFrame = 0;
		for (uint32_t frame = 0; frame < 20 && (evictedFrame == 0 || materialReadyFrame == 0); frame++)
		{
			RunDeviceFrame(gpu, [&]()
			{
				manager->Update();
				if (evictedFrame == 0 && manager->GetAssetState(old) == AssetState::Unloaded)
					evictedFrame = manager->GetFrameIndex();
				if (materialReadyFrame == 0 && manager->GetAssetState(material) == AssetState::Ready)
					materialReadyFrame = manager->GetFrameIndex();
				manager->GetAsset(inUse);
				if (manager->GetFrameIndex() >= lastOldRequest + grace)
					manager->GetAsset(material);
			});
		}
		REQUIRE(evictedFrame > 0);
		REQUIRE(materialReadyFrame > 0);
		CHECK(evictedFrame == lastOldRequest + grace + 1);
		CHECK(materialReadyFrame == evictedFrame);

		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Uploads go on without device frames, one step per update, while staging is full")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		const Ref<const std::vector<uint8_t>> cooked = CreateRef<const std::vector<uint8_t>>(CookSolidTexture(256, glm::u8vec4(5, 10, 15, 255)));
		Ref<TextureManager> manager = CreateRef<TextureManager>();
		const AssetHandle first = manager->Add(0x48000, cooked);
		const AssetHandle second = manager->Add(0x48001, cooked);

		// Staging released and never recycled (no device frames run here) fills the staging budget.
		StagingTexturePool& pool = Renderer::GetStagingTextures();
		nvrhi::TextureDesc band;
		band.width = 512;
		band.height = 512;
		band.format = nvrhi::Format::RGBA8_UNORM;
		pool.Release(pool.Acquire(band));
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.StagingBytes = pool.GetPendingBytes();
		manager->SetResidencyBudgets(budgets);

		// Each update's first step goes: one texture per update, never none.
		manager->RequestLoad(first);
		manager->RequestLoad(second);
		manager->Update();
		const auto ready = [&manager](AssetHandle handle) { return manager->GetAssetState(handle) == AssetState::Ready ? 1u : 0u; };
		CHECK(ready(first) + ready(second) == 1);
		manager->Update();
		CHECK(ready(first) + ready(second) == 2);

		manager.reset();
		SettleDevice(gpu);
		CHECK(pool.GetPendingBytes() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Managers start with GPU budgets derived from the device's memory budget")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const uint64_t deviceBudgetBefore = gpu.GetDevice().GetMemoryBudget().Budget;
		Ref<TextureManager> manager = CreateRef<TextureManager>();
		const AssetResidencyBudgets defaults = AssetManagerBase::GetDefaultResidencyBudgets();
		const uint64_t deviceBudgetAfter = gpu.GetDevice().GetMemoryBudget().Budget;
		REQUIRE(deviceBudgetBefore > 0);

		// The device's budget moves when other processes allocate: the defaults lie between those of the budgets read
		// before and after.
		const AssetResidencyBudgets low = AssetResidencyBudgets::FromDeviceBudget(std::min(deviceBudgetBefore, deviceBudgetAfter));
		const AssetResidencyBudgets high = AssetResidencyBudgets::FromDeviceBudget(std::max(deviceBudgetBefore, deviceBudgetAfter));
		for (const AssetResidencyBudgets& budgets : { manager->GetResidencyBudgets(), defaults })
		{
			CHECK(budgets.GpuTextures >= low.GpuTextures);
			CHECK(budgets.GpuTextures <= high.GpuTextures);
			CHECK(budgets.GpuBuffers >= low.GpuBuffers);
			CHECK(budgets.GpuBuffers <= high.GpuBuffers);
			CHECK(budgets.GpuTextures != AssetResidencyBudgets::c_Unlimited);
			CHECK(budgets.GpuBuffers < budgets.GpuTextures);
			CHECK(budgets.Cpu == AssetResidencyBudgets::c_Unlimited);
		}
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
		CHECK(manager->GetStats().Evictions == 0);
		// The shared texture's and material's objects: they must survive the switch, not be evicted and loaded again.
		const std::weak_ptr<Asset> sharedTexture = manager->GetAsset(textures[4]);
		const std::weak_ptr<Asset> sharedMaterial = manager->GetAsset(materials[4]);
		REQUIRE_FALSE(sharedTexture.expired());
		REQUIRE_FALSE(sharedMaterial.expired());

		// The game switches to Big, the way scripts ask for it: the textures only World used are gone within five frames.
		runtime->GetScene()->RequestSceneLoad(big.Handle);
		uint32_t framesUntilReleased = 0;
		for (uint32_t switched = 1; switched <= 5; switched++)
		{
			frame();
			if (framesUntilReleased == 0 && residentTextureBytes(0, 3) == 0)
				framesUntilReleased = switched;
			CAPTURE(switched);
			CHECK(manager->GetAssetState(textures[4]) == AssetState::Ready);
			CHECK(manager->GetAssetState(materials[4]) == AssetState::Ready);
			CHECK_FALSE(sharedTexture.expired());
			CHECK_FALSE(sharedMaterial.expired());
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
		// What the new scene uses stays: the shared texture and material are the objects loaded for World, and Big's own
		// arrived.
		CHECK(manager->GetAsset(textures[4]) == sharedTexture.lock());
		CHECK(manager->GetAsset(materials[4]) == sharedMaterial.lock());
		CHECK(residentTextureBytes(4, 8) == 5 * GetTextureBytes(256));
		CHECK(manager->GetStats().Evictions == 9); // Four textures, four materials, and World's scene document
		CHECK(manager->GetAssetState(world.Handle) == AssetState::Unloaded);

		runtime.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
