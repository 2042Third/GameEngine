#include "stpch.h"
#include "Strata/Renderer/Renderer.h"

#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureTable.h"
#include "Strata/Renderer/TextureReadback.h"

namespace Strata
{

	namespace
	{

		struct RendererData
		{
			GraphicsDevice* Device = nullptr;
			Scope<ShaderLibrary> Shaders;
			Scope<BindlessTextureTable> BindlessTextures;
			Scope<TextureTable> Textures;
			Scope<StagingPool> Staging;
			Scope<GPUUploadQueue> UploadQueue;          // Records into the staging pool: destroyed before it
			Scope<RendererUploadContext> UploadContext; // Records into the upload queue: destroyed before it
			Scope<DeferredReleaseQueue> DeferredReleases;
			Scope<GPUProfiler> Profiler;
			Ref<GeometryPool> Geometry;
			Scope<RenderSceneGPUCache> SceneGPUs;

			nvrhi::SamplerHandle LinearClamp;
			nvrhi::SamplerHandle LinearWrap;
			nvrhi::SamplerHandle PointClamp;
			nvrhi::SamplerHandle AnisotropicWrap;

			nvrhi::CommandListHandle FrameCommandList;
			bool BackBufferPrepared = false; // PrepareBackBuffer ran in this frame
			uint64_t FrameNumber = 0;        // Frames begun (BeginFrame)

			nvrhi::TextureHandle WhiteTexture;
			nvrhi::TextureHandle BlackTexture;
			nvrhi::TextureHandle FlatNormalTexture;
			nvrhi::TextureHandle BlackCubeTexture;
			nvrhi::TextureHandle BlackCubeArrayTexture;
		};

		Scope<RendererData> s_Data;

		// A 1x1 texture of one color: 2D, a cube (six faces) or a cube array of one cube.
		nvrhi::TextureHandle CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name,
			nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Texture2D)
		{
			const bool cube = dimension == nvrhi::TextureDimension::TextureCube || dimension == nvrhi::TextureDimension::TextureCubeArray;
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.dimension = dimension;
			desc.arraySize = cube ? 6 : 1;
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;

			nvrhi::TextureHandle texture = s_Data->Device->GetDevice()->createTexture(desc);
			if (!texture)
				return nullptr;
			for (uint32_t slice = 0; slice < desc.arraySize; slice++)
				commandList->writeTexture(texture, slice, 0, &rgba, sizeof(rgba));
			return texture;
		}

	}

	bool Renderer::Init(GraphicsDevice& device, const RendererSpecification& specification)
	{
		ST_CORE_VERIFY(!s_Data, "Renderer initialized twice");
		s_Data = CreateScope<RendererData>();
		s_Data->Device = &device;
		nvrhi::IDevice* nvrhiDevice = device.GetDevice();
		s_Data->Shaders = CreateScope<ShaderLibrary>(nvrhiDevice);

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		s_Data->LinearClamp = nvrhiDevice->createSampler(samplerDesc);
		samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
		s_Data->LinearWrap = nvrhiDevice->createSampler(samplerDesc);
		samplerDesc.setMaxAnisotropy(16.0f);
		s_Data->AnisotropicWrap = nvrhiDevice->createSampler(samplerDesc);
		nvrhi::SamplerDesc pointDesc;
		pointDesc.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		s_Data->PointClamp = nvrhiDevice->createSampler(pointDesc);

		nvrhi::CommandListHandle commandList = nvrhiDevice->createCommandList();
		commandList->open();
		s_Data->WhiteTexture = CreateSolidTexture(commandList, 0xFFFFFFFFu, "WhiteTexture");
		s_Data->BlackTexture = CreateSolidTexture(commandList, 0xFF000000u, "BlackTexture");
		s_Data->FlatNormalTexture = CreateSolidTexture(commandList, 0xFFFF8080u, "FlatNormalTexture"); // (0.5, 0.5, 1.0)
		s_Data->BlackCubeTexture = CreateSolidTexture(commandList, 0xFF000000u, "BlackCubeTexture", nvrhi::TextureDimension::TextureCube);
		s_Data->BlackCubeArrayTexture = CreateSolidTexture(commandList, 0xFF000000u, "BlackCubeArrayTexture", nvrhi::TextureDimension::TextureCubeArray);
		commandList->close();
		nvrhiDevice->executeCommandList(commandList);

		s_Data->FrameCommandList = nvrhiDevice->createCommandList();
		if (!s_Data->LinearClamp || !s_Data->LinearWrap || !s_Data->AnisotropicWrap || !s_Data->PointClamp || !s_Data->WhiteTexture
			|| !s_Data->BlackTexture || !s_Data->FlatNormalTexture || !s_Data->BlackCubeTexture || !s_Data->BlackCubeArrayTexture || !s_Data->FrameCommandList)
		{
			ST_CORE_ERROR("Renderer: failed to create shared GPU resources");
			Shutdown();
			return false;
		}

		s_Data->BindlessTextures = CreateScope<BindlessTextureTable>(nvrhiDevice, device.GetInfo().MaxBindlessTextures, device.GetMaxFramesInFlight());
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_WhiteSlot, s_Data->WhiteTexture);
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_BlackSlot, s_Data->BlackTexture);
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_FlatNormalSlot, s_Data->FlatNormalTexture);
		s_Data->Textures = CreateScope<TextureTable>(nvrhiDevice, device.GetMaxFramesInFlight());
		s_Data->Staging = CreateScope<StagingPool>(nvrhiDevice);
		s_Data->UploadQueue = CreateScope<GPUUploadQueue>(device, *s_Data->Staging);
		s_Data->UploadContext = CreateScope<RendererUploadContext>(*s_Data->UploadQueue, *s_Data->Staging);
		if (specification.TransferQueueUploads && !s_Data->UploadQueue->SetTransferQueue(true))
			ST_CORE_INFO("Renderer: the device has no dedicated transfer queue; uploads run on the graphics queue");
		s_Data->DeferredReleases = CreateScope<DeferredReleaseQueue>(nvrhiDevice);
		s_Data->Geometry = GeometryPool::Create(nvrhiDevice);
		if (!s_Data->Geometry)
		{
			ST_CORE_ERROR("Renderer: failed to create the geometry pool");
			Shutdown();
			return false;
		}
		s_Data->SceneGPUs = CreateScope<RenderSceneGPUCache>(nvrhiDevice);
		s_Data->Profiler = CreateScope<GPUProfiler>(device);
		if (!s_Data->Profiler->IsEnabled())
			ST_CORE_INFO("Renderer: the device writes no GPU timestamps; GPU stages are not measured");
		// Texture loaders decode block-compressed data on their workers for devices that cannot sample it.
		Texture::SetBlockCompressionSupported(device.GetInfo().SupportsBCCompression);
		return true;
	}

	void Renderer::Shutdown()
	{
		if (!s_Data)
			return;

		s_Data->Device->WaitForIdle();
		s_Data.reset();
		Texture::SetBlockCompressionSupported(true);
	}

	bool Renderer::IsInitialized()
	{
		return s_Data != nullptr;
	}

	GraphicsDevice& Renderer::GetGraphicsDevice()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Device;
	}

	nvrhi::IDevice* Renderer::GetDevice()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return s_Data->Device->GetDevice();
	}

	ShaderLibrary& Renderer::GetShaderLibrary()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Shaders;
	}

	BindlessTextureTable& Renderer::GetBindlessTextures()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->BindlessTextures;
	}

	TextureTable& Renderer::GetTextureTable()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Textures;
	}

	StagingPool& Renderer::GetStagingPool()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Staging;
	}

	RendererUploadContext& Renderer::GetUploadContext()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->UploadContext;
	}

	GPUUploadQueue& Renderer::GetUploadQueue()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->UploadQueue;
	}

	GPUProfiler& Renderer::GetGPUProfiler()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Profiler;
	}

	Ref<GeometryPool> Renderer::GetGeometryPool()
	{
		return s_Data ? s_Data->Geometry : nullptr;
	}

	RenderSceneGPUCache& Renderer::GetRenderSceneGPUs()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->SceneGPUs;
	}

	GPUProfiler* Renderer::FindGPUProfiler()
	{
		return s_Data ? s_Data->Profiler.get() : nullptr;
	}

	DeferredReleaseQueue& Renderer::GetDeferredReleases()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->DeferredReleases;
	}

	void Renderer::ReleaseDeferred(std::vector<nvrhi::ResourceHandle> resources)
	{
		if (s_Data)
			s_Data->DeferredReleases->Release(std::move(resources));
	}

	void Renderer::BeginFrame()
	{
		if (!s_Data)
			return;

		s_Data->Profiler->BeginFrame();
		s_Data->Geometry->BeginFrame();
		s_Data->SceneGPUs->BeginFrame();
		s_Data->BindlessTextures->BeginFrame(s_Data->Device->GetFrameIndex());
		s_Data->Textures->BeginFrame(s_Data->Device->GetFrameIndex());
		s_Data->Staging->BeginFrame(s_Data->Device->GetFrameIndex());
		s_Data->UploadQueue->BeginFrame();
		s_Data->DeferredReleases->Collect(DeferredReleaseQueue::c_FrameDestroyBudget);
		s_Data->BackBufferPrepared = false;
		s_Data->FrameNumber++;
	}

	uint64_t Renderer::GetFrameNumber()
	{
		return s_Data ? s_Data->FrameNumber : 0;
	}

	nvrhi::ITexture* Renderer::PrepareBackBuffer()
	{
		if (!s_Data)
			return nullptr;
		if (s_Data->BackBufferPrepared)
			return s_Data->Device->GetBackBuffer();
		s_Data->BackBufferPrepared = true;
		// The first work on the image waits for the display where the driver hands the image out only at its first use
		// (MoltenVK waits for its drawable in the submission below): a wait, not work, like presenting.
		ST_PROFILE_STAGE("Present");
		if (!s_Data->Device->PrepareBackBuffer())
			return nullptr;
		nvrhi::ITexture* backBuffer = s_Data->Device->GetBackBuffer();
		if (backBuffer)
		{
			s_Data->FrameCommandList->open();
			s_Data->FrameCommandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			s_Data->FrameCommandList->close();
			s_Data->Device->GetDevice()->executeCommandList(s_Data->FrameCommandList);
		}
		return backBuffer;
	}

	nvrhi::ISampler* Renderer::GetLinearClampSampler() { return s_Data->LinearClamp; }
	nvrhi::ISampler* Renderer::GetLinearWrapSampler() { return s_Data->LinearWrap; }
	nvrhi::ISampler* Renderer::GetPointClampSampler() { return s_Data->PointClamp; }
	nvrhi::ISampler* Renderer::GetAnisotropicWrapSampler() { return s_Data->AnisotropicWrap; }
	nvrhi::ITexture* Renderer::GetWhiteTexture() { return s_Data->WhiteTexture; }
	nvrhi::ITexture* Renderer::GetBlackTexture() { return s_Data->BlackTexture; }
	nvrhi::ITexture* Renderer::GetFlatNormalTexture() { return s_Data->FlatNormalTexture; }
	nvrhi::ITexture* Renderer::GetBlackCubeTexture() { return s_Data->BlackCubeTexture; }
	nvrhi::ITexture* Renderer::GetBlackCubeArrayTexture() { return s_Data->BlackCubeArrayTexture; }

	bool Renderer::ReadTexture(nvrhi::ITexture* texture, ReadbackImage& outImage, uint32_t mipLevel, uint32_t arraySlice)
	{
		if (!s_Data || !texture)
			return false;

		TextureReadbackRegion region;
		region.MipLevel = mipLevel;
		region.ArraySlice = arraySlice;
		std::string error;
		Scope<TextureReadback> readback = TextureReadback::Create(texture, region, &error);
		if (!readback)
		{
			ST_CORE_ERROR("Renderer::ReadTexture: {}", error);
			return false;
		}
		readback->Wait();
		return readback->GetResult(outImage);
	}

}
